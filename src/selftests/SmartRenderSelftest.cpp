// SmartRenderSelftest.cpp
// Headless selftest for the pure smart-render segment eligibility predicate.
// Run via: --selftest=smart-render

#include <cstdio>

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include "../SmartRender.h"

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

    std::printf("[smart-render] summary: passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
