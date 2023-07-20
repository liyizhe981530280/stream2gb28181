#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>
#include "rtp.h"
#include <unistd.h>
#include <sys/time.h>

FILE* logfile;
char date[64] = "";
int log_lock;

int init_rtp(RtpHandler* handler, int ssrc, int fps, int payloadType, int mtu, char* ip, short port)
{
	if(handler == NULL)
	{
		TOLOG("handler is null\n");
		return -1;
	}

	handler->rtp_header.csrc_count = 0;
	handler->rtp_header.extension = 0;
	handler->rtp_header.padding = 0;
	handler->rtp_header.version = 2;

	handler->rtp_header.marker = 0;
	handler->rtp_header.payload_type = payloadType;

	handler->rtp_header.seq = 0;
	handler->rtp_header.timestamp = 0;
	handler->rtp_header.ssrc = htonl(ssrc);

	handler->rtp_sock = -1;
	handler->base_port = -1;

	if(fps == -1)
		handler->fps = 25;
	else
		handler->fps = fps;

	if(payloadType == -1)
		handler->rtp_header.payload_type = 96; // gd28181 is 96
	else
		handler->rtp_header.payload_type = payloadType;

	handler->diff_ts = 90000/handler->fps;
	if(mtu == -1)
		handler->mtu = 1472; // default udp value
	else
		handler->mtu = mtu;

	strcpy(handler->ip, ip);
	handler->port = port;

	handler->data = (unsigned char*)malloc(mtu);

	handler->isInit = 1;

	handler->rtcp_header.rc = 0;	
	handler->rtcp_header.padding = 0;	
	handler->rtcp_header.version = 2;	
	handler->rtcp_header.pt = 200;	
	handler->rtcp_header.length = htons(6);
	handler->rtcp_header.ssrc = htonl(ssrc);
	handler->rtcp_header.send_packet_count = 0;
	handler->rtcp_header.send_bytes_count = 0;

	handler->rtp_addr.sin_family = AF_INET;
	handler->rtp_addr.sin_port = htons(handler->port);
	handler->rtp_addr.sin_addr.s_addr = inet_addr(handler->ip);

	handler->rtcp_addr.sin_family = AF_INET;
	handler->rtcp_addr.sin_port = htons(handler->port+1);
	handler->rtcp_addr.sin_addr.s_addr = inet_addr(handler->ip);
}

int send_rtcp(RtpHandler* handler, int is_tcp)
{
	int tmp_send_packet_count = handler->rtcp_header.send_packet_count;
	int tmp_send_bytes_count = handler->rtcp_header.send_bytes_count;

	handler->rtcp_header.send_packet_count = htonl(handler->rtcp_header.send_packet_count);
	handler->rtcp_header.send_bytes_count = htonl(handler->rtcp_header.send_bytes_count);

	if(is_tcp != 1){
		int ret = sendto(handler->rtcp_sock, &(handler->rtcp_header), sizeof(RtcpHeader), 0, (struct sockaddr *)&handler->rtcp_addr, (socklen_t)sizeof(struct sockaddr));
		if(ret <= 0){

			TOLOG("sendto error or peer close:%d--%s\n", ret, strerror(errno));
			return -1;
		}
	}else{

		unsigned short data_size;
		data_size = htons(sizeof(RtcpHeader));
		int ret = send(handler->rtcp_sock, &data_size, sizeof(short), MSG_WAITALL|MSG_NOSIGNAL);
		ret = send(handler->rtcp_sock, &(handler->rtcp_header), sizeof(RtcpHeader), MSG_WAITALL|MSG_NOSIGNAL);
		if(ret <= 0){

			TOLOG("send error or peer close:%d--%s\n", ret, strerror(errno));
			return -1;
		}
	}

	handler->rtcp_header.send_packet_count = tmp_send_packet_count;
	handler->rtcp_header.send_bytes_count = tmp_send_bytes_count;

	return 0;
}

static int create_socket(RtpHandler* handler, int is_tcp)
{

	if(is_tcp == 1){
		handler->rtp_sock = socket(AF_INET, SOCK_STREAM, 0);
		handler->rtcp_sock = socket(AF_INET, SOCK_STREAM, 0);
	}else{
		handler->rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
		handler->rtcp_sock = socket(AF_INET, SOCK_DGRAM, 0);
	}

	if(handler->rtp_sock < 0 || handler->rtcp_sock < 0)
	{
		TOLOG("create socket error:%s\n", strerror(errno));
		return -1;
	}

	if(handler->base_port > 0){ // 如果指定了client的端口

		struct sockaddr_in addr_rtp;
		addr_rtp.sin_family = AF_INET;
		addr_rtp.sin_port = htons(handler->base_port);
		addr_rtp.sin_addr.s_addr = inet_addr("0.0.0.0");
		bzero(&(addr_rtp.sin_zero), 8);
		if (bind(handler->rtp_sock, (struct sockaddr*)&addr_rtp, sizeof(struct sockaddr)) < 0) { 

			TOLOG("bind %d error:%s\n", handler->base_port, strerror(errno));
			return -1;
		}

		struct sockaddr_in addr_rtcp;
		addr_rtcp.sin_family = AF_INET;
		addr_rtcp.sin_port = htons(handler->base_port+1);
		addr_rtcp.sin_addr.s_addr = inet_addr("0.0.0.0");
		bzero(&(addr_rtp.sin_zero), 8);
		if (bind(handler->rtcp_sock, (struct sockaddr*)&addr_rtcp, sizeof(struct sockaddr)) < 0 && is_tcp != 1) {  // 基于tcp时，rtcp不做检查

			TOLOG("bind %d error:%s\n", handler->base_port+1, strerror(errno));
			return -1;
		}
	}

	if(is_tcp == 1){

		if(connect(handler->rtp_sock, (struct sockaddr*)&(handler->rtp_addr), sizeof(struct sockaddr)) < 0)
		{

			TOLOG("connect %s:%d failed:%s\n", handler->ip, handler->port, strerror(errno));
			return -1;
		}

		/* 基于tcp的国标流，基本都不使用rtcp，所以就算连不上也没关系*/
		if(connect(handler->rtcp_sock, (struct sockaddr*)&(handler->rtcp_addr), sizeof(struct sockaddr)) < 0)
		{

			TOLOG("connect %s:%d failed:%s\n", handler->ip, handler->port+1, strerror(errno));
			return 0;
		}

	}

	return 0;
}

static void update_rtcp(RtpHandler* handler, int send_bytes, int pos)
{
	if(pos == 0){

		if(handler->rtcp_header.send_packet_count == (unsigned int)0xffffffff ) // max value
			handler->rtcp_header.send_packet_count = 0;
		else
			handler->rtcp_header.send_packet_count ++;
	}

	if(((unsigned int)0xffffffff - handler->rtcp_header.send_bytes_count) < send_bytes ) // max value
		handler->rtcp_header.send_bytes_count = 0;
	else
		handler->rtcp_header.send_bytes_count += send_bytes;

	handler->rtcp_header.rtp_ts = handler->rtp_header.timestamp;
}

static int send_rtp_packet(RtpHandler* handler, int size, int is_tcp)
{
	if(is_tcp != 1){
		int ret = sendto(handler->rtp_sock, handler->data, size, 0, (struct sockaddr *)&handler->rtp_addr, (socklen_t)sizeof(struct sockaddr));
		if(ret <= 0){

			TOLOG("sendto error:%s\n", strerror(errno));
			return -1;
		}
	}else{

		unsigned short data_size;
		data_size = htons(size);
		int ret = send(handler->rtp_sock, &data_size, sizeof(short), MSG_WAITALL|MSG_NOSIGNAL);
		ret = send(handler->rtp_sock, handler->data, size, MSG_WAITALL|MSG_NOSIGNAL);
		if(ret <= 0){

			TOLOG("send error or peer close:%d--%s\n", ret, strerror(errno));
			return -1;
		}
	}

	return 0;
}

int send_rtp(void* data, size_t size, RtpHandler* handler, int is_tcp)
{
	if(handler == NULL )
	{
		TOLOG("handler is null\n");
		return -1;
	}

	if(handler->isInit != 1){

		TOLOG("handler is not init\n");
		return -1;
	}

	if(data == NULL || size <=0)
	{
		TOLOG("data is invalid\n");
		return -1;
	}

	if(handler->rtp_sock == -1){

		if(create_socket(handler, is_tcp) < 0)
			return -1;

		if(is_tcp == 1 && handler->mtu < 1460){ // 如果是tcp  修改mtu ,不麻烦协议栈分包了 
			handler->data = realloc(handler->data, 1460);
			handler->mtu = 1460;
		}
		handler->rtcp_counter = time(NULL);
	}

	int max_body = handler->mtu-sizeof(handler->rtp_header);// max rtp payload length

	if(((unsigned int)0xffffffff - handler->rtp_header.timestamp) < handler->diff_ts) // max value
		handler->rtp_header.timestamp = 0;
	else
		handler->rtp_header.timestamp +=  handler->diff_ts;

	handler->rtp_header.timestamp = htonl(handler->rtp_header.timestamp);

	struct timeval tv;
	gettimeofday(&tv, NULL);
	if(tv.tv_sec - handler->rtcp_counter >=30)  
	{
		handler->rtcp_header.ntp_ts_high32 = htonl(tv.tv_sec + 0x83AA7E80); // ntp_ts_high32表示1900-now的秒数, 0x83AA7E80是1900-1970的秒数
		handler->rtcp_header.ntp_ts_low32 = htonl(tv.tv_usec*1000);         // ms
		send_rtcp(handler, is_tcp);  // rtcp ,no need to check return value
		handler->rtcp_counter = tv.tv_sec;
	}

	int index = 0;
	while(1)
	{

		if(size-index <= 0) // no data
			break;

		int send_size = 0;

		if(size-index <= max_body){

			handler->rtp_header.marker = 1; // last packet in frame, set marker bit
			send_size = size-index;

		}else{
		
			send_size = max_body;
		}

		handler->rtp_header.seq = htons(handler->rtp_header.seq);
		memcpy(handler->data, &(handler->rtp_header), sizeof(handler->rtp_header));
		memcpy(handler->data+sizeof(handler->rtp_header), data+index, send_size);

		if(send_rtp_packet(handler, sizeof(handler->rtp_header)+send_size, is_tcp) < 0)
			return -1;

		handler->rtp_header.seq = ntohs(handler->rtp_header.seq);

		if(handler->rtp_header.seq == 65535)
			handler->rtp_header.seq = 0;
		else
			handler->rtp_header.seq++;

		update_rtcp(handler, send_size+ sizeof(handler->rtp_header), index);

		index += send_size;
		handler->rtp_header.marker = 0;

	}

	handler->rtp_header.timestamp = ntohl(handler->rtp_header.timestamp);

	return 0;
}

int rtp_destroy(RtpHandler* handler)
{
	if(handler == NULL)
		return 0;

	if(handler->rtp_sock != -1)
	{
		close(handler->rtp_sock);
		handler->rtp_sock = -1;
	}

	if(handler->rtcp_sock != -1)
	{
		close(handler->rtcp_sock);
		handler->rtcp_sock = -1;
	}

	if(handler->data != NULL)
	{
		free(handler->data);
		handler->data = NULL;
	}

	return 0;
}

int set_base_port(RtpHandler* handler, short port)
{
	handler->base_port = port;
}
