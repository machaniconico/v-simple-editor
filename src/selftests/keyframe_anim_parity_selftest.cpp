// src/selftests/keyframe_anim_parity_selftest.cpp
// Render-time S1 motion/opacity keyframe parity gates.

#include "../clipanim/ClipAnim.h"
#include "../ClipGeometry.h"
#include "../FrameDiff.h"
#include "../Keyframe.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"

#include <cmath>
#include <cstdio>

#include <QColor>
#include <QByteArray>
#include <QDebug>
#include <QTemporaryDir>

namespace {

constexpr int kClipW = 64;
constexpr int kClipH = 64;
constexpr int kFps = 10;
constexpr int kFrameCount = 20;
constexpr double kEps = 1e-6;

QImage makePatternFrame()
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const int r = (x * 5 + y * 2) & 255;
            const int g = (x * 3 + y * 7) & 255;
            const int b = (x * 11 + y * 13 + 23) & 255;
            frame.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    return frame;
}

bool writeSyntheticClip(const QString &path, QString *error)
{
    libavcore::EncodeRequest req;
    req.width = kClipW;
    req.height = kClipH;
    req.fps = kFps;
    req.fpsNum = kFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 600000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (auto err = encoder.open(req)) {
        if (error)
            *error = QString::fromStdString(*err);
        return false;
    }

    const QImage frame = makePatternFrame();
    for (int i = 0; i < kFrameCount; ++i) {
        if (!encoder.pushFrame(frame, i)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrame failed at frame %1").arg(i);
            return false;
        }
    }
    if (auto err = encoder.finalize()) {
        if (error)
            *error = QString::fromStdString(*err);
        return false;
    }
    return true;
}

ClipInfo makeClip(const QString &path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("keyframe_anim_parity");
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
}

void setTimelineClip(Timeline &timeline, const ClipInfo &clip)
{
    timeline.videoTracks().first()->setClips(QVector<ClipInfo>{clip});
}

bool near(double a, double b, double eps = kEps)
{
    return std::abs(a - b) <= eps;
}

} // namespace

int runKeyframeAnimParitySelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[keyframe-anim-parity] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    QTemporaryDir tempDir;
    QString clipPath;
    if (!tempDir.isValid()) {
        check(0, "temp dir", false, QStringLiteral("QTemporaryDir invalid"));
        return 1;
    }
    clipPath = tempDir.filePath(QStringLiteral("keyframe_anim_src.mp4"));
    QString mediaError;
    if (!writeSyntheticClip(clipPath, &mediaError)) {
        check(0, "synthetic media", false, mediaError);
        return 1;
    }

    // G1: no motion keyframes returns static transform + static opacity.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.videoScale = 1.35;
        clip.videoDx = 0.17;
        clip.videoDy = -0.09;
        clip.rotation2DDegrees = 12.5;
        clip.opacity = 0.42;

        const clipgeom::ClipTransform t =
            clipanim::effectiveTransformAt(clip, 0.5);
        const double opacity =
            clipanim::effectiveOpacityAt(clip, 0.5, clip.opacity);
        check(1, "no keyframes == static values",
              near(t.videoScale, clip.videoScale)
                  && near(t.videoDx, clip.videoDx)
                  && near(t.videoDy, clip.videoDy)
                  && near(t.rotationDeg, clip.rotation2DDegrees)
                  && near(opacity, clip.opacity));
    }

    // G2: renderFrameAt keyframe-free output matches the legacy static oracle.
    {
        const QSize outSize(80, 60);
        const double sourceSec = 0.5;
        const qint64 usec = 500000;
        const QVector<clipgeom::ClipTransform> transforms = {
            clipgeom::ClipTransform{0.75, 0.10, -0.08, 0.0},
            clipgeom::ClipTransform{1.30, -0.12, 0.07, 15.0},
        };
        bool ok = true;
        QString detail;
        const QImage native =
            tlrender::detail::decodeClipFrameNativeForTest(clipPath, sourceSec);
        if (native.isNull()) {
            ok = false;
            detail = QStringLiteral("decodeClipFrameNativeForTest returned null");
        }
        for (int i = 0; ok && i < transforms.size(); ++i) {
            ClipInfo clip = makeClip(clipPath);
            clip.videoScale = transforms[i].videoScale;
            clip.videoDx = transforms[i].videoDx;
            clip.videoDy = transforms[i].videoDy;
            clip.rotation2DDegrees = transforms[i].rotationDeg;
            Timeline timeline;
            setTimelineClip(timeline, clip);
            const QImage rendered = tlrender::renderFrameAt(&timeline, usec, outSize);
            const QImage expected =
                clipgeom::renderLayer(native, transforms[i], outSize, /*smooth=*/true);
            const double mse = framediff::mse(rendered, expected);
            if (mse != 0.0) {
                ok = false;
                detail = QStringLiteral("variant %1 MSE=%2").arg(i).arg(mse, 0, 'g', 12);
            }
        }
        check(2, "keyframe-free renderFrameAt == static oracle", ok, detail);
    }

    // G3: motion.scale interpolation is delegated to KeyframeManager::valueAt.
    {
        ClipInfo clip = makeClip(clipPath);
        KeyframeTrack scale(QStringLiteral("motion.scale"), 1.0);
        scale.addKeyframe(0.0, 1.0);
        scale.addKeyframe(1.0, 2.0);
        clip.keyframes.addTrack(scale);
        const clipgeom::ClipTransform t =
            clipanim::effectiveTransformAt(clip, 0.5);
        check(3, "motion.scale linear interpolation at t=0.5",
              near(t.videoScale, 1.5, 1e-5),
              QStringLiteral("scale=%1").arg(t.videoScale, 0, 'g', 12));
    }

    // G4: renderFrameAt changes over time when motion.scale is keyed.
    {
        ClipInfo clip = makeClip(clipPath);
        KeyframeTrack scale(QStringLiteral("motion.scale"), 1.0);
        scale.addKeyframe(0.0, 1.0);
        scale.addKeyframe(1.0, 2.0);
        clip.keyframes.addTrack(scale);
        Timeline timeline;
        setTimelineClip(timeline, clip);
        const QSize outSize(80, 60);
        const QImage a = tlrender::renderFrameAt(&timeline, 0, outSize);
        const QImage b = tlrender::renderFrameAt(&timeline, 1'000'000, outSize);
        const double mse = framediff::mse(a, b);
        check(4, "renderFrameAt output changes with scale keyframes",
              mse > 0.0,
              QStringLiteral("MSE=%1").arg(mse, 0, 'g', 12));
    }

    // G5: motion.opacity evaluation changes over time.
    {
        ClipInfo clip = makeClip(clipPath);
        KeyframeTrack opacity(QStringLiteral("motion.opacity"), 1.0);
        opacity.addKeyframe(0.0, 1.0);
        opacity.addKeyframe(1.0, 0.2);
        clip.keyframes.addTrack(opacity);
        const double at0 = clipanim::effectiveOpacityAt(clip, 0.0, clip.opacity);
        const double atHalf = clipanim::effectiveOpacityAt(clip, 0.5, clip.opacity);
        const double at1 = clipanim::effectiveOpacityAt(clip, 1.0, clip.opacity);
        check(5, "motion.opacity keyframe evaluation changes over time",
              near(at0, 1.0, 1e-5) && near(atHalf, 0.6, 1e-5) && near(at1, 0.2, 1e-5),
              QStringLiteral("t0=%1 t0.5=%2 t1=%3")
                  .arg(at0, 0, 'g', 12)
                  .arg(atHalf, 0, 'g', 12)
                  .arg(at1, 0, 'g', 12));
    }

    // G6: discrete effect parameters clamp easing overshoot into schema range.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createMirror(0));

        const QString trackName = QStringLiteral("effect.0.mode");
        KeyframeTrack mirrorMode(trackName, 0.0);
        mirrorMode.addKeyframe(0.0, 0.0, KeyframePoint::ElasticOut);
        mirrorMode.addKeyframe(1.0, 3.0);
        clip.keyframes.addTrack(mirrorMode);

        const double sampleTime = 0.15;
        const double rawValue = clip.keyframes.valueAt(trackName, sampleTime, 0.0);
        const QVector<VideoEffect> effects =
            clipanim::effectiveEffectsAt(clip, sampleTime);
        const double effectiveMode =
            effects.isEmpty() ? -1.0 : effects.first().param1;
        check(6, "ElasticOut enum/int effect keyframes clamp to valid range",
              rawValue > 3.0
                  && effects.size() == 1
                  && effectiveMode >= 0.0
                  && effectiveMode <= 3.0
                  && near(effectiveMode, 3.0, 1e-9),
              QStringLiteral("raw=%1 effective=%2")
                  .arg(rawValue, 0, 'g', 12)
                  .arg(effectiveMode, 0, 'g', 12));
    }

    qInfo().noquote()
        << QStringLiteral("KEYFRAME-ANIM-PARITY: %1/%2 PASS")
               .arg(passed)
               .arg(passed + failed);
    return failed == 0 ? 0 : 1;
}
