#include "ffmediaengineImpl.h"
#include "mediaplayertypes.h"
#include "ffdemuxer.h"

FFMediaEngineImpl::FFMediaEngineImpl()
	: MediaEngineBase()
	, m_parameters(new MediaParameters)
	, m_demutex(nullptr)
	, m_opened(false)
{
}

FFMediaEngineImpl::~FFMediaEngineImpl()
{
	stop();
	delete m_demutex;
	m_demutex = nullptr;
	delete m_parameters;
}

void FFMediaEngineImpl::ensureDemuxer()
{
	if (m_demutex)
	{
		return;
	}
	m_demutex = new FFDemuxer;
	// 解码后的视频帧（BGRA）由 FFDemuxer 的刷新线程转好，这里直接转给上层渲染
	m_demutex->setVideoFrameCallback([this](const VideoData& data)
	{
		if (listener())
		{
			listener()->onVideoFrame(data);
		}
	});
}

bool FFMediaEngineImpl::openFile(const char* filePath)
{
	if (!filePath || !*filePath)
	{
		m_opened = false;
		return false;
	}
	// 若正在播放，先停掉旧流
	stop();

	m_parameters->file_name = filePath;
	m_parameters->loop = 1;
	m_parameters->hw_decode = true;
	m_parameters->disable_debug_render = false;
	m_parameters->av_sync_type = AV_SYNC_AUDIO_MASTER;
	m_opened = true;
	return true;
}

void FFMediaEngineImpl::close()
{
	stop();
	m_opened = false;
}

bool FFMediaEngineImpl::isOpened() const
{
	return m_opened;
}

std::string FFMediaEngineImpl::currentFile() const
{
	return m_parameters->file_name;
}

void FFMediaEngineImpl::play()
{
	if (!m_opened || m_parameters->file_name.empty())
	{
		return;
	}

	ensureDemuxer();
	if (m_demutex->isRunning())
	{
		// 已暂停则恢复，否则保持播放状态
		if (m_demutex->isPaused())
		{
			m_demutex->resume();
		}
		return;
	}

	m_demutex->startPlay(m_parameters);
}

void FFMediaEngineImpl::switchStatus()
{
	if (!m_demutex || !m_demutex->isRunning())
	{
		play();
		return;
	}

	if (m_demutex->isPaused())
	{
		m_demutex->resume();
	}
	else
	{
		m_demutex->pause();
	}
}

void FFMediaEngineImpl::stop()
{
	if (m_demutex)
	{
		m_demutex->stop();
		delete m_demutex;
		m_demutex = nullptr;
	}
}

PlaybackState FFMediaEngineImpl::getState() const
{
	if (!m_demutex || !m_demutex->isRunning())
	{
		return PlaybackState::Stopped;
	}
	return m_demutex->isPaused() ? PlaybackState::Paused : PlaybackState::Playing;
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
	return 0;
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
	return 0;
}

int FFMediaEngineImpl::getCurrentSubtitleTrack() const
{
	return 0;
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

extern "C" LIBPLAYERENGINESHARED_EXPORT MediaEngineBase* createMediaEngine()
{
	return new FFMediaEngineImpl();
}