#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
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

//acce config
#define ACCE_QUEUE_DEPTH	512
#define ACCE_SERVER 		"localhost"
#define ACCE_PORT 		"9992"		//XXX - change it
#define SERVER_LEN 512

//old stuff
#define TOPIC_LEN 512
#define HOSTNAME_LEN 256
#define BROKER_LEN 512
#define GROUP_LEN 512
#define ERR_LEN 512

//----- CONFIG -----


//----- OPTIONS -----
//#define MULTI_INPUT_QUEUES
#define DEBUG
#define ERROR_DBG
#define OPTION_PRINT_PACKETS

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


#if 0
struct kafka_format_data_t 
{
	//kafka vars
	rd_kafka_t *rk;
        rd_kafka_conf_t *conf;                  //main conf object
        rd_kafka_topic_t *rkt;                  //topic object
        rd_kafka_topic_conf_t *topic_conf;      //topic configuration obj
	rd_kafka_topic_partition_list_t *topics;//list of topics to subscribe as consumer
        int partition;
        char topic[TOPIC_LEN];			//our specific topic name
        char brokers[BROKER_LEN];
        char errstr[ERR_LEN];
	//other vars
	void *pkt;				//store received packet here
	int pkt_len;				//length of current packet
	int pvt;				//for private data saving
	unsigned int pkts_read;
	u_char *l2h;				//l2 header for current packet
	libtrace_list_t *per_stream;		//pointer to the whole list structure: head, tail, size etc inside.
};
#endif


struct acce_format_data_t
{
	//accelio vars
	struct xio_server       *server;        /* server portal */
	struct xio_context      *ctx;
        struct xio_connection   *conn;
        int max_msg_size;

	//other vars
	void *pkt;				//store received packet here
	int pkt_len;				//length of current packet
	u_char got_pkt;				//set to 1 if we have packet
	int pvt;				//for private data saving
	unsigned int pkts_read;
	u_char *l2h;				//l2 header for current packet
	libtrace_list_t *per_stream;		//pointer to the whole list structure: head, tail, size etc inside.
	pthread_t thread;
};

struct acce_format_data_out_t 
{
	//accelio vars
	struct xio_context *ctx;
        struct xio_connection *conn;
	struct xio_msg req_ring;		//could be a buffer, but let's store a single msg here
        uint64_t cnt;
        int max_msg_size;
	
	//other vars
	char *path;
	int level;
	int compress_type;			//store compression type here: bz2, gz etc
	int fileflag;
	iow_t *file;
};

#if 1
typedef struct kafka_per_stream_s 
{
	int id;
	int core;
	void *pkt;				//store received packet here
	int pkt_len;
	u_char *l2h;
	unsigned int pkts_read;
} kafka_per_stream_t;
#endif


static int on_session_event_client(struct xio_session *session,
                            struct xio_session_event_data *event_data,
                            void *cb_user_context)
{
        struct acce_format_data_out_t *session_data = (struct acce_format_data_out_t*)
                                                cb_user_context;

        printf("session event: %s. reason: %s\n",
               xio_session_event_str(event_data->event),
               xio_strerror(event_data->reason));

        switch (event_data->event) 
	{
        case XIO_SESSION_CONNECTION_TEARDOWN_EVENT:
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

//callbacks for accelio client (output)
static struct xio_session_ops ses_ops = {
        .on_session_event               =  on_session_event_client, 
        .on_session_established         =  NULL,
        .on_msg                         =  NULL,		//XXX - add callback
        .on_msg_error                   =  NULL
};

//------------------------------ server callbacks ------------------------------

static int on_session_event_server(struct xio_session *session,
                            struct xio_session_event_data *event_data,
                            void *cb_user_context)
{
        struct acce_format_data_t *server_data = (struct acce_format_data_t*)cb_user_context;

        printf("session event: %s. session:%p, connection:%p, reason: %s\n",
               xio_session_event_str(event_data->event),
               session, event_data->conn,
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
	debug("%s() - ENTER \n", __func__);

        struct xio_iovec_ex     *sglist = vmsg_sglist(&req->in);
        char                    *str;
        int                     nents = vmsg_sglist_nents(&req->in);
        int                     len, i;
        //char                    tmp;

        /* note all data is packed together so in order to print each
 *          * part on its own NULL character is temporarily stuffed
 *                   * before the print and the original character is restored after
 *                            * the printf
 *                                     */

//we don't need header yet
#if 0
        if (++server_data->cnt == PRINT_COUNTER) {
                str = (char *)req->in.header.iov_base;
                len = req->in.header.iov_len;
                if (str) {
                        if (((unsigned)len) > 64)
                                len = 64;
                        tmp = str[len];
                        str[len] = '\0';
                        printf("message header : [%llu] - %s\n",
                               (unsigned long long)(req->sn + 1), str);
                        str[len] = tmp;
                }
	}
#endif
	for (i = 0; i < nents; i++)	//it should be always 1, as we set in client part
	{
		debug("process_request: in loop(), nents: %d \n", nents);
		str = (char *)sglist[i].iov_base;
		len = sglist[i].iov_len;
		debug("process_request. str: %p, len: %d\n", str, len);
		if (str) 
		{	//copy ptr and len
			dt->pkt = malloc(len);
			memcpy(dt->pkt, str, len);
			dt->pkt_len = len;
			dt->got_pkt = 1;	//we signal that we have packet
#if 0
			if (((unsigned)len) > 64)
				len = 64;
			tmp = str[len];
			str[len] = '\0';
			printf("message data: [%llu][%d][%d] - %s\n",
			       (unsigned long long)(req->sn + 1),
			       i, len, str);
			str[len] = tmp;
#endif
		}
	}
        //server_data->cnt = 0;
        req->in.header.iov_base   = NULL;
        req->in.header.iov_len    = 0;
        vmsg_sglist_set_nents(&req->in, 0);

	debug("%s() - EXIT\n", __func__);
}

static int on_request(struct xio_session *session,
                      struct xio_msg *req,
                      int last_in_rxq,
                      void *cb_user_context)
{
	debug("on_request()\n");

        struct acce_format_data_t *server_data = (struct acce_format_data_t*)cb_user_context;
        //struct xio_msg     *rsp = ring_get_next_msg(server_data);

        /* process request */
        process_request(server_data, req);

//XXX - are we going to send responses back or not?
        /* attach request to response */
#if 0
        rsp->request = req;
        xio_send_response(rsp);		
        server_data->nsent++;
#endif

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

//get hostname
#if 0
static char* kafka_hostname()
{
	int rv;
	static int done = 0;
	char hname[HOSTNAME_LEN] = {0};
	static char topic[TOPIC_LEN] = "capture.";
	const char *h = "nohostname";

	//executing just once
	if (!done)
	{
		done = 1;
		rv = gethostname(hname, HOSTNAME_LEN);
		if (rv)
		{
			error("error getting hostname\n");
			strcpy(hname, h);	//if we failed to get hostname - return default one
		}
		else
		{
			debug("got hostname successfully: %s \n", hname);
		}
		strcat(topic, hname);
		debug("full topicname: %s \n", topic);
	}

	return topic;
}
#endif

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
	char url[256] = {0};

	debug("%s() \n", __func__);

	kafka_per_stream_t stream;
	memset(&stream, 0x0, sizeof(kafka_per_stream_t));

	//init all the data in kafka_format_data_t
	libtrace->format_data = malloc(sizeof(struct acce_format_data_t));
	FORMAT(libtrace)->pvt = 0xFAFAFAFA;
	FORMAT(libtrace)->pkts_read = 0;

        /* initialize library */
	//init accelio ---------------------------------------------------------
        xio_init();

        /* get max msg size */
        /* this size distinguishes between big and small msgs, where for small msgs 
   	   rdma_post_send/rdma_post_recv
           are called as opposed to to big msgs where rdma_write/rdma_read are called */
        xio_get_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_INLINE_XIO_DATA, &opt, &optlen);
        FORMAT(libtrace)->max_msg_size = opt;

	/* create thread context for the client */
        FORMAT(libtrace)->ctx = xio_context_create(NULL, 0, -1);

        sprintf(url, "rdma://%s:%s", acce_server(), ACCE_PORT);

	debug("%s() url: %s\n", __func__, url);

	/* bind a listener server to a portal/url */
        FORMAT(libtrace)->server = xio_bind(FORMAT(libtrace)->ctx, &server_ops, url, NULL, 0, libtrace->format_data);

	return rv;
}

/* Initialises an input trace using the capture format. 
   @param libtrace 	The input trace to be initialised */
#if 0
static int kafka_init_input(libtrace_t *libtrace) 
{
	int rv;

	debug("%s() \n", __func__);

	kafka_per_stream_t stream;
	memset(&stream, 0x0, sizeof(kafka_per_stream_t));

	//init all the data in kafka_format_data_t
	libtrace->format_data = malloc(sizeof(struct kafka_format_data_t));
	FORMAT(libtrace)->pvt = 0xFAFAFAFA;
	FORMAT(libtrace)->pkts_read = 0;

	/* Make our first stream */
	FORMAT(libtrace)->per_stream = libtrace_list_init(sizeof(kafka_per_stream_t));
	libtrace_list_push_back(FORMAT(libtrace)->per_stream, &stream);//copies inside, so its ok to alloc on stack.

	//init kafka
		FORMAT(libtrace)->partition = 0;
	//strcpy(FORMAT(libtrace)->topic, KAFKA_TOPIC);
	strcpy(FORMAT(libtrace)->topic, kafka_hostname());

	strcpy(FORMAT(libtrace)->brokers, kafka_broker());
	memset(FORMAT(libtrace)->errstr, 0x0, sizeof(FORMAT(libtrace)->errstr));

	FORMAT(libtrace)->conf = rd_kafka_conf_new();
        rd_kafka_conf_set(FORMAT(libtrace)->conf, "compression.codec", KAFKA_COMPRESSION,
		FORMAT(libtrace)->errstr, sizeof(FORMAT(libtrace)->errstr));
        //the min numb of messages to wait for to accumulate before sending
        rd_kafka_conf_set(FORMAT(libtrace)->conf, "batch.num.messages", KAFKA_BATCH_MSGS,
		FORMAT(libtrace)->errstr, sizeof(FORMAT(libtrace)->errstr));
        rd_kafka_conf_set(FORMAT(libtrace)->conf, "queue.buffering.max.ms", KAFKA_BUFFERING_MS,
		FORMAT(libtrace)->errstr, sizeof(FORMAT(libtrace)->errstr));

        //topic configuration
        FORMAT(libtrace)->topic_conf = rd_kafka_topic_conf_new();

	//------------------------------ below is setup similar to librdkafka example --------------------
	/* Consumer groups require a group id */
	debug("setting group\n");
	if (rd_kafka_conf_set(FORMAT(libtrace)->conf, "group.id", kafka_group(), FORMAT(libtrace)->errstr,
		sizeof(FORMAT(libtrace)->errstr)) != RD_KAFKA_CONF_OK)
	{
		fprintf(stderr, "%% %s\n", FORMAT(libtrace)->errstr);
		exit(1);
	}

	/* Consumer groups always use broker based offset storage */
	if (rd_kafka_topic_conf_set(FORMAT(libtrace)->topic_conf, "offset.store.method", "broker",
		FORMAT(libtrace)->errstr, sizeof(FORMAT(libtrace)->errstr)) != RD_KAFKA_CONF_OK) 
	{
		fprintf(stderr, "%% %s\n", FORMAT(libtrace)->errstr);
		exit(1);
	}

	/* Set default topic config for pattern-matched topics. */
	rd_kafka_conf_set_default_topic_conf(FORMAT(libtrace)->conf, FORMAT(libtrace)->topic_conf);

	//-------------------------------------------------------------------------------------------------

        //----- create kafka handle -----
        FORMAT(libtrace)->rk = rd_kafka_new(RD_KAFKA_CONSUMER, FORMAT(libtrace)->conf, 
		FORMAT(libtrace)->errstr, sizeof(FORMAT(libtrace)->errstr));
        if (!FORMAT(libtrace)->rk)
        {
                fprintf(stderr, "%% Failed to create new producer: %s\n", FORMAT(libtrace)->errstr);
                exit(1);
        }

        rd_kafka_set_log_level(FORMAT(libtrace)->rk, LOG_DEBUG);

        //add brokers
        if (!rd_kafka_brokers_add(FORMAT(libtrace)->rk, FORMAT(libtrace)->brokers))
        {
                fprintf(stderr, "%% No valid brokers specified\n");
                exit(1);
        }



        /* Redirect rd_kafka_poll() to consumer_poll() */
	rd_kafka_poll_set_consumer(FORMAT(libtrace)->rk);

	rv = kafka_init_consume(libtrace);
	if (!rv)
	{
		debug("no topics found in env variables - will use default one \n");
	}
	else
	{
		debug("found and subscribed to %d topics \n", rv);
	}

	return 0;
}
#endif

//Initialises an output trace using the capture format.
static int acce_init_output(libtrace_out_t *libtrace) 
{
	struct xio_session *session;
	struct xio_session_params params;
	struct xio_connection_params cparams;
	int queue_depth; 
	int opt, optlen;
	char url[256] = {0};

	debug("%s() \n", __func__);

	libtrace->format_data = malloc(sizeof(struct acce_format_data_out_t));
	OUTPUT->file = NULL;
	OUTPUT->level = 0;
	OUTPUT->compress_type = TRACE_OPTION_COMPRESSTYPE_NONE;
	OUTPUT->fileflag = O_CREAT | O_WRONLY;
	memset(&OUTPUT->req_ring, 0x0, sizeof(struct xio_msg));

	memset(&params, 0, sizeof(params));
	//init accelio ---------------------------------------------------------
	/* initialize library */
        xio_init();
        /* get minimal queue depth */
        xio_get_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_MSGS, &opt, &optlen);
        queue_depth = ACCE_QUEUE_DEPTH > opt ? opt : ACCE_QUEUE_DEPTH;
	debug("queue_depth: %d\n", queue_depth);

        /* get max msg size */
        /* this size distinguishes between big and small msgs, where for small msgs
	   rdma_post_send/rdma_post_recv are called as opposed to to big msgs where 
	   rdma_write/rdma_read are called */
        xio_get_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_INLINE_XIO_DATA, &opt, &optlen);
        OUTPUT->max_msg_size = opt;
	debug("max_msg_size : %d\n", OUTPUT->max_msg_size);

        /* create thread context for the client */
        OUTPUT->ctx = xio_context_create(NULL, 0, -1);

	/* create url to connect to */
        sprintf(url, "rdma://%s:%s", acce_server(), ACCE_PORT);

	debug("%s() url: %s\n", __func__, url);

        params.type             = XIO_SESSION_CLIENT;
        params.ses_ops          = &ses_ops;
        params.user_context     = libtrace->format_data;	//XXX - check it later(was &session_data)
        params.uri              = url;

        session = xio_session_create(&params);

	cparams.session                 = session;                                                                     
        cparams.ctx                     = OUTPUT->ctx;                                                            
        cparams.conn_user_context       = libtrace->format_data;	//XXX - check it later(was &session_data)
                                                                                                                       
        /* connect the session  */                                                                                     
        OUTPUT->conn = xio_connect(&cparams);

	//end accelio ----------------------------------------------------------

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



//we run it in separate thread to not have blocking issues
void* input_loop(void *arg)
{
	libtrace_t *libtrace = (libtrace_t*)arg;

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

static int acce_start_output(libtrace_out_t *libtrace) 
{
	debug("%s() \n", __func__);

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
	return 0;
}

static int acce_fin_input(libtrace_t *libtrace) 
{
	debug("%s() \n", __func__);

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

	xio_unbind(FORMAT(libtrace)->server);
        xio_context_destroy(FORMAT(libtrace)->ctx);
        xio_shutdown();


	debug("%s() exiting\n", __func__);

	return 0;
}

static int acce_fin_output(libtrace_out_t *libtrace) 
{
	debug("%s() \n", __func__);

	/* free the context */
        xio_context_destroy(OUTPUT->ctx);
        xio_shutdown();

	wandio_wdestroy(OUTPUT->file);

	free(libtrace->format_data);
	return 0;
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

/* internal function (not a registered format routine).
 * with ODP_SCHED_NO_WAIT we always skip to a next iteration with 'continue'
 * but anyway we have a forever loop here till get a new packet */


//in callback process packet and save it into our struct. set flag we got packet
static int acce_read_pack(libtrace_t *libtrace)
{
	int numbytes;

	debug("%s() \n", __func__);

	while (1) 
	{	//XXX - moved to start_input()
		//xio_context_run_loop(FORMAT(libtrace)->ctx, 100); //XXX - make it shorter, 10ms?

		//if we got Ctrl-C from one of our utilities, etc
		if (libtrace_halt)
		{
			printf("[got halt]\n");
			return READ_EOF;
		}

		if (FORMAT(libtrace)->got_pkt)
		{
			FORMAT(libtrace)->got_pkt = 0;
			numbytes = FORMAT(libtrace)->pkt_len;
			debug("have packet with len %d \n", numbytes);
			return numbytes;
		}

		usleep(10000);	//10ms
	}

#if 0
	while (1) 
	{
		rd_kafka_message_t *rkmessage;
		//rd_kafka_resp_err_t err;
//old API style of poll
#if 0
		/* Poll for errors, etc. */
		rd_kafka_poll(FORMAT(libtrace)->rk, 0);

		/* Consume single message. See rdkafka_performance.c for high speed */
		rkmessage = rd_kafka_consume(FORMAT(libtrace)->rkt, FORMAT(libtrace)->partition, 1000);
#endif
		//HighLevel API for poll. Will block for at most 1000 ms
                rkmessage = rd_kafka_consumer_poll(FORMAT(libtrace)->rk, 1000);

		if (!rkmessage) /* timeout */
			continue;

		//copy received packet to internally allocated ram
		FORMAT(libtrace)->pkt = malloc(rkmessage->len);
		memcpy(FORMAT(libtrace)->pkt, rkmessage->payload, rkmessage->len);
		FORMAT(libtrace)->pkt_len = rkmessage->len;
		
		numbytes = rkmessage->len;
		debug("msg received from topic [%s] with len: %d \n", 
			rd_kafka_topic_name(rkmessage->rkt) ,numbytes);

		msg_consume(rkmessage, NULL);

		/* Return message to rdkafka */
		rd_kafka_message_destroy(rkmessage);

		if (rkmessage->len == 0)
			continue;
#if 0
                /* Use schedule to get buf from any input queue. 
		   Waits infinitely for a new event with ODP_SCHED_WAIT param. */
		//debug("%s() - waiting for packet!\n", __func__);
                ev = odp_schedule(NULL, ODP_SCHED_NO_WAIT); //no wait here
#endif

		//if we got Ctrl-C from one of our utilities, etc
		if (libtrace_halt)
		{
			printf("[got halt]\n");
			return READ_EOF;
		}

#if 0
                FORMAT(libtrace)->pkt = odp_packet_from_event(ev);
                if (!odp_packet_is_valid(FORMAT(libtrace)->pkt))
		{
        		//debug("%s() - packet is INVALID, skipping, or NO PACKET\n", __func__);
                        continue;
		}
		else
		{
#ifdef OPTION_PRINT_PACKETS
        		fprintf(stdout, "%s() - packet is valid, print:\n", __func__);
        		fprintf(stdout, "--------------------------------------------------\n");
			odp_packet_print(FORMAT(libtrace)->pkt);
        		fprintf(stdout, "--------------------------------------------------\n");
#endif
		}

                //Returns pointer to the start of the layer 2 header
                FORMAT(libtrace)->l2h = (u_char *)odp_packet_l2_ptr(FORMAT(libtrace)->pkt, NULL);
                FORMAT(libtrace)->pkt_len = (int)odp_packet_len(FORMAT(libtrace)->pkt);
                numbytes = FORMAT(libtrace)->pkt_len;
		FORMAT(libtrace)->pkts_read++;
	
		debug("packet is %d bytes, total packets: %u\n", numbytes, FORMAT(libtrace)->pkts_read);
#endif


		return numbytes;
	}

#endif
	/* We'll NEVER get here */
	return READ_ERROR;
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
	numbytes = acce_read_pack(libtrace);
	if (numbytes == -1) 
	{
		trace_set_err(libtrace, errno, "Reading odp packet failed");
		return -1;
	}
	else if (numbytes == 0)
		return 0;

	//#3. Get pointer from packet and assign it to packet->buffer
	if (!packet->buffer || packet->buf_control == TRACE_CTRL_EXTERNAL) 
	{
		packet->buffer = FORMAT(libtrace)->pkt;
		packet->capture_length = FORMAT(libtrace)->pkt_len;
		packet->payload = packet->buffer;
		packet->wire_length = FORMAT(libtrace)->pkt_len + WIRELEN_DROPLEN;
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
	
	return numbytes;
}

//need to get struct per_stream from thread and use its pointers
static int kafka_pread_pack(libtrace_t *libtrace, libtrace_thread_t *t UNUSED)
{
	int numbytes;
	kafka_per_stream_t *stream = t->format_data;

#if 0
	while (1) 
	{
		rd_kafka_message_t *rkmessage;
		//rd_kafka_resp_err_t err;

#if 0
		/* Poll for errors, etc. */
		rd_kafka_poll(FORMAT(libtrace)->rk, 0);

		/* Consume single message. See rdkafka_performance.c for high speed */
		rkmessage = rd_kafka_consume(FORMAT(libtrace)->rkt, FORMAT(libtrace)->partition, 1000);
#endif
	
		//HighLevel API for poll. Will block for at most 1000 ms
                rkmessage = rd_kafka_consumer_poll(FORMAT(libtrace)->rk, 1000);

		if (!rkmessage) /* timeout */
			continue;

		//copy received packet to internally allocated ram
		stream->pkt = malloc(rkmessage->len);
		memcpy(stream->pkt, rkmessage->payload, rkmessage->len);
		stream->pkt_len = rkmessage->len;

		numbytes = rkmessage->len;
		debug("msg received from topic [%s] with len: %d \n", 
			rd_kafka_topic_name(rkmessage->rkt) ,numbytes);

		msg_consume(rkmessage, NULL);

		/* Return message to rdkafka */
		rd_kafka_message_destroy(rkmessage);

		if (rkmessage->len == 0)
			continue;
		else
		{
			stream->pkts_read++;
			debug("thread: #%d, packet is %d bytes, total packets: %u\n",
				 t->perpkt_num, numbytes, stream->pkts_read);
		}

		//if we got Ctrl-C from one of our utilities, etc
		if (libtrace_halt)
		{
			debug("[got halt]\n");
			return READ_EOF;
		}

#if 0
                stream->pkt = odp_packet_from_event(ev);
                if (!odp_packet_is_valid(stream->pkt))
		{
        		//debug("%s() - packet is INVALID, skipping, or NO PACKET\n", __func__);
                        continue;
		}
		else
		{
#ifdef OPTION_PRINT_PACKETS
			fprintf(stdout, "\n\n NEW PACKET \n");
        		fprintf(stdout, "%s() - packet is valid, print:\n", __func__);
        		fprintf(stdout, "--------------------------------------------------\n");
			odp_packet_print(stream->pkt);
        		fprintf(stdout, "--------------------------------------------------\n");
#endif
		}

                //Returns pointer to the start of the layer 2 header
                stream->l2h = (u_char *)odp_packet_l2_ptr(stream->pkt, NULL);
                stream->pkt_len = (int)odp_packet_len(stream->pkt);
                numbytes = stream->pkt_len;
		stream->pkts_read++;
	
		debug("thread: #%d, packet is %d bytes, total packets: %u\n",
			 t->perpkt_num, numbytes, stream->pkts_read);
#endif
		return numbytes;
	}

#endif
	/* We'll NEVER get here */
	return READ_ERROR;
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
static int kafka_pread_packets(libtrace_t *trace, libtrace_thread_t *t, libtrace_packet_t **packets, size_t nb_packets)
{
	int pkts_read = 0;
	int numbytes = 0;
	uint32_t flags = 0;
	unsigned int i;
	kafka_per_stream_t *stream = t->format_data;

	debug("%s() \n", __func__);

	debug("trying to read %zu packets by a reader thread : %p , type: %u , tid: %lu , perpkt_num: %d \n", 
			nb_packets, t, t->type, t->tid, t->perpkt_num);

	for (i = 0; i < nb_packets; i++, pkts_read++)
	{
		//#0. Free the last packet buffer
		if (packets[i]->buffer) 
		{
			//Check buffer memory is owned by the packet. It is if flag is TRACE_CTRL_PACKET
			assert(packets[i]->buf_control == TRACE_CTRL_PACKET); 
			free(packets[i]->buffer);
			packets[i]->buffer = NULL;
		}

		//#1. Set packet fields
		//TRACE_CTRL_EXTERNAL means buffer memory is owned by an external source, this is it, odp pool.
		packets[i]->buf_control = TRACE_CTRL_EXTERNAL;
		packets[i]->type = TRACE_RT_DATA_ODP;

		//#2. Read a packet from odp. We wait here forever till packet appears.
		numbytes = kafka_pread_pack(trace, t);
		if (numbytes == -1) 
		{
			trace_set_err(trace, errno, "Reading odp packet failed");
			pkts_read = -1;
			break;
		}
		else if (numbytes == 0)
		{
			pkts_read = 0;
			break;
		}

		//#3. Get pointer from packet and assign it to packet->buffer
		if (!packets[i]->buffer || packets[i]->buf_control == TRACE_CTRL_EXTERNAL) 
		{
			packets[i]->buffer = stream->pkt; 
			packets[i]->capture_length = stream->pkt_len;
			packets[i]->payload = packets[i]->buffer; 
			packets[i]->wire_length = stream->pkt_len + WIRELEN_DROPLEN;
			packets[i]->trace = trace;
			packets[i]->error = 1;
			debug("pointer to packet: %p \n", packets[i]->buffer);
			if (!packets[i]->buffer) 
			{
				trace_set_err(trace, errno, "Cannot allocate memory or invalid pointer to packet");
				return -1;
			}
		}
#if 1
		if (lodp_prepare_packet(trace, packets[i], packets[i]->buffer, packets[i]->type, flags))
		{
			pkts_read = -1;
			break;
		}
#endif
	}

	debug("%s() exit with pkts_read : %d \n", __func__, pkts_read);

	return pkts_read;
}

static void acce_fin_packet(libtrace_packet_t *packet)
{
	debug("%s() \n", __func__);

	if (packet->buf_control == TRACE_CTRL_EXTERNAL) 
	{
		free(packet->buffer);
		packet->buffer = NULL;
	}
}

static int acce_write_packet(libtrace_out_t *libtrace, libtrace_packet_t *packet)
{
	debug("%s() \n", __func__);

	int numbytes = 0;
	struct xio_reg_mem xbuf;
	uint8_t *data = NULL;
	struct xio_msg *req = &OUTPUT->req_ring;
	size_t len = trace_get_capture_length(packet);

	//sending accelio message -----
	req->out.header.iov_base = strdup("accelio header request");
	req->out.header.iov_len = strlen((const char *)req->out.header.iov_base) + 1;
	/* iovec[0]*/
	req->in.sgl_type = XIO_SGL_TYPE_IOV;
	req->in.data_iov.max_nents = XIO_IOVLEN;
	req->out.sgl_type = XIO_SGL_TYPE_IOV;
	req->out.data_iov.max_nents = XIO_IOVLEN;

	/* data */
	if ((int)len < OUTPUT->max_msg_size) 
	{ 	/* small msgs - just set iov_base to packet pointer*/
		req->out.data_iov.sglist[0].iov_base = packet->payload;
	} 
	else 
	{ 	/* big msgs */
		if (data == NULL) 
		{
			printf("allocating xio memory...\n");
			xio_mem_alloc(len, &xbuf);
			data = (uint8_t *)xbuf.addr;
			memset(data, 0x0, len);
			memcpy(data, packet->payload, len);
		}
		req->out.data_iov.sglist[0].mr = xbuf.mr;
		req->out.data_iov.sglist[0].iov_base = data;
	}

	req->out.data_iov.sglist[0].iov_len = len + 1;
	req->out.data_iov.nents = 1;

	//sending
	xio_send_request(OUTPUT->conn, req);

	/* event dispatcher is now running */
        xio_context_run_loop(OUTPUT->ctx, XIO_INFINITE);	//XXX - maybe bad idea to wait infinitely

	//freeing packet memory
	free(req->out.header.iov_base);
	if ((int)len < OUTPUT->max_msg_size) 
		free(req->out.data_iov.sglist[0].iov_base);
        if (xbuf.addr) 
	{
                xio_mem_free(&xbuf);
                xbuf.addr = NULL;
        }

	//end of accelio part ---------

	assert(OUTPUT->file);

	//seems like we are writing just raw packet in file
	if ((numbytes = wandio_wwrite(OUTPUT->file, packet->payload, trace_get_capture_length(packet))) !=
				(int)trace_get_capture_length(packet)) 
	{
		trace_set_err_out(libtrace, errno, "Writing packet failed");
		return -1;
	}
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
		return packet->wire_length;
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
	kafka_pstart_input,              /* pstart_input */
	kafka_pread_packets,             /* pread_packets */
	acce_pause_input,               /* ppause */
	acce_fin_input,                 /* p_fin */
	lodp_pregister_thread,          /* pregister_thread */
	lodp_punregister_thread,        /* punregister_thread */
	NULL				/* get thread stats */ 
};

void acce_constructor(void) 
{
	debug("registering acce struct with address: %p , init_output: %p\n", &acce, acce.init_output);
	register_format(&acce);
}
