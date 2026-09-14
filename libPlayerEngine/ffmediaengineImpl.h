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

	// ��Ƕ��Ļ����������Ժ���Ƕ��Ļ����������Ч��
	int getSubtitleTrackCount() const override;
	int getCurrentSubtitleTrack() const override;
	void switchSubtitleTrack(int trackIndex) override;

	void setPlaybackSpeed(float speed) override;
	float getPlaybackSpeed() const override;

	void setAudioDevice(const char* deviceName) override;
	const char* getAudioDevice() const override;

private:
	// 解码后的视频帧（BGRA）由 FFDemuxer 的刷新线程转好，这里直接转给上层渲染
	void ensureDemuxer();

private:
	MediaParameters* m_parameters;
	FFDemuxer* m_demutex;
	bool m_opened;
};
