#pragma once

#include <QImage>
#include <QRect>
#include <QSize>

namespace snsfit {

struct ContainGeom {
    QSize intermediateCanvas;
    QRect contentRect;
};

ContainGeom containGeom(const QSize& srcSize, double canvasAspect);
QImage containInAspectCanvas(const QImage& src, double canvasAspect, bool smooth = true);

// TODO S2b: implement once fitContain is wired into ClipInfo/project output handling.
bool shouldContain(bool fitContain, QSize projOutSize, QSize srcSize);
QImage maybeContain(const QImage& src, bool fitContain, QSize projOutSize);

} // namespace snsfit
