/* format rbd (spdk ceph) support */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include "config.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_helper.h"
#include "wandio.h"
#include "rt_protocol.h"
//dpdk. /usr/local/include/dpdk/rte_ring.h
#include <rte_ring.h>			//for lockless ring buffer (lockless queue)
//spdk
#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/copy_engine.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/queue.h"
#include "spdk/nvme.h"

#include "../lib/bdev/rbd/bdev_rbd.h"
#include "../lib/bdev/nvme/bdev_nvme.h"

#define WIRELEN_DROPLEN 4
#define ERR_SIZE 512
#define DEVNAME_SIZE 32
#define NVME_MAX_BDEVS_PER_RPC 32

//rbd
#define RBD_DEVNAME "rbdev"
#define RBD_POOLNAME "capture"
#define RBD_IMAGE "foo2"
#define RBD_BLOCKSIZE 512

//options
#define QUEUE_RING_SIZE 1048576*8
#define LOCKLESS
#define SPIN_LOCK
#define MAX_PACKET_SIZE 1600
#define BUFFER_SIZE 1048576
#define ERROR_DBG
//#define DEBUG
#define HL_DEBUGS		//high level debugs - on writing buffers and counting callbacks

#define FORMAT(x) ((spdk_format_data_t *)x->format_data)
#ifdef DEBUG
 #define debug(x...) printf(x)
#else
 #define debug(x...)
#endif

#ifdef ERROR_DBG
 #define error(x...) printf("[error] " x)
#else
 #define error(x...)
#endif

//------------------ spdk structs ----------------------------------------------
/* Used to pass messages between fio threads */
struct pls_msg {
        spdk_thread_fn  cb_fn;
        void            *cb_arg;
};

/* A polling function */
struct pls_poller
{
        spdk_poller_fn          cb_fn;
        void                    *cb_arg;
        uint64_t                period_microseconds;
        TAILQ_ENTRY(pls_poller) link;
};

typedef struct pls_target_s
{
        struct spdk_bdev        *bd;
        struct spdk_bdev_desc   *desc;
        struct spdk_io_channel  *ch;
        TAILQ_ENTRY(pls_target_s) link;
} pls_target_t;

typedef struct pls_thread_s
{
        bool finished;
        int idx;
        bool read_complete;             //flag, false when read callback not finished, else - tru
        unsigned char *buf;
        uint64_t offset;                //just for stats
        pthread_t pthread_desc;
        struct spdk_thread *thread;     /* spdk thread context */
        struct spdk_ring *ring;         /* ring for passing messages to this thread */
        pls_target_t pls_target;
        TAILQ_HEAD(, pls_poller) pollers; /* List of registered pollers on this thread */
} pls_thread_t;

//------------------ libtrace structs ------------------------------------------
//global data
typedef struct spdk_format_data_s
{
	struct spdk_bdev *bd;		//bdev device created for rbd
	const char *names[NVME_MAX_BDEVS_PER_RPC];
	uint32_t block_size;
        uint64_t num_blocks;
	uint64_t bytes;
	uint64_t max_offset;
	uint64_t overwrap_read_cnt;
	struct rte_ring *rbuf;		//ptr to dpdk ring buffer, used for lockless queue
	pls_thread_t *t;
	int num_threads;		//keep number of threads passed from utility command line
	void *pkt;			//store received packet here
	int pkt_len;			//length of current packet
	int pvt;			//for private data saving
	unsigned int pkts_read;
	u_char *l2h;			//l2 header for current packet
	libtrace_list_t *per_stream;	//pointer to the whole list structure: head, tail, size etc inside.
} spdk_format_data_t;

//per thread
typedef struct spdk_per_stream_s 
{
	int id;
	int core;
	void *pkt;
	int pkt_len;
	u_char *l2h;
	unsigned int pkts_read;
} spdk_per_stream_t;

typedef struct pckt_s
{
        void *ptr;
        int len;
	//XXX - timestamp for packet?
        struct pckt_s *next;
} pckt_t;

typedef struct stat_s
{
	uint64_t pkts_read;
	uint64_t pkts_finished;
} stat_t;

stat_t pkt_stat;
pls_thread_t pls_ctrl_thread;
pthread_t poller_thread;

static void pkt_parser(void *bf, spdk_format_data_t *fd);

//queue implementation----------------------------------------------------------
//input queue (on server)
#ifndef LOCKLESS
static pckt_t *queue_head = NULL;
static pckt_t *queue_tail = NULL;
static int queue_num = 0;
#endif
#ifdef SPIN_LOCK
static pthread_spinlock_t queue_lock;
static int pshared;
#else
pthread_mutex_t mutex_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifndef LOCKLESS
static int queue_add(pckt_t *pkt)
{
	if (!pkt)
	{
		error("trying to add to queue NULL pkt!\n");
		return -1;
	}

#ifdef SPIN_LOCK
	pthread_spin_lock(&queue_lock);
#else
	pthread_mutex_lock(&mutex_lock);
#endif
        if (!queue_head)
        {
                queue_head = pkt;
                queue_tail = pkt;
        }
        else
        {
                queue_tail->next = pkt;		//we set element->next here
                queue_tail = pkt;
        }
        pkt->next = NULL;
        queue_num++;
#ifdef SPIN_LOCK
	pthread_spin_unlock(&queue_lock);
#else
	pthread_mutex_unlock(&mutex_lock);
#endif

	pkt_stat.pkts_read++;

	return queue_num;
}

static pckt_t* queue_de()
{
        pckt_t *deq = NULL;

#ifdef SPIN_LOCK
	//pthread_spin_trylock(&queue_lock);
	pthread_spin_lock(&queue_lock);
#else
	pthread_mutex_lock(&mutex_lock);
#endif
        if (queue_head)
        {
                deq = queue_head;
                if (queue_head != queue_tail) //not last element
                {
                        queue_head = queue_head->next;
			if (!queue_head)
			{
#ifdef SPIN_LOCK
				pthread_spin_unlock(&queue_lock);
#else
				pthread_mutex_unlock(&mutex_lock);
#endif
				error("we lost head but queue is not empty: %d\n", queue_num);
				return NULL;
			}
                }
                else //last element
                {
                        queue_head = queue_tail = NULL;
                }
                queue_num--;
#ifdef SPIN_LOCK
		pthread_spin_unlock(&queue_lock);
#else
		pthread_mutex_unlock(&mutex_lock);
#endif
                return deq;
        }
        else
	{
#ifdef SPIN_LOCK
		pthread_spin_unlock(&queue_lock);
#else
		pthread_mutex_unlock(&mutex_lock);
#endif
                return NULL;
	}
}
#endif

static pckt_t* queue_create_pckt()
{
	pckt_t *p = malloc(sizeof (pckt_t));
	if (!p)
		return NULL;
	else
		memset(p, 0x0, sizeof (pckt_t));
	return p;
}

//------------------ spdk functions --------------------------------------------
static size_t pls_poll_thread(pls_thread_t *thread)
{
        struct pls_msg *msg;
        struct pls_poller *p, *tmp;
        size_t count;

        debug("%s() called \n", __func__);

        /* Process new events */
        count = spdk_ring_dequeue(thread->ring, (void **)&msg, 1);
        if (count > 0) {
                msg->cb_fn(msg->cb_arg);
                free(msg);
        }

        /* Call all pollers */
        TAILQ_FOREACH_SAFE(p, &thread->pollers, link, tmp) {
                p->cb_fn(p->cb_arg);
        }

        //printf("%s() exited \n", __func__);

        return count;
}

//This is pass message function for spdk_allocate_thread
static void pls_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
        pls_thread_t *thread = thread_ctx;
        struct pls_msg *msg;
        size_t count;

        printf("%s() called \n", __func__);

        msg = calloc(1, sizeof(*msg));
        assert(msg != NULL);

        msg->cb_fn = fn;
        msg->cb_arg = ctx;

        count = spdk_ring_enqueue(thread->ring, (void **)&msg, 1);
        if (count != 1) {
                SPDK_ERRLOG("Unable to send message to thread %p. rc: %lu\n", thread, count);
        }
}

static struct spdk_poller* pls_start_poller(void *thread_ctx, spdk_poller_fn fn,
                                            void *arg, uint64_t period_microseconds)
{
        pls_thread_t *thread = thread_ctx;
        struct pls_poller *poller;

        printf("%s() called \n", __func__);

        poller = calloc(1, sizeof(*poller));
        if (!poller)
        {
                SPDK_ERRLOG("Unable to allocate poller\n");
                return NULL;
        }

        poller->cb_fn = fn;
        poller->cb_arg = arg;
        poller->period_microseconds = period_microseconds;

        TAILQ_INSERT_TAIL(&thread->pollers, poller, link);

        return (struct spdk_poller *)poller;
}

static void pls_stop_poller(struct spdk_poller *poller, void *thread_ctx)
{
        struct pls_poller *lpoller;
        pls_thread_t *thread = thread_ctx;

        printf("%s() called \n", __func__);

        lpoller = (struct pls_poller *)poller;

        TAILQ_REMOVE(&thread->pollers, lpoller, link);

        free(lpoller);
}

static void pls_bdev_init_done(void *cb_arg, int rc)
{
        printf("bdev init is done\n");
        *(bool *)cb_arg = true;
	(void)rc;
}

static void pls_bdev_read_done_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
        static unsigned int cnt = 0;
        pls_thread_t *t = (pls_thread_t*)cb_arg;

        if (success)
        {
                t->read_complete = true;
                //global.stat_read_bytes += BUFFER_SIZE;
                __atomic_fetch_add(&cnt, 1, __ATOMIC_SEQ_CST);
                debug("read completed successfully\n");
        }
        else
                printf("read failed\n");

#ifdef HL_DEBUGS
        if (cnt % 1000 == 0)
                printf("have %u successful read callabacks. thread #%d, offset: 0x%lx \n",
                         cnt, t->idx, t->offset);
#endif
        spdk_bdev_free_io(bdev_io);
}

//spdk init, which could be used for both: spdk_init_input() and spdk_init_output()
static int spdk_init_environment(char *uridata, spdk_format_data_t *fd, char *err, int errlen)
{
	int rv = 0;
        bool done = false;
        size_t cnt;
        struct spdk_env_opts opts;
	struct spdk_bdev_desc *desc;
        //int i, j;
        //struct spdk_nvme_transport_id trid[NUM_RAID_DEVICES] = {{0}};
        //size_t count = NVME_MAX_BDEVS_PER_RPC;

	(void)uridata;
	(void)err;
	(void)errlen;

        printf("%s() called \n", __func__);

        spdk_log_set_print_level(SPDK_LOG_DEBUG);
        spdk_log_set_level(SPDK_LOG_DEBUG);
        spdk_log_open();

        spdk_env_opts_init(&opts);
        opts.name = "bdev_rbd";

        if (spdk_env_init(&opts) < 0) {
                SPDK_ERRLOG("Unable to initialize SPDK env\n");
                return -1;
        }
        spdk_unaffinitize_thread();

        pls_ctrl_thread.ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 4096, SPDK_ENV_SOCKET_ID_ANY);
        if (!pls_ctrl_thread.ring)
        {
                SPDK_ERRLOG("failed to allocate ring\n");
                return -1;
        }

        pls_ctrl_thread.thread = spdk_allocate_thread(pls_send_msg, pls_start_poller,
                                 pls_stop_poller, &pls_ctrl_thread, "pls_ctrl_thread");

        if (!pls_ctrl_thread.thread)
        {
                spdk_ring_free(pls_ctrl_thread.ring);
                SPDK_ERRLOG("failed to allocate thread\n");
                return -1;
        }

        TAILQ_INIT(&pls_ctrl_thread.pollers);

        /* Initialize the copy engine */
        spdk_copy_engine_initialize();

        /* Initialize the bdev layer */
        spdk_bdev_initialize(pls_bdev_init_done, &done);

        /* First, poll until initialization is done. */
        do {
                pls_poll_thread(&pls_ctrl_thread);
        } while (!done);

        /* Continue polling until there are no more events.
	This handles any final events posted by pollers. */
        do {
                cnt = pls_poll_thread(&pls_ctrl_thread);
        } while (cnt > 0);

	//rbd device creation/init
	//1.bdev_name, 2.pool_name, 3.image_name, 4.block_size
	fd->bd = spdk_bdev_rbd_create(RBD_DEVNAME, RBD_POOLNAME, RBD_IMAGE, RBD_BLOCKSIZE);
	if (!fd->bd)
	{
		printf("error: can't create bdev device!\n");
		return -1;
	}
	else
	{
		printf("rbd created. name: %s, pool: %s, img: %s, blocksize: %d \n",
			RBD_DEVNAME, RBD_POOLNAME, RBD_IMAGE, RBD_BLOCKSIZE);
	}

	//returns a descriptor
	rv = spdk_bdev_open(fd->bd, 1, NULL, NULL, &desc);
	if (rv)
	{
		printf("failed to open device\n");
		return -1;
	}

	fd->block_size = spdk_bdev_get_block_size(fd->bd);
	fd->num_blocks = spdk_bdev_get_num_blocks(fd->bd);
	printf("device block size is: %u bytes, num blocks: %lu\n", 
		fd->block_size, fd->num_blocks);
	fd->max_offset = fd->block_size * fd->num_blocks - 1;
	printf("max offset(bytes): 0x%lx\n", fd->max_offset);

	spdk_bdev_close(desc);


#if 0
        for (i = 0; i < NUM_RAID_DEVICES; i++)
        {
                trid[i].trtype = SPDK_NVME_TRANSPORT_PCIE;
                trid[i].adrfam = 0;
                memcpy(trid[i].traddr, fd->pci_nvme_addr[i], strlen(fd->pci_nvme_addr[i]));

                printf("creating bdev device #%d\n", i);

                rv = spdk_bdev_nvme_create(&trid[i], fd->devname[i],
                        &fd->names[i], &count, DEVICE_NAME_NQN);
                if (rv)
                {
                        printf("error: can't create bdev device!\n");
                        return -1;
                }
                for (j = 0; j < (int)count; j++)
                {
                        printf("#%d: device %s created \n", j, fd->names[i]);
                }
        }

        rv = spdk_construct_raid_bdev(RAID_DEVICE, STRIPE_SIZE, 0, NUM_RAID_DEVICES, 
				      fd->names[0], fd->names[1]);
        if (!rv)
                printf("[raid created successfully]\n");
        else
                printf("<failed to create raid>\n");
#endif

	//free thread to allocate it one more time. special for libtrace
	spdk_free_thread();

	//init dpdk ring buffer
	fd->rbuf = rte_ring_create("queue_ring", QUEUE_RING_SIZE, SOCKET_ID_ANY,
					RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (!fd->rbuf)
	{
		error("failed to create rte_ring. exiting.\n");
		return -1;
	}
	else
	{
		printf("rte_ring created successfully \n");
	}

        return rv;
}

/* Initialises an input trace using the capture format. 
   @param libtrace 	The input trace to be initialised */
static int rbd_init_input(libtrace_t *libtrace) 
{
	//int i;
	char err[ERR_SIZE] = {0};

	printf("%s() \n", __func__);

	spdk_per_stream_t stream;
	memset(&stream, 0x0, sizeof(spdk_per_stream_t));

#ifdef SPIN_LOCK
	pthread_spin_init(&queue_lock, pshared);
#endif

	//init all the data in spdk_format_data_t
	libtrace->format_data = malloc(sizeof(spdk_format_data_t));
	memset(libtrace->format_data, 0x0, sizeof(spdk_format_data_t));
	FORMAT(libtrace)->t = calloc(1, sizeof(pls_thread_t));
	FORMAT(libtrace)->pvt = 0xFAFAFAFA;
	FORMAT(libtrace)->pkts_read = 0;
	FORMAT(libtrace)->num_threads = libtrace->perpkt_thread_count;
//raid related
/*
	FORMAT(libtrace)->pci_nvme_addr[0] = strdup(RAID1);
	FORMAT(libtrace)->pci_nvme_addr[1] = strdup(RAID2);
        for (i = 0; i < NUM_RAID_DEVICES; i++)
        {
                char c[2] = {0};
                strcpy(FORMAT(libtrace)->devname[i], DEVICE_NAME);
                sprintf(c, "%d", i+1);
                strcat(FORMAT(libtrace)->devname[i], c);
        }
*/
	/* Make our first stream */
	FORMAT(libtrace)->per_stream = libtrace_list_init(sizeof(spdk_per_stream_t));
	libtrace_list_push_back(FORMAT(libtrace)->per_stream, &stream);//copies inside, so ok to alloc on stack.

	if (spdk_init_environment(libtrace->uridata, FORMAT(libtrace), err, sizeof(err))) 
	{
		trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "%s", err);
		free(libtrace->format_data);
		libtrace->format_data = NULL;
		return -1;
	}
	return 0;
}

//takes buf. parses it. add packets to the queue
static void pkt_parser(void *bf, spdk_format_data_t *fd)
{
	int i, j;
	unsigned char *p = (unsigned char*)bf;
	bool new_packet = false;
	bool new_len = false;
	unsigned short len = 0;
	uint64_t ts = 0, t = 0;
	pckt_t *pkt = NULL;
	int num;

	debug("%s() called \n", __func__);

	//parsing packets in 0xEE format here
	for (i = 0; i < BUFFER_SIZE; i++)
	{
		//printf("i: %d, 0x%X\n", i, p[i]);
		if (p[i] == 0xEE)
		{
			new_packet = true;
			continue;
		}
		if (new_packet)
		{
			//getting timestamp
			ts = 0;
			for (j = 0; j < 8; j++)
			{
				t = (uint64_t)p[i+j];
				ts |= t << 8*(7-j);
				//printf("j: %d, ts: 0x%lx \n", j, ts);
			}
			i += 8;

			len = p[i] << 8;
			i++;
			len |= p[i];
			new_packet = false;
			if (!len) //check for packet sanity, if no len - skip
				continue;
			if (len > MAX_PACKET_SIZE)
				printf("parsing 0xEE format we have big len: %d , at addr: %p\n",
					len, p+i);
			new_len = true; 
			debug("new packet len: %d , ts: %lu \n", len, ts);
			continue;
		}
		if (new_len)
		{
			pkt = queue_create_pckt();
			if (!pkt)
				error("failed to allocate RAM for a new packet!\n");
			else
			{
				pkt->len = len;
				pkt->ptr = malloc(len);
				if (!pkt->ptr)
					error("failed to allocate RAM for a new packet!\n");
				memcpy(pkt->ptr, p+i, len);
				i += len - 1; //we skip till next 0xEE
				new_len = false;
#ifdef LOCKLESS
				num = rte_ring_sp_enqueue(fd->rbuf, pkt);
#else
				num = queue_add(pkt);
#endif
				if (num >= 0)
				{
#ifdef LOCKLESS
					pkt_stat.pkts_read++;
					debug("packets in lockless ring: %u, free entries: %u\n",
					      rte_ring_count(fd->rbuf), rte_ring_free_count(fd->rbuf));
#else
					debug("packet added to queue. now in queue: %d\n", num);
#endif
				}
				else
				{
					error("failed to add packet in queue/ring: %d \n", num);
					usleep(100000);
					if (pkt->ptr)
					{	free(pkt->ptr); pkt->ptr = NULL;
					}
				}
			}
		}
	}

	debug("%s() exited \n", __func__);
}

static void* poller_thread_f(void *arg)
{
	libtrace_t *libtrace = (libtrace_t*)arg;

	while(1)
	{
		pls_poll_thread(FORMAT(libtrace)->t);
		//usleep(2700);
	}

	return NULL;
}

//we run it in separate thread to avoid blocking issues
static void* reader_thread_f(void *arg)
{
	int rv;
	libtrace_t *libtrace = (libtrace_t*)arg;
	uint64_t nbytes = BUFFER_SIZE;
        uint64_t offset = 0;
	void *bf = NULL;

	debug("reader thread started\n");

        //set higher priority
        rv = nice(-20);
        printf("set reader thread priority to : %d \n", rv);

	spdk_format_data_t *fd = FORMAT(libtrace);
	if (!fd)
	{
                printf("failed to get format data ptr\n");
                rv = -1; return NULL;
	}

	pls_thread_t *t = FORMAT(libtrace)->t;
	if (!t)
	{
                printf("failed to get thread ptr\n");
                rv = -1; return NULL;
	}

        t->ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 4096, 
					SPDK_ENV_SOCKET_ID_ANY);
        if (!t->ring)
        {
                printf("failed to allocate ring\n");
                rv = -1; return NULL;
        }

        t->thread = spdk_allocate_thread(pls_send_msg, pls_start_poller,
                    	pls_stop_poller, (void*)t, "pls_reader_thread");
        if (!t->thread)
        {
                spdk_ring_free(t->ring);
                SPDK_ERRLOG("failed to allocate thread\n");
                rv = -1; return NULL;
        }

        TAILQ_INIT(&t->pollers);

#if 0
        struct raid_bdev_config *raid_cfg = NULL;
        //raid_cfg = raid_bdev_config_find_by_name(RAID_DEVICE);
        raid_cfg = spdk_construct_raid_cfg(RAID_DEVICE);
        if (!raid_cfg)
        {
                printf("<failed to get raid config>\n");
                rv = 1; return NULL;
        }

        t->pls_target.bd = &raid_cfg->raid_bdev->bdev;
        if (!t->pls_target.bd)
        {
                printf("<failed to get raid device from config>\n");
                rv = 1; return NULL;
        }
        else
                printf("got raid device with name [%s]\n", 
			t->pls_target.bd->name);
#endif

	//rbd creation
	t->pls_target.bd = fd->bd;
	if (!t->pls_target.bd)
	{
		printf("<failed to get bdev rbd device from global>\n");
		rv = 1; return NULL;
	}
	else
		printf("got rbd device with name [%s]\n", t->pls_target.bd->name);

        rv = spdk_bdev_open(t->pls_target.bd, 1, NULL, NULL, 
				&t->pls_target.desc);
        if (rv)
        {
                printf("failed to open device\n");
                return NULL;
        }

        fd->block_size = spdk_bdev_get_block_size(t->pls_target.bd);
        fd->num_blocks = spdk_bdev_get_num_blocks(t->pls_target.bd);
        fd->bytes = fd->block_size * fd->num_blocks;
        printf("device block size is: %u bytes, num blocks: %lu, bytes: %lu \n",
                fd->block_size, fd->num_blocks, fd->bytes);
        fd->max_offset = fd->block_size * fd->num_blocks - 1;
        printf("max offset(bytes): 0x%lx\n", fd->max_offset);

        printf("open io channel\n");
        t->pls_target.ch = spdk_bdev_get_io_channel(t->pls_target.desc);
        if (!t->pls_target.ch)
        {
                printf("Unable to get I/O channel for bdev.\n");
                spdk_bdev_close(t->pls_target.desc);
                rv = -1; return NULL;
        }

//------ write to file clean data experiment
#if 0
        int f = 0;
        char fname[256] = {0};

        strcpy(fname, "trace_raid_file");
        f = open(fname, O_CREAT|O_WRONLY|O_TRUNC);
        if (!f)
        {
                printf("can't create file\n");
                exit(1);
        }
#endif
//-------

        while(1)
        {
                bf = spdk_dma_zmalloc(nbytes, 0, NULL);
                if (!bf)
                {
                	printf("failed to allocate RAM for reading\n");
			return NULL;
		}
		t->read_complete = false;

		rv = spdk_bdev_read(t->pls_target.desc, t->pls_target.ch,
			bf, offset, nbytes, pls_bdev_read_done_cb, t);
                if (rv)
			printf("spdk_bdev_read failed\n");
		else
		{
			offset += nbytes;
			//readbytes += nbytes;
		}
		if (offset + BUFFER_SIZE > fd->max_offset)
		{
                	fd->overwrap_read_cnt++;
                        offset = 0;
                        printf("read overwrap: %lu. read offset reset to 0\n", 
				fd->overwrap_read_cnt);
                }

		/*need to wait for bdev read completion first*/
                while(t->read_complete == false)
                {
                        usleep(10);
                }
		/* parse buf with packets and add packets to queue */
		pkt_parser(bf, fd); //@fd - pointer to format data
//----- XXX file writing experiment
#if 0
		write(f, bf, BUFFER_SIZE);

#endif
//-----
		spdk_dma_free(bf);
	}

	return NULL;
}

static int rbd_start_input(libtrace_t *libtrace) 
{
	int rv = 0;
	//XXX - check pthread_t address
	rv = pthread_create(&FORMAT(libtrace)->t->pthread_desc, NULL, reader_thread_f, libtrace);
	if (rv)
		error("failed to create a reader thread!\n");
	else
	{	debug("reader thread created successfully\n");
	}

	//create poller thread - to get callbacks
	rv = pthread_create(&poller_thread, NULL, poller_thread_f, libtrace);
	if (rv)
		error("failed to create a poller thread!\n");
	else
	{	debug("poller thread created successfully\n");
	}

	return rv;
}

/* Pauses an input trace - this function should close or detach the file or 
   device that is being read from. 
   @return 0 if successful, -1 in the event of error
*/
static int rbd_pause_input(libtrace_t * libtrace) 
{
	(void)libtrace;

	debug("%s() \n", __func__);

	debug("fake function. instead of pausing input - do nothing \n");

	return 0;
}

static int lodp_flush_output(libtrace_out_t *libtrace UNUSED)
{
	//XXX
        //return wandio_wflush(OUTPUT->file);
        return 0;
}

//Initialises an output trace using the capture format.
static int rbd_init_output(libtrace_out_t *libtrace) 
{
	int rv = 0;
	(void)libtrace;

	debug("%s() \n", __func__);

	return rv;
}

static int rbd_config_output(libtrace_out_t *libtrace, trace_option_output_t option, void *data)
{
	int rv = 0;
	(void)libtrace;
	(void)option;
	(void)data;

	debug("%s() \n", __func__);

	return rv;
}

static int rbd_start_output(libtrace_out_t *libtrace) 
{
	int rv = 0;
	(void)libtrace;

	debug("%s() \n", __func__);

	return rv;
}

static int rbd_fin_input(libtrace_t *libtrace) 
{
	int rv = 0;
	rte_ring_free(FORMAT(libtrace)->rbuf);

	debug("%s() \n", __func__);

	return rv;
}

static int rbd_fin_output(libtrace_out_t *libtrace) 
{
	int rv = 0;
	(void)libtrace;

	debug("%s() \n", __func__);

	return rv;
}

/*
Converts a buffer containing a packet record into a libtrace packet
should be called in odp_read_packet()
Updates internal trace and packet details, such as payload pointers,
loss counters and packet types to match the packet record provided
in the buffer. This is a zero-copy function.
*/
static int lodp_prepare_packet(libtrace_t *libtrace UNUSED, libtrace_packet_t *packet,
		void *buffer, libtrace_rt_types_t rt_type, uint32_t flags) 
{
	debug("%s() \n", __func__);

	//in theory we don't have packets allocated with TRACE_CTRL_PACKET
	if (packet->buffer != buffer && packet->buf_control == TRACE_CTRL_PACKET)
                free(packet->buffer);

        if ((flags & TRACE_PREP_OWN_BUFFER) == TRACE_PREP_OWN_BUFFER) {
                packet->buf_control = TRACE_CTRL_PACKET;
        } else
                packet->buf_control = TRACE_CTRL_EXTERNAL; //XXX - we already set it in odp_read_packet()

/*	void *header;			**< Pointer to the framing header *
 *	void *payload;			**< Pointer to the link layer *
 *	void *buffer;			**< Allocated buffer */
        packet->buffer = buffer;
        packet->header = buffer;

/*	MOVED THIS PART to lodp_read_packet()
	-----
	packet->payload = FORMAT(libtrace)->l2h; //XXX - maybe do it as in dpdk with dpdk_get_framing_length?
	packet->capture_length = FORMAT(libtrace)->pkt_len;
	packet->wire_length = FORMAT(libtrace)->pkt_len + WIRELEN_DROPLEN;
	-----
*/
	//packet->payload = (char *)buffer + dpdk_get_framing_length(packet);
	packet->type = rt_type;

#if 0
	if (libtrace->format_data == NULL) {
		if (odp_init_input(libtrace))
			return -1;
	}
#endif

	return 0;
}

#if 0
static int spdk_read_pack(libtrace_t *libtrace)
{
	int numbytes = 0;
	pckt_t *pkt = NULL;

	while (1) 
	{
		//if we got Ctrl-C from one of our utilities, etc
		if (libtrace_halt)
		{
			printf("[got halt]\n");
			return READ_EOF;
		}

		if (queue_num)
		{
			pkt = queue_de();
			if (pkt)
			{
				numbytes = pkt->len;
				debug("have packet with len %d, left in queue: %d \n", numbytes, queue_num);
				break;
			}
		}
		usleep(10000);

		
		return numbytes;
	}

	/* We'll NEVER get here */
	return READ_ERROR;
}
#endif


/* Reads the next packet from an input trace into the provided packet 
 * structure.
 *
 * @param libtrace      The input trace to read from
 * @param packet        The libtrace packet to read into
 * @return The size of the packet read (in bytes) including the capture
 * framing header, or -1 if an error occurs. 0 is returned in the
 * event of an EOF. 
 *
 * If no packets are available for reading, this function should block
 * until one appears or return 0 if the end of a trace file has been
 * reached.
 */

//So endless loop while no packets and return bytes read in case there is a packet (no one checks returned bytes)
static int rbd_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) 
{
	int rv = 0;
	uint32_t flags = 0;
	int numbytes = 0;
	pckt_t *pkt = NULL;
	
	debug("%s() \n", __func__);

	//print stats
	if (pkt_stat.pkts_read % 10000 == 0)
	{
#ifdef HL_DEBUGS
	#ifdef LOCKLESS
		printf("total pkts read: %lu, total pkts finished: %lu , in ring: %u\n",
			pkt_stat.pkts_read, pkt_stat.pkts_finished,
			rte_ring_count(FORMAT(libtrace)->rbuf));
	#else
		printf("total pkts read: %lu, total pkts finished: %lu , in queue: %d\n",
			pkt_stat.pkts_read, pkt_stat.pkts_finished, queue_num);
	#endif
#endif
	}

	//#0. Free the last packet buffer
	if (packet->buffer) 
	{
		//Check buffer memory is owned by the packet. It is if flag is TRACE_CTRL_PACKET
		assert(packet->buf_control == TRACE_CTRL_PACKET); 
		free(packet->buffer);
		packet->buffer = NULL;
	}

	//#1. Set packet fields
	//TRACE_CTRL_EXTERNAL means buffer memory is owned by an external source
	packet->buf_control = TRACE_CTRL_EXTERNAL;
	packet->type = TRACE_RT_DATA_SPDK; //XXX - does it affect something?

	//#2. Read a packet . We wait here forever till packet appears.
	while (1) 
	{
		//if we got Ctrl-C from one of our utilities, etc
		if (libtrace_halt)
		{
			printf("[got halt]\n");
			printf("total pkts read: %lu, total pkts finished: %lu \n",
				pkt_stat.pkts_read, pkt_stat.pkts_finished);
			return READ_EOF;
		}
#ifdef LOCKLESS
		rv = rte_ring_sc_dequeue(FORMAT(libtrace)->rbuf, (void **)&pkt);
		if (!rv)
		{
			if (pkt)
			{
				numbytes = pkt->len;
				debug("ring dequeue. packet with len %d. left: %u \n",
					numbytes, rte_ring_count(FORMAT(libtrace)->rbuf));
				break;
			}
		}
		else
		{
#ifdef HL_DEBUGS
			printf("ring is empty\n");
#endif
			usleep(10000);
		}
#else
		if (queue_num)
		{
			pkt = queue_de();
			if (pkt)
			{
				numbytes = pkt->len;
				debug("have packet with len %d, left in queue: %d \n",
				       numbytes, queue_num);
				break;
			}
		}
		else
		{
			printf("queue is empty\n");
			usleep(10000);
		}
		//usleep(10000);	//to avoid eating 100% cpu if no packets
#endif
	}
	if (numbytes == -1) 
	{
		trace_set_err(libtrace, errno, "Reading packet failed");
		return -1;
	}
	else if (numbytes == 0)
		return 0;

	//#3. Get pointer from packet and assign it to packet->buffer
	if (!packet->buffer || packet->buf_control == TRACE_CTRL_EXTERNAL) 
	{
		packet->buffer = pkt->ptr;
		packet->cached.capture_length = pkt->len;
		packet->payload = packet->buffer;
		packet->cached.wire_length = pkt->len + WIRELEN_DROPLEN;
		debug("pointer to packet: %p \n", packet->buffer);
                if (!packet->buffer) 
		{
                        trace_set_err(libtrace, errno, 
				      "Cannot alloc memory or have invalid pointer to packet");
                        return -1;
                }
        }

	if (lodp_prepare_packet(libtrace, packet, packet->buffer, packet->type, flags))
		return -1;

	//we don't need a queue packet cover anymore, but we keep ptr to packet and len
	//in packet->buffer and packet->capture_length
	free(pkt);

	return numbytes;
}

static void rbd_fin_packet(libtrace_packet_t *packet)
{
	debug("%s() \n", __func__);

	if (packet->buf_control == TRACE_CTRL_EXTERNAL) 
	{
                if (packet->buffer)
                {
			free(packet->buffer);
			packet->buffer = NULL;
			pkt_stat.pkts_finished++;
		}
	}
}

static int rbd_write_packet(libtrace_out_t *libtrace, libtrace_packet_t *packet)
{
	int numbytes = 0;
	(void)libtrace;
	(void)packet;

	debug("%s() \n", __func__);

	return numbytes;
}

//Returns the payload length of the captured packet record
//We use the value we got from odp and stored in FORMAT(libtrace)->pkt_len
static int lodp_get_capture_length(const libtrace_packet_t *packet)
{
	int pkt_len;

	debug("lodp_get_capture_length() called! \n");

	if (packet)
	{
		// this won't work probably, as we don't set packet->length anywhere, so can't return it.
		//pkt_len = (int)trace_get_capture_length(packet);
		//pkt_len = FORMAT(libtrace)->pkt_len;
		pkt_len = packet->cached.capture_length;
		debug("packet: %p , length: %d\n", packet, pkt_len);
		return pkt_len;
	}
	else
	{
		debug("NO packet. \n");
		trace_set_err(packet->trace,TRACE_ERR_BAD_PACKET, "Have no packet");
		return -1;
	}
}

static int lodp_get_framing_length(const libtrace_packet_t *packet) 
{
	debug("lodp_get_framing_length() called! \n");

	if (packet)
		//return trace_get_framing_length(packet);
		return 0; //XXX - TODO, fix it, this is just for test
	else
	{
		trace_set_err(packet->trace,TRACE_ERR_BAD_PACKET, "Have no packet");
		return -1;
	}
}

//Returns the original length of the packet as it was on the wire
static int lodp_get_wire_length(const libtrace_packet_t *packet) 
{
	debug("lodp_get_wire_length() called! \n");

	if (packet)
		//return trace_get_wire_length(packet);
		return packet->cached.wire_length;
	else
	{
		trace_set_err(packet->trace,TRACE_ERR_BAD_PACKET, "Have no packet");
		return -1;
	}
}

static libtrace_linktype_t lodp_get_link_type(const libtrace_packet_t *packet UNUSED) 
{
	debug("%s() \n", __func__);

	return TRACE_TYPE_ETH;	//We have Ethernet for ODP and in DPDK.
}

//returns timestamp from a packet or time now (as hack)
static double lodp_get_seconds(const libtrace_packet_t *packet)
{
	double seconds = 0.0f;
	time_t t;
	const void *p;

	//avoid warning about unused packet var
	p = packet;
	(void)p;

	time(&t);

	seconds = (double)t;
	debug("packet framing header is : %p, time : %.0f \n",
		packet->header, seconds);

	return seconds;
}

//sequence of calling time functions from trace_get_erf_timestamp():
//1)erf 2)timespec 3)timewal 4)seconds
static struct timeval lodp_get_timeval(const libtrace_packet_t *packet)
{
	struct timeval tv;
	const void *p;

	//avoid warning about unused packet var
	p = packet;
	(void)p;

	gettimeofday(&tv, NULL);
/*
	debug("packet header: %p, seconds: %zu , microseconds: %zu \n",
		packet->header, tv.tv_sec, tv.tv_usec);
*/

	return tv;
}

static void rbd_help(void)
{
	printf("Endace SPDK format module\n");
	printf("Supported input uris:\n");
	printf("\todp:/path/to/input/file\n");
	printf("Supported output uris:\n");
	printf("\todp:/path/to/output/file\n");
	printf("\n");
	return;
}


static int rbd_pstart_input(libtrace_t *libtrace)
{
	int ret = 0;
	(void)libtrace;


	return ret;
}

/**
 * Read a batch of packets from the input stream related to thread.
 * At most read nb_packets, however should return with less if packets
 * are not waiting. However still must return at least 1, 0 still indicates
 * EOF.
 *
 * @param libtrace	The input trace
 * @param t	The thread
 * @param packets	An array of packets
 * @param nb_packets	The number of packets in the array (the maximum to read)
 * @return The number of packets read, or 0 in the case of EOF or -1 in error or -2 to represent
 * interrupted due to message waiting before packets had been read.
 */
//we mostly read 10 packets in loop and then exit, this is how actually function works
static int rbd_pread_packets(libtrace_t *trace, libtrace_thread_t *t, 
			      libtrace_packet_t **packets, size_t nb_packets)
{
	int pkts_read = 0;
/*
	int numbytes = 0;
	uint32_t flags = 0;
	unsigned int i;
*/
	(void)trace;
	(void)t;
	(void)packets;
	(void)nb_packets;

	debug("%s() \n", __func__);





	debug("%s() exit with pkts_read : %d \n", __func__, pkts_read);

	return pkts_read;
}

/* libtrace creates threads with pthread_create(), then fills
  libtrace_thread_t struct and passes ptr to it here (*t) */
static int lodp_pregister_thread(libtrace_t *libtrace, libtrace_thread_t *t, bool reader)
{
	int rv = 0;

	libtrace=libtrace;

	debug("%s() \n", __func__);

	if (reader)
	{
		debug("trying to register a reader thread : %p , type: %d , tid: %lu , perpkt_num: %d \n", 
			t, t->type, t->tid, t->perpkt_num);

		//Bind thread and its per_thread struct
		if(t->type == THREAD_PERPKT) 
		{
			t->format_data = libtrace_list_get_index(FORMAT(libtrace)->per_stream,
								 t->perpkt_num)->data;
			if (t->format_data == NULL) 
			{
				trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
				              "Too many threads registered");
				return -1;
			}
		}
	}
	else
	{
	 debug("trying to register not reading thread : %p , type: %d , tid: %lu , perpkt_num: %d \n", 
		t, t->type, t->tid, t->perpkt_num);
	}

	return rv;	
}

static void lodp_punregister_thread(libtrace_t *libtrace, libtrace_thread_t *t)
{
	libtrace = libtrace;
	t = t;

	debug("%s() \n", __func__);

	debug("unregistering thread : %p , type: %d , tid: %lu , perpkt_num: %d \n", 
		t, t->type, t->tid, t->perpkt_num);

	return;
}

/* A libtrace capture format module */
/* All functions should return -1, or NULL on failure */
static struct libtrace_format_t rbd = {
        "rbd",				/* name used in URI to identify capture format - rbd:iface */
        "$Id$",				/* version of this module */
        TRACE_FORMAT_RBD,		/* The RT protocol type of this module */
	NULL,				/* probe filename - guess capture format - NOT NEEDED*/
	NULL,				/* probe magic - NOT NEEDED*/
        rbd_init_input,	        	/* init_input - Initialises an input trace using the capture format */
        NULL,                           /* config_input - Sets value to some option */
        rbd_start_input,	        /* starts or unpause an input trace (also opens file or device for reading) */
        rbd_pause_input,               	/* pause_input */
        rbd_init_output,               	/* init_output - Initialises an output trace using the capture format. */
        rbd_config_output,             	/* config_output */
        rbd_start_output,              	/* start_output */
        rbd_fin_input,	         	/* fin_input - Stops capture input data.*/
        rbd_fin_output,                	/* fin_output */
        rbd_read_packet,        	/* read_packet - Reads next packet from input trace into the packet structure */
        lodp_prepare_packet,		/* converts a buffer containing a packet record into a libtrace packet */
	rbd_fin_packet,                	/* fin_packet - Frees any resources allocated for a libtrace packet */
        rbd_write_packet,              	/* write_packet - Write a libtrace packet to an output trace */
	lodp_flush_output,		/* flush output buffers, new API starting from libtrace 4.0.6 */
        lodp_get_link_type,    		/* get_link_type - Returns the libtrace link type for a packet */
        NULL,              		/* get_direction */
        NULL,              		/* set_direction */
	NULL,				/* get_erf_timestamp */
/*	lodp_get_erf_timestamp,         */
        lodp_get_timeval,               /* get_timeval */
	NULL,				/* get_timespec */
        lodp_get_seconds,               /* get_seconds */
        NULL,                   	/* seek_erf */
        NULL,                           /* seek_timeval */
        NULL,                           /* seek_seconds */
        lodp_get_capture_length,  	/* get_capture_length */
        lodp_get_wire_length,  		/* get_wire_length */
        lodp_get_framing_length, 	/* get_framing_length */
        NULL,         			/* set_capture_length */
	NULL,				/* get_received_packets */
	NULL,				/* get_filtered_packets */
	NULL,				/* get_dropped_packets */
	NULL,				/* get_statistics */
        NULL,                           /* get_fd */
        NULL,              		/* trace_event */
        rbd_help,                     	/* help */
        NULL,                           /* next pointer */
	{true, 8},                      /* Live, NICs typically have 8 threads */
	rbd_pstart_input,              /* pstart_input */
	rbd_pread_packets,             /* pread_packets */
	rbd_pause_input,               /* ppause */
	rbd_fin_input,                 /* p_fin */
	lodp_pregister_thread,          /* pregister_thread */
	lodp_punregister_thread,        /* punregister_thread */
	NULL				/* get thread stats */ 
};

void rbd_constructor(void) 
{
	debug("registering rbd struct with address: %p , init_output: %p\n", 
	      &rbd, rbd.init_output);
	register_format(&rbd);
}
