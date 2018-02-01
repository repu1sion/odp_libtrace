#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <syslog.h>
#include <pthread.h>

#include "config.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_helper.h"
#include "wandio.h"
#include "rt_protocol.h"

#include <odp_api.h>
#include <libxio.h>

#define FORMAT(x) ((struct acce_format_data_t *)x->format_data)
#define DATAOUT(x) ((struct acce_format_data_out_t *)x->format_data)
#define OUTPUT DATAOUT(libtrace)

#define WIRELEN_DROPLEN 4

//----- CONFIG -----
#define ACCE_VERSION		"0.99"
#define ACCE_QUEUE_DEPTH	1048576
#define ACCE_MAX_MSG_SIZE	16384
#define ACCE_BATCH_SIZE 	10000
#define ACCE_MIN_BATCH_SIZE 	2000
#define ACCE_DEC_BATCH_STEP 	2000
#define ACCE_CHECK_TRAFFIC_SEC	5
#define ACCE_CRITICAL_DIFF	50000		//diff between messages sent and callbacks received
#define ACCE_SERVER 		"localhost"	//default value if no env var set
#define ACCE_PORT 		"9992"
#define ACCE_SRV_USE_MALLOC			//must be used
#define SERVER_LEN 		512

//----- OPTIONS -----
//#define DEBUG
#define ERROR_DBG
//#define OPTION_OUTPUT_FILE			//in case we also want to save packets into file on client side
//#define OPTION_PRINT_PACKETS

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

struct acce_format_data_t
{
	//accelio vars
	struct xio_server       *server;
	struct xio_context      *ctx;
        struct xio_connection   *conn;
        int max_msg_size;

	//other vars
	void *pkt;				
	int pkt_len;				//length of current packet
	int pvt;				//for private data saving
	unsigned int pkts_read;			//read by libtrace
	unsigned int pkts_rcvd;			//received via accelio but not read yet
	u_char *l2h;				//l2 header for current packet
	libtrace_list_t *per_stream;		//pointer to the whole list structure: head, tail, size etc inside.
	pthread_t thread;
};

struct acce_format_data_out_t 
{
	//accelio vars
	struct xio_context *ctx;
        struct xio_connection *conn;
	struct xio_session *session;
        uint64_t rcvd_cb_cnt;				//received callbacks
        uint64_t cnt;					//packets sent
        int max_msg_size;
	unsigned char conn_established;
	int batchsize;
	
	//other vars
	int level;
	int compress_type;			//store compression type here: bz2, gz etc
	int fileflag;
	iow_t *file;
	pthread_t thread;
};

//queue implementation----------------------------------------------------------
typedef struct pckt_s
{
        void *ptr;
        int len;
        struct pckt_s *next;
} pckt_t;

//input queue (on server)
pckt_t *queue_head = NULL;
pckt_t *queue_tail = NULL;
int queue_num = 0;
int pshared;
pthread_spinlock_t queue_lock;

static int queue_add(pckt_t *pkt)
{
	if (!pkt)
	{
		error("trying to add to queue NULL pkt!\n");
		return -1;
	}

	pthread_spin_lock(&queue_lock);
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
	pthread_spin_unlock(&queue_lock);

	return queue_num;
}

static pckt_t* queue_de()
{
        pckt_t *deq = NULL;

	//pthread_spin_trylock(&queue_lock);
	pthread_spin_lock(&queue_lock);
        if (queue_head)
        {
                deq = queue_head;
                if (queue_head != queue_tail) //not last element
                {
                        queue_head = queue_head->next;
			if (!queue_head)
			{
				pthread_spin_unlock(&queue_lock);
				error("we lost head but queue is not empty: %d\n", queue_num);
				return NULL;
			}
                }
                else //last element
                {
                        queue_head = queue_tail = NULL;
                }
                queue_num--;
		pthread_spin_unlock(&queue_lock);
                return deq;
        }
        else
	{
		pthread_spin_unlock(&queue_lock);
                return NULL;
	}
}

static pckt_t* queue_create_pckt()
{
	pckt_t *p = malloc(sizeof (pckt_t));
	if (!p)
		return NULL;
	else
		memset(p, 0x0, sizeof (pckt_t));
	return p;
}

//------------------------ output queue ----------------------------------------
pckt_t *o_queue_head = NULL;
pckt_t *o_queue_tail = NULL;
int o_queue_num = 0;
pthread_mutex_t o_mutex_lock = PTHREAD_MUTEX_INITIALIZER;

static int o_queue_add(pckt_t *pkt)
{
	pthread_mutex_lock(&o_mutex_lock);
        if (!o_queue_head)
        {
                o_queue_head = pkt;
                o_queue_tail = pkt;
        }
        else
        {
                o_queue_tail->next = pkt;
                o_queue_tail = pkt;
        }
        pkt->next = NULL;
        o_queue_num++;
	pthread_mutex_unlock(&o_mutex_lock);

	return o_queue_num;
}

//IMPORTANT: use mutex outside of o_queue_de() and call it in loop till the queue will be empty
static pckt_t* o_queue_de()
{
        pckt_t *deq = NULL;

        if (o_queue_head)
        {
                deq = o_queue_head;
                if (o_queue_head != o_queue_tail)
                {
                        o_queue_head = o_queue_head->next;
                }
                else
                {
                        o_queue_head = o_queue_tail = NULL;
                }
                o_queue_num--;
                return deq;
        }
        else
	{
                return NULL;
	}
}

#ifdef OPTION_PRINT_PACKETS
static void hexdump(void *addr, unsigned int size)
{
        unsigned int i;
        /* move with 1 byte step */
        unsigned char *p = (unsigned char*)addr;

        /*printf("addr : %p \n", addr);*/

        if (!size)
        {
                printf("bad size %u\n",size);
                return;
        }

        for (i = 0; i < size; i++)
        {
                if (!(i % 16))    /* 16 bytes on line */
                {
                        if (i)
                                printf("\n");
                        printf("0x%lX | ", (long unsigned int)(p+i)); /* print addr at the line begin */
                }
                printf("%02X ", p[i]); /* space here */
        }

        printf("\n");
}
#endif

/* disconnect sequence:
   --------------------
   client session event: connection closed. reason: Session closed
   client session event: connection teardown. reason: Session closed
   client session event: session teardown. reason: Session closed */

//----------------- client callbacks -------------------------------------------
static int on_session_event_client(struct xio_session *session,
                            struct xio_session_event_data *event_data,
                            void *cb_user_context)
{
        struct acce_format_data_out_t *session_data = (struct acce_format_data_out_t*)cb_user_context;

        printf("client session event: %s. reason: %s\n",
               xio_session_event_str(event_data->event), xio_strerror(event_data->reason));

        switch (event_data->event) 
	{
		case XIO_SESSION_CONNECTION_ESTABLISHED_EVENT:
			session_data->conn_established = 1;
			break;
		case XIO_SESSION_CONNECTION_TEARDOWN_EVENT:
			session_data->conn_established = 0;
			xio_connection_destroy(event_data->conn);
			break;
		case XIO_SESSION_TEARDOWN_EVENT:
			xio_session_destroy(session);
			xio_context_stop_loop(session_data->ctx);  /* exit */
			break;
		default:
			break;
        };

        return 0;
}

/* we receive exactly same msg pointer we used to send packet */
static int on_msg_send_complete_client(struct xio_session *session,
					struct xio_msg *msg, void *cb_user_context)
{
	session = session;
        struct acce_format_data_out_t *session_data = (struct acce_format_data_out_t*)cb_user_context;

	static int showerror = 1;
	static time_t errortime = 0;
	static unsigned long olddiff = 0;
	unsigned long diff = 0;

	session_data->rcvd_cb_cnt++;
	debug("%s() rcvd: %lu, msg: %p\n", __func__, session_data->rcvd_cb_cnt, msg);

	diff = session_data->cnt - session_data->rcvd_cb_cnt;
	if (diff > ACCE_CRITICAL_DIFF)
	{
		if (showerror)
		{
			if (diff > olddiff)
			{
				if (session_data->batchsize > ACCE_MIN_BATCH_SIZE)
				{
					session_data->batchsize -= ACCE_DEC_BATCH_STEP;
					//don't let batchsize be less then MIN_BATCH_SIZE
					if (session_data->batchsize < ACCE_MIN_BATCH_SIZE) 
						session_data->batchsize = ACCE_MIN_BATCH_SIZE;
					error("batchsize set to %d \n", session_data->batchsize);
					xio_context_stop_loop(session_data->ctx); //we need it to forse sending queue? - XXX?
				}
			}
			olddiff = diff;
			errortime = time(NULL);
			showerror = 0;
			error("%s diff between msgs and callbacks is: %lu. msgs: %lu, callbacks: %lu \n", 
				ctime(&errortime), diff, session_data->cnt, session_data->rcvd_cb_cnt);
		}
		else
		{	//check diff between messages and callbacks once and few sec
			if (time(NULL) - errortime >= ACCE_CHECK_TRAFFIC_SEC)
				showerror = 1;
		}
	}

	if (msg->out.header.iov_base)
	{
		free(msg->out.header.iov_base);
		msg->out.header.iov_base = NULL;
	}

	//freeing allocated ram for packet
	if (msg->out.data_iov.sglist[0].iov_base)
	{
		debug("freeing allocated ram for packet: %p \n", msg->out.data_iov.sglist[0].iov_base);
		free(msg->out.data_iov.sglist[0].iov_base);
		msg->out.data_iov.sglist[0].iov_base = NULL;
	}

        /* reset message */
        msg->in.header.iov_base = NULL;                                                                                
        msg->in.header.iov_len  = 0;
        msg->in.data_iov.nents  = 0;

        msg->flags = 0;

	//now we can free the message ram
	free(msg);

        return 0;
}
   
//callbacks for accelio client (output)
static struct xio_session_ops ses_ops = {
        .on_session_event               =  on_session_event_client, 
	.on_ow_msg_send_complete        =  on_msg_send_complete_client,
	//.on_ow_msg_send_complete        =  NULL,
        .on_session_established         =  NULL,
        .on_msg                         =  NULL,
        .on_msg_error                   =  NULL
};

//------------------------------ server callbacks ------------------------------

static int on_session_event_server(struct xio_session *session,
                            struct xio_session_event_data *event_data,
                            void *cb_user_context)
{
        struct acce_format_data_t *server_data = (struct acce_format_data_t*)cb_user_context;

        printf("server session event: %s. session:%p, connection:%p, reason: %s\n",
               xio_session_event_str(event_data->event), session, event_data->conn,
               xio_strerror(event_data->reason));

        switch (event_data->event) 
	{
		case XIO_SESSION_NEW_CONNECTION_EVENT:
			server_data->conn = event_data->conn;
			break;
		case XIO_SESSION_CONNECTION_TEARDOWN_EVENT:
			xio_connection_destroy(event_data->conn);
			server_data->conn = NULL;
			break;
		case XIO_SESSION_TEARDOWN_EVENT:
			xio_session_destroy(session);
			xio_context_stop_loop(server_data->ctx);  /* exit */
			break;
		default:
			break;
        };

        return 0;
}

static int on_new_session(struct xio_session *session,
                          struct xio_new_session_req *req,
                          void *cb_user_context)
{
        struct acce_format_data_t *server_data = (struct acce_format_data_t*)cb_user_context;
	req = req;

        /* automatically accept the request */
        printf("new session event. session:%p\n", session);

        if (!server_data->conn)
                xio_accept(session, NULL, 0, NULL, 0);
        else
                xio_reject(session, (enum xio_status)EISCONN, NULL, 0);

        return 0;
}

static void process_request(struct acce_format_data_t *dt, struct xio_msg *req)
{
	debug("%s() - ENTER. sn: %lu \n", __func__, req->sn);

        struct xio_iovec_ex *sglist = vmsg_sglist(&req->in);
        char *str;
	pckt_t *pkt = NULL;
        int nents = vmsg_sglist_nents(&req->in);
        int len, num, i;

	for (i = 0; i < nents; i++)	//it should be always 1, as we set in client part
	{
		debug("process_request: in loop(), nents: %d \n", nents);
		str = (char *)sglist[i].iov_base;
		len = sglist[i].iov_len;
		debug("process_request. str: %p, len: %d\n", str, len);
		if (str) 
		{
			pkt = queue_create_pckt();
			if (!pkt)
				error("failed to allocate RAM for a new packet!\n");
			else
			{
				pkt->len = len;

#ifndef ACCE_SRV_USE_MALLOC
				pkt->ptr = sglist[i].iov_base;				
#else //use malloc() to keep packet
				pkt->ptr = malloc(len);
				if (!pkt->ptr)
					error("failed to allocate RAM for a new msg!\n");
				memcpy(pkt->ptr, sglist[i].iov_base, pkt->len);
#endif
				num = queue_add(pkt);
				if (num > 0)
				{
					dt->pkts_rcvd++;
					debug("packet added to queue. now in queue: %d, pkts_rcvd: %u \n",
						num, dt->pkts_rcvd);
				}
			}	
		}
	}
        req->in.header.iov_base   = NULL;
        req->in.header.iov_len    = 0;
        vmsg_sglist_set_nents(&req->in, 0);

	debug("%s() - EXIT\n", __func__);
}

static int on_request(struct xio_session *session, struct xio_msg *req,
			int last_in_rxq, void *cb_user_context)
{
	debug("on_request()\n");

	session = session;
	last_in_rxq = last_in_rxq; 

        struct acce_format_data_t *server_data = (struct acce_format_data_t*)cb_user_context;

        process_request(server_data, req);

	xio_release_msg(req);

	debug("on_request() - EXIT\n");

        return 0;
}

// asynchronous callbacks for accelio server (input)
static struct xio_session_ops server_ops = {
        .on_session_event               =  on_session_event_server,
        .on_new_session                 =  on_new_session,
        .on_msg_send_complete           =  NULL,
        .on_msg                         =  on_request,
        .on_msg_error                   =  NULL
};

//get env variable ACCELIO_SERVER. if no such - use default value from define	
static char* acce_server()
{
	char *env;
	static char server[SERVER_LEN] = {0};

	env = getenv("ACCELIO_SERVER");
        if (env)
	{
        	debug("ACCELIO_SERVER var is: [%s]\n", env);
		memset(server, 0x0, SERVER_LEN);
		strcpy(server, env);
	}
	else
	{
		memset(server, 0x0, SERVER_LEN);
		strcpy(server, ACCE_SERVER);
		debug("no ACCELIO_SERVER var found. default server will be used\n");
	}

	debug("full server address: %s \n", server);
	return server;
}

static int acce_init_input(libtrace_t *libtrace)
{
	int rv = 0;
	int opt, optlen;
	char url[512] = {0};

	debug("%s() \n", __func__);

	pthread_spin_init(&queue_lock, pshared);

	libtrace->format_data = malloc(sizeof(struct acce_format_data_t));
	memset(libtrace->format_data, 0x0, sizeof(struct acce_format_data_t));
	FORMAT(libtrace)->pvt = 0xFAFAFAFA;
	FORMAT(libtrace)->pkts_read = 0;
	FORMAT(libtrace)->pkts_rcvd = 0;
	FORMAT(libtrace)->per_stream = NULL;

	//init accelio ---------------------------------------------------------
        xio_init();

        /* set, then get max msg size */
        /* this size distinguishes between big and small msgs, where for small msgs 
   	   rdma_post_send/rdma_post_recv
           are called as opposed to to big msgs where rdma_write/rdma_read are called */
	opt = ACCE_MAX_MSG_SIZE;
        xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_INLINE_XIO_DATA, &opt, sizeof(int));
        xio_get_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_INLINE_XIO_DATA, &opt, &optlen);
        FORMAT(libtrace)->max_msg_size = opt;
	printf("max_msg_size : %d\n", FORMAT(libtrace)->max_msg_size);

	opt = ACCE_QUEUE_DEPTH;	
        xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_MSGS, &opt, sizeof(int));
        xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_RCV_QUEUE_DEPTH_MSGS, &opt, sizeof(int));

        xio_get_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_MSGS, &opt, &optlen);
	printf("accelio queue send depth: %d\n", opt);
        xio_get_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_RCV_QUEUE_DEPTH_MSGS, &opt, &optlen);
	printf("accelio queue rcv depth: %d\n", opt);

	/* create thread context for the client */
        FORMAT(libtrace)->ctx = xio_context_create(NULL, 0, -1);

        sprintf(url, "rdma://%s:%s", acce_server(), ACCE_PORT);

	debug("%s() url: %s\n", __func__, url);

	/* bind a listener server to a portal/url */
        FORMAT(libtrace)->server = xio_bind(FORMAT(libtrace)->ctx, &server_ops, url, NULL, 0, libtrace->format_data);

	return rv;
}

//Initialises an output trace using the capture format.
static int acce_init_output(libtrace_out_t *libtrace) 
{
	struct xio_session_params params;
	struct xio_connection_params cparams;
	int queue_depth; 
	int opt, optlen;
	char url[512] = {0};

	debug("%s() \n", __func__);

	memset(&cparams, 0x0, sizeof(struct xio_connection_params));

	libtrace->format_data = malloc(sizeof(struct acce_format_data_out_t));
	memset(libtrace->format_data, 0x0, sizeof(struct acce_format_data_out_t));
	OUTPUT->file = NULL;
	OUTPUT->level = 0;
	OUTPUT->compress_type = TRACE_OPTION_COMPRESSTYPE_NONE;
	OUTPUT->fileflag = O_CREAT | O_WRONLY;
	OUTPUT->cnt = 0;
	OUTPUT->rcvd_cb_cnt = 0;
	OUTPUT->conn_established = 0;
	OUTPUT->batchsize = ACCE_BATCH_SIZE;

	memset(&params, 0, sizeof(params));

	//init accelio ---------------------------------------------------------
        xio_init();

        /* get max msg size */
        /* this size distinguishes between big and small msgs, where for small msgs
	   rdma_post_send/rdma_post_recv are called as opposed to to big msgs where 
	   rdma_write/rdma_read are called */
	opt = ACCE_MAX_MSG_SIZE;
        xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_INLINE_XIO_DATA, &opt, sizeof(int));
        xio_get_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_INLINE_XIO_DATA, &opt, &optlen);
        OUTPUT->max_msg_size = opt;
	printf("max_msg_size : %d\n", OUTPUT->max_msg_size);

	//set depth
	opt = ACCE_QUEUE_DEPTH;	
        xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_MSGS, &opt, sizeof(int));
        xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_RCV_QUEUE_DEPTH_MSGS, &opt, sizeof(int));

        /* get minimal queue depth */
        xio_get_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_MSGS, &opt, &optlen);
	printf("accelio queue snd depth: %d\n", opt);
        queue_depth = ACCE_QUEUE_DEPTH > opt ? opt : ACCE_QUEUE_DEPTH;
	queue_depth = queue_depth; 
	debug("queue_depth: %d\n", queue_depth);
        xio_get_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_RCV_QUEUE_DEPTH_MSGS, &opt, &optlen);
	printf("accelio queue rcv depth: %d\n", opt);

        /* create thread context for the client */
        OUTPUT->ctx = xio_context_create(NULL, 0, -1);

	/* create url to connect to */
        sprintf(url, "rdma://%s:%s", acce_server(), ACCE_PORT);

	debug("%s() url: %s\n", __func__, url);

        params.type             = XIO_SESSION_CLIENT;
        params.ses_ops          = &ses_ops;
        params.user_context     = libtrace->format_data;
        params.uri              = url;

        OUTPUT->session = xio_session_create(&params);

	cparams.session                 = OUTPUT->session;
        cparams.ctx                     = OUTPUT->ctx;                                                            
        cparams.conn_user_context       = libtrace->format_data;
	cparams.out_addr                = NULL;				//prevents segfault on accelio for_next branch
                                                                                                                       
        /* connect the session  */                                                                                     
        OUTPUT->conn = xio_connect(&cparams);

	return 0;
}

static int acce_config_output(libtrace_out_t *libtrace, trace_option_output_t option, void *data)
{
	debug("%s() \n", __func__);

	if (!data)
		return -1;

	switch (option) 
	{
		case TRACE_OPTION_OUTPUT_COMPRESS:
			//OUTPUT->level = *(int *)data;
			return 0;
		case TRACE_OPTION_OUTPUT_COMPRESSTYPE:
			//OUTPUT->compress_type = *(int *)data;
			return 0;
		case TRACE_OPTION_OUTPUT_FILEFLAGS:
			//OUTPUT->fileflag = *(int *)data;
			return 0;
		default:
			trace_set_err_out(libtrace, TRACE_ERR_UNKNOWN_OPTION, "Unknown option");
			return -1;
	}
	assert(0);
}

//we run it in separate thread to avoid blocking issues
static void* input_loop(void *arg)
{
	int rv;
	libtrace_t *libtrace = (libtrace_t*)arg;

        //set higher priority
        rv = nice(-20);
        printf("set priority for input events thread to : %d \n", rv);

	xio_context_run_loop(FORMAT(libtrace)->ctx, XIO_INFINITE);

	return NULL;
}

static int acce_start_input(libtrace_t *libtrace) 
{
	int rv = 0;
	
	debug("%s() - ENTER \n", __func__);

	rv = pthread_create(&FORMAT(libtrace)->thread, NULL, input_loop, libtrace);
	if (rv)
		error("failed to create a thread!\n");
	else
	{
		debug("thread created successfully\n");
	}

	debug("%s() - EXIT\n", __func__);

	return rv;
}

static int kafka_pstart_input(libtrace_t *libtrace) 
{
	int ret = 0;
	libtrace = libtrace;
#if 0
	int i;
	kafka_per_stream_t *stream;
	kafka_per_stream_t empty_stream;
	int num_threads = libtrace->perpkt_thread_count;

	debug("%s() num_threads: %d \n", __func__, libtrace->perpkt_thread_count);

	memset(&empty_stream, 0x0, sizeof(kafka_per_stream_t));

	for (i = 0; i < num_threads; i++)
	{
		//we add all missed structs here per required threads
		if (libtrace_list_get_size(FORMAT(libtrace)->per_stream) <= (size_t) i)
			libtrace_list_push_back(FORMAT(libtrace)->per_stream, &empty_stream);
		//we just get a pointer to our per_stream struct (which is filled with zeroes yet)
		stream = libtrace_list_get_index(FORMAT(libtrace)->per_stream, i)->data;
		stream->id = i;
	}
#endif
	return ret;
}
	
/* Pauses an input trace - this function should close or detach the file or 
   device that is being read from. 
   @return 0 if successful, -1 in the event of error
*/
static int acce_pause_input(libtrace_t * libtrace) 
{
	(void)libtrace;

	debug("%s() \n", __func__);

	debug("fake function. instead of pausing input - do nothing \n");

	return 0;
}

//we run it in separate thread to avoid blocking issues
static void* output_loop(void *arg)
{
	libtrace_out_t *libtrace = (libtrace_out_t*)arg;
	pckt_t *pkt = NULL;
	struct xio_msg *msg = NULL;
	int rv;

	//set higher priority
        rv = nice(-20);
        printf("set priority for output events thread to : %d \n", rv);

	while(1)
	{
		xio_context_run_loop(OUTPUT->ctx, XIO_INFINITE);
		//we get to this line once the stopping thread will call method xio_context_stop_loop()
		//error("before mutex o_queue_num is : %d \n", o_queue_num);
		pthread_mutex_lock(&o_mutex_lock);	//lock used outside loop to send whole queue at once

		if (o_queue_num > OUTPUT->batchsize + 1000)
		{
			error("o_queue_num is big: %d \n", o_queue_num);
		}
		while (o_queue_num)
		{
			pkt = o_queue_de();
			if (pkt)
			{
				if (xio_send_msg(OUTPUT->conn, pkt->ptr) == -1) 
				{
					if (xio_errno() != EAGAIN)
					{
						error("[%p] Error - xio_send_msg failed. %s\n",
							OUTPUT->session, xio_strerror(xio_errno()));
						break;
					}
					else
					{
						error("will try to send msg again!\n");
					}
				}
				else
				{
					msg = (struct xio_msg*)pkt->ptr; msg = msg;
					debug("packet with sn: %lu sent successfully\n", msg->sn);
					free(pkt);
				}
			}
		}
		pthread_mutex_unlock(&o_mutex_lock);
	}

	return NULL;
}

static int acce_start_output(libtrace_out_t *libtrace) 
{
	int rv = 0; 

	debug("%s() \n", __func__);

	rv = pthread_create(&OUTPUT->thread, NULL, output_loop, libtrace);
	if (rv)
		error("failed to create a thread!\n");
	else
	{
		debug("thread created successfully\n");
	}

#ifdef OPTION_OUTPUT_FILE
	//wandio_wcreate() called inside
	OUTPUT->file = trace_open_file_out(libtrace, OUTPUT->compress_type, OUTPUT->level, OUTPUT->fileflag);
	if (!OUTPUT->file)
	{
		error("can't open out file with wandio\n");
		return -1;
	}
	else
	{
		debug("opened out file with wandio successfully\n");
	}
#endif
	return rv;
}

static int acce_fin_input(libtrace_t *libtrace) 
{
	debug("%s() \n", __func__);

	debug("%s() freeing accelio resources\n", __func__);
	xio_unbind(FORMAT(libtrace)->server);
        xio_context_destroy(FORMAT(libtrace)->ctx);
        xio_shutdown();

	if (libtrace->io)
	{
		wandio_destroy(libtrace->io);
		debug("wandio destroyed\n");
	}

	if (FORMAT(libtrace)->per_stream)
		libtrace_list_deinit(FORMAT(libtrace)->per_stream);

	if (libtrace->format_data)
	{
		free(libtrace->format_data);
		libtrace->format_data = NULL;
	}

	debug("%s() exiting\n", __func__);

	return 0;
}

static int acce_fin_output(libtrace_out_t *libtrace) 
{
	debug("%s() \n", __func__);

	printf("%s() output is over. disconnect in 5 secs\n", __func__);
	xio_context_stop_loop(OUTPUT->ctx);
        sleep(5);

	error("packets sent: %lu, callbacks received: %lu, diff: %lu \n", 
		OUTPUT->cnt, OUTPUT->rcvd_cb_cnt, OUTPUT->cnt - OUTPUT->rcvd_cb_cnt);

	xio_disconnect(OUTPUT->conn);

	//wait till we get events: connection teardown, session teardown. 
	while (OUTPUT->conn_established)
	{
		sleep(1);
	}

	//we do connection/session destroy in callbacks on according events, not here

	debug("%s() dstr context\n", __func__);
        xio_context_destroy(OUTPUT->ctx);
	debug("%s() dstr xio\n", __func__);
        xio_shutdown();

#ifdef OPTION_OUTPUT_FILE
	debug("%s() dstr wandio\n", __func__);
	wandio_wdestroy(OUTPUT->file);
#endif

	//free(libtrace->format_data);
	free(OUTPUT);
	return 0;
}

//Converts a buffer containing a packet record into a libtrace packet
static int lodp_prepare_packet(libtrace_t *libtrace UNUSED, libtrace_packet_t *packet,
		void *buffer, libtrace_rt_types_t rt_type, uint32_t flags) 
{
	debug("%s() \n", __func__);

	if (packet->buffer != buffer && packet->buf_control == TRACE_CTRL_PACKET)
                free(packet->buffer);

        if ((flags & TRACE_PREP_OWN_BUFFER) == TRACE_PREP_OWN_BUFFER)
                packet->buf_control = TRACE_CTRL_PACKET;
        else
                packet->buf_control = TRACE_CTRL_EXTERNAL;

        packet->buffer = buffer;
        packet->header = buffer;
	packet->type = rt_type;

	return 0;
}

/* Reads the next packet from an input trace into the provided packet 
 * structure.
 *
 * @param libtrace      The input trace to read from
 * @param packet        The libtrace packet to read into
 * @return The size of the packet read (in bytes) including the capture
 * framing header, or -1 if an error occurs. 0 is returned in the
 * event of an EOF. 
 *
 * IF NO PACKETS ARE AVAILABLE FOR READING, THIS FUNCTION SHOULD BLOCK
 * until one appears or return 0 if the end of a trace file has been
 * reached.
 */

//So endless loop while no packets and return bytes read in case there is a packet (no one checks returned bytes)
static int acce_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) 
{
	uint32_t flags = 0;
	int numbytes = 0;
	pckt_t *pkt = NULL;
	
	debug("%s() \n", __func__);

	//#0. Free the last packet buffer
	if (packet->buffer) 
	{
		//Check buffer memory is owned by the packet. It is if flag is TRACE_CTRL_PACKET
		assert(packet->buf_control == TRACE_CTRL_PACKET); 
		free(packet->buffer);
		packet->buffer = NULL;
	}

	//#1. Set packet fields
	//TRACE_CTRL_EXTERNAL means buffer memory is owned by an external source, this is it, odp pool.
	packet->buf_control = TRACE_CTRL_EXTERNAL;
	packet->type = TRACE_RT_DATA_ODP;

	//#2. Read a packet from input. We wait here forever till packet appears.
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
	}

	if (numbytes == -1) 
	{
		trace_set_err(libtrace, errno, "Reading packet failed");
		return -1;
	}
	else if (numbytes == 0)
		return 0;

	//get pointer from packet and assign it to packet->buffer
	if (!packet->buffer || packet->buf_control == TRACE_CTRL_EXTERNAL) 
	{
		packet->buffer = pkt->ptr;
		packet->capture_length = pkt->len;
		packet->payload = packet->buffer;
		packet->wire_length = pkt->len + WIRELEN_DROPLEN;
		//-----
		debug("pointer to packet: %p \n", packet->buffer);
                if (!packet->buffer) 
		{
                        trace_set_err(libtrace, errno, "Cannot allocate memory or have invalid pointer to packet");
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

static void acce_fin_packet(libtrace_packet_t *packet)
{
        debug("%s() \n", __func__);

        if (packet->buf_control == TRACE_CTRL_EXTERNAL)
        {
                if (packet->buffer)
                {
#ifdef ACCE_SRV_USE_MALLOC
                        debug("%s(): freeing buffer allocated for packet \n", __func__);
                        free(packet->buffer);
#endif
                        packet->buffer = NULL;
                }
        }
}

//adding packet to a queue here
static int acce_write_packet(libtrace_out_t *libtrace, libtrace_packet_t *packet)
{
	debug("%s() packet: %p , total packets: %lu \n", __func__, packet, OUTPUT->cnt+1);

	int i = 0;
	int numbytes = 0;
	int num;
	struct xio_reg_mem xbuf;
	uint8_t *data = NULL;
	struct xio_msg *msg = NULL;
	pckt_t *pkt = NULL;
	void *p;
	size_t len = trace_get_capture_length(packet);

	//MSG SETUP
	msg = malloc(sizeof(struct xio_msg));
	if (!msg)
		{ error("failed to allocate RAM\n"); return -1; }
	memset(msg, 0x0, sizeof(struct xio_msg));
	msg->type = XIO_MSG_TYPE_ONE_WAY;
	msg->flags = 0x0;

	//set msg->in just to be sure, as we set msg->out with real data
	msg->in.header.iov_base = NULL;
	msg->in.header.iov_len  = 0;
	vmsg_sglist_set_nents(&msg->in, 0);
	msg->in.sgl_type = XIO_SGL_TYPE_IOV;
	msg->in.data_iov.max_nents = XIO_IOVLEN;

	//set msg->out
	msg->out.sgl_type = XIO_SGL_TYPE_IOV;
	msg->out.data_iov.max_nents = XIO_IOVLEN;

	//header
	msg->out.header.iov_base = strdup("m");
	msg->out.header.iov_len = strlen((const char *)msg->out.header.iov_base) + 1;

	/* data */
	if ((int)len <= OUTPUT->max_msg_size) 
	{
		p = malloc(len);
		if (!p) 
			{ error("failed to allocate RAM\n"); return -1; }
		memcpy(p, packet->payload, len);
		msg->out.data_iov.sglist[0].iov_base = p;
#ifdef OPTION_PRINT_PACKETS
		hexdump(msg->out.data_iov.sglist[0].iov_base, 16);
#endif
	}
	else 
	{
		error("packet > %d bytes. trying to allocate big message!!!\n", OUTPUT->max_msg_size);
	 	/* big msgs */
		if (data == NULL) 
		{
			debug("allocating xio memory...\n");
			xio_mem_alloc(len, &xbuf);
			data = (uint8_t *)xbuf.addr;
			memset(data, 0x0, len);
			memcpy(data, packet->payload, len);
		}
		msg->out.data_iov.sglist[0].mr = xbuf.mr;
		msg->out.data_iov.sglist[0].iov_base = data;
	}

	msg->out.data_iov.sglist[0].iov_len = len;
	msg->out.data_iov.nents = 1;
	
	//sleep here till we have event, that connection established
	while (!OUTPUT->conn_established)
	{
		usleep(10000);
		//show debug for a first time, and then every second
		if (!(i++ % 100))
		{
			debug("waiting for connection\n");
		}
	}

	//we should let sending thread have some time for receiving callbacks
	//so we sleep here and don't call stop_loop() and don't increase queue, etc
	while (o_queue_num >= OUTPUT->batchsize)
	{
		usleep(10000);
	}

	//adding to queue
	pkt = queue_create_pckt();
	if (!pkt)
		error("failed to allocate RAM for a new packet!\n");
	else
	{
		pkt->len = len;
		pkt->ptr = (void*)msg;
		num = o_queue_add(pkt); num = num;
		OUTPUT->cnt++;
		numbytes = len;
		debug("packet added to output queue. now in queue: %d, pkts went to sending: %lu \n",
			num, OUTPUT->cnt);
	}

	if (o_queue_num >= OUTPUT->batchsize)
	{
		//error("stop loop. cnt: %d , o_queue_num: %d \n", OUTPUT->cnt, o_queue_num);
		xio_context_stop_loop(OUTPUT->ctx);
	}

#ifdef OPTION_OUTPUT_FILE
	assert(OUTPUT->file);

	//seems like we are writing just raw packet in file
	if ((numbytes = wandio_wwrite(OUTPUT->file, packet->payload, trace_get_capture_length(packet))) !=
				(int)trace_get_capture_length(packet)) 
	{
		trace_set_err_out(libtrace, errno, "Writing packet failed");
		return -1;
	}
#endif
	return numbytes;
}

/** Returns the next libtrace event for the input trace.
 *
 * @param trace		The input trace to get the next event from
 * @param packet	A libtrace packet to read a packet into
 * @return A libtrace event describing the event that occured
 *
 * The event API allows for non-blocking reading of packets from an
 * input trace. If a packet is available and ready to be read, a packet
 * event should be returned. Otherwise a sleep or fd event should be
 * returned to indicate that the caller needs to wait. If the input
 * trace has an error or reaches EOF, a terminate event should be
 * returned.
 */
static struct libtrace_eventobj_t acce_trace_event(libtrace_t *trace, libtrace_packet_t *packet)
{
	struct libtrace_eventobj_t event;
	int len = 0;
	trace = trace;
	packet = packet;

	debug("%s() \n", __func__);
	
	memset(&event, 0x0, sizeof(struct libtrace_eventobj_t));

	//XXX - get len of packet. then copy packet into *packet
//	len = 

	event.type = TRACE_EVENT_PACKET;
	event.fd = -1; //XXX - should it be -1?
	event.seconds = 0.0f;
	event.size = len;
	
	return event;
}


//Returns the payload length of the captured packet record
//We use the value we got from odp and stored in FORMAT(libtrace)->pkt_len
static int acce_get_capture_length(const libtrace_packet_t *packet)
{
	int pkt_len;

	debug("%s() called! \n", __func__);

	if (packet)
	{
		// this won't work probably, as we don't set packet->length anywhere, so can't return it.
		//pkt_len = (int)trace_get_capture_length(packet);
		//pkt_len = FORMAT(libtrace)->pkt_len;
		pkt_len = packet->capture_length;
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

static int acce_get_framing_length(const libtrace_packet_t *packet) 
{
	debug("%s() called! \n", __func__);

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
static int acce_get_wire_length(const libtrace_packet_t *packet) 
{
	debug("acce_get_framing_length() called! \n");

	if (packet)
		//return trace_get_wire_length(packet);
		return packet->wire_length;
	else
	{
		trace_set_err(packet->trace,TRACE_ERR_BAD_PACKET, "Have no packet");
		return -1;
	}
}

static libtrace_linktype_t acce_get_link_type(const libtrace_packet_t *packet UNUSED) 
{
	debug("%s() \n", __func__);

	return TRACE_TYPE_ETH;	//We have Ethernet for ODP and in DPDK.
}

//returns timestamp from a packet or time now (as hack)
static double acce_get_seconds(const libtrace_packet_t *packet)
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
static struct timeval acce_get_timeval(const libtrace_packet_t *packet)
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

#if 0
static uint64_t lodp_get_erf_timestamp(const libtrace_packet_t *packet UNUSED)
{
	uint64_t rv = 0;

/*
	debug("packet header: %p, seconds: %zu , microseconds: %zu \n",
		packet->header, tv.tv_sec, tv.tv_usec);
*/
	return rv;
}
#endif

//libtrace creates threads with pthread_create(), then fills libtrace_thread_t struct and passes ptr to it here (*t)
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
			t->format_data = libtrace_list_get_index(FORMAT(libtrace)->per_stream, t->perpkt_num)->data;
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

static void lodp_help(void)
{
	printf("Endace ODP format module\n");
	printf("Supported input uris:\n");
	printf("\todp:/path/to/input/file\n");
	printf("Supported output uris:\n");
	printf("\todp:/path/to/output/file\n");
	printf("\n");
	return;
}

/* A libtrace capture format module */
/* All functions should return -1, or NULL on failure */
static struct libtrace_format_t acce = {
        "acce",				/* name used in URI to identify capture format - odp:iface */
        "$Id$",				/* version of this module */
        TRACE_FORMAT_ACCE,		/* The RT protocol type of this module */
	NULL,				/* probe filename - guess capture format - NOT NEEDED*/
	NULL,				/* probe magic - NOT NEEDED*/
        acce_init_input,	        /* init_input - Initialises an input trace using the capture format */
        NULL,                           /* config_input - Sets value to some option */
        acce_start_input,	        /* start_input-Starts or unpause an input trace */
        acce_pause_input,               /* pause_input */
        acce_init_output,               /* init_output - Initialises an output trace using the capture format. */
        acce_config_output,             /* config_output */
        acce_start_output,              /* start_output */
        acce_fin_input,	         	/* fin_input - Stops capture input data.*/
        acce_fin_output,                /* fin_output */
        acce_read_packet,        	/* read_packet - Reads next packet from input trace into the packet */
        lodp_prepare_packet,		/* prepare_packet - Converts a buffer with packet into a libtrace packet */
	acce_fin_packet,                /* fin_packet - Frees any resources allocated for a libtrace packet */
        acce_write_packet,              /* write_packet - Write a libtrace packet to an output trace */
        acce_get_link_type,    		/* get_link_type - Returns the libtrace link type for a packet */
        NULL,              		/* get_direction */
        NULL,              		/* set_direction */
	NULL,				/* get_erf_timestamp */
/*	lodp_get_erf_timestamp,         */
        acce_get_timeval,               /* get_timeval */
	NULL,				/* get_timespec */
        acce_get_seconds,               /* get_seconds */
        NULL,                   	/* seek_erf */
        NULL,                           /* seek_timeval */
        NULL,                           /* seek_seconds */
        acce_get_capture_length,  	/* get_capture_length */
        acce_get_wire_length,  		/* get_wire_length */
        acce_get_framing_length, 	/* get_framing_length */
        NULL,         			/* set_capture_length */
	NULL,				/* get_received_packets */
	NULL,				/* get_filtered_packets */
	NULL,				/* get_dropped_packets */
	NULL,				/* get_statistics */
        NULL,                           /* get_fd */
        acce_trace_event,              	/* trace_event */
        lodp_help,                     	/* help */
        NULL,                           /* next pointer */
	{true, 8},                      /* Live, NICs typically have 8 threads */
	kafka_pstart_input,              /* pstart_input */
	NULL,             		/* pread_packets */
	acce_pause_input,               /* ppause */
	acce_fin_input,                 /* p_fin */
	lodp_pregister_thread,          /* pregister_thread */
	lodp_punregister_thread,        /* punregister_thread */
	NULL				/* get thread stats */ 
};

void acce_constructor(void) 
{
	printf("accelio module version: %s\n", ACCE_VERSION);
	debug("registering acce struct with address: %p , init_output: %p\n", &acce, acce.init_output);
	register_format(&acce);
}
