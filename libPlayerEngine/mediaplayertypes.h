#pragma once

#include <string>

#include "config.h"

extern "C"
{
#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
#include "libavformat/avformat.h"
#include "libavutil/fifo.h"
#include "libavfilter/avfilter.h"
#include "libavcodec/avcodec.h"
#include "SDL2/SDL_render.h"
#include "SDL2/SDL_mutex.h"

struct AVFilterContext;
}

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

#define SAMPLE_ARRAY_SIZE (8 * 65536)

enum
{
	AV_SYNC_AUDIO_MASTER, /* default choice */
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

enum ShowMode {
	SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
};

struct MediaParameters
{
	std::string file_name = "";

	// we can control the play speed by av filters
	std::string audio_filter = ""; // "atempo=tempo=2"
	std::string video_filter = ""; // "setpts=PTS*0.5"

	bool disable_debug_render = false;
	bool disable_video = false;
	bool disable_audio = false;
	bool disable_subtitle = true; // disable subtitle

	int av_sync_type;

	int loop = 1; // 0 means loop forever
	bool auto_exit_when_eof = false; // it will be ignore if loop is 0

	bool auto_rotate = true;
	int filter_nbthreads = 0;

	int framedrop = -1; // "drop frames when cpu is too slow"
	int infinite_buffer = -1; // don't limit the input buffer size (useful with realtime streams)

	bool hw_decode = false; // it will be ignored if min_hw_image_size is not matched
};

typedef struct MyAVPacketList {
	AVPacket* pkt;
	int serial;
} MyAVPacketList;

typedef struct PacketQueue {
	AVFifo* pkt_list;
	int nb_packets;
	int size;
	int64_t duration;
	int abort_request;
	int serial;
	SDL_mutex* mutex;
	SDL_cond* cond;
} PacketQueue;

typedef struct Clock {
	double pts;           /* clock base */
	double pts_drift;     /* clock base minus time at which we updated the clock */
	double last_updated;
	double speed;
	int serial;           /* clock is based on a packet with this serial */
	int paused;
	int* queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
	AVFrame* frame;
	AVSubtitle sub;
	int serial;
	double pts;           /* presentation timestamp for the frame */
	double duration;      /* estimated duration of the frame */
	int64_t pos;          /* byte position of the frame in the input file */
	int width;
	int height;
	int format;
	AVRational sar;
	int uploaded;
	int flip_v;
} Frame;

typedef struct FrameQueue {
	Frame queue[FRAME_QUEUE_SIZE];
	int rindex;
	int windex;
	int size;
	int max_size;
	int keep_last;
	int rindex_shown;
	SDL_mutex* mutex;
	SDL_cond* cond;
	PacketQueue* pktq;
} FrameQueue;

typedef struct Decoder {
	AVPacket* pkt;
	PacketQueue* queue;
	AVCodecContext* avctx;
	int pkt_serial;
	int finished;
	int packet_pending;
	SDL_cond* empty_queue_cond;
	int64_t start_pts;
	AVRational start_pts_tb;
	int64_t next_pts;
	AVRational next_pts_tb;
	SDL_Thread* decoder_tid;
} Decoder;

struct AudioParams {
	int freq;
	AVChannelLayout ch_layout;
	enum AVSampleFormat fmt;
	int frame_size;
	int bytes_per_sec;
};

typedef struct VideoState {
	SDL_Thread* read_tid;
	const AVInputFormat* iformat = NULL;
	int abort_request;
	int force_refresh;
	int paused;
	int last_paused;
	int queue_attachments_req;
	int seek_req;
	int seek_flags;
	int64_t seek_pos;
	int64_t seek_rel;
	int read_pause_return;
	AVFormatContext* ic = NULL;
	int realtime;

	Clock audclk;
	Clock vidclk;
	Clock extclk;

	FrameQueue pictq;
	FrameQueue subpq;
	FrameQueue sampq;

	Decoder auddec;
	Decoder viddec;
	Decoder subdec;

	int audio_stream = -1;

	int av_sync_type;

	double audio_clock;
	int audio_clock_serial;
	double audio_diff_cum; /* used for AV difference average computation */
	double audio_diff_avg_coef;
	double audio_diff_threshold;
	int audio_diff_avg_count;
	AVStream* audio_st;
	PacketQueue audioq;
	int audio_hw_buf_size;
	uint8_t* audio_buf;
	uint8_t* audio_buf1;
	unsigned int audio_buf_size; /* in bytes */
	unsigned int audio_buf1_size;
	int audio_buf_index; /* in bytes */
	int audio_write_buf_size;
	int audio_volume;
	int muted;
	struct AudioParams audio_src;
#if CONFIG_AVFILTER
	struct AudioParams audio_filter_src;
#endif
	struct AudioParams audio_tgt;
	struct SwrContext* swr_ctx;
	int frame_drops_early;
	int frame_drops_late;

	enum ShowMode show_mode;
	int16_t sample_array[SAMPLE_ARRAY_SIZE];
	int sample_array_index;
	int last_i_start;
	// RDFTContext* rdft;
	int rdft_bits;
	// FFTSample* rdft_data;
	int xpos;
	double last_vis_time;
	SDL_Texture* vis_texture;
	SDL_Texture* sub_texture;
	SDL_Texture* vid_texture;

	int subtitle_stream = -1;
	AVStream* subtitle_st;
	PacketQueue subtitleq;

	double frame_timer;
	double frame_last_returned_time;
	double frame_last_filter_delay;
	int video_stream = -1;
	AVStream* video_st;
	PacketQueue videoq;
	double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
	struct SwsContext* img_convert_ctx;
	struct SwsContext* sub_convert_ctx;
	int eof;

	char* filename;
	int width, height, xleft = 0, ytop = 0;
	int step;

#if CONFIG_AVFILTER
	int vfilter_idx;
	AVFilterContext* in_video_filter;   // the first filter in the video chain
	AVFilterContext* out_video_filter;  // the last filter in the video chain
	AVFilterContext* in_audio_filter;   // the first filter in the audio chain
	AVFilterContext* out_audio_filter;  // the last filter in the audio chain
	AVFilterGraph* agraph;              // audio filter graph
#endif

	int last_video_stream = -1;
	int last_audio_stream = -1;
	int last_subtitle_stream = -1;

	SDL_cond* continue_read_thread;
} VideoState;
