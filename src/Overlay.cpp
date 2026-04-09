#include "Overlay.h"
#include <QPainter>
#include <QFontMetrics>

void OverlayRenderer::renderTextOverlay(QImage &frame, const TextOverlay &overlay, double currentTime)
{
    if (!overlay.visible) return;
    if (currentTime < overlay.startTime) return;
    if (overlay.endTime > 0 && currentTime > overlay.endTime) return;

    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    painter.setFont(overlay.font);
    QFontMetrics fm(overlay.font);
    QRect textBounds = fm.boundingRect(QRect(0, 0, frame.width() - 40, 0),
        overlay.alignment | Qt::TextWordWrap, overlay.text);

    int px = static_cast<int>(overlay.x * frame.width()) - textBounds.width() / 2;
    int py = static_cast<int>(overlay.y * frame.height()) - textBounds.height() / 2;
    QRect drawRect(px, py, textBounds.width(), textBounds.height());

    // Background
    if (overlay.backgroundColor.alpha() > 0) {
        painter.fillRect(drawRect.adjusted(-8, -4, 8, 4), overlay.backgroundColor);
    }

    // Outline
    if (overlay.outlineWidth > 0) {
        QPainterPath path;
        path.addText(px, py + fm.ascent(), overlay.font, overlay.text);
        painter.setPen(QPen(overlay.outlineColor, overlay.outlineWidth));
        painter.drawPath(path);
    }

    // Text
    painter.setPen(overlay.color);
    painter.drawText(drawRect, overlay.alignment | Qt::TextWordWrap, overlay.text);
}

void OverlayRenderer::renderImageOverlay(QImage &frame, const ImageOverlay &overlay, double currentTime)
{
    if (!overlay.visible) return;
    if (currentTime < overlay.startTime) return;
    if (overlay.endTime > 0 && currentTime > overlay.endTime) return;

    QImage img(overlay.filePath);
    if (img.isNull()) return;

    int x = static_cast<int>(overlay.rect.x() * frame.width());
    int y = static_cast<int>(overlay.rect.y() * frame.height());
    int w = static_cast<int>(overlay.rect.width() * frame.width());
    int h = static_cast<int>(overlay.rect.height() * frame.height());

    QImage scaled;
    if (overlay.keepAspectRatio)
        scaled = img.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    else
        scaled = img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QPainter painter(&frame);
    painter.setOpacity(overlay.opacity);
    painter.drawImage(x, y, scaled);
}

void OverlayRenderer::renderPip(QImage &frame, const QImage &pipSource, const PipConfig &config)
{
    if (!config.visible || pipSource.isNull()) return;

    int x = static_cast<int>(config.rect.x() * frame.width());
    int y = static_cast<int>(config.rect.y() * frame.height());
    int w = static_cast<int>(config.rect.width() * frame.width());
    int h = static_cast<int>(config.rect.height() * frame.height());

    QImage scaled = pipSource.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QPainter painter(&frame);
    painter.setOpacity(config.opacity);

    // Border
    if (config.borderWidth > 0) {
        painter.setPen(QPen(config.borderColor, config.borderWidth));
        painter.drawRect(x - 1, y - 1, scaled.width() + 1, scaled.height() + 1);
    }

    painter.drawImage(x, y, scaled);
}

QImage OverlayRenderer::applyTransition(const QImage &from, const QImage &to,
    TransitionType type, double progress)
{
    if (type == TransitionType::None) return (progress < 0.5) ? from : to;

    int w = qMax(from.width(), to.width());
    int h = qMax(from.height(), to.height());
    QImage result(w, h, QImage::Format_RGB888);
    QPainter painter(&result);

    QImage fromScaled = from.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage toScaled = to.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    switch (type) {
    case TransitionType::FadeIn:
        painter.drawImage(0, 0, fromScaled);
        painter.setOpacity(progress);
        painter.drawImage(0, 0, toScaled);
        break;

    case TransitionType::FadeOut:
        painter.drawImage(0, 0, toScaled);
        painter.setOpacity(1.0 - progress);
        painter.drawImage(0, 0, fromScaled);
        break;

    case TransitionType::CrossDissolve:
        painter.setOpacity(1.0 - progress);
        painter.drawImage(0, 0, fromScaled);
        painter.setOpacity(progress);
        painter.drawImage(0, 0, toScaled);
        break;

    case TransitionType::WipeLeft: {
        int boundary = static_cast<int>(w * progress);
        painter.drawImage(0, 0, fromScaled);
        painter.setClipRect(0, 0, boundary, h);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::WipeRight: {
        int boundary = static_cast<int>(w * (1.0 - progress));
        painter.drawImage(0, 0, fromScaled);
        painter.setClipRect(boundary, 0, w - boundary, h);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::WipeUp: {
        int boundary = static_cast<int>(h * progress);
        painter.drawImage(0, 0, fromScaled);
        painter.setClipRect(0, 0, w, boundary);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::WipeDown: {
        int boundary = static_cast<int>(h * (1.0 - progress));
        painter.drawImage(0, 0, fromScaled);
        painter.setClipRect(0, boundary, w, h - boundary);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::SlideLeft: {
        int offset = static_cast<int>(w * progress);
        painter.drawImage(-offset, 0, fromScaled);
        painter.drawImage(w - offset, 0, toScaled);
        break;
    }

    case TransitionType::SlideRight: {
        int offset = static_cast<int>(w * progress);
        painter.drawImage(offset, 0, fromScaled);
        painter.drawImage(-w + offset, 0, toScaled);
        break;
    }

    default:
        painter.drawImage(0, 0, (progress < 0.5) ? fromScaled : toScaled);
        break;
    }

    return result;
}
