#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <string>
#include <vector>

namespace
{

class MainWindowNotifyListener : public MediaNotifyListener
{
public:
    explicit MainWindowNotifyListener(MainWindow* window)
        : m_window(window)
    {
    }

    void onStatusChanged(PlaybackState) override {}
    void onPlaybackProgress(int64_t, int64_t) override {}
    void onVideoDurationChanged(int64_t) override {}
    void onVolumeChanged(float) override {}
    void onVideoParamsChanged(float, float, float) override {}
    void onPlaybackSpeedChanged(float) override {}
    void onVideoFrame(const VideoData& data) override
    {
        m_window->onVideoFrameFromEngine(data);
    }
    void onAudioDevicesChanged(const std::vector<std::string>&) override {}
    void onMediaNotify(MediaType) override {}
    void onAudioSpectrum(const std::vector<float>&) override {}
    void onMusicMetadata(const MusicMetadata&) override {}

private:
    MainWindow* m_window;
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);

    m_engine = createMediaEngine();
    m_listener = new MainWindowNotifyListener(this);
    m_engine->setNotifyListener(m_listener);

    // 在黑色占位控件上叠加视频显示控件
    ui.videoWidget->setAttribute(Qt::WA_StyledBackground, true);
    auto* viewLayout = new QVBoxLayout(ui.videoWidget);
    viewLayout->setContentsMargins(0, 0, 0, 0);
    m_videoView = new VideoWidget(ui.videoWidget);
    viewLayout->addWidget(m_videoView);

    m_frameTimer = new QTimer(this);
    m_frameTimer->setInterval(40);
    connect(m_frameTimer, &QTimer::timeout, this, &MainWindow::onTimerCheckFrame);
    m_frameTimer->start();

    initConnect();
    // 优先加载上次选择并播放过的文件
    loadLastFile();
}

MainWindow::~MainWindow()
{
    m_frameTimer->stop();
    m_engine->stop();
    delete m_engine;
    m_engine = nullptr;
    delete m_listener;
    m_listener = nullptr;
}

void MainWindow::onVideoFrameFromEngine(const VideoData& data)
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    m_pendingFrame = data;
}

void MainWindow::onTimerCheckFrame()
{
    VideoData frame;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (!m_pendingFrame.data || m_pendingFrame.width <= 0 || m_pendingFrame.height <= 0)
        {
            return;
        }
        frame = m_pendingFrame;
        m_pendingFrame = VideoData();
    }
    m_videoView->setFrameBuffer(frame.data, frame.width, frame.height, frame.linesize);
}

void MainWindow::loadLastFile()
{
    QSettings settings(QApplication::applicationDirPath() + "/VVPlayer.ini", QSettings::IniFormat);
    const QString lastFile = settings.value("recent/lastFile").toString();
    if (lastFile.isEmpty())
    {
        return;
    }

    // 文件已不存在时静默忽略，避免界面上残留无效路径
    const QFileInfo info(lastFile);
    if (!info.exists())
    {
        return;
    }

    ui.editFilePath->setText(lastFile);
    // 预打开，点击播放即可直接播放
    m_engine->openFile(lastFile.toUtf8().constData());
}

void MainWindow::saveLastFile(const QString& filePath)
{
    QSettings settings(QApplication::applicationDirPath() + "/VVPlayer.ini", QSettings::IniFormat);
    settings.setValue("recent/lastFile", filePath);
    settings.sync();
}

void MainWindow::initConnect()
{
    connect(ui.btnSelectFile, &QPushButton::clicked, this, [this]() {
        // 重新选择文件时，默认打开当前选中文件所在的目录
        QString startDir;
        const QString currentFile = ui.editFilePath->text();
        if (!currentFile.isEmpty())
        {
            const QFileInfo info(currentFile);
            if (info.exists())
            {
                startDir = info.absolutePath();
            }
        }
        if (startDir.isEmpty())
        {
            startDir = QDir::homePath();
        }

        QString filePath = QFileDialog::getOpenFileName(
            this,
            "Select Video File",
            startDir,
            "Video Files (*.mp4 *.avi *.mkv *.mov *.flv *.wmv *.rmvb *.ts);;All Files (*)"
        );
        if (filePath.isEmpty())
        {
            return;
        }

        m_engine->openFile(filePath.toUtf8().constData());
        ui.editFilePath->setText(filePath);
        saveLastFile(filePath);
        // 选中文件后直接开始播放
        m_engine->play();
        ui.btnPlay->setText("Pause");
    });

    connect(ui.btnPlay, &QPushButton::clicked, this, [this]() {
        if (m_engine->getState() == PlaybackState::Playing)
        {
            m_engine->switchStatus();
            ui.btnPlay->setText("Play");
        }
        else
        {
            if (!m_engine->isOpened())
            {
                ui.btnSelectFile->click();
                return;
            }
            m_engine->switchStatus();
            ui.btnPlay->setText("Pause");
        }
    });

    connect(ui.btnStop, &QPushButton::clicked, this, [this]() {
        m_engine->stop();
        m_videoView->clearFrame();
        ui.btnPlay->setText("Play");
    });
}