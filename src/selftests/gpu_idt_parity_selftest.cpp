// src/selftests/gpu_idt_parity_selftest.cpp
// Story1 GPU IDT capability parity: composite16Idt() vs direct aces:: CPU oracle.

#include "../playback/GpuLayerCompositor.h"
#include "../playback/GpuCompositeMath.h"
#include "../playback/HdrCompositeMath.h"
#include "../AcesColor.h"
#include "../color/ClipColorTransform.h"

#include <QApplication>
#include <QGuiApplication>
#include <QImage>
#include <QRgba64>
#include <QSize>
#include <QtGlobal>
#include <QVector>

#include <cmath>
#include <cstdio>

namespace {

using gpucomposite::LayerDesc;

constexpr quint16 kMax16 = 65535;

clipcolor::ColorMeta meta(clipcolor::Primaries primaries,
                          clipcolor::Transfer transfer = clipcolor::Transfer::sRGB,
                          bool hdr = false)
{
    clipcolor::ColorMeta m;
    m.primaries = primaries;
    m.transfer = transfer;
    m.bitDepth = hdr ? 10 : 8;
    m.isHdr = hdr;
    return m;
}

quint16 clampTo16(double normalized)
{
    if (!std::isfinite(normalized) || normalized <= 0.0)
        return 0;
    if (normalized >= 1.0)
        return kMax16;
    return static_cast<quint16>(std::llround(normalized * kMax16));
}

quint16 premultiply16(quint16 straight, quint16 alpha)
{
    const quint64 premul =
        (static_cast<quint64>(straight) * alpha + kMax16 / 2) / kMax16;
    return static_cast<quint16>(premul > kMax16 ? kMax16 : premul);
}

quint16 unpremultiply16(quint16 premul, quint16 alpha)
{
    if (alpha == 0)
        return 0;
    const quint64 straight =
        (static_cast<quint64>(premul) * kMax16 + alpha / 2) / alpha;
    return static_cast<quint16>(straight > kMax16 ? kMax16 : straight);
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

QImage transparent16(QSize size)
{
    return makePremul16(size, [](int, int,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = g = b = 0;
        a = 0;
    });
}

QImage rec709Signal16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = static_cast<quint16>(9000 + ((x * 191 + y * 37) % 42000));
        g = static_cast<quint16>(14000 + ((x * 43 + y * 173) % 38000));
        b = static_cast<quint16>(7000 + ((x * 109 + y * 211) % 45000));
        a = kMax16;
    });
}

QImage rec2020Signal16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = static_cast<quint16>(10000 + ((x * 331 + y * 97) % 42000));
        g = static_cast<quint16>(12000 + ((x * 53 + y * 389) % 36000));
        b = static_cast<quint16>(8000 + ((x * 181 + y * 211) % 46000));
        a = kMax16;
    });
}

QImage rec2020LumaMatteSignal16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        // Highly saturated, bright Rec.2020 primaries in every band: the wider
        // Rec.2020 gamut shifts Rec.601 luma the most under Rec.2020->sRGB, so
        // the converted-vs-raw matte luma divergence (the negative control) is
        // large and not gamut-desaturated. Each band is a different primary mix
        // so the matte still has spatial structure.
        const int band = ((x / 8) + (y / 6)) % 4;
        if (band == 0) {
            r = 60000; g = 60000; b = 2000;   // saturated yellow
        } else if (band == 1) {
            r = 2000; g = 60000; b = 60000;   // saturated cyan
        } else if (band == 2) {
            r = 60000; g = 2000; b = 60000;   // saturated magenta
        } else {
            r = 60000; g = 24000; b = 2000;   // saturated orange
        }
        a = kMax16;
    });
}

QImage alphaMatteSignal16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = static_cast<quint16>(7000 + ((x * 131 + y * 59) % 43000));
        g = static_cast<quint16>(9000 + ((x * 67 + y * 211) % 41000));
        b = static_cast<quint16>(6000 + ((x * 191 + y * 103) % 45000));
        a = static_cast<quint16>(3000 + ((x * 419 + y * 283) % 60000));
    });
}

QImage acescgSignal16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = static_cast<quint16>(5000 + ((x * 157 + y * 271) % 33000));
        g = static_cast<quint16>(9000 + ((x * 283 + y * 61) % 35000));
        b = static_cast<quint16>(7000 + ((x * 89 + y * 199) % 31000));
        a = kMax16;
    });
}

GpuLayerInput makeLayer(const QImage& img,
                        const clipcolor::ColorMeta& colorMeta,
                        int sourceTrack,
                        double opacity = 1.0)
{
    GpuLayerInput in;
    in.image = img;
    in.colorMeta = colorMeta;
    in.desc.sourceTrack = sourceTrack;
    in.desc.srcSize = img.size();
    in.desc.opacity = opacity;
    in.desc.videoScale = 1.0;
    in.desc.videoDx = 0.0;
    in.desc.videoDy = 0.0;
    in.desc.rotation2DDegrees = 0.0;
    in.desc.visible = true;
    in.desc.matteSourceIndex = -1;
    in.desc.matteType = gpucomposite::MatteType::None;
    return in;
}

QVector<LayerDesc> descsFor(const QVector<GpuLayerInput>& layers)
{
    QVector<LayerDesc> descs;
    descs.reserve(layers.size());
    for (const GpuLayerInput& in : layers)
        descs.push_back(in.desc);
    return descs;
}

aces::ColorSpace outputSpaceFor(const QVector<GpuLayerInput>& layers)
{
    aces::ColorSpace outSpace = aces::ColorSpace::sRGB;
    bool haveOutSpace = false;
    int minSourceTrack = 0;
    for (const GpuLayerInput& in : layers) {
        if (!haveOutSpace || in.desc.sourceTrack < minSourceTrack) {
            haveOutSpace = true;
            minSourceTrack = in.desc.sourceTrack;
            outSpace = clipcolor::acesSpaceFor(in.colorMeta);
        }
    }
    return outSpace;
}

QImage convertLayerNoCache(const QImage& src,
                           const clipcolor::ColorMeta& in,
                           aces::ColorSpace outputSpace)
{
    if (src.isNull() || src.width() <= 0 || src.height() <= 0)
        return src;

    const aces::ColorSpace inputSpace = clipcolor::acesSpaceFor(in);
    const aces::Mat3 matrix = aces::conversionMatrix(inputSpace, outputSpace);

    QImage out = src.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    for (int y = 0; y < out.height(); ++y) {
        QRgba64* line = reinterpret_cast<QRgba64*>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const QRgba64 px = line[x];
            const quint16 a = px.alpha();
            if (a == 0) {
                line[x] = qRgba64(0, 0, 0, 0);
                continue;
            }

            const aces::Vec3 encodedIn = {
                double(unpremultiply16(px.red(), a)) / kMax16,
                double(unpremultiply16(px.green(), a)) / kMax16,
                double(unpremultiply16(px.blue(), a)) / kMax16
            };
            const aces::Vec3 linearIn = {
                aces::eotf(inputSpace, encodedIn[0]),
                aces::eotf(inputSpace, encodedIn[1]),
                aces::eotf(inputSpace, encodedIn[2])
            };
            const aces::Vec3 linearOut = aces::apply(matrix, linearIn);
            const quint16 outR = clampTo16(aces::oetf(outputSpace, linearOut[0]));
            const quint16 outG = clampTo16(aces::oetf(outputSpace, linearOut[1]));
            const quint16 outB = clampTo16(aces::oetf(outputSpace, linearOut[2]));
            line[x] = qRgba64(premultiply16(outR, a),
                              premultiply16(outG, a),
                              premultiply16(outB, a),
                              a);
        }
    }
    return out;
}

QImage cpuIdtOracle(const QVector<GpuLayerInput>& layers, QSize canvas)
{
    const aces::ColorSpace outSpace = outputSpaceFor(layers);

    QVector<QImage> converted;
    converted.reserve(layers.size());
    for (const GpuLayerInput& in : layers) {
        const aces::ColorSpace inSpace = clipcolor::acesSpaceFor(in.colorMeta);
        const bool passthrough =
            in.colorMeta.transfer == clipcolor::Transfer::PQ
            || in.colorMeta.transfer == clipcolor::Transfer::HLG
            || inSpace == outSpace;
        converted.push_back(passthrough
                            ? in.image
                            : convertLayerNoCache(in.image, in.colorMeta, outSpace));
    }

    return hdrcomposite::compositeReference16(descsFor(layers), converted, canvas);
}

QVector<QImage> idtConvertedImagesFor(const QVector<GpuLayerInput>& layers)
{
    const aces::ColorSpace outSpace = outputSpaceFor(layers);

    QVector<QImage> converted;
    converted.reserve(layers.size());
    for (const GpuLayerInput& in : layers) {
        const aces::ColorSpace inSpace = clipcolor::acesSpaceFor(in.colorMeta);
        const bool passthrough =
            in.colorMeta.transfer == clipcolor::Transfer::PQ
            || in.colorMeta.transfer == clipcolor::Transfer::HLG
            || inSpace == outSpace;
        converted.push_back(passthrough
                            ? in.image
                            : convertLayerNoCache(in.image, in.colorMeta, outSpace));
    }
    return converted;
}

QImage cpuIdtMatteOracle(const QVector<GpuLayerInput>& layers, QSize canvas)
{
    return hdrcomposite::compositeReference16Matte(
        descsFor(layers), idtConvertedImagesFor(layers), canvas);
}

QVector<QImage> idtConvertedImagesExceptMatteSource(
    const QVector<GpuLayerInput>& layers,
    int matteSourceIndex)
{
    QVector<QImage> converted = idtConvertedImagesFor(layers);
    if (matteSourceIndex >= 0 && matteSourceIndex < layers.size())
        converted[matteSourceIndex] = layers.at(matteSourceIndex).image;
    return converted;
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

bool runCacheBudgetGate()
{
    const QSize canvas(64, 48);
    const clipcolor::ColorMeta rec2020 = meta(clipcolor::Primaries::Rec2020);
    const QImage src = makePremul16(canvas, [canvas](int x, int y,
                                                     quint16& r, quint16& g,
                                                     quint16& b, quint16& a) {
        const quint16 delta = static_cast<quint16>((y * canvas.width() + x) & 0x0f);
        r = static_cast<quint16>(32000 + delta);
        g = static_cast<quint16>(24000 + delta);
        b = static_cast<quint16>(16000 + delta);
        a = kMax16;
    });
    const QImage cached = clipcolor::toUnifiedSpace(src, rec2020, aces::ColorSpace::sRGB);
    const QImage oracle = convertLayerNoCache(src, rec2020, aces::ColorSpace::sRGB);
    const Metrics16 m = compare16(cached, oracle);
    const bool ok = m.valid && m.rgb16Mae <= 64.0 && m.a16Mae == 0.0;
    std::printf("[gpu-idt-parity] %s G5 CPU cache-budget sanity check: cached toUnifiedSpace stays within direct aces:: budget "
                "(rgb16Mae=%.3f <= 64.0, a16Mae=%.3f == 0)\n",
                ok ? "PASS" : "FAIL", m.rgb16Mae, m.a16Mae);
    return ok;
}

} // namespace

int runGpuIdtParitySelftest()
{
    int passed = 0;
    int total = 0;

    ++total;
    const bool cacheGateOk = runCacheBudgetGate();
    if (cacheGateOk)
        ++passed;

    if (!qApp) {
        std::printf("[gpu-idt-parity] SKIP: no QApplication instance (GL unavailable)\n");
        return cacheGateOk ? 0 : 1;
    }

    GpuLayerCompositor gpu;
    if (!gpu.isAvailable()) {
        std::printf("[gpu-idt-parity] SKIP: no usable GL context (headless/WSL)\n");
        return cacheGateOk ? 0 : 1;
    }

    const clipcolor::ColorMeta rec709 = meta(clipcolor::Primaries::Rec709);
    const clipcolor::ColorMeta rec2020 = meta(clipcolor::Primaries::Rec2020);
    const clipcolor::ColorMeta displayP3 = meta(clipcolor::Primaries::DisplayP3);
    const clipcolor::ColorMeta acescg =
        meta(clipcolor::Primaries::ACEScg, clipcolor::Transfer::Linear);
    const clipcolor::ColorMeta rec2020Pq =
        meta(clipcolor::Primaries::Rec2020, clipcolor::Transfer::PQ, true);

    const QSize probeCanvas(64, 48);
    {
        QVector<GpuLayerInput> probe;
        probe.push_back(makeLayer(rec709Signal16(probeCanvas), rec709,
                                  /*track*/0, 1.0));
        const QImage probeImg = gpu.composite16Idt(probe, probeCanvas);
        if (probeImg.isNull() || probeImg.size() != probeCanvas
            || probeImg.format() != QImage::Format_RGBA64_Premultiplied) {
            std::printf("[gpu-idt-parity] SKIP: RGBA16 IDT FBO unavailable\n");
            return cacheGateOk ? 0 : 1;
        }
    }

    auto gate = [&](int g, const char* desc,
                    const QVector<GpuLayerInput>& layers, QSize canvas,
                    double ssimMin, double rgbMaeMax, double aMaeMax) {
        ++total;
        const QImage cpu = cpuIdtOracle(layers, canvas);
        const QImage gpuImg = gpu.composite16Idt(layers, canvas);
        const Metrics16 m = compare16(cpu, gpuImg);
        const bool ok = m.valid
                        && gpuImg.format() == QImage::Format_RGBA64_Premultiplied
                        && m.ssim >= ssimMin
                        && m.rgb16Mae <= rgbMaeMax
                        && m.a16Mae <= aMaeMax;
        std::printf("[gpu-idt-parity] %s G%d %s "
                    "(SSIM=%.5f >= %.3f, rgb16Mae=%.3f <= %.1f, a16Mae=%.3f <= %.1f)\n",
                    ok ? "PASS" : "FAIL", g, desc,
                    m.ssim, ssimMin, m.rgb16Mae, rgbMaeMax,
                    m.a16Mae, aMaeMax);
        if (ok)
            ++passed;
    };

    const QSize canvas(96, 64);
    const double ssimMin = 0.98;
    const double maeMax = 20.0;

    {
        QVector<GpuLayerInput> layers;
        layers.push_back(makeLayer(rec709Signal16(canvas), rec709,
                                   /*track*/0, 1.0));
        gate(1, "single Rec709/sRGB layer passthrough identity",
             layers, canvas, ssimMin, maeMax, maeMax);
    }

    {
        QVector<GpuLayerInput> layers;
        layers.push_back(makeLayer(transparent16(canvas), rec709,
                                   /*track*/0, 1.0));
        layers.push_back(makeLayer(rec2020Signal16(canvas), rec2020,
                                   /*track*/1, 1.0));
        gate(2, "Rec2020 overlay converted to V1 Rec709/sRGB (asymmetric matrix)",
             layers, canvas, ssimMin, maeMax, maeMax);
    }

    {
        QVector<GpuLayerInput> layers;
        layers.push_back(makeLayer(transparent16(canvas), rec709,
                                   /*track*/0, 1.0));
        layers.push_back(makeLayer(rec2020Signal16(canvas), rec2020,
                                   /*track*/1, 0.5));
        gate(3, "opacity 0.5 on converted Rec2020 overlay",
             layers, canvas, ssimMin, maeMax, maeMax);
    }

    {
        QVector<GpuLayerInput> layers;
        layers.push_back(makeLayer(transparent16(canvas), rec709,
                                   /*track*/0, 1.0));
        layers.push_back(makeLayer(acescgSignal16(canvas), acescg,
                                   /*track*/1, 1.0));
        gate(6, "ACEScg linear overlay converted to V1 Rec709/sRGB",
             layers, canvas, ssimMin, maeMax, maeMax);
    }

    {
        QVector<GpuLayerInput> layers;
        layers.push_back(makeLayer(transparent16(canvas), acescg,
                                   /*track*/0, 1.0));
        layers.push_back(makeLayer(rec709Signal16(canvas), rec709,
                                   /*track*/1, 1.0));
        gate(7, "Rec709/sRGB overlay converted to V1 ACEScg linear output",
             layers, canvas, ssimMin, maeMax, maeMax);
    }

    {
        QVector<GpuLayerInput> layers;
        layers.push_back(makeLayer(transparent16(canvas), rec709,
                                   /*track*/0, 1.0));
        layers.push_back(makeLayer(rec2020Signal16(canvas), displayP3,
                                   /*track*/1, 1.0));
        gate(8, "DisplayP3 overlay converted to V1 Rec709/sRGB",
             layers, canvas, ssimMin, maeMax, maeMax);
    }

    {
        ++total;
        QVector<GpuLayerInput> layers;
        layers.push_back(makeLayer(transparent16(canvas), rec709,
                                   /*track*/0, 1.0));
        layers.push_back(makeLayer(rec2020Signal16(canvas), rec2020Pq,
                                   /*track*/1, 1.0));

        const QImage cpu = cpuIdtOracle(layers, canvas);
        const QImage gpuImg = gpu.composite16Idt(layers, canvas);
        const Metrics16 m = compare16(cpu, gpuImg);

        QVector<QImage> unchanged;
        unchanged.push_back(layers[0].image);
        unchanged.push_back(layers[1].image);
        const QImage plain = hdrcomposite::compositeReference16(
            descsFor(layers), unchanged, canvas);

        QVector<QImage> converted;
        converted.push_back(layers[0].image);
        converted.push_back(convertLayerNoCache(layers[1].image, rec2020,
                                                aces::ColorSpace::sRGB));
        const QImage wouldConvert = hdrcomposite::compositeReference16(
            descsFor(layers), converted, canvas);
        const Metrics16 unchangedDiff = compare16(gpuImg, plain);
        const Metrics16 conversionSignal = compare16(plain, wouldConvert);

        // Rec2020->sRGB conversion produces much larger MAE here; this floor
        // only proves the PQ passthrough result is not the would-convert image.
        const bool ok = m.valid
                        && gpuImg.format() == QImage::Format_RGBA64_Premultiplied
                        && m.ssim >= ssimMin
                        && m.rgb16Mae <= maeMax
                        && m.a16Mae <= maeMax
                        && unchangedDiff.valid
                        && unchangedDiff.rgb16Mae <= maeMax
                        && conversionSignal.valid
                        && conversionSignal.rgb16Mae > 200.0;
        std::printf("[gpu-idt-parity] %s G4 PQ Rec2020 overlay passthrough, not converted "
                    "(SSIM=%.5f >= %.3f, rgb16Mae=%.3f <= %.1f, "
                    "unchangedMae=%.3f <= %.1f, convertSignal=%.3f > 200.0)\n",
                    ok ? "PASS" : "FAIL",
                    m.ssim, ssimMin, m.rgb16Mae, maeMax,
                    unchangedDiff.rgb16Mae, maeMax,
                    conversionSignal.rgb16Mae);
        if (ok)
            ++passed;
    }

    std::printf("[gpu-idt-parity] Result: %d/%d PASSED\n", passed, total);
    return (passed == total) ? 0 : 1;
}

int runGpuIdtMatteParitySelftest()
{
    if (!qApp) {
        std::printf("[gpu-idt-matte-parity] SKIP: no QApplication instance (GL unavailable)\n");
        return 0;
    }

    GpuLayerCompositor gpu;
    if (!gpu.isAvailable()) {
        std::printf("[gpu-idt-matte-parity] SKIP: no usable GL context (headless/WSL)\n");
        return 0;
    }

    const clipcolor::ColorMeta rec709 = meta(clipcolor::Primaries::Rec709);
    const clipcolor::ColorMeta rec2020 = meta(clipcolor::Primaries::Rec2020);
    const clipcolor::ColorMeta displayP3 = meta(clipcolor::Primaries::DisplayP3);

    const QSize probeCanvas(64, 48);
    {
        QVector<GpuLayerInput> probe;
        GpuLayerInput masked = makeLayer(rec709Signal16(probeCanvas), rec709,
                                         /*track*/0, 1.0);
        masked.desc.matteType = gpucomposite::MatteType::Alpha;
        masked.desc.matteSourceIndex = 1;
        probe.push_back(masked);
        probe.push_back(makeLayer(alphaMatteSignal16(probeCanvas), rec2020,
                                  /*track*/1, 0.25));
        const QImage probeImg = gpu.composite16IdtMatte(probe, probeCanvas);
        if (probeImg.isNull() || probeImg.size() != probeCanvas
            || probeImg.format() != QImage::Format_RGBA64_Premultiplied) {
            std::printf("[gpu-idt-matte-parity] SKIP: RGBA16 IDT matte FBO unavailable\n");
            return 0;
        }
    }

    int passed = 0;
    int total = 0;

    auto gate = [&](int g, const char* desc,
                    const QVector<GpuLayerInput>& layers, QSize canvas,
                    double ssimMin, double rgbMaeMax, double aMaeMax) {
        ++total;
        const QImage cpu = cpuIdtMatteOracle(layers, canvas);
        const QImage gpuImg = gpu.composite16IdtMatte(layers, canvas);
        const Metrics16 m = compare16(cpu, gpuImg);
        const bool ok = m.valid
                        && gpuImg.format() == QImage::Format_RGBA64_Premultiplied
                        && m.ssim >= ssimMin
                        && m.rgb16Mae <= rgbMaeMax
                        && m.a16Mae <= aMaeMax;
        std::printf("[gpu-idt-matte-parity] %s G%d %s "
                    "(SSIM=%.5f >= %.3f, rgb16Mae=%.3f <= %.1f, a16Mae=%.3f <= %.1f)\n",
                    ok ? "PASS" : "FAIL", g, desc,
                    m.ssim, ssimMin, m.rgb16Mae, rgbMaeMax,
                    m.a16Mae, aMaeMax);
        if (ok)
            ++passed;
    };

    const QSize canvas(96, 64);
    const double ssimMin = 0.998;
    const double maeMax = 64.0;

    {
        ++total;
        QVector<GpuLayerInput> layers;
        GpuLayerInput masked = makeLayer(rec709Signal16(canvas), rec709,
                                         /*track*/0, 1.0);
        masked.desc.matteType = gpucomposite::MatteType::Luminance;
        masked.desc.matteSourceIndex = 1;
        layers.push_back(masked);
        layers.push_back(makeLayer(rec2020LumaMatteSignal16(canvas), rec2020,
                                   /*track*/1, 0.20));
        // No covering top layer: the V1 masked layer (paint order V1-wins) sits
        // over a transparent canvas so the luma-matte modulation drives the
        // output directly. A covering opaque layer would mask the matte'd region
        // and dilute the negative control (rawSignal) below the >256 floor, so a
        // matte-source-not-converted regression could hide under the parity slack.

        const QImage cpu = cpuIdtMatteOracle(layers, canvas);
        const QImage gpuImg = gpu.composite16IdtMatte(layers, canvas);
        const Metrics16 m = compare16(cpu, gpuImg);
        const QImage rawMatteOracle = hdrcomposite::compositeReference16Matte(
            descsFor(layers), idtConvertedImagesExceptMatteSource(layers, 1), canvas);
        const Metrics16 rawSignal = compare16(cpu, rawMatteOracle);
        const bool ok = m.valid
                        && gpuImg.format() == QImage::Format_RGBA64_Premultiplied
                        && m.ssim >= ssimMin
                        && m.rgb16Mae <= maeMax
                        && m.a16Mae <= maeMax
                        && rawSignal.valid
                        && rawSignal.rgb16Mae > 256.0;
        std::printf("[gpu-idt-matte-parity] %s G1 cross-primaries Rec2020 luma matte source over sRGB V1 "
                    "(SSIM=%.5f >= %.3f, rgb16Mae=%.3f <= %.1f, a16Mae=%.3f <= %.1f, "
                    "rawMatteSignal=%.3f > 256.0)\n",
                    ok ? "PASS" : "FAIL",
                    m.ssim, ssimMin, m.rgb16Mae, maeMax,
                    m.a16Mae, maeMax, rawSignal.rgb16Mae);
        if (ok)
            ++passed;
    }

    {
        QVector<GpuLayerInput> layers;
        GpuLayerInput masked = makeLayer(rec709Signal16(canvas), rec709,
                                         /*track*/0, 0.85);
        masked.desc.matteType = gpucomposite::MatteType::Alpha;
        masked.desc.matteSourceIndex = 1;
        layers.push_back(masked);
        layers.push_back(makeLayer(alphaMatteSignal16(canvas), rec2020,
                                   /*track*/1, 0.15));
        layers.push_back(makeLayer(rec2020Signal16(canvas), displayP3,
                                   /*track*/2, 1.0));
        gate(2, "alpha matte keeps mask from transformed matte-source alpha",
             layers, canvas, ssimMin, maeMax, maeMax);
    }

    {
        QVector<GpuLayerInput> layers;
        GpuLayerInput masked = makeLayer(rec709Signal16(canvas), rec709,
                                         /*track*/0, 1.0);
        masked.desc.matteType = gpucomposite::MatteType::Luminance;
        masked.desc.matteSourceIndex = 1;
        layers.push_back(masked);
        layers.push_back(makeLayer(rec709Signal16(canvas), rec709,
                                   /*track*/1, 0.40));
        layers.push_back(makeLayer(transparent16(canvas), rec709,
                                   /*track*/2, 1.0));
        gate(3, "same-space passthrough luma matte",
             layers, canvas, ssimMin, maeMax, maeMax);
    }

    std::printf("[gpu-idt-matte-parity] Result: %d/%d PASSED\n", passed, total);
    return (passed == total) ? 0 : 1;
}
