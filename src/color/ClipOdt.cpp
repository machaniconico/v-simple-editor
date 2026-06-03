#include "ClipOdt.h"

#include <QRgba64>
#include <QtGlobal>

#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace clipodt {
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
                           const aces::Mat3& rec2020ToAcescg,
                           const aces::Mat3& acescgToOutput,
                           const aces::Mat3& rec2020ToOutput,
                           const OdtParams& p)
{
    const aces::Vec3 linearRec2020 = {
        static_cast<double>(r) / kMax16,
        static_cast<double>(g) / kMax16,
        static_cast<double>(b) / kMax16
    };

    aces::Vec3 outputLinear{};
    if (p.tonemap) {
        const aces::Vec3 acescg = aces::apply(rec2020ToAcescg, linearRec2020);
        const aces::Vec3 tonemapped = {
            aces::acesFilmicTonemap(acescg[0]),
            aces::acesFilmicTonemap(acescg[1]),
            aces::acesFilmicTonemap(acescg[2])
        };
        outputLinear = aces::apply(acescgToOutput, tonemapped);
    } else {
        outputLinear = aces::apply(rec2020ToOutput, linearRec2020);
    }

    return packRgb16(clampTo16(aces::oetf(p.outputSpace, outputLinear[0])),
                     clampTo16(aces::oetf(p.outputSpace, outputLinear[1])),
                     clampTo16(aces::oetf(p.outputSpace, outputLinear[2])));
}

} // namespace

QImage applyOdt16(const QImage& linearWorkingRgba64Premul, const OdtParams& p)
{
    if (linearWorkingRgba64Premul.isNull()
        || linearWorkingRgba64Premul.width() <= 0
        || linearWorkingRgba64Premul.height() <= 0) {
        return linearWorkingRgba64Premul;
    }

    QImage out = linearWorkingRgba64Premul.convertToFormat(
        QImage::Format_RGBA64_Premultiplied);

    const aces::Mat3 rec2020ToAcescg = aces::conversionMatrix(
        aces::ColorSpace::Rec2020, aces::ColorSpace::ACEScg);
    const aces::Mat3 acescgToOutput = aces::conversionMatrix(
        aces::ColorSpace::ACEScg, p.outputSpace);
    const aces::Mat3 rec2020ToOutput = aces::conversionMatrix(
        aces::ColorSpace::Rec2020, p.outputSpace);

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
                const quint64 packed = convertStraightRgb(
                    straightR, straightG, straightB,
                    rec2020ToAcescg, acescgToOutput, rec2020ToOutput, p);
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

bool enabledFromEnv()
{
    const char* value = std::getenv("VEDITOR_HDR_ODT");
    return value != nullptr
        && value[0] != '\0'
        && !(value[0] == '0' && value[1] == '\0');
}

} // namespace clipodt
