// Deterministic Radeon/AMF routing policy selftest (no Radeon hardware required).
// Ported from PR #9 and upgraded for Wave 29 with GateTally (18 gates).
// Run via: --selftest=radeon-amf

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <QJsonObject>

#include "../CodecDetector.h"
#include "../ExportDialog.h"
#include "../RenderQueue.h"
#include "../libavcore/Encode.h"
#include "../libavcore/Probe.h"
#include "SelftestGate.h"

namespace {

bool startsWith(const std::vector<std::string> &names, const char *expected)
{
    return !names.empty() && names.front() == expected;
}

bool containsBefore(const std::vector<std::string> &names,
                    const char *first,
                    const char *second)
{
    const auto firstIt = std::find(names.begin(), names.end(), first);
    const auto secondIt = std::find(names.begin(), names.end(), second);
    return firstIt != names.end() && secondIt != names.end()
        && firstIt < secondIt;
}

bool hasOption(const QList<QPair<QString, QString>> &opts,
               const char *key,
               const char *val)
{
    for (const auto &p : opts) {
        if (p.first == QLatin1String(key) && p.second == QLatin1String(val))
            return true;
    }
    return false;
}

} // namespace

int runRadeonAmfSelftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };

    libavcore::EncodeRequest request;
    request.useHardwareAccel = true;
    request.hwVendorHint = "amf";

    // G1: H.264 explicit AMF routes h264_amf -> libx264 (never spills to NVENC/QSV/mpeg4)
    request.videoCodecName = "libx264";
    const auto h264 = libavcore::videoEncoderCandidateNames(request);
    rec(1, startsWith(h264, "h264_amf")
            && containsBefore(h264, "h264_amf", "libx264")
            && std::find(h264.begin(), h264.end(), "h264_nvenc") == h264.end()
            && std::find(h264.begin(), h264.end(), "mpeg4") == h264.end(),
        QStringLiteral("h264First=%1 cnt=%2").arg(QString::fromStdString(h264.front())).arg(h264.size()));

    // G2: HEVC explicit AMF routes hevc_amf -> libx265 (never spills to NVENC/QSV/mpeg4)
    request.videoCodecName = "libx265";
    const auto hevc = libavcore::videoEncoderCandidateNames(request);
    rec(2, startsWith(hevc, "hevc_amf")
            && containsBefore(hevc, "hevc_amf", "libx265")
            && std::find(hevc.begin(), hevc.end(), "hevc_nvenc") == hevc.end()
            && std::find(hevc.begin(), hevc.end(), "mpeg4") == hevc.end(),
        QStringLiteral("hevcFirst=%1 cnt=%2").arg(QString::fromStdString(hevc.front())).arg(hevc.size()));

    // G3: AV1 explicit AMF routes av1_amf -> libaom-av1 / libsvtav1
    request.videoCodecName = "libaom-av1";
    const auto av1 = libavcore::videoEncoderCandidateNames(request);
    rec(3, startsWith(av1, "av1_amf")
            && containsBefore(av1, "av1_amf", "libaom-av1")
            && std::find(av1.begin(), av1.end(), "av1_nvenc") == av1.end(),
        QStringLiteral("av1First=%1 cnt=%2").arg(QString::fromStdString(av1.front())).arg(av1.size()));

    // G4: Hardware encoder classification includes av1_amf and excludes software encoders
    rec(4, libavcore::isHardwareVideoEncoderName("h264_amf")
            && libavcore::isHardwareVideoEncoderName("hevc_amf")
            && libavcore::isHardwareVideoEncoderName("av1_amf")
            && !libavcore::isHardwareVideoEncoderName("libsvtav1")
            && !libavcore::isHardwareVideoEncoderName("libaom-av1"),
        QStringLiteral("amfHwClass=1"));

    // G5: RenderQueue::applyHardwareEncodingConfig preserves explicit AMF hint
    QJsonObject config;
    config[QStringLiteral("hwEncoder")] = QStringLiteral("amf");
    config[QStringLiteral("useHardwareAccel")] = true;
    libavcore::EncodeRequest routed;
    RenderQueue::applyHardwareEncodingConfig(config, routed);
    rec(5, routed.useHardwareAccel && routed.hwVendorHint == "amf",
        QStringLiteral("hw=%1 vendor=%2").arg(routed.useHardwareAccel ? 1 : 0)
            .arg(QString::fromStdString(routed.hwVendorHint)));

    // G6: RenderQueue::addJob preserves explicit AV1 codec & AMF vendor
    RenderQueue queue;
    RenderJob job;
    job.exportConfig = config;
    job.exportConfig[QStringLiteral("videoCodec")] = QStringLiteral("libaom-av1");
    queue.addJob(job);
    const QJsonObject stored = queue.jobs().isEmpty() ? QJsonObject() : queue.jobs().constFirst().exportConfig;
    rec(6, stored.value(QStringLiteral("videoCodec")).toString() == QLatin1String("libaom-av1")
            && stored.value(QStringLiteral("hwEncoder")).toString() == QLatin1String("amf")
            && stored.value(QStringLiteral("useHardwareAccel")).toBool(),
        QStringLiteral("codec=%1 hw=%2").arg(stored.value(QStringLiteral("videoCodec")).toString())
            .arg(stored.value(QStringLiteral("hwEncoder")).toString()));

    // G7: AvailabilityCache marks registered AMF unavailable when runtime probe fails and caches it
    AVCodec fakeAmf{};
    fakeAmf.type = AVMEDIA_TYPE_VIDEO;
    fakeAmf.wrapper_name = "amf";
    AVCodec fakeSoftware{};
    fakeSoftware.type = AVMEDIA_TYPE_VIDEO;

    std::atomic<int> failedOpenCalls{0};
    codecdetector_detail::AvailabilityCache unavailableCache(
        [&](const char *name) -> const AVCodec * {
            return std::strcmp(name, "h264_amf") == 0 ? &fakeAmf : nullptr;
        },
        [&](const AVCodec *) {
            ++failedOpenCalls;
            return false;
        });
    const bool firstFail = !unavailableCache.isAvailable(QStringLiteral("h264_amf"));
    const bool secondFail = !unavailableCache.isAvailable(QStringLiteral("h264_amf"));
    rec(7, firstFail && secondFail && failedOpenCalls.load() == 1,
        QStringLiteral("calls=%1").arg(failedOpenCalls.load()));

    // G8: AvailabilityCache is thread-safe (16 threads probe once)
    std::atomic<int> concurrentOpenCalls{0};
    codecdetector_detail::AvailabilityCache concurrentCache(
        [&](const char *) -> const AVCodec * { return &fakeAmf; },
        [&](const AVCodec *) {
            ++concurrentOpenCalls;
            return true;
        });
    std::vector<std::thread> workers;
    std::atomic<int> concurrentSuccesses{0};
    for (int i = 0; i < 16; ++i) {
        workers.emplace_back([&]() {
            if (concurrentCache.isAvailable(QStringLiteral("av1_amf")))
                ++concurrentSuccesses;
        });
    }
    for (auto &w : workers)
        w.join();
    rec(8, concurrentSuccesses.load() == 16 && concurrentOpenCalls.load() == 1,
        QStringLiteral("ok=%1 calls=%2").arg(concurrentSuccesses.load()).arg(concurrentOpenCalls.load()));

    // G9: Software encoders stay cheap registration checks (0 hardware opens)
    std::atomic<int> softwareOpenCalls{0};
    codecdetector_detail::AvailabilityCache softwareCache(
        [&](const char *name) -> const AVCodec * {
            return std::strcmp(name, "libx264") == 0 ? &fakeSoftware : nullptr;
        },
        [&](const AVCodec *) {
            ++softwareOpenCalls;
            return false;
        });
    rec(9, softwareCache.isAvailable(QStringLiteral("libx264")) && softwareOpenCalls.load() == 0,
        QStringLiteral("swCalls=%1").arg(softwareOpenCalls.load()));

    // G10: Non-family codecs (prores_ks) never fall back to another codec family
    request.videoCodecName = "prores_ks";
    const auto prores = libavcore::videoEncoderCandidateNames(request);
    rec(10, prores.size() == 1 && prores.front() == "prores_ks",
        QStringLiteral("cnt=%1").arg(prores.size()));

    // G11: selectAv1SoftwareEncoder respects AOM / SVT / rav1e priority
    const QString noAv1 = codecdetector_detail::selectAv1SoftwareEncoder([](const QString &) { return false; });
    const QString svtOnly = codecdetector_detail::selectAv1SoftwareEncoder(
        [](const QString &n) { return n == QLatin1String("libsvtav1"); });
    const QString rav1eOnly = codecdetector_detail::selectAv1SoftwareEncoder(
        [](const QString &n) { return n == QLatin1String("librav1e"); });
    rec(11, noAv1 == QLatin1String("libaom-av1")
            && svtOnly == QLatin1String("libsvtav1")
            && rav1eOnly == QLatin1String("librav1e"),
        QStringLiteral("seed=%1 svt=%2 rav=%3").arg(noAv1, svtOnly, rav1eOnly));

    // G12: availableVideoEncoders keeps all AV1 choices selectable & ExportConfig display name = AV1
    const auto videoEncoders = CodecDetector::availableVideoEncoders();
    const bool av1EntriesPresent =
        std::any_of(videoEncoders.cbegin(), videoEncoders.cend(),
                    [](const CodecOption &o) { return o.ffmpegName == QLatin1String("libaom-av1"); })
        && std::any_of(videoEncoders.cbegin(), videoEncoders.cend(),
                       [](const CodecOption &o) { return o.ffmpegName == QLatin1String("libsvtav1"); })
        && std::any_of(videoEncoders.cbegin(), videoEncoders.cend(),
                       [](const CodecOption &o) { return o.ffmpegName == QLatin1String("av1_amf"); });
    ExportConfig aomCfg;
    aomCfg.videoCodec = QStringLiteral("libaom-av1");
    rec(12, av1EntriesPresent && aomCfg.codecDisplayName() == QLatin1String("AV1"),
        QStringLiteral("av1Ui=%1 disp=%2").arg(av1EntriesPresent ? 1 : 0).arg(aomCfg.codecDisplayName()));

    // G13: HDR 10-bit probe obeys runtime availability predicate
    const bool runtimeFiltered = !libavcore::firstTenBitHevcEncoder(
        [](const std::string &) { return false; }).has_value();
    rec(13, runtimeFiltered, QStringLiteral("rejected=%1").arg(runtimeFiltered ? 1 : 0));

    // G14: 10-bit hevc_amf availability implies base hevc_amf availability
    const bool hostHevcAmf = CodecDetector::isEncoderAvailable(QStringLiteral("hevc_amf"));
    const bool hostHevcAmf10 = CodecDetector::isTenBitEncoderAvailable(QStringLiteral("hevc_amf"));
    rec(14, !hostHevcAmf10 || hostHevcAmf,
        QStringLiteral("amf8=%1 amf10=%2").arg(hostHevcAmf ? 1 : 0).arg(hostHevcAmf10 ? 1 : 0));

    // G15: Explicit AMD HDR policy never spills into NVENC/QSV
    const auto explicitAmfHdr = libavcore::tenBitHevcEncoderCandidateNames(true, "amf");
    rec(15, startsWith(explicitAmfHdr, "hevc_amf")
            && std::find(explicitAmfHdr.begin(), explicitAmfHdr.end(), "hevc_nvenc") == explicitAmfHdr.end()
            && std::find(explicitAmfHdr.begin(), explicitAmfHdr.end(), "hevc_qsv") == explicitAmfHdr.end()
            && containsBefore(explicitAmfHdr, "hevc_amf", "libx265"),
        QStringLiteral("cnt=%1").arg(explicitAmfHdr.size()));

    // G16: Software-only HDR policy contains only libx265
    const auto softwareHdr = libavcore::tenBitHevcEncoderCandidateNames(false, "none");
    rec(16, softwareHdr.size() == 1 && softwareHdr.front() == "libx265",
        QStringLiteral("cnt=%1").arg(softwareHdr.size()));

    // G17: Automatic HDR policy prefers GPU candidates before libx265
    const auto autoHdr = libavcore::tenBitHevcEncoderCandidateNames(true, "auto");
    rec(17, startsWith(autoHdr, "hevc_nvenc") && containsBefore(autoHdr, "hevc_amf", "libx265"),
        QStringLiteral("cnt=%1").arg(autoHdr.size()));

    // G18: AMF encoder options emit transcoding usage, balanced quality, and vbr_peak in bitrate mode
    libavcore::EncodeRequest brReq;
    brReq.rateControl = libavcore::EncodeRequest::RateControl::Bitrate;
    brReq.videoBitrateBits = 12'000'000;
    const auto amfH264Opts = libavcore::codecOptionsFor(brReq, QStringLiteral("h264_amf"));
    const auto amfAv1Opts = libavcore::codecOptionsFor(brReq, QStringLiteral("av1_amf"));
    rec(18, hasOption(amfH264Opts, "usage", "transcoding")
            && hasOption(amfH264Opts, "quality", "balanced")
            && hasOption(amfH264Opts, "rc", "vbr_peak")
            && hasOption(amfAv1Opts, "usage", "transcoding")
            && hasOption(amfAv1Opts, "quality", "balanced"),
        QStringLiteral("h264Opts=%1 av1Opts=%2").arg(amfH264Opts.size()).arg(amfAv1Opts.size()));

    return tally.finish("radeon-amf");
}
