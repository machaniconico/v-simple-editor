// hdr_export16_selftest.cpp
// Headless selftest for HDR Stage2 export-side RGBA64 matte-free compositing.
// Run via: --selftest=hdr-export16

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QRgba64>
#include <QSize>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include "../playback/TlrCompose16.h"
#include "../playback/hdrexport16_flag.h"

namespace {

struct EnvGuard {
    EnvGuard()
        : had(qEnvironmentVariableIsSet("VEDITOR_HDR_EXPORT16")),
          value(qgetenv("VEDITOR_HDR_EXPORT16"))
    {
    }

    ~EnvGuard()
    {
        if (had)
            qputenv("VEDITOR_HDR_EXPORT16", value);
        else
            qunsetenv("VEDITOR_HDR_EXPORT16");
    }

    bool had = false;
    QByteArray value;
};

QImage makePattern8(QSize size)
{
    QImage img(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            img.setPixelColor(x, y, QColor(20 + x * 17,
                                           40 + y * 19,
                                           90 + (x + y) * 11,
                                           255));
        }
    }
    return img;
}

QImage makeSolid8(QSize size, const QColor& color)
{
    QImage img(size, QImage::Format_RGBA8888);
    img.fill(color);
    return img;
}

QImage compose8Reference(const QVector<QImage>& placedBackToFront,
                         const QVector<double>& opacities,
                         QSize canvas)
{
    QImage out(canvas, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);

    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (int i = 0; i < placedBackToFront.size(); ++i) {
        const QImage& img = placedBackToFront[i];
        const double rawOpacity = i < opacities.size() ? opacities[i] : 1.0;
        const double opacity = qBound(0.0, rawOpacity, 1.0);
        if (img.isNull() || opacity <= 0.001)
            continue;
        painter.setOpacity(opacity);
        painter.drawImage(0, 0, img);
    }
    painter.end();

    return out.convertToFormat(QImage::Format_RGBA8888);
}

bool pixelsEqual(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return false;
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < aa.height(); ++y) {
        for (int x = 0; x < aa.width(); ++x) {
            if (aa.pixelColor(x, y) != bb.pixelColor(x, y))
                return false;
        }
    }
    return true;
}

struct DiffStats {
    double mae = 1e9;
    int maxAbs = 255;
};

DiffStats comparePixels(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return DiffStats{};
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    long long sum = 0;
    int maxAbs = 0;
    int count = 0;
    for (int y = 0; y < aa.height(); ++y) {
        for (int x = 0; x < aa.width(); ++x) {
            const QColor ca = aa.pixelColor(x, y);
            const QColor cb = bb.pixelColor(x, y);
            const int diffs[4] = {
                std::abs(ca.red() - cb.red()),
                std::abs(ca.green() - cb.green()),
                std::abs(ca.blue() - cb.blue()),
                std::abs(ca.alpha() - cb.alpha())
            };
            for (int d : diffs) {
                sum += d;
                if (d > maxAbs)
                    maxAbs = d;
                ++count;
            }
        }
    }
    return DiffStats{count > 0 ? double(sum) / double(count) : 1e9, maxAbs};
}

bool internalRgba64KeepsMoreThan8Bit()
{
    const int width = 600;
    QImage grad(width, 1, QImage::Format_RGBA64_Premultiplied);
    QRgba64* gradLine = reinterpret_cast<QRgba64*>(grad.scanLine(0));
    for (int x = 0; x < width; ++x)
        gradLine[x] = qRgba64(static_cast<quint16>(20000 + x), 0, 0, 65535);

    QImage canvas64(width, 1, QImage::Format_RGBA64_Premultiplied);
    canvas64.fill(QColor(0, 0, 0, 0));
    QPainter painter(&canvas64);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(0, 0, grad);
    painter.end();

    std::set<quint16> distinct;
    const QRgba64* outLine = reinterpret_cast<const QRgba64*>(canvas64.constScanLine(0));
    for (int x = 0; x < width; ++x)
        distinct.insert(outLine[x].red());
    return distinct.size() > 256;
}

} // namespace

int runHdrExport16Selftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[hdr-export16] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    EnvGuard envGuard;

    qunsetenv("VEDITOR_HDR_EXPORT16");
    check(1, "flag OFF when env unset",
          !hdrexport16::enabledFromEnv()
          && !hdrexport16::enabledFromEnvValue(QString()));

    qputenv("VEDITOR_HDR_EXPORT16", QByteArray("1"));
    check(2, "flag ON when env == 1",
          hdrexport16::enabledFromEnv()
          && hdrexport16::enabledFromEnvValue(QStringLiteral("1")));

    qputenv("VEDITOR_HDR_EXPORT16", QByteArray("0"));
    check(3, "flag OFF when env == 0 and rejects non-1 values",
          !hdrexport16::enabledFromEnv()
          && !hdrexport16::enabledFromEnvValue(QStringLiteral("0"))
          && !hdrexport16::enabledFromEnvValue(QStringLiteral("2")));

    const QSize canvas(7, 5);
    const QImage base = makePattern8(canvas);

    {
        QVector<QImage> placed;
        placed.append(base);
        const QImage got = tlrcompose16::composeRgba64ToRgba8888(placed, canvas);
        check(4, "single-layer compose == original (strict match)",
              pixelsEqual(got, base));
    }

    {
        const QImage overlay = makeSolid8(canvas, QColor(220, 40, 120, 255));
        QVector<QImage> placed;
        placed.append(base);
        placed.append(overlay);
        QVector<double> opacities;
        opacities.append(1.0);
        opacities.append(0.5);

        const QImage got = tlrcompose16::composeRgba64ToRgba8888(placed, opacities, canvas);
        const QImage want = compose8Reference(placed, opacities, canvas);
        const DiffStats diff = comparePixels(got, want);
        check(5, "2-layer compose MAE <= 2 and max abs diff <= 3",
              diff.mae <= 2.0 && diff.maxAbs <= 3);
    }

    check(6, "extra-precision distinct RGBA64 values > 256",
          internalRgba64KeepsMoreThan8Bit());

    {
        const QImage skipped = makeSolid8(canvas, QColor(255, 0, 0, 255));
        QVector<QImage> placed;
        placed.append(base);
        placed.append(QImage());
        placed.append(skipped);
        placed.append(skipped);
        QVector<double> opacities;
        opacities.append(1.0);
        opacities.append(1.0);
        opacities.append(0.0);
        opacities.append(0.001);

        const QImage got = tlrcompose16::composeRgba64ToRgba8888(placed, opacities, canvas);
        check(7, "null and zero-opacity layers are skipped",
              pixelsEqual(got, base));
    }

    {
        QVector<QImage> placed;
        placed.append(base);
        const QImage got = tlrcompose16::composeRgba64ToRgba8888(placed, canvas);
        check(8, "output format == QImage::Format_RGBA8888",
              got.format() == QImage::Format_RGBA8888);
    }

    std::printf("[hdr-export16] summary: gates=8 passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
