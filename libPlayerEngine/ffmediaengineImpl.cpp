#include "ffmediaengineImpl.h"
#include "mediaplayertypes.h"
#include "ffdemuxer.h"

FFMediaEngineImpl::FFMediaEngineImpl()
	: MediaEngineBase()
	, m_parameters(new MediaParameters)
	, m_demutex(new FFDemuxer)
{
	// 解码后的视频帧（BGRA）由 FFDemuxer 的刷新线程转好，这里直接转给上层渲染
	m_demutex->setVideoFrameCallback([this](const VideoData& data)
	{
		if (listener())
		{
			listener()->onVideoFrame(data);
		}
	});
}

FFMediaEngineImpl::~FFMediaEngineImpl()
{
	stop();
	delete m_demutex;
	delete m_parameters;
}

bool FFMediaEngineImpl::openFile(const char* filePath)
{
	m_parameters->file_name = filePath;
	if (m_parameters->file_name.empty())
	{
		return false;
	}
	m_parameters->loop = 1;
	m_parameters->hw_decode = true;
	m_parameters->disable_debug_render = false;
	m_parameters->av_sync_type = AV_SYNC_AUDIO_MASTER;
	return true;
}

void FFMediaEngineImpl::close()
{
	m_demutex->stop();
}

bool FFMediaEngineImpl::isOpened() const
{
	return m_demutex->isRunning();
}

std::string FFMediaEngineImpl::currentFile() const
{
	return m_parameters->file_name;
}

void FFMediaEngineImpl::play()
{
	m_demutex->startPlay(m_parameters);
}

void FFMediaEngineImpl::switchStatus()
{

}

void FFMediaEngineImpl::stop()
{
	m_demutex->stop();
}

PlaybackState FFMediaEngineImpl::getState() const
{
	return PlaybackState::Stopped;
}

void FFMediaEngineImpl::setVolume(float volume)
{
}

float FFMediaEngineImpl::getVolume() const
{
	return 100;
}

void FFMediaEngineImpl::setMute(bool mute)
{

}

bool FFMediaEngineImpl::isMuted() const
{
	return true;
}

void FFMediaEngineImpl::setBrightness(float brightness)
{

}

float FFMediaEngineImpl::getBrightness() const
{
	return 1.0f;
}

void FFMediaEngineImpl::setContrast(float contrast)
{

}

float FFMediaEngineImpl::getContrast() const
{
	return 1.0f;
}

void FFMediaEngineImpl::setSaturation(float saturation)
{

}

float FFMediaEngineImpl::getSaturation() const
{
	return 1.f;
}

void FFMediaEngineImpl::seekTo(int64_t positionMs)
{

}

int64_t FFMediaEngineImpl::getCurrentPosition() const
{
	return 0;
}

int64_t FFMediaEngineImpl::getDuration() const
{
	return 0;
}

int FFMediaEngineImpl::getAudioTrackCount() const
{
	return 2;
}

int FFMediaEngineImpl::getCurrentAudioTrack() const
{
	return 0;
}

void FFMediaEngineImpl::switchAudioTrack(int trackIndex)
{

}

void FFMediaEngineImpl::setSubtitleFile(const char* subtitlePath)
{

}

int FFMediaEngineImpl::getSubtitleTrackCount() const
{
	return 3;
}

int FFMediaEngineImpl::getCurrentSubtitleTrack() const
{
	return 1;
}

void FFMediaEngineImpl::switchSubtitleTrack(int trackIndex)
{

}

void FFMediaEngineImpl::setPlaybackSpeed(float speed)
{

}

float FFMediaEngineImpl::getPlaybackSpeed() const
{
	return 1.0f;
}

void FFMediaEngineImpl::setAudioDevice(const char* deviceName)
{

}

const char* FFMediaEngineImpl::getAudioDevice() const
{
	return "";
}
