#include "../Timeline.h"
#include "../UndoManager.h"
#include "../TimeRemap.h"
#include "../ProjectFile.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"

#include <cstddef>
#include <cstring>
#include <QColor>
#include <QDebug>
#include <QImage>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <cmath>

void syncTimeRemapEntriesToTimeline(
    Timeline *timeline,
    const QHash<QString, TimeRemapClipEntry> &source);

namespace {

constexpr double kEps = 1e-6;
constexpr int kClipW = 40;
constexpr int kClipH = 24;
constexpr int kFps = 10;
constexpr int kFrameCount = 30;

ClipInfo makeFreezeClip(const QString &name, double durationSec, double leadInSec = 0.0)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("/selftests/") + name + QStringLiteral(".mov");
    clip.displayName = name;
    clip.duration = durationSec;
    clip.outPoint = durationSec;
    clip.leadInSec = leadInSec;
    return clip;
}

bool near(double a, double b)
{
    return std::fabs(a - b) <= kEps;
}

QVector<double> clipStarts(const QVector<ClipInfo> &clips)
{
    QVector<double> starts;
    double cursor = 0.0;
    for (const ClipInfo &clip : clips) {
        const double start = cursor + clip.leadInSec;
        starts.append(start);
        cursor = start + clip.effectiveDuration();
    }
    return starts;
}

bool sameCurve(const timeremap::TimeRemapCurve &a,
               const timeremap::TimeRemapCurve &b)
{
    if (a.keys.size() != b.keys.size()
        || a.blendMode != b.blendMode
        || !near(a.sourceFps, b.sourceFps)) {
        return false;
    }
    for (int i = 0; i < a.keys.size(); ++i) {
        if (!near(a.keys[i].outTime, b.keys[i].outTime)
            || !near(a.keys[i].srcTime, b.keys[i].srcTime)) {
            return false;
        }
    }
    return true;
}

bool sameClip(const ClipInfo &a, const ClipInfo &b)
{
    return a.filePath == b.filePath
        && a.displayName == b.displayName
        && near(a.duration, b.duration)
        && near(a.inPoint, b.inPoint)
        && near(a.outPoint, b.outPoint)
        && near(a.leadInSec, b.leadInSec)
        && near(a.speed, b.speed)
        && sameCurve(a.timeRemapCurve, b.timeRemapCurve);
}

bool sameTracks(const QVector<QVector<ClipInfo>> &a,
                const QVector<QVector<ClipInfo>> &b)
{
    if (a.size() != b.size())
        return false;
    for (int track = 0; track < a.size(); ++track) {
        if (a[track].size() != b[track].size())
            return false;
        for (int clip = 0; clip < a[track].size(); ++clip) {
            if (!sameClip(a[track][clip], b[track][clip]))
                return false;
        }
    }
    return true;
}

bool sameState(const TimelineState &a, const TimelineState &b)
{
    return sameTracks(a.videoTracks, b.videoTracks)
        && sameTracks(a.audioTracks, b.audioTracks)
        && a.selectedClip == b.selectedClip
        && a.selectedVideoTrackIndex == b.selectedVideoTrackIndex
        && a.selectedVideoClipIndex == b.selectedVideoClipIndex
        && a.selectedAudioTrackIndex == b.selectedAudioTrackIndex
        && a.selectedAudioClipIndex == b.selectedAudioClipIndex
        && near(a.playheadPos, b.playheadPos)
        && a.projectWidth == b.projectWidth
        && a.projectHeight == b.projectHeight
        && a.projectExplicitOutput == b.projectExplicitOutput;
}

bool expect(bool condition, const char *gate, const QString &message,
            int &passed, int &failed)
{
    if (condition) {
        ++passed;
        qInfo().noquote() << "[freeze-frame] PASS" << gate;
        return true;
    }
    ++failed;
    qWarning().noquote() << "[freeze-frame] FAIL" << gate << ":" << message;
    return false;
}

void resetUndoBaseline(Timeline &timeline, const QString &description)
{
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), description);
}

QImage solidFrame(const QColor &color)
{
    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}

QImage encodedFrame(int frameIndex)
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            frame.setPixelColor(x, y, QColor((x * 7 + frameIndex * 43) & 255,
                                             (y * 11 + frameIndex * 61) & 255,
                                             (x * 5 + y * 13 + frameIndex * 29) & 255));
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
            *error = QStringLiteral("FrameEncoder::open failed: ")
                + QString::fromStdString(*err);
        return false;
    }

    for (int i = 0; i < kFrameCount; ++i) {
        if (!encoder.pushFrame(encodedFrame(i), i)) {
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

ClipInfo makeRenderClip(const QString &path, const QString &name)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = name;
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
}

bool equalRgbaBytes(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.isNull() || bb.isNull())
        return aa.isNull() == bb.isNull();
    if (aa.size() != bb.size())
        return false;

    for (int y = 0; y < aa.height(); ++y) {
        if (std::memcmp(aa.constScanLine(y), bb.constScanLine(y),
                        static_cast<std::size_t>(aa.width() * 4)) != 0) {
            return false;
        }
    }
    return true;
}

double rgbaMse(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.isNull() || bb.isNull() || aa.size() != bb.size())
        return 1.0e99;

    double sum = 0.0;
    const double denom = static_cast<double>(aa.width()) * aa.height() * 4.0;
    for (int y = 0; y < aa.height(); ++y) {
        const uchar *pa = aa.constScanLine(y);
        const uchar *pb = bb.constScanLine(y);
        for (int x = 0; x < aa.width() * 4; ++x) {
            const double d = static_cast<double>(pa[x]) - static_cast<double>(pb[x]);
            sum += d * d;
        }
    }
    return denom > 0.0 ? sum / denom : 1.0e99;
}

QHash<QString, TimeRemapClipEntry> timeRemapEntryMap(
    const QVector<TimeRemapClipEntry> &entries)
{
    QHash<QString, TimeRemapClipEntry> map;
    map.reserve(entries.size());
    for (const TimeRemapClipEntry &entry : entries) {
        if (!entry.clipId.isEmpty())
            map.insert(entry.clipId, entry);
    }
    return map;
}

ProjectData projectDataForTimelineFreeze(const Timeline &timeline)
{
    ProjectData data;
    data.config.width = kClipW;
    data.config.height = kClipH;
    data.config.fps = kFps;
    data.videoTracks = timeline.allVideoTracks();
    data.audioTracks = timeline.allAudioTracks();
    data.playheadPos = timeline.playheadPosition();

    for (int trackIdx = 0; trackIdx < data.videoTracks.size(); ++trackIdx) {
        const QVector<ClipInfo> &clips = data.videoTracks[trackIdx];
        for (int clipIdx = 0; clipIdx < clips.size(); ++clipIdx) {
            if (!clips[clipIdx].hasTimeRemap())
                continue;
            TimeRemapClipEntry entry;
            entry.clipId = trackMatteClipKey(trackIdx, clipIdx);
            entry.curve = clips[clipIdx].timeRemapCurve;
            data.timeRemapClipEntries.append(entry);
        }
    }
    return data;
}

bool renderedHoldFramesMatch(const Timeline &timeline)
{
    const QSize outSize(kClipW, kClipH);
    const QImage a = tlrender::renderFrameAt(&timeline, 1100000, outSize);
    const QImage b = tlrender::renderFrameAt(&timeline, 2200000, outSize);
    return !a.isNull() && equalRgbaBytes(a, b) && near(rgbaMse(a, b), 0.0);
}

} // namespace

int runFreezeFrameSelftest()
{
    qInfo().noquote() << "[freeze-frame] selftest start";
    int passed = 0;
    int failed = 0;

    Timeline timeline;
    timeline.videoTracks()[0]->setClips({
        makeFreezeClip(QStringLiteral("source-a"), 10.0),
        makeFreezeClip(QStringLiteral("downstream"), 3.0, 2.0),
    });
    timeline.videoTracks()[0]->setSelectedClip(0);
    timeline.setPlayheadPosition(4.0);
    const TimelineState before = timeline.currentState();
    resetUndoBaseline(timeline, QStringLiteral("freeze baseline"));

    const bool applied = timeline.freezeFrameAtPlayhead(timeline.videoTracks()[0], 0);
    const QVector<ClipInfo> clips = timeline.videoTracks()[0]->clips();
    const QVector<double> starts = clipStarts(clips);
    const QVector<PlaybackEntry> playback = timeline.computePlaybackSequence();

    const bool splitAndHeld = applied
        && clips.size() == 3
        && playback.size() == 3
        && clips[0].displayName == QStringLiteral("source-a")
        && clips[1].displayName == QStringLiteral("source-a")
        && clips[2].displayName == QStringLiteral("downstream")
        && near(starts[0], 0.0)
        && near(starts[1], 4.0)
        && near(starts[2], 12.0)
        && near(clips[0].outPoint, 4.0)
        && near(clips[1].inPoint, 4.0)
        && near(clips[1].outPoint, 10.0)
        && clips[1].hasTimeRemap()
        && clips[1].timeRemapCurve.keys.size() == 1
        && near(clips[1].timeRemapCurve.keys[0].outTime, 0.0)
        && near(clips[1].timeRemapCurve.keys[0].srcTime, 0.0)
        && near(playback.value(1).timelineStart, 4.0)
        && near(playback.value(1).timelineEnd, 10.0)
        && near(playback.value(1).clipIn, 4.0)
        && playback.value(1).speed < 1.0e-6;
    expect(splitAndHeld, "G1 split + hold curve",
           QStringLiteral("freeze did not create a split hold segment with one constant key"),
           passed, failed);

    QVector<int> fetched;
    const QVector<QImage> frames = {
        solidFrame(QColor(255, 0, 0)),
        solidFrame(QColor(0, 255, 0)),
        solidFrame(QColor(0, 0, 255)),
    };
    const QImage heldFrame = timeremap::resolveFrame(
        clips.value(1).timeRemapCurve,
        qMax(0.0, clips.value(1).effectiveDuration() - 0.001),
        [&fetched, frames](int index) -> QImage {
            fetched.append(index);
            if (index < 0 || index >= frames.size())
                return {};
            return frames[index];
        });
    const bool boundaryFrame = !heldFrame.isNull()
        && fetched.size() == 1
        && fetched[0] == 0
        && qRed(heldFrame.pixel(0, 0)) > 200
        && qGreen(heldFrame.pixel(0, 0)) < 32
        && near(clips.value(1).inPoint + clips.value(1).timeRemapCurve.srcTimeAt(0.0), 4.0)
        && near(clips.value(1).inPoint + clips.value(1).timeRemapCurve.srcTimeAt(999.0), 4.0);
    expect(boundaryFrame, "G2 boundary frame resolves to split frame",
           QStringLiteral("hold curve did not resolve the split-boundary frame"),
           passed, failed);

    QTemporaryDir tmpDir;
    QString mediaError;
    const QString mediaPath = tmpDir.isValid()
        ? tmpDir.filePath(QStringLiteral("freeze_frame_src.mp4"))
        : QString();
    const bool mediaReady = tmpDir.isValid() && writeSyntheticClip(mediaPath, &mediaError);

    Timeline renderTimeline;
    bool renderApplied = false;
    bool v1RenderHold = false;
    if (mediaReady) {
        renderTimeline.videoTracks()[0]->setClips({
            makeRenderClip(mediaPath, QStringLiteral("render-freeze")),
        });
        renderTimeline.setPlayheadPosition(1.0);
        renderApplied = renderTimeline.freezeFrameAtPlayhead(renderTimeline.videoTracks()[0], 0);
        v1RenderHold = renderApplied && renderedHoldFramesMatch(renderTimeline);
    }
    expect(v1RenderHold, "G3 renderFrameAt V1 hold byte-identical/MSE0",
           mediaReady
               ? QStringLiteral("V1 renderFrameAt did not hold the split source frame")
               : QStringLiteral("synthetic render media failed: %1").arg(mediaError),
           passed, failed);

    Timeline overlayTimeline;
    bool overlayRenderHold = false;
    if (mediaReady) {
        overlayTimeline.videoTracks()[0]->setHidden(true);
        overlayTimeline.addVideoTrack();
        TimelineTrack *overlayTrack = overlayTimeline.videoTracks().value(1, nullptr);
        if (overlayTrack) {
            overlayTrack->setClips({
                makeRenderClip(mediaPath, QStringLiteral("overlay-freeze")),
            });
            overlayTimeline.setPlayheadPosition(1.0);
            const bool overlayApplied =
                overlayTimeline.freezeFrameAtPlayhead(overlayTrack, 0);
            overlayRenderHold = overlayApplied && renderedHoldFramesMatch(overlayTimeline);
        }
    }
    expect(overlayRenderHold, "G4 renderFrameAt overlay hold byte-identical/MSE0",
           mediaReady
               ? QStringLiteral("overlay renderFrameAt did not hold the split source frame")
               : QStringLiteral("synthetic render media failed: %1").arg(mediaError),
           passed, failed);

    bool saveLoadRoundtrip = false;
    if (renderApplied) {
        ProjectData saved = projectDataForTimelineFreeze(renderTimeline);
        ProjectData loaded;
        Timeline loadedTimeline;
        const bool loadedOk =
            ProjectFile::fromJsonString(ProjectFile::toJsonString(saved), loaded);
        if (loadedOk) {
            loadedTimeline.restoreFromProject(loaded.videoTracks, loaded.audioTracks,
                                              loaded.playheadPos, loaded.markIn,
                                              loaded.markOut, loaded.zoomLevel);
            syncTimeRemapEntriesToTimeline(
                &loadedTimeline, timeRemapEntryMap(loaded.timeRemapClipEntries));
            const QVector<ClipInfo> loadedClips = loadedTimeline.videoTracks()[0]->clips();
            saveLoadRoundtrip =
                loaded.timeRemapClipEntries.size() == 1
                && loadedClips.size() == 2
                && loadedClips[1].hasTimeRemap()
                && renderedHoldFramesMatch(loadedTimeline);
        }
    }
    expect(saveLoadRoundtrip, "G5 save/load sync keeps freeze active",
           QStringLiteral("ProjectFile round-trip did not restore the hold curve onto Timeline clips"),
           passed, failed);

    timeline.undo();
    expect(sameState(before, timeline.currentState()) && !timeline.canUndo(),
           "G6 one undo restores",
           QStringLiteral("single undo did not fully restore the pre-freeze state"),
           passed, failed);

    timeremap::TimeRemapCurve identity;
    timeremap::TimeRemapCurve constant;
    constant.addKey(0.0, 2.0);
    timeremap::TimeRemapCurve linear;
    linear.sourceFps = 1.0;
    linear.addKey(0.0, 0.0);
    linear.addKey(2.0, 2.0);
    const QImage nearest = timeremap::resolveFrame(
        linear, 0.1, [frames](int index) -> QImage {
            if (index < 0 || index >= frames.size())
                return {};
            return frames[index];
        });
    linear.blendMode = timeremap::FrameBlendMode::Blend;
    const QImage blended = timeremap::resolveFrame(
        linear, 0.5, [frames](int index) -> QImage {
            if (index < 0 || index >= frames.size())
                return {};
            return frames[index];
        });
    const bool timeremapRegression =
        near(identity.srcTimeAt(1.25), 1.25)
        && near(constant.srcTimeAt(-10.0), 2.0)
        && near(constant.srcTimeAt(10.0), 2.0)
        && near(linear.srcTimeAt(1.5), 1.5)
        && !nearest.isNull()
        && qRed(nearest.pixel(0, 0)) > 200
        && !blended.isNull()
        && qRed(blended.pixel(0, 0)) > 32
        && qGreen(blended.pixel(0, 0)) > 32;
    expect(timeremapRegression, "G7 timeremap regression",
           QStringLiteral("TimeRemapCurve identity/constant/interp/resolveFrame regression failed"),
           passed, failed);

    qInfo().noquote() << "[freeze-frame] summary:"
                      << passed << "PASS," << failed << "FAIL";
    return failed == 0 ? 0 : 1;
}
