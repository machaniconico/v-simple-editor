#include "SelftestRegistry.h"
// matte16_parity_selftests.cpp
// Stage7 S7-1: headless parity for the dormant RGBA64 track-matte compositor.

#include "../AcesColor.h"
#include "../LayerCompositor.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TrackMatteBake.h"
#include "../TrackMatteKey.h"
#include "../color/ClipOdt.h"
#include "../playback/HdrCompositeMath.h"
#include "../playback/TrackMatteCompose16.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include <QCryptographicHash>
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
                 double opacity = 1.0,
                 BlendMode blendMode = BlendMode::Normal)
{
    gpucomposite::LayerDesc desc;
    desc.sourceTrack = stack.descs.size();
    desc.srcSize = image16.isNull() ? canvas : image16.size();
    desc.visible = visible;
    desc.opacity = opacity;
    desc.matteType = matteType;
    desc.matteSourceIndex = matteSourceIndex;
    desc.blendModeOrdinal = static_cast<int>(blendMode);
    stack.descs.push_back(desc);
    stack.images16.push_back(image16);

    CompositeLayer layer;
    layer.visible = visible;
    layer.opacity = opacity;
    layer.blendMode = blendMode;
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

// G9 (a2): LayerCompositor::blendPixel's equations on one premultiplied RGBA64
// pixel pair in double, rounded once at the end (the 16 bit reference).
QRgba64 blendRef16(QRgba64 base, QRgba64 top, BlendMode mode, double opacity)
{
    const double max = static_cast<double>(kMax16);
    const double topA = top.alpha() / max * opacity;
    if (topA <= 0.0)
        return base;
    const double baseA = base.alpha() / max;
    const double topAlpha = top.alpha() / max;
    const double baseRgb[3] = {baseA > 0.0 ? base.red() / max / baseA : 0.0,
                               baseA > 0.0 ? base.green() / max / baseA : 0.0,
                               baseA > 0.0 ? base.blue() / max / baseA : 0.0};
    const double topRgb[3] = {top.red() / max / topAlpha,
                              top.green() / max / topAlpha,
                              top.blue() / max / topAlpha};
    const double outA = topA + baseA * (1.0 - topA);
    if (outA <= 0.0)
        return qRgba64(0, 0, 0, 0);
    quint16 out[3];
    for (int c = 0; c < 3; ++c) {
        const double blend = LayerCompositor::blendChannel(baseRgb[c], topRgb[c], mode);
        const double rgb = (topA * ((1.0 - baseA) * topRgb[c] + baseA * blend)
                            + baseRgb[c] * baseA * (1.0 - topA)) / outA;
        out[c] = static_cast<quint16>(qBound(0.0, rgb * outA * max + 0.5, max));
    }
    return qRgba64(out[0], out[1], out[2],
                   static_cast<quint16>(qBound(0.0, outA * max + 0.5, max)));
}

int maxChannelDelta16(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return 65535;
    const QImage aa = a.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    int maxDelta = 0;
    for (int y = 0; y < aa.height(); ++y) {
        const QRgba64* la = reinterpret_cast<const QRgba64*>(aa.constScanLine(y));
        const QRgba64* lb = reinterpret_cast<const QRgba64*>(bb.constScanLine(y));
        for (int x = 0; x < aa.width(); ++x) {
            maxDelta = std::max(maxDelta, std::abs(int(la[x].red()) - int(lb[x].red())));
            maxDelta = std::max(maxDelta, std::abs(int(la[x].green()) - int(lb[x].green())));
            maxDelta = std::max(maxDelta, std::abs(int(la[x].blue()) - int(lb[x].blue())));
            maxDelta = std::max(maxDelta, std::abs(int(la[x].alpha()) - int(lb[x].alpha())));
        }
    }
    return maxDelta;
}

// Fixture E (G9 (a)): every pixel alpha 0 or 65535, channels 257 * v8, 2-valued
// alpha mattes only, an opaque full-canvas Normal base, opacity on the blended
// layer only.
Stack makeFixtureE(QSize canvas, BlendMode mode, double blendOpacity)
{
    auto twoValued = [canvas](int salt, int mod, int r0, int g0, int b0) {
        return makePremul16(canvas, [=](int x, int y,
                                        quint16& r, quint16& g, quint16& b, quint16& a) {
            r = lift8((r0 + x * 17 + y * 5) & 0xff);
            g = lift8((g0 + x * 7 + y * 29) & 0xff);
            b = lift8((b0 + x * 11 + y * 13) & 0xff);
            a = ((x * salt + y * 3) % mod) != 0 ? static_cast<quint16>(kMax16) : 0;
        });
    };
    Stack stack;
    appendLayer(stack, makeBase16(canvas), canvas);
    appendLayer(stack, twoValued(2, 4, 60, 200, 90), canvas,
                gpucomposite::MatteType::Alpha, 2);
    appendLayer(stack, twoValued(3, 4, 0, 0, 0), canvas);
    appendLayer(stack, twoValued(5, 4, 220, 40, 130), canvas,
                gpucomposite::MatteType::AlphaInverted, 4, true, blendOpacity, mode);
    appendLayer(stack, twoValued(1, 5, 0, 0, 0), canvas);
    return stack;
}

// G9 (a2): base, one blended layer (translucent foreground, optionally under a
// graded alpha matte) and the matte source when there is one.
Stack makeBlendStack(QSize canvas, const QImage& base16, BlendMode mode,
                     double opacity, bool withMatte)
{
    Stack stack;
    appendLayer(stack, base16, canvas);
    appendLayer(stack, makeForeground16(canvas, lift8(128)), canvas,
                withMatte ? gpucomposite::MatteType::Alpha : gpucomposite::MatteType::None,
                withMatte ? 2 : -1, true, opacity, mode);
    if (withMatte)
        appendLayer(stack, makeAlphaMatte16(canvas), canvas);
    return stack;
}

QString rawSha(const QImage& img)
{
    const QImage rgba64 = img.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < rgba64.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(rgba64.constScanLine(y)),
                                    rgba64.width() * 8));
    return QString::fromLatin1(hash.result().toHex().left(16));
}

// G9 (d)(e): shape-clip timeline, V1 opaque, V2 under an alpha matte from V3's
// left half, V4 a VFX layer at 0.8 with `mode`.
ClipInfo shapeClip16(const QColor& color, const QRectF& rect)
{
    Shape rectangle;
    rectangle.type = ShapeType::Rectangle;
    rectangle.position = rect.center();
    rectangle.properties.size = rect.size();
    rectangle.fill.color = color;
    rectangle.stroke.enabled = false;
    ClipInfo clip;
    clip.displayName = QStringLiteral("matte16-parity shape");
    clip.duration = clip.outPoint = 2.0;
    clip.shapes = {rectangle};
    return clip;
}

QImage renderMatteBlendFrame(BlendMode mode, bool hdr16, bool odt)
{
    constexpr int kW = 64;
    constexpr int kH = 36;
    struct EnvRestore {
        QByteArray name;
        QByteArray old;
        bool had;
        explicit EnvRestore(const char* n) : name(n), old(qgetenv(n)), had(qEnvironmentVariableIsSet(n)) {}
        ~EnvRestore()
        {
            if (had)
                qputenv(name, old);
            else
                qunsetenv(name);
        }
    };
    EnvRestore restoreHdr("VEDITOR_HDR_MATTE16");
    EnvRestore restoreOdt("VEDITOR_HDR_ODT");
    if (hdr16)
        qputenv("VEDITOR_HDR_MATTE16", "1");
    else
        qunsetenv("VEDITOR_HDR_MATTE16");
    if (odt)
        qputenv("VEDITOR_HDR_ODT", "1");
    else
        qunsetenv("VEDITOR_HDR_ODT");

    ClipInfo v1 = shapeClip16(QColor(200, 60, 40), QRectF(0, 0, kW, kH));
    ClipInfo v2 = shapeClip16(QColor(30, 160, 90), QRectF(0, 0, kW, kH));
    ClipInfo v3 = shapeClip16(QColor(255, 255, 255), QRectF(0, 0, kW / 2, kH));
    ClipInfo v4 = shapeClip16(QColor(40, 90, 170), QRectF(0, 0, kW, kH));
    v4.isVfxFootage = true;
    v4.blendMode = mode;
    v4.opacity = 0.8;

    Timeline timeline;
    timeline.setProjectOutputConfig(kW, kH, false, 10);
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{v1}, {v2}, {v3}, {v4}},
                                QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    QHash<QString, TimelineTrackMatteEntry> entries;
    TimelineTrackMatteEntry entry;
    entry.matteType = TrackMatteType::AlphaMatte;
    entry.matteSourceClipId = trackMatteClipKey(2, 0);
    entries.insert(trackMatteClipKey(1, 0), entry);
    timeline.setTrackMatteEntries(entries);
    return tlrender::renderFrameAt(&timeline, 500000, QSize(kW, kH));
}

qint64 diffPixelCount(const QImage& a, const QImage& b, int threshold)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return -1;
    const QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage ib = b.convertToFormat(QImage::Format_RGBA8888);
    qint64 count = 0;
    for (int y = 0; y < ia.height(); ++y) {
        const uchar* pa = ia.constScanLine(y);
        const uchar* pb = ib.constScanLine(y);
        for (int x = 0; x < ia.width(); ++x) {
            int px = 0;
            for (int c = 0; c < 4; ++c)
                px = std::max(px, std::abs(int(pa[x * 4 + c]) - int(pb[x * 4 + c])));
            if (px > threshold)
                ++count;
        }
    }
    return count;
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

    // G9: the effective blend mode rides in LayerDesc::blendModeOrdinal and
    // composeExport blends each blended layer after its matte and after the
    // Normal stack, ascending, with LayerCompositor's channel equations at 16 bit.
    {
        std::string why;
        const BlendMode modes[] = {BlendMode::Multiply, BlendMode::Screen,
                                   BlendMode::Overlay, BlendMode::Add};

        // (a) fixture E against the 8 bit kernel.
        const int kD0 = 0;   // measured: blend-less fixture E delta
        const QSize eCanvas(13, 11);
        int d0 = 0;
        {
            const Stack none = makeFixtureE(eCanvas, BlendMode::Normal, 1.0);
            const QImage got16 = trackmatte16::composeExport(none.descs, none.images16,
                                                             eCanvas, displayCtx);
            d0 = maxPerChannelDelta(hdrcomposite::to8bit(got16),
                                    trackmatte::composite(none.layers8, none.images8, eCanvas));
        }
        bool aOk = (d0 == kD0);
        // Fixture E must not be degenerate: the Normal layer's matte source has
        // both alpha values and the Normal layer shows through it somewhere.
        int srcOpaque = 0;
        int srcClear = 0;
        int normalVisible = 0;
        {
            const Stack e = makeFixtureE(eCanvas, BlendMode::Normal, 1.0);
            for (int y = 0; y < eCanvas.height(); ++y) {
                const QRgba64* layerLine = reinterpret_cast<const QRgba64*>(e.images16[1].constScanLine(y));
                const QRgba64* srcLine = reinterpret_cast<const QRgba64*>(e.images16[2].constScanLine(y));
                for (int x = 0; x < eCanvas.width(); ++x) {
                    if (srcLine[x].alpha() == kMax16) ++srcOpaque;
                    if (srcLine[x].alpha() == 0) ++srcClear;
                    if (srcLine[x].alpha() == kMax16 && layerLine[x].alpha() == kMax16) ++normalVisible;
                }
            }
            std::printf("[matte16-parity] G9 (a) fixture E matte source alpha 65535 px=%d alpha 0 px=%d, "
                        "Normal matted layer visible px=%d (expect all > 0)\n",
                        srcOpaque, srcClear, normalVisible);
            if (!(srcOpaque > 0 && srcClear > 0 && normalVisible > 0)) aOk = false;
        }
        int aWorst = 0;
        int aEffect = 0;
        for (BlendMode mode : modes) {
            for (double opacity : {1.0, 0.5}) {
                const Stack stack = makeFixtureE(eCanvas, mode, opacity);
                const QImage got16 = trackmatte16::composeExport(stack.descs, stack.images16,
                                                                 eCanvas, displayCtx);
                const int delta = maxPerChannelDelta(
                    hdrcomposite::to8bit(got16),
                    trackmatte::composite(stack.layers8, stack.images8, eCanvas));
                aWorst = std::max(aWorst, delta);
                if (delta > kD0 + 1)
                    aOk = false;
                const Stack normal = makeFixtureE(eCanvas, BlendMode::Normal, opacity);
                aEffect = std::max(aEffect, maxPerChannelDelta(
                    hdrcomposite::to8bit(got16),
                    hdrcomposite::to8bit(trackmatte16::composeExport(
                        normal.descs, normal.images16, eCanvas, displayCtx))));
            }
        }
        aOk = aOk && aEffect > 8;
        std::printf("[matte16-parity] G9 (a) d0=%d worstBlendDelta=%d (allow %d) blendVsNormal=%d (expect > 8)\n",
                    d0, aWorst, kD0 + 1, aEffect);
        if (!aOk) why += " (a)";

        // (a2) the 16 bit equations against a test-side reference, +-2/65535.
        const QImage translucentBase = makeForeground16(canvas, lift8(150));
        int a2Worst = 0;
        int a2Vs8 = 0;
        for (const QImage& base : {makeBase16(canvas), translucentBase}) {
            for (BlendMode mode : modes) {
                for (bool withMatte : {false, true}) {
                    for (double opacity : {1.0, 0.37}) {
                        const Stack stack = makeBlendStack(canvas, base, mode, opacity, withMatte);
                        const QImage got16 = trackmatte16::composeExport(
                            stack.descs, stack.images16, canvas, displayCtx);

                        Stack baseOnly;
                        appendLayer(baseOnly, base, canvas);
                        const QImage base16 = trackmatte16::composeExport(
                            baseOnly.descs, baseOnly.images16, canvas, displayCtx);
                        Stack topOnly;
                        appendLayer(topOnly, makeForeground16(canvas, lift8(128)), canvas,
                                    withMatte ? gpucomposite::MatteType::Alpha
                                              : gpucomposite::MatteType::None,
                                    withMatte ? 1 : -1);
                        if (withMatte)
                            appendLayer(topOnly, makeAlphaMatte16(canvas), canvas);
                        const QImage top16 = trackmatte16::composeExport(
                            topOnly.descs, topOnly.images16, canvas, displayCtx);

                        QImage want = base16;
                        for (int y = 0; y < canvas.height(); ++y) {
                            QRgba64* wl = reinterpret_cast<QRgba64*>(want.scanLine(y));
                            const QRgba64* bl = reinterpret_cast<const QRgba64*>(base16.constScanLine(y));
                            const QRgba64* tl = reinterpret_cast<const QRgba64*>(top16.constScanLine(y));
                            for (int x = 0; x < canvas.width(); ++x)
                                wl[x] = blendRef16(bl[x], tl[x], mode, opacity);
                        }
                        a2Worst = std::max(a2Worst, maxChannelDelta16(got16, want));
                        a2Vs8 = std::max(a2Vs8, maxPerChannelDelta(
                            hdrcomposite::to8bit(got16),
                            trackmatte::composite(stack.layers8, stack.images8, canvas)));
                    }
                }
            }
        }
        std::printf("[matte16-parity] G9 (a2) worstVsReference16=%d (allow 2) delta vs 8-bit kernel (recorded only)=%d\n",
                    a2Worst, a2Vs8);
        if (a2Worst > 2) why += " (a2)";

        // (b) more than 256 levels survive the blend, on the (a2) fixture's
        // output (matte, opacity 1.0, Screen). The 11 x 9 (a2) canvas holds
        // only 99 pixels, too few to show 257 levels, so the same builder is
        // run on a larger canvas.
        {
            const QSize big(96, 64);
            const Stack stack = makeBlendStack(big, makeBase16(big), BlendMode::Screen, 1.0, true);
            const QImage out16 = trackmatte16::composeExport(stack.descs, stack.images16,
                                                             big, displayCtx);
            const QImage out8 = hdrcomposite::to8bit(out16)
                                    .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            std::set<quint16> levels16;
            std::set<int> levels8;
            for (int y = 0; y < big.height(); ++y) {
                const QRgba64* l16 = reinterpret_cast<const QRgba64*>(out16.constScanLine(y));
                const QRgb* l8 = reinterpret_cast<const QRgb*>(out8.constScanLine(y));
                for (int x = 0; x < big.width(); ++x) {
                    levels16.insert(l16[x].red());
                    levels8.insert(qRed(l8[x]));
                }
            }
            std::printf("[matte16-parity] G9 (b) (a2) fixture on %dx%d (a2 canvas has 99 px): "
                        "levels16=%zu levels8=%zu (expect > 256 / <= 256)\n",
                        big.width(), big.height(), levels16.size(), levels8.size());
            if (!(levels16.size() > 256 && levels8.size() <= 256)) why += " (b)";
        }
        // (b) extra leg: a matte-free gradient under a solid Screen layer.
        {
            const int width = 1200;
            const QSize wide(width, 1);
            QImage gradient(wide, QImage::Format_RGBA64_Premultiplied);
            QRgba64* line = reinterpret_cast<QRgba64*>(gradient.scanLine(0));
            for (int x = 0; x < width; ++x) {
                const quint16 v = static_cast<quint16>(20000 + x * 3);
                line[x] = qRgba64(v, v, v, static_cast<quint16>(kMax16));
            }
            Stack stack;
            appendLayer(stack, gradient, wide);
            appendLayer(stack, makeSolidPremul16(wide, premultiply16(lift8(100), lift8(160)),
                                                 premultiply16(lift8(100), lift8(160)),
                                                 premultiply16(lift8(100), lift8(160)), lift8(160)),
                        wide, gpucomposite::MatteType::None, -1, true, 1.0, BlendMode::Screen);
            const QImage out16 = trackmatte16::composeExport(stack.descs, stack.images16,
                                                             wide, displayCtx);
            const QImage out8 = hdrcomposite::to8bit(out16)
                                    .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            const QRgba64* l16 = reinterpret_cast<const QRgba64*>(out16.constScanLine(0));
            const QRgb* l8 = reinterpret_cast<const QRgb*>(out8.constScanLine(0));
            std::set<quint16> levels16;
            std::set<int> levels8;
            for (int x = 0; x < width; ++x) {
                levels16.insert(l16[x].red());
                levels8.insert(qRed(l8[x]));
            }
            std::printf("[matte16-parity] G9 (b) extra gradient leg levels16=%zu levels8=%zu (expect > 256 / <= 256)\n",
                        levels16.size(), levels8.size());
            if (!(levels16.size() > 256 && levels8.size() <= 256)) why += " (b)";
        }

        // (c) no blended layer: bytes unchanged from HEAD.
        {
            const Stack stack = makeParityStack(
                canvas, gpucomposite::MatteType::Alpha, makeAlphaMatte16(canvas));
            const QString sha = rawSha(trackmatte16::composeExport(
                stack.descs, stack.images16, canvas, displayCtx));
            const QString kHeadSha = QStringLiteral("2ba08a2a2f1855c0");
            std::printf("[matte16-parity] G9 (c) sha=%s (HEAD %s)\n",
                        sha.toStdString().c_str(), kHeadSha.toStdString().c_str());
            if (sha != kHeadSha) why += " (c)";
        }

        // (d) the export: hdrmatte16 on, ODT off, matte + VFX Screen.
        {
            const QImage hdrScreen = renderMatteBlendFrame(BlendMode::Screen, true, false);
            const QImage hdrNormal = renderMatteBlendFrame(BlendMode::Normal, true, false);
            const QImage plainScreen = renderMatteBlendFrame(BlendMode::Screen, false, false);
            const QImage plainNormal = renderMatteBlendFrame(BlendMode::Normal, false, false);
            const qint64 dI = diffPixelCount(hdrScreen, hdrNormal, 40);
            const int dII = maxPerChannelDelta(hdrScreen, plainScreen);
            const int dIIbase = maxPerChannelDelta(hdrNormal, plainNormal);
            std::printf("[matte16-parity] G9 (d) (i) pixels > 40/255 vs Normal drawing=%lld (expect >= 100) "
                        "(ii) vs hdrmatte16-off=%d/255 (allow 6, same allowance as G1-G4) blend-less %d/255\n",
                        static_cast<long long>(dI), dII, dIIbase);
            if (!(dI >= 100)) why += " (d)(i)";
            if (!(dII <= 6 && dIIbase <= 6)) why += " (d)(ii)";

            // (e) ODT on: the blend is only pinned to take effect.
            const QImage odtScreen = renderMatteBlendFrame(BlendMode::Screen, true, true);
            const QImage odtNormal = renderMatteBlendFrame(BlendMode::Normal, true, true);
            const qint64 dE = diffPixelCount(odtScreen, odtNormal, 8);
            std::printf("[matte16-parity] G9 (e) ODT on: pixels > 8/255 vs Normal drawing=%lld (expect >= 100); "
                        "linear blend vs 8-bit screen delta (declared, not judged)=%d/255\n",
                        static_cast<long long>(dE), maxPerChannelDelta(odtScreen, plainScreen));
            if (!(dE >= 100)) why += " (e)";
        }

        check(9, "effective blend mode: matte then blend at 16 bit in the 8-bit kernel's order, "
                 "fixture E vs trackmatte::composite, 16-bit equations, >256 levels, no-blend bytes pinned, export leg",
              why.empty());
        if (!why.empty())
            std::printf("[matte16-parity] G9 failing legs:%s\n", why.c_str());
    }

    std::printf("[matte16-parity] summary: gates=9 passed=%d failed=%d\n",
                passed, failed);
    selftests::reportTally(passed, failed, 0);
    return failed == 0 ? 0 : 1;
}
