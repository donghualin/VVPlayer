#pragma once

#include "decoderbase.h"

#include <string>

struct VideoState;
struct MediaParameters;
struct Frame;
struct FrameQueue;
struct Clock;
struct PacketQueue;

extern "C"
{
#include "libavutil/pixfmt.h"
#include "libavutil/rational.h"
#include "SDL2/SDL_render.h"
	struct AVFrame;
	struct AVFilterGraph;
	struct AVSubtitle;
	struct AVFilterContext;
	struct AVRational;
	struct AVDictionary;
	struct AVPacket;
}

struct Decoder;

class FFVideoDecoder : public DecoderBase
{
public:
	FFVideoDecoder();
	~FFVideoDecoder();
	void setUseHwDecode(int decode_used);
	void setHwFormat(AVPixelFormat hwformat);

protected:
	bool run() override;
	/// 循环开始前调用（while 之前），子类可重写做准备工作
	bool onReady();
	/// 循环退出后调用（while 之后），子类可重写做清理工作
	void onFinish();

private:
	int configure_video_filters(AVFilterGraph* graph, VideoState* is, const char* vfilters, AVFrame* frame);
	int queue_picture(VideoState* is, AVFrame* src_frame, double pts, double duration, int64_t pos, int serial);
	int decoder_decode_frame(Decoder* d, AVFrame* frame, AVSubtitle* sub);
	int configure_filtergraph(AVFilterGraph* graph, const char* filtergraph, AVFilterContext* source_ctx, AVFilterContext* sink_ctx);
	
private:
	int hw_decode_used;
	AVFrame* sw_frame;
	AVFrame* hw_frame;
	enum AVPixelFormat hw_format = AVPixelFormat::AV_PIX_FMT_NONE;
	int last_w = 0;
	int last_h = 0;
	enum AVPixelFormat last_format = AV_PIX_FMT_NONE;
	int last_serial = -1;
	int last_vfilter_idx = 0;
	AVFilterGraph* graph = NULL;
	std::string vfilters_list = ""; // such as "setpts=PTS*0.5,vflip"
	AVFilterContext* filt_out = NULL, * filt_in = NULL;
	AVRational frame_rate;
	AVRational tb;
	double duration;
	double pts;
	SDL_RendererInfo renderer_info = { 0 };
	AVDictionary* sws_dict = NULL;
	int decoder_reorder_pts = -1;
};
