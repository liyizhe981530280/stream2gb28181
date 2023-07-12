#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>

/****  only support little endian  ****/
typedef struct _RtpHeader{
	/* byte 0 */
	unsigned char csrc_count:4;
	unsigned char extension:1;
	unsigned char padding:1;
	unsigned char version:2;

	/* byte 1 */
	unsigned char payload_type:7;
	unsigned char marker:1;

	/* bytes 2,3 */
	unsigned short seq;

	/* bytes 4-7 */
	unsigned int timestamp;

	/* bytes 8-11 */
	unsigned int ssrc;

}RtpHeader;

typedef struct _RtcpHeader{

	/* byte 0*/
	unsigned char rc:5;
	unsigned char padding:1;
	unsigned char version:2;

	/* byte 1*/
	unsigned char pt;

	/* byte 2-3*/
	unsigned short length;

	unsigned int ssrc;
	
	unsigned int ntp_ts_high32;

	unsigned int ntp_ts_low32;

	unsigned int rtp_ts;

	unsigned int send_packet_count;

	unsigned int send_bytes_count;

}RtcpHeader;


enum plType{

	PCMU=0,
	PCMA = 8,
	PL_H264 = 96,
	PL_PS = 96,
};

typedef struct _RtpHandler
{

	int rtp_sock;
	int rtcp_sock;

	int fps;
	int mtu;
	unsigned int diff_ts;
	unsigned char* data;
	char ip[32];         // 接收端的ip
	short port;          // 接收端端口ip 
	time_t rtcp_counter; // 30s发送一个rtcp包

	int isInit;
	
	struct sockaddr_in rtp_addr;
	struct sockaddr_in rtcp_addr;

	short base_port;    // 本地绑定的端口
	RtpHeader rtp_header;
	RtcpHeader rtcp_header;
	_RtpHandler()
	{
		isInit = 0;
	}

}RtpHandler;

/**
   发送rtp数据
   data: 媒体数据
   size：数据长度
   handler：句柄
   is_tcp： 0：基于udp的rtp，1：基于tcp
**/
int send_rtp(void* data, size_t size, RtpHandler* handler, int is_tcp);
/**
	初始化rtphandler
	handler：句柄
	ssrc：rtp协议中的ssrc
	fps：帧率
	payloadType：国标流在rtp中的负载是96
	mtu：最大传输单元
	ip：接收方的ip
	port: 接收放的port
**/
int init_rtp(RtpHandler* handler, int ssrc, int fps, int payloadType, int mtu, char* ip, short port);

/**
	设置发送rtp包的本地端口
	rtcp是port+1
**/
int set_base_port(RtpHandler* handler, short port);
/**
释放handler
**/
int rtp_destroy(RtpHandler* handler);

extern FILE* logfile;
extern char date[64];
extern int log_lock;

#define TOLOG(_format, ...) \
{\
	while(log_lock!=0) usleep(10*1000); \
	log_lock = 1;\
	if(logfile == stdout)\
	{\
		fprintf(logfile, "[%s:%d]--" _format "", __FILE__, __LINE__, ##__VA_ARGS__);\
	}else{\
		time_t _now_time;\
		struct tm* _info;\
		char _buffer[80] = "";\
		time(&_now_time);\
		_info = localtime(&_now_time);\
		strftime(_buffer, 80, "%Y-%m-%d %H:%M:%S", _info);\
		char* _pos = strchr(_buffer, ' ');\
		if (_pos != NULL) {\
			if(strlen(date) == 0){\
				strncpy(date, _buffer,  _pos - _buffer);\
			}else{\
				if(strncmp(date, _buffer,  _pos - _buffer) != 0){\
					fclose(logfile);logfile = NULL;\
					memset(date, 0x00, sizeof(date));\
					strncpy(date, _buffer,  _pos - _buffer);\
				}\
			}\
		}\
		if(logfile == NULL){\
			char _file[128] = "";\
			sprintf(_file, "%s_download.txt", date);\
			logfile = fopen(_file, "a+");\
		}\
		if(logfile){\
			fprintf(logfile, "[%s:%d]--%s----" _format "", __FILE__, __LINE__, _buffer, ##__VA_ARGS__);\
			fflush(logfile);\
		}\
	}\
	log_lock = 0;\
}


