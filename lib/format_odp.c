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
#define DATAOUT(x) ((struct odp_format_data_out_t *)x->format_data)
#define OUTPUT DATAOUT(libtrace)

struct odp_format_data_t {
	int pvt;
	unsigned int pkts_read;
	odp_pktio_t pktio;
	odp_packet_t pkt;	//ptr for current packet which we pass to prepare_packet()
	u_char *l2h;		//l2 header for current packet
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

#if 0
struct duck_format_data_t {
	char *path;
	int dag_version;
};
#endif

struct odp_format_data_out_t {
	char *path;
/*
	int level;
	int compress_type;
	int fileflag;
*/
	iow_t *file;
//	int dag_version;	
};

static int odp_init_environment(char *uridata, struct odp_format_data_t *format_data, char *err, int errlen)
{
	int ret; //returned error codes
	char cpu_number[10] = {0}; /* The CPU mask we want to bind to */
	int num_cpu; /* The number of CPUs in the system */
	int my_cpu; /* The CPU number we want to bind to */
	//odp vars
	odp_pool_t pool;
	//odp_pktio_t pktio;
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
        FORMAT(libtrace)->pktio = odp_pktio_open(devname, pool, &pktio_param);
        if (FORMAT(libtrace)->pktio == ODP_PKTIO_INVALID) {
                fprintf(stderr, "  Error: pktio create failed %s\n", devname);
                return 1;
        }

        //setting queue param
        odp_pktin_queue_param_init(&pktin_param);
        pktin_param.queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
        pktin_param.queue_param.sched.prio = ODP_SCHED_PRIO_DEFAULT;

	//configure in queue
        if (odp_pktin_queue_config(FORMAT(libtrace)->pktio, &pktin_param))
        {
                fprintf(stderr, "  Error: queue config failed %s\n", devname);
                return 1;
        }

        //set OUT queue. NULL as param means default values will be used
        if (odp_pktout_queue_config(FORMAT(libtrace)->pktio, NULL))
                fprintf(stderr, "Error: pktout config failed\n");

	return 0;
}


/* Initialises an input trace using the capture format. 
   @param libtrace 	The input trace to be initialised */
static int odp_init_input(libtrace_t *libtrace) 
{
	char err[500] = {0};
#if 0
	dpdk_per_stream_t stream = DPDK_EMPTY_STREAM;
#endif
	//init all the data in odp_format_data_t
	libtrace->format_data = malloc(sizeof(struct odp_format_data_t));
	FORMAT(libtrace)->pvt = 0xFAFAFAFA;
	FORMAT(libtrace)->pkts_read = 0;
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

//Initialises an output trace using the capture format.
static int odp_init_output(libtrace_out_t *libtrace) {
	libtrace->format_data = malloc(sizeof(struct odp_format_data_out_t));
#if 0
	OUTPUT->level = 0;
	OUTPUT->compress_type = TRACE_OPTION_COMPRESSTYPE_NONE;
	OUTPUT->fileflag = O_CREAT | O_WRONLY;
#endif
	OUTPUT->file = 0;
//	OUTPUT->dag_version = 0;

	//this is same we do in odp_init_input(), but with odp_format_data_out_t struct
	if (odp_init_environment(libtrace->uridata, FORMAT(libtrace), err, sizeof(err)) != 0) {
		trace_set_err_out(libtrace, TRACE_ERR_INIT_FAILED, "%s", err);
		free(libtrace->format_data);
		libtrace->format_data = NULL;
		return -1;
	}

	return 0;
}

//XXX - can comment it out, left from duck.
static int odp_config_output(libtrace_out_t *libtrace, trace_option_output_t option, void *data)
{
	switch (option) 
	{
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
			trace_set_err_out(libtrace, TRACE_ERR_UNKNOWN_OPTION, "Unknown option");
			return -1;
	}
	assert(0);
}

static int odp_start_input(libtrace_t *libtrace) 
{
	int ret;

	if (libtrace->io)
		/* File already open */
		return 0;
	
	libtrace->io = trace_open_file(libtrace);
	if (!libtrace->io)
		return -1;

	//start pktio
        fprintf(stdout, "going to start pktio\n");
        ret = odp_pktio_start(FORMAT(libtrace)->pktio);
        if (ret != 0)
                fprintf(stderr, "Error: unable to start pktio\n");

        printf("  created pktio:%02i, queue mode\n default pktio%02i-INPUT queue\n",
                (int)FORMAT(libtrace)->pktio, (int)FORMAT(libtrace)->pktio);

	return 0;
}

static int odp_start_output(libtrace_out_t *libtrace) {
	OUTPUT->file = trace_open_file_out(libtrace, 
						OUTPUT->compress_type,
						OUTPUT->level,
						OUTPUT->fileflag);
	if (!OUTPUT->file) {
		return -1;
	}
	return 0;
}

static int odp_fin_input(libtrace_t *libtrace) 
{
	wandio_destroy(libtrace->io);

        odp_pktio_stop(FORMAT(libtrace)->pktio);
        odp_pktio_close(FORMAT(libtrace)->pktio);

	free(libtrace->format_data);

	return 0;
}

static int odp_fin_output(libtrace_out_t *libtrace) 
{
	wandio_wdestroy(OUTPUT->file);
	//XXX - we have stopped pktio already in odp_fin_input(). 
	//Probably there is a need to stop it also here, but probably not.

	free(libtrace->format_data);
	return 0;
}

//Converts a buffer containing a packet record into a libtrace packet
//should be called in odp_read_packet()
static int odp_prepare_packet(libtrace_t *libtrace, libtrace_packet_t *packet,
		void *buffer, libtrace_rt_types_t rt_type, uint32_t flags) 
{
	//XXX - do we need it?
        if (packet->buffer != buffer &&
                        packet->buf_control == TRACE_CTRL_PACKET) {
                free(packet->buffer);
        }

        if ((flags & TRACE_PREP_OWN_BUFFER) == TRACE_PREP_OWN_BUFFER) {
                packet->buf_control = TRACE_CTRL_PACKET;
        } else
                packet->buf_control = TRACE_CTRL_EXTERNAL; //XXX - we already set it in odp_read_packet()

        packet->buffer = buffer;
        packet->header = buffer;
	packet->payload = FORMAT(libtrace)->l2h;
	packet->type = rt_type;

	//XXX - looks strange, maybe remove it later
	if (libtrace->format_data == NULL) {
		if (odp_init_input(libtrace))
			return -1;
	}

	return 0;
}

static int odp_read_pack()
{
	int numbytes;
	odp_event_t ev;
	unsigned int processed_pkts = 0;

        fprintf(stdout, "odp_read_pack() start\n");

	while (1) 
	{
                /* Use schedule to get buf from any input queue. 
		   Waits infinitely for a new event with ODP_SCHED_WAIT param. */
                ev = odp_schedule(NULL, ODP_SCHED_WAIT);
                FORMAT(libtrace)->pkt = odp_packet_from_event(ev);
                if (!odp_packet_is_valid(FORMAT(libtrace)->pkt))
                        continue;

                //Returns pointer to the start of the layer 2 header
                FORMAT(libtrace)->l2h = (u_char *)odp_packet_l2_ptr(FORMAT(libtrace)->pkt, NULL);
		//uint32_t odp_packet_len(odp_packet_t pkt);
                numbytes = (int)odp_packet_len(FORMAT(libtrace)->pkt);
		processed_pkts++;

		//if trace stopped
		if (libtrace_halt)
			return READ_EOF;
		//XXX - move freeing packet to some other place and call it later
                //fprintf(stdout, "freeing packet #%d \n", processed_pkts);
	
        	fprintf(stdout, "odp_read_pack() exits\n");
		return numbytes;
	}

	/* We'll NEVER get here - but if we did it would be bad */
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

//So endless loop while no packets and return bytes read in case there is a packet
static int odp_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) 
{
//	unsigned int duck_size;
	uint32_t flags = 0;
	int numbytes = 0;
	
	//1. Set packet fields
	//XXX - check it later
	packet->buf_control = TRACE_CTRL_EXTERNAL;
	packet->type = TRACE_RT_DATA_ODP;

//	flags |= TRACE_PREP_OWN_BUFFER;

	//2. Read 1 packet from odp
	numbytes = odp_read_pack();
	if (numbytes == -1) 
	{
		trace_set_err(libtrace, errno, "Reading odp packet failed");
		return -1;
	}
	else if (numbytes == 0) {
		return 0;
	}
	else {
		trace_set_err(libtrace, TRACE_ERR_BAD_PACKET, "Truncated odp packet");
	}

	//3. Get pointer from packet and assign it to packet->buffer
	if (!packet->buffer || packet->buf_control == TRACE_CTRL_EXTERNAL) 
	{
		packet->buffer = FORMAT(libtrace)->pkt;	//XXX - we just copy pointer now - check it later
		//packet->buffer = malloc((size_t)LIBTRACE_PACKET_BUFSIZE); //XXX - 65536 - do we need malloc here?
                if (!packet->buffer) 
		{
                        trace_set_err(libtrace, errno, "Cannot allocate memory or have invalid pointer to packet");
                        return -1;
                }
        }


#if 0
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
#endif

	if (odp_prepare_packet(libtrace, packet, packet->buffer, packet->type, flags))
		return -1;
	
	return numbytes;
}

static void odp_fin_packet(libtrace_packet_t *packet)
{
	if (packet->buf_control == TRACE_CTRL_EXTERNAL) 
	{
                odp_packet_free(pkt);
		packet->buffer = NULL;
	}
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

static libtrace_linktype_t odp_get_link_type(
				const libtrace_packet_t *packet UNUSED) 
{
	return TRACE_TYPE_ETH;	//We have Ethernet for ODP and in DPDK.
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
        NULL,                           /* config_input - Sets value to some option */
        odp_start_input,	        /* start_input - Starts or unpauses an input trace (also opens file or device for reading)*/
        NULL,                           /* pause_input */
        odp_init_output,                /* init_output - Initialises an output trace using the capture format. */
        odp_config_output,             /* config_output */
        odp_start_output,              /* start_output */
        odp_fin_input,	               	/* fin_input - Stops capture input data.*/
        odp_fin_output,                /* fin_output */
        duck_read_packet,        	/* read_packet  - Reads the next packet from an input trace into the provided packet structure*/
        odp_prepare_packet,		/* prepare_packet - Converts a buffer containing a packet record into a libtrace packet. Used in read_packet*/
	odp_fin_packet,                 /* fin_packet */
        duck_write_packet,              /* write_packet - Write a libtrace packet to an output trace */
        odp_get_link_type,    		/* get_link_type - Returns the libtrace link type for a packet */
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
