// src/selftests/AdjustmentLayerSelftest.cpp
// ADJ-1: ClipInfo::isAdjustment render/serialization gates.

#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoEffect.h"
#include "../libavcore/Encode.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QTemporaryDir>

namespace {

constexpr int kClipW = 64;
constexpr int kClipH = 48;
constexpr int kFps = 12;
constexpr int kFrameCount = 18;
constexpr double kDuration = static_cast<double>(kFrameCount) / kFps;

QImage solidFrame(const QColor &color)
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    frame.fill(color);
    return frame;
}

bool writeSolidClip(const QString &path, const QColor &color, QString *error)
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
            *error = QStringLiteral("FrameEncoder::open failed: ")
                + QString::fromStdString(*err);
        return false;
    }

    const QImage frame = solidFrame(color);
    for (int i = 0; i < kFrameCount; ++i) {
        if (!encoder.pushFrame(frame, i)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrame failed at frame %1").arg(i);
            return false;
        }
    }

    if (auto err = encoder.finalize()) {
        if (error)
            *error = QStringLiteral("FrameEncoder::finalize failed: ")
                + QString::fromStdString(*err);
        return false;
    }
    return true;
}

ClipInfo makeMediaClip(const QString &path, const QString &name)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = name;
    clip.duration = kDuration;
    clip.inPoint = 0.0;
    clip.outPoint = kDuration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
}

ClipInfo makeAdjustmentClip(const QString &name, const VideoEffect &effect)
{
    ClipInfo clip;
    clip.displayName = name;
    clip.duration = kDuration;
    clip.inPoint = 0.0;
    clip.outPoint = kDuration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    clip.isAdjustment = true;
    clip.effects.append(effect);
    return clip;
}

bool setTimelineClip(Timeline &timeline, int trackIndex, const ClipInfo &clip,
                     QString *error)
{
    while (timeline.videoTracks().size() <= trackIndex)
        timeline.addVideoTrack();

    TimelineTrack *track = timeline.videoTracks().value(trackIndex, nullptr);
    if (!track) {
        if (error)
            *error = QStringLiteral("missing V%1").arg(trackIndex + 1);
        return false;
    }

    track->setClips(QVector<ClipInfo>{clip});
    return true;
}

QImage renderRgba(const Timeline &timeline, qint64 usec)
{
    return tlrender::renderFrameAt(&timeline, usec, QSize(kClipW, kClipH))
        .convertToFormat(QImage::Format_RGBA8888);
}

bool equalRgbaBytes(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.isNull() || bb.isNull() || aa.size() != bb.size())
        return false;

    for (int y = 0; y < aa.height(); ++y) {
        if (std::memcmp(aa.constScanLine(y), bb.constScanLine(y),
                        static_cast<std::size_t>(aa.width() * 4)) != 0) {
            return false;
        }
    }
    return true;
}

bool colorNear(const QColor &a, const QColor &b, int tolerance = 1)
{
    return std::abs(a.red() - b.red()) <= tolerance
        && std::abs(a.green() - b.green()) <= tolerance
        && std::abs(a.blue() - b.blue()) <= tolerance
        && std::abs(a.alpha() - b.alpha()) <= tolerance;
}

QColor invertedColor(const QColor &color)
{
    return QColor(255 - color.red(),
                  255 - color.green(),
                  255 - color.blue(),
                  color.alpha());
}

ProjectData projectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    return data;
}

} // namespace

int runAdjustmentLayerSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[adjustment-layer] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    QTemporaryDir tempDir;
    check(0, "temporary directory", tempDir.isValid());
    if (!tempDir.isValid())
        return 1;

    QString error;
    const QString redPath = tempDir.filePath(QStringLiteral("adj_red.mp4"));
    const QString bluePath = tempDir.filePath(QStringLiteral("adj_blue.mp4"));
    const bool mediaOk =
        writeSolidClip(redPath, QColor(220, 24, 24), &error)
        && writeSolidClip(bluePath, QColor(24, 64, 220), &error);
    check(0, "synthetic source clips", mediaOk, error);
    if (!mediaOk)
        return 1;

    const ClipInfo red = makeMediaClip(redPath, QStringLiteral("front-red"));
    const ClipInfo blue = makeMediaClip(bluePath, QStringLiteral("back-blue"));
    const qint64 insideUsec = 500000;
    const qint64 outsideUsec = 900000;
    const QPoint frontPixel(kClipW / 2, kClipH / 2);
    const QPoint backPixel(4, 4);

    // G1: adding an adjustment clip must not flip unrelated V1/V2 occlusion.
    {
        Timeline plain;
        Timeline adjusted;
        bool ok = setTimelineClip(plain, 0, red, &error)
            && setTimelineClip(plain, 1, blue, &error)
            && setTimelineClip(adjusted, 0, red, &error)
            && setTimelineClip(adjusted, 1, blue, &error);

        VideoEffect invert = VideoEffect::createInvert();
        const ClipInfo backmostAdjustment =
            makeAdjustmentClip(QStringLiteral("backmost-adjustment"), invert);
        ok = ok && setTimelineClip(adjusted, 2, backmostAdjustment, &error);

        const QImage plainFrame = ok ? renderRgba(plain, insideUsec) : QImage();
        const QImage adjustedFrame = ok ? renderRgba(adjusted, insideUsec) : QImage();
        ok = ok && equalRgbaBytes(plainFrame, adjustedFrame)
            && colorNear(adjustedFrame.pixelColor(frontPixel),
                         plainFrame.pixelColor(frontPixel), 0);
        check(1, "V1/V2 z-order unchanged when adjustment is present", ok, error);
    }

    // G2/G3: an adjustment between V1 and V3 affects V3 only, within its range.
    {
        ClipInfo front = red;
        front.videoScale = 0.5;

        VideoEffect rangedInvert = VideoEffect::createInvert();
        rangedInvert.startSec = 0.25;
        rangedInvert.endSec = 0.75;
        const ClipInfo adjustment =
            makeAdjustmentClip(QStringLiteral("middle-adjustment"), rangedInvert);

        Timeline plain;
        Timeline adjusted;
        bool ok = setTimelineClip(plain, 0, front, &error)
            && setTimelineClip(plain, 2, blue, &error)
            && setTimelineClip(adjusted, 0, front, &error)
            && setTimelineClip(adjusted, 1, adjustment, &error)
            && setTimelineClip(adjusted, 2, blue, &error);

        const QImage plainFrame = ok ? renderRgba(plain, insideUsec) : QImage();
        const QImage inside = ok ? renderRgba(adjusted, insideUsec) : QImage();
        const QImage outside = ok ? renderRgba(adjusted, outsideUsec) : QImage();
        ok = ok && !plainFrame.isNull() && !inside.isNull() && !outside.isNull();

        QColor plainBack;
        QColor insideBack;
        QColor outsideBack;
        if (ok) {
            plainBack = plainFrame.pixelColor(backPixel);
            insideBack = inside.pixelColor(backPixel);
            outsideBack = outside.pixelColor(backPixel);
        }
        const bool applyOk = ok
            && colorNear(insideBack, invertedColor(outsideBack), 1)
            && colorNear(outsideBack, plainBack, 1)
            && !colorNear(insideBack, outsideBack, 1);
        check(2, "adjustment modifies only layers behind it during active range",
              applyOk,
              QStringLiteral("plain=%1,%2,%3 inside=%4,%5,%6 outside=%7,%8,%9")
                  .arg(plainBack.red()).arg(plainBack.green()).arg(plainBack.blue())
                  .arg(insideBack.red()).arg(insideBack.green()).arg(insideBack.blue())
                  .arg(outsideBack.red()).arg(outsideBack.green()).arg(outsideBack.blue()));

        const bool frontOk = ok
            && colorNear(inside.pixelColor(frontPixel),
                         plainFrame.pixelColor(frontPixel), 1);
        check(3, "front layer is not affected by adjustment behind it", frontOk);
    }

    // G4: adjustment-free renderFrameAt stays byte-identical to the decode oracle.
    {
        Timeline timeline;
        bool ok = setTimelineClip(timeline, 0, red, &error);
        const QImage rendered = ok ? renderRgba(timeline, 0) : QImage();
        const QImage decoded =
            tlrender::detail::decodeClipFrameNativeForTest(redPath, 0.0)
                .convertToFormat(QImage::Format_RGBA8888)
                .scaled(QSize(kClipW, kClipH), Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation);
        ok = ok && equalRgbaBytes(rendered, decoded);
        check(4, "unused adjustment path is byte-identical", ok, error);
    }

    // G5: ProjectFile true-only persistence and old-project default omission.
    {
        const QString legacyJson = ProjectFile::toJsonString(projectWithClip(red));
        const bool omitted = !legacyJson.contains(QStringLiteral("\"isAdjustment\""));

        VideoEffect invert = VideoEffect::createInvert();
        const ClipInfo adjustment =
            makeAdjustmentClip(QStringLiteral("serial-adjustment"), invert);
        const QString adjustmentJson =
            ProjectFile::toJsonString(projectWithClip(adjustment));

        ProjectData loaded;
        const bool loadedOk = ProjectFile::fromJsonString(adjustmentJson, loaded);
        const bool survives = loadedOk
            && loaded.videoTracks.size() == 1
            && loaded.videoTracks[0].size() == 1
            && loaded.videoTracks[0][0].isAdjustment;

        check(5, "serialization omits false and round-trips true",
              omitted
                  && adjustmentJson.contains(QStringLiteral("\"isAdjustment\":true"))
                  && survives,
              QStringLiteral("omitted=%1 survives=%2")
                  .arg(omitted ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg(survives ? QStringLiteral("true") : QStringLiteral("false")));
    }

    std::printf("[adjustment-layer] summary: passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
