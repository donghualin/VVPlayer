// 注意：cmdutils.h 在 extern "C" 之外包含了 libswscale/swscale.h，而该头文件自身没有
// C 链接保护。必须在这里（任何其他头文件之前）先以 C 链接包含一次，否则 sws_* 会被
// 声明成 C++ 名称修饰，链接时报无法解析的外部符号。
extern "C"
{
#include "libswscale/swscale.h"
}

#include "ffdemuxer.h"
#include "cmdutils.h"
#include "ffaudiodecoder.h"
#include "ffvideodecoder.h"
#include "ffsubtitledecoder.h"
#include "config_components.h"

extern "C"
{
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"
#include "libavutil/macros.h"
#include "libavutil/channel_layout.h"
#include "libavutil/hwcontext.h"
#include "libavutil/imgutils.h"
#include "libavutil/pixdesc.h"
#include "libavformat/avformat.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/avfilter.h"
#include "libswresample/swresample.h"
}

static const auto min_hw_image_size = 1920 * 1080;
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30
#define AUDIO_DIFF_AVG_NB 20
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define AV_NOSYNC_THRESHOLD 10.0
#define SAMPLE_CORRECTION_PERCENT_MAX 10
// 以下宏对齐样例 ffplay.cpp，供视频帧消费（video_refresh）使用
#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001
#define REFRESH_RATE 0.01

FFDemuxer::FFDemuxer()
	: m_parameters(nullptr)
	, m_videoState(nullptr)
	, m_format_context(nullptr)
	, startup_volume(100)
	, m_audioDecoder(new FFAudioDecoder())
	, m_videoDecoder(new FFVideoDecoder())
	, m_subtitleDecoder(new FFSubtitleDecoder())
	, wait_mutex(SDL_CreateMutex())
{
	m_audioDecoder->setConfigureAudioFilters([this](VideoState* is, const char* intput_afilters, int force_output_format)->int 
	{
		return configure_audio_filters(is, intput_afilters, force_output_format);
	});
}

FFDemuxer::~FFDemuxer()
{
	if (m_videoState)
	{
		av_free(m_videoState);
	}
}

VideoState* FFDemuxer::videoState() const
{
	return m_videoState;
}

bool FFDemuxer::startPlay(MediaParameters* parameters)
{
	m_parameters = parameters;
	//  启动线程
	start();
	/*m_audioDecoder->start();
	m_videoDecoder->start();
	m_subtitleDecoder->start();
	*/
	m_videoDecoder->setMediaParameters(parameters);
	m_videoDecoder->setGetMasterClock([this](VideoState* is) { return get_master_clock(is); });
	// 注意：这里不能下发硬件解码标志。startPlay() 在视频流打开之前执行，
	// 此时 hw_decode_used 还是初值 false，一旦下发就会把真实结果覆盖掉，
	// 导致解码线程把 libavcodec 输出的硬件帧当成软件帧直接送进滤镜图。
	// 正确的下发时机在 stream_component_open() 的 AVMEDIA_TYPE_VIDEO 分支：解码线程启动之前。
	m_audioDecoder->setMediaParameters(parameters);
	m_audioDecoder->setGetMasterClock([this](VideoState* is) { return get_master_clock(is); });
	return true;
}

void FFDemuxer::closeStream()
{
	if (!m_videoState)
	{
		return;
	}

}

bool FFDemuxer::run()
{
	if (m_videoState->abort_request)
		return false;

	if (m_videoState->paused != m_videoState->last_paused) {
		m_videoState->last_paused = m_videoState->paused;
		if (m_videoState->paused)
			m_videoState->read_pause_return = av_read_pause(ic);
		else
			av_read_play(ic);
	}
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
	if (m_videoState->paused &&
		(!strcmp(ic->iformat->name, "rtsp") ||
			(ic->pb && !strncmp(m_parameters->file_name.c_str(), "mmsh:", 5)))) {
		/* wait 10 ms to avoid trying to get another packet */
		/* XXX: horrible */
		SDL_Delay(10);
		return true;
	}
#endif
	if (m_videoState->seek_req) {
		int64_t seek_target = m_videoState->seek_pos;
		int64_t seek_min = m_videoState->seek_rel > 0 ? seek_target - m_videoState->seek_rel + 2 : INT64_MIN;
		int64_t seek_max = m_videoState->seek_rel < 0 ? seek_target - m_videoState->seek_rel - 2 : INT64_MAX;
		// FIXME the +-2 is due to rounding being not done in the correct direction in generation
		//      of the seek_pos/seek_rel variables

		int ret = avformat_seek_file(m_videoState->ic, -1, seek_min, seek_target, seek_max, m_videoState->seek_flags);
		if (ret < 0) {
			assert(false);
			//av_log_ffplay(NULL, AV_LOG_ERROR, "%s: error while seeking\n", is->ic->url);
		} else {
			eof_happened = false;

			if (m_videoState->audio_stream >= 0)
				packet_queue_flush(&m_videoState->audioq);
			if (m_videoState->subtitle_stream >= 0)
				packet_queue_flush(&m_videoState->subtitleq);
			if (m_videoState->video_stream >= 0)
				packet_queue_flush(&m_videoState->videoq);
			if (m_videoState->seek_flags & AVSEEK_FLAG_BYTE) {
				set_clock(&m_videoState->extclk, NAN, 0);
			}
			else {
				set_clock(&m_videoState->extclk, seek_target / (double)AV_TIME_BASE, 0);
			}
		}
		m_videoState->seek_req = 0;
		m_videoState->queue_attachments_req = 1;
		m_videoState->eof = 0;
		if (m_videoState->paused)
			step_to_next_frame(m_videoState);
	}
	if (m_videoState->queue_attachments_req) {
		if (m_videoState->video_st && m_videoState->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
			if (av_packet_ref(pkt, &m_videoState->video_st->attached_pic) < 0)
				return false;

			packet_queue_put(&m_videoState->videoq, pkt);
			packet_queue_put_nullpacket(&m_videoState->videoq, pkt, m_videoState->video_stream);
		}
		m_videoState->queue_attachments_req = 0;
	}

	/* if the queue are full, no need to read more */
	if (infinite_buffer < 1 &&
		(m_videoState->audioq.size + m_videoState->videoq.size + m_videoState->subtitleq.size > MAX_QUEUE_SIZE
			|| (stream_has_enough_packets(m_videoState->audio_st, m_videoState->audio_stream, &m_videoState->audioq) &&
				stream_has_enough_packets(m_videoState->video_st, m_videoState->video_stream, &m_videoState->videoq) &&
				stream_has_enough_packets(m_videoState->subtitle_st, m_videoState->subtitle_stream, &m_videoState->subtitleq)))) {
		/* wait 10 ms */
		SDL_LockMutex(wait_mutex);
		SDL_CondWaitTimeout(m_videoState->continue_read_thread, wait_mutex, 10);
		SDL_UnlockMutex(wait_mutex);
		return true;
	}
	if (!m_videoState->paused &&
		(!m_videoState->audio_st || (m_videoState->auddec.finished == m_videoState->audioq.serial && frame_queue_nb_remaining(&m_videoState->sampq) == 0)) &&
		(!m_videoState->video_st || (m_videoState->viddec.finished == m_videoState->videoq.serial && frame_queue_nb_remaining(&m_videoState->pictq) == 0))) {
		if (m_parameters->loop != 1 && (!m_parameters->loop || --m_parameters->loop)) {
			/*auto cb = event_cb.lock();
			if (cb) {
				cb->on_player_restart();
			}*/
			stream_seek(m_videoState, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
		}
// 		else if (autoexit) {
// 			auto cb = event_cb.lock();
// 			if (cb) {
// 				cb->on_player_auto_exit();
// 			}
// 			ret = AVERROR_EOF;
// 			goto fail;
// 		}
// 		else {
// 			if (!eof_happened) {
// 				eof_happened = true;
// 				auto cb = event_cb.lock();
// 				if (cb) {
// 					cb->on_stream_eof();
// 				}
// 			}
// 		}
	}
	int ret = av_read_frame(ic, pkt);
	if (ret < 0) {
		if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !m_videoState->eof) {
			if (m_videoState->video_stream >= 0)
				packet_queue_put_nullpacket(&m_videoState->videoq, pkt, m_videoState->video_stream);
			if (m_videoState->audio_stream >= 0)
				packet_queue_put_nullpacket(&m_videoState->audioq, pkt, m_videoState->audio_stream);
			if (m_videoState->subtitle_stream >= 0)
				packet_queue_put_nullpacket(&m_videoState->subtitleq, pkt, m_videoState->subtitle_stream);
			m_videoState->eof = 1;
		}
		if (ic->pb && ic->pb->error) {
			return false;
// 			if (autoexit) {
// 				auto cb = event_cb.lock();
// 				if (cb) {
// 					cb->on_stream_error("av_read_frame failed");
// 					cb->on_player_auto_exit();
// 				}
// 				goto fail;
// 			}
// 			else
// 				break;
		}
		SDL_LockMutex(wait_mutex);
		SDL_CondWaitTimeout(m_videoState->continue_read_thread, wait_mutex, 10);
		SDL_UnlockMutex(wait_mutex);
		return true;
	}
	else {
		m_videoState->eof = 0;
	}
	/* check if packet is in play range specified by user, then queue, otherwise discard */
	int64_t stream_start_time = ic->streams[pkt->stream_index]->start_time;
	int64_t pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
	int pkt_in_play_range = duration == AV_NOPTS_VALUE ||
		(pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
		av_q2d(ic->streams[pkt->stream_index]->time_base) -
		(double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
		<= ((double)duration / 1000000);
	if (pkt->stream_index == m_videoState->audio_stream && pkt_in_play_range) {
		packet_queue_put(&m_videoState->audioq, pkt);
	}
	else if (pkt->stream_index == m_videoState->video_stream && pkt_in_play_range
		&& !(m_videoState->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
		packet_queue_put(&m_videoState->videoq, pkt);
	}
	else if (pkt->stream_index == m_videoState->subtitle_stream && pkt_in_play_range) {
		packet_queue_put(&m_videoState->subtitleq, pkt);
	}
	else {
		av_packet_unref(pkt);
	}
	return true;
}

bool FFDemuxer::onReady()
{
	if (!openStream())
	{
		return false;
	}

	return readPacket();
}

void FFDemuxer::onFinish()
{
	// 先停掉 pictq 的消费线程，再让解码线程退出，
	// 否则解码线程会再次卡在 frame_queue_peek_writable 上无法 join
	stopVideoRefresh();
	abortVideoDecoder();
	resetVideoConverter();
	abortAudioDecoder();

	if (ic && !m_videoState->ic)
		avformat_close_input(&ic);

	av_packet_free(&pkt);
	SDL_DestroyMutex(wait_mutex);

	stream_ready = false;
}

bool FFDemuxer::openStream()
{
	if (!m_parameters || m_parameters->file_name.empty())
	{
		return false;
	}
	m_videoState = (VideoState*)av_mallocz(sizeof(VideoState));
	m_videoState->filename = av_strdup(m_parameters->file_name.c_str());
	if (!m_videoState->filename)
	{
		free(m_videoState);
		m_videoState = nullptr;
		return false;
	}
	/* start video display */

	if (frame_queue_init(&m_videoState->pictq, &m_videoState->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1)
		|| frame_queue_init(&m_videoState->subpq, &m_videoState->subtitleq, SUBPICTURE_QUEUE_SIZE, 0)
		|| frame_queue_init(&m_videoState->sampq, &m_videoState->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
	{
		closeStream();
		return false;
	}

	if (packet_queue_init(&m_videoState->videoq) < 0 || packet_queue_init(&m_videoState->audioq) < 0 || packet_queue_init(&m_videoState->subtitleq) < 0)
	{
		closeStream();
		return false;
	}

	if (!(m_videoState->continue_read_thread = SDL_CreateCond()))
	{
		//av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		closeStream();
		return false;
	}

	m_audioDecoder->setVideoState(m_videoState);
	m_videoDecoder->setVideoState(m_videoState);
	init_clock(&m_videoState->vidclk, &m_videoState->videoq.serial);
	init_clock(&m_videoState->audclk, &m_videoState->audioq.serial);
	init_clock(&m_videoState->extclk, &m_videoState->extclk.serial);
	m_videoState->audio_clock_serial = -1;
	startup_volume = av_clip(startup_volume, 0, 100);
	startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
	m_videoState->audio_volume = startup_volume;
	m_videoState->muted = 0;
	m_videoState->av_sync_type = AV_SYNC_AUDIO_MASTER;
	m_videoState->show_mode = SHOW_MODE_NONE;
	return true;
}

bool FFDemuxer::readPacket()
{
	bool eof_happened = false;

	VideoState* is = m_videoState;
	int err, i, ret;
	int st_index[AVMEDIA_TYPE_NB];
	int64_t stream_start_time;
	int pkt_in_play_range = 0;
	const AVDictionaryEntry* t;
	SDL_mutex* wait_mutex = SDL_CreateMutex();
	int scan_all_pmts_set = 0;
	int64_t pkt_ts;

	auto readFailure = [&]
	{
		if (ic && !is->ic)
			avformat_close_input(&ic);

		av_packet_free(&pkt);
		SDL_DestroyMutex(wait_mutex);
		stream_ready = false;
	};

	if (!wait_mutex) {
		//av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		ret = AVERROR(ENOMEM);
		readFailure();
		return false;
	}

	memset(st_index, -1, sizeof(st_index));
	is->eof = 0;

	pkt = av_packet_alloc();
	if (!pkt) {
		//av_log_ffplay(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
		ret = AVERROR(ENOMEM);
		readFailure();
		return false;
	}
	ic = avformat_alloc_context();
	if (!ic) {
		//av_log_ffplay(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
		ret = AVERROR(ENOMEM);
		readFailure();
		return false;
	}
	ic->interrupt_callback.callback = s_decode_interrupt_cb;
	ic->interrupt_callback.opaque = this;
	if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
		av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
		scan_all_pmts_set = 1;
	}
	err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
	if (err < 0) {
		char errBuf[AV_ERROR_MAX_STRING_SIZE]{ 0 };
		av_strerror(err, errBuf, AV_ERROR_MAX_STRING_SIZE);
		//av_log_ffplay(NULL, AV_LOG_ERROR, "avformat_open_input failed: %s\n", get_ffmpeg_error(err).c_str());
		// assert(false);
		ret = -1;
		readFailure();
		return false;
	}
	if (scan_all_pmts_set)
		av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

	if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
		//av_log_ffplay(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		readFailure();
		return false;
	}
	is->ic = ic;

	if (genpts)
		ic->flags |= AVFMT_FLAG_GENPTS;

	//av_format_inject_global_side_data(ic);

	if (find_stream_info)
	{
		AVDictionary** opts = setup_find_stream_info_opts(ic, codec_opts);
		int orig_nb_streams = ic->nb_streams;

		err = avformat_find_stream_info(ic, opts);

		for (i = 0; i < orig_nb_streams; i++)
			av_dict_free(&opts[i]);
		av_freep(&opts);

		if (err < 0) {
			//av_log_ffplay(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n", is->filename);
			ret = -1;
			//goto fail;
			readFailure();
			return false;
		}
	}

	if (ic->pb)
		ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

	if (seek_by_bytes < 0)
		seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
		!!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
		strcmp("ogg", ic->iformat->name);

	is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

	/* if seeking requested, we execute it */
	if (start_time != AV_NOPTS_VALUE) {
		int64_t timestamp;

		timestamp = start_time;
		/* add the stream start time */
		if (ic->start_time != AV_NOPTS_VALUE)
			timestamp += ic->start_time;
		ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
		if (ret < 0) {
			//av_log_ffplay(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n", is->filename, (double)timestamp / AV_TIME_BASE);
		}
	}

	is->realtime = is_realtime(ic);

	for (i = 0; i < ic->nb_streams; i++) {
		AVStream* st = ic->streams[i];
		enum AVMediaType type = st->codecpar->codec_type;
		st->discard = AVDISCARD_ALL;
		if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
			if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
				st_index[type] = i;
	}
	for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
		if (wanted_stream_spec[i] && st_index[i] == -1) {
			// av_log_ffplay(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string((enum AVMediaType)i));
			st_index[i] = INT_MAX;
		}
	}

	st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
	st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO], st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
	st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE], (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]), NULL, 0);

	/*if (st_index[AVMEDIA_TYPE_VIDEO] >= 0)
	{
		AVStream* st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
		AVCodecParameters* codecpar = st->codecpar;
		AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
		if (codecpar->width)
			set_default_window_size(codecpar->width, codecpar->height, sar);
	}*/

	/* open the streams */
	if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
		stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
	}

	ret = -1;
	if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
		ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
	}
	if (is->show_mode == SHOW_MODE_NONE)
		is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

	if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
		stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
	}

	if (is->video_stream < 0 && is->audio_stream < 0) {
		//av_log_ffplay(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", is->filename);
		ret = -1;
		readFailure();
		return false;
		//goto fail;
	}

	if (infinite_buffer < 0 && is->realtime)
		infinite_buffer = 1;

	stream_ready = true;
	video_included = is->video_stream >= 0;
	audio_included = is->audio_stream >= 0;
	duration_seconds = get_duration_seconds(is);

	// 对齐样例 run_player：read 线程之外再起一个刷新线程，
	// 它负责消费 pictq（video_refresh -> video_display -> send_video_image_to_upper）
	startVideoRefresh();
	return true;
}

int FFDemuxer::stream_component_open(VideoState* is, int stream_index)
{
	// for hw by walker-WSH
	AVBufferRef* temp_hw_device_ctx = nullptr;

	AVFormatContext* ic = is->ic;
	AVCodecContext* avctx;
	const AVCodec* codec;
	const char* forced_codec_name = NULL;
	AVDictionary* opts = NULL;
	const AVDictionaryEntry* t = NULL;
	int sample_rate;
	AVChannelLayout ch_layout = { 0 };
	int ret = 0;
	int stream_lowres = 0;

	if (stream_index < 0 || stream_index >= ic->nb_streams)
		return -1;

	avctx = avcodec_alloc_context3(NULL);
	if (!avctx)
		return AVERROR(ENOMEM);

	auto onOpenFail = [&]
	{
		avcodec_free_context(&avctx);
		// for send audio by walker-WSH
		/*if (audio_dev != 0) {
			SDL_CloseAudioDevice(audio_dev);
			audio_dev = 0;
		}*/
	};

	ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
	if (ret < 0)
	{
		onOpenFail();
		return false;
	}
	avctx->pkt_timebase = ic->streams[stream_index]->time_base;

	codec = avcodec_find_decoder(avctx->codec_id);

	switch (avctx->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		is->last_audio_stream = stream_index;
		//forced_codec_name = audio_codec_name;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->last_subtitle_stream = stream_index;
		//forced_codec_name = subtitle_codec_name;
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->last_video_stream = stream_index;
		//forced_codec_name = video_codec_name;
		break;
	}
	if (forced_codec_name)
		codec = avcodec_find_decoder_by_name(forced_codec_name);
	if (!codec) {
		//if (forced_codec_name)
		//	av_log_ffplay(NULL, AV_LOG_WARNING, "No codec could be found with name '%s'\n", forced_codec_name);
		//else
		//	av_log_ffplay(NULL, AV_LOG_WARNING, "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
		ret = AVERROR(EINVAL);
		onOpenFail();
		return false;
	}

	avctx->codec_id = codec->id;
	if (stream_lowres > codec->max_lowres)
	{
		//av_log_ffplay(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n", codec->max_lowres);
		stream_lowres = codec->max_lowres;
	}
	avctx->lowres = stream_lowres;

	if (fast)
		avctx->flags2 |= AV_CODEC_FLAG2_FAST;

	// for hw by walker-WSH
	if (m_parameters->hw_decode && avctx->codec_type == AVMediaType::AVMEDIA_TYPE_VIDEO)
	{
		if ((avctx->width * avctx->height) >= min_hw_image_size)
		{
			temp_hw_device_ctx = init_hw_decoder(&is->viddec, avctx, codec);
		}
		else
		{
			//av_log_ffplay(NULL, AV_LOG_INFO, "ignore hw decode since low resolution. %dx%d \n", avctx->width, avctx->height);
		}
	}

	opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
	if (!av_dict_get(opts, "threads", NULL, 0))
		av_dict_set(&opts, "threads", "auto", 0);
	if (stream_lowres)
		av_dict_set_int(&opts, "lowres", stream_lowres, 0);
	if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
		//goto fail;
		onOpenFail();
		return false;
	}
	if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
		//av_log_ffplay(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		//goto fail;
		onOpenFail();
		return false;
	}

	is->eof = 0;
	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
	switch (avctx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
	{
		AVFilterContext* sink;

		is->audio_filter_src.freq = avctx->sample_rate;
		ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
		if (ret < 0)
		{
			onOpenFail();
			return false;
		}
		is->audio_filter_src.fmt = avctx->sample_fmt;
		if ((ret = configure_audio_filters(is, afilters.empty() ? NULL : afilters.c_str(), 0)) < 0)
		{
			onOpenFail();
			return false;
		}
		sink = is->out_audio_filter;
		sample_rate = av_buffersink_get_sample_rate(sink);
		ret = av_buffersink_get_ch_layout(sink, &ch_layout);
		if (ret < 0)
		{
			onOpenFail();
			return false;
		}
	}
#else
		sample_rate = avctx->sample_rate;
		ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout);
		if (ret < 0)
			goto fail;
#endif

		/* prepare audio output */
		if ((ret = audio_open(is, &ch_layout, sample_rate, &is->audio_tgt)) < 0)
		{
			onOpenFail();
			return false;
		}
		is->audio_hw_buf_size = ret;
		is->audio_src = is->audio_tgt;
		is->audio_buf_size = 0;
		is->audio_buf_index = 0;

		/* init averaging filter */
		is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
		is->audio_diff_avg_count = 0;
		/* since we do not have a precise anough audio FIFO fullness,
		   we correct audio sync only if larger than this threshold */
		is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

		is->audio_stream = stream_index;
		is->audio_st = ic->streams[stream_index];

		if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0)
		{
			onOpenFail();
			return false;
		}
		/*if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
			is->auddec.start_pts = is->audio_st->start_time;
			is->auddec.start_pts_tb = is->audio_st->time_base;
		}*/
		packet_queue_start(is->auddec.queue);
		m_audioDecoder->start();
		//if ((ret = decoder_start(&is->auddec, s_audio_thread, "audio_decoder", this)) < 0)
		//	goto out;

		// for send audio by walker-WSH
		if (m_parameters->disable_debug_render)
		{
			//pop_audio_handle = std::thread(&ffplayer::pop_audio_thread, this, is);
		}
		else {
			SDL_PauseAudioDevice(audio_dev, 0);
		}

		break;
	case AVMEDIA_TYPE_VIDEO:
		is->video_stream = stream_index;
		is->video_st = ic->streams[stream_index];

		if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0)
		{
			onOpenFail();
			return false;
		}
		packet_queue_start(is->viddec.queue);

		// for hw by walker-WSH
		// 必须在解码线程启动前确定并下发硬件解码标志：
		// 否则解码线程会按“未启用硬解”处理，把 libavcodec 输出的硬件帧原样送进滤镜图，
		// 最终在 configure_video_filters() 里以 -22(EINVAL) 失败。
		hw_device_buf = temp_hw_device_ctx;
		hw_decode_used = !!temp_hw_device_ctx;
		temp_hw_device_ctx = nullptr;
		m_videoDecoder->setUseHwDecode(hw_decode_used);

		m_videoDecoder->start();
		//if ((ret = decoder_start(&is->viddec, s_video_thread, "video_decoder", this)) < 0)
		//	goto out;
		is->queue_attachments_req = 1;

		//av_log_ffplay(NULL, AV_LOG_INFO, "request hardware decode:%d, result:%d \n", request_hw_decode, hw_decode_used);
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->subtitle_stream = stream_index;
		is->subtitle_st = ic->streams[stream_index];

		if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0)
		{
			onOpenFail();
			return false;
		}
		packet_queue_start(is->subdec.queue);
		m_subtitleDecoder->start();
		break;
	default:
		break;
	}
	goto out;

out:
	// for hw by walker-WSH
	av_buffer_unref(&temp_hw_device_ctx);

	av_channel_layout_uninit(&ch_layout);
	av_dict_free(&opts);

	return ret;
}

int FFDemuxer::frame_queue_init(FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last)
{
	int i;
	memset(f, 0, sizeof(FrameQueue));
	if (!(f->mutex = SDL_CreateMutex()))
	{
		// av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	if (!(f->cond = SDL_CreateCond()))
	{
		// av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	f->pktq = pktq;
	f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
	f->keep_last = !!keep_last;
	for (i = 0; i < f->max_size; i++)
		if (!(f->queue[i].frame = av_frame_alloc()))
			return AVERROR(ENOMEM);
	return 0;
}

int FFDemuxer::packet_queue_init(PacketQueue* q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
	if (!q->pkt_list)
		return AVERROR(ENOMEM);
	q->mutex = SDL_CreateMutex();
	if (!q->mutex)
	{
		//av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->cond = SDL_CreateCond();
	if (!q->cond)
	{
		//av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->abort_request = 1;
	return 0;
}

void FFDemuxer::init_clock(Clock* c, int* queue_serial)
{
	c->speed = 1.0;
	c->paused = 0;
	c->queue_serial = queue_serial;
	set_clock(c, NAN, -1);
}

void FFDemuxer::set_clock(Clock* c, double pts, int serial)
{
	double time = av_gettime_relative() / 1000000.0;
	set_clock_at(c, pts, serial, time);
}

void FFDemuxer::set_clock_at(Clock* c, double pts, int serial, double time)
{
	c->pts = pts;
	c->last_updated = time;
	c->pts_drift = c->pts - time;
	c->serial = serial;
}

int FFDemuxer::s_decode_interrupt_cb(void* ctx)
{
	auto self = (FFDemuxer*)ctx;
	return self->m_videoState->abort_request;
}

void FFDemuxer::s_sdl_audio_callback(void* arg, Uint8* stream, int len)
{
	auto self = (FFDemuxer*)arg;
	self->sdl_audio_callback(self->m_videoState, stream, len);
}

int FFDemuxer::is_realtime(AVFormatContext* s)
{
	if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") || !strcmp(s->iformat->name, "sdp"))
		return 1;

	if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
		return 1;

	return 0;
}

double FFDemuxer::get_duration_seconds(VideoState* is)
{
	auto ctx_dur = is->ic->duration; // micro seconds
	if (ctx_dur != AV_NOPTS_VALUE)
		return double(ctx_dur) / AV_TIME_BASE;

	double audio_dur = 0.0;
	if (is->audio_st)
	{
		audio_dur = av_q2d(is->audio_st->time_base) * is->audio_st->duration;
	}

	double video_dur = 0.0;
	if (is->video_st)
	{
		video_dur = av_q2d(is->video_st->time_base) * is->video_st->duration;
	}

	return std::max(audio_dur, video_dur);
}

// for hw by walker-WSH
AVBufferRef* FFDemuxer::init_hw_decoder(Decoder* d, AVCodecContext* c, const AVCodec* codec)
{
	enum AVHWDeviceType hw_priority[] = 
	{
		AV_HWDEVICE_TYPE_D3D11VA,      AV_HWDEVICE_TYPE_DXVA2,
		AV_HWDEVICE_TYPE_CUDA,         AV_HWDEVICE_TYPE_VAAPI,
		AV_HWDEVICE_TYPE_VDPAU,        AV_HWDEVICE_TYPE_QSV,
		AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_NONE,
	};

	enum AVHWDeviceType* priority = hw_priority;
	enum AVPixelFormat temp_format = AVPixelFormat::AV_PIX_FMT_NONE;
	AVBufferRef* hw_ctx = NULL;

	while (*priority != AV_HWDEVICE_TYPE_NONE)
	{
		if (has_hw_type(codec, *priority, &temp_format)) {
			int ret = av_hwdevice_ctx_create(&hw_ctx, *priority, NULL, NULL, 0);
			if (ret == 0)
				break;
		}

		priority++;
	}

	if (hw_ctx) {
		// this ref will be decreased by ffmpeg
		c->hw_device_ctx = av_buffer_ref(hw_ctx);
		hw_format = temp_format;
		m_videoDecoder->setHwFormat(hw_format);
	}

	return hw_ctx;
}

bool FFDemuxer::has_hw_type(const AVCodec* c, enum AVHWDeviceType type, enum AVPixelFormat* output_hw_format)
{
	for (int i = 0;; i++) {
		const AVCodecHWConfig* config = avcodec_get_hw_config(c, i);
		if (!config) {
			break;
		}

		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
			config->device_type == type) {
			*output_hw_format = config->pix_fmt;
			return true;
		}
	}

	return false;
}

int FFDemuxer::configure_audio_filters(VideoState* is, const char* intput_afilters, int force_output_format)
{
	AVFilterContext* filt_asrc = NULL, * filt_asink = NULL;
	char aresample_swr_opts[512] = "";
	const AVDictionaryEntry* e = NULL;
	AVBPrint bp;
	char asrc_args[256];
	int ret;

	avfilter_graph_free(&is->agraph);
	if (!(is->agraph = avfilter_graph_alloc()))
		return AVERROR(ENOMEM);
	is->agraph->nb_threads = m_parameters->filter_nbthreads;

	av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

	while ((e = av_dict_iterate(swr_opts, e)))
		av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
	if (strlen(aresample_swr_opts))
		aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
	av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

	av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

	ret = snprintf(asrc_args, sizeof(asrc_args),
		"sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
		is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
		1, is->audio_filter_src.freq, bp.str);

	ret = avfilter_graph_create_filter(&filt_asrc,
		avfilter_get_by_name("abuffer"), "ffplay_abuffer",
		asrc_args, NULL, is->agraph);
	if (ret < 0)
		goto end;

	/* 只分配、不初始化；否则初始化后再设置非 runtime 选项会返回 EINVAL(-22) */
	filt_asink = avfilter_graph_alloc_filter(is->agraph, avfilter_get_by_name("abuffersink"), "ffplay_abuffersink");
	if (!filt_asink)
	{
		ret = AVERROR(ENOMEM);
		goto end;
	}

	if ((ret = av_opt_set(filt_asink, "sample_formats", "s16", AV_OPT_SEARCH_CHILDREN)) < 0)
		goto end;

	if (force_output_format)
	{
		if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN, 0, 1, AV_OPT_TYPE_CHLAYOUT, &is->audio_tgt.ch_layout)) < 0)
			goto end;
		if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN, 0, 1, AV_OPT_TYPE_INT, &is->audio_tgt.freq)) < 0)
			goto end;
	}

	/* 先设置所有选项，最后再初始化 filter */
	if ((ret = avfilter_init_dict(filt_asink, NULL)) < 0)
		goto end;


	if ((ret = configure_filtergraph(is->agraph, intput_afilters, filt_asrc, filt_asink)) < 0)
		goto end;

	is->in_audio_filter = filt_asrc;
	is->out_audio_filter = filt_asink;

end:
	if (ret < 0)
		avfilter_graph_free(&is->agraph);
	av_bprint_finalize(&bp, NULL);

	return ret;
}

int FFDemuxer::audio_open(void* opaque, AVChannelLayout* wanted_channel_layout, int wanted_sample_rate, struct AudioParams* audio_hw_params)
{
    // 拆成独立 DLL 后，legacy 引擎持有的 SDL2 与主引擎持有的 SDL3 是两个互不相干的实例：
    // 以前同处一个模块时，SDL2 的音频子系统是被主引擎的 SDL_Init(SDL_INIT_AUDIO) 顺带打开的，
    // 现在必须由本模块自己初始化，否则 SDL_OpenAudioDevice 会直接返回 0，
    // 重试完所有 声道/采样率 组合后返回 -1。
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        fprintf(stderr, "[FFDemuxer] SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s\n", SDL_GetError());
        return -1;
    }

	SDL_AudioSpec wanted_spec, spec;
	const char* env;
	const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
	const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
	int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
	int wanted_nb_channels = wanted_channel_layout->nb_channels;

	env = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (env)
	{
		wanted_nb_channels = atoi(env);
		av_channel_layout_uninit(wanted_channel_layout);
		av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
	}
	if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
		av_channel_layout_uninit(wanted_channel_layout);
		av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
	}
	wanted_nb_channels = wanted_channel_layout->nb_channels;
	wanted_spec.channels = wanted_nb_channels;
	wanted_spec.freq = wanted_sample_rate;
	if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
		//av_log_ffplay(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
		return -1;
	}
	while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
		next_sample_rate_idx--;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.silence = 0;
	wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
	wanted_spec.callback = s_sdl_audio_callback;
	wanted_spec.userdata = this;

	// for send audio by walker-WSH
	if (m_parameters->disable_debug_render) {
		// 
		spec = wanted_spec;

		spec.channels = 2;
		spec.size = wanted_spec.samples * spec.channels * 2; // 2 : sizeof AUDIO_S16SYS
	}
	else {
		while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
			fprintf(stderr, "[FFDemuxer] SDL_OpenAudioDevice(%d channels, %d Hz) failed: %s\n",
				wanted_spec.channels, wanted_spec.freq, SDL_GetError());
			wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
			if (!wanted_spec.channels) {
				wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
				wanted_spec.channels = wanted_nb_channels;
				if (!wanted_spec.freq) {
					fprintf(stderr, "[FFDemuxer] No more combinations to try, audio open failed\n");
					return -1;
				}
			}
			av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
		}
	}

	if (spec.format != AUDIO_S16SYS) {
		//av_log_ffplay(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
		return -1;
	}
	if (spec.channels != wanted_spec.channels) {
		av_channel_layout_uninit(wanted_channel_layout);
		av_channel_layout_default(wanted_channel_layout, spec.channels);
		if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
			// av_log_ffplay(NULL, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
			return -1;
		}
	}

	audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
	audio_hw_params->freq = spec.freq;
	if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
		return -1;
	audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
	audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
	if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
		// av_log_ffplay(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
		return -1;
	}
	return spec.size;
}

int FFDemuxer::decoder_init(Decoder* d, AVCodecContext* avctx, PacketQueue* queue, SDL_cond* empty_queue_cond)
{
	memset(d, 0, sizeof(Decoder));
	d->pkt = av_packet_alloc();
	if (!d->pkt)
		return AVERROR(ENOMEM);
	d->avctx = avctx;
	d->queue = queue;
	d->empty_queue_cond = empty_queue_cond;
	d->start_pts = AV_NOPTS_VALUE;
	d->pkt_serial = -1;
	return 0;
}

void FFDemuxer::packet_queue_start(PacketQueue* q)
{
	SDL_LockMutex(q->mutex);
	q->abort_request = 0;
	q->serial++;
	SDL_UnlockMutex(q->mutex);
}

void FFDemuxer::packet_queue_flush(PacketQueue* q)
{
	MyAVPacketList pkt1;

	SDL_LockMutex(q->mutex);
	while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
		av_packet_free(&pkt1.pkt);
	q->nb_packets = 0;
	q->size = 0;
	q->duration = 0;
	q->serial++;
	SDL_UnlockMutex(q->mutex);
}

void FFDemuxer::step_to_next_frame(VideoState* is)
{
	/* if the stream is paused unpause it, then step */
	if (is->paused)
		stream_toggle_pause(is);
	is->step = 1;
}

double FFDemuxer::get_clock(Clock* c)
{
	if (*c->queue_serial != c->serial)
		return NAN;
	if (c->paused) {
		return c->pts;
	}
	else {
		double time = av_gettime_relative() / 1000000.0;
		return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
	}
}

int FFDemuxer::packet_queue_put(PacketQueue* q, AVPacket* pkt)
{
	AVPacket* pkt1;
	int ret;

	pkt1 = av_packet_alloc();
	if (!pkt1) {
		av_packet_unref(pkt);
		return -1;
	}
	av_packet_move_ref(pkt1, pkt);

	SDL_LockMutex(q->mutex);
	ret = packet_queue_put_private(q, pkt1);
	SDL_UnlockMutex(q->mutex);

	if (ret < 0)
		av_packet_free(&pkt1);

	return ret;
}

int FFDemuxer::packet_queue_put_nullpacket(PacketQueue* q, AVPacket* pkt, int stream_index)
{
	pkt->stream_index = stream_index;
	return packet_queue_put(q, pkt);
}

int FFDemuxer::packet_queue_put_private(PacketQueue* q, AVPacket* pkt)
{
	MyAVPacketList pkt1;
	int ret;

	if (q->abort_request)
		return -1;


	pkt1.pkt = pkt;
	pkt1.serial = q->serial;

	ret = av_fifo_write(q->pkt_list, &pkt1, 1);
	if (ret < 0)
		return ret;
	q->nb_packets++;
	q->size += pkt1.pkt->size + sizeof(pkt1);
	q->duration += pkt1.pkt->duration;
	/* XXX: should duplicate packet data in DV case */
	SDL_CondSignal(q->cond);
	return 0;
}

int FFDemuxer::stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue)
{
	return stream_id < 0 ||
		queue->abort_request ||
		(st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
		queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

void FFDemuxer::stream_seek(VideoState* is, int64_t pos, int64_t rel, int by_bytes)
{
	if (!is->seek_req) {
		is->seek_pos = pos;
		is->seek_rel = rel;
		is->seek_flags &= ~AVSEEK_FLAG_BYTE;
		if (by_bytes)
			is->seek_flags |= AVSEEK_FLAG_BYTE;
		is->seek_req = 1;
		SDL_CondSignal(is->continue_read_thread);
	}
}

int FFDemuxer::frame_queue_nb_remaining(FrameQueue* f)
{
	return f->size - f->rindex_shown;
}

void FFDemuxer::sdl_audio_callback(void* opaque, Uint8* stream, int len)
{
	VideoState* is = (VideoState*)opaque;
	int audio_size, len1;

	audio_callback_time = av_gettime_relative();

	while (len > 0) {
		if (is->audio_buf_index >= is->audio_buf_size) {
			audio_size = audio_decode_frame(is);
			if (audio_size < 0) {
				/* if error, just output silence */
				is->audio_buf = NULL;
				is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
			}
			else {
				if (is->show_mode != SHOW_MODE_VIDEO)
					update_sample_display(is, (int16_t*)is->audio_buf, audio_size);
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;
		if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
			memcpy(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, len1);
		else {
			memset(stream, 0, len1);
			if (!is->muted && is->audio_buf)
				SDL_MixAudioFormat(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
		}
		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
	is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
	/* Let's assume the audio driver that is used by SDL has two periods. */
	if (!isnan(is->audio_clock)) {
		set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
		sync_clock_to_slave(&is->extclk, &is->audclk);
	}
}

int FFDemuxer::audio_decode_frame(VideoState* is)
{
	int data_size, resampled_data_size;
	av_unused double audio_clock0;
	int wanted_nb_samples;
	Frame* af;

	if (is->paused)
		return -1;

	do {
#if defined(_WIN32)
		while (frame_queue_nb_remaining(&is->sampq) == 0) {
			if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
				return -1;
			av_usleep(1000);
		}
#endif
		if (!(af = frame_queue_peek_readable(&is->sampq)))
			return -1;
		frame_queue_next(&is->sampq);
	} while (af->serial != is->audioq.serial);

	data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
		af->frame->nb_samples,
		(enum AVSampleFormat)af->frame->format, 1);

	wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

	if (af->frame->format != is->audio_src.fmt ||
		av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
		af->frame->sample_rate != is->audio_src.freq ||
		(wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
		swr_free(&is->swr_ctx);
		swr_alloc_set_opts2(&is->swr_ctx,
			&is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
			&af->frame->ch_layout, (enum AVSampleFormat)af->frame->format, af->frame->sample_rate,
			0, NULL);
		if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
			//av_log_ffplay(NULL, AV_LOG_ERROR,
			//	"Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
			//	af->frame->sample_rate, av_get_sample_fmt_name((enum AVSampleFormat)af->frame->format), af->frame->ch_layout.nb_channels,
			//	is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);
			swr_free(&is->swr_ctx);
			return -1;
		}
		if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
			return -1;
		is->audio_src.freq = af->frame->sample_rate;
		is->audio_src.fmt = (enum AVSampleFormat)af->frame->format;
	}

	if (is->swr_ctx) {
		const uint8_t** in = (const uint8_t**)af->frame->extended_data;
		uint8_t** out = &is->audio_buf1;
		int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
		int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
		int len2;
		if (out_size < 0) {
			//av_log_ffplay(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
			return -1;
		}
		if (wanted_nb_samples != af->frame->nb_samples) {
			if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
				wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
				//av_log_ffplay(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
				return -1;
			}
		}
		av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
		if (!is->audio_buf1)
			return AVERROR(ENOMEM);
		len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
		if (len2 < 0) {
			//av_log_ffplay(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
			return -1;
		}
		if (len2 == out_count) {
			//av_log_ffplay(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
			if (swr_init(is->swr_ctx) < 0)
				swr_free(&is->swr_ctx);
		}
		is->audio_buf = is->audio_buf1;
		resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
	}
	else {
		is->audio_buf = af->frame->data[0];
		resampled_data_size = data_size;
	}

	audio_clock0 = is->audio_clock;
	/* update the audio clock with the pts */
	if (!isnan(af->pts))
		is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
	else
		is->audio_clock = NAN;

	// for send audio by walker-WSH
	if (!isnan(af->pts)) {
		frame_pts_begin = pts_pos = af->pts;
		frame_pts_end = is->audio_clock;
	}
	else {
		frame_pts_begin = pts_pos = frame_pts_end = NAN;
	}

#if defined(DEBUG_SYNC)
	av_log_ffplay(NULL, AV_LOG_INFO, "----- pop audio , % lf \n", af->pts);
#endif

	is->audio_clock_serial = af->serial;
	return resampled_data_size;
}

Frame* FFDemuxer::frame_queue_peek_readable(FrameQueue* f)
{
	/* wait until we have a readable a new frame */
	SDL_LockMutex(f->mutex);
	while (f->size - f->rindex_shown <= 0 &&
		!f->pktq->abort_request) {
		SDL_CondWait(f->cond, f->mutex);
	}
	SDL_UnlockMutex(f->mutex);

	if (f->pktq->abort_request)
		return NULL;

	return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

void FFDemuxer::frame_queue_next(FrameQueue* f)
{
	if (f->keep_last && !f->rindex_shown) {
		f->rindex_shown = 1;
		return;
	}
	frame_queue_unref_item(&f->queue[f->rindex]);
	if (++f->rindex == f->max_size)
		f->rindex = 0;
	SDL_LockMutex(f->mutex);
	f->size--;
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

void FFDemuxer::frame_queue_unref_item(Frame* vp)
{
	av_frame_unref(vp->frame);
	avsubtitle_free(&vp->sub);
}

int FFDemuxer::synchronize_audio(VideoState* is, int nb_samples)
{
	int wanted_nb_samples = nb_samples;

	/* if not master, then we try to remove or add samples to correct the clock */
	if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
		double diff, avg_diff;
		int min_nb_samples, max_nb_samples;

		diff = get_clock(&is->audclk) - get_master_clock(is);

		if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
			is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
			if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
				/* not enough measures to have a correct estimate */
				is->audio_diff_avg_count++;
			}
			else {
				/* estimate the A-V difference */
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

				if (fabs(avg_diff) >= is->audio_diff_threshold) {
					wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
					min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
				}
				/*av_log_ffplay(NULL, AV_LOG_DEBUG, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
					diff, avg_diff, wanted_nb_samples - nb_samples,
					is->audio_clock, is->audio_diff_threshold);
				*/
			}
		}
		else {
			/* too big difference : may be initial PTS errors, so
			   reset A-V filter */
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}

	return wanted_nb_samples;
}

int FFDemuxer::get_master_sync_type(VideoState* is)
{
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
		if (is->video_st)
			return AV_SYNC_VIDEO_MASTER;
		else
			return AV_SYNC_AUDIO_MASTER;
	}
	else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
		if (is->audio_st)
			return AV_SYNC_AUDIO_MASTER;
		else
			return AV_SYNC_EXTERNAL_CLOCK;
	}
	else {
		return AV_SYNC_EXTERNAL_CLOCK;
	}
}

double FFDemuxer::get_master_clock(VideoState* is)
{
	double val;

	switch (get_master_sync_type(is)) {
	case AV_SYNC_VIDEO_MASTER:
		val = get_clock(&is->vidclk);
		break;
	case AV_SYNC_AUDIO_MASTER:
		val = get_clock(&is->audclk);
		break;
	default:
		val = get_clock(&is->extclk);
		break;
	}
	return val;
}

void FFDemuxer::update_sample_display(VideoState* is, short* samples, int samples_size)
{
	int size, len;

	size = samples_size / sizeof(short);
	while (size > 0) {
		len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
		if (len > size)
			len = size;
		memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
		samples += len;
		is->sample_array_index += len;
		if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
			is->sample_array_index = 0;
		size -= len;
	}
}

void FFDemuxer::sync_clock_to_slave(Clock* c, Clock* slave)
{
	double clock = get_clock(c);
	double slave_clock = get_clock(slave);
	if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
		set_clock(c, slave_clock, slave->serial);
}

void FFDemuxer::stream_toggle_pause(VideoState* is, bool cb_need /*= false*/)
{
	if (is->paused)
	{
		is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
		if (is->read_pause_return != AVERROR(ENOSYS)) {
			is->vidclk.paused = 0;
		}
		set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
	}
	set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
	is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;

	if (cb_need) {
		/*auto cb = event_cb.lock();
		if (cb) {
			if (is->paused)
			{
				cb->on_player_paused();
			}
			else {
				cb->on_player_resumed();
			}
		}*/
	}
}

#if CONFIG_AVFILTER
int FFDemuxer::configure_filtergraph(AVFilterGraph* graph, const char* filtergraph, AVFilterContext* source_ctx, AVFilterContext* sink_ctx)
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
#endif

// ============================================================================
// 视频帧消费端
// 与样例 ffplay.cpp 保持一致：read 线程只负责解封装，解码由 FFVideoDecoder 线程完成，
// 而 pictq 的消费者是样例事件循环线程 s_event_loop 里的 video_refresh()。
// 本引擎没有 SDL 窗口，所以这里只保留“取帧 -> 交给上层”这一条通路。
// ============================================================================

void FFDemuxer::setVideoFrameCallback(std::function<void(const VideoData&)> callback)
{
    m_videoFrameCallback = callback;
}

void FFDemuxer::startVideoRefresh()
{
    if (m_refreshing)
    {
        return;
    }
    m_refreshing = true;
    m_refreshThread = std::thread(&FFDemuxer::videoRefreshLoop, this);
}

void FFDemuxer::stopVideoRefresh()
{
    if (!m_refreshing)
    {
        return;
    }
    m_refreshing = false;
    if (m_refreshThread.joinable() && m_refreshThread.get_id() != std::this_thread::get_id())
    {
        m_refreshThread.join();
    }
}

// 对齐样例 refresh_loop_wait_event 里的循环体：
// 本引擎没有 SDL 窗口事件需要处理，只保留 video_refresh 的节拍
void FFDemuxer::videoRefreshLoop()
{
    double remaining_time = 0.0;
    VideoState* is = nullptr;

    while (m_refreshing)
    {
        is = m_videoState;
        if (!is)
        {
            break;
        }

        if (remaining_time > 0.0)
        {
            av_usleep((int64_t)(remaining_time * 1000000.0));
        }
        remaining_time = REFRESH_RATE;

        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
        {
            video_refresh(is, &remaining_time);
        }
    }
}

double FFDemuxer::compute_target_delay(double delay, VideoState* is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)
    {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration)
        {
            if (diff <= -sync_threshold)
            {
                delay = FFMAX(0, delay + diff);
            }
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
            {
                delay = delay + diff;
            }
            else if (diff >= sync_threshold)
            {
                delay = 2 * delay;
            }
        }
    }

    return delay;
}

double FFDemuxer::vp_duration(VideoState* is, Frame* vp, Frame* nextvp)
{
    if (vp->serial == nextvp->serial)
    {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
        {
            return vp->duration;
        }
        else
        {
            return duration;
        }
    }
    else
    {
        return 0.0;
    }
}

void FFDemuxer::update_video_pts(VideoState* is, double pts, int64_t pos, int serial)
{
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

void FFDemuxer::set_clock_speed(Clock* c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

void FFDemuxer::check_external_clock_speed(VideoState* is)
{
    if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
        is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)
    {
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    }
    else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
        (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES))
    {
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    }
    else
    {
        double speed = is->extclk.speed;
        if (speed != 1.0)
        {
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }
}

Frame* FFDemuxer::frame_queue_peek(FrameQueue* f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

Frame* FFDemuxer::frame_queue_peek_next(FrameQueue* f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

Frame* FFDemuxer::frame_queue_peek_last(FrameQueue* f)
{
    return &f->queue[f->rindex];
}

void FFDemuxer::frame_queue_signal(FrameQueue* f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

void FFDemuxer::packet_queue_abort(PacketQueue* q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

// 对齐样例 decoder_abort(&is->viddec, &is->pictq)
void FFDemuxer::abortVideoDecoder()
{
    if (!m_videoState || !m_videoDecoder)
    {
        return;
    }
    if (!m_videoDecoder->isRunning())
    {
        return;
    }

    /* 让阻塞在 packet_queue_get / frame_queue_peek_writable 里的解码线程退出 */
    packet_queue_abort(&m_videoState->videoq);
    frame_queue_signal(&m_videoState->pictq);
    m_videoDecoder->stop();
    packet_queue_flush(&m_videoState->videoq);
}

// 对齐样例 decoder_abort(&is->auddec, &is->sampq) 与 stream_component_close 里的
// SDL_CloseAudioDevice：先停掉 SDL 音频回调（回调里会消费 sampq），再让解码线程退出，
// 否则解码线程会一直阻塞在 packet_queue_get / frame_queue_peek_writable 上无法 join
void FFDemuxer::abortAudioDecoder()
{
    if (!m_videoState || !m_audioDecoder)
    {
        return;
    }
    if (!m_audioDecoder->isRunning())
    {
        return;
    }

    if (audio_dev)
    {
        SDL_PauseAudioDevice(audio_dev, 1);
    }

    packet_queue_abort(&m_videoState->audioq);
    frame_queue_signal(&m_videoState->sampq);
    m_audioDecoder->stop();
    packet_queue_flush(&m_videoState->audioq);

    if (audio_dev)
    {
        SDL_CloseAudioDevice(audio_dev);
        audio_dev = 0;
    }
}

// called to display each frame（对齐样例 video_refresh）
void FFDemuxer::video_refresh(VideoState* is, double* remaining_time)
{
    double time;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
    {
        check_external_clock_speed(is);
    }

    /* 样例此处还有 RDFT/波形显示分支，它依赖 SDL 渲染窗口，本引擎没有窗口，故只保留视频通路 */
    if (is->video_st)
    {
    retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0)
        {
            // nothing to do, no picture to display in the queue
        }
        else
        {
            double last_duration, duration, delay;
            Frame* vp;
            Frame* lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial)
            {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
            {
                is->frame_timer = av_gettime_relative() / 1000000.0;
            }

            if (is->paused)
            {
                goto display;
            }

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is);

            time = av_gettime_relative() / 1000000.0;
            if (time < is->frame_timer + delay)
            {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
            {
                is->frame_timer = time;
            }

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts))
            {
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            }
            SDL_UnlockMutex(is->pictq.mutex);

            if (frame_queue_nb_remaining(&is->pictq) > 1)
            {
                Frame* nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if (!is->step && (m_parameters->framedrop > 0 || (m_parameters->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration)
                {
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            /* 样例在这里把 subpq 的字幕取走并叠加到画面上；
               本引擎不在这里叠加字幕，但同样必须消费，否则 subpq 写满后
               字幕解码线程会阻塞在 frame_queue_peek_writable 上 */
            if (is->subtitle_st)
            {
                while (frame_queue_nb_remaining(&is->subpq) > 0)
                {
                    Frame* sp = frame_queue_peek(&is->subpq);
                    Frame* sp2 = NULL;

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                    {
                        sp2 = frame_queue_peek_next(&is->subpq);
                    }

                    if (sp->serial != is->subtitleq.serial
                        || (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000)))
                        || (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
                    {
                        frame_queue_next(&is->subpq);
                    }
                    else
                    {
                        break;
                    }
                }
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused)
            {
                stream_toggle_pause(is);
            }
        }
    display:
        /* display picture */
        if (is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
        {
            video_display(is);
        }
    }

    is->force_refresh = 0;
}

// 样例 video_display 分两支：display_disable 时把帧交给上层，否则用 SDL 渲染到窗口。
// 本引擎自身没有 SDL 窗口（画面由上层 UI 渲染），因此走“交给上层”这一支。
void FFDemuxer::video_display(VideoState* is)
{
    send_video_image_to_upper(is);
}

// 对齐样例 send_video_image_to_upper：
// 取 pictq 里当前正在显示的那一帧，转成 BGRA 的 VideoData 交给上层渲染
void FFDemuxer::send_video_image_to_upper(VideoState* is)
{
    if (!m_videoFrameCallback)
    {
        return;
    }

    /* 与样例 video_display 一致：显示的是 frame_queue_peek_last 那一帧 */
    Frame* vp = frame_queue_peek_last(&is->pictq);
    if (!vp || !vp->frame)
    {
        return;
    }

    VideoData data;
    if (!frameToVideoData(vp->frame, vp->pts, vp->duration, data))
    {
        return;
    }

    m_videoFrameCallback(data);
}

// 与 VideoRenderer::doRenderer 相同的渲染方式：
// 硬解帧先 av_hwframe_transfer_data 转软件帧，再 sws_scale 转 BGRA 后交给上层
bool FFDemuxer::frameToVideoData(AVFrame* frame, double pts, double duration, VideoData& out)
{
    if (!frame)
    {
        return false;
    }

    AVFrame* displayFrame = frame;

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
    if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
    {
        if (!m_videoSwFrame)
        {
            m_videoSwFrame = av_frame_alloc();
            if (!m_videoSwFrame)
            {
                return false;
            }
        }
        av_frame_unref(m_videoSwFrame);
        int ret = av_hwframe_transfer_data(m_videoSwFrame, frame, 0);
        if (ret < 0)
        {
            return false;
        }
        displayFrame = m_videoSwFrame;
    }

    int dstW = displayFrame->width;
    int dstH = displayFrame->height;
    if (dstW <= 0 || dstH <= 0)
    {
        return false;
    }

    /* SWS_FAST_BILINEAR：与 doRenderer 一致，4K 下比 SWS_BILINEAR 快 15~20% */
    m_videoSwsCtx = sws_getCachedContext(m_videoSwsCtx,
        displayFrame->width, displayFrame->height, (AVPixelFormat)displayFrame->format,
        dstW, dstH, AV_PIX_FMT_BGRA,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_videoSwsCtx)
    {
        return false;
    }

    /* 直接缩放到交给上层的 buffer，省掉 doRenderer 里 m_buffer -> cbData 的那次整帧拷贝 */
    const int linesize = dstW * 4;
    const int bufSize = linesize * dstH;
    std::shared_ptr<uint8_t> pixels((uint8_t*)av_malloc(bufSize), [](uint8_t* ptr)
    {
        av_free(ptr);
    });
    if (!pixels)
    {
        return false;
    }

    uint8_t* dstData[4] = { pixels.get(), nullptr, nullptr, nullptr };
    int      dstStride[4] = { linesize, 0, 0, 0 };
    sws_scale(m_videoSwsCtx, displayFrame->data, displayFrame->linesize,
        0, displayFrame->height, dstData, dstStride);

    out.width = dstW;
    out.height = dstH;
    out.format = AV_PIX_FMT_BGRA;
    out.linesize = linesize;
    out.data = pixels;
    out.pts = pts;
    out.duration = duration;

    return true;
}

void FFDemuxer::resetVideoConverter()
{
    if (m_videoSwsCtx)
    {
        sws_freeContext(m_videoSwsCtx);
        m_videoSwsCtx = nullptr;
    }
    if (m_videoSwFrame)
    {
        av_frame_free(&m_videoSwFrame);
        m_videoSwFrame = nullptr;
    }
}