/* format odp support 
 *
 */

#include <stdlib.h>
#include <unistd.h>
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
//#include <odp/helper/linux.h>
//#include <odp/helper/eth.h>
//#include <odp/helper/ip.h>

#define SHM_PKT_POOL_SIZE      (512*2048)
#define SHM_PKT_POOL_BUF_SIZE  1856

#define FORMAT(x) ((struct odp_format_data_t *)x->format_data)
#define DATAOUT(x) ((struct duck_format_data_out_t *)x->format_data)

#define OUTPUT DATAOUT(libtrace)

struct odp_format_data_t {
	int pvt;
	/* Our parallel streams */
	libtrace_list_t *per_stream;
};

//----- DPDK stream part. Maybe we don't need it -------------------------------
#if 0
struct dpdk_per_stream_t
{
	uint16_t queue_id;
	uint64_t ts_last_sys; /* System timestamp of our most recent packet in nanoseconds */
	struct rte_mempool *mempool;
	int lcore;
}

#define DPDK_EMPTY_STREAM {-1, 0, NULL, -1}

typedef struct dpdk_per_stream_t dpdk_per_stream_t;
#endif

struct duck_format_data_t {
	char *path;
	int dag_version;
};

struct duck_format_data_out_t {
	char *path;
	int level;
	int compress_type;
	int fileflag;
	iow_t *file;
	int dag_version;	
};


static int odp_init_environment(char *uridata, struct odp_format_data_t *format_data, char *err, int errlen)
{
	int ret; //returned error codes
	char cpu_number[10] = {0}; /* The CPU mask we want to bind to */
	int num_cpu; /* The number of CPUs in the system */
	int my_cpu; /* The CPU number we want to bind to */
	//odp vars
	odp_pool_t pool;
	odp_pktio_t pktio;
        odp_pool_param_t params;
        odp_pktio_param_t pktio_param;
        odp_pktin_queue_param_t pktin_param;
	char devname[] = "odp";

	//DPDK setup -----------------------------------------------------------
	//we need to set command line for DPDK which we will pass through ODP
	char* argv[] = {"libtrace",
	                "-c", cpu_number,
	                "-n", "1",
	                "--proc-type", "auto",
	                "--file-prefix",
	                "-m", "512", NULL};

	int argc = sizeof(argv) / sizeof(argv[0]) - 1;

	/* Get the number of cpu cores in the system and use the last core
	 * on the correct numa node */
	num_cpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (num_cpu <= 0) 
	{
		perror("sysconf(_SC_NPROCESSORS_ONLN) failed."
		       " Falling back to the first core.");
		num_cpu = 1; /* fallback to the first core */
	}

	my_cpu = 0;	//XXX

	//ODP setup ------------------------------------------------------------
	/* Init ODP before calling anything else */
        if (odp_init_global(NULL, NULL)) 
	{
                fprintf(stderr, "Error: ODP global init failed.\n");
                exit(EXIT_FAILURE);
        }

        /* Create thread structure for ODP */		//XXX - maybe ODP_THREAD_CONTROL ?
        if (odp_init_local(ODP_THREAD_WORKER)) 
	{
                fprintf(stderr, "Error: ODP local init failed.\n");
                exit(EXIT_FAILURE);
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
        pktio = odp_pktio_open(devname, pool, &pktio_param);
        if (pktio == ODP_PKTIO_INVALID) {
                fprintf(stderr, "  Error: pktio create failed %s\n", devname);
                return 1;
        }

        //setting queue param
        odp_pktin_queue_param_init(&pktin_param);
        pktin_param.queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
        pktin_param.queue_param.sched.prio = ODP_SCHED_PRIO_DEFAULT;

	//configure in queue
        if (odp_pktin_queue_config(pktio, &pktin_param))
        {
                fprintf(stderr, "  Error: queue config failed %s\n", devname);
                return 1;
        }

        //set OUT queue. NULL as param means default values will be used
        if (odp_pktout_queue_config(pktio, NULL))
                fprintf(stderr, "Error: pktout config failed\n");

	//start pktio
        fprintf(stdout, "going to start pktio\n");
        ret = odp_pktio_start(pktio);
        if (ret != 0)
                fprintf(stderr, "Error: unable to start pktio\n");

        printf("  created pktio:%02i, queue mode\n default pktio%02i-INPUT queue\n",
                (int)pktio, (int)pktio);

	return 0;
}


/* Initialises an input trace using the capture format. 
   @param libtrace 	The input trace to be initialised */
static int odp_init_input(libtrace_t *libtrace) 
{
#if 0
	dpdk_per_stream_t stream = DPDK_EMPTY_STREAM;
#endif
	char err[500] = {0};

	//this is a common practice in every format to allocate specific struct and then init it fields 
	libtrace->format_data = malloc(sizeof(struct odp_format_data_t));
	//XXX - here we need to init all the data in odp_format_data_t
	FORMAT(libtrace)->pvt = 0xFAFAFAFA;

#if 0
	/* Make our first stream XXX - add our struct per stream */
	FORMAT(libtrace)->per_stream = libtrace_list_init(sizeof(struct dpdk_per_stream_t));
	libtrace_list_push_back(FORMAT(libtrace)->per_stream, &stream);
#endif

	if (odp_init_environment(libtrace->uridata, FORMAT(libtrace), err, sizeof(err))) 
	{
		trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "%s", err);
		free(libtrace->format_data);
		libtrace->format_data = NULL;
		return -1;
	}
	return 0;
}

static int duck_init_output(libtrace_out_t *libtrace) {
	libtrace->format_data = malloc(sizeof(struct duck_format_data_out_t));
	
	OUTPUT->level = 0;
	OUTPUT->compress_type = TRACE_OPTION_COMPRESSTYPE_NONE;
	OUTPUT->fileflag = O_CREAT | O_WRONLY;
	OUTPUT->file = 0;
	OUTPUT->dag_version = 0;
	return 0;
}

static int duck_config_output(libtrace_out_t *libtrace, 
				trace_option_output_t option,
				void *data) {
	switch (option) {
		case TRACE_OPTION_OUTPUT_COMPRESS:
			OUTPUT->level = *(int *)data;
			return 0;
		case TRACE_OPTION_OUTPUT_COMPRESSTYPE:
			OUTPUT->compress_type = *(int *)data;
			return 0;
		case TRACE_OPTION_OUTPUT_FILEFLAGS:
			OUTPUT->fileflag = *(int *)data;
			return 0;
		default:
			trace_set_err_out(libtrace, TRACE_ERR_UNKNOWN_OPTION,
					"Unknown option");
			return -1;
	}
	assert(0);
}

static int duck_start_input(libtrace_t *libtrace) {
	
	if (libtrace->io)
		/* File already open */
		return 0;
	
	libtrace->io = trace_open_file(libtrace);
	if (!libtrace->io)
		return -1;

	return 0;
}

static int duck_start_output(libtrace_out_t *libtrace) {
	OUTPUT->file = trace_open_file_out(libtrace, 
						OUTPUT->compress_type,
						OUTPUT->level,
						OUTPUT->fileflag);
	if (!OUTPUT->file) {
		return -1;
	}
	return 0;
}

static int duck_fin_input(libtrace_t *libtrace) {
	wandio_destroy(libtrace->io);
	free(libtrace->format_data);

	return 0;
}

static int duck_fin_output(libtrace_out_t *libtrace) {
	wandio_wdestroy(OUTPUT->file);
	free(libtrace->format_data);
	return 0;
}

static int duck_prepare_packet(libtrace_t *libtrace, libtrace_packet_t *packet,
		void *buffer, libtrace_rt_types_t rt_type, uint32_t flags) {

        if (packet->buffer != buffer &&
                        packet->buf_control == TRACE_CTRL_PACKET) {
                free(packet->buffer);
        }

        if ((flags & TRACE_PREP_OWN_BUFFER) == TRACE_PREP_OWN_BUFFER) {
                packet->buf_control = TRACE_CTRL_PACKET;
        } else
                packet->buf_control = TRACE_CTRL_EXTERNAL;


        packet->buffer = buffer;
        packet->header = NULL;
	packet->payload = buffer;
	packet->type = rt_type;

	if (libtrace->format_data == NULL) {
		if (odp_init_input(libtrace))
			return -1;
	}

	return 0;
}

static int duck_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) {

	int numbytes = 0;
	uint32_t version = 0;
	unsigned int duck_size;
	uint32_t flags = 0;
	
	if (!packet->buffer || packet->buf_control == TRACE_CTRL_EXTERNAL) {
                packet->buffer = malloc((size_t)LIBTRACE_PACKET_BUFSIZE);
                if (!packet->buffer) {
                        trace_set_err(libtrace, errno,
                                        "Cannot allocate memory");
                        return -1;
                }
        }

	flags |= TRACE_PREP_OWN_BUFFER;
	
#if 0
	if (DATA(libtrace)->dag_version == 0) {
		/* Read in the duck version from the start of the trace */
		if ((numbytes = wandio_read(libtrace->io, &version, 
					sizeof(version))) != sizeof(uint32_t)) {
			trace_set_err(libtrace, errno, 
					"Reading DUCK version failed");
			return -1;
		}
		if (numbytes == 0) {
			return 0;
		}
		DATA(libtrace)->dag_version = bswap_le_to_host32(version);
	}
	

	if (DATA(libtrace)->dag_version == TRACE_RT_DUCK_2_4) {
		duck_size = sizeof(duck2_4_t);
		packet->type = TRACE_RT_DUCK_2_4;
	} else if (DATA(libtrace)->dag_version == TRACE_RT_DUCK_2_5) {
		duck_size = sizeof(duck2_5_t);
		packet->type = TRACE_RT_DUCK_2_5;
	} else if (DATA(libtrace)->dag_version == TRACE_RT_DUCK_5_0) {
		duck_size = sizeof(duck5_0_t);
		packet->type = TRACE_RT_DUCK_5_0;
	} else {
		trace_set_err(libtrace, TRACE_ERR_BAD_PACKET,
				"Unrecognised DUCK version %i", 
				DATA(libtrace)->dag_version);
		return -1;
	}
#endif

	if ((numbytes = wandio_read(libtrace->io, packet->buffer,
					(size_t)duck_size)) != (int)duck_size) {
		if (numbytes == -1) {
			trace_set_err(libtrace, errno, "Reading DUCK failed");
			return -1;
		}
		else if (numbytes == 0) {
			return 0;
		}
		else {
			trace_set_err(libtrace, TRACE_ERR_BAD_PACKET, "Truncated DUCK packet");
		}
	}

	if (duck_prepare_packet(libtrace, packet, packet->buffer, packet->type,
				flags)) 
		return -1;
	
	return numbytes;
}

static int duck_write_packet(libtrace_out_t *libtrace, 
		libtrace_packet_t *packet) 
{

	int numbytes = 0;
	uint32_t duck_version;

	if (packet->type != TRACE_RT_DUCK_2_4 
			&& packet->type != TRACE_RT_DUCK_2_5 &&
			packet->type != TRACE_RT_DUCK_5_0) {
		trace_set_err_out(libtrace, TRACE_ERR_BAD_PACKET,
				"Only DUCK packets may be written to a DUCK file");
		return -1;
	}
	
	assert(OUTPUT->file);

	if (OUTPUT->dag_version == 0) {
	/* Writing the DUCK version will help with reading it back in later! */
		duck_version = bswap_host_to_le32(packet->type);
		if ((numbytes = wandio_wwrite(OUTPUT->file, &duck_version,
				sizeof(duck_version))) != sizeof(uint32_t)){
			trace_set_err_out(libtrace, errno, 
					"Writing DUCK version failed");
			return -1;
		}
		OUTPUT->dag_version = packet->type;
	}
	
	if ((numbytes = wandio_wwrite(OUTPUT->file, packet->payload, 
					trace_get_capture_length(packet))) !=
				(int)trace_get_capture_length(packet)) {
		trace_set_err_out(libtrace, errno, "Writing DUCK failed");
		return -1;
	}
	return numbytes;
}

static int duck_get_capture_length(const libtrace_packet_t *packet) {
	switch(packet->type) {
		case TRACE_RT_DUCK_2_4:
			return sizeof(duck2_4_t);
		case TRACE_RT_DUCK_2_5:
			return sizeof(duck2_5_t);
		case TRACE_RT_DUCK_5_0:
			return sizeof(duck5_0_t);
		default:
			trace_set_err(packet->trace,TRACE_ERR_BAD_PACKET,
					"Not a duck packet");
			return -1;
	}
	return 0;
}

static int duck_get_framing_length(const libtrace_packet_t *packet UNUSED) 
{
	return 0;
}

static int duck_get_wire_length(const libtrace_packet_t *packet UNUSED) 
{
	return 0;
}

static libtrace_linktype_t duck_get_link_type(
				const libtrace_packet_t *packet UNUSED) 
{
	return TRACE_TYPE_DUCK;
}

/* <== *** ==> */
static void odp_help(void) {
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
static struct libtrace_format_t odp = {
        "odp",				/* name used in URI to identify capture format - odp:iface */
        "$Id$",				/* version of this module */
        TRACE_FORMAT_ODP,		/* The RT protocol type of this module */
	NULL,				/* probe filename - guess capture format - NOT NEEDED*/
	NULL,				/* probe magic - NOT NEEDED*/
        odp_init_input,	        	/* init_input - Initialises an input trace using the capture format */
        NULL,                           /* config_input */
        duck_start_input,	        /* start_input */
        NULL,                           /* pause_input */
        duck_init_output,               /* init_output */
        duck_config_output,             /* config_output */
        duck_start_output,              /* start_output */
        duck_fin_input,	               	/* fin_input */
        duck_fin_output,                /* fin_output */
        duck_read_packet,        	/* read_packet */
        duck_prepare_packet,		/* prepare_packet */
	NULL,                           /* fin_packet */
        duck_write_packet,              /* write_packet */
        duck_get_link_type,    		/* get_link_type */
        NULL,              		/* get_direction */
        NULL,              		/* set_direction */
        NULL,          			/* get_erf_timestamp */
        NULL,                           /* get_timeval */
	NULL,				/* get_timespec */
        NULL,                           /* get_seconds */
        NULL,                   	/* seek_erf */
        NULL,                           /* seek_timeval */
        NULL,                           /* seek_seconds */
        duck_get_capture_length,  	/* get_capture_length */
        duck_get_wire_length,  		/* get_wire_length */
        duck_get_framing_length, 	/* get_framing_length */
        NULL,         			/* set_capture_length */
	NULL,				/* get_received_packets */
	NULL,				/* get_filtered_packets */
	NULL,				/* get_dropped_packets */
	NULL,				/* get_statistics */
        NULL,                           /* get_fd */
        NULL,              		/* trace_event */
        odp_help,                     	/* help */
        NULL,                            /* next pointer */
        NON_PARALLEL(false)
};

/* <== *** ==> */
void odp_constructor(void) {
	register_format(&odp);
}
