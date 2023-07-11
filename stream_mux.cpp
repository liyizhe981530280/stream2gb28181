#include <string>
#include <memory>
#include <thread>
#include <iostream>

extern "C"
{
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/fifo.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
}
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mpeg-ps.h"
#include "rtp.h"

#define ADTS_HEADER_SIZE   (7)

typedef struct ADTSContext
{
	int write_adts;
	int objecttype;
	int sample_rate_index;
	int channel_conf;
}ADTSContext; 

using namespace std;

#define GETAVERROR(func, ret) {\
	char __tmp[1024];\
	av_strerror(ret, __tmp, sizeof(__tmp));\
	TOLOG("%s failed:%s\n", func, __tmp);\
}

static int interrupt_cb(void* ctx)
{

	int64_t* time = (int64_t*)ctx;
	int64_t cur = av_gettime();
	if ((cur - *time) >= 10 * 1000 * 1000) {

		return -1;
	}
	return 0;
}

static int openInput(AVFormatContext** inputContext, const char* inputfile, int64_t* lastReadPacktTime, int& audioIndex, int& videoIndex, int& dropAudio)
{
	*lastReadPacktTime = av_gettime();
	*inputContext = avformat_alloc_context();
	(*inputContext)->interrupt_callback.callback = interrupt_cb;
	(*inputContext)->interrupt_callback.opaque = lastReadPacktTime;

	int ret = avformat_open_input(inputContext, inputfile, NULL, NULL);
	if (ret < 0)
	{
		GETAVERROR("avformat_open_input", ret);
		return  ret;
	}

	ret = avformat_find_stream_info(*inputContext, nullptr);
	if (ret < 0)
	{
		GETAVERROR("avformat_find_stream_info", ret);
		return ret;
	}

	for (unsigned i = 0; i < (*inputContext)->nb_streams; i++)
	{
		if ((*inputContext)->streams[i]->codec->codec_type == AVMediaType::AVMEDIA_TYPE_VIDEO)
		{
			videoIndex = i;
			TOLOG("video index:%d----num--[%d]:den:[%d]\n", i, (*inputContext)->streams[i]->time_base.num, (*inputContext)->streams[i]->time_base.den);
		}
		else if ((*inputContext)->streams[i]->codec->codec_type == AVMediaType::AVMEDIA_TYPE_AUDIO)
		{
			TOLOG("audio index:%d----num--[%d]:den:[%d]\n", i, (*inputContext)->streams[i]->time_base.num, (*inputContext)->streams[i]->time_base.den);
			audioIndex = i;
		}
	}
	/*** 检查time_base
	 *   发现某些视频经常出现流的time_base和codec的time_base值是反的情况
	 *   这个逻辑可以纠正这个情况
	 */
	int check_count = 0;
	while(1){

		if(videoIndex != -1 && audioIndex != -1){
			float audio_start_pts = (float)(((*inputContext)->streams[audioIndex]->start_time) * av_q2d((*inputContext)->streams[audioIndex]->time_base));
			float video_start_pts = (float)(((*inputContext)->streams[videoIndex]->start_time) * av_q2d((*inputContext)->streams[videoIndex]->time_base));

			if(audio_start_pts-video_start_pts > 10.00 || audio_start_pts-video_start_pts < -10.00) // timebase error, reset timebase 
			{

				if(check_count == 1){

					TOLOG("drop audio\n");
					dropAudio = 1;

					break;
				}
				if((*inputContext)->streams[audioIndex]->time_base.den/(*inputContext)->streams[audioIndex]->time_base.num == 90000){
					TOLOG("reset stream time_base(codec tb) [%d,%d]\n",  (*inputContext)->streams[audioIndex]->codec->time_base.num, (*inputContext)->streams[audioIndex]->codec->time_base.den);

					(*inputContext)->streams[audioIndex]->time_base.num = (*inputContext)->streams[audioIndex]->codec->time_base.num;
					(*inputContext)->streams[audioIndex]->time_base.den = (*inputContext)->streams[audioIndex]->codec->time_base.den;
				}else{

					TOLOG("reset time_base [%d.%d]->[1,90000]\n", (*inputContext)->streams[audioIndex]->codec->time_base.num, (*inputContext)->streams[audioIndex]->codec->time_base.den);
					(*inputContext)->streams[audioIndex]->time_base.num = 1;
					(*inputContext)->streams[audioIndex]->time_base.den = 90000;
				}
				check_count++;
			}else{

				break;
			}
		}else{

			break;
		}
	}

	return ret;
}

AVPacket* readPacket(AVFormatContext* inputContext, int64_t* lastReadPacktTime)
{
	AVPacket* packet = new AVPacket;
	av_init_packet(packet);

	int ret = av_read_frame(inputContext, packet);
	if (ret >= 0)
	{
		*lastReadPacktTime = av_gettime();
		return packet;
	}
	else
	{
		av_packet_unref(packet);
		delete packet;
		return nullptr;
	}
}

int initAudioEncodeCodec(AVStream* inputStream, AVCodecContext** encodeContext)
{
	int ret = 0;
	AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!audioCodec) {

		TOLOG("avcodec_find_encoder failed");
		return -1;
	}
	*encodeContext = avcodec_alloc_context3(audioCodec);
	(*encodeContext)->codec = audioCodec;
	(*encodeContext)->sample_rate = inputStream->codecpar->sample_rate;
	(*encodeContext)->channel_layout = inputStream->codecpar->channel_layout;
	(*encodeContext)->channels = inputStream->codecpar->channels;
	if ((*encodeContext)->channel_layout == 0)
	{
		(*encodeContext)->channel_layout = av_get_default_channel_layout(inputStream->codecpar->channels);
	}
	(*encodeContext)->sample_fmt = audioCodec->sample_fmts[0];

	//if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
	(*encodeContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	(*encodeContext)->codec_tag = 0;
	ret = avcodec_open2((*encodeContext), audioCodec, 0);
	return ret;
}

int initAudioDecodeCodec(AVFormatContext* inputContext, AVCodecContext** decodeContext, int audioIndex)
{
	if (audioIndex == -1)
		return 1;
	AVCodec* pdec;
	int ret = av_find_best_stream(inputContext, AVMEDIA_TYPE_AUDIO, -1, -1, &pdec, 0);
	if (ret < 0)
	{
		GETAVERROR("av_find_best_stream", ret);
		return ret;
	}
	*decodeContext = avcodec_alloc_context3(pdec);
	avcodec_parameters_to_context(*decodeContext, inputContext->streams[audioIndex]->codecpar);
	if (inputContext->streams[audioIndex]->codec->codec_id == AV_CODEC_ID_PCM_ALAW || inputContext->streams[audioIndex]->codec->codec_id == AV_CODEC_ID_PCM_MULAW) {

		(*decodeContext)->sample_rate = 8000;
	}
	(*decodeContext)->time_base = inputContext->streams[audioIndex]->codec->time_base;
	ret = avcodec_open2(*decodeContext, pdec, NULL);
	return 0;
}

int initAudioFilter(AVFilterGraph** audioGraph, AVCodecContext* decodeContext, AVFilterContext** bfSrcCtxAudio, AVFilterContext** bfSinkCtxAudio, AVCodecContext* encode_audio)
{
	char args[512];
	int ret = 0;
	const AVFilter* abuffersrc = avfilter_get_by_name("abuffer");
	const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
	AVFilterInOut* outputs = avfilter_inout_alloc();
	AVFilterInOut* inputs = avfilter_inout_alloc();
	static const enum AVSampleFormat out_sample_fmts[] = { encode_audio->sample_fmt, AV_SAMPLE_FMT_NONE };
	static const int64_t out_channel_layouts[] = { encode_audio->channel_layout, -1 };
	static const int out_sample_rates[] = { encode_audio->sample_rate, -1 };
	const AVFilterLink* outlink;
	AVRational time_base = decodeContext->time_base;

	*audioGraph = avfilter_graph_alloc();
	if (!outputs || !inputs || !(*audioGraph)) {
		ret = AVERROR(ENOMEM);
		goto end;
	}

	/* buffer audio source: the decoded frames from the decoder will be inserted here. */
	if (!decodeContext->channel_layout)
		decodeContext->channel_layout = av_get_default_channel_layout(decodeContext->channels);
	snprintf(args, sizeof(args),
			"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%d",
			time_base.num, time_base.den, decodeContext->sample_rate,
			av_get_sample_fmt_name(decodeContext->sample_fmt), decodeContext->channel_layout);
	ret = avfilter_graph_create_filter(bfSrcCtxAudio, abuffersrc, "in",
			args, NULL, *audioGraph);
	TOLOG("%s\n", args);
	if (ret < 0) {
		GETAVERROR("avfilter_graph_create_filter", ret);
		goto end;
	}

	/* buffer audio sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(bfSinkCtxAudio, abuffersink, "out",
			NULL, NULL, *audioGraph);
	if (ret < 0) {
		GETAVERROR("avfilter_graph_create_filter", ret);
		goto end;
	}

	ret = av_opt_set_int_list(*bfSinkCtxAudio, "sample_fmts", out_sample_fmts, -1,
			AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		GETAVERROR("av_opt_set_int_list1", ret);
		goto end;
	}

	ret = av_opt_set_int_list(*bfSinkCtxAudio, "channel_layouts", out_channel_layouts, -1,
			AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		GETAVERROR("av_opt_set_int_list2", ret);
		goto end;
	}

	ret = av_opt_set_int_list(*bfSinkCtxAudio, "sample_rates", out_sample_rates, -1,
			AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		GETAVERROR("av_opt_set_int_list3", ret);
		goto end;
	}

	outputs->name = av_strdup("in");
	outputs->filter_ctx = *bfSrcCtxAudio;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	inputs->name = av_strdup("out");
	inputs->filter_ctx = *bfSinkCtxAudio;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	if ((ret = avfilter_graph_parse_ptr(*audioGraph, "anull",
					&inputs, &outputs, NULL)) < 0)
		goto end;

	if ((ret = avfilter_graph_config(*audioGraph, NULL)) < 0)
		goto end;

	//av_buffersink_set_frame_size(*bfSinkCtxAudio, 1024);
	av_buffersink_set_frame_size(*bfSinkCtxAudio, encode_audio->frame_size);
end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return ret;
}

AVFrame* DecodeAudio(AVPacket* packet, AVCodecContext* decodeContext)
{
	int ret = avcodec_send_packet(decodeContext, packet);
	if (ret < 0) return nullptr;

	av_packet_unref(packet);
	AVFrame* frame = av_frame_alloc();
	ret = avcodec_receive_frame(decodeContext, frame);
	if (ret >= 0)
	{
		return frame;
	}
	else
	{
		av_frame_free(&frame);
		return nullptr;
	}
}

AVPacket* EncodeAudio(AVFrame* frame, AVCodecContext* encodeContext)
{
	int ret = 0;

	ret = avcodec_send_frame(encodeContext, frame);
	av_frame_unref(frame);
	if (ret < 0) return nullptr;

	AVPacket* packet = new AVPacket;
	av_init_packet(packet);
	ret = avcodec_receive_packet(encodeContext, packet);
	if (ret >= 0) return packet;
	av_packet_unref(packet);
	delete packet;
	return nullptr;
}

static AVFrame* FilterFrame(AVFrame* frame, AVFilterContext* bfSrcCtxAudio, AVFilterContext* bfSinkCtxAudio)
{
	int ret = av_buffersrc_add_frame_flags(bfSrcCtxAudio, frame, AV_BUFFERSRC_FLAG_PUSH);
	if (ret < 0)
	{
		av_frame_unref(frame);
		return nullptr;
	}
	AVFrame* filterFrame = av_frame_alloc();
	ret = av_buffersink_get_frame(bfSinkCtxAudio, filterFrame);
	if (ret >= 0)
	{
		return filterFrame;
	}
	else
	{
		av_frame_free(&filterFrame);
		return nullptr;
	}
}


int getAudioCoder(AVFormatContext* inputContext, AVCodecContext*& decodeContext, AVCodecContext*& encodeContext, int audioIndex, AVFilterGraph*& audioGraph,
		AVFilterContext*& bfSrcCtxAudio, AVFilterContext*& bfSinkCtxAudio)
{
	int ret = initAudioDecodeCodec(inputContext, &decodeContext, audioIndex);
	if(ret != 0)
		return -1;
	if (decodeContext->codec_id == AV_CODEC_ID_AAC )
	{
		encodeContext = inputContext->streams[audioIndex]->codec;  
	}
	else {

		ret = initAudioEncodeCodec(inputContext->streams[audioIndex], &encodeContext);
		if (ret < 0)
		{
			return -1;

		}
		ret = initAudioFilter(&audioGraph, decodeContext, &bfSrcCtxAudio, &bfSinkCtxAudio, encodeContext); // audio transcode must use filter
		if (ret < 0)
		{
			return -1;
		}
	}

	return 0; 
}
#if 1
static int aac_decode_extradata(ADTSContext *adts, unsigned char *pbuf, int bufsize)
{
	int aot, aotext, samfreindex;
	int i, channelconfig;
	unsigned char *p = pbuf;
	if (!adts || !pbuf || bufsize<2)
	{
		return -1;
	}
	aot = (p[0]>>3)&0x1f;
	if (aot == 31)
	{
		aotext = (p[0]<<3 | (p[1]>>5)) & 0x3f;
		aot = 32 + aotext;
		samfreindex = (p[1]>>1) & 0x0f;
		if (samfreindex == 0x0f)
		{
			channelconfig = ((p[4]<<3) | (p[5]>>5)) & 0x0f;
		}
		else
		{
			channelconfig = ((p[1]<<3)|(p[2]>>5)) & 0x0f;
		}
	}
	else
	{
		samfreindex = ((p[0]<<1)|p[1]>>7) & 0x0f;
		if (samfreindex == 0x0f)
		{
			channelconfig = (p[4]>>3) & 0x0f;
		}
		else
		{
			channelconfig = (p[1]>>3) & 0x0f;
		}
	}

#ifdef AOT_PROFILE_CTRL
	if (aot < 2) aot = 2;
#endif
	adts->objecttype = aot-1;
	adts->sample_rate_index = samfreindex;
	adts->channel_conf = channelconfig;
	adts->write_adts = 1;
	return 0;
}

static int aac_set_adts_head(ADTSContext *acfg, unsigned char *buf, int size)
{
	unsigned char byte = 0;
	if (size < ADTS_HEADER_SIZE)
	{
		return -1;
	}

	buf[0] = 0xff;
	buf[1] = 0xf1;
	byte = 0;
	byte |= (acfg->objecttype & 0x03) << 6;
	byte |= (acfg->sample_rate_index & 0x0f) << 2;
	byte |= (acfg->channel_conf & 0x07) >> 2;
	buf[2] = byte;
	byte = 0;
	byte |= (acfg->channel_conf & 0x07) << 6;
	byte |= (ADTS_HEADER_SIZE + size) >> 11;
	buf[3] = byte;
	byte = 0;
	byte |= (ADTS_HEADER_SIZE + size) >> 3;
	buf[4] = byte;
	byte = 0;
	byte |= ((ADTS_HEADER_SIZE + size) & 0x7) << 5;
	byte |= (0x7ff >> 6) & 0x1f;
	buf[5] = byte;
	byte = 0;
	byte |= (0x7ff & 0x3f) << 2;
	buf[6] = byte;

	return 0;
}
#endif

static void* ps_alloc(void* /*param*/, size_t bytes)
{
	static char s_buffer[2 * 1024 * 1024];
	return s_buffer;
}

static void ps_free(void* /*param*/, void* /*packet*/)
{
	return;
}

RtpHandler * rtp_handler = NULL;
static int ps_write(void* param, int stream, void* packet, size_t bytes)
{

	if(rtp_handler == NULL){
		rtp_handler = (RtpHandler*)malloc(sizeof(RtpHandler));
		init_rtp(rtp_handler, 1234, 25, 96, 1472, "192.168.3.76", 10000);
		set_base_port(rtp_handler, 12900);
	}

	send_rtp(packet, bytes, rtp_handler, 0);
	if(param != NULL){
		fwrite(packet, bytes, 1, (FILE*)param) ? 0 : ferror((FILE*)param);
		fflush((FILE*)param);
	}

	return 1;
}

static char *av_fourcc_make_string(char *buf, uint32_t fourcc)
{
	int i;
	char *orig_buf = buf;
	size_t buf_size = AV_FOURCC_MAX_STRING_SIZE;

	for (i = 0; i < 4; i++) {
		const int c = fourcc & 0xff;
		const int print_chr = (c >= '0' && c <= '9') ||
			(c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z') ||
			(c && strchr(". -_", c));
		const int len = snprintf(buf, buf_size, print_chr ? "%c" : "[%d]", c);
		if (len < 0)
			break;
		buf += len;
		buf_size = buf_size > len ? buf_size - len : 0;
		fourcc >>= 8;
	}

	return orig_buf;
}

/**** printf input format ****/
static void dump_format(AVFormatContext* inputContext, int videoIndex, int audioIndex)
{
	if(videoIndex != -1 && inputContext->streams[videoIndex]->codec){

		AVStream* st = inputContext->streams[videoIndex];

		char videoInfo[4096] = "";
		snprintf(videoInfo, sizeof(videoInfo), "video:%s (%s)", avcodec_get_name(st->codec->codec_id),  avcodec_profile_name(st->codec->codec_id, st->codec->profile));

		if(st->codec->codec_tag){
			char codec_tag[128] = "";
			snprintf(videoInfo+strlen(videoInfo), sizeof(videoInfo)," (%s)", av_fourcc_make_string(codec_tag, st->codec->codec_tag));
		}else{
		
			snprintf(videoInfo+strlen(videoInfo), sizeof(videoInfo)," (%s)", "no codec tag");
		}

		char* pix_fomat = av_get_pix_fmt_name(st->codec->pix_fmt);
		snprintf(videoInfo+strlen(videoInfo), sizeof(videoInfo), " %s,", pix_fomat?pix_fomat:"unkown");

		snprintf(videoInfo+strlen(videoInfo), sizeof(videoInfo), " %dx%d,", st->codec->width, st->codec->height);

		int fps = st->avg_frame_rate.den && st->avg_frame_rate.num;
		int tbr = st->r_frame_rate.den && st->r_frame_rate.num;
		int tbn = st->time_base.den && st->time_base.num;

		snprintf(videoInfo+strlen(videoInfo), sizeof(videoInfo), " fps:%0.2f, tbr:%0.2f, tbn:%0.2f", fps?av_q2d(st->avg_frame_rate):0.0, tbr?av_q2d(st->r_frame_rate):0.0, tbn?1/av_q2d(st->time_base):0.0);	
		TOLOG("%s\n", videoInfo);
	}	

	if(audioIndex != -1 && inputContext->streams[audioIndex]->codec){
	
		AVStream* st = inputContext->streams[audioIndex];

		char audioInfo[4096] = "";
		snprintf(audioInfo, sizeof(audioInfo), "audio:%s (%s),", avcodec_get_name(st->codec->codec_id),  avcodec_profile_name(st->codec->codec_id, st->codec->profile));

		snprintf(audioInfo + strlen(audioInfo), sizeof(audioInfo), " %d Hz", st->codec->sample_rate);

		TOLOG("%s\n", audioInfo);
	}
}

int stream_mux(const char* filename)
{
	FILE* fp = NULL;
#ifdef DEBUG
	logfile = stdout;
	fp = fopen("out.mpg", "wb");
#endif

	ps_muxer_t* ps = NULL;
	ps_muxer_func_t ps_func;
	ps_func.alloc = ps_alloc;
	ps_func.write = ps_write;
	ps_func.free = ps_free;

	ps = ps_muxer_create(&ps_func, fp);

	int ps_video = -1, ps_audio = -1;

	AVFormatContext* inputContext = NULL;
	AVCodecContext* decodeContext = NULL; // audio 
	AVCodecContext* encodeContext = NULL; // audio
	AVFilterContext* bfSrcCtxAudio = NULL;   // audio filter
	AVFilterContext* bfSinkCtxAudio = NULL;  // audio filter
	AVFilterGraph* audioGraph = NULL;        // audio filter

	int audioIndex = -1;
	int videoIndex = -1;

	ADTSContext AdtsCtx;   // adts header
	memset(&AdtsCtx, 0, sizeof(ADTSContext));

	int audio_base, video_base; // 时间基倍数转换

	const AVBitStreamFilter* VideoAbsFilter = NULL; // AnnexB 滤镜
	AVBSFContext* VideoAbsFCtx = NULL;

	int64_t lastPacketTime = 0;       // 用作超时处理  

	// 是否丢弃音频帧，有的流video pts和audio pts不匹配，无法正常播放。需要丢弃音频帧
	int dropAudio = -1; 

	int ret = openInput(&inputContext, filename, &lastPacketTime, audioIndex, videoIndex, dropAudio);
	if (ret < 0)
	{
		goto End;
	}

	if (videoIndex == -1 && audioIndex == -1)
	{
		ret = -1;
		goto End;
	}

	dump_format(inputContext, videoIndex, audioIndex);

	if( audioIndex != -1 && dropAudio != 1 ) {

		if(inputContext->streams[audioIndex]->codec->codec_id != AV_CODEC_ID_AAC){
			if(getAudioCoder(inputContext, decodeContext, encodeContext, audioIndex, audioGraph, bfSrcCtxAudio, bfSinkCtxAudio) < 0) // 获取音频编解码器和滤镜
				goto End;
		}else{

			decodeContext = inputContext->streams[audioIndex]->codec;
		}

		ps_audio = ps_muxer_add_stream(ps, PSI_STREAM_AAC, NULL, 0); 
		audio_base = 90000*av_q2d(inputContext->streams[audioIndex]->time_base); // rtp输出的时间基是90000，需要做倍数转换
	} 

	if(videoIndex != -1){

		video_base = 90000*av_q2d(inputContext->streams[videoIndex]->time_base);     // rtp输出的时间基是90000，需要做倍数转换

		if(inputContext->streams[videoIndex]->codec->codec_id == AV_CODEC_ID_H264){

			VideoAbsFilter = av_bsf_get_by_name("h264_mp4toannexb");
			ps_video = ps_muxer_add_stream(ps, PSI_STREAM_H264, NULL, 0);

		}else if(inputContext->streams[videoIndex]->codec->codec_id == AV_CODEC_ID_H265){

			VideoAbsFilter = av_bsf_get_by_name("hevc_mp4toannexb");
			ps_video = ps_muxer_add_stream(ps, PSI_STREAM_H265, NULL, 0);

		}else{

			TOLOG("not 264 or 265, exit\n");
			goto End;
		}

		if( (ret = av_bsf_alloc(VideoAbsFilter, &VideoAbsFCtx)) != 0) // 创建视频滤镜
		{
			GETAVERROR("av_bsf_alloc", ret);
			goto End;
		}
		avcodec_parameters_copy(VideoAbsFCtx->par_in, inputContext->streams[videoIndex]->codecpar);

		if((ret = av_bsf_init(VideoAbsFCtx)) != 0 ) // 初始化滤镜
		{
			GETAVERROR("av_bsf_init", ret);
			goto End;
		}
	}

	while (true)
	{
		auto packet = readPacket(inputContext, &lastPacketTime);
		if (packet)
		{

			if (packet->stream_index == audioIndex)
			{

				if(dropAudio == 1)
					continue;

				packet->stream_index = audioIndex;

				if (decodeContext->codec_id == AV_CODEC_ID_AAC )
				{
					if(decodeContext->extradata && decodeContext->extradata_size != 0){ // add adts header

						unsigned char head[7];
						aac_decode_extradata(&AdtsCtx, decodeContext->extradata, decodeContext->extradata_size);
						aac_set_adts_head(&AdtsCtx, head, packet->size);

						unsigned char* buff = (unsigned char*)malloc(packet->data+7);
						memcpy(buff, head, 7);
						memcpy(buff+7, packet->data, packet->size);

						ps_muxer_input(ps, ps_audio, 1, packet->pts * audio_base, packet->dts * audio_base, buff, packet->size+7);

						free(buff);
					}

				}
				else {  //not aac 


					AVFrame* frame = DecodeAudio(packet, decodeContext ); // 解码
					if (frame)
					{
						frame->pts = frame->best_effort_timestamp;
						AVFrame* filterFrame = FilterFrame(frame, bfSrcCtxAudio, bfSinkCtxAudio); //滤镜
						if (filterFrame)
						{
							filterFrame->pts = filterFrame->best_effort_timestamp;
							AVPacket* packet1 = EncodeAudio(filterFrame, encodeContext); // 编码
							if (packet1)
							{
								packet1->stream_index = audioIndex;
								lastPacketTime = av_gettime();
								unsigned char head[7];
								if(encodeContext->extradata && encodeContext->extradata_size != 0){ // 加adts header,可在文件中直接播放

									aac_decode_extradata(&AdtsCtx, encodeContext->extradata, encodeContext->extradata_size);
									aac_set_adts_head(&AdtsCtx, head, packet1->size);
								}

								unsigned char* buff = (unsigned char*)malloc(packet1->data+7);
								memcpy(buff, head, 7);
								memcpy(buff+7, packet1->data, packet1->size);

								if(packet1->flags & AV_PKT_FLAG_KEY)
									ps_muxer_input(ps, ps_audio, 1, packet1->pts * audio_base, packet1->dts * audio_base, buff, packet1->size+7);
								else
									ps_muxer_input(ps, ps_audio, 0, packet1->pts * audio_base, packet1->dts * audio_base, buff, packet1->size+7);
								free(buff);
								av_packet_unref(packet1);
								continue;

							}
							av_frame_free(&filterFrame);
						}
						av_frame_free(&frame);
					} // frame
				} // else
			} // packet->stream_index == audioIndex
			else if (packet->stream_index == videoIndex)
			{

				av_bsf_send_packet(VideoAbsFCtx, packet);
				while ((ret = av_bsf_receive_packet(VideoAbsFCtx, packet)) == 0) // 转AnnexB,可在文件中直接播放
				{
					//fprintf(stderr, "key:[%x]--flags:[%x]---packet size:%d---%x-%x-%x-%x-%x\n", AV_PKT_FLAG_KEY, packet->flags, packet->size, packet->data[0], packet->data[1], packet->data[2],packet->data[3], packet->data[4]);
					if(packet->flags & AV_PKT_FLAG_KEY){
						ps_muxer_input(ps, ps_video, 1, packet->pts * video_base, packet->dts * video_base, packet->data, packet->size);
					}else
						ps_muxer_input(ps, ps_video, 0, packet->pts * video_base, packet->dts * video_base, packet->data, packet->size);

				}

				av_packet_unref(packet);
				delete packet;
			}
			else {

				TOLOG("unkown id:[%d], [%d], [%d]------\n", packet->stream_index, audioIndex, videoIndex);
				break;
			}
		}else{

			TOLOG("no data\n");
			break;
		}
	}
End:
	TOLOG("ffmpeg end:-----pid:%d\n",  getpid());

	if(rtp_handler)
		rtp_destroy(rtp_handler);

	if (encodeContext) { 
		avcodec_close(encodeContext);
		avcodec_free_context(&encodeContext);
	}
	if (decodeContext && decodeContext != inputContext->streams[audioIndex]->codec) { // 如果音频是aac，decodeContext就是输入上下文的codec。防止double free
		avcodec_close(decodeContext);
		avcodec_free_context(&decodeContext);
	}

	if(audioGraph)
		avfilter_graph_free(&audioGraph);
	if (inputContext) {
		avformat_close_input(&inputContext);
		avformat_free_context(inputContext);
	}
	if(VideoAbsFCtx)
		av_bsf_free(&VideoAbsFCtx);

	return 0;
}
int main(int argc, char** argv)
{
	string filename;
	if(argc == 1)
		filename = "WeChat_20220430105424.mp4";
	else
		filename = argv[1];

	stream_mux(filename.c_str());
}
