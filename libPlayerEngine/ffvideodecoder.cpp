#include "ffvideodecoder.h"

#include "config.h"
#include "auto-run.hpp"
#include "mediaplayertypes.h"

extern "C"
{
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/rational.h"
#include "libavutil/dict.h"
#include "libavformat/avformat.h"
#include "libavcodec/packet.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/avfilter.h"
}

#define AV_NOSYNC_THRESHOLD 10.0

const struct TextureFormatEntry {
	enum AVPixelFormat format;
	int texture_fmt;
} sdl_texture_format_map[] = {
	{ AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
	{ AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
	{ AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
	{ AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
	{ AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
	{ AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
	{ AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
	{ AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
	{ AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
	{ AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
	{ AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
	{ AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
	{ AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
	{ AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
	{ AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
	{ AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
	{ AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
	{ AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
	{ AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
	{ AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

// 判断一帧是否为硬件解码帧（D3D11/DXVA2/CUDA/VAAPI...）。
// 硬件帧不能直接进入滤镜图：buffer 源会因为缺少 hw_frames_ctx 而返回 EINVAL(-22)，
// 必须先通过 av_hwframe_transfer_data() 转到系统内存。
static bool is_hw_frame(const AVFrame* frame)
{
	if (!frame || frame->format == AV_PIX_FMT_NONE)
	{
		return false;
	}

	const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((enum AVPixelFormat)frame->format);
	if (!desc)
	{
		return false;
	}

	return (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

FFVideoDecoder::FFVideoDecoder()
	: DecoderBase()
	, hw_decode_used(false)
{
}

FFVideoDecoder::~FFVideoDecoder()
{
}

void FFVideoDecoder::setUseHwDecode(int decode_used)
{
	hw_decode_used = decode_used;
}

void FFVideoDecoder::setHwFormat(AVPixelFormat hwformat)
{
	hw_format = hwformat;
}

bool FFVideoDecoder::run()
{
	// for hw by walker-WSH
	// 解码目标固定用 hw_frame：是否真的走硬解由 libavcodec 决定（avctx->hw_device_ctx 已经设置），
	// 不能依赖 hw_decode_used —— 该标志一旦同步滞后，硬件帧就会被当成普通帧直接送进滤镜图。
	AVFrame* decode_frame = hw_frame;
	int ret = get_video_frame(m_videoState, decode_frame);
	if (ret < 0)
		return false;
	if (!ret)
		return true;

	// for hw by walker-WSH ------------------------- start
	AVFrame* frame = decode_frame;

	RUN_WHEN_SECTION_END([=]() {
		av_frame_unref(sw_frame);
		av_frame_unref(hw_frame); });

	if (is_hw_frame(decode_frame))
	{
		// 硬件帧必须先转到系统内存，滤镜图只接受软件帧
		auto err = av_hwframe_transfer_data(sw_frame, decode_frame, 0);
		if (err != 0)
		{
			av_log(NULL, AV_LOG_ERROR, "av_hwframe_transfer_data failed, err=%d\n", err);
			return true;
		}

		av_frame_copy_props(sw_frame, decode_frame);
		frame = sw_frame;
	}
	// for hw by walker-WSH ------------------------- end

#if CONFIG_AVFILTER
	if (last_w != frame->width
		|| last_h != frame->height
		|| last_format != frame->format
		|| last_serial != m_videoState->viddec.pkt_serial
		|| last_vfilter_idx != m_videoState->vfilter_idx) {
		/* av_log_ffplay(NULL, AV_LOG_DEBUG,
			"Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
			last_w, last_h,
			(const char*)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
			frame->width, frame->height,
			(const char*)av_x_if_null(av_get_pix_fmt_name((enum AVPixelFormat)frame->format), "none"), is->viddec.pkt_serial);*/
		avfilter_graph_free(&graph);
		graph = avfilter_graph_alloc();
		if (!graph) {
			ret = AVERROR(ENOMEM);
			return false;
		}
		graph->nb_threads = m_parameters->filter_nbthreads;
		if ((ret = configure_video_filters(graph, m_videoState, !vfilters_list.empty() ? vfilters_list.c_str() : NULL, frame)) < 0) {
			return false;
		}
		filt_in = m_videoState->in_video_filter;
		filt_out = m_videoState->out_video_filter;
		last_w = frame->width;
		last_h = frame->height;
		last_format = (enum AVPixelFormat)frame->format;
		last_serial = m_videoState->viddec.pkt_serial;
		last_vfilter_idx = m_videoState->vfilter_idx;
		frame_rate = av_buffersink_get_frame_rate(filt_out);
	}

	ret = av_buffersrc_add_frame(filt_in, frame);
	if (ret < 0)
		return false;

	while (ret >= 0) {
		m_videoState->frame_last_returned_time = av_gettime_relative() / 1000000.0;

		ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
		if (ret < 0) {
			if (ret == AVERROR_EOF)
				m_videoState->viddec.finished = m_videoState->viddec.pkt_serial;
			ret = 0;
			break;
		}

		m_videoState->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - m_videoState->frame_last_returned_time;
		if (fabs(m_videoState->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
			m_videoState->frame_last_filter_delay = 0;
		tb = av_buffersink_get_time_base(filt_out);
#endif
		duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational(av_make_q(frame_rate.den, frame_rate.num))) : 0);
		pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
		//ret = queue_picture(m_videoState, frame, pts, duration, frame->pkt_pos, m_videoState->viddec.pkt_serial);
		ret = queue_picture(m_videoState, frame, pts, duration, 0, m_videoState->viddec.pkt_serial);
		av_frame_unref(frame);
#if CONFIG_AVFILTER
		if (m_videoState->videoq.serial != m_videoState->viddec.pkt_serial)
			break;
	}
#endif

	if (ret < 0)
		return false;
	return true;
}

bool FFVideoDecoder::onReady()
{
	// for hw by walker-WSH
	// 这两帧的生命周期是整个解码线程，必须在本函数之外（onFinish）释放；
	// 不能在这里用 RUN_WHEN_SECTION_END：该宏是 RAII，作用域结束（即 onReady 返回）
	// 就会立刻执行，导致 run() 里拿到的全是空指针。
	sw_frame = av_frame_alloc();
	hw_frame = av_frame_alloc();
    if (!sw_frame || !hw_frame)
    {
        av_frame_free(&sw_frame);
        av_frame_free(&hw_frame);
        return false;
    }

	VideoState* is = m_videoState;
	//AVFrame* frame = av_frame_alloc();
	int ret;
	tb = is->video_st->time_base;
	frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

#if CONFIG_AVFILTER
	AVFilterContext* filt_out = NULL, * filt_in = NULL;
	int last_w = 0;
	int last_h = 0;
	enum AVPixelFormat last_format = AV_PIX_FMT_NONE;
	int last_serial = -1;
	int last_vfilter_idx = 0;
#endif
	return true;
}

void FFVideoDecoder::onFinish()
{
    av_frame_free(&sw_frame);
    av_frame_free(&hw_frame);
#if CONFIG_AVFILTER
	avfilter_graph_free(&graph);
#endif
}

int FFVideoDecoder::configure_video_filters(AVFilterGraph* graph, VideoState* is, const char* vfilters, AVFrame* frame)
{
	enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
	char sws_flags_str[512] = "";
	char buffersrc_args[256];
	int ret;
	AVFilterContext* filt_src = NULL, * filt_out = NULL, * last_filter = NULL;
	AVCodecParameters* codecpar = is->video_st->codecpar;
	AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
	const AVDictionaryEntry* e = NULL;
	int nb_pix_fmts = 0;
	int i, j;

	if (is_hw_frame(frame))
	{
		// 防御性检查：硬件帧不能进入滤镜图（buffer 源会直接返回 EINVAL），
		// 必须先在解码线程里通过 av_hwframe_transfer_data() 转为软件帧
		av_log(NULL, AV_LOG_ERROR, "hardware frame %s is not allowed to enter filter graph\n",
			av_get_pix_fmt_name((enum AVPixelFormat)frame->format));
		return AVERROR(EINVAL);
	}

	for (i = 0; i < renderer_info.num_texture_formats; i++) {
		for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
			if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
				pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
				break;
			}
		}
	}
	pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

	while ((e = av_dict_iterate(sws_dict, e))) {
		if (!strcmp(e->key, "sws_flags")) {
			av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
		}
		else
			av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
	}
	if (strlen(sws_flags_str))
		sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

	graph->scale_sws_opts = av_strdup(sws_flags_str);

	snprintf(buffersrc_args, sizeof(buffersrc_args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		frame->width, frame->height, frame->format,
		is->video_st->time_base.num, is->video_st->time_base.den,
		codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
	if (fr.num && fr.den)
		av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

	if ((ret = avfilter_graph_create_filter(&filt_src,
		avfilter_get_by_name("buffer"),
		"ffplay_buffer", buffersrc_args, NULL,
		graph)) < 0)
		goto fail;

	/* 只分配、不初始化；否则初始化后再设置非 runtime 选项会返回 EINVAL(-22) */
	filt_out = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"), "ffplay_buffersink");
	if (!filt_out)
	{
		ret = AVERROR(ENOMEM);
		goto fail;
	}

	if ((ret = av_opt_set_array(filt_out, "pixel_formats", AV_OPT_SEARCH_CHILDREN, 0, nb_pix_fmts, AV_OPT_TYPE_PIXEL_FMT, pix_fmts)) < 0)
		goto fail;

	/* 先设置选项，最后再初始化 filter */
	if ((ret = avfilter_init_dict(filt_out, NULL)) < 0)
		goto fail;

	last_filter = filt_out;

	/* Note: this macro adds a filter before the lastly added filter, so the
	 * processing order of the filters is in reverse */
	#define INSERT_FILT(name, arg) do {                                          \
		AVFilterContext *filt_ctx;                                               \
																				 \
		ret = avfilter_graph_create_filter(&filt_ctx,                            \
										   avfilter_get_by_name(name),           \
										   "ffplay_" name, arg, NULL, graph);    \
		if (ret < 0)                                                             \
			goto fail;                                                           \
																				 \
		ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
		if (ret < 0)                                                             \
			goto fail;                                                           \
																				 \
		last_filter = filt_ctx;                                                  \
	} while (0)

	/*if (m_parameters->auto_rotate) {
		double theta = 0.0;
		int32_t* displaymatrix = NULL;
		AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
		if (sd)
			displaymatrix = (int32_t*)sd->data;
		if (!displaymatrix)
			displaymatrix = (int32_t*)av_stream_get_side_data(is->video_st, AV_PKT_DATA_DISPLAYMATRIX, NULL);
		theta = get_rotation(displaymatrix);

		if (fabs(theta - 90) < 1.0) {
			INSERT_FILT("transpose", "clock");
		}
		else if (fabs(theta - 180) < 1.0) {
			INSERT_FILT("hflip", NULL);
			INSERT_FILT("vflip", NULL);
		}
		else if (fabs(theta - 270) < 1.0) {
			INSERT_FILT("transpose", "cclock");
		}
		else if (fabs(theta) > 1.0) {
			char rotate_buf[64];
			snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
			INSERT_FILT("rotate", rotate_buf);
		}
	}*/

	if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
		goto fail;

	is->in_video_filter = filt_src;
	is->out_video_filter = filt_out;

fail:
	return ret;
}

int FFVideoDecoder::queue_picture(VideoState* is, AVFrame* src_frame, double pts, double duration, int64_t pos, int serial)
{
	Frame* vp;

	if (!(vp = frame_queue_peek_writable(&is->pictq)))
		return -1;

	vp->sar = src_frame->sample_aspect_ratio;
	vp->uploaded = 0;

	vp->width = src_frame->width;
	vp->height = src_frame->height;
	vp->format = src_frame->format;

	vp->pts = pts;
	vp->duration = duration;
	vp->pos = pos;
	vp->serial = serial;

	//set_default_window_size(vp->width, vp->height, vp->sar);

	av_frame_move_ref(vp->frame, src_frame);
	frame_queue_push(&is->pictq);
	return 0;
}

int FFVideoDecoder::decoder_decode_frame(Decoder* d, AVFrame* frame, AVSubtitle* sub)
{
	int ret = AVERROR(EAGAIN);

	for (;;) {
		if (d->queue->serial == d->pkt_serial) {
			do {
				if (d->queue->abort_request)
					return -1;

				switch (d->avctx->codec_type) {
				case AVMEDIA_TYPE_VIDEO:
					ret = avcodec_receive_frame(d->avctx, frame);
					if (ret >= 0) {
						if (decoder_reorder_pts == -1) {
							frame->pts = frame->best_effort_timestamp;
						}
						else if (!decoder_reorder_pts) {
							frame->pts = frame->pkt_dts;
						}
					}
					break;
				case AVMEDIA_TYPE_AUDIO:
					ret = avcodec_receive_frame(d->avctx, frame);
					if (ret >= 0) {
						AVRational tb = AVRational(av_make_q(1, frame->sample_rate));
						if (frame->pts != AV_NOPTS_VALUE)
							frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
						else if (d->next_pts != AV_NOPTS_VALUE)
							frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
						if (frame->pts != AV_NOPTS_VALUE) {
							d->next_pts = frame->pts + frame->nb_samples;
							d->next_pts_tb = tb;
						}
					}
					break;
				}
				if (ret == AVERROR_EOF) {
					d->finished = d->pkt_serial;
					avcodec_flush_buffers(d->avctx);
					return 0;
				}
				if (ret >= 0)
					return 1;
			} while (ret != AVERROR(EAGAIN));
		}

		do {
			if (d->queue->nb_packets == 0)
				SDL_CondSignal(d->empty_queue_cond);
			if (d->packet_pending) {
				d->packet_pending = 0;
			}
			else {
				int old_serial = d->pkt_serial;
				if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
					return -1;
				if (old_serial != d->pkt_serial) {
					avcodec_flush_buffers(d->avctx);
					d->finished = 0;
					d->next_pts = d->start_pts;
					d->next_pts_tb = d->start_pts_tb;
				}
			}
			if (d->queue->serial == d->pkt_serial)
				break;
			av_packet_unref(d->pkt);
		} while (1);

		if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
			int got_frame = 0;
			ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
			if (ret < 0) {
				ret = AVERROR(EAGAIN);
			}
			else {
				if (got_frame && !d->pkt->data) {
					d->packet_pending = 1;
				}
				ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
			}
			av_packet_unref(d->pkt);
		}
		else {
			if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
				//av_log_ffplay(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
				d->packet_pending = 1;
			}
			else {
				av_packet_unref(d->pkt);
			}
		}
	}
}

int FFVideoDecoder::configure_filtergraph(AVFilterGraph* graph, const char* filtergraph, AVFilterContext* source_ctx, AVFilterContext* sink_ctx)
{
	int ret, i;
	int nb_filters = graph->nb_filters;
	AVFilterInOut* outputs = NULL, * inputs = NULL;

	if (filtergraph) {
		outputs = avfilter_inout_alloc();
		inputs = avfilter_inout_alloc();
		if (!outputs || !inputs) {
			ret = AVERROR(ENOMEM);
			goto fail;
		}

		outputs->name = av_strdup("in");
		outputs->filter_ctx = source_ctx;
		outputs->pad_idx = 0;
		outputs->next = NULL;

		inputs->name = av_strdup("out");
		inputs->filter_ctx = sink_ctx;
		inputs->pad_idx = 0;
		inputs->next = NULL;

		if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
			goto fail;
	}
	else {
		if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
			goto fail;
	}

	/* Reorder the filters to ensure that inputs of the custom filters are merged first */
	for (i = 0; i < graph->nb_filters - nb_filters; i++)
		FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

	ret = avfilter_graph_config(graph, NULL);
fail:
	avfilter_inout_free(&outputs);
	avfilter_inout_free(&inputs);
	return ret;
}
