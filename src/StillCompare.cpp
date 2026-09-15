#include "StillCompare.h"

#include <QPainter>
#include <QtGlobal>

namespace stillcompare {
namespace {

QImage letterboxed(const QImage &source, const QSize &size)
{
    if (source.isNull() || size.isEmpty())
        return {};

    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::black);
    const QImage scaled = source.scaled(size, Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation);
    QPainter painter(&result);
    painter.drawImage((size.width() - scaled.width()) / 2,
                      (size.height() - scaled.height()) / 2,
                      scaled);
    return result;
}

void drawPanel(QPainter &painter, const QRect &panel, const QImage &source)
{
    if (panel.isEmpty() || source.isNull())
        return;

    painter.fillRect(panel, Qt::black);
    const QImage scaled = source.scaled(panel.size(), Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation);
    const QPoint topLeft(panel.x() + (panel.width() - scaled.width()) / 2,
                         panel.y() + (panel.height() - scaled.height()) / 2);
    painter.drawImage(topLeft, scaled);
}

} // namespace

QImage apply(const QImage &display, const QImage &still,
             Mode mode, double position)
{
    if (display.isNull() || still.isNull())
        return display;

    const double p = qBound(0.0, position, 1.0);
    if (mode == Mode::SplitSideBySide) {
        QImage result(display.size(), QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::black);
        const int leftWidth = display.width() / 2;
        const QRect leftPanel(0, 0, leftWidth, display.height());
        const QRect rightPanel(leftWidth, 0,
                               display.width() - leftWidth, display.height());
        QPainter painter(&result);
        drawPanel(painter, leftPanel, display);
        drawPanel(painter, rightPanel, still);
        return result;
    }

    const QImage fittedStill = letterboxed(still, display.size());
    if (p <= 0.0)
        return display;
    if (p >= 1.0)
        return fittedStill;
    QImage result = display.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&result);
    if (mode == Mode::WipeHorizontal) {
        const int boundary = qRound((1.0 - p) * result.width());
        const QRect stillRegion(boundary, 0,
                                result.width() - boundary, result.height());
        painter.drawImage(stillRegion, fittedStill, stillRegion);
    } else {
        const int boundary = qRound((1.0 - p) * result.height());
        const QRect stillRegion(0, boundary,
                                result.width(), result.height() - boundary);
        painter.drawImage(stillRegion, fittedStill, stillRegion);
    }
    return result;
}

} // namespace stillcompare
