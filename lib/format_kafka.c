#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>

#include "config.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_helper.h"
#include "wandio.h"
#include "rt_protocol.h"

#include <odp_api.h>

#include <librdkafka/rdkafka.h>
#include <syslog.h>

#define FORMAT(x) ((struct kafka_format_data_t *)x->format_data)
#define DATAOUT(x) ((struct kafka_format_data_out_t *)x->format_data)
#define OUTPUT DATAOUT(libtrace)

#define WIRELEN_DROPLEN 4

#define TOPIC_LEN 512
#define HOSTNAME_LEN 256
#define BROKER_LEN 512
#define GROUP_LEN 512
#define ERR_LEN 512

//----- KAFKA CONFIG -----
//#define KAFKA_TOPIC "kafkatrace"		//not used anymore
#define KAFKA_BROKER "localhost:9092"
//#define KAFKA_GROUP "kafkatracegroup"		//group name - not used anymore
#define KAFKA_MAX_TOPICS 3			//max number of topics to subscribe (only for input)
#define KAFKA_COMPRESSION "snappy"		//could be also "gzip" or "lz4"
#define KAFKA_BATCH_MSGS "10000"		//batch of msgs to send
#define KAFKA_BUFFERING_MS "100"		//ms of waiting till we get full batch of msgs
#define KAFKA_BUFFERING_MAX_MSG "10000000"	//max num of msgs for producer queue (max 10M)

//----- OPTIONS -----
//#define MULTI_INPUT_QUEUES
//#define DEBUG
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

struct kafka_format_data_out_t 
{
	//kafka vars
	rd_kafka_t *rk;
        rd_kafka_conf_t *conf;                  //main conf object
        rd_kafka_topic_t *rkt;                  //topic object
        rd_kafka_topic_conf_t *topic_conf;      //topic configuration obj
        int partition;
        char topic[TOPIC_LEN];			//our specific topic name
        char brokers[BROKER_LEN];
        char errstr[ERR_LEN];
	//other vars
	char *path;
	int level;
	int compress_type;			//store compression type here: bz2, gz etc
	int fileflag;
	iow_t *file;
};

typedef struct kafka_per_stream_s 
{
	int id;
	int core;
	void *pkt;				//store received packet here
	int pkt_len;
	u_char *l2h;
	unsigned int pkts_read;
} kafka_per_stream_t;

//get hostname
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

//get env variable KAFKA_BROKER. if no such - use default value from define	
static char* kafka_broker()
{
	char *env;
	static char broker[BROKER_LEN] = {0};

	env = getenv("KAFKA_BROKER");
        if (env)
	{
        	debug("our env var is: [%s]\n", env);
		memset(broker, 0x0, BROKER_LEN);
		strcpy(broker, env);
	}
	else
	{
		memset(broker, 0x0, BROKER_LEN);
		strcpy(broker, KAFKA_BROKER);
		debug("no KAFKA_BROKER var found. default broker will be used\n");
	}

	debug("full broker address: %s \n", broker);
	return broker;
}

//get env variable KAFKA_GROUP . if no such - use default value group.$hostname	
static char* kafka_group()
{
	char *env;
	static char group[GROUP_LEN] = {0};
	char hname[HOSTNAME_LEN] = {0};

	env = getenv("KAFKA_GROUP");
        if (env)
	{
        	debug("our KAFKA_GROUP env var is: [%s]\n", env);
		memset(group, 0x0, GROUP_LEN);
		strcpy(group, env);
	}
	else
	{
		memset(group, 0x0, GROUP_LEN);
		strcpy(group, "group.");
		gethostname(hname, HOSTNAME_LEN);
		strcat(group, hname);
		debug("no KAFKA_GROUP var found. default group will be used\n");
	}

	debug("group name: %s \n", group);
	return group;
}

//legacy func called on every message. consuming code could be added here
static void msg_consume(rd_kafka_message_t *rkmessage, void *opaque UNUSED) 
{
	rd_kafka_message_t *msg = rkmessage;
	msg = msg;
	debug("len: %d , payload: %s\n", (int)msg->len, (char *)msg->payload);
}

//1. read env variables. 2. create topics-partitions. 3. if fail to get
//topics from env variables - set default one capture.$hostname. 4. subscribe
//returns num of found topics in env vars and registered successfully
static int kafka_init_consume(libtrace_t *libtrace)
{
	int numtopics = 0;
	char *env;
	char topicname[50] = {0};
	char digit[10] = {0};
	rd_kafka_resp_err_t err;
	int i;

	FORMAT(libtrace)->topics = rd_kafka_topic_partition_list_new(KAFKA_MAX_TOPICS);

	for (i = 0; i < KAFKA_MAX_TOPICS; i++)
	{
		memset(topicname, 0x0, 50);
		strcpy(topicname, "KAFKA_CONSUME_TOPIC");
		sprintf(digit, "%d", i+1);
		strcat(topicname, digit);
		debug("looking for env variable %s to read topic name \n", topicname);
		env = getenv(topicname);
		if (env)
		{
			debug("#%d topic name is: [%s]\n", i+1, env);
			if (rd_kafka_topic_partition_list_add(FORMAT(libtrace)->topics, env,
								 FORMAT(libtrace)->partition))
			{
				numtopics++;
				debug("topic+partition added successfully\n");
			}
			else
			{
				error("failed to add topic+partition\n");
				continue;
			}
			
		}
		else
		{
			debug("no var found. \n");
		}

	}
	
	//if we didn't get any topics from env variables - we try to add default one: capture.$hostname
	if (!numtopics)
	{
		if (rd_kafka_topic_partition_list_add(FORMAT(libtrace)->topics, kafka_hostname(),
							FORMAT(libtrace)->partition))
		{
			debug("default topic+partition added successfully\n");
			numtopics++;
		}
		else
			error("failed to add topic+partition\n");
	}

	//we need at least one topic found to subscribe
	if (numtopics)	
	{
		err = rd_kafka_subscribe(FORMAT(libtrace)->rk, FORMAT(libtrace)->topics);
		if (err)
                        error("Failed to start consuming topics: %s\n", rd_kafka_err2str(err));
	}

	return numtopics;
}

#if 0
static int lodp_init_environment(char *uridata, struct kafka_format_data_t *format_data, char *err, int errlen)
{
	//int ret; //returned error codes
	//struct rte_pci_addr use_addr; /* The only address that we don't blacklist - needed for DPDK */
	//char cpu_number[10] = {0}; /* The CPU mask we want to bind to */
	int num_cpu; /* The number of CPUs in the system */
	//int my_cpu; /* The CPU number we want to bind to */
	//odp vars
	odp_pool_t pool;
	//odp_pktio_t pktio;
        odp_pool_param_t params;
        odp_pktio_param_t pktio_param;
        odp_pktin_queue_param_t pktin_param;
	odp_pktio_capability_t capa;
	char devname[] = "0";		// - IMPORTANT - this is dpdk port number, should be 0! Only digits accepted!
	char dpdk_params[256] = {0};
	char *odp_error = "No error";

	if (strlen(odp_error) < (size_t)errlen) 
		strcpy(err, odp_error);

	//DPDK setup -----------------------------------------------------------
	//we need to set command line for DPDK which we will pass through ODP


#if 0
	char* argv[] = {"libtrace",
	                "-c", cpu_number,
	                "-n", "1",
	                "--proc-type", "auto",
	                "--file-prefix",
	                "-m", "512", NULL};

	int argc = sizeof(argv) / sizeof(argv[0]) - 1;
#endif

	/* Get the number of cpu cores in the system and use the last core
	 * on the correct numa node */
	num_cpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (num_cpu <= 0) 
	{
		perror("sysconf(_SC_NPROCESSORS_ONLN) failed."
		       " Falling back to the first core.");
		num_cpu = 1; /* fallback to the first core */
	}

	//have 0 core selected by default
	//my_cpu = 0;

	//forming params -------------------------------------------------------
	printf("uridata: %s \n", uridata);
	strcpy(dpdk_params, "-c 0xF -n 4 -w ");
	strcat(dpdk_params, uridata);
	printf("dpdk params passed: %s \n", dpdk_params);


	/* This allows the user to specify the core - we would try to do this
	 * automatically but it's hard to tell that this is secondary
	 * before running rte_eal_init(...). Currently we are limited to 1
	 * instance per core due to the way memory is allocated. */

//don't need it now - we don't call dpdk white/black list functions, just pass param
#if 0
	if (parse_pciaddr(uridata, &use_addr, &my_cpu) != 0) {
		fprintf(stderr, "Failed to parse URI\n");
		return -1;
	}
#endif

	//ODP setup ------------------------------------------------------------
	/* Init ODP before calling anything else */
	//@first param - odp params, @second param - dpdk params (passed through)
	//const odp_platform_init_t *platform_params
        if (odp_init_global(&format_data->odp_instance, NULL, (odp_platform_init_t*)dpdk_params))
	{
                fprintf(stderr, "Error: ODP global init failed.\n");
                exit(EXIT_FAILURE);
        }

        /* Create thread structure for ODP */		//XXX - maybe ODP_THREAD_CONTROL ?
	int i;
	for (i = 0; i < 1; i++)
	{
		if (odp_init_local(format_data->odp_instance, ODP_THREAD_WORKER))
		{
			fprintf(stderr, "Error: ODP local init failed.\n");
			exit(EXIT_FAILURE);
		}
		else
			printf("worker thread #%d was inited successfully\n", i);
	}

        /* Creating pool */
        pool = odp_pool_lookup("packet_pool");
        if (pool == ODP_POOL_INVALID) 
	{
                /* Create packet pool */
                odp_pool_param_init(&params);                   //init pool with default values
                params.pkt.seg_len = SHM_PKT_POOL_BUF_SIZE;
                params.pkt.len     = SHM_PKT_POOL_BUF_SIZE;
                params.pkt.num     = SHM_PKT_POOL_SIZE/SHM_PKT_POOL_BUF_SIZE;
                params.type        = ODP_POOL_PACKET;

                pool = odp_pool_create("packet_pool", &params);

                if (pool == ODP_POOL_INVALID) {
                        fprintf(stderr, "Error: packet pool create failed.\n");
                        exit(EXIT_FAILURE);
                }
                odp_pool_print(pool);
        } 
	else 
                fprintf(stdout, "packet pool have been created.\n");

        //----- setting up pktio ------------------------------------------------------

        //setting pktio_param
        odp_pktio_param_init(&pktio_param);
        pktio_param.in_mode = ODP_PKTIN_MODE_SCHED;     //XXX - if wont work try MODE_QUEUE

        /* Open a packet IO instance */
	fprintf(stdout, "calling odp_pktio_open()\n");
        format_data->pktio = odp_pktio_open(devname, pool, &pktio_param);
        if (format_data->pktio == ODP_PKTIO_INVALID) {
                fprintf(stderr, "  Error: pktio create failed %s\n", devname);
                return 1;
        }

	if (odp_pktio_capability(format_data->pktio, &capa)) 
	{
		printf("Error: capability query failed \n");
		return 1;
        }

	printf("max input queues: %d \n", (int)capa.max_input_queues);

        //setting queue param
        odp_pktin_queue_param_init(&pktin_param);
	//-----multiqueues-----
	pktin_param.op_mode     = ODP_PKTIO_OP_MT_UNSAFE;
	pktin_param.hash_enable = 1;
#ifdef MULTI_INPUT_QUEUES
	pktin_param.num_queues  = 4;			//XXX - HARDCODE
#else
	pktin_param.num_queues  = 1;

#endif
	//-----multiqueues-----
        pktin_param.queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
        pktin_param.queue_param.sched.prio = ODP_SCHED_PRIO_DEFAULT;

	//configure in queue
        if (odp_pktin_queue_config(format_data->pktio, &pktin_param))
        {
                fprintf(stderr, "  Error: queue config failed %s\n", devname);
                return 1;
        }

        //set OUT queue. NULL as param means default values will be used
        if (odp_pktout_queue_config(format_data->pktio, NULL))
                fprintf(stderr, "Error: pktout config failed\n");

	return 0;
}
#endif

/* Initialises an input trace using the capture format. 
   @param libtrace 	The input trace to be initialised */
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

//let's don't use it yet, maybe we could live without it
#if 0
	/* Callback called on partition assignment changes */
	rd_kafka_conf_set_rebalance_cb(conf, rebalance_cb);
#endif
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

        //create topic
        //Topic handles are refcounted internally and calling rd_kafka_topic_new()
        //again with the same topic name will return the previous topic handle
#if 0
        FORMAT(libtrace)->rkt = rd_kafka_topic_new(FORMAT(libtrace)->rk, 
		FORMAT(libtrace)->topic, FORMAT(libtrace)->topic_conf);
        FORMAT(libtrace)->topic_conf = NULL; /* Now owned by topic */
#endif

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

//Initialises an output trace using the capture format.
static int kafka_init_output(libtrace_out_t *libtrace) 
{
	char *env;

	debug("%s() \n", __func__);

	libtrace->format_data = malloc(sizeof(struct kafka_format_data_out_t));
	OUTPUT->file = NULL;
	OUTPUT->level = 0;
	OUTPUT->compress_type = TRACE_OPTION_COMPRESSTYPE_NONE;
	OUTPUT->fileflag = O_CREAT | O_WRONLY;

	//init kafka
	/* The unassigned partition is used by the producer API for messages
	 * that should be partitioned using the configured or default partitioner.*/
	OUTPUT->partition = RD_KAFKA_PARTITION_UA;	//it is -1

	//set default output topic: capture.$hostname
	strcpy(OUTPUT->topic, kafka_hostname());

	//overwrite output topic from env variable if such is present
        env = getenv("KAFKA_OUTPUT_TOPIC");
        if (env)
        {
                debug("set output kafka topic to: [%s]\n", env);
                memset(OUTPUT->topic, 0x0, TOPIC_LEN);
                strcpy(OUTPUT->topic, env);
        }

	strcpy(OUTPUT->brokers, kafka_broker());
	memset(OUTPUT->errstr, 0x0, sizeof(OUTPUT->errstr));

        //----- kafka configuration -----
	//default settings, batch.num.messages=10000 and queue.buffering.max.ms=1000
        //'batch.num.messages' - the min numb of messages to wait for to accumulate before sending
        OUTPUT->conf = rd_kafka_conf_new();
        rd_kafka_conf_set(OUTPUT->conf, "compression.codec", KAFKA_COMPRESSION, OUTPUT->errstr, sizeof(OUTPUT->errstr));
        rd_kafka_conf_set(OUTPUT->conf, "batch.num.messages", KAFKA_BATCH_MSGS, OUTPUT->errstr, sizeof(OUTPUT->errstr));
        rd_kafka_conf_set(OUTPUT->conf, "queue.buffering.max.ms", KAFKA_BUFFERING_MS, OUTPUT->errstr, sizeof(OUTPUT->errstr));
        rd_kafka_conf_set(OUTPUT->conf, "queue.buffering.max.messages", KAFKA_BUFFERING_MAX_MSG, OUTPUT->errstr, sizeof(OUTPUT->errstr));

        //topic configuration
        OUTPUT->topic_conf = rd_kafka_topic_conf_new();

        //----- create kafka handle -----
        OUTPUT->rk = rd_kafka_new(RD_KAFKA_PRODUCER, OUTPUT->conf, OUTPUT->errstr, sizeof(OUTPUT->errstr));
        if (!OUTPUT->rk)
        {
                fprintf(stderr, "%% Failed to create new producer: %s\n", OUTPUT->errstr);
                exit(1);
        }

        rd_kafka_set_log_level(OUTPUT->rk, LOG_DEBUG);

        //add brokers
        if (!rd_kafka_brokers_add(OUTPUT->rk, OUTPUT->brokers))
        {
                fprintf(stderr, "%% No valid brokers specified\n");
                exit(1);
        }

        //create topic
        OUTPUT->rkt = rd_kafka_topic_new(OUTPUT->rk, OUTPUT->topic, OUTPUT->topic_conf);
        OUTPUT->topic_conf = NULL; /* Now owned by topic */



#if 0
	//this is same we do in odp_init_input(), but with kafka_format_data_out_t struct
	if (lodp_init_environment(libtrace->uridata, FORMAT(libtrace), err, sizeof(err)) != 0) {
		trace_set_err_out(libtrace, TRACE_ERR_INIT_FAILED, "%s", err);
		free(libtrace->format_data);
		libtrace->format_data = NULL;
		return -1;
	}
#endif

	return 0;
}

static int kafka_config_output(libtrace_out_t *libtrace, trace_option_output_t option, void *data)
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

static int kafka_start_input(libtrace_t *libtrace) 
{
	int ret = 0;
	libtrace = libtrace;

	debug("%s() \n", __func__);

#if 0
	/* Start consuming */
	if (rd_kafka_consume_start(FORMAT(libtrace)->rkt, FORMAT(libtrace)->partition,
		 /*start_offset*/ RD_KAFKA_OFFSET_BEGINNING) == -1)
	{
		fprintf(stderr, "<error> failed to start consuming!\n");
		/*
		rd_kafka_resp_err_t err = rd_kafka_last_error();
		fprintf(stderr, "%% Failed to start consuming: %s\n", rd_kafka_err2str(err));
		if (err == RD_KAFKA_RESP_ERR__INVALID_ARG)
			fprintf(stderr, "%% Broker based offset storage "
				"requires a group.id, add: -X group.id=yourGroup\n");
		*/
		
		ret = -1;
		return ret;
	}
	else
		debug("start consuming from first topic\n");
#endif


	return ret;
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

#if 0
	/* Start consuming */
	if (rd_kafka_consume_start(FORMAT(libtrace)->rkt, FORMAT(libtrace)->partition,
		 /*start_offset*/ RD_KAFKA_OFFSET_BEGINNING) == -1)
	{
		fprintf(stderr, "<error> failed to start consuming!\n");
		/*
		rd_kafka_resp_err_t err = rd_kafka_last_error();
		fprintf(stderr, "%% Failed to start consuming: %s\n", rd_kafka_err2str(err));
		if (err == RD_KAFKA_RESP_ERR__INVALID_ARG)
			fprintf(stderr, "%% Broker based offset storage "
				"requires a group.id, add: -X group.id=yourGroup\n");
		*/
		
		ret = -1;
		return ret;
	}
#endif

	return ret;
}
	
/* Pauses an input trace - this function should close or detach the file or 
   device that is being read from. 
   @return 0 if successful, -1 in the event of error
*/
static int kafka_pause_input(libtrace_t * libtrace) 
{
	(void)libtrace;

	debug("%s() \n", __func__);

	debug("fake function. instead of pausing input - do nothing \n");

	return 0;
}

static int kafka_start_output(libtrace_out_t *libtrace) 
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

static int kafka_fin_input(libtrace_t *libtrace) 
{
        rd_kafka_resp_err_t err;

	debug("%s() \n", __func__);

	if (libtrace->io)
	{
		wandio_destroy(libtrace->io);
		debug("wandio destroyed\n");
	}

        err = rd_kafka_consumer_close(FORMAT(libtrace)->rk);
        if (err)
                fprintf(stderr, "%% Failed to close consumer: %s\n",
                        rd_kafka_err2str(err));
        else
                fprintf(stderr, "%% kafka consumer closed\n");

	if (FORMAT(libtrace)->per_stream)
		libtrace_list_deinit(FORMAT(libtrace)->per_stream);

	if(FORMAT(libtrace)->topics)
		rd_kafka_topic_partition_list_destroy(FORMAT(libtrace)->topics);

	if (libtrace->format_data)
	{
		free(libtrace->format_data);
		libtrace->format_data = NULL;
	}

	debug("%s() exiting\n", __func__);

	return 0;
}

static int kafka_fin_output(libtrace_out_t *libtrace) 
{
	debug("%s() \n", __func__);

	//STOP KAFKA AND FREE RESOURCES
        /* Poll to handle delivery reports */
        rd_kafka_poll(OUTPUT->rk, 0);

        /* Wait for messages to be delivered */
        while (rd_kafka_outq_len(OUTPUT->rk) > 0)       //XXX - do we need it?
                rd_kafka_poll(OUTPUT->rk, 100);


        //----- free resources -----
        /* Destroy topic */
        rd_kafka_topic_destroy(OUTPUT->rkt);

        /* Destroy the handle */
        rd_kafka_destroy(OUTPUT->rk);

        if (OUTPUT->topic_conf)
                rd_kafka_topic_conf_destroy(OUTPUT->topic_conf);

	wandio_wdestroy(OUTPUT->file);

	//XXX - we have stopped pktio already in odp_fin_input(). 
	//Probably there is a need to stop it also here, but probably not.

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
static int kafka_read_pack(libtrace_t *libtrace)
{
	int numbytes;

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
 * If no packets are available for reading, this function should block
 * until one appears or return 0 if the end of a trace file has been
 * reached.
 */

//So endless loop while no packets and return bytes read in case there is a packet (no one checks returned bytes)
static int kafka_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) 
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

	//#2. Read a packet from odp. We wait here forever till packet appears.
	numbytes = kafka_read_pack(libtrace);
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

static void kafka_fin_packet(libtrace_packet_t *packet)
{
	debug("%s() \n", __func__);

	if (packet->buf_control == TRACE_CTRL_EXTERNAL) 
	{
		free(packet->buffer);
		packet->buffer = NULL;
	}
}

static int kafka_write_packet(libtrace_out_t *libtrace, 
		libtrace_packet_t *packet) 
{
	int numbytes = 0;
	//rd_kafka_resp_err_t err;

	debug("%s() \n", __func__);

	//sending kafka message -----
	size_t len = trace_get_capture_length(packet);
	if (rd_kafka_produce(OUTPUT->rkt, OUTPUT->partition, RD_KAFKA_MSG_F_COPY,
		 packet->payload, len, NULL, 0, NULL) == -1)
	{
		fprintf(stderr, "%% Failed to produce to topic %s partition %i. err num: %d, err: %s \n",
			rd_kafka_topic_name(OUTPUT->rkt), OUTPUT->partition, rd_kafka_errno2err(errno),
			rd_kafka_err2str(rd_kafka_errno2err(errno)));
		
		/* Poll to handle delivery reports */
		rd_kafka_poll(OUTPUT->rk, 0);
		return numbytes;
	}

	debug("# %% Sent %zd bytes to topic [%s] partition [%i]\n",
		len, rd_kafka_topic_name(OUTPUT->rkt), OUTPUT->partition);

	/* Poll to handle delivery reports */
	rd_kafka_poll(OUTPUT->rk, 0);

	//end of kafka -----

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
static struct libtrace_format_t kafka = {
        "kafka",			/* name used in URI to identify capture format - odp:iface */
        "$Id$",				/* version of this module */
        TRACE_FORMAT_KAFKA,		/* The RT protocol type of this module */
	NULL,				/* probe filename - guess capture format - NOT NEEDED*/
	NULL,				/* probe magic - NOT NEEDED*/
        kafka_init_input,	        /* init_input - Initialises an input trace using the capture format */
        NULL,                           /* config_input - Sets value to some option */
        kafka_start_input,	        /* start_input-Starts or unpause an input trace (also opens file or device for reading)*/
        kafka_pause_input,               /* pause_input */
        kafka_init_output,               /* init_output - Initialises an output trace using the capture format. */
        kafka_config_output,             /* config_output */
        kafka_start_output,              /* start_output */
        kafka_fin_input,	         /* fin_input - Stops capture input data.*/
        kafka_fin_output,                /* fin_output */
        kafka_read_packet,        	 /* read_packet - Reads next packet from input trace into the packet structure */
        lodp_prepare_packet,		/* prepare_packet - Converts a buffer containing a packet record into a libtrace packet */
	kafka_fin_packet,                /* fin_packet - Frees any resources allocated for a libtrace packet */
        kafka_write_packet,              /* write_packet - Write a libtrace packet to an output trace */
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
	kafka_pause_input,               /* ppause */
	kafka_fin_input,                 /* p_fin */
	lodp_pregister_thread,          /* pregister_thread */
	lodp_punregister_thread,        /* punregister_thread */
	NULL				/* get thread stats */ 
};

void kafka_constructor(void) 
{
	debug("registering kafka struct with address: %p , init_output: %p\n", &kafka, kafka.init_output);
	register_format(&kafka);
}
