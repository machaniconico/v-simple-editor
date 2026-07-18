// OnionSkin.cpp — 前後フレーム半透明オーバーレイの純粋エンジン。
// display-local 適用専用。export / render / timeline 経路には一切関与しない。
#include "OnionSkin.h"

#include <QColor>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QtGlobal>

namespace onionskin {
namespace {

QImage ghostLayer(const QImage &ghost, const QSize &size, const QColor &tint,
                  bool tintColors)
{
    QImage layer(size, QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);

    QPainter p(&layer);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(QRect(QPoint(0, 0), size), ghost, ghost.rect());
    if (tintColors) {
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(layer.rect(), tint);
    }
    p.end();
    return layer;
}

void drawGhosts(QPainter &painter, const QVector<QImage> &ghosts,
                const QSize &size, const QColor &tint, double opacity,
                bool tintColors)
{
    const int count = ghosts.size();
    if (count <= 0)
        return;

    for (int i = 0; i < count; ++i) {
        if (ghosts[i].isNull())
            continue;
        const double ageScale = static_cast<double>(count - i) / count;
        painter.setOpacity(qBound(0.0, opacity * ageScale, 1.0));
        painter.drawImage(0, 0, ghostLayer(ghosts[i], size, tint, tintColors));
    }
    painter.setOpacity(1.0);
}

} // namespace

QImage compose(const QImage &current,
               const QVector<QImage> &before,
               const QVector<QImage> &after,
               const Config &cfg)
{
    if (!cfg.enabled || current.isNull()
        || (before.isEmpty() && after.isEmpty())
        || cfg.opacity <= 0.0) {
        return current;
    }

    QImage result = current.copy();
    QPainter painter(&result);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    drawGhosts(painter, before, current.size(), QColor(255, 64, 64),
               cfg.opacity, cfg.tintColors);
    drawGhosts(painter, after, current.size(), QColor(64, 128, 255),
               cfg.opacity, cfg.tintColors);

    painter.end();
    return result;
}

} // namespace onionskin
