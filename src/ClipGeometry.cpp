#include "ClipGeometry.h"

#include <QPainter>
#include <QtMath>

namespace clipgeom {

QTransform resolveTransform(const ClipTransform& t, QSize canvasSize)
{
    // 契約と op-order は ClipGeometry.h の CANONICAL CLIP-PLACEMENT CONTRACT を参照。
    // 以下の数式はそれと厳密一致が必須。
    const double canvasW = canvasSize.width();
    const double canvasH = canvasSize.height();

    QTransform xf;
    xf.translate(canvasW * 0.5 + t.videoDx * canvasW,
                 canvasH * 0.5 + t.videoDy * canvasH); // (4) placement point
    xf.rotate(t.rotationDeg);                           // (3) pivot = placement
    xf.scale(t.videoScale, t.videoScale);               // (2) scale about placement
    xf.translate(-canvasW * 0.5, -canvasH * 0.5);       // (1) un-center source
    return xf;
}

QString nullObjectFilePath()
{
    return QStringLiteral("__VEDITOR_NULL_OBJECT__");
}

bool isNullObjectFilePath(const QString& filePath)
{
    return filePath.isEmpty() || filePath == nullObjectFilePath();
}

ClipTransform composeParented(const ClipTransform& child,
                              const ClipTransform& parent,
                              QSize canvasSize)
{
    const double canvasW = canvasSize.width();
    const double canvasH = canvasSize.height();
    if (canvasW <= 0.0 || canvasH <= 0.0)
        return child;

    const QPointF canvasCenter(canvasW * 0.5, canvasH * 0.5);
    const QPointF parentWorldPos(canvasCenter.x() + parent.videoDx * canvasW,
                                 canvasCenter.y() + parent.videoDy * canvasH);
    const QPointF childLocal(child.videoDx * canvasW,
                             child.videoDy * canvasH);
    const QPointF scaled(childLocal.x() * parent.videoScale,
                         childLocal.y() * parent.videoScale);
    const double radians = qDegreesToRadians(parent.rotationDeg);
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const QPointF rotated(scaled.x() * c - scaled.y() * s,
                          scaled.x() * s + scaled.y() * c);

    ClipTransform result;
    result.videoDx =
        (parentWorldPos.x() + rotated.x() - canvasW * 0.5) / canvasW;
    result.videoDy =
        (parentWorldPos.y() + rotated.y() - canvasH * 0.5) / canvasH;
    result.videoScale = child.videoScale * parent.videoScale;
    result.rotationDeg = child.rotationDeg + parent.rotationDeg;
    return result;
}

QImage renderLayer(const QImage& src, const ClipTransform& t,
                   QSize canvasSize, bool smooth)
{
    // Self-scale: resolveTransform un-centers by canvasSize, so src MUST be
    // canvas-sized for the math to hold. Scale here defensively so any
    // native-resolution input (e.g. 1920x1080 into a 640x360 canvas) is
    // correct without requiring callers to pre-scale.
    const QImage scaled = (src.size() != canvasSize)
        ? src.scaled(canvasSize, Qt::IgnoreAspectRatio,
                     smooth ? Qt::SmoothTransformation : Qt::FastTransformation)
        : src;

    QImage canvas(canvasSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    p.setRenderHint(QPainter::Antialiasing, smooth);
    p.setTransform(resolveTransform(t, canvasSize));
    p.drawImage(0, 0, scaled);
    p.end();

    return canvas;
}

} // namespace clipgeom
