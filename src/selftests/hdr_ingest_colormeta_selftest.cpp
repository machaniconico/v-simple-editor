// hdr_ingest_colormeta_selftest.cpp
// Headless selftest for Stage10 ingest-time ColorMeta derivation.
// Run via: --selftest=hdr-ingest-colormeta

#include <QDebug>
#include <QJsonObject>
#include <QString>

#include "../color/ClipColor.h"
#include "../playback/hdringest_flag.h"
#include "../playback/PixFmtDepth.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace {

bool isMeta(const clipcolor::ColorMeta& meta,
            clipcolor::Primaries primaries,
            clipcolor::Transfer transfer,
            int bitDepth,
            bool isHdr)
{
    return meta.primaries == primaries
        && meta.transfer == transfer
        && meta.bitDepth == bitDepth
        && meta.isHdr == isHdr;
}

} // namespace

int runHdrIngestColorMetaSelftest()
{
    qInfo().noquote() << "[hdr-ingest-colormeta] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[hdr-ingest-colormeta] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[hdr-ingest-colormeta] FAIL" << name << ":" << msg;
    };
    auto check = [&](const char* name, bool ok, const QString& msg = QString()) {
        if (ok)
            pass(name);
        else
            fail(name, msg);
    };

    constexpr int kAvcolPriBt709 = 1;       // AVCOL_PRI_BT709 = 1
    constexpr int kAvcolPriUnspecified = 2; // AVCOL_PRI_UNSPECIFIED = 2
    constexpr int kAvcolPriBt2020 = 9;      // AVCOL_PRI_BT2020 = 9
    constexpr int kAvcolPriSmpte432 = 12;   // AVCOL_PRI_SMPTE432 = 12 (Display P3)

    constexpr int kAvcolTrcBt709 = 1;         // AVCOL_TRC_BT709 = 1
    constexpr int kAvcolTrcUnspecified = 2;   // AVCOL_TRC_UNSPECIFIED = 2
    constexpr int kAvcolTrcIec61966_2_1 = 13; // AVCOL_TRC_IEC61966_2_1 = 13 (sRGB)
    constexpr int kAvcolTrcSmpte2084 = 16;    // AVCOL_TRC_SMPTE2084 = 16 (PQ)
    constexpr int kAvcolTrcAribStdB67 = 18;   // AVCOL_TRC_ARIB_STD_B67 = 18 (HLG)

    check("G1 PQ+BT2020+10bit -> Rec2020/PQ/10/HDR",
          isMeta(clipcolor::fromCodecParams(kAvcolPriBt2020, kAvcolTrcSmpte2084, 10),
                 clipcolor::Primaries::Rec2020, clipcolor::Transfer::PQ, 10, true));

    check("G2 HLG+BT2020+10bit -> Rec2020/HLG/10/HDR",
          isMeta(clipcolor::fromCodecParams(kAvcolPriBt2020, kAvcolTrcAribStdB67, 10),
                 clipcolor::Primaries::Rec2020, clipcolor::Transfer::HLG, 10, true));

    check("G3 BT709+BT709+8bit equals defaultSdr",
          clipcolor::fromCodecParams(kAvcolPriBt709, kAvcolTrcBt709, 8)
              == clipcolor::defaultSdr());

    check("G4 Display P3+sRGB remains SDR",
          isMeta(clipcolor::fromCodecParams(kAvcolPriSmpte432, kAvcolTrcIec61966_2_1, 10),
                 clipcolor::Primaries::DisplayP3, clipcolor::Transfer::sRGB, 10, false));

    check("G5 unspecified and 0 values fall back to SDR defaults",
          clipcolor::fromCodecParams(kAvcolPriUnspecified, kAvcolTrcUnspecified, 8)
              == clipcolor::defaultSdr()
          && clipcolor::fromCodecParams(0, 0, 8) == clipcolor::defaultSdr());

    check("G6 PQ with unspecified primaries still detects HDR by transfer",
          isMeta(clipcolor::fromCodecParams(kAvcolPriUnspecified, kAvcolTrcSmpte2084, 10),
                 clipcolor::Primaries::Rec709, clipcolor::Transfer::PQ, 10, true)
          && isMeta(clipcolor::fromCodecParams(0, kAvcolTrcSmpte2084, 10),
                    clipcolor::Primaries::Rec709, clipcolor::Transfer::PQ, 10, true));

    check("G7 bitDepth passthrough/floor and 10-bit non-HDR stays SDR",
          clipcolor::fromCodecParams(kAvcolPriBt2020, kAvcolTrcSmpte2084, 12).bitDepth == 12
          && clipcolor::fromCodecParams(kAvcolPriBt2020, kAvcolTrcSmpte2084, 0).bitDepth == 8
          && isMeta(clipcolor::fromCodecParams(kAvcolPriBt2020, kAvcolTrcBt709, 10),
                    clipcolor::Primaries::Rec2020, clipcolor::Transfer::sRGB, 10, false));

    check("G8 ingest and trace flag helpers accept only 1",
          hdringest::enabledFromEnvValue(QStringLiteral("1"))
          && !hdringest::enabledFromEnvValue(QString())
          && !hdringest::enabledFromEnvValue(QStringLiteral("0"))
          && hdringest::traceEnabledFromEnvValue(QStringLiteral("1"))
          && !hdringest::traceEnabledFromEnvValue(QString())
          && !hdringest::traceEnabledFromEnvValue(QStringLiteral("0")));

    {
        const clipcolor::ColorMeta hdr =
            clipcolor::fromCodecParams(kAvcolPriBt2020, kAvcolTrcSmpte2084, 10);
        const clipcolor::ColorMeta p3 =
            clipcolor::fromCodecParams(kAvcolPriSmpte432, kAvcolTrcIec61966_2_1, 10);
        check("G9 fromCodecParams toJson/fromJson round-trip is stable",
              clipcolor::fromJson(clipcolor::toJson(hdr)) == hdr
              && clipcolor::fromJson(clipcolor::toJson(p3)) == p3);
    }

    // G10 exercises the LOAD-BEARING codecpar bit-depth extraction that the
    // ingest probe (Timeline::addClip) actually runs. The prior derivation
    // (av_get_bits_per_pixel(desc)/3) collapsed 10/12-bit 4:2:0 HDR to 8-bit
    // because that API returns the subsampling-weighted AVERAGE bpp, not the
    // per-component depth. comp[0].depth reads the true depth. G1-G9 never
    // touched this path (they pass bitDepth as a literal), so the bug escaped.
    check("G10 bitDepthFromPixFmt reads per-component depth (subsampled 4:2:0/4:2:2 -> 10/12, not 8)",
          pixfmtdepth::bitDepthFromPixFmt(AV_PIX_FMT_YUV420P10LE) == 10
          && pixfmtdepth::bitDepthFromPixFmt(AV_PIX_FMT_P010LE) == 10
          && pixfmtdepth::bitDepthFromPixFmt(AV_PIX_FMT_YUV422P10LE) == 10
          && pixfmtdepth::bitDepthFromPixFmt(AV_PIX_FMT_YUV444P10LE) == 10
          && pixfmtdepth::bitDepthFromPixFmt(AV_PIX_FMT_YUV420P12LE) == 12
          && pixfmtdepth::bitDepthFromPixFmt(AV_PIX_FMT_YUV420P) == 8
          && pixfmtdepth::bitDepthFromPixFmt(AV_PIX_FMT_NONE) == 8);

    qInfo().noquote() << "[hdr-ingest-colormeta] selftest done: passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
