/*
 * threadbase.h
 * 线程虚基类 -- 提供 bool run() 接口
 * run() 返回 true 继续下一个循环，返回 false 线程退出
 */
#ifndef THREADBASE_H
#define THREADBASE_H

#include <thread>
#include <atomic>

class ThreadBase
{
public:
    virtual ~ThreadBase();

    void start();

    void stop();

    void pause();
    void resume();
    bool isPaused() const;

    bool isRunning() const;

protected:
    /// 子类实现：每次循环调用，返回 true 继续，false 退出
    virtual bool run() = 0;

    /// 循环开始前调用（while 之前），子类可重写做准备工作
    virtual bool onReady();

    /// 循环退出后调用（while 之后），子类可重写做清理工作
    virtual void onFinish() {}

private:
    void threadLoop();

    void join();

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
};

#endif // THREADBASE_H
