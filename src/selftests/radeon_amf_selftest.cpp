// Deterministic Radeon/AMF routing policy selftest (no Radeon required).
// Run via: --selftest=radeon-amf

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <QJsonObject>
#include <QByteArray>
#include <QFileInfo>
#include <QTemporaryDir>

#include "../CodecDetector.h"
#include "../ExportDialog.h"
#include "../RenderQueue.h"
#include "../libavcore/Encode.h"
#include "../libavcore/Probe.h"

namespace {

bool startsWith(const std::vector<std::string>& names, const char* expected)
{
    return !names.empty() && names.front() == expected;
}

bool containsBefore(const std::vector<std::string>& names,
                    const char* first,
                    const char* second)
{
    const auto firstIt = std::find(names.begin(), names.end(), first);
    const auto secondIt = std::find(names.begin(), names.end(), second);
    return firstIt != names.end() && secondIt != names.end()
        && firstIt < secondIt;
}

#if defined(VEDITOR_AV1)
bool runAv1AmfFallbackSmoke(std::string *activeEncoder,
                            std::string *error)
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        *error = "failed to create temporary directory";
        return false;
    }

    const QString outputPath = tempDir.filePath(QStringLiteral("amf-av1.mp4"));
    libavcore::EncodeRequest smokeRequest;
    smokeRequest.width = 128;
    smokeRequest.height = 128;
    smokeRequest.fps = 30;
    smokeRequest.videoBitrateBits = 500'000;
    smokeRequest.outputPath = outputPath.toUtf8().toStdString();
    smokeRequest.videoCodecName = "libaom-av1";
    smokeRequest.useHardwareAccel = true;
    smokeRequest.hwVendorHint = "amf";
    smokeRequest.encoderAvailableHook = [](const std::string &name) {
        return CodecDetector::isEncoderAvailable(
            QString::fromStdString(name));
    };

    libavcore::FrameEncoder encoder;
    if (const auto openError = encoder.open(smokeRequest)) {
        *error = *openError;
        return false;
    }

    QByteArray rgb(smokeRequest.width * smokeRequest.height * 3, '\0');
    if (!encoder.pushFrameRgb24(
            reinterpret_cast<const uint8_t *>(rgb.constData()),
            smokeRequest.width * 3, 0)) {
        *error = "failed to push AV1 smoke frame";
        return false;
    }
    if (const auto finalizeError = encoder.finalize()) {
        *error = *finalizeError;
        return false;
    }

    *activeEncoder = encoder.activeEncoderName();
    const QFileInfo output(outputPath);
    if (!output.isFile() || output.size() <= 0) {
        *error = "AV1 smoke output is missing or empty";
        return false;
    }
    return *activeEncoder == "av1_amf"
        || *activeEncoder == "libaom-av1";
}
#endif

} // namespace

int runRadeonAmfSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char* description, bool ok) {
        std::printf("[radeon-amf] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", gate, description);
        ok ? ++passed : ++failed;
    };

    libavcore::EncodeRequest request;
    request.useHardwareAccel = true;
    request.hwVendorHint = "amf";

    request.videoCodecName = "libx264";
    const auto h264 = libavcore::videoEncoderCandidateNames(request);
    check(1, "explicit AMD selection routes H.264 to h264_amf first",
          startsWith(h264, "h264_amf"));
    check(2, "H.264 AMF falls back to libx264",
          containsBefore(h264, "h264_amf", "libx264"));

    request.videoCodecName = "libx265";
    const auto hevc = libavcore::videoEncoderCandidateNames(request);
    check(3, "explicit AMD selection routes HEVC to hevc_amf first",
          startsWith(hevc, "hevc_amf"));
    check(4, "HEVC AMF falls back to libx265",
          containsBefore(hevc, "hevc_amf", "libx265"));

    request.videoCodecName = "libaom-av1";
    const auto av1 = libavcore::videoEncoderCandidateNames(request);
    check(5, "explicit AMD selection routes AV1 to av1_amf first",
          startsWith(av1, "av1_amf"));
    check(6, "AV1 AMF falls back to libaom-av1",
          containsBefore(av1, "av1_amf", "libaom-av1"));

    check(7, "av1_amf is classified as hardware",
          libavcore::isHardwareVideoEncoderName("av1_amf"));
    check(8, "software AV1 is not classified as hardware",
          !libavcore::isHardwareVideoEncoderName("libsvtav1"));

    QJsonObject config;
    config[QStringLiteral("hwEncoder")] = QStringLiteral("amf");
    config[QStringLiteral("useHardwareAccel")] = true;
    libavcore::EncodeRequest routed;
    RenderQueue::applyHardwareEncodingConfig(config, routed);
    check(9, "RenderQueue preserves the AMD vendor hint",
          routed.hwVendorHint == "amf");
    check(10, "RenderQueue enables the requested hardware path",
          routed.useHardwareAccel);

    RenderQueue queue;
    RenderJob job;
    job.exportConfig = config;
    job.exportConfig[QStringLiteral("videoCodec")] =
        QStringLiteral("libaom-av1");
    queue.addJob(job);
    const auto jobs = queue.jobs();
    const QJsonObject stored = jobs.isEmpty()
        ? QJsonObject() : jobs.constFirst().exportConfig;
    check(11, "queue insertion preserves an explicit AV1 codec",
          stored.value(QStringLiteral("videoCodec")).toString()
              == QStringLiteral("libaom-av1"));
    check(12, "queue insertion preserves the AMF selection",
          stored.value(QStringLiteral("hwEncoder")).toString()
              == QStringLiteral("amf")
          && stored.value(QStringLiteral("useHardwareAccel")).toBool());

    check(13, "linked FFmpeg registers h264_amf",
          avcodec_find_encoder_by_name("h264_amf") != nullptr);
    check(14, "linked FFmpeg registers hevc_amf",
          avcodec_find_encoder_by_name("hevc_amf") != nullptr);
    check(15, "linked FFmpeg registers av1_amf",
          avcodec_find_encoder_by_name("av1_amf") != nullptr);

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
    check(16, "registered AMF with a failed runtime probe is unavailable",
          !unavailableCache.isAvailable(QStringLiteral("h264_amf")));
    check(17, "failed hardware probes are cached",
          !unavailableCache.isAvailable(QStringLiteral("h264_amf"))
          && failedOpenCalls.load() == 1);

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
    for (auto &worker : workers)
        worker.join();
    check(18, "concurrent hardware probes execute once",
          concurrentSuccesses.load() == 16
          && concurrentOpenCalls.load() == 1);

    std::atomic<int> softwareOpenCalls{0};
    codecdetector_detail::AvailabilityCache softwareCache(
        [&](const char *name) -> const AVCodec * {
            return std::strcmp(name, "libx264") == 0
                ? &fakeSoftware : nullptr;
        },
        [&](const AVCodec *) {
            ++softwareOpenCalls;
            return false;
        });
    check(19, "software availability remains a registration check",
          softwareCache.isAvailable(QStringLiteral("libx264"))
          && softwareOpenCalls.load() == 0);

    request.videoCodecName = "prores_ks";
    request.useHardwareAccel = true;
    request.hwVendorHint = "amf";
    const auto prores = libavcore::videoEncoderCandidateNames(request);
    check(20, "non-family codecs never fall back to a different codec family",
          prores.size() == 1 && prores.front() == "prores_ks");
    check(21, "H.264 fallback never changes the requested codec family",
          std::find(h264.begin(), h264.end(), "mpeg4") == h264.end());
    check(22, "HEVC fallback never changes the requested codec family",
          std::find(hevc.begin(), hevc.end(), "mpeg4") == hevc.end());

    const auto noAv1Software =
        codecdetector_detail::selectAv1SoftwareEncoder(
            [](const QString &) { return false; });
    check(23, "missing AV1 software keeps an AOM same-family seed",
          noAv1Software == QLatin1String("libaom-av1"));
    const auto svtOnly =
        codecdetector_detail::selectAv1SoftwareEncoder(
            [](const QString &name) {
                return name == QLatin1String("libsvtav1");
            });
    check(24, "SVT-only installations select the available AV1 encoder",
          svtOnly == QLatin1String("libsvtav1"));
    const auto rav1eOnly =
        codecdetector_detail::selectAv1SoftwareEncoder(
            [](const QString &name) {
                return name == QLatin1String("librav1e");
            });
    check(25, "rav1e-only installations select the available AV1 encoder",
          rav1eOnly == QLatin1String("librav1e"));

    request.videoCodecName = "libaom-av1";
    const auto aomAv1 = libavcore::videoEncoderCandidateNames(request);
    check(26, "explicit AMD selection routes AOM AV1 through av1_amf first",
          containsBefore(aomAv1, "av1_amf", "libaom-av1"));

#if defined(VEDITOR_AV1)
    check(27, "Modern FFmpeg registers the declared AOM fallback",
          avcodec_find_encoder_by_name("libaom-av1") != nullptr);
#endif

    const auto videoEncoders = CodecDetector::availableVideoEncoders();
#if defined(VEDITOR_AV1)
    const bool aomVisible = std::any_of(
        videoEncoders.cbegin(), videoEncoders.cend(),
        [](const CodecOption &option) {
            return option.ffmpegName == QLatin1String("libaom-av1")
                && option.available;
        });
    check(28, "Modern exposes AOM AV1 through codec detection", aomVisible);
#endif

    const bool av1FamiliesRemainSelectable = std::any_of(
        videoEncoders.cbegin(), videoEncoders.cend(),
        [](const CodecOption &option) {
            return option.ffmpegName == QLatin1String("libaom-av1");
        }) && std::any_of(
        videoEncoders.cbegin(), videoEncoders.cend(),
        [](const CodecOption &option) {
            return option.ffmpegName == QLatin1String("libsvtav1");
        }) && std::any_of(
        videoEncoders.cbegin(), videoEncoders.cend(),
        [](const CodecOption &option) {
            return option.ffmpegName == QLatin1String("librav1e");
        });
    check(29, "AV1 software choices remain in the UI when unavailable",
          av1FamiliesRemainSelectable);

    const auto exportPresets = ExportDialog::presets();
    const bool aomPreset = std::any_of(
        exportPresets.cbegin(), exportPresets.cend(),
        [](const ExportPreset &preset) {
            return preset.videoCodec == QLatin1String("libaom-av1");
        });
    ExportConfig aomConfig;
    aomConfig.videoCodec = QStringLiteral("libaom-av1");
    check(30, "AV1 preset and display name use the AOM family seed",
          aomPreset && aomConfig.codecDisplayName() == QLatin1String("AV1"));

    check(31, "HDR capability probe obeys runtime availability filtering",
          !libavcore::firstTenBitHevcEncoder(
              [](const std::string &) { return false; }).has_value());
    const bool hostHevcAmf = CodecDetector::isEncoderAvailable(
        QStringLiteral("hevc_amf"));
    const bool hostHevcAmf10 = CodecDetector::isTenBitEncoderAvailable(
        QStringLiteral("hevc_amf"));
    check(32, "10-bit AMF availability implies base AMF availability",
          !hostHevcAmf10 || hostHevcAmf);

#if defined(VEDITOR_AV1)
    std::string smokeEncoder;
    std::string smokeError;
    const bool smokeOk = runAv1AmfFallbackSmoke(
        &smokeEncoder, &smokeError);
    if (!smokeOk) {
        std::fprintf(stderr, "[radeon-amf] AV1 smoke error: %s\n",
                     smokeError.c_str());
    }
    check(33, "AV1 opens AMF or produces a real AOM fallback artifact",
          smokeOk);
    if (smokeOk) {
        std::printf("[radeon-amf] AV1 smoke encoder: %s\n",
                    smokeEncoder.c_str());
    }
#endif

    const auto explicitAmfHdr =
        libavcore::tenBitHevcEncoderCandidateNames(true, "amf");
    check(34, "explicit AMD HDR policy does not spill into another vendor",
          startsWith(explicitAmfHdr, "hevc_amf")
          && std::find(explicitAmfHdr.begin(), explicitAmfHdr.end(),
                       "hevc_nvenc") == explicitAmfHdr.end()
          && std::find(explicitAmfHdr.begin(), explicitAmfHdr.end(),
                       "hevc_qsv") == explicitAmfHdr.end()
          && containsBefore(explicitAmfHdr, "hevc_amf", "libx265"));
    const auto softwareHdr =
        libavcore::tenBitHevcEncoderCandidateNames(false, "none");
    check(35, "software-only HDR policy never selects a hardware encoder",
          softwareHdr.size() == 1 && softwareHdr.front() == "libx265");
    const auto automaticHdr =
        libavcore::tenBitHevcEncoderCandidateNames(true, "auto");
    check(36, "automatic HDR policy prefers GPU candidates before software",
          startsWith(automaticHdr, "hevc_nvenc")
          && containsBefore(automaticHdr, "hevc_amf", "libx265"));

    std::printf("[radeon-amf] host runtime: h264_amf=%s hevc_amf=%s "
                "hevc_amf10=%s av1_amf=%s\n",
                CodecDetector::isEncoderAvailable(QStringLiteral("h264_amf"))
                    ? "available" : "unavailable",
                hostHevcAmf ? "available" : "unavailable",
                hostHevcAmf10 ? "available" : "unavailable",
                CodecDetector::isEncoderAvailable(QStringLiteral("av1_amf"))
                    ? "available" : "unavailable");

    std::printf("[radeon-amf] summary: passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
