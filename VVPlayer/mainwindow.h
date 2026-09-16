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

private slots:
    void onTimerCheckFrame();

private:
    void initConnect();
    void loadLastFile();
    void saveLastFile(const QString& filePath);

private:
    Ui::MainWindow ui;
    VideoWidget* m_videoView = nullptr;
    MediaEngineBase* m_engine = nullptr;
    MediaNotifyListener* m_listener = nullptr;
    QTimer* m_frameTimer = nullptr;

    std::mutex m_frameMutex;
    VideoData m_pendingFrame;
};

#endif // MAINWINDOW_H