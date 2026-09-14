#pragma once
#include "mediaenginebase.h"

class MediaParameters;
class FFDemuxer;

class FFMediaEngineImpl : public MediaEngineBase
{
public:
	FFMediaEngineImpl();
	~FFMediaEngineImpl();

protected:
	bool openFile(const char* filePath) override;
	void close() override;
	bool isOpened() const override;
	std::string currentFile() const override;
	void play() override;
	void switchStatus() override;
	void stop() override;
	PlaybackState getState() const override;

	void setVolume(float volume) override;
	float getVolume() const override;
	void setMute(bool mute) override;
	bool isMuted() const override;

	void setBrightness(float brightness) override;
	float getBrightness() const override;
	void setContrast(float contrast) override;
	float getContrast() const override;
	void setSaturation(float saturation) override;
	float getSaturation() const override;

	void seekTo(int64_t positionMs) override;
	int64_t getCurrentPosition() const override;
	int64_t getDuration() const override;

	int getAudioTrackCount() const override;
	int getCurrentAudioTrack() const override;
	void switchAudioTrack(int trackIndex) override;

	void setSubtitleFile(const char* subtitlePath) override;

	// 内嵌字幕轨操作（仅对含内嵌字幕流的容器有效）
	int getSubtitleTrackCount() const override;
	int getCurrentSubtitleTrack() const override;
	void switchSubtitleTrack(int trackIndex) override;

	void setPlaybackSpeed(float speed) override;
	float getPlaybackSpeed() const override;

	void setAudioDevice(const char* deviceName) override;
	const char* getAudioDevice() const override;

private:
	MediaParameters* m_parameters;
	FFDemuxer* m_demutex;
};
