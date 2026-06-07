// SNS fit headless selftest.
//
// QApplication 不要。snsfit:: の純粋 geometry/image contain helper を検証する。

#include <QDebug>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>

#include <cmath>
#include <cstring>

#include "../playback/SnsFit.h"

#if __has_include("../playback/clipgeom.h")
#include "../playback/clipgeom.h"
#define SNS_FIT_HAVE_CLIPGEOM 1
#elif __has_include("../ClipGeometry.h")
#include "../ClipGeometry.h"
#define SNS_FIT_HAVE_CLIPGEOM 1
#else
#define SNS_FIT_HAVE_CLIPGEOM 0
#endif

namespace {

QImage makePatternImage(const QSize& size, QImage::Format format)
{
    QImage image(size, format);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixel(x, y, qRgba((x * 31 + y * 17) & 0xff,
                                       (x * 11 + y * 29) & 0xff,
                                       (x * 7 + y * 13) & 0xff,
                                       0x40 + ((x * 5 + y * 3) & 0x7f)));
        }
    }
    return image;
}

bool imageBytesEqual(const QImage& a, const QImage& b)
{
    return a.size() == b.size()
        && a.format() == b.format()
        && a.bytesPerLine() == b.bytesPerLine()
        && a.sizeInBytes() == b.sizeInBytes()
        && std::memcmp(a.constBits(), b.constBits(),
                       static_cast<std::size_t>(a.sizeInBytes())) == 0;
}

QRect alphaBounds(const QImage& image)
{
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) == 0)
                continue;
            if (x < left)
                left = x;
            if (x > right)
                right = x;
            if (y < top)
                top = y;
            if (y > bottom)
                bottom = y;
        }
    }
    if (right < left || bottom < top)
        return QRect();
    return QRect(left, top, right - left + 1, bottom - top + 1);
}

} // namespace

int runSnsFitSelftest()
{
    qInfo().noquote() << "[sns-fit] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[sns-fit] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[sns-fit] FAIL" << name << ":" << msg;
    };
    auto check = [&](const char* name, bool ok, const QString& msg = QString()) {
        if (ok)
            pass(name);
        else
            fail(name, msg);
    };

    {
        const QSize srcSize(1920, 1080);
        const double aspect = 9.0 / 16.0;
        const snsfit::ContainGeom g = snsfit::containGeom(srcSize, aspect);
        const int expectedHeight = qRound(1920.0 / aspect);
        check("G1 16:9 into 9:16 geometry is vertically centered",
              g.intermediateCanvas.width() == 1920
                  && g.intermediateCanvas.height() == expectedHeight
                  && g.contentRect.y() == (g.intermediateCanvas.height() - 1080) / 2
                  && g.contentRect.width() == 1920
                  && g.contentRect.height() == 1080,
              QStringLiteral("canvas=%1x%2 rect=(%3,%4 %5x%6) expectedH=%7")
                  .arg(g.intermediateCanvas.width()).arg(g.intermediateCanvas.height())
                  .arg(g.contentRect.x()).arg(g.contentRect.y())
                  .arg(g.contentRect.width()).arg(g.contentRect.height())
                  .arg(expectedHeight));
    }

    {
        const QSize srcSize(1920, 1080);
        const snsfit::ContainGeom g = snsfit::containGeom(srcSize, 9.0 / 16.0);
        check("G2 no distortion: contentRect remains source-sized",
              g.contentRect.size() == srcSize,
              QStringLiteral("content=%1x%2 src=%3x%4")
                  .arg(g.contentRect.width()).arg(g.contentRect.height())
                  .arg(srcSize.width()).arg(srcSize.height()));
    }

    {
        QImage src(192, 108, QImage::Format_RGBA8888);
        src.fill(qRgba(255, 64, 32, 255));
        const QImage out = snsfit::containInAspectCanvas(src, 9.0 / 16.0);
        const int alpha = qAlpha(out.pixel(out.width() / 2, 0));
        check("G3 letterbox bars are transparent",
              alpha == 0,
              QStringLiteral("top bar alpha=%1").arg(alpha));
    }

    {
        const QImage src16x9 = makePatternImage(QSize(16, 9), QImage::Format_RGBA8888);
        const QImage out16x9 = snsfit::containInAspectCanvas(src16x9, 16.0 / 9.0);
        const QImage src4x4 = makePatternImage(QSize(4, 4), QImage::Format_RGBA8888);
        const QImage out4x4 = snsfit::containInAspectCanvas(src4x4, 1.0);
        check("G4 aspect-match identity is byte-identical",
              imageBytesEqual(src16x9, out16x9) && imageBytesEqual(src4x4, out4x4),
              QStringLiteral("16x9Equal=%1 4x4Equal=%2")
                  .arg(imageBytesEqual(src16x9, out16x9))
                  .arg(imageBytesEqual(src4x4, out4x4)));
    }

    {
        const QSize srcSize(1080, 1920);
        const snsfit::ContainGeom g = snsfit::containGeom(srcSize, 16.0 / 9.0);
        check("G5 portrait source into landscape canvas pillarboxes",
              g.contentRect.x() > 0
                  && g.contentRect.y() == 0
                  && g.contentRect.width() == 1080
                  && g.contentRect.height() == 1920,
              QStringLiteral("canvas=%1x%2 rect=(%3,%4 %5x%6)")
                  .arg(g.intermediateCanvas.width()).arg(g.intermediateCanvas.height())
                  .arg(g.contentRect.x()).arg(g.contentRect.y())
                  .arg(g.contentRect.width()).arg(g.contentRect.height()));
    }

    {
        QImage rgba8(4, 4, QImage::Format_RGBA8888);
        rgba8.fill(qRgba(1, 2, 3, 255));
        QImage rgba64(4, 4, QImage::Format_RGBA64);
        rgba64.fill(Qt::white);
        const QImage out8 = snsfit::containInAspectCanvas(rgba8, 2.0);
        const QImage out64 = snsfit::containInAspectCanvas(rgba64, 2.0);
        check("G6 output format is preserved",
              out8.format() == rgba8.format() && out64.format() == rgba64.format(),
              QStringLiteral("out8=%1 in8=%2 out64=%3 in64=%4")
                  .arg(static_cast<int>(out8.format())).arg(static_cast<int>(rgba8.format()))
                  .arg(static_cast<int>(out64.format())).arg(static_cast<int>(rgba64.format())));
    }

    {
        const QSize srcSize(1920, 1080);
        const snsfit::ContainGeom g = snsfit::containGeom(srcSize, 1.0);
        check("G7 square canvas letterboxes landscape source",
              g.contentRect.y() > 0
                  && g.intermediateCanvas.width() == 1920,
              QStringLiteral("canvas=%1x%2 rectY=%3")
                  .arg(g.intermediateCanvas.width()).arg(g.intermediateCanvas.height())
                  .arg(g.contentRect.y()));
    }

#if SNS_FIT_HAVE_CLIPGEOM
    {
        QImage src(144, 81, QImage::Format_RGBA8888);
        src.fill(qRgba(200, 30, 20, 255));
        const QImage contained = snsfit::containInAspectCanvas(src, 9.0 / 16.0, false);
        const clipgeom::ClipTransform identity;
        const QImage placed = clipgeom::renderLayer(contained, identity, QSize(288, 512), false);
        const QRect bounds = alphaBounds(placed);
        const double placedAspect = bounds.width() / static_cast<double>(bounds.height());
        const double srcAspect = src.width() / static_cast<double>(src.height());
        check("G8 contain->clipgeom preserves content aspect",
              bounds.width() == 288
                  && bounds.height() == 162
                  && std::fabs(placedAspect - srcAspect) < 1e-6,
              QStringLiteral("bounds=%1x%2 placedAspect=%3 srcAspect=%4")
                  .arg(bounds.width()).arg(bounds.height())
                  .arg(placedAspect, 0, 'f', 9)
                  .arg(srcAspect, 0, 'f', 9));
    }
#else
    qInfo().noquote() << "[sns-fit] SKIP G8 contain->clipgeom preserves content aspect:"
                      << "clipgeom header unavailable";
    pass("G8 contain->clipgeom preserves content aspect (skipped)");
#endif

    {
        const QSize src16x9(1920, 1080);
        check("G9 shouldContain false unless every gate is satisfied",
              !snsfit::shouldContain(true, QSize(), src16x9)
                  && !snsfit::shouldContain(true, QSize(0, 1920), src16x9)
                  && !snsfit::shouldContain(false, QSize(1080, 1920), src16x9)
                  && !snsfit::shouldContain(true, QSize(1920, 1080), src16x9),
              QStringLiteral("invalid=%1 empty=%2 fitFalse=%3 aspectMatch=%4")
                  .arg(snsfit::shouldContain(true, QSize(), src16x9))
                  .arg(snsfit::shouldContain(true, QSize(0, 1920), src16x9))
                  .arg(snsfit::shouldContain(false, QSize(1080, 1920), src16x9))
                  .arg(snsfit::shouldContain(true, QSize(1920, 1080), src16x9)));
    }

    {
        check("G10 shouldContain true when all gates are satisfied",
              snsfit::shouldContain(true, QSize(1080, 1920), QSize(1920, 1080)),
              QStringLiteral("expected true for fitContain=true, valid 9:16 canvas, 16:9 src"));
    }

    {
        const QImage src = makePatternImage(QSize(16, 9), QImage::Format_RGBA8888);
        const QImage out = snsfit::maybeContain(src, false, QSize(1080, 1920));
        check("G11 maybeContain identity when shouldContain is false",
              imageBytesEqual(src, out),
              QStringLiteral("identityEqual=%1").arg(imageBytesEqual(src, out)));
    }

    qInfo().noquote() << "[sns-fit] selftest done: passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
