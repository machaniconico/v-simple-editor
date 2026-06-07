#pragma once

#include <QImage>
#include <QRect>
#include <QSize>

namespace snsfit {

struct ContainGeom {
    QSize intermediateCanvas;
    QRect contentRect;
};

struct CoverGeom {
    QSize croppedSize;
    QRect srcCropRect;
};

ContainGeom containGeom(const QSize& srcSize, double canvasAspect);
QImage containInAspectCanvas(const QImage& src, double canvasAspect, bool smooth = true);
CoverGeom coverGeom(const QSize& srcSize, double canvasAspect);
QImage coverInAspectCanvas(const QImage& src, double canvasAspect, bool smooth = true);
void containContentInset(QSize srcSize, QSize projOut, double& fw, double& fh);

// TODO S2b: implement once fitContain is wired into ClipInfo/project output handling.
bool shouldContain(bool fitContain, QSize projOutSize, QSize srcSize);
QImage maybeContain(const QImage& src, bool fitContain, QSize projOutSize);
bool shouldCover(bool fitCover, QSize projOutSize, QSize srcSize);
QImage maybeCover(const QImage& src, bool fitCover, QSize projOutSize);
bool shouldFit(bool fitContain, bool fitCover, QSize projOutSize, QSize srcSize);
QImage maybeFit(const QImage& src, bool fitContain, bool fitCover, QSize projOutSize);

} // namespace snsfit
