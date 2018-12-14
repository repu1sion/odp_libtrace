/* format spdk (raid) support */

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

#define ERR_SIZE 512
#define DEVNAME_SIZE 32
#define NVME_MAX_BDEVS_PER_RPC 32
//raid
#define STRIPE_SIZE 512                 //it's in Kb already
#define RAID_DEVICE "pulseraid"
#define NUM_RAID_DEVICES 2
#define RAID1 "0000:04:00.0"
#define RAID2 "0000:05:00.0"
#define DEVICE_NAME "s4msung"
#define DEVICE_NAME_NQN "s4msungnqn"

#define FORMAT(x) ((spdk_format_data_t *)x->format_data)
#ifdef DEBUG
 #define debug(x...) printf(x)
#else
 #define debug(x...)
#endif

//global data
typedef struct spdk_format_data_s
{
	char *pci_nvme_addr[NUM_RAID_DEVICES];
	char devname[NUM_RAID_DEVICES][DEVNAME_SIZE];
	const char *names[NVME_MAX_BDEVS_PER_RPC];
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

//------------------ spdk functions --------------------------------------------
static size_t pls_poll_thread(pls_thread_t *thread)
{
        struct pls_msg *msg;
        struct pls_poller *p, *tmp;
        size_t count;

        printf("%s() called \n", __func__);

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

//spdk init, which could be used for both: spdk_init_input() and spdk_init_output()
static int spdk_init_environment(char *uridata, spdk_format_data_t *fd, char *err, int errlen)
{
	int rv = 0;
        int i, j;
        bool done = false;
        size_t cnt;
        struct spdk_env_opts opts;
        struct spdk_nvme_transport_id trid[NUM_RAID_DEVICES] = {{0}};
        size_t count = NVME_MAX_BDEVS_PER_RPC;

        printf("%s() called \n", __func__);

        spdk_log_set_print_level(SPDK_LOG_DEBUG);
        spdk_log_set_level(SPDK_LOG_DEBUG);
        spdk_log_open();

        spdk_env_opts_init(&opts);
        opts.name = "bdev_raid";

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

        rv = spdk_construct_raid_bdev(RAID_DEVICE, STRIPE_SIZE, 0, NUM_RAID_DEVICES, fd->names[0], fd->names[1]);
        if (!rv)
                printf("[raid created successfully]\n");
        else
                printf("<failed to create raid>\n");

        return rv;
}

/* Initialises an input trace using the capture format. 
   @param libtrace 	The input trace to be initialised */
static int spdk_init_input(libtrace_t *libtrace) 
{
	int i;
	char err[ERR_SIZE] = {0};

	printf("%s() \n", __func__);

	spdk_per_stream_t stream;
	memset(&stream, 0x0, sizeof(spdk_per_stream_t));

	//init all the data in spdk_format_data_t
	libtrace->format_data = malloc(sizeof(spdk_format_data_t));
	memset(libtrace->format_data, 0x0, sizeof(spdk_format_data_t));
	FORMAT(libtrace)->t = calloc(1, sizeof(pls_thread_t));
	FORMAT(libtrace)->pvt = 0xFAFAFAFA;
	FORMAT(libtrace)->pkts_read = 0;
	FORMAT(libtrace)->num_threads = libtrace->perpkt_thread_count;
	FORMAT(libtrace)->pci_nvme_addr[0] = strdup(RAID1);
	FORMAT(libtrace)->pci_nvme_addr[1] = strdup(RAID2);
        for (i = 0; i < NUM_RAID_DEVICES; i++)
        {
                char c[2] = {0};
                strcpy(FORMAT(libtrace)->devname[i], DEVICE_NAME);
                sprintf(c, "%d", i+1);
                strcat(FORMAT(libtrace)->devname[i], c);
        }
	/* Make our first stream */
	FORMAT(libtrace)->per_stream = libtrace_list_init(sizeof(spdk_per_stream_t));
	libtrace_list_push_back(FORMAT(libtrace)->per_stream, &stream);//copies inside, so its ok to alloc on stack.

	if (spdk_init_environment(libtrace->uridata, FORMAT(libtrace), err, sizeof(err))) 
	{
		trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "%s", err);
		free(libtrace->format_data);
		libtrace->format_data = NULL;
		return -1;
	}
	return 0;
}

static int spdk_start_input(libtrace_t *libtrace) 
{
	pls_thread_t *t = FORMAT(libtrace)->t;
	if (!t)
	{
                printf("failed to get thread ptr\n");
                rv = -1; return NULL;
	}

        t->ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 4096, SPDK_ENV_SOCKET_ID_ANY);
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
                return NULL;
        }

        TAILQ_INIT(&t->pollers);

        struct raid_bdev_config *raid_cfg = NULL;
        raid_cfg = raid_bdev_config_find_by_name(RAID_DEVICE);
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
                printf("got raid device with name [%s]\n", t->pls_target.bd->name);

        rv = spdk_bdev_open(t->pls_target.bd, 1, NULL, NULL, &t->pls_target.desc);
        if (rv)
        {
                printf("failed to open device\n");
                return NULL;
        }

        fd->block_size = spdk_bdev_get_block_size(bd);
        fd->num_blocks = spdk_bdev_get_num_blocks(bd);
        fd->bytes = global.block_size * global.num_blocks;
        fd->kb = global.bytes / 1024;
        printf("device block size is: %u bytes, num blocks: %lu, bytes: %lu, kb: %lu\n",
                fd->block_size, fd->num_blocks, fd->bytes, fd->kb);
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
}


/* A libtrace capture format module */
/* All functions should return -1, or NULL on failure */
static struct libtrace_format_t spdk = {
        "spdk",				/* name used in URI to identify capture format - spdk:iface */
        "$Id$",				/* version of this module */
        TRACE_FORMAT_SPDK,		/* The RT protocol type of this module */
	NULL,				/* probe filename - guess capture format - NOT NEEDED*/
	NULL,				/* probe magic - NOT NEEDED*/
        spdk_init_input,	        /* init_input - Initialises an input trace using the capture format */
        NULL,                           /* config_input - Sets value to some option */
        spdk_start_input,	        /* start_input-Starts or unpause an input trace (also opens file or device for reading)*/
        spdk_pause_input,               /* pause_input */
        spdk_init_output,               /* init_output - Initialises an output trace using the capture format. */
        spdk_config_output,             /* config_output */
        spdk_start_output,              /* start_output */
        spdk_fin_input,	         /* fin_input - Stops capture input data.*/
        spdk_fin_output,                /* fin_output */
        spdk_read_packet,        	 /* read_packet - Reads next packet from input trace into the packet structure */
        lodp_prepare_packet,		/* prepare_packet - Converts a buffer containing a packet record into a libtrace packet */
	spdk_fin_packet,                /* fin_packet - Frees any resources allocated for a libtrace packet */
        spdk_write_packet,              /* write_packet - Write a libtrace packet to an output trace */
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
        lodp_help,                     	/* help */
        NULL,                           /* next pointer */
	{true, 8},                      /* Live, NICs typically have 8 threads */
	spdk_pstart_input,              /* pstart_input */
	spdk_pread_packets,             /* pread_packets */
	spdk_pause_input,               /* ppause */
	spdk_fin_input,                 /* p_fin */
	lodp_pregister_thread,          /* pregister_thread */
	lodp_punregister_thread,        /* punregister_thread */
	NULL				/* get thread stats */ 
};

void spdk_constructor(void) 
{
	debug("registering spdk struct with address: %p , init_output: %p\n", &spdk, spdk.init_output);
	register_format(&spdk);
}
