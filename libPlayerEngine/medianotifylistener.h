/*
 * medianotifylistener.h
 */
#ifndef MEDIANOTIFYLISTENER_H
#define MEDIANOTIFYLISTENER_H

#include <cstdint>
#include <vector>
#include <string>
#include "libPlayerEngine_global.h"
#include "mediainterface.h"

// 播放状态枚举
enum class PlaybackState : int
{
    Stopped = 0,
    Playing = 1,
    Paused  = 2
};

// 媒体类型枚举
enum class MediaType : int
{
    Video = 0,
    Music = 1
};

// 音乐元数据结构（从音频文件标签中提取）
struct LIBPLAYERENGINESHARED_EXPORT MusicMetadata
{
    std::string title;         // TIT2 - 歌曲标题
    std::string artist;        // TPE1 - 歌手
    std::string albumArtist;   // TPE2 - 专辑艺术家
    std::string album;         // TALB - 专辑名
    std::string genre;         // TCON - 音乐流派
    std::string year;          // TYER - 发行年份
    std::string composer;      // TCOM - 作曲
    std::string publisher;     // TPUB - 发行商
    std::string subtitle;      // TIT3 - 副标题
    std::string url;           // WXXX - 链接

    // 嵌入封面（attached_pic 解码后的 BGRA 数据），通过 onMusicMetadata 一次性传递给 UI
    // data 为 null 表示该音乐文件无嵌入封面，UI 端应显示默认封面
    VideoData cover;

    bool isEmpty() const
    {
        return title.empty() && artist.empty() && albumArtist.empty() &&
               album.empty() && genre.empty() && year.empty() &&
               composer.empty() && publisher.empty() && subtitle.empty() && url.empty();
    }

    bool hasCover() const
    {
        return cover.width > 0 && cover.height > 0 && cover.data != nullptr;
    }
};

class LIBPLAYERENGINESHARED_EXPORT MediaNotifyListener
{
public:
    virtual ~MediaNotifyListener() = default;
    virtual void onStatusChanged(PlaybackState state) = 0;
    virtual void onPlaybackProgress(int64_t currentMs, int64_t totalMs) = 0;
    virtual void onVideoDurationChanged(int64_t durationMs) = 0;
    virtual void onVolumeChanged(float volume) = 0;
    virtual void onVideoParamsChanged(float brightness, float contrast, float saturation) = 0;
    virtual void onPlaybackSpeedChanged(float speed) = 0;
    virtual void onVideoFrame(const VideoData& data) = 0;
    virtual void onAudioDevicesChanged(const std::vector<std::string>& devices) = 0;
    virtual void onMediaNotify(MediaType type) = 0;

    // 音频频谱数据回调（FFT 分析结果，频段归一化幅值 0.0~1.0）
    virtual void onAudioSpectrum(const std::vector<float>& bands) = 0;

    // 音乐元数据回调（启动播放时从音频文件标签提取）
    virtual void onMusicMetadata(const MusicMetadata& metadata) = 0;

    // 字幕文本变化回调（空串表示当前时刻无字幕应显示）。
    // 提供默认空实现，避免破坏未关心字幕的旧实现者。
    virtual void onSubtitle(const std::string& text) { (void)text; }
};
#endif