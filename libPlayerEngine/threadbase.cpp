/*
 * threadbase.cpp
 * 线程虚基类实现
 */
#include "threadbase.h"
#include <chrono>

ThreadBase::~ThreadBase()
{
    stop();
}

void ThreadBase::start()
{
    if (m_running)
        return;
    m_running = true;
    m_thread = std::thread(&ThreadBase::threadLoop, this);
}

void ThreadBase::stop()
{
    m_running = false;
    if (m_thread.get_id() != std::this_thread::get_id())
        join();
}

void ThreadBase::pause()
{
    m_paused = true;
}

void ThreadBase::resume()
{
    m_paused = false;
}

bool ThreadBase::isPaused() const
{
    return m_paused.load();
}

bool ThreadBase::isRunning() const
{
    return m_running;
}


bool ThreadBase::onReady()
{
    return true;
}

void ThreadBase::threadLoop()
{
    if (!onReady())
        return;

    while (m_running)
    {
        if (m_paused.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (!run())
        {
            break;
        }
    }
    m_running = false;
    onFinish();
}

void ThreadBase::join()
{
    if (m_thread.joinable())
        m_thread.join();
}
