#include "ClipColorTransform.h"

#include <QRgba64>
#include <QtGlobal>

#include <cmath>
#include <unordered_map>

namespace clipcolor {
namespace {

constexpr quint16 kMax16 = 65535;
constexpr int kCacheReserve = 4096;

quint64 cacheKey(quint16 r, quint16 g, quint16 b)
{
    const quint64 qr = static_cast<quint64>(r >> 4);
    const quint64 qg = static_cast<quint64>(g >> 4);
    const quint64 qb = static_cast<quint64>(b >> 4);
    return (qr << 24) | (qg << 12) | qb;
}

quint64 packRgb16(quint16 r, quint16 g, quint16 b)
{
    return (static_cast<quint64>(r) << 32)
         | (static_cast<quint64>(g) << 16)
         | static_cast<quint64>(b);
}

void unpackRgb16(quint64 packed, quint16& r, quint16& g, quint16& b)
{
    r = static_cast<quint16>((packed >> 32) & 0xffffu);
    g = static_cast<quint16>((packed >> 16) & 0xffffu);
    b = static_cast<quint16>(packed & 0xffffu);
}

quint16 clampTo16(double normalized)
{
    if (!std::isfinite(normalized) || normalized <= 0.0)
        return 0;
    if (normalized >= 1.0)
        return kMax16;
    return static_cast<quint16>(std::llround(normalized * kMax16));
}

quint16 unpremultiply16(quint16 premul, quint16 alpha)
{
    if (alpha == 0)
        return 0;
    const quint64 straight =
        (static_cast<quint64>(premul) * kMax16 + alpha / 2) / alpha;
    return static_cast<quint16>(straight > kMax16 ? kMax16 : straight);
}

quint16 premultiply16(quint16 straight, quint16 alpha)
{
    const quint64 premul =
        (static_cast<quint64>(straight) * alpha + kMax16 / 2) / kMax16;
    return static_cast<quint16>(premul > kMax16 ? kMax16 : premul);
}

quint64 convertStraightRgb(quint16 r,
                           quint16 g,
                           quint16 b,
                           const aces::Mat3& matrix,
                           aces::ColorSpace inputSpace,
                           aces::ColorSpace outputSpace)
{
    const aces::Vec3 encodedIn = {
        static_cast<double>(r) / kMax16,
        static_cast<double>(g) / kMax16,
        static_cast<double>(b) / kMax16
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
    return packRgb16(outR, outG, outB);
}

} // namespace

aces::ColorSpace acesSpaceFor(const ColorMeta& meta)
{
    switch (meta.primaries) {
    case Primaries::Rec709:    return aces::ColorSpace::sRGB;
    case Primaries::Rec2020:   return aces::ColorSpace::Rec2020;
    case Primaries::DisplayP3: return aces::ColorSpace::DisplayP3;
    case Primaries::ACEScg:    return aces::ColorSpace::ACEScg;
    }
    return aces::ColorSpace::sRGB;
}

QImage toUnifiedSpace(const QImage& rgba64Premul,
                      const ColorMeta& in,
                      aces::ColorSpace outputSpace)
{
    if (rgba64Premul.isNull()
        || rgba64Premul.width() <= 0
        || rgba64Premul.height() <= 0) {
        return rgba64Premul;
    }

    const aces::ColorSpace inputSpace = acesSpaceFor(in);
    if (inputSpace == outputSpace)
        return rgba64Premul;

    // Stage5 limitation: ACES here only exposes sRGB-style/linear transfer
    // helpers. PQ/HLG clips must not be routed through aces::eotf(), because
    // that would silently decode them with the wrong transfer curve.
    if (in.transfer == Transfer::PQ || in.transfer == Transfer::HLG)
        return rgba64Premul;

    QImage out = rgba64Premul.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    const aces::Mat3 matrix = aces::conversionMatrix(inputSpace, outputSpace);

    std::unordered_map<quint64, quint64> cache;
    cache.reserve(kCacheReserve);

    for (int y = 0; y < out.height(); ++y) {
        QRgba64* line = reinterpret_cast<QRgba64*>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const QRgba64 px = line[x];
            const quint16 a = px.alpha();
            if (a == 0) {
                line[x] = qRgba64(0, 0, 0, 0);
                continue;
            }

            const quint16 straightR = unpremultiply16(px.red(), a);
            const quint16 straightG = unpremultiply16(px.green(), a);
            const quint16 straightB = unpremultiply16(px.blue(), a);
            const quint64 key = cacheKey(straightR, straightG, straightB);

            quint16 outR = 0;
            quint16 outG = 0;
            quint16 outB = 0;
            const auto it = cache.find(key);
            if (it != cache.end()) {
                unpackRgb16(it->second, outR, outG, outB);
            } else {
                const quint64 packed = convertStraightRgb(straightR, straightG, straightB,
                                                          matrix, inputSpace, outputSpace);
                unpackRgb16(packed, outR, outG, outB);
                if (cache.size() < kCacheReserve)
                    cache.emplace(key, packed);
            }

            line[x] = qRgba64(premultiply16(outR, a),
                              premultiply16(outG, a),
                              premultiply16(outB, a),
                              a);
        }
    }

    return out;
}

} // namespace clipcolor
