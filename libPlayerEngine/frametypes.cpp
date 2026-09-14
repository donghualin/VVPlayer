/*
 * frametypes.cpp
 * 帧数据和包数据结构实现
 */
#include "frametypes.h"

AVPacketData::AVPacketData() : pkt()
{
    pkt.data = nullptr;
    pkt.size = 0;
    pkt.buf = nullptr;
    pkt.stream_index = -1;
    pkt.pts = AV_NOPTS_VALUE;
    pkt.dts = AV_NOPTS_VALUE;
}

AVPacketData::~AVPacketData()
{
    av_packet_unref(&pkt);
}

AVPacketData::AVPacketData(const AVPacketData& other) : pkt()
{
    av_packet_ref(&pkt, &other.pkt);
}

AVPacketData& AVPacketData::operator=(const AVPacketData& other)
{
    if (this != &other)
    {
        av_packet_unref(&pkt);
        av_packet_ref(&pkt, &other.pkt);
    }
    return *this;
}

AVPacketData::AVPacketData(AVPacketData&& other) noexcept : pkt()
{
    av_packet_move_ref(&pkt, &other.pkt);
}

AVPacketData& AVPacketData::operator=(AVPacketData&& other) noexcept
{
    if (this != &other)
    {
        av_packet_unref(&pkt);
        av_packet_move_ref(&pkt, &other.pkt);
    }
    return *this;
}
