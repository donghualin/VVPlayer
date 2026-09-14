#include "ffaudiodecoder.h"
#include "mediaplayertypes.h"

extern "C"
{
#include "libavutil/frame.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include "libavutil/bprint.h"
}

FFAudioDecoder::FFAudioDecoder()
	: DecoderBase()
{
}

FFAudioDecoder::~FFAudioDecoder()
{
}

bool FFAudioDecoder::run()
{
	if (!m_parameters)
	{
		return false;
	}
    // ret 是成员变量且被 av_buffersink_get_frame_flags 复用：上一轮把缓冲取空后
    // 会留下 AVERROR(EAGAIN)，所以这里不能照搬 ffplay 的 if (ret < 0) goto the_end
    // 语义，否则音频解码线程在处理完第一帧后就直接退出（外部表现就是完全没有声音）。
    // 每轮重新起算，解码线程只在 decoder_decode_frame 报致命错误（abort）时结束。
    ret = 0;

	if ((got_frame = decoder_decode_frame(&m_videoState->auddec, frame, NULL)) < 0)
		return false;

	if (got_frame) {
		tb = AVRational(av_make_q(1, frame->sample_rate));

#if CONFIG_AVFILTER
		reconfigure =
			cmp_audio_fmts(m_videoState->audio_filter_src.fmt, m_videoState->audio_filter_src.ch_layout.nb_channels,
				(enum AVSampleFormat)frame->format, frame->ch_layout.nb_channels) ||
			av_channel_layout_compare(&m_videoState->audio_filter_src.ch_layout, &frame->ch_layout) ||
			m_videoState->audio_filter_src.freq != frame->sample_rate ||
			m_videoState->auddec.pkt_serial != last_serial;

		if (reconfigure) {
			char buf1[1024], buf2[1024];
			av_channel_layout_describe(&m_videoState->audio_filter_src.ch_layout, buf1, sizeof(buf1));
			av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
			/*av_log_ffplay(NULL, AV_LOG_DEBUG,
				"Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
				is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
				frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name((enum AVSampleFormat)frame->format), buf2, is->auddec.pkt_serial);
			*/

			m_videoState->audio_filter_src.fmt = (enum AVSampleFormat)frame->format;
			ret = av_channel_layout_copy(&m_videoState->audio_filter_src.ch_layout, &frame->ch_layout);
			if (ret < 0)
				return false;
			m_videoState->audio_filter_src.freq = frame->sample_rate;
			last_serial = m_videoState->auddec.pkt_serial;

			if ((ret = configure_audio_filters(m_videoState, m_parameters->audio_filter.empty() ? NULL : m_parameters->audio_filter.c_str(), 1)) < 0)
				return false;
		}

		if ((ret = av_buffersrc_add_frame(m_videoState->in_audio_filter, frame)) < 0)
			return false;

		while ((ret = av_buffersink_get_frame_flags(m_videoState->out_audio_filter, frame, 0)) >= 0) {
			tb = av_buffersink_get_time_base(m_videoState->out_audio_filter);
#endif
			if (!(af = frame_queue_peek_writable(&m_videoState->sampq)))
				return false;

			af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
			af->pos = 0; // frame->pkt_pos;
			af->serial = m_videoState->auddec.pkt_serial;
			af->duration = av_q2d(AVRational(av_make_q(frame->nb_samples, frame->sample_rate)));


#if defined(DEBUG_SYNC)
			//    av_log_ffplay(NULL, AV_LOG_INFO, "----- push audio , % lf \n", af->pts);
#endif

			av_frame_move_ref(af->frame, frame);
			frame_queue_push(&m_videoState->sampq);

#if CONFIG_AVFILTER
			if (m_videoState->audioq.serial != m_videoState->auddec.pkt_serial)
				break;
		}
		if (ret == AVERROR_EOF)
			m_videoState->auddec.finished = m_videoState->auddec.pkt_serial;
#endif
	}
	return true;
}

bool FFAudioDecoder::onReady()
{
	if (!m_videoState)
	{
		return false;
	}
	frame = av_frame_alloc();
	if (!frame)
		return false;

	return true;
}

void FFAudioDecoder::onFinish()
{
#if CONFIG_AVFILTER
	avfilter_graph_free(&m_videoState->agraph);
#endif
	av_frame_free(&frame);
}

void FFAudioDecoder::setConfigureAudioFilters(std::function<int(VideoState* is, const char* intput_afilters, int force_output_format)> f)
{
	configure_audio_filters = f;
}

int FFAudioDecoder::cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1, enum AVSampleFormat fmt2, int64_t channel_count2)
{
	/* If channel count == 1, planar and non-planar formats are the same */
	if (channel_count1 == 1 && channel_count2 == 1)
		return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
	else
		return channel_count1 != channel_count2 || fmt1 != fmt2;
}
