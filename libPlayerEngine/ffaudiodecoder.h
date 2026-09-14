#pragma once

#include "decoderbase.h"
#include "config.h"

#include <functional>

struct VideoState;
struct MediaParameters;
struct Frame;
struct Decoder;
struct FrameQueue;

extern "C"
{
#include "libavutil/rational.h"
	struct AVFrame;
	struct AVSubtitle;
}

class FFAudioDecoder : public DecoderBase
{
public:
	FFAudioDecoder();
	~FFAudioDecoder();
	void setConfigureAudioFilters(std::function<int(VideoState* is, const char* intput_afilters, int force_output_format)> f);

protected:
	bool run() override;
	/// 循环开始前调用（while 之前），子类可重写做准备工作
	bool onReady();
	/// 循环退出后调用（while 之后），子类可重写做清理工作
	void onFinish();

private:
	int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1, enum AVSampleFormat fmt2, int64_t channel_count2);
	
private:
	AVFrame* frame;
	Frame* af;
	AVRational tb;
	int got_frame = 0;
#if CONFIG_AVFILTER
	int last_serial = -1;
	int reconfigure;
#endif
	int ret = 0;
	std::function<int(VideoState* is, const char* intput_afilters, int force_output_format)> configure_audio_filters;
};
