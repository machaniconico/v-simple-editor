// src/selftests/effect_keyframe_parity_selftest.cpp
// Render-time S2 effect-parameter keyframe parity gates.

#include "../clipanim/ClipAnim.h"
#include "../EffectParamSchema.h"
#include "../FrameDiff.h"
#include "../Keyframe.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"

#include <cmath>
#include <cstdio>

#include <QByteArray>
#include <QColor>
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
    clip.displayName = QStringLiteral("effect_keyframe_parity");
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

bool sameEffect(const VideoEffect &a, const VideoEffect &b)
{
    return a.type == b.type
        && a.enabled == b.enabled
        && near(a.param1, b.param1)
        && near(a.param2, b.param2)
        && near(a.param3, b.param3)
        && a.keyColor == b.keyColor;
}

bool sameEffects(const QVector<VideoEffect> &a, const QVector<VideoEffect> &b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (!sameEffect(a[i], b[i]))
            return false;
    }
    return true;
}

} // namespace

int runEffectKeyframeParitySelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[effect-keyframe-parity] %s G%d %s%s%s\n",
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
    clipPath = tempDir.filePath(QStringLiteral("effect_keyframe_src.mp4"));
    QString mediaError;
    if (!writeSyntheticClip(clipPath, &mediaError)) {
        check(0, "synthetic media", false, mediaError);
        return 1;
    }

    // G1: no effect.* keyframes returns the static effect stack unchanged.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(7.0));
        clip.effects.append(VideoEffect::createVignette(0.35, 0.72));

        KeyframeTrack scale(QStringLiteral("motion.scale"), 1.0);
        scale.addKeyframe(0.0, 1.0);
        scale.addKeyframe(1.0, 2.0);
        clip.keyframes.addTrack(scale);

        const QVector<VideoEffect> effects =
            clipanim::effectiveEffectsAt(clip, 0.5);
        check(1, "no effect keyframes == static effect values",
              sameEffects(effects, clip.effects));
    }

    // G2: renderFrameAt without effect.* keyframes matches a static FX oracle.
    {
        const QSize outSize(80, 60);
        const double sourceSec = 0.5;
        const qint64 usec = 500000;
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(6.0));

        Timeline timeline;
        setTimelineClip(timeline, clip);
        const QImage rendered = tlrender::renderFrameAt(&timeline, usec, outSize);

        const QImage native =
            tlrender::detail::decodeClipFrameNativeForTest(clipPath, sourceSec);
        QImage expected;
        QString detail;
        bool ok = !native.isNull() && !rendered.isNull();
        if (ok) {
            expected = VideoEffectProcessor::applyEffectStack(
                           native, ColorCorrection(), clip.effects)
                           .convertToFormat(QImage::Format_RGBA8888)
                           .scaled(outSize, Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
        } else if (native.isNull()) {
            detail = QStringLiteral("decodeClipFrameNativeForTest returned null");
        } else {
            detail = QStringLiteral("renderFrameAt returned null");
        }

        const double mse = ok ? framediff::mse(rendered, expected) : -1.0;
        check(2, "keyframe-free renderFrameAt == static FX oracle",
              ok && mse == 0.0,
              ok ? QStringLiteral("MSE=%1").arg(mse, 0, 'g', 12) : detail);
    }

    // G3: effect.0.radius interpolation is delegated to KeyframeManager::valueAt.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(1.0));
        KeyframeTrack radius(QStringLiteral("effect.0.radius"), 1.0);
        radius.addKeyframe(0.0, 1.0);
        radius.addKeyframe(1.0, 11.0);
        clip.keyframes.addTrack(radius);

        const QVector<VideoEffect> effects =
            clipanim::effectiveEffectsAt(clip, 0.5);
        const double value = effects.isEmpty()
            ? -1.0
            : effectctrl::paramValue(effects.first(), QStringLiteral("radius"));
        check(3, "Blur radius linear interpolation at t=0.5",
              near(value, 6.0, 1e-5),
              QStringLiteral("radius=%1").arg(value, 0, 'g', 12));
    }

    // G4: renderFrameAt changes over clip-local time when an effect param is keyed.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.speed = 0.000001;
        clip.effects.append(VideoEffect::createBlur(1.0));
        KeyframeTrack radius(QStringLiteral("effect.0.radius"), 1.0);
        radius.addKeyframe(0.0, 1.0);
        radius.addKeyframe(1.0, 18.0);
        clip.keyframes.addTrack(radius);

        Timeline timeline;
        setTimelineClip(timeline, clip);
        const QSize outSize(80, 60);
        const QImage a = tlrender::renderFrameAt(&timeline, 0, outSize);
        const QImage b = tlrender::renderFrameAt(&timeline, 1'000'000, outSize);
        const double mse = framediff::mse(a, b);
        check(4, "renderFrameAt output changes with keyed Blur radius",
              mse > 0.0,
              QStringLiteral("MSE=%1").arg(mse, 0, 'g', 12));
    }

    qInfo().noquote()
        << QStringLiteral("EFFECT-KEYFRAME-PARITY: %1/%2 PASS")
               .arg(passed)
               .arg(passed + failed);
    return failed == 0 ? 0 : 1;
}
