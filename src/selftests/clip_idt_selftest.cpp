// clip_idt_selftest.cpp
// Headless selftest for Stage5 per-clip input color transform into a common
// 16-bit compositing space.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <QImage>
#include <QRgba64>
#include <QSize>
#include <QtGlobal>

#include "../AcesColor.h"
#include "../color/ClipColorTransform.h"

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

QRgba64 rawPixel(QImage img, int x = 0, int y = 0)
{
    img = img.convertToFormat(QImage::Format_RGBA64_Premultiplied);
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

Rgb16 referenceStraightRgb(aces::ColorSpace inputSpace,
                           aces::ColorSpace outputSpace,
                           quint16 r,
                           quint16 g,
                           quint16 b)
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
    const aces::Mat3 matrix = aces::conversionMatrix(inputSpace, outputSpace);
    const aces::Vec3 linearOut = aces::apply(matrix, linearIn);
    return Rgb16{
        clampTo16(aces::oetf(outputSpace, linearOut[0])),
        clampTo16(aces::oetf(outputSpace, linearOut[1])),
        clampTo16(aces::oetf(outputSpace, linearOut[2]))
    };
}

QImage convertNoCache(const QImage& src,
                      const clipcolor::ColorMeta& in,
                      aces::ColorSpace outputSpace)
{
    const aces::ColorSpace inputSpace = clipcolor::acesSpaceFor(in);
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
            const Rgb16 straight = referenceStraightRgb(
                inputSpace, outputSpace,
                unpremultiply16(px.red(), a),
                unpremultiply16(px.green(), a),
                unpremultiply16(px.blue(), a));
            line[x] = qRgba64(premultiply16(straight.r, a),
                              premultiply16(straight.g, a),
                              premultiply16(straight.b, a),
                              a);
        }
    }
    return out;
}

bool pixelMatchesReference(const QImage& img,
                           const Rgb16& straight,
                           quint16 alpha,
                           int tolerance)
{
    const QRgba64 got = rawPixel(img);
    const int dr = std::abs(int(got.red()) - int(premultiply16(straight.r, alpha)));
    const int dg = std::abs(int(got.green()) - int(premultiply16(straight.g, alpha)));
    const int db = std::abs(int(got.blue()) - int(premultiply16(straight.b, alpha)));
    const int da = std::abs(int(got.alpha()) - int(alpha));
    return dr <= tolerance && dg <= tolerance && db <= tolerance && da == 0;
}

} // namespace

int runClipIdtSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[clip-idt] %s G%d %s\n",
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
        check(1, "same-space opaque Rec709->sRGB short-circuit has MSE 0",
              diff.mse == 0.0 && diff.maxAbs == 0);
    }

    {
        const QImage in = makeSolidPremul(QSize(1, 1), 28000, 41000, 19000, kMax16);
        const QImage to2020 = clipcolor::toUnifiedSpace(
            in, rec709, aces::ColorSpace::Rec2020);
        const QImage back = clipcolor::toUnifiedSpace(
            to2020, rec2020, aces::ColorSpace::sRGB);
        const DiffStats diff = diffImages(in, back);
        check(2, "Rec709->Rec2020->Rec709 round-trip stays within tiny 16-bit error",
              diff.maxAbs <= 4 && diff.mse <= 4.0);
    }

    {
        const QImage in = makeSolidPremul(QSize(1, 1), kMax16, 0, 0, kMax16);
        const QImage out = clipcolor::toUnifiedSpace(in, rec709, aces::ColorSpace::Rec2020);
        const Rgb16 want = referenceStraightRgb(
            aces::ColorSpace::sRGB, aces::ColorSpace::Rec2020, kMax16, 0, 0);
        check(3, "Rec709 primary red to Rec2020 matches matrix+OETF reference",
              pixelMatchesReference(out, want, kMax16, 1));
    }

    {
        const QImage base = makeSolidPremul(QSize(1, 1), 18000, 22000, 26000, kMax16);
        const QImage overlay = makeSolidPremul(QSize(1, 1), 52000, 18000, 9000, kMax16);
        const QImage baseOut = clipcolor::toUnifiedSpace(base, rec709, aces::ColorSpace::sRGB);
        const QImage overlayOut = clipcolor::toUnifiedSpace(overlay, rec2020,
                                                            aces::ColorSpace::sRGB);
        const Rgb16 want = referenceStraightRgb(
            aces::ColorSpace::Rec2020, aces::ColorSpace::sRGB, 52000, 18000, 9000);
        const DiffStats baseDiff = diffImages(base, baseOut);
        check(4, "Rec2020 overlay to sRGB matches hand-computed direction",
              baseDiff.mse == 0.0
              && pixelMatchesReference(overlayOut, want, kMax16, 1));
    }

    {
        const clipcolor::ColorMeta pq = meta(clipcolor::Primaries::Rec2020,
                                             clipcolor::Transfer::PQ, true);
        const QImage in = makeSolidPremul(QSize(2, 2), 45000, 32000, 17000, kMax16);
        const QImage out = clipcolor::toUnifiedSpace(in, pq, aces::ColorSpace::sRGB);
        const DiffStats diff = diffImages(in, out);
        check(5, "PQ transfer returns unchanged instead of sRGB mis-linearization",
              diff.mse == 0.0 && diff.maxAbs == 0);
    }

    {
        const clipcolor::ColorMeta hlg = meta(clipcolor::Primaries::Rec2020,
                                              clipcolor::Transfer::HLG, true);
        const QImage in = makeSolidPremul(QSize(2, 2), 15000, 42000, 52000, kMax16);
        const QImage out = clipcolor::toUnifiedSpace(in, hlg, aces::ColorSpace::sRGB);
        const DiffStats diff = diffImages(in, out);
        check(6, "HLG transfer returns unchanged instead of sRGB mis-linearization",
              diff.mse == 0.0 && diff.maxAbs == 0);
    }

    {
        const quint16 alpha = 32768;
        const QImage in = makeSolidPremul(QSize(1, 1), 39000, 23000, 11000, alpha);
        const QImage out = clipcolor::toUnifiedSpace(in, rec2020, aces::ColorSpace::sRGB);
        const QImage want = convertNoCache(in, rec2020, aces::ColorSpace::sRGB);
        const DiffStats diff = diffImages(out, want);
        const QRgba64 got = rawPixel(out);
        check(7, "50%-alpha premul layer matches straight unpremul/repremul reference",
              diff.maxAbs <= 1
              && got.alpha() == alpha
              && got.red() <= alpha
              && got.green() <= alpha
              && got.blue() <= alpha);
    }

    {
        QImage in(QSize(4, 4), QImage::Format_RGBA64_Premultiplied);
        for (int y = 0; y < in.height(); ++y) {
            QRgba64* line = reinterpret_cast<QRgba64*>(in.scanLine(y));
            for (int x = 0; x < in.width(); ++x) {
                const quint16 delta = static_cast<quint16>(y * in.width() + x);
                line[x] = qRgba64(32000 + delta,
                                  24000 + delta,
                                  16000 + delta,
                                  kMax16);
            }
        }
        const QImage cached = clipcolor::toUnifiedSpace(in, rec709, aces::ColorSpace::Rec2020);
        const QImage brute = convertNoCache(in, rec709, aces::ColorSpace::Rec2020);
        const DiffStats diff = diffImages(cached, brute);
        check(8, "quantized color cache stays within 12-bit-key error bound",
              diff.maxAbs <= 24 && diff.mse <= 64.0);
    }

    {
        const QImage in = makeSolidPremul(QSize(1, 1), kMax16, 0, 0, kMax16);
        const QImage out = clipcolor::toUnifiedSpace(in, rec2020, aces::ColorSpace::sRGB);
        const QRgba64 got = rawPixel(out);
        check(9, "over-1.0 Rec2020 red into sRGB clamps to 65535 and does not wrap",
              got.red() == kMax16 && got.green() <= 1024 && got.blue() <= 1024);
    }

    std::printf("[clip-idt] summary: gates=9 passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
