#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <netdb.h>
#include <stdlib.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <infiniband/verbs.h>
#include "sock.h"

char *msgs = "Hello client, I'm server\n";
char *msgc = "Hello server, I'm client\n";
enum {
	DEMO_RECV_WRID = 1,
	DEMO_SEND_WRID = 2
};

int page_size;
//send via TCP
struct demo_dest_info {
	uint16_t lid;			/* Local ID */
	uint32_t qpn;			/* Queue Pairs number */
	//uint32_t psn;			/* Packet Sequence number */
	//union ibv_gid gid;
	uint32_t rkey;			/* remote key, key for mr */
	uint32_t addr;			/* Buffer address, why needed? */
};

struct config_s {
	int 		size;
	char 		*dev_name;
	uint32_t 	tcp_port;
	int 		ib_port;
	int			rx_depth;
};
struct config_s config = {
	.dev_name 	= NULL,
	.size 		= 1024,
	.tcp_port 	= 18515,
	.ib_port 	= 1,
	.rx_depth	= 1
};

//sock
//device list has no need to be conbined with RDMA connection, put it in main
struct demo_context {
	struct ibv_device		**dev_list;		/* IB device list */
	struct demo_dest_info	remote_info;	/* values to build RDMA connections */ 
	struct ibv_port_attr	port_attr;		/* IB port attributes */
	struct ibv_context 		*ib_ctx;		/* device handle */
	struct ibv_pd			*pd;			/* Protect Domain handle */
	struct ibv_cq 			*cq;			/* Completion Queue handle */
	struct ibv_qp 			*qp;			/* Queue Pair handle */
	struct ibv_mr 			*mr;			/* Memory Reg handle */
	void 					*buf;			/* memory buffer handle */
	int						size;
};

void demo_context_setnull(struct demo_context *ctx){
	ctx->ib_ctx		= NULL;
	ctx->pd		 	= NULL;
	ctx->cq 		= NULL;
	ctx->qp 		= NULL;
	ctx->mr 		= NULL;
	ctx->buf 		= NULL;
}

struct demo_context* demo_context_init(void){

	struct demo_context 	*ctx;
	//struct ibv_device 		**dev_list;
	struct ibv_device 		*ib_dev;
	int						mr_flages;

	ctx = (struct demo_context *)malloc(sizeof(struct demo_context));
	if(!ctx) 
		return NULL;
	demo_context_setnull(ctx);

	ctx->dev_list = ibv_get_device_list(NULL);
	if(!ctx->dev_list) {
		fprintf(stderr, "Couldn't get IB device list.\n");
		return NULL;
	}

	if(!config.dev_name) {
		ib_dev = *(ctx->dev_list);
		if(!ib_dev){
			fprintf(stderr, "No IB device found.\n");
			return NULL;
		}
	} 
	else {
		int i;
		for (i = 0; ctx->dev_list[i]; i++){
			if (!strcmp(ibv_get_device_name(ctx->dev_list[i]), config.dev_name))
				break;
		}
		ib_dev = ctx->dev_list[i];
		if(!ib_dev) {
			fprintf(stderr, "IB device %s node found\n",config.dev_name);
			return NULL;
		}
	}

	ctx->ib_ctx = ibv_open_device(ib_dev);
	if(!ctx->ib_ctx) {
		fprintf(stderr, "Cannot open IB device %s\n", config.dev_name);
		return NULL;
	}

	// query port attributions, its lid is used later
	if(ibv_query_port(ctx->ib_ctx, config.ib_port, &ctx->port_attr)){
		fprintf(stderr, "Couldn't get local port's attribution\n");
		return NULL;
	}

	ctx->pd = ibv_alloc_pd(ctx->ib_ctx);
	if(!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return NULL;
	}

	//in pingpong, ctx->channel is passed if use_event
	ctx->cq = ibv_create_cq(ctx->ib_ctx, config.rx_depth, NULL, NULL, 0);
	if(!ctx->cq){
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	ctx->size = config.size;
	ctx->buf = malloc(roundup(config.size, page_size));
	if(!ctx->buf){
		fprintf(stderr, "Couldn't allocate work buffer.\n");
		return NULL;
	}
	memset(ctx->buf, 0, ctx->size);

	//now we want to exchange msg between both sides, so flag sets local & remote write
	//is it okay to put message in buffer later? not know yet
	strcpy(ctx->buf, "Hello world");

	mr_flages = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->size, mr_flages);
	if(!ctx->mr) {
		fprintf(stderr, "Couldn't regist MR\n");
		return NULL;
	}
	// lkey used to identify the buffer
	printf("MR registed with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",ctx->buf, ctx->mr->lkey, ctx->mr->rkey, mr_flages);

	//memset(&qp_attr, 0, sizeof(qp_attr));
	struct ibv_qp_init_attr qp_attr = {
		.qp_type 			= IBV_QPT_RC,	/* what? */
		.sq_sig_all 		= 1,			/* what? */
		.send_cq 			= ctx->cq,
		.recv_cq			= ctx->cq,
		.cap 				= {
			.max_send_wr  	= 1,
			.max_recv_wr  	= config.rx_depth,
			.max_send_sge 	= 1,
			.max_recv_sge 	= 1
		}
	};

	ctx->qp = ibv_create_qp(ctx->pd, &qp_attr);
	if(!ctx->qp) {
		fprintf(stderr, "Couldn't create QP\n");
		return NULL;
	}
	printf("QP created, QP number=0x%x\n", ctx->qp->qp_num);

	// move QP to init state
	// what does flags do? why pingpong's flag is 0
	struct ibv_qp_attr attr = {
		.qp_state 			= IBV_QPS_INIT,
		.pkey_index 		= 0,
		.port_num 			= config.ib_port,
		.qp_access_flags 	= 0
	};
	if (ibv_modify_qp(ctx->qp, &attr,
			IBV_QP_STATE		|
			IBV_QP_PKEY_INDEX	|
			IBV_QP_PORT			|
			IBV_QP_ACCESS_FLAGS)) {
		fprintf(stderr, "Couldn't modify QP to init state\n");
		return NULL;
	}

	return ctx;
}

int demo_post_recv(struct demo_context *ctx, int n){
	struct ibv_sge list = {
		.addr 	= ctx->buf,
		.length = ctx->size,
		.lkey 	= ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id 		= DEMO_RECV_WRID,
		.sg_list 	= &list,
		.num_sge 	= 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;
	for(i = 0; i < n; i++){
		if(ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;
	}
	return i;
}

int demo_post_send(struct demo_context *ctx){
	struct ibv_sge list = {
		.addr = ctx->buf,
		.length = ctx->size,
		.lkey = ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id = DEMO_SEND_WRID,
		.sg_list = &list,
		.num_sge = 1,
		.opcode = IBV_WR_SEND,
		.send_flags = IBV_SEND_SIGNALED,
		.wr.rdma = {
			.remote_addr = ctx->remote_info.addr,
			.rkey = ctx->remote_info.rkey
		}
	};
	struct ibv_send_wr *bad_wr;

	if(ibv_post_send(ctx->qp, &wr, &bad_wr)){
		fprintf(stderr, "Couldn't post send WR\n");
		return 1;
	}

	return 0;
}

int demo_connect_qp(struct demo_context *ctx, char *server_name){
	int sock;

	//if client
	if(server_name) 
		sock = sock_client_connect(server_name, config.tcp_port);
	else
		sock = sock_daemon_connect(config.tcp_port);
	if(sock < 0) {
		fprintf(stderr, "Couldn't built TCP connection to server %s, port %d\n", server_name, config.tcp_port);
		return 1;
	}

	struct demo_dest_info tmp;
	struct demo_dest_info local_info = {
		.addr 	= htonl(ctx->buf),
		.rkey 	= htonl(ctx->mr->rkey),
		.qpn 	= htonl(ctx->qp->qp_num),
		.lid 	= htons(ctx->port_attr.lid)
	};

	int sync_rst;
	sync_rst = sock_sync_data(sock, !server_name, sizeof(struct demo_dest_info), &local_info, &tmp);
	if(sync_rst < 0) {
		fprintf(stderr, "Couldn't exchange info via TCP\n");
		return 1;
	}

	struct demo_dest_info remote_info = {
		.addr 	= ntohl(tmp.addr),
		.rkey 	= ntohl(tmp.rkey),
		.qpn 	= ntohl(tmp.qpn),
		.lid 	= ntohs(tmp.lid)
	};

	ctx->remote_info = remote_info;

	printf("Remote address	= 0x%x\n", remote_info.addr);
	printf("Remote rkey = 0x%x\n", remote_info.rkey);
	printf("Remote QP number = 0x%x\n", remote_info.qpn);
	printf("Remote LID = 0x%x\n", remote_info.lid);

	if(sock_sync_ready(sock, !server_name)) {
		fprintf(stderr, "Couldn't sync\n");
		return 1;
	}
 
	close(sock);

	{
		//modify QP to RTR
		struct ibv_qp_attr attr = {
			.qp_state 			= IBV_QPS_RTR,
			.path_mtu 			= IBV_MTU_256,
			.dest_qp_num 		= remote_info.qpn,
			.rq_psn 			= 0,
			.max_dest_rd_atomic = 0,
			.min_rnr_timer 		= 12,
			.ah_attr 			= {
				.is_global 		= 0,
				.dlid 			= remote_info.lid,
				.sl 			= 0,
				.src_path_bits 	= 0,
				.port_num 		= config.ib_port
			}
		};

		int flags = IBV_QP_STATE				|
					IBV_QP_AV					|
					IBV_QP_PATH_MTU				|
					IBV_QP_DEST_QPN				|
					IBV_QP_RQ_PSN				|
					IBV_QP_MAX_DEST_RD_ATOMIC	|
					IBV_QP_MIN_RNR_TIMER;

		if(ibv_modify_qp(ctx->qp, &attr, flags)) {
			fprintf(stderr, "Couldn't modify QP to RTR\n");
			return 1;
		}
	}

	{
		//modify QP to RTS
		struct ibv_qp_attr attr = {
			.qp_state 		= IBV_QPS_RTS,
			.timeout 		= 12,
			.retry_cnt 		= 7,
			.rnr_retry 		= 0,
			.sq_psn 		= 0,
			.max_rd_atomic 	= 0
		};

		int flags = IBV_QP_STATE		|
					IBV_QP_TIMEOUT		|
					IBV_QP_RETRY_CNT	|
					IBV_QP_RNR_RETRY	|
					IBV_QP_SQ_PSN		|
					IBV_QP_MAX_QP_RD_ATOMIC;

		if(ibv_modify_qp(ctx->qp, &attr, flags)){
			fprintf(stderr, "Couldn't modify QP to RTR\n");
			return 1;
		}
	}

	return 0;
}

int demo_close_ctx(struct demo_context *ctx){
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ibv_close_device(ctx->ib_ctx)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	ibv_free_device_list(ctx->dev_list);
	free(ctx->buf);
	free(ctx);

	return 0;
}

void usage(const char *argv0){
	printf("Usage:\n");
	printf("  %s					start a server and wait for connection\n", argv0);
	printf("  %s <host>				connect to server at <host>\n", argv0);
	printf("\nOptions:\n");
	printf("  -s, --size=<size>		alloc <size> size of buffer (default 1024)\n");
	printf("  -p, --port=<port>		listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>	use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>	use port <port> of IB device (default 1)\n");
	printf("  -r, --rx_depth=<dep>	number of messages to send (default 1)\n");
}
int main(int argc, char *argv[]){
	struct ibv_device 		**dev_list;
	//struct ibv_device		*ib_dev;
	struct demo_context 	*ctx;
	//int						size 		= 1024;			/* buffer size */
	//int 					tcp_port 	= 18515;		/* TCP port */
	//int						ib_port 	= 1;			/* IB port */
	//char					*dev_name 	= NULL;		/* IV device */
	char					*server_name 	= NULL;		/* server name */
	while (1) {
		int c;
		struct option opt[] = {
			{ .name = "size",		.has_arg = 1, .val = 's' },
			{ .name = "port",		.has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",		.has_arg = 1, .val = 'd' },
			{ .name = "ib-port",	.has_arg = 1, .val = 'i' },
			{ .name = "rx_depth",	.has_arg = 1, .val = 'r' },
			{ .name = NULL,			.has_arg = 0, .val = '\0'}
		};

		c = getopt_long(argc, argv, "s:p:d:i", opt, NULL);

		if (c == -1)
			break;

		switch (c) {
		case 's':
			config.size = strtoul(optarg, NULL, 0);
			if(config.size < 0){
				usage(argv[0]);
				return 1;
			}
			break;

		case 'p':
			config.tcp_port = strtoul(optarg, NULL, 0);
			if(config.tcp_port < 0 || config.tcp_port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'd':
			config.dev_name = strdup(optarg);
			break;

		case 'i':
			config.ib_port = strtoul(optarg, NULL, 0);
			if(config.ib_port < 0) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'r':
			config.rx_depth = strtoul(optarg, NULL, 0);
			if(config.rx_depth < 0){
				usage(argv[0]);
				return 1;
			}
			break;

		default:
			usage(argv[0]);
			return 1;
		}	
	}

	// get server name
	if(optind == argc - 1)
		server_name = strdup(argv[optind]);
	else if(optind < argc) {
		usage(argv[0]);
		return 1;
	}

	// print config
	{
		printf(" ----------------------------------------------\n");
		if(config.dev_name) 
			printf(" Device name: \"%s\"\n",config.dev_name);
		else 
			printf(" Device name: No Dedault Device, use the first device found\n");
		printf(" IB port: %u\n",config.ib_port);
		if(server_name) 
			printf(" Server name: \"%s\"\n",server_name);
		printf(" TCP port: %u\n",config.tcp_port);
		printf(" ----------------------------------------------\n");
	}

	page_size = sysconf(_SC_PAGESIZE);

	//ctx = (struct demo_context*)malloc(sizeof(struct demo_context));
	//demo_context_setnull(ctx);

	ctx = demo_context_init();
	if (!ctx) {
		fprintf(stderr, "Couldn't initial context\n");
		return 1;
	}

	if (demo_post_recv(ctx, config.rx_depth) < config.rx_depth){
		fprintf(stderr, "Couldn't post receive work request\n");
		return 1;
	}

	if (demo_connect_qp(ctx, server_name)){
		fprintf(stderr, "Couldn't connect QP\n");
		return 1;
	}

	if(server_name){
		strcpy(ctx->buf, msgc);
		if(demo_post_send(ctx)) {
			fprintf(stderr, "Couldn't post send work request\n");
			return 1;
		}
		int j;
		for(j = 0; j < 2; j++) {
			struct ibv_wc wc[1];
			int ne, i;
			do {
				ne = ibv_poll_cq(ctx->cq, 1, wc);
				if(ne < 0){
					fprintf(stderr, "Couldn't poll CQ\n");
					return 1;
				}
			} while(ne < 1);

			for(i = 0; i < ne; i++){
				if(wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr, "RDMA communication failed\n");
					return 1;
				}
			}
		}
		printf("Message is %s\n", ctx->buf);
	}
	else {
		struct ibv_wc wc[1];
		int ne, i;
		do {
			ne = ibv_poll_cq(ctx->cq, 1, wc);
			if(ne < 0){
				fprintf(stderr, "Couldn't poll CQ\n");
				return 1;
			}
		} while(ne < 1);

		for(i = 0; i < ne; i++){
			if(wc[i].status != IBV_WC_SUCCESS) {
				fprintf(stderr, "RDMA communication failed\n");
				return 1;
			}
		}
		printf("Message is %s\n", ctx->buf);

		strcpy(ctx->buf, msgs);
		if(demo_post_send(ctx)) {
			fprintf(stderr, "Couldn't post send work request\n");
			return 1;
		}

	}


	//printf("Message is %s\n", ctx->buf);
	if(demo_close_ctx(ctx)){
		return 1;
	}

	//ibv_free_device_list(ctx->dev_list);
	free(config.dev_name);
	if(server_name) free(server_name);
	return 0;
}