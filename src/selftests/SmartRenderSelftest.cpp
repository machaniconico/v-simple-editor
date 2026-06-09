// SmartRenderSelftest.cpp
// Headless selftest for the pure smart-render segment eligibility predicate.
// Run via: --selftest=smart-render

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtGlobal>

#include "../ClipGeometry.h"
#include "../SmartRender.h"
#include "../libavcore/Concat.h"
#include "../libavcore/Encode.h"
#include "../libavcore/Probe.h"

namespace {

struct EnvGuard {
    EnvGuard()
        : had(qEnvironmentVariableIsSet("VEDITOR_SMART_RENDER")),
          value(qgetenv("VEDITOR_SMART_RENDER"))
    {
    }

    ~EnvGuard()
    {
        if (had)
            qputenv("VEDITOR_SMART_RENDER", value);
        else
            qunsetenv("VEDITOR_SMART_RENDER");
    }

    bool had = false;
    QByteArray value;
};

ClipInfo pristineClip(const QString& codec = QStringLiteral("h264"))
{
    ClipInfo clip;
    clip.filePath = QStringLiteral(
        "smartrender://clip?codec=%1&width=1920&height=1080&fps=30&pixfmt=yuv420p")
        .arg(codec);
    clip.displayName = QStringLiteral("smart-render-fixture");
    clip.duration = 1.0;
    clip.outPoint = 1.0;
    clip.speed = 1.0;
    clip.videoScale = 1.0;
    clip.videoDx = 0.0;
    clip.videoDy = 0.0;
    clip.rotation2DDegrees = 0.0;
    return clip;
}

smartrender::SegmentEligibility eligibleFor(const ClipInfo& clip,
                                            const QString& outputCodec = QStringLiteral("h264"),
                                            int outputWidth = 1920,
                                            int outputHeight = 1080,
                                            double outputFps = 30.0,
                                            bool hasEffects = false,
                                            bool hasColorCorrection = false,
                                            bool hasTransform = false,
                                            bool hasTransitions = false,
                                            bool hasSpeedChange = false,
                                            bool hasKeyframes = false,
                                            bool hasLayerStyle = false,
                                            bool hasTrackMatte = false,
                                            bool isOverlayLayer = false)
{
    return smartrender::canStreamCopy(clip,
                                      outputCodec,
                                      outputWidth,
                                      outputHeight,
                                      outputFps,
                                      hasEffects,
                                      hasColorCorrection,
                                      hasTransform,
                                      hasTransitions,
                                      hasSpeedChange,
                                      hasKeyframes,
                                      hasLayerStyle,
                                      hasTrackMatte,
                                      isOverlayLayer);
}

smartrender::PassThroughTimelineRequest passThroughRequest(
    const ClipInfo& clip = pristineClip())
{
    smartrender::PassThroughTimelineRequest request;
    request.videoTracks = {{clip}};
    request.outputPath = QStringLiteral("smart-render-output.mp4");
    request.outputCodec = QStringLiteral("h264");
    request.outputWidth = 1920;
    request.outputHeight = 1080;
    request.outputFps = 30.0;
    request.timelineDurationSec = clip.effectiveDuration();
    return request;
}

bool encodeTinyMp4(const QString& path, QString* error)
{
    static constexpr int kWidth = 16;
    static constexpr int kHeight = 16;
    static constexpr int kFps = 4;
    static constexpr int kFrames = 4;
    static constexpr int kStride = kWidth * 3;

    libavcore::EncodeRequest request;
    request.width = kWidth;
    request.height = kHeight;
    request.fps = kFps;
    request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 200000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (const auto openErr = encoder.open(request)) {
        if (error)
            *error = QStringLiteral("FrameEncoder::open failed: ")
                + QString::fromStdString(*openErr);
        return false;
    }

    QByteArray frame(kStride * kHeight, Qt::Uninitialized);
    for (int f = 0; f < kFrames; ++f) {
        uchar* data = reinterpret_cast<uchar*>(frame.data());
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int off = y * kStride + x * 3;
                data[off + 0] = static_cast<uchar>((x * 16 + f * 11) & 0xff);
                data[off + 1] = static_cast<uchar>((y * 16 + f * 17) & 0xff);
                data[off + 2] = static_cast<uchar>((x * y + f * 23) & 0xff);
            }
        }
        if (!encoder.pushFrameRgb24(
                reinterpret_cast<const uint8_t*>(frame.constData()),
                kStride,
                f)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrameRgb24 failed");
            return false;
        }
    }

    if (const auto finalizeErr = encoder.finalize()) {
        if (error)
            *error = QStringLiteral("FrameEncoder::finalize failed: ")
                + QString::fromStdString(*finalizeErr);
        return false;
    }

    return QFile::exists(path);
}

bool realRemuxGate(QString* error)
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        if (error)
            *error = QStringLiteral("QTemporaryDir failed");
        return false;
    }

    const QString inputPath = dir.filePath(QStringLiteral("input.mp4"));
    const QString outputPath = dir.filePath(QStringLiteral("output.mp4"));
    if (!encodeTinyMp4(inputPath, error))
        return false;

    if (const auto copyErr = libavcore::concatCopy(
            {inputPath.toStdString()}, outputPath.toStdString())) {
        if (error)
            *error = QStringLiteral("concatCopy failed: ")
                + QString::fromStdString(*copyErr);
        return false;
    }

    const auto inputCodec =
        libavcore::probeVideoCodecName(inputPath.toStdString());
    const auto outputCodec =
        libavcore::probeVideoCodecName(outputPath.toStdString());
    if (!inputCodec.has_value() || !outputCodec.has_value()
        || *inputCodec != *outputCodec) {
        if (error) {
            *error = QStringLiteral("codec mismatch after remux: in=%1 out=%2")
                .arg(inputCodec.has_value()
                         ? QString::fromStdString(*inputCodec)
                         : QStringLiteral("<none>"))
                .arg(outputCodec.has_value()
                         ? QString::fromStdString(*outputCodec)
                         : QStringLiteral("<none>"));
        }
        return false;
    }

    const auto inputDuration =
        libavcore::probeDurationMicroseconds(inputPath.toStdString());
    const auto outputDuration =
        libavcore::probeDurationMicroseconds(outputPath.toStdString());
    if (!inputDuration.has_value() || !outputDuration.has_value()
        || std::llabs(*inputDuration - *outputDuration) > 500000) {
        if (error) {
            *error = QStringLiteral("duration mismatch after remux: in=%1 out=%2")
                .arg(inputDuration.has_value()
                         ? QString::number(*inputDuration)
                         : QStringLiteral("<none>"))
                .arg(outputDuration.has_value()
                         ? QString::number(*outputDuration)
                         : QStringLiteral("<none>"));
        }
        return false;
    }

    return true;
}

} // namespace

int runSmartRenderSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[smart-render] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    EnvGuard envGuard;

    {
        const smartrender::SegmentEligibility result = eligibleFor(pristineClip());
        check(1, "pristine matching clip is eligible", result.eligible);
    }

    auto checkRejected = [&](const char* desc,
                             const smartrender::SegmentEligibility& result) {
        check(2, desc, !result.eligible && !result.reason.isEmpty());
    };

    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasEffects rejects", eligibleFor(clip, QStringLiteral("h264"),
                                                        1920, 1080, 30.0, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasColorCorrection rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, true));
    }
    {
        ClipInfo clip = pristineClip();
        clip.videoScale = 1.01;
        checkRejected("non-identity transform rejects", eligibleFor(clip));
    }
    {
        ClipInfo clip = pristineClip();
        clip.speed = 0.5;
        checkRejected("speed change rejects", eligibleFor(clip));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasTransitions rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasKeyframes rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, false, false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasLayerStyle rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, false, false, false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasTrackMatte rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, false, false, false, false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("isOverlayLayer rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, false, false, false, false,
                                  false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("codec mismatch rejects", eligibleFor(clip, QStringLiteral("hevc")));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("resolution width mismatch rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1280));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("fps mismatch rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 24.0));
    }

    qunsetenv("VEDITOR_SMART_RENDER");
    check(3, "env unset keeps smart render disabled by default",
          !smartrender::enabledFromEnv());

    {
        const ClipInfo clip = pristineClip(QString());
        const smartrender::SegmentEligibility result = eligibleFor(clip);
        check(4, "empty source codec is conservative false",
              !result.eligible && !result.reason.isEmpty());
    }

    auto checkPassThroughRejected =
        [&](int g,
            const char* desc,
            const smartrender::PassThroughTimelineRequest& request) {
            const smartrender::PassThroughEligibility result =
                smartrender::timelinePassThrough(request);
            check(g, desc, !result.eligible && !result.reason.isEmpty());
        };

    {
        auto request = passThroughRequest();
        request.videoTracks[0].append(pristineClip());
        request.timelineDurationSec = 2.0;
        checkPassThroughRejected(5, "timelinePassThrough rejects multiple clips",
                                 request);
    }
    {
        ClipInfo clip = pristineClip();
        clip.inPoint = 0.1;
        clip.outPoint = 1.0;
        checkPassThroughRejected(6, "timelinePassThrough rejects trims",
                                 passThroughRequest(clip));
    }
    {
        ClipInfo clip = pristineClip();
        clip.leadInSec = 0.5;
        auto request = passThroughRequest(clip);
        request.timelineDurationSec = 1.5;
        checkPassThroughRejected(7, "timelinePassThrough rejects non-zero start",
                                 request);
    }
    {
        ClipInfo clip = pristineClip();
        clip.volume = 0.5;
        checkPassThroughRejected(8, "timelinePassThrough rejects audio processing",
                                 passThroughRequest(clip));
    }
    {
        auto request = passThroughRequest();
        request.jobEndUs = 500000;
        checkPassThroughRejected(9, "timelinePassThrough rejects partial export range",
                                 request);
    }
    {
        auto request = passThroughRequest();
        request.outputCodec = QStringLiteral("hevc");
        const smartrender::PassThroughEligibility result =
            smartrender::timelinePassThrough(request);
        check(10, "timelinePassThrough propagates canStreamCopy rejection",
              !result.eligible && result.reason.contains(QStringLiteral("codec")));
    }
    {
        const smartrender::PassThroughEligibility result =
            smartrender::timelinePassThrough(passThroughRequest());
        check(11, "timelinePassThrough accepts whole pristine timeline",
              result.eligible);
    }
    {
        QString error;
        const bool ok = realRemuxGate(&error);
        if (!ok)
            std::printf("[smart-render] real remux detail: %s\n",
                        error.toUtf8().constData());
        check(12, "concatCopy single-input remux preserves codec and duration",
              ok);
    }
    {
        auto request = passThroughRequest();
        request.videoTracks = {{}, {pristineClip()}};
        checkPassThroughRejected(13, "timelinePassThrough rejects clip off track 0",
                                 request);
    }
    {
        auto request = passThroughRequest();
        request.audioTracks = {{pristineClip()}};
        checkPassThroughRejected(14, "timelinePassThrough rejects separate audio clips",
                                 request);
    }
    {
        ClipInfo clip = pristineClip();
        EnhancedTextOverlay overlay;
        overlay.text = QStringLiteral("subtitle");
        clip.textManager.addOverlay(overlay);
        checkPassThroughRejected(15, "timelinePassThrough rejects text overlays",
                                 passThroughRequest(clip));
    }
    {
        auto request = passThroughRequest();
        request.hasAdjustmentLayers = true;
        checkPassThroughRejected(16, "timelinePassThrough rejects adjustment layers",
                                 request);
    }
    {
        auto request = passThroughRequest();
        TimelineTrackMatteEntry entry;
        entry.matteType = TrackMatteType::AlphaMatte;
        entry.matteSourceClipId = trackMatteClipKey(1, 0);
        request.trackMatteEntries.insert(trackMatteClipKey(0, 0), entry);
        checkPassThroughRejected(17, "timelinePassThrough rejects track matte entries",
                                 request);
    }
    {
        auto request = passThroughRequest(pristineClip(QStringLiteral("vp9")));
        request.outputPath = QStringLiteral("smart-render-output.mp4");
        request.outputCodec = QStringLiteral("vp9");
        checkPassThroughRejected(18, "timelinePassThrough rejects unsafe container/codec",
                                 request);
    }
    {
        auto request = passThroughRequest();
        request.hdrExport16Enabled = true;
        checkPassThroughRejected(19, "timelinePassThrough rejects HDR export flags",
                                 request);
    }
    {
        ClipInfo clip = pristineClip();
        clip.filePath = clipgeom::nullObjectFilePath();
        checkPassThroughRejected(20, "timelinePassThrough rejects generated null objects",
                                 passThroughRequest(clip));
    }
    {
        auto request = passThroughRequest();
        request.exportMarkedRangeOnly = true;
        request.hasMarkedRange = true;
        request.markedIn = 0.25;
        request.markedOut = 0.75;
        checkPassThroughRejected(21, "timelinePassThrough rejects marked partial range",
                                 request);
    }
    {
        auto request = passThroughRequest();
        request.hasMarkedRange = true;
        request.markedIn = 0.25;
        request.markedOut = 0.75;
        const smartrender::PassThroughEligibility result =
            smartrender::timelinePassThrough(request);
        check(22, "timelinePassThrough ignores inactive marked range",
              result.eligible);
    }

    std::printf("[smart-render] summary: passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
