#ifndef FRAMETYPES_H
#define FRAMETYPES_H

#include <cstdint>
#include "mediainterface.h"

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/frame.h"
#include "libavutil/rational.h"
}
/*
 * frametypes.h
 * 帧数据和包数据结构定义
 */

/// 解码后的音频帧
struct AudioFrame
{
    uint8_t* data = nullptr;
    int      size = 0;
    double   pts  = 0.0;   // 播放时间戳（秒）

    enum AVSampleFormat sampleFmt = AV_SAMPLE_FMT_NONE;
    int sampleRate = 0;
    int channels = 0;
    uint64_t channelLayout = 0;
};

/// 解码后的视频帧（内部使用，包含 FFmpeg 类型）
struct VideoFrame
{
    AVFrame* frame    = nullptr;
    double   pts      = 0.0;
    double   duration = 0.0;
};

/// AVPacket 所有权包装，支持拷贝（av_packet_ref）和移动（av_packet_move_ref）
struct AVPacketData
{
    AVPacket pkt;

    AVPacketData();
    ~AVPacketData();

    AVPacketData(const AVPacketData& other);
    AVPacketData& operator=(const AVPacketData& other);
    AVPacketData(AVPacketData&& other) noexcept;
    AVPacketData& operator=(AVPacketData&& other) noexcept;
};

/// 常量定义
static const int AUDIO_PACKET_QUEUE_MAX   = 50;
static const int VIDEO_PACKET_QUEUE_MAX   = 50;
static const int AUDIO_FRAME_QUEUE_MAX    = 20;
static const int VIDEO_FRAME_QUEUE_MAX    = 30;
static const int AUDIO_CALLBACK_SAMPLES   = 2048;
static const double AV_SYNC_THRESHOLD     = 0.01;

#endif // FRAMETYPES_H
