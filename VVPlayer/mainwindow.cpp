#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <limits>
#include <string>
#include <vector>

namespace
{

// 毫秒格式化为 mm:ss 或 h:mm:ss
QString formatTimeMs(int64_t ms)
{
    if (ms < 0)
    {
        ms = 0;
    }
    const int64_t totalSeconds = ms / 1000;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds % 3600) / 60;
    const int64_t seconds = totalSeconds % 60;
    if (hours > 0)
    {
        return QString("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
    }
    return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

class MainWindowNotifyListener : public MediaNotifyListener
{
public:
    explicit MainWindowNotifyListener(MainWindow* window)
        : m_window(window)
    {
    }

    void onStatusChanged(PlaybackState) override {}
    void onPlaybackProgress(int64_t currentMs, int64_t) override
    {
        m_window->onProgressFromEngine(currentMs);
    }
    void onVideoDurationChanged(int64_t durationMs) override
    {
        m_window->onDurationFromEngine(durationMs);
    }
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

void MainWindow::onProgressFromEngine(int64_t currentMs)
{
    std::lock_guard<std::mutex> lock(m_posMutex);
    m_currentMs = currentMs;
}

void MainWindow::onDurationFromEngine(int64_t durationMs)
{
    std::lock_guard<std::mutex> lock(m_posMutex);
    m_totalMs = durationMs;
}

void MainWindow::onTimerCheckFrame()
{
    // 定位栏刷新与帧刷新独立，避免暂停时进度条停止响应
    updateSeekBar();

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

void MainWindow::updateSeekBar()
{
    int64_t currentMs = 0;
    int64_t totalMs = 0;
    {
        std::lock_guard<std::mutex> lock(m_posMutex);
        currentMs = m_currentMs;
        totalMs = m_totalMs;
    }

    // 拖动定位期间滑块位置由用户控制，只允许时长范围变化生效
    const int sliderMax = static_cast<int>(qMin<int64_t>(totalMs, std::numeric_limits<int>::max()));
    if (ui.sliderProgress->maximum() != sliderMax)
    {
        ui.sliderProgress->setMaximum(sliderMax);
        ui.labelTotalTime->setText(formatTimeMs(totalMs));
    }

    if (m_seeking)
    {
        return;
    }

    const int posMs = static_cast<int>(qBound<int64_t>(0, currentMs, sliderMax));
    if (ui.sliderProgress->value() != posMs)
    {
        ui.sliderProgress->setValue(posMs);
    }
    ui.labelCurrentTime->setText(formatTimeMs(currentMs));
}

void MainWindow::onSliderPressed()
{
    // 进入拖动定位状态，暂停用播放进度回写滑块
    m_seeking = true;
}

void MainWindow::onSliderMoved(int value)
{
    // 拖动过程中实时预览目标时间，但先不触发引擎 seek，避免频繁定位
    ui.labelCurrentTime->setText(formatTimeMs(static_cast<int64_t>(value)));
}

void MainWindow::onSliderReleased()
{
    const int targetMs = ui.sliderProgress->value();
    m_seeking = false;
    if (!m_engine->isOpened())
    {
        return;
    }
    // 定位播放：跳转到用户松开滑块时的位置
    m_engine->seekTo(static_cast<int64_t>(targetMs));
    {
        std::lock_guard<std::mutex> lock(m_posMutex);
        m_currentMs = targetMs;
    }
    ui.labelCurrentTime->setText(formatTimeMs(targetMs));
}

void MainWindow::resetSeekBar()
{
    {
        std::lock_guard<std::mutex> lock(m_posMutex);
        m_currentMs = 0;
        m_totalMs = 0;
    }
    m_seeking = false;
    ui.sliderProgress->setMaximum(0);
    ui.sliderProgress->setValue(0);
    ui.labelCurrentTime->setText(formatTimeMs(0));
    ui.labelTotalTime->setText(formatTimeMs(0));
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

        // 切换新文件时复位定位栏，避免残留上一个文件的进度
        resetSeekBar();
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
        resetSeekBar();
    });

    // 定位播放：拖动进度条，松开后跳转到对应位置
    connect(ui.sliderProgress, &QSlider::sliderPressed, this, &MainWindow::onSliderPressed);
    connect(ui.sliderProgress, &QSlider::sliderMoved, this, &MainWindow::onSliderMoved);
    connect(ui.sliderProgress, &QSlider::sliderReleased, this, &MainWindow::onSliderReleased);
}