#pragma once

#include "mediaplayertypes.h"
#include "frametypes.h"
#include "threadbase.h"
#include <functional>
#include <memory>
#include <thread>

struct VideoState;
class FFAudioDecoder;
class FFVideoDecoder;
class FFSubtitleDecoder;

extern "C"
{
	struct AVFrame;
	struct AVFilterGraph;
	struct AVFilterContext;
	struct SwsContext;
};

class FFDemuxer : public ThreadBase
{
public:
	FFDemuxer();
	~FFDemuxer();
	VideoState* videoState() const;
	bool startPlay(MediaParameters* parameters);
	void closeStream();

	/// 解码后的视频帧回调（BGRA，与 VideoRenderer::doRenderer 的输出一致）
	void setVideoFrameCallback(std::function<void(const VideoData&)> callback);

	/// 定位播放：跳转到指定毫秒位置（内部走 ffplay 的 stream_seek 流程）
	void seekToMs(int64_t positionMs);
	/// 当前播放位置（master clock，毫秒；未就绪返回 0）
	int64_t currentPosMs();
	/// 媒体总时长（毫秒；未知返回 0）
	int64_t durationMs() const;
	/// 播放进度回调（currentMs/totalMs），由视频刷新线程周期性触发，暂停期间也会保持触发
	void setProgressCallback(std::function<void(int64_t, int64_t)> callback);

protected:
	/// 子类实现：每次循环调用，返回 true 继续，false 退出
	bool run() override;
	/// 循环开始前调用（while 之前），子类可重写做准备工作
	bool onReady() override;
	void onFinish() override;

private:
	bool openStream();
	bool readPacket();
	int stream_component_open(VideoState* is, int stream_index);

private:
	int frame_queue_init(FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last);
	int packet_queue_init(PacketQueue* q);
	void init_clock(Clock* c, int* queue_serial);
	void set_clock(Clock* c, double pts, int serial);
	void set_clock_at(Clock* c, double pts, int serial, double time);
	static int s_decode_interrupt_cb(void* ctx);
	static void s_sdl_audio_callback(void* arg, Uint8* stream, int len);
	int is_realtime(AVFormatContext* s);
	double get_duration_seconds(VideoState* is);
	AVBufferRef* init_hw_decoder(Decoder* d, AVCodecContext* c, const AVCodec* codec);
	bool has_hw_type(const AVCodec* c, enum AVHWDeviceType type, enum AVPixelFormat* output_hw_format);
	int configure_audio_filters(VideoState* is, const char* intput_afilters, int force_output_format);
	int audio_open(void* opaque, AVChannelLayout* wanted_channel_layout, int wanted_sample_rate, struct AudioParams* audio_hw_params);
	int decoder_init(Decoder* d, AVCodecContext* avctx, PacketQueue* queue, SDL_cond* empty_queue_cond);
	void packet_queue_start(PacketQueue* q);
	void packet_queue_flush(PacketQueue* q);
	void step_to_next_frame(VideoState* is);
	void stream_toggle_pause(VideoState* is, bool cb_need = false);
	double get_clock(Clock* c);
	int packet_queue_put(PacketQueue* q, AVPacket* pkt);
	int packet_queue_put_nullpacket(PacketQueue* q, AVPacket* pkt, int stream_index);
	int packet_queue_put_private(PacketQueue* q, AVPacket* pkt);
	int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue);
	void stream_seek(VideoState* is, int64_t pos, int64_t rel, int by_bytes);
	int frame_queue_nb_remaining(FrameQueue* f);
	void sdl_audio_callback(void* opaque, Uint8* stream, int len);
	int audio_decode_frame(VideoState* is);
	Frame* frame_queue_peek_readable(FrameQueue* f);
	void frame_queue_next(FrameQueue* f);
	void frame_queue_unref_item(Frame* vp);
	int synchronize_audio(VideoState* is, int nb_samples);
	int get_master_sync_type(VideoState* is);
	double get_master_clock(VideoState* is);
	void update_sample_display(VideoState* is, short* samples, int samples_size);
	void sync_clock_to_slave(Clock* c, Clock* slave);

	// ---------------- 视频帧消费端 ----------------
	// 对齐样例 ffplay.cpp: video_refresh / video_display / send_video_image_to_upper
	// 没有它，pictq 写满 VIDEO_PICTURE_QUEUE_SIZE 帧后，FFVideoDecoder 会永远阻塞在
	// DecoderBase::frame_queue_peek_writable 的 SDL_CondWait 上
	void startVideoRefresh();
	void stopVideoRefresh();
	void videoRefreshLoop();
	void video_refresh(VideoState* is, double* remaining_time);
	void video_display(VideoState* is);
	void send_video_image_to_upper(VideoState* is);
	// 视频帧 -> VideoData(BGRA)，与 VideoRenderer::doRenderer 的渲染方式保持一致
	bool frameToVideoData(AVFrame* frame, double pts, double duration, VideoData& out);
	void resetVideoConverter();
	double compute_target_delay(double delay, VideoState* is);
	double vp_duration(VideoState* is, Frame* vp, Frame* nextvp);
	void update_video_pts(VideoState* is, double pts, int64_t pos, int serial);
	void check_external_clock_speed(VideoState* is);
	void set_clock_speed(Clock* c, double speed);
	Frame* frame_queue_peek(FrameQueue* f);
	Frame* frame_queue_peek_next(FrameQueue* f);
	Frame* frame_queue_peek_last(FrameQueue* f);
	void frame_queue_signal(FrameQueue* f);
	void packet_queue_abort(PacketQueue* q);
	void abortVideoDecoder();
	void abortAudioDecoder();

#if CONFIG_AVFILTER
	int configure_filtergraph(AVFilterGraph* graph, const char* filtergraph, AVFilterContext* source_ctx, AVFilterContext* sink_ctx);
#endif

private:
	MediaParameters* m_parameters;
	VideoState* m_videoState;
	AVFormatContext* m_format_context;
	std::atomic<bool> stream_ready = false;
	int startup_volume;
	AVDictionary* format_opts = NULL;
	int find_stream_info = 1;
	int genpts = 0;
	AVDictionary* codec_opts = NULL;
	int seek_by_bytes = -1;
	int64_t start_time = AV_NOPTS_VALUE;
	const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };
	int infinite_buffer = -1;
	std::atomic<bool> video_included = false;
	std::atomic<bool> audio_included = false;
	std::atomic<double> duration_seconds = 0.0;
	int fast = 0;
	enum AVPixelFormat hw_format = AVPixelFormat::AV_PIX_FMT_NONE;
	AVDictionary* swr_opts = NULL;
	SDL_AudioDeviceID audio_dev = 0;
#if CONFIG_AVFILTER
	std::string afilters = ""; // such as "atempo=tempo=3,volume=0.3"
	std::string vfilters_list = ""; // such as "setpts=PTS*0.5,vflip"
	int nb_vfilters = 0; // 1 represent video filter is included, no matter how many
#endif
	FFAudioDecoder* m_audioDecoder = NULL;
	FFVideoDecoder* m_videoDecoder = NULL;
	FFSubtitleDecoder* m_subtitleDecoder = NULL;
	AVBufferRef* hw_device_buf = nullptr;
	bool hw_decode_used = false;
	AVFormatContext* ic = NULL;
	AVPacket* pkt = NULL;
	bool eof_happened = false;
	SDL_mutex* wait_mutex;
	int64_t duration = AV_NOPTS_VALUE;
	int64_t audio_callback_time = 0;
	double frame_pts_begin = NAN;
	double frame_pts_end = NAN;
	double pts_pos = NAN;
	std::function<void(const VideoData&)> m_videoFrameCallback;
	std::function<void(int64_t, int64_t)> m_progressCallback;
	std::thread m_refreshThread;
	std::atomic<bool> m_refreshing{ false };
	// 像素格式转换（BGRA），只被视频刷新线程访问
	SwsContext* m_videoSwsCtx = nullptr;
	// 硬解帧 -> 软件帧的中间帧（复用，避免每帧 av_frame_alloc）
	AVFrame* m_videoSwFrame = nullptr;
};

