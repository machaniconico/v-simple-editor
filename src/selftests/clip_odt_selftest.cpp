// clip_odt_selftest.cpp
// Headless selftest for Stage6 per-clip linear working space and ODT engine.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <QImage>
#include <QRgba64>
#include <QSize>
#include <QtGlobal>

#include "../AcesColor.h"
#include "../color/ClipColorTransform.h"
#include "../color/ClipOdt.h"

namespace {

constexpr quint16 kMax16 = 65535;

struct DiffStats {
    double mse = 1e18;
    int maxAbs = 65535;
};

struct Rgb16 {
    quint16 r = 0;
    quint16 g = 0;
    quint16 b = 0;
};

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

QImage makeSolidPremul(QSize size, quint16 r, quint16 g, quint16 b, quint16 a)
{
    QImage img(size, QImage::Format_RGBA64_Premultiplied);
    for (int y = 0; y < img.height(); ++y) {
        QRgba64* line = reinterpret_cast<QRgba64*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            line[x] = qRgba64(premultiply16(r, a),
                              premultiply16(g, a),
                              premultiply16(b, a),
                              a);
        }
    }
    return img;
}

QImage makeLinearPremul(QSize size, double r, double g, double b, quint16 a)
{
    return makeSolidPremul(size, clampTo16(r), clampTo16(g), clampTo16(b), a);
}

QRgba64 rawPixel(const QImage& src, int x = 0, int y = 0)
{
    const QImage img = src.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    return reinterpret_cast<const QRgba64*>(img.constScanLine(y))[x];
}

DiffStats diffImages(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return DiffStats{};

    const QImage aa = a.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA64_Premultiplied);

    double sumSq = 0.0;
    int maxAbs = 0;
    int count = 0;
    for (int y = 0; y < aa.height(); ++y) {
        const QRgba64* la = reinterpret_cast<const QRgba64*>(aa.constScanLine(y));
        const QRgba64* lb = reinterpret_cast<const QRgba64*>(bb.constScanLine(y));
        for (int x = 0; x < aa.width(); ++x) {
            const int da[4] = {
                std::abs(int(la[x].red()) - int(lb[x].red())),
                std::abs(int(la[x].green()) - int(lb[x].green())),
                std::abs(int(la[x].blue()) - int(lb[x].blue())),
                std::abs(int(la[x].alpha()) - int(lb[x].alpha()))
            };
            for (int d : da) {
                sumSq += double(d) * double(d);
                if (d > maxAbs)
                    maxAbs = d;
                ++count;
            }
        }
    }
    return DiffStats{count > 0 ? sumSq / double(count) : 1e18, maxAbs};
}

Rgb16 referenceLinearWorking(quint16 r,
                             quint16 g,
                             quint16 b,
                             aces::ColorSpace inputSpace)
{
    const aces::Vec3 encodedIn = {
        double(r) / kMax16,
        double(g) / kMax16,
        double(b) / kMax16
    };
    const aces::Vec3 linearIn = {
        aces::eotf(inputSpace, encodedIn[0]),
        aces::eotf(inputSpace, encodedIn[1]),
        aces::eotf(inputSpace, encodedIn[2])
    };
    const aces::Vec3 linearRec2020 = aces::apply(
        aces::conversionMatrix(inputSpace, aces::ColorSpace::Rec2020),
        linearIn);
    return Rgb16{clampTo16(linearRec2020[0]),
                 clampTo16(linearRec2020[1]),
                 clampTo16(linearRec2020[2])};
}

Rgb16 referenceRrtOdtFromRec2020(const aces::Vec3& linearRec2020,
                                 aces::ColorSpace outputSpace)
{
    const aces::Vec3 acescg = aces::apply(
        aces::conversionMatrix(aces::ColorSpace::Rec2020,
                               aces::ColorSpace::ACEScg),
        linearRec2020);
    const aces::Vec3 encoded = aces::rrtOdt(acescg, outputSpace);
    return Rgb16{clampTo16(encoded[0]),
                 clampTo16(encoded[1]),
                 clampTo16(encoded[2])};
}

Rgb16 referenceOdtStraight(const QRgba64& src, const clipodt::OdtParams& p)
{
    const quint16 a = src.alpha();
    const aces::Vec3 linearRec2020 = {
        double(unpremultiply16(src.red(), a)) / kMax16,
        double(unpremultiply16(src.green(), a)) / kMax16,
        double(unpremultiply16(src.blue(), a)) / kMax16
    };

    aces::Vec3 outputLinear{};
    if (p.tonemap) {
        const aces::Vec3 acescg = aces::apply(
            aces::conversionMatrix(aces::ColorSpace::Rec2020,
                                   aces::ColorSpace::ACEScg),
            linearRec2020);
        const aces::Vec3 tonemapped = {
            aces::acesFilmicTonemap(acescg[0]),
            aces::acesFilmicTonemap(acescg[1]),
            aces::acesFilmicTonemap(acescg[2])
        };
        outputLinear = aces::apply(
            aces::conversionMatrix(aces::ColorSpace::ACEScg, p.outputSpace),
            tonemapped);
    } else {
        outputLinear = aces::apply(
            aces::conversionMatrix(aces::ColorSpace::Rec2020, p.outputSpace),
            linearRec2020);
    }

    return Rgb16{clampTo16(aces::oetf(p.outputSpace, outputLinear[0])),
                 clampTo16(aces::oetf(p.outputSpace, outputLinear[1])),
                 clampTo16(aces::oetf(p.outputSpace, outputLinear[2]))};
}

bool pixelMatchesStraight(const QImage& img,
                          const Rgb16& straight,
                          quint16 alpha,
                          int tolerance)
{
    const QRgba64 got = rawPixel(img);
    return std::abs(int(got.red()) - int(premultiply16(straight.r, alpha))) <= tolerance
        && std::abs(int(got.green()) - int(premultiply16(straight.g, alpha))) <= tolerance
        && std::abs(int(got.blue()) - int(premultiply16(straight.b, alpha))) <= tolerance
        && got.alpha() == alpha;
}

int maxRgb(const QRgba64& px)
{
    return std::max(int(px.red()), std::max(int(px.green()), int(px.blue())));
}

} // namespace

int runClipOdtSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[clip-odt] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    const clipcolor::ColorMeta rec709 = meta(clipcolor::Primaries::Rec709);
    const clipcolor::ColorMeta rec2020 = meta(clipcolor::Primaries::Rec2020);

    {
        const QImage in = makeSolidPremul(QSize(3, 2), 12345, 23456, 34567, kMax16);
        const QImage out = clipcolor::toUnifiedSpace(
            in, rec709, clipcolor::acesSpaceFor(rec709));
        const DiffStats diff = diffImages(in, out);
        check(1, "OFF path same-space toUnifiedSpace identity remains MSE 0",
              diff.mse == 0.0 && diff.maxAbs == 0);
    }

    {
        const quint16 r = 30000;
        const quint16 g = 42000;
        const quint16 b = 11000;
        const QImage in = makeSolidPremul(QSize(1, 1), r, g, b, kMax16);
        const QImage out = clipcolor::toLinearWorking(in, rec709);
        const Rgb16 want = referenceLinearWorking(r, g, b, aces::ColorSpace::sRGB);
        const DiffStats encodedDiff = diffImages(in, out);
        check(2, "toLinearWorking Rec709 patch matches eotf+Rec2020 matrix and is distinct",
              out.format() == QImage::Format_RGBA64_Premultiplied
              && pixelMatchesStraight(out, want, kMax16, 1)
              && encodedDiff.maxAbs > 1024);
    }

    {
        const aces::Vec3 patch = {0.18, 0.35, 0.05};
        const QImage in = makeLinearPremul(QSize(1, 1),
                                           patch[0], patch[1], patch[2], kMax16);
        const clipodt::OdtParams p{aces::ColorSpace::Rec709, true};
        const QImage out = clipodt::applyOdt16(in, p);
        const QRgba64 src = rawPixel(in);
        const aces::Vec3 storedPatch = {
            double(src.red()) / kMax16,
            double(src.green()) / kMax16,
            double(src.blue()) / kMax16
        };
        const Rgb16 want = referenceRrtOdtFromRec2020(storedPatch, p.outputSpace);
        check(3, "applyOdt16 tonemap equals rrtOdt(Rec2020->ACEScg patch)",
              out.format() == QImage::Format_RGBA64_Premultiplied
              && pixelMatchesStraight(out, want, kMax16, 1));
    }

    {
        QImage ramp(QSize(4, 1), QImage::Format_RGBA64_Premultiplied);
        const double levels[4] = {0.04, 0.18, 0.50, 1.00};
        QRgba64* line = reinterpret_cast<QRgba64*>(ramp.scanLine(0));
        for (int x = 0; x < 4; ++x) {
            const quint16 v = clampTo16(levels[x]);
            line[x] = qRgba64(v, v, v, kMax16);
        }

        const clipodt::OdtParams tmP{aces::ColorSpace::Rec709, true};
        const QImage tmRamp = clipodt::applyOdt16(ramp, tmP);
        const QRgba64* tmLine = reinterpret_cast<const QRgba64*>(tmRamp.constScanLine(0));
        bool monotonic = true;
        for (int x = 1; x < 4; ++x) {
            monotonic = monotonic
                && tmLine[x - 1].red() <= tmLine[x].red()
                && tmLine[x - 1].green() <= tmLine[x].green()
                && tmLine[x - 1].blue() <= tmLine[x].blue();
        }

        // Highlight compression at the representable ceiling. A literal >1.0
        // ACEScg value is unreachable through a 16-bit integer working buffer
        // (input clamps at 1.0, and Rec2020 sits inside AP1 so no in-range
        // Rec2020 code maps to an ACEScg channel > 1.0). So we exercise the
        // brightest representable linear value (1.0 neutral) with outputSpace =
        // Rec2020 (working == output, so the no-tonemap closer is an identity
        // matrix + oetf, avoiding white-point drift): the closer must reach the
        // ceiling (oetf(1.0) -> 65535) while the filmic tonemap compresses the
        // same input BELOW the ceiling (acesFilmic(1.0) ~= 0.80 < 1.0).
        const QImage ceiling = makeLinearPremul(QSize(1, 1), 1.0, 1.0, 1.0, kMax16);
        const clipodt::OdtParams rec2020Tm{aces::ColorSpace::Rec2020, true};
        const clipodt::OdtParams rec2020NoTm{aces::ColorSpace::Rec2020, false};
        const QImage withTm = clipodt::applyOdt16(ceiling, rec2020Tm);
        const QImage noTm = clipodt::applyOdt16(ceiling, rec2020NoTm);
        check(4, "tonemap is monotonic and compresses the ceiling highlight below clamp",
              monotonic
              && maxRgb(rawPixel(noTm)) == int(kMax16)
              && maxRgb(rawPixel(withTm)) < int(kMax16));
    }

    {
        const quint16 r = 18000;
        const quint16 g = 39000;
        const quint16 b = 52000;
        const QImage encoded = makeSolidPremul(QSize(1, 1), r, g, b, kMax16);
        const QImage linear = clipcolor::toLinearWorking(encoded, rec2020);
        const clipodt::OdtParams p{aces::ColorSpace::Rec2020, false};
        const QImage closed = clipodt::applyOdt16(linear, p);
        const DiffStats diff = diffImages(encoded, closed);
        check(5, "tonemap=false Rec2020 closer returns near original encoded patch",
              diff.maxAbs <= 1 && diff.mse <= 1.0);
    }

    {
        const QImage in = makeLinearPremul(QSize(1, 1), 0.24, 0.10, 0.04, kMax16);
        const clipodt::OdtParams p{aces::ColorSpace::Rec709, true};
        const QImage out = clipodt::applyOdt16(in, p);
        const QRgba64 got = rawPixel(out);
        const quint16 reEncodedRed = clampTo16(
            aces::oetf(p.outputSpace, double(got.red()) / kMax16));
        check(6, "applyOdt16 output is already encoded; a second oetf differs",
              std::abs(int(reEncodedRed) - int(got.red())) > 1024);
    }

    {
        const quint16 alpha = 32768;
        const QImage in = makeLinearPremul(QSize(1, 1), 0.42, 0.18, 0.06, alpha);
        const QRgba64 src = rawPixel(in);
        const clipodt::OdtParams p{aces::ColorSpace::Rec709, true};
        const QImage out = clipodt::applyOdt16(in, p);
        const Rgb16 want = referenceOdtStraight(src, p);
        const QRgba64 got = rawPixel(out);
        check(7, "50%-alpha ODT matches manual unpremul/ODT/repremul reference",
              pixelMatchesStraight(out, want, alpha, 1)
              && got.alpha() == alpha
              && got.red() <= alpha
              && got.green() <= alpha
              && got.blue() <= alpha);
    }

    {
        const clipcolor::ColorMeta pq = meta(clipcolor::Primaries::Rec2020,
                                             clipcolor::Transfer::PQ, true);
        const clipcolor::ColorMeta hlg = meta(clipcolor::Primaries::Rec2020,
                                              clipcolor::Transfer::HLG, true);
        const quint16 r = 21000;
        const quint16 g = 33000;
        const quint16 b = 47000;
        const QImage encoded = makeSolidPremul(QSize(1, 1), r, g, b, kMax16);
        const QImage pqLinear = clipcolor::toLinearWorking(encoded, pq);
        const QImage hlgLinear = clipcolor::toLinearWorking(encoded, hlg);
        const clipodt::OdtParams p{aces::ColorSpace::Rec2020, false};
        const QImage pqOut = clipodt::applyOdt16(pqLinear, p);
        const QImage hlgOut = clipodt::applyOdt16(hlgLinear, p);
        const Rgb16 sdrRef{
            clampTo16(aces::oetf(aces::ColorSpace::Rec2020, double(r) / kMax16)),
            clampTo16(aces::oetf(aces::ColorSpace::Rec2020, double(g) / kMax16)),
            clampTo16(aces::oetf(aces::ColorSpace::Rec2020, double(b) / kMax16))
        };
        const DiffStats pqNoop = diffImages(encoded, pqLinear);
        const DiffStats hlgNoop = diffImages(encoded, hlgLinear);
        check(10, "PQ/HLG Stage6 gap feeds unchanged codes into SDR Rec2020 oetf",
              pqNoop.maxAbs == 0
              && hlgNoop.maxAbs == 0
              && pixelMatchesStraight(pqOut, sdrRef, kMax16, 1)
              && pixelMatchesStraight(hlgOut, sdrRef, kMax16, 1));
    }

    std::printf("[clip-odt] summary: gates=8 passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
