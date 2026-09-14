#include "mediaenginebase.h"

void MediaEngineBase::setNotifyListener(MediaNotifyListener* listener)
{
	m_listener = listener;
}

std::vector<std::string> MediaEngineBase::getAudioOutputDevices()
{
	return {};
}

MediaEngineBase::MediaEngineBase()
	: m_listener(nullptr)
{
}

MediaEngineBase::~MediaEngineBase()
{
}

MediaNotifyListener* MediaEngineBase::listener() const
{
	return m_listener;
}
