#include "SnsFit.h"

#include <QPainter>
#include <cmath>

namespace snsfit {
namespace {

constexpr double kAspectEps = 1e-6;
constexpr double kShouldContainAspectEps = 1e-4;

bool isValidSourceSize(const QSize& size)
{
    return size.width() > 0 && size.height() > 0;
}

} // namespace

ContainGeom containGeom(const QSize& srcSize, double canvasAspect)
{
    if (!isValidSourceSize(srcSize) || canvasAspect <= 0.0 || !std::isfinite(canvasAspect))
        return { srcSize, QRect(0, 0, srcSize.width(), srcSize.height()) };

    const double srcA = srcSize.width() / static_cast<double>(srcSize.height());
    if (std::fabs(srcA - canvasAspect) < kAspectEps) {
        return {
            srcSize,
            QRect(0, 0, srcSize.width(), srcSize.height())
        };
    }

    if (srcA > canvasAspect) {
        const int iw = srcSize.width();
        const int ih = qRound(iw / canvasAspect);
        return {
            QSize(iw, ih),
            QRect(0, (ih - srcSize.height()) / 2, srcSize.width(), srcSize.height())
        };
    }

    const int ih = srcSize.height();
    const int iw = qRound(ih * canvasAspect);
    return {
        QSize(iw, ih),
        QRect((iw - srcSize.width()) / 2, 0, srcSize.width(), srcSize.height())
    };
}

CoverGeom coverGeom(const QSize& srcSize, double canvasAspect)
{
    if (!isValidSourceSize(srcSize) || canvasAspect <= 0.0 || !std::isfinite(canvasAspect))
        return { srcSize, QRect(0, 0, srcSize.width(), srcSize.height()) };

    const double srcA = srcSize.width() / static_cast<double>(srcSize.height());
    if (std::fabs(srcA - canvasAspect) < kAspectEps) {
        return {
            srcSize,
            QRect(0, 0, srcSize.width(), srcSize.height())
        };
    }

    if (srcA > canvasAspect) {
        const int h = srcSize.height();
        const int cropW = qRound(h * canvasAspect);
        const int x = (srcSize.width() - cropW) / 2;
        return {
            QSize(cropW, h),
            QRect(x, 0, cropW, h)
        };
    }

    const int w = srcSize.width();
    const int cropH = qRound(w / canvasAspect);
    const int y = (srcSize.height() - cropH) / 2;
    return {
        QSize(w, cropH),
        QRect(0, y, w, cropH)
    };
}

QImage containInAspectCanvas(const QImage& src, double canvasAspect, bool smooth)
{
    (void)smooth;

    if (src.isNull() || canvasAspect <= 0.0 || !std::isfinite(canvasAspect) || src.height() <= 0)
        return src;

    const double srcA = src.width() / static_cast<double>(src.height());
    if (std::fabs(srcA - canvasAspect) < kAspectEps)
        return src;

    const ContainGeom g = containGeom(src.size(), canvasAspect);
    QImage out(g.intermediateCanvas, src.format());
    out.fill(Qt::transparent);

    QPainter painter(&out);
    painter.drawImage(g.contentRect, src, QRect(0, 0, src.width(), src.height()));
    painter.end();

    return out;
}

QImage coverInAspectCanvas(const QImage& src, double canvasAspect, bool smooth)
{
    (void)smooth;

    if (src.isNull() || canvasAspect <= 0.0 || !std::isfinite(canvasAspect) || src.height() <= 0)
        return src;

    const double srcA = src.width() / static_cast<double>(src.height());
    if (std::fabs(srcA - canvasAspect) < kAspectEps)
        return src;

    const CoverGeom g = coverGeom(src.size(), canvasAspect);
    QImage out(g.croppedSize, src.format());
    out.fill(Qt::transparent);

    QPainter painter(&out);
    painter.drawImage(QRect(0, 0, out.width(), out.height()), src, g.srcCropRect);
    painter.end();

    return out;
}

void containContentInset(QSize srcSize, QSize projOut, double& fw, double& fh)
{
    fw = 1.0;
    fh = 1.0;
    if (!projOut.isValid() || srcSize.width() <= 0 || srcSize.height() <= 0)
        return;
    const double srcA = double(srcSize.width()) / srcSize.height();
    const double canA = double(projOut.width()) / projOut.height();
    if (srcA > canA) {
        fh = canA / srcA;
    } else {
        fw = srcA / canA;
    }
}

bool shouldContain(bool fitContain, QSize projOutSize, QSize srcSize)
{
    if (!fitContain)
        return false;
    if (!projOutSize.isValid() || projOutSize.isEmpty())
        return false;
    if (!isValidSourceSize(srcSize))
        return false;

    const double canvasAspect =
        projOutSize.width() / static_cast<double>(projOutSize.height());
    const double srcAspect = srcSize.width() / static_cast<double>(srcSize.height());
    return std::fabs(srcAspect - canvasAspect) >= kShouldContainAspectEps;
}

bool shouldCover(bool fitCover, QSize projOutSize, QSize srcSize)
{
    if (!fitCover)
        return false;
    if (!projOutSize.isValid() || projOutSize.isEmpty())
        return false;
    if (!isValidSourceSize(srcSize))
        return false;

    const double canvasAspect =
        projOutSize.width() / static_cast<double>(projOutSize.height());
    const double srcAspect = srcSize.width() / static_cast<double>(srcSize.height());
    return std::fabs(srcAspect - canvasAspect) >= kShouldContainAspectEps;
}

QImage maybeContain(const QImage& src, bool fitContain, QSize projOutSize)
{
    if (!shouldContain(fitContain, projOutSize, src.size()))
        return src;

    const double canvasAspect =
        projOutSize.width() / static_cast<double>(projOutSize.height());
    return containInAspectCanvas(src, canvasAspect);
}

QImage maybeCover(const QImage& src, bool fitCover, QSize projOutSize)
{
    if (!shouldCover(fitCover, projOutSize, src.size()))
        return src;

    const double canvasAspect =
        projOutSize.width() / static_cast<double>(projOutSize.height());
    return coverInAspectCanvas(src, canvasAspect);
}

bool shouldFit(bool fitContain, bool fitCover, QSize projOutSize, QSize srcSize)
{
    if (fitCover)
        return shouldCover(true, projOutSize, srcSize);
    if (fitContain)
        return shouldContain(true, projOutSize, srcSize);
    return false;
}

QImage maybeFit(const QImage& src, bool fitContain, bool fitCover, QSize projOutSize)
{
    if (fitCover)
        return maybeCover(src, true, projOutSize);
    if (fitContain)
        return maybeContain(src, true, projOutSize);
    return src;
}

} // namespace snsfit
