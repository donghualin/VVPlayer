#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <mutex>
#include "ui_mainwindow.h"
#include "mediaenginebase.h"
#include "videowidget.h"

class QTimer;

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 引擎后台线程调用：把 BGRA 帧交到 UI 侧待处理槽位
    void onVideoFrameFromEngine(const VideoData& data);
    // 引擎后台线程调用：记录播放进度，UI 线程定时刷新到定位栏
    void onProgressFromEngine(int64_t currentMs);
    // 引擎后台线程调用：记录总时长，用于设置进度条范围
    void onDurationFromEngine(int64_t durationMs);

private slots:
    void onTimerCheckFrame();
    void onSliderPressed();
    void onSliderMoved(int value);
    void onSliderReleased();

private:
    void initConnect();
    void loadLastFile();
    void saveLastFile(const QString& filePath);
    void resetSeekBar();
    void updateSeekBar();

private:
    Ui::MainWindow ui;
    VideoWidget* m_videoView = nullptr;
    MediaEngineBase* m_engine = nullptr;
    MediaNotifyListener* m_listener = nullptr;
    QTimer* m_frameTimer = nullptr;

    std::mutex m_frameMutex;
    VideoData m_pendingFrame;

    // 进度/时长由引擎后台线程写入，UI 线程读取，加锁保护
    std::mutex m_posMutex;
    int64_t m_currentMs = 0;
    int64_t m_totalMs = 0;
    bool m_seeking = false; // 用户拖动进度条期间，暂停刷新滑块位置
};

#endif // MAINWINDOW_H