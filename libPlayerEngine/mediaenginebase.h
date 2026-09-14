#pragma once

#include <string>
#include <vector>
#include <mutex>

#include "libPlayerEngine_global.h"
#include "medianotifylistener.h"

class LIBPLAYERENGINESHARED_EXPORT MediaEngineBase
{
public:
	// 实现体可能位于另一个 DLL（legacy 引擎在 libFFMediaEngine.dll 内），
	// 必须为虚析构，销毁才会回到创建它的模块内完成。
	virtual ~MediaEngineBase();
	virtual bool openFile(const char* filePath) = 0;
	virtual void close() = 0;
	virtual bool isOpened() const = 0;
	virtual std::string currentFile() const = 0;
	virtual void play() = 0;
	virtual void switchStatus() = 0;
	virtual void stop() = 0;
	virtual PlaybackState getState() const = 0;

	virtual void setVolume(float volume) = 0;
	virtual float getVolume() const = 0;
	virtual void setMute(bool mute) = 0;
	virtual bool isMuted() const = 0;

	virtual void setBrightness(float brightness) = 0;
	virtual float getBrightness() const = 0;
	virtual void setContrast(float contrast) = 0;
	virtual float getContrast() const = 0;
	virtual void setSaturation(float saturation) = 0;
	virtual float getSaturation() const = 0;

	virtual void seekTo(int64_t positionMs) = 0;
	virtual int64_t getCurrentPosition() const = 0;
	virtual int64_t getDuration() const = 0;

	virtual int getAudioTrackCount() const = 0;
	virtual int getCurrentAudioTrack() const = 0;
	virtual void switchAudioTrack(int trackIndex) = 0;

	virtual void setSubtitleFile(const char* subtitlePath) = 0;

	// 内嵌字幕轨操作（仅对含内嵌字幕流的容器有效）
	virtual int getSubtitleTrackCount() const = 0;
	virtual int getCurrentSubtitleTrack() const = 0;
	virtual void switchSubtitleTrack(int trackIndex) = 0;

	virtual void setPlaybackSpeed(float speed) = 0;
	virtual float getPlaybackSpeed() const = 0;
	void setNotifyListener(MediaNotifyListener* listener);

	virtual void setAudioDevice(const char* deviceName) = 0;
	virtual const char* getAudioDevice() const = 0;
	std::vector<std::string> getAudioOutputDevices();

protected:
	MediaEngineBase();
	MediaNotifyListener* listener() const;

private:
	MediaNotifyListener* m_listener;
};

/// 工厂：创建媒体引擎实例（具体引擎在 DLL 内部实现，外部只持有 MediaEngineBase 接口）
extern "C" LIBPLAYERENGINESHARED_EXPORT MediaEngineBase* createMediaEngine();

