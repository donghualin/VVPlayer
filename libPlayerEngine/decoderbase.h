#pragma once

#include "threadbase.h"
#include <functional>

struct VideoState;
struct Decoder;
struct MediaParameters;
struct FrameQueue;
struct Frame;
struct Clock;
struct PacketQueue;

extern "C"
{
	struct AVFrame;
	struct AVSubtitle;
	struct AVPacket;
}

class DecoderBase : public ThreadBase
{
public:
	DecoderBase();
	~DecoderBase();
	void setVideoState(VideoState* vs);
	void setMediaParameters(MediaParameters* parameters);
	void setGetMasterClock(std::function <double(VideoState*)> f);

protected:
	bool run() override;

protected:
	int get_video_frame(VideoState* is, AVFrame* frame);
	int decoder_decode_frame(Decoder* d, AVFrame* frame, AVSubtitle* sub);
	int get_master_sync_type(VideoState* is);
	Frame* frame_queue_peek_writable(FrameQueue* f);
	void frame_queue_push(FrameQueue* f);
	double get_master_clock(VideoState* is);
	int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block, int* serial);

protected:
	VideoState* m_videoState;
	MediaParameters* m_parameters;
	std::function <double(VideoState*)> m_getMasterClock;

private:
	int decoder_reorder_pts = -1;
};
