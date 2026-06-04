// src/selftests/gpu_composite_parity16_matte_selftest.cpp
// Stage8 S8-1: GPU RGBA16 track-matte compositor parity.

#include "../playback/GpuLayerCompositor.h"
#include "../playback/GpuCompositeMath.h"
#include "../playback/HdrCompositeMath.h"
#include "../playback/TrackMatteCompose16.h"

#include <QApplication>
#include <QGuiApplication>
#include <QColor>
#include <QImage>
#include <QRgba64>
#include <QSize>
#include <QtGlobal>
#include <QVector>

#include <cmath>
#include <cstdio>
#include <set>

namespace {

using gpucomposite::LayerDesc;

constexpr quint32 kMax16 = hdrcomposite::kMax16;

quint16 lift8(int v)
{
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return static_cast<quint16>(v * 257);
}

quint16 premultiply16(quint16 straight, quint16 alpha)
{
    const quint64 premul =
        (static_cast<quint64>(straight) * alpha + kMax16 / 2) / kMax16;
    return static_cast<quint16>(premul > kMax16 ? kMax16 : premul);
}

template <typename Fn>
QImage makePremul16(QSize size, Fn fn)
{
    QImage img(size, QImage::Format_RGBA64_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        QRgba64* line = reinterpret_cast<QRgba64*>(img.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            quint16 r = 0;
            quint16 g = 0;
            quint16 b = 0;
            quint16 a = 0;
            fn(x, y, r, g, b, a);
            line[x] = qRgba64(premultiply16(r, a),
                              premultiply16(g, a),
                              premultiply16(b, a),
                              a);
        }
    }
    return img;
}

QImage base16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = lift8((35 + x * 5 + y * 3) & 0xff);
        g = lift8((75 + x * 2 + y * 7) & 0xff);
        b = lift8((130 + x * 3 + y * 5) & 0xff);
        a = static_cast<quint16>(kMax16);
    });
}

QImage foreground16(QSize size, quint16 alpha = static_cast<quint16>(kMax16))
{
    return makePremul16(size, [alpha](int x, int y,
                                      quint16& r, quint16& g, quint16& b, quint16& a) {
        r = lift8((210 + x * 7 + y * 2) & 0xff);
        g = lift8((45 + x * 3 + y * 11) & 0xff);
        b = lift8((90 + x * 13 + y * 5) & 0xff);
        a = alpha;
    });
}

QImage alphaMatte16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = lift8((25 + x * 11) & 0xff);
        g = lift8((120 + y * 17) & 0xff);
        b = lift8((70 + (x + y) * 9) & 0xff);
        a = lift8((16 + x * 19 + y * 23) & 0xff);
    });
}

QImage lumaMatte16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = lift8((15 + x * 29 + y * 3) & 0xff);
        g = lift8((55 + x * 5 + y * 31) & 0xff);
        b = lift8((105 + x * 17 + y * 7) & 0xff);
        a = static_cast<quint16>(kMax16);
    });
}

QImage solidPremul16(QSize size, quint16 r, quint16 g, quint16 b, quint16 a)
{
    return makePremul16(size, [=](int, int,
                                  quint16& rr, quint16& gg, quint16& bb, quint16& aa) {
        rr = r;
        gg = g;
        bb = b;
        aa = a;
    });
}

GpuLayerInput makeLayer(const QImage& img, int sourceTrack, double opacity,
                        double scale, double dx, double dy, double rotDeg)
{
    GpuLayerInput in;
    in.image = img;
    in.desc.sourceTrack = sourceTrack;
    in.desc.srcSize = img.size();
    in.desc.opacity = opacity;
    in.desc.videoScale = scale;
    in.desc.videoDx = dx;
    in.desc.videoDy = dy;
    in.desc.rotation2DDegrees = rotDeg;
    in.desc.visible = true;
    in.desc.matteSourceIndex = -1;
    in.desc.matteType = gpucomposite::MatteType::None;
    return in;
}

QVector<GpuLayerInput> matteStack(QSize canvas,
                                  gpucomposite::MatteType matteType,
                                  const QImage& matte,
                                  const QImage& foreground,
                                  double foregroundOpacity = 1.0)
{
    QVector<GpuLayerInput> layers;
    layers.push_back(makeLayer(base16(canvas),
                               /*track*/2, 1.0, 1.0, 0.0, 0.0, 0.0));

    // Matte source: consumed by layer index 2 and read at opacity=1 through its
    // own transform. Its low opacity proves source opacity is ignored.
    layers.push_back(makeLayer(matte,
                               /*track*/1, 0.20, 0.82, -0.05, 0.04, 0.0));

    GpuLayerInput masked = makeLayer(foreground,
                                     /*track*/0, foregroundOpacity,
                                     0.74, 0.08, -0.06, 0.0);
    masked.desc.matteType = matteType;
    masked.desc.matteSourceIndex = 1;
    layers.push_back(masked);
    return layers;
}

QVector<LayerDesc> descsFor(const QVector<GpuLayerInput>& layers)
{
    QVector<LayerDesc> descs;
    descs.reserve(layers.size());
    for (const GpuLayerInput& in : layers)
        descs.push_back(in.desc);
    return descs;
}

QVector<QImage> imagesFor(const QVector<GpuLayerInput>& layers)
{
    QVector<QImage> images;
    images.reserve(layers.size());
    for (const GpuLayerInput& in : layers)
        images.push_back(in.image);
    return images;
}

struct Metrics16 {
    bool valid = false;
    double ssim = 0.0;
    double rgb16Mae = 1e18;
    double a16Mae = 1e18;
};

Metrics16 compare16(const QImage& aIn, const QImage& bIn)
{
    Metrics16 out;
    if (aIn.isNull() || bIn.isNull() || aIn.size() != bIn.size())
        return out;

    const int w = aIn.width();
    const int h = aIn.height();
    const double n = double(w) * double(h);
    if (n <= 0.0)
        return out;

    const QImage a16 = aIn.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    const QImage b16 = bIn.convertToFormat(QImage::Format_RGBA64_Premultiplied);

    double rgbSum = 0.0;
    double aSum = 0.0;
    for (int y = 0; y < h; ++y) {
        const QRgba64* ra = reinterpret_cast<const QRgba64*>(a16.constScanLine(y));
        const QRgba64* rb = reinterpret_cast<const QRgba64*>(b16.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgba64 pa = ra[x];
            const QRgba64 pb = rb[x];
            rgbSum += std::abs(int(pa.red()) - int(pb.red()));
            rgbSum += std::abs(int(pa.green()) - int(pb.green()));
            rgbSum += std::abs(int(pa.blue()) - int(pb.blue()));
            aSum += std::abs(int(pa.alpha()) - int(pb.alpha()));
        }
    }
    out.rgb16Mae = rgbSum / (n * 3.0);
    out.a16Mae = aSum / n;

    const QImage a = aIn.convertToFormat(QImage::Format_ARGB32);
    const QImage b = bIn.convertToFormat(QImage::Format_ARGB32);
    double meanA = 0.0;
    double meanB = 0.0;
    for (int y = 0; y < h; ++y) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb pa = ra[x];
            const QRgb pb = rb[x];
            meanA += 0.299 * qRed(pa) + 0.587 * qGreen(pa) + 0.114 * qBlue(pa);
            meanB += 0.299 * qRed(pb) + 0.587 * qGreen(pb) + 0.114 * qBlue(pb);
        }
    }
    meanA /= n;
    meanB /= n;

    double varA = 0.0;
    double varB = 0.0;
    double cov = 0.0;
    for (int y = 0; y < h; ++y) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb pa = ra[x];
            const QRgb pb = rb[x];
            const double la = 0.299 * qRed(pa) + 0.587 * qGreen(pa) + 0.114 * qBlue(pa);
            const double lb = 0.299 * qRed(pb) + 0.587 * qGreen(pb) + 0.114 * qBlue(pb);
            const double da = la - meanA;
            const double db = lb - meanB;
            varA += da * da;
            varB += db * db;
            cov += da * db;
        }
    }
    varA /= n;
    varB /= n;
    cov /= n;

    const double C1 = (0.01 * 255.0) * (0.01 * 255.0);
    const double C2 = (0.03 * 255.0) * (0.03 * 255.0);
    out.ssim = ((2.0 * meanA * meanB + C1) * (2.0 * cov + C2)) /
               ((meanA * meanA + meanB * meanB + C1) * (varA + varB + C2));
    out.valid = true;
    return out;
}

} // namespace

int runGpuCompositeParity16MatteSelftest()
{
    if (!qApp) {
        std::printf("[gpu-composite-parity-16-matte] SKIP: no QApplication instance (GL unavailable)\n");
        return 0;
    }

    GpuLayerCompositor gpu;
    if (!gpu.isAvailable()) {
        std::printf("[gpu-composite-parity-16-matte] SKIP: no usable GL context (headless/WSL)\n");
        return 0;
    }

    const QSize probeCanvas(64, 48);
    {
        const QVector<GpuLayerInput> probe = matteStack(
            probeCanvas,
            gpucomposite::MatteType::Alpha,
            alphaMatte16(probeCanvas),
            foreground16(probeCanvas));
        const QImage probeImg = gpu.composite16Matte(probe, probeCanvas);
        if (probeImg.isNull() || probeImg.size() != probeCanvas
            || probeImg.format() != QImage::Format_RGBA64_Premultiplied) {
            std::printf("[gpu-composite-parity-16-matte] SKIP: RGBA16 FBO unavailable\n");
            return 0;
        }
    }

    int passed = 0;
    int total = 0;

    auto gate = [&](int g, const char* desc,
                    const QVector<GpuLayerInput>& layers, QSize canvas,
                    double ssimMin, double rgbMaeMax, double aMaeMax) {
        ++total;
        const QImage cpu = hdrcomposite::compositeReference16Matte(
            descsFor(layers), imagesFor(layers), canvas);
        const QImage gpuImg = gpu.composite16Matte(layers, canvas);
        const Metrics16 m = compare16(cpu, gpuImg);
        const bool ok = m.valid
                        && gpuImg.format() == QImage::Format_RGBA64_Premultiplied
                        && m.ssim >= ssimMin
                        && m.rgb16Mae <= rgbMaeMax
                        && m.a16Mae <= aMaeMax;
        std::printf("[gpu-composite-parity-16-matte] %s G%d %s "
                    "(SSIM=%.5f >= %.3f, rgb16Mae=%.3f <= %.1f, a16Mae=%.3f <= %.1f)\n",
                    ok ? "PASS" : "FAIL", g, desc,
                    m.ssim, ssimMin, m.rgb16Mae, rgbMaeMax,
                    m.a16Mae, aMaeMax);
        if (ok) ++passed;
    };

    const QSize canvas(192, 128);
    const double ssimMin = 0.98;
    // 8-bit matte gates allow about 4/255 MAE. 1600 is ~6.2/255 in 16-bit,
    // leaving extra room for float luma and nearest-edge differences.
    const double matteMae16Max = 1600.0;

    gate(1, "Alpha matte, transformed matte source and matte'd layer",
         matteStack(canvas, gpucomposite::MatteType::Alpha,
                    alphaMatte16(canvas), foreground16(canvas)),
         canvas, ssimMin, matteMae16Max, matteMae16Max);

    gate(2, "AlphaInverted matte, transformed matte source and matte'd layer",
         matteStack(canvas, gpucomposite::MatteType::AlphaInverted,
                    alphaMatte16(canvas), foreground16(canvas)),
         canvas, ssimMin, matteMae16Max, matteMae16Max);

    gate(3, "Luma matte, transformed matte source and matte'd layer",
         matteStack(canvas, gpucomposite::MatteType::Luminance,
                    lumaMatte16(canvas), foreground16(canvas)),
         canvas, ssimMin, matteMae16Max, matteMae16Max);

    gate(4, "LumaInverted matte, transformed matte source and matte'd layer",
         matteStack(canvas, gpucomposite::MatteType::LuminanceInverted,
                    lumaMatte16(canvas), foreground16(canvas)),
         canvas, ssimMin, matteMae16Max, matteMae16Max);

    gate(5, "translucent matte'd layer preserves premultiplied rgb/a",
         matteStack(canvas, gpucomposite::MatteType::Alpha,
                    alphaMatte16(canvas), foreground16(canvas, lift8(128))),
         canvas, ssimMin, matteMae16Max, matteMae16Max);

    {
        ++total;
        const int width = 1200;
        const QSize precisionCanvas(width, 1);

        QImage matte(precisionCanvas, QImage::Format_RGBA64_Premultiplied);
        QRgba64* matteLine = reinterpret_cast<QRgba64*>(matte.scanLine(0));
        std::set<quint16> expectedLevels;
        for (int x = 0; x < width; ++x) {
            const quint16 r = static_cast<quint16>(20000 + x);
            matteLine[x] = qRgba64(r, 0, 0, static_cast<quint16>(kMax16));
            expectedLevels.insert(trackmatte16::matteMaskValue16(
                gpucomposite::MatteType::Luminance, r, 0, 0,
                static_cast<quint16>(kMax16)));
        }

        QVector<GpuLayerInput> layers;
        layers.push_back(makeLayer(QImage(), /*track*/2, 1.0, 1.0, 0.0, 0.0, 0.0));
        layers.push_back(makeLayer(matte, /*track*/1, 0.10, 1.0, 0.0, 0.0, 0.0));
        GpuLayerInput masked = makeLayer(
            solidPremul16(precisionCanvas,
                          static_cast<quint16>(kMax16),
                          static_cast<quint16>(kMax16),
                          static_cast<quint16>(kMax16),
                          static_cast<quint16>(kMax16)),
            /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0);
        masked.desc.matteType = gpucomposite::MatteType::Luminance;
        masked.desc.matteSourceIndex = 1;
        layers.push_back(masked);

        const QImage cpu = hdrcomposite::compositeReference16Matte(
            descsFor(layers), imagesFor(layers), precisionCanvas);
        const QImage gpuImg = gpu.composite16Matte(layers, precisionCanvas);
        const Metrics16 m = compare16(cpu, gpuImg);

        std::set<quint16> levels16;
        std::set<int> levels8;
        if (!gpuImg.isNull() && gpuImg.size() == precisionCanvas) {
            const QImage out16 = gpuImg.convertToFormat(QImage::Format_RGBA64_Premultiplied);
            const QRgba64* outLine = reinterpret_cast<const QRgba64*>(out16.constScanLine(0));
            for (int x = 0; x < width; ++x) {
                levels16.insert(outLine[x].red());
                levels8.insert(outLine[x].red() / 257);
            }
        }

        const bool ok = m.valid
                        && gpuImg.format() == QImage::Format_RGBA64_Premultiplied
                        && m.ssim >= ssimMin
                        && m.rgb16Mae <= matteMae16Max
                        && m.a16Mae <= matteMae16Max
                        && expectedLevels.size() > 256
                        && levels16.size() > 256
                        && levels8.size() <= 256;
        std::printf("[gpu-composite-parity-16-matte] %s G6 16-bit matte temp precision proof "
                    "(SSIM=%.5f >= %.3f, rgb16Mae=%.3f <= %.1f, a16Mae=%.3f <= %.1f, "
                    "expectedLevels=%zu > 256, distinct16=%zu > 256, distinct8proj=%zu <= 256)\n",
                    ok ? "PASS" : "FAIL",
                    m.ssim, ssimMin, m.rgb16Mae, matteMae16Max,
                    m.a16Mae, matteMae16Max,
                    expectedLevels.size(), levels16.size(), levels8.size());
        if (ok) ++passed;
    }

    std::printf("[gpu-composite-parity-16-matte] Result: %d/%d PASSED\n", passed, total);
    return (passed == total) ? 0 : 1;
}
