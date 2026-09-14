#ifndef VIDEOWIDGET_H
#define VIDEOWIDGET_H

#include <QWidget>
#include <QImage>
#include <cstdint>
#include <memory>

// 视频显示控件：把引擎输出的 BGRA 帧（VideoData.data）以保持宽高比的方式绘制到控件上。
// 帧数据以 shared_ptr 持有，QImage 只是引用其内存，不产生整帧拷贝。
class VideoWidget : public QWidget
{
public:
    explicit VideoWidget(QWidget *parent = nullptr);

    void setFrameBuffer(const std::shared_ptr<uint8_t>& buffer, int width, int height, int stride);
    void clearFrame();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::shared_ptr<uint8_t> m_buffer;
    QImage m_image;
    bool m_hasFrame = false;
};

#endif // VIDEOWIDGET_H