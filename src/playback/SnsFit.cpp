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

QImage maybeContain(const QImage& src, bool fitContain, QSize projOutSize)
{
    if (!shouldContain(fitContain, projOutSize, src.size()))
        return src;

    const double canvasAspect =
        projOutSize.width() / static_cast<double>(projOutSize.height());
    return containInAspectCanvas(src, canvasAspect);
}

} // namespace snsfit
