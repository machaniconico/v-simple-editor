// matte16_parity_selftests.cpp
// Stage7 S7-1: headless parity for the dormant RGBA64 track-matte compositor.

#include "../AcesColor.h"
#include "../LayerCompositor.h"
#include "../TrackMatteBake.h"
#include "../color/ClipOdt.h"
#include "../playback/HdrCompositeMath.h"
#include "../playback/TrackMatteCompose16.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include <QImage>
#include <QRgba64>
#include <QSize>
#include <QtGlobal>
#include <QVector>

namespace {

constexpr quint32 kMax16 = hdrcomposite::kMax16;

static_assert(static_cast<int>(gpucomposite::MatteType::None)
              == static_cast<int>(TrackMatteType::None),
              "gpucomposite::MatteType::None must match TrackMatteType::None");
static_assert(static_cast<int>(gpucomposite::MatteType::Alpha)
              == static_cast<int>(TrackMatteType::AlphaMatte),
              "gpucomposite::MatteType::Alpha must match TrackMatteType::AlphaMatte");
static_assert(static_cast<int>(gpucomposite::MatteType::AlphaInverted)
              == static_cast<int>(TrackMatteType::AlphaInvertedMatte),
              "gpucomposite::MatteType::AlphaInverted must match TrackMatteType::AlphaInvertedMatte");
static_assert(static_cast<int>(gpucomposite::MatteType::Luminance)
              == static_cast<int>(TrackMatteType::LumaMatte),
              "gpucomposite::MatteType::Luminance must match TrackMatteType::LumaMatte");
static_assert(static_cast<int>(gpucomposite::MatteType::LuminanceInverted)
              == static_cast<int>(TrackMatteType::LumaInvertedMatte),
              "gpucomposite::MatteType::LuminanceInverted must match TrackMatteType::LumaInvertedMatte");

quint16 lift8(int v)
{
    return static_cast<quint16>(qBound(0, v, 255) * 257);
}

quint16 clampTo16(double normalized)
{
    if (!std::isfinite(normalized) || normalized <= 0.0)
        return 0;
    if (normalized >= 1.0)
        return static_cast<quint16>(kMax16);
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
    const quint32 straight =
        qBound(0u, static_cast<quint32>(premul) * kMax16 / alpha, kMax16);
    return static_cast<quint16>(straight);
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

QImage makeBase16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = lift8((35 + x * 19 + y * 5) & 0xff);
        g = lift8((80 + x * 7 + y * 23) & 0xff);
        b = lift8((150 + x * 13 + y * 11) & 0xff);
        a = static_cast<quint16>(kMax16);
    });
}

QImage makeForeground16(QSize size, quint16 alpha = static_cast<quint16>(kMax16))
{
    return makePremul16(size, [alpha](int x, int y,
                                      quint16& r, quint16& g, quint16& b, quint16& a) {
        r = lift8((210 + x * 9 + y * 3) & 0xff);
        g = lift8((35 + x * 5 + y * 17) & 0xff);
        b = lift8((70 + x * 21 + y * 7) & 0xff);
        a = alpha;
    });
}

QImage makeAlphaMatte16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = lift8((20 + x * 13) & 0xff);
        g = lift8((90 + y * 29) & 0xff);
        b = lift8((40 + (x + y) * 11) & 0xff);
        a = lift8((24 + x * 31 + y * 17) & 0xff);
    });
}

QImage makeLumaMatte16(QSize size)
{
    return makePremul16(size, [](int x, int y,
                                 quint16& r, quint16& g, quint16& b, quint16& a) {
        r = lift8((10 + x * 33 + y * 3) & 0xff);
        g = lift8((50 + x * 5 + y * 37) & 0xff);
        b = lift8((100 + x * 17 + y * 9) & 0xff);
        a = static_cast<quint16>(kMax16);
    });
}

QImage makeSolidPremul16(QSize size, quint16 r, quint16 g, quint16 b, quint16 a)
{
    return makePremul16(size, [=](int, int,
                                  quint16& rr, quint16& gg, quint16& bb, quint16& aa) {
        rr = r;
        gg = g;
        bb = b;
        aa = a;
    });
}

QImage makeLinearGray16(QSize size, double linear)
{
    const quint16 v = clampTo16(linear);
    return makeSolidPremul16(size, v, v, v, static_cast<quint16>(kMax16));
}

QImage to8(const QImage& img)
{
    return hdrcomposite::to8bit(img);
}

TrackMatteType toTrackMatteType(gpucomposite::MatteType type)
{
    switch (type) {
    case gpucomposite::MatteType::Alpha:
        return TrackMatteType::AlphaMatte;
    case gpucomposite::MatteType::AlphaInverted:
        return TrackMatteType::AlphaInvertedMatte;
    case gpucomposite::MatteType::Luminance:
        return TrackMatteType::LumaMatte;
    case gpucomposite::MatteType::LuminanceInverted:
        return TrackMatteType::LumaInvertedMatte;
    case gpucomposite::MatteType::None:
    default:
        return TrackMatteType::None;
    }
}

struct Stack {
    QVector<gpucomposite::LayerDesc> descs;
    QVector<QImage> images16;
    QVector<CompositeLayer> layers8;
    QVector<QImage> images8;
};

void appendLayer(Stack& stack,
                 const QImage& image16,
                 QSize canvas,
                 gpucomposite::MatteType matteType = gpucomposite::MatteType::None,
                 int matteSourceIndex = -1,
                 bool visible = true,
                 double opacity = 1.0)
{
    gpucomposite::LayerDesc desc;
    desc.sourceTrack = stack.descs.size();
    desc.srcSize = image16.isNull() ? canvas : image16.size();
    desc.visible = visible;
    desc.opacity = opacity;
    desc.matteType = matteType;
    desc.matteSourceIndex = matteSourceIndex;
    stack.descs.push_back(desc);
    stack.images16.push_back(image16);

    CompositeLayer layer;
    layer.visible = visible;
    layer.opacity = opacity;
    layer.blendMode = BlendMode::Normal;
    layer.matteType = toTrackMatteType(matteType);
    layer.matteSourceLayerIndex = matteSourceIndex;
    stack.layers8.push_back(layer);
    stack.images8.push_back(to8(image16));
}

Stack makeParityStack(QSize canvas,
                      gpucomposite::MatteType matteType,
                      const QImage& matte16,
                      const QImage& foreground16 = QImage())
{
    Stack stack;
    appendLayer(stack, makeBase16(canvas), canvas);
    appendLayer(stack,
                foreground16.isNull() ? makeForeground16(canvas) : foreground16,
                canvas, matteType, 2);
    appendLayer(stack, matte16, canvas);
    return stack;
}

int maxPerChannelDelta(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return 65535;

    const QImage aa = a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage bb = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int maxDelta = 0;
    for (int y = 0; y < aa.height(); ++y) {
        const QRgb* la = reinterpret_cast<const QRgb*>(aa.constScanLine(y));
        const QRgb* lb = reinterpret_cast<const QRgb*>(bb.constScanLine(y));
        for (int x = 0; x < aa.width(); ++x) {
            maxDelta = std::max(maxDelta, std::abs(qRed(la[x]) - qRed(lb[x])));
            maxDelta = std::max(maxDelta, std::abs(qGreen(la[x]) - qGreen(lb[x])));
            maxDelta = std::max(maxDelta, std::abs(qBlue(la[x]) - qBlue(lb[x])));
            maxDelta = std::max(maxDelta, std::abs(qAlpha(la[x]) - qAlpha(lb[x])));
        }
    }
    return maxDelta;
}

QRgba64 rawPixel16(const QImage& img, int x = 0, int y = 0)
{
    const QImage rgba64 = img.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    return reinterpret_cast<const QRgba64*>(rgba64.constScanLine(y))[x];
}

bool parityOk(const Stack& stack,
              QSize canvas,
              const trackmatte16::MatteColorCtx& ctx,
              int maxAllowedDelta,
              int* deltaOut = nullptr)
{
    const QImage got16 = trackmatte16::composeExport(stack.descs, stack.images16, canvas, ctx);
    const QImage got8 = hdrcomposite::to8bit(got16);
    const QImage want8 = trackmatte::composite(stack.layers8, stack.images8, canvas);
    const int delta = maxPerChannelDelta(got8, want8);
    if (deltaOut)
        *deltaOut = delta;
    return got16.format() == QImage::Format_RGBA64_Premultiplied
        && delta <= maxAllowedDelta;
}

quint16 luma16(quint16 r, quint16 g, quint16 b)
{
    const quint32 luma = static_cast<quint32>(0.299 * r + 0.587 * g + 0.114 * b);
    return static_cast<quint16>(qBound(0u, luma, kMax16));
}

} // namespace

int runMatte16ParitySelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[matte16-parity] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    const QSize canvas(11, 9);
    trackmatte16::MatteColorCtx displayCtx;
    displayCtx.matteSourceIsLinear = false;

    {
        const Stack stack = makeParityStack(
            canvas, gpucomposite::MatteType::Alpha, makeAlphaMatte16(canvas));
        int delta = 0;
        const bool ok = parityOk(stack, canvas, displayCtx, 6, &delta);
        check(1, "AlphaMatte parity to 8-bit SSOT (max channel delta <= 6)", ok);
    }

    {
        const Stack stack = makeParityStack(
            canvas, gpucomposite::MatteType::AlphaInverted, makeAlphaMatte16(canvas));
        int delta = 0;
        const bool ok = parityOk(stack, canvas, displayCtx, 6, &delta);
        check(2, "AlphaInvertedMatte parity to 8-bit SSOT (max channel delta <= 6)", ok);
    }

    {
        const Stack stack = makeParityStack(
            canvas, gpucomposite::MatteType::Luminance, makeLumaMatte16(canvas));
        int delta = 0;
        const bool ok = parityOk(stack, canvas, displayCtx, 6, &delta);
        check(3, "LumaMatte opaque-source parity to 8-bit SSOT (max channel delta <= 6)", ok);
    }

    {
        const Stack stack = makeParityStack(
            canvas, gpucomposite::MatteType::LuminanceInverted, makeLumaMatte16(canvas));
        int delta = 0;
        const bool ok = parityOk(stack, canvas, displayCtx, 6, &delta);
        check(4, "LumaInvertedMatte opaque-source parity to 8-bit SSOT (max channel delta <= 6)", ok);
    }

    {
        const QImage translucentFg = makeForeground16(canvas, lift8(128));
        const QImage opaqueAlphaMatte =
            makeSolidPremul16(canvas, lift8(255), lift8(255), lift8(255),
                              static_cast<quint16>(kMax16));
        Stack stack;
        appendLayer(stack, QImage(), canvas);
        appendLayer(stack, translucentFg, canvas, gpucomposite::MatteType::Alpha, 2);
        appendLayer(stack, opaqueAlphaMatte, canvas);
        const QImage got16 = trackmatte16::composeExport(stack.descs, stack.images16,
                                                         canvas, displayCtx);
        const QImage got8 = hdrcomposite::to8bit(got16);
        const QImage want8 = trackmatte::composite(stack.layers8, stack.images8, canvas);
        const int delta = maxPerChannelDelta(got8, want8);
        const QRgba64 px = rawPixel16(got16, 3, 2);
        const bool premulStillValid = px.red() <= px.alpha()
                                   && px.green() <= px.alpha()
                                   && px.blue() <= px.alpha();
        hdrcomposite::Rgba16 helperPx{30000, 20000, 10000, lift8(128)};
        const hdrcomposite::Rgba16 before = helperPx;
        const quint16 mask = lift8(128);
        trackmatte16::applyMaskPremul16(helperPx, mask);
        const bool helperScalesTogether =
            helperPx.r == static_cast<quint16>(static_cast<quint32>(before.r) * mask / kMax16)
            && helperPx.g == static_cast<quint16>(static_cast<quint32>(before.g) * mask / kMax16)
            && helperPx.b == static_cast<quint16>(static_cast<quint32>(before.b) * mask / kMax16)
            && helperPx.a == static_cast<quint16>(static_cast<quint32>(before.a) * mask / kMax16);
        check(5, "translucent foreground with opaque AlphaMatte preserves premul and helper scales rgb/a",
              delta <= 6 && premulStillValid && helperScalesTogether);
    }

    {
        Stack invalidSource;
        appendLayer(invalidSource, makeBase16(canvas), canvas);
        appendLayer(invalidSource, makeForeground16(canvas), canvas,
                    gpucomposite::MatteType::Alpha, 0);
        appendLayer(invalidSource, QImage(), canvas);
        const bool invalidPlain = parityOk(invalidSource, canvas, displayCtx, 6);

        Stack consumedSource;
        appendLayer(consumedSource, makeBase16(canvas), canvas);
        appendLayer(consumedSource, makeForeground16(canvas), canvas,
                    gpucomposite::MatteType::Alpha, 2);
        appendLayer(consumedSource,
                    makeSolidPremul16(canvas, lift8(0), lift8(255), lift8(0),
                                      static_cast<quint16>(kMax16)),
                    canvas);
        const bool excludedParity = parityOk(consumedSource, canvas, displayCtx, 6);

        Stack plain = consumedSource;
        plain.descs[1].matteType = gpucomposite::MatteType::None;
        plain.descs[1].matteSourceIndex = -1;
        plain.layers8[1].matteType = TrackMatteType::None;
        plain.layers8[1].matteSourceLayerIndex = -1;
        const QImage excluded = hdrcomposite::to8bit(
            trackmatte16::composeExport(consumedSource.descs, consumedSource.images16,
                                        canvas, displayCtx));
        const QImage notExcluded = hdrcomposite::to8bit(
            trackmatte16::composeExport(plain.descs, plain.images16, canvas, displayCtx));
        const bool sourceReallyExcluded = maxPerChannelDelta(excluded, notExcluded) > 16;

        check(6, "index-space contract: index0 invalid plain, valid matte source excluded",
              invalidPlain && excludedParity && sourceReallyExcluded);
    }

    {
        const int width = 1200;
        const QSize precisionCanvas(width, 1);
        Stack stack;
        appendLayer(stack, QImage(), precisionCanvas);
        appendLayer(stack,
                    makeSolidPremul16(precisionCanvas,
                                      static_cast<quint16>(kMax16),
                                      static_cast<quint16>(kMax16),
                                      static_cast<quint16>(kMax16),
                                      static_cast<quint16>(kMax16)),
                    precisionCanvas, gpucomposite::MatteType::Luminance, 2);
        QImage matte(precisionCanvas, QImage::Format_RGBA64_Premultiplied);
        QRgba64* matteLine = reinterpret_cast<QRgba64*>(matte.scanLine(0));
        std::set<quint16> expectedLevels;
        for (int x = 0; x < width; ++x) {
            const quint16 r = static_cast<quint16>(20000 + x);
            matteLine[x] = qRgba64(r, 0, 0, static_cast<quint16>(kMax16));
            expectedLevels.insert(luma16(r, 0, 0));
        }
        appendLayer(stack, matte, precisionCanvas);

        const QImage out16 = trackmatte16::composeExport(stack.descs, stack.images16,
                                                         precisionCanvas, displayCtx);
        const QImage out8 = hdrcomposite::to8bit(out16);
        const QRgba64* outLine16 = reinterpret_cast<const QRgba64*>(out16.constScanLine(0));
        const QImage out8Argb = out8.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QRgb* outLine8 = reinterpret_cast<const QRgb*>(out8Argb.constScanLine(0));
        std::set<quint16> levels16;
        std::set<int> levels8;
        for (int x = 0; x < width; ++x) {
            levels16.insert(outLine16[x].red());
            levels8.insert(qRed(outLine8[x]));
        }
        check(7, "16-bit luma matte preserves >256 distinct masked levels",
              out16.format() == QImage::Format_RGBA64_Premultiplied
              && expectedLevels.size() > 256
              && levels16.size() > 256
              && levels8.size() <= 256);
    }

    {
        const QSize odtCanvas(3, 2);
        Stack stack;
        appendLayer(stack, QImage(), odtCanvas);
        appendLayer(stack,
                    makeSolidPremul16(odtCanvas,
                                      static_cast<quint16>(kMax16),
                                      static_cast<quint16>(kMax16),
                                      static_cast<quint16>(kMax16),
                                      static_cast<quint16>(kMax16)),
                    odtCanvas, gpucomposite::MatteType::Luminance, 2);
        const QImage linearMatte = makeLinearGray16(odtCanvas, 0.18);
        appendLayer(stack, linearMatte, odtCanvas);

        trackmatte16::MatteColorCtx linearCtx;
        linearCtx.matteSourceIsLinear = true;
        linearCtx.odtForLuma = clipodt::OdtParams{aces::ColorSpace::Rec709, false};

        const QImage out16 = trackmatte16::composeExport(stack.descs, stack.images16,
                                                         odtCanvas, linearCtx);
        const QRgba64 got = rawPixel16(out16);

        const QImage displayMatte = clipodt::applyOdt16(linearMatte, linearCtx.odtForLuma);
        const QRgba64 displayPx = rawPixel16(displayMatte);
        const quint16 displayR = unpremultiply16(displayPx.red(), displayPx.alpha());
        const quint16 displayG = unpremultiply16(displayPx.green(), displayPx.alpha());
        const quint16 displayB = unpremultiply16(displayPx.blue(), displayPx.alpha());
        const quint16 expectedMask = luma16(displayR, displayG, displayB);

        const QRgba64 rawLinearPx = rawPixel16(linearMatte);
        const quint16 rawLinearMask = luma16(rawLinearPx.red(),
                                            rawLinearPx.green(),
                                            rawLinearPx.blue());

        const bool matchesOdtLuma = std::abs(int(got.red()) - int(expectedMask)) <= 2
                                 && std::abs(int(got.green()) - int(expectedMask)) <= 2
                                 && std::abs(int(got.blue()) - int(expectedMask)) <= 2
                                 && std::abs(int(got.alpha()) - int(expectedMask)) <= 2;
        const bool differsFromRawLinear =
            std::abs(int(expectedMask) - int(rawLinearMask)) > 4096;

        check(8, "ODT luma-space proof uses display-referred Rec.601 luma",
              matchesOdtLuma && differsFromRawLinear);
    }

    std::printf("[matte16-parity] summary: gates=8 passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
