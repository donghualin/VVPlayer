#include "videowidget.h"

#include <QPainter>
#include <QPaintEvent>

VideoWidget::VideoWidget(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
}

void VideoWidget::setFrameBuffer(const std::shared_ptr<uint8_t>& buffer, int width, int height, int stride)
{
    if (!buffer || width <= 0 || height <= 0 || stride <= 0)
    {
        return;
    }
    // BGRA(内存序 B,G,R,A) 与 Format_ARGB32 (0xAARRGGBB) 在小端平台上字节排布一致，
    // QImage 直接引用 buffer 的内存，不拷贝。
    m_image = QImage(buffer.get(), width, height, stride, QImage::Format_ARGB32);
    m_buffer = buffer;
    m_hasFrame = true;
    update();
}

void VideoWidget::clearFrame()
{
    m_buffer.reset();
    m_image = QImage();
    m_hasFrame = false;
    update();
}

void VideoWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (!m_hasFrame || m_image.isNull())
    {
        return;
    }

    // 保持宽高比缩放，居中绘制
    QSize target = m_image.size();
    QSize view = size();
    if (view.width() > 0 && view.height() > 0)
    {
        target.scale(view, Qt::KeepAspectRatio);
    }
    if (target.width() <= 0 || target.height() <= 0)
    {
        return;
    }

    QRect dst(QPoint(0, 0), target);
    dst.moveCenter(rect().center());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(dst, m_image);
}