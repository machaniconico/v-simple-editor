// src/selftests/effect_timing_selftest.cpp
// Render-time clip-local VideoEffect active-range gates.

#include "../clipanim/ClipAnim.h"
#include "../FrameDiff.h"
#include "../ProjectFile.h"
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

ClipInfo makeClip(const QString &path = QString())
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("effect_timing");
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
        && a.keyColor == b.keyColor
        && near(a.startSec, b.startSec)
        && near(a.endSec, b.endSec);
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

QImage expectedFrame(const QString &clipPath, double sourceSec, const QSize &outSize,
                     const QVector<VideoEffect> &effects)
{
    const QImage native =
        tlrender::detail::decodeClipFrameNativeForTest(clipPath, sourceSec);
    if (native.isNull())
        return QImage();
    QImage expected = native;
    if (!effects.isEmpty()) {
        expected = VideoEffectProcessor::applyEffectStack(
                       native, ColorCorrection(), effects)
                       .convertToFormat(QImage::Format_RGBA8888);
    }
    return expected.scaled(outSize, Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation);
}

ProjectData projectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    return data;
}

} // namespace

int runEffectTimingSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[effect-timing] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    // G1: default timing (-1/-1) keeps the static effects vector unchanged.
    {
        ClipInfo clip = makeClip();
        clip.effects.append(VideoEffect::createBlur(7.0));
        clip.effects.append(VideoEffect::createVignette(0.35, 0.72));

        const QVector<VideoEffect> effects =
            clipanim::effectiveEffectsAt(clip, 0.5);
        check(1, "default timing returns static effects",
              sameEffects(effects, clip.effects));
    }

    // G2: explicit [1.0, 2.0] clip-local range filters outside the interval.
    {
        ClipInfo clip = makeClip();
        VideoEffect effect = VideoEffect::createInvert();
        effect.startSec = 1.0;
        effect.endSec = 2.0;
        clip.effects.append(effect);

        const QVector<VideoEffect> atHalf =
            clipanim::effectiveEffectsAt(clip, 0.5);
        const QVector<VideoEffect> atMid =
            clipanim::effectiveEffectsAt(clip, 1.5);
        const QVector<VideoEffect> atLate =
            clipanim::effectiveEffectsAt(clip, 2.5);
        check(2, "clip-local timing filters active interval",
              atHalf.isEmpty()
                  && sameEffects(atMid, clip.effects)
                  && atLate.isEmpty(),
              QStringLiteral("sizes=%1/%2/%3")
                  .arg(atHalf.size())
                  .arg(atMid.size())
                  .arg(atLate.size()));
    }

    QTemporaryDir tempDir;
    QString clipPath;
    if (!tempDir.isValid()) {
        check(0, "temp dir", false, QStringLiteral("QTemporaryDir invalid"));
        return 1;
    }
    clipPath = tempDir.filePath(QStringLiteral("effect_timing_src.mp4"));
    QString mediaError;
    if (!writeSyntheticClip(clipPath, &mediaError)) {
        check(0, "synthetic media", false, mediaError);
        return 1;
    }

    // G3: renderFrameAt skips the effect outside the range and applies it inside.
    {
        ClipInfo clip = makeClip(clipPath);
        // speed=1.0: v1LocalSec == usec/1e6 when clip starts at timeline t=0.
        // usec=500000  -> v1LocalSec=0.5  (before startSec=1.0 -> effect skipped)
        // usec=1500000 -> v1LocalSec=1.5  (inside [1.0,2.0]    -> effect applied)
        clip.speed = 1.0;
        VideoEffect effect = VideoEffect::createInvert();
        effect.startSec = 1.0;
        effect.endSec = 2.0;
        clip.effects.append(effect);

        Timeline timeline;
        setTimelineClip(timeline, clip);
        const QSize outSize(kClipW, kClipH);
        const double outsideLocal = 0.5;  // clip-local seconds
        const double insideLocal  = 1.5;  // clip-local seconds
        const QImage outside =
            tlrender::renderFrameAt(&timeline, 500000, outSize);
        const QImage inside =
            tlrender::renderFrameAt(&timeline, 1500000, outSize);
        // source sec = inPoint + clipLocal * speed = 0 + clipLocal * 1.0
        const QImage expectedOutside =
            expectedFrame(clipPath, outsideLocal, outSize, {});
        const QImage expectedInside =
            expectedFrame(clipPath, insideLocal,  outSize, QVector<VideoEffect>{effect});

        bool ok = !outside.isNull() && !inside.isNull()
            && !expectedOutside.isNull() && !expectedInside.isNull();
        const double mseOutside = ok ? framediff::mse(outside, expectedOutside) : -1.0;
        const double mseInside = ok ? framediff::mse(inside, expectedInside) : -1.0;
        const double mseDifferent = ok ? framediff::mse(outside, inside) : -1.0;
        check(3, "renderFrameAt applies timing-filtered effect stack",
              ok && mseOutside == 0.0 && mseInside == 0.0 && mseDifferent > 0.0,
              QStringLiteral("outside=%1 inside=%2 diff=%3")
                  .arg(mseOutside, 0, 'g', 12)
                  .arg(mseInside, 0, 'g', 12)
                  .arg(mseDifferent, 0, 'g', 12));
    }

    // G4: ProjectFile persists non-default timing and omits default timing keys.
    {
        ClipInfo defaultClip = makeClip(QStringLiteral("default.mp4"));
        defaultClip.effects.append(VideoEffect::createBlur(3.0));
        const QString defaultJson =
            ProjectFile::toJsonString(projectWithClip(defaultClip));

        ProjectData defaultLoaded;
        const bool defaultLoadOk =
            ProjectFile::fromJsonString(defaultJson, defaultLoaded);
        bool defaultRestored = defaultLoadOk
            && !defaultLoaded.videoTracks.isEmpty()
            && !defaultLoaded.videoTracks.first().isEmpty()
            && !defaultLoaded.videoTracks.first().first().effects.isEmpty()
            && near(defaultLoaded.videoTracks.first().first().effects.first().startSec, -1.0)
            && near(defaultLoaded.videoTracks.first().first().effects.first().endSec, -1.0);

        ClipInfo timedClip = makeClip(QStringLiteral("timed.mp4"));
        VideoEffect timed = VideoEffect::createBlur(5.0);
        timed.startSec = 1.25;
        timed.endSec = 2.75;
        timedClip.effects.append(timed);
        const QString timedJson =
            ProjectFile::toJsonString(projectWithClip(timedClip));
        ProjectData timedLoaded;
        const bool timedLoadOk =
            ProjectFile::fromJsonString(timedJson, timedLoaded);
        bool timedRestored = timedLoadOk
            && !timedLoaded.videoTracks.isEmpty()
            && !timedLoaded.videoTracks.first().isEmpty()
            && !timedLoaded.videoTracks.first().first().effects.isEmpty()
            && near(timedLoaded.videoTracks.first().first().effects.first().startSec, 1.25)
            && near(timedLoaded.videoTracks.first().first().effects.first().endSec, 2.75);

        const bool defaultKeysOmitted =
            !defaultJson.contains(QStringLiteral("\"startSec\""))
            && !defaultJson.contains(QStringLiteral("\"endSec\""));
        const bool timedKeysWritten =
            timedJson.contains(QStringLiteral("\"startSec\""))
            && timedJson.contains(QStringLiteral("\"endSec\""));

        check(4, "ProjectFile timing JSON round-trip and legacy omission",
              defaultRestored && timedRestored && defaultKeysOmitted && timedKeysWritten,
              QStringLiteral("defaultLoad=%1 timedLoad=%2 omit=%3 write=%4")
                  .arg(defaultLoadOk)
                  .arg(timedLoadOk)
                  .arg(defaultKeysOmitted)
                  .arg(timedKeysWritten));
    }

    qInfo().noquote()
        << QStringLiteral("EFFECT-TIMING: %1/%2 PASS")
               .arg(passed)
               .arg(passed + failed);
    return failed == 0 ? 0 : 1;
}
