#ifndef MEDIAINTERFACE_H
#define MEDIAINTERFACE_H

#include <cstdint>
#include <memory>
#include "libPlayerEngine_global.h"

// 自定义删除器：使用 av_free 释放 FFmpeg 分配的内存
struct AvFreeDeleter
{
    LIBPLAYERENGINESHARED_EXPORT void operator()(uint8_t* p) const;
};

// VideoData 使用 shared_ptr 管理帧数据，拷贝时只增加引用计数，
// 不再深拷贝 GB 级 BGRA 数据，解决 4K 视频内存爆炸和 CPU 卡顿问题。
// playerdemo 使用零拷贝 GPU 纹理(700MB)，本方案通过引用计数达到类似效果。
struct LIBPLAYERENGINESHARED_EXPORT VideoData
{
    int width = 0;
    int height = 0;
    int format = 0;
    int linesize = 0;
    std::shared_ptr<uint8_t> data;
    double pts = 0.0;
    double duration = 0.0;

    VideoData() = default;
    // 使用编译器默认的拷贝/移动/析构，shared_ptr 自动管理引用计数
};

#endif // MEDIAINTERFACE_H