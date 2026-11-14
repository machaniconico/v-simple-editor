#include "../GLPreview.h"
#include "../RotoClipEntry.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"
#include "SelftestGl.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryDir>

namespace {

constexpr int kClipW = 48;
constexpr int kClipH = 32;
constexpr int kFps = 12;
constexpr int kFrameCount = 12;

QImage makePatternFrame(int frameIndex)
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            frame.setPixelColor(x, y,
                                QColor((x * 11 + frameIndex * 17) & 255,
                                       (y * 13 + frameIndex * 19) & 255,
                                       (x * 5 + y * 7 + frameIndex * 23) & 255));
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
    req.videoBitrateBits = 700000;
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
        if (!encoder.pushFrame(makePatternFrame(i), i)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrame failed at %1").arg(i);
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
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.outPoint = clip.duration;
    clip.opacity = 1.0;
    return clip;
}

ClipInfo makeSequenceRef(const QString &id, double duration)
{
    ClipInfo clip;
    clip.sequenceRefId = id;
    clip.filePath = timeline_nesting::sequenceClipFilePath(id);
    clip.displayName = id;
    clip.duration = duration;
    clip.outPoint = duration;
    clip.opacity = 1.0;
    return clip;
}

TimelineSequence makeSequence(const QString &id,
                              const QVector<QVector<ClipInfo>> &videoTracks,
                              const QVector<QVector<ClipInfo>> &audioTracks =
                                  QVector<QVector<ClipInfo>>())
{
    TimelineSequence sequence;
    sequence.id = id;
    sequence.name = id;
    sequence.videoTracks = videoTracks;
    sequence.audioTracks = audioTracks;
    return sequence;
}

QVector<QVector<ClipInfo>> oneTrack(const ClipInfo &clip)
{
    return QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
}

bool equalRgbaBytes(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.size() != bb.size())
        return false;
    for (int y = 0; y < aa.height(); ++y) {
        if (std::memcmp(aa.constScanLine(y), bb.constScanLine(y),
                        static_cast<std::size_t>(aa.width() * 4)) != 0)
            return false;
    }
    return true;
}

bool isTransparent(const QImage &image)
{
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull())
        return false;
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *row = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            if (row[x * 4 + 3] != 0)
                return false;
        }
    }
    return true;
}

bool containsSequenceId(const QJsonObject &store, const QString &id)
{
    const QJsonArray arr = store.value(QStringLiteral("sequences")).toArray();
    for (const QJsonValue &value : arr) {
        if (value.toObject().value(QStringLiteral("id")).toString() == id)
            return true;
    }
    return false;
}

bool containsSequenceId(const QVector<TimelineSequence> &sequences, const QString &id)
{
    for (const TimelineSequence &sequence : sequences) {
        if (sequence.id == id)
            return true;
    }
    return false;
}

QString uniquePrecomposeSequenceIdForTest(const Timeline &timeline)
{
    const QVector<TimelineSequence> sequences = timeline.sequences();
    for (int i = 1; i < 100000; ++i) {
        const QString candidate = QStringLiteral("precomp-%1").arg(i);
        if (!containsSequenceId(sequences, candidate))
            return candidate;
    }
    return QStringLiteral("precomp-overflow");
}

QString firstPrecomposeSequenceId(const QVector<TimelineSequence> &sequences)
{
    for (const TimelineSequence &sequence : sequences) {
        if (sequence.id.startsWith(QStringLiteral("precomp-")))
            return sequence.id;
    }
    return {};
}

TimelineSequence snapshotActiveTracksAsSequence(const Timeline &timeline,
                                                const QString &id,
                                                const QString &name)
{
    TimelineSequence sequence;
    sequence.id = id;
    sequence.name = name;
    sequence.videoTracks.reserve(timeline.videoTracks().size());
    for (const TimelineTrack *track : timeline.videoTracks())
        sequence.videoTracks.append(track ? track->clips() : QVector<ClipInfo>{});
    sequence.audioTracks.reserve(timeline.audioTracks().size());
    for (const TimelineTrack *track : timeline.audioTracks())
        sequence.audioTracks.append(track ? track->clips() : QVector<ClipInfo>{});
    return sequence;
}

void replaceParentTracksWithSequenceRef(Timeline &timeline, const ClipInfo &sequenceClip)
{
    for (int i = 0; i < timeline.videoTracks().size(); ++i) {
        if (TimelineTrack *track = timeline.videoTracks().value(i, nullptr))
            track->setClips(i == 0 ? QVector<ClipInfo>{sequenceClip}
                                   : QVector<ClipInfo>{});
    }
    for (int i = 0; i < timeline.audioTracks().size(); ++i) {
        if (TimelineTrack *track = timeline.audioTracks().value(i, nullptr))
            track->setClips(i == 0 ? QVector<ClipInfo>{sequenceClip}
                                   : QVector<ClipInfo>{});
    }
}

bool simulatePrecomposeAndSave(Timeline &timeline,
                               const QString &name,
                               QString *sequenceIdOut,
                               QString *detail)
{
    if (!timeline.undoManager()) {
        if (detail)
            *detail = QStringLiteral("undo manager unavailable");
        return false;
    }

    const QString sequenceId = uniquePrecomposeSequenceIdForTest(timeline);
    TimelineSequence sequence =
        snapshotActiveTracksAsSequence(timeline, sequenceId, name);
    if (!timeline.addSequence(sequence)) {
        if (detail)
            *detail = QStringLiteral("addSequence failed for %1").arg(sequenceId);
        return false;
    }

    const QVector<TimelineSequence> afterAdd = timeline.sequences();
    if (!containsSequenceId(afterAdd, sequenceId)) {
        if (detail)
            *detail = QStringLiteral("addSequence did not retain %1").arg(sequenceId);
        return false;
    }

    ClipInfo sequenceClip = timeline.makeSequenceClip(sequenceId, name);
    if (sequenceClip.duration <= 0.0) {
        if (detail)
            *detail = QStringLiteral("sequence clip %1 has non-positive duration").arg(sequenceId);
        return false;
    }
    replaceParentTracksWithSequenceRef(timeline, sequenceClip);

    timeline.undoManager()->saveState(
        timeline.currentState(),
        QStringLiteral("Pre-compose selftest: %1").arg(name));
    if (sequenceIdOut)
        *sequenceIdOut = sequenceId;
    return true;
}

bool preparePrecomposeUndoTimeline(Timeline &timeline,
                                   const ClipInfo &media,
                                   int *baselineCount,
                                   QString *detail)
{
    const TimelineSequence main =
        makeSequence(QStringLiteral("main"), oneTrack(media), oneTrack(media));
    timeline.setSequences(QVector<TimelineSequence>{main}, QStringLiteral("main"));

    const QVector<TimelineSequence> baselineSequences = timeline.sequences();
    const QString leaked = firstPrecomposeSequenceId(baselineSequences);
    if (!leaked.isEmpty()) {
        if (detail)
            *detail = QStringLiteral("baseline unexpectedly contains %1").arg(leaked);
        return false;
    }
    if (baselineSequences.isEmpty()) {
        if (detail)
            *detail = QStringLiteral("sequence-model baseline is empty");
        return false;
    }

    if (!timeline.undoManager()) {
        if (detail)
            *detail = QStringLiteral("undo manager unavailable");
        return false;
    }
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(),
                                      QStringLiteral("precompose undo baseline"));
    if (baselineCount)
        *baselineCount = baselineSequences.size();
    return true;
}

bool assertNoPrecomposeStoreLeak(const Timeline &timeline,
                                 int baselineCount,
                                 QString *detail)
{
    const QVector<TimelineSequence> restored = timeline.sequences();
    if (restored.size() != baselineCount) {
        if (detail) {
            *detail = QStringLiteral("sequence count after undo is %1, expected %2")
                .arg(restored.size())
                .arg(baselineCount);
        }
        return false;
    }

    const QString leaked = firstPrecomposeSequenceId(restored);
    if (!leaked.isEmpty()) {
        if (detail)
            *detail = QStringLiteral("%1 remained after undo").arg(leaked);
        return false;
    }
    return true;
}

bool singlePrecomposeUndoClearsSequenceStore(const ClipInfo &media, QString *detail)
{
    Timeline timeline;
    int baselineCount = 0;
    if (!preparePrecomposeUndoTimeline(timeline, media, &baselineCount, detail))
        return false;

    QString sequenceId;
    if (!simulatePrecomposeAndSave(timeline,
                                   QStringLiteral("Comp 1"),
                                   &sequenceId,
                                   detail)) {
        return false;
    }
    if (!containsSequenceId(timeline.sequences(), sequenceId)) {
        if (detail)
            *detail = QStringLiteral("%1 missing before undo").arg(sequenceId);
        return false;
    }

    timeline.undo();
    return assertNoPrecomposeStoreLeak(timeline, baselineCount, detail);
}

bool doublePrecomposeUndoClearsSequenceStore(const ClipInfo &media, QString *detail)
{
    Timeline timeline;
    int baselineCount = 0;
    if (!preparePrecomposeUndoTimeline(timeline, media, &baselineCount, detail))
        return false;

    QString firstSequenceId;
    if (!simulatePrecomposeAndSave(timeline,
                                   QStringLiteral("Comp 1"),
                                   &firstSequenceId,
                                   detail)) {
        return false;
    }

    QString secondSequenceId;
    if (!simulatePrecomposeAndSave(timeline,
                                   QStringLiteral("Comp 2"),
                                   &secondSequenceId,
                                   detail)) {
        return false;
    }
    if (firstSequenceId == secondSequenceId) {
        if (detail)
            *detail = QStringLiteral("second precompose reused %1").arg(firstSequenceId);
        return false;
    }
    if (!containsSequenceId(timeline.sequences(), firstSequenceId)
        || !containsSequenceId(timeline.sequences(), secondSequenceId)) {
        if (detail) {
            *detail = QStringLiteral("precompose sequences missing before undo: %1, %2")
                .arg(firstSequenceId, secondSequenceId);
        }
        return false;
    }

    timeline.undo();
    timeline.undo();
    return assertNoPrecomposeStoreLeak(timeline, baselineCount, detail);
}

// US-2305 (G15-G16): a nested child's parked roto / matte / parent sidecars. Static
// 160x90 clips (every frame equal), so a shown frame is comparable with the export at
// any frame of the same clip and only the sidecar and the clip change the picture.
constexpr int kSideW = 160;
constexpr int kSideH = 90;
constexpr int kSideFps = 12;
constexpr double kSideClipSec = 1.0;
constexpr qint64 kSideFrameUs = 1000000 / kSideFps;

bool writeSideClip(const QString &path, int blue, QString *error)
{
    QImage frame(kSideW, kSideH, QImage::Format_RGB888);
    for (int y = 0; y < kSideH; ++y)
        for (int x = 0; x < kSideW; ++x)
            frame.setPixelColor(x, y, QColor(40 + x, 20 + y * 2, blue));
    libavcore::EncodeRequest req;
    req.width = kSideW;
    req.height = kSideH;
    req.fps = kSideFps;
    req.fpsNum = kSideFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 4000000;
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
    for (int i = 0; i < kSideFps; ++i) {
        if (!encoder.pushFrameRgb24(frame.constBits(), frame.bytesPerLine(), i)) {
            if (error)
                *error = QStringLiteral("pushFrameRgb24 failed at %1").arg(i);
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

struct SideLayout {
    int tracks = 1;
    double scale[3] = {1.0, 1.0, 1.0};
    double dx[3] = {0.0, 0.0, 0.0};
    double dy[3] = {0.0, 0.0, 0.0};
};

// Parent P (V1 = a reference to C, 2 s) with C active nowhere: C's sidecars are parked.
// Each track of C holds clip 0 (media A, 0-1 s) and clip 1 (media B, 1-2 s).
void buildSideTimeline(Timeline &tl, const QString &pathA, const QString &pathB,
                       const SideLayout &layout)
{
    QVector<QVector<ClipInfo>> childVideo;
    for (int t = 0; t < layout.tracks; ++t) {
        QVector<ClipInfo> track;
        for (const QString &path : {pathA, pathB}) {
            ClipInfo clip = makeMediaClip(path, QStringLiteral("side"));
            clip.duration = kSideClipSec;
            clip.outPoint = kSideClipSec;
            clip.videoScale = layout.scale[t];
            clip.videoDx = layout.dx[t];
            clip.videoDy = layout.dy[t];
            track.append(clip);
        }
        childVideo.append(track);
    }
    const TimelineSequence child = makeSequence(QStringLiteral("side-c"), childVideo);
    const TimelineSequence parent =
        makeSequence(QStringLiteral("side-p"),
                     oneTrack(makeSequenceRef(QStringLiteral("side-c"), 2.0 * kSideClipSec)));
    tl.setProjectOutputConfig(kSideW, kSideH, false, kSideFps);
    tl.setSequences(QVector<TimelineSequence>{parent, child}, QStringLiteral("side-p"));
}

using SideSidecars = QHash<QString, Timeline::SequenceSidecars>;

SideSidecars sideParked(const Timeline::SequenceSidecars &sidecars)
{
    return SideSidecars{{QStringLiteral("side-c"), sidecars}};
}

// A left-aligned rectangle `width` wide over the full height, on both clips of V1.
Timeline::SequenceSidecars sideRoto(double width)
{
    Timeline::SequenceSidecars out;
    for (int c = 0; c < 2; ++c) {
        RotoClipEntry roto;
        roto.clipId = QStringLiteral("0:%1").arg(c);
        for (const QPointF &p : {QPointF(0, 0), QPointF(width, 0), QPointF(width, kSideH),
                                 QPointF(0, kSideH)}) {
            RotoPoint point;
            point.position = p;
            point.handleIn = p;
            point.handleOut = p;
            roto.path.points.append(point);
        }
        out.roto.insert(roto.clipId, roto);
    }
    return out;
}

// V2 (clip "1:c") matted by V3 (clip "2:c") on both clips.
Timeline::SequenceSidecars sideMatte(TrackMatteType type)
{
    Timeline::SequenceSidecars out;
    for (int c = 0; c < 2; ++c) {
        TimelineTrackMatteEntry entry;
        entry.matteType = type;
        entry.matteSourceClipId = QStringLiteral("2:%1").arg(c);
        out.matte.insert(QStringLiteral("1:%1").arg(c), entry);
    }
    return out;
}

// v3ToV2: V3 follows V2; otherwise V2 follows V3.
Timeline::SequenceSidecars sideParent(bool v3ToV2)
{
    Timeline::SequenceSidecars out;
    for (int c = 0; c < 2; ++c) {
        const QString v2 = QStringLiteral("1:%1").arg(c);
        const QString v3 = QStringLiteral("2:%1").arg(c);
        out.parent.insert(v3ToV2 ? v3 : v2, v3ToV2 ? v2 : v3);
    }
    return out;
}

int sideMaxDiff(const QImage &a, const QImage &b)
{
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage y = b.convertToFormat(QImage::Format_RGBA8888);
    if (x.isNull() || y.isNull() || x.size() != y.size())
        return 999;
    int worst = 0;
    for (int row = 0; row < x.height(); ++row) {
        const uchar *p = x.constScanLine(row);
        const uchar *q = y.constScanLine(row);
        for (int i = 0; i < x.width() * 4; ++i)
            worst = qMax(worst, std::abs(int(p[i]) - int(q[i])));
    }
    return worst;
}

// Pixels whose RGBA differ by more than 1 in any channel (-1: sizes differ).
int sideDiffPixels(const QImage &a, const QImage &b, const QRect &region = QRect())
{
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage y = b.convertToFormat(QImage::Format_RGBA8888);
    if (x.isNull() || y.isNull() || x.size() != y.size())
        return -1;
    const QRect r = region.isNull() ? x.rect() : region.intersected(x.rect());
    int n = 0;
    for (int row = r.top(); row <= r.bottom(); ++row) {
        const uchar *p = x.constScanLine(row);
        const uchar *q = y.constScanLine(row);
        for (int col = r.left(); col <= r.right(); ++col) {
            for (int ch = 0; ch < 4; ++ch) {
                if (std::abs(int(p[col * 4 + ch]) - int(q[col * 4 + ch])) > 1) {
                    ++n;
                    break;
                }
            }
        }
    }
    return n;
}

// Pixels in the region where material remains: alpha > 1 and any of R / G / B > 1.
int sideMaterialPixels(const QImage &a, const QRect &region)
{
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888);
    if (x.isNull())
        return -1;
    const QRect r = region.intersected(x.rect());
    int n = 0;
    for (int row = r.top(); row <= r.bottom(); ++row) {
        const uchar *p = x.constScanLine(row);
        for (int col = r.left(); col <= r.right(); ++col) {
            if (p[col * 4 + 3] > 1 && (p[col * 4] > 1 || p[col * 4 + 1] > 1 || p[col * 4 + 2] > 1))
                ++n;
        }
    }
    return n;
}

void sidePump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
}

// The export at `usec` with the child's parked sidecars `parked`, restored afterwards.
QImage sideRender(Timeline &tl, const SideSidecars &parked, qint64 usec)
{
    const SideSidecars keep = tl.parkedSidecarsBySequence();
    tl.setParkedSidecarsBySequence(parked);
    const QImage image = tlrender::renderFrameAt(&tl, usec, QSize(kSideW, kSideH));
    tl.setParkedSidecarsBySequence(keep);
    return image;
}

// A VideoPlayer on a nest whose child's sidecars are parked; the preview timeline is
// `tl` and every composed frame is recorded.
struct SideRig {
    Timeline tl;
    std::unique_ptr<VideoPlayer> player;
    QImage shown;
    int fires = 0;
    bool gl = false;
};

bool openSideRig(SideRig &rig, const QString &pathA, const QString &pathB,
                 const SideLayout &layout, const SideSidecars &parked)
{
    buildSideTimeline(rig.tl, pathA, pathB, layout);
    rig.tl.setParkedSidecarsBySequence(parked);
    rig.player = std::make_unique<VideoPlayer>();
    rig.player->resize(640, 360);
    rig.player->show();
    sidePump(300);
    GLPreview *view = rig.player->glPreview();
    rig.gl = view && rig.player->isGLAccelerated() && selftest::glContextUsable(view);
    if (!rig.gl)
        return false;
    QObject::connect(rig.player.get(), &VideoPlayer::frameComposited, rig.player.get(),
                     [&rig](const QImage &image) {
                         rig.shown = image;
                         ++rig.fires;
                     });
    view->setTimeline(&rig.tl);
    rig.player->setCanvasSize(kSideW, kSideH);
    rig.player->setProjectOutputSize(QSize(kSideW, kSideH));
    rig.player->setSequence(rig.tl.computePlaybackSequence());
    sidePump(400);
    return rig.player->hasActiveSequence();
}

// Runs `act`, then waits until a frame was composed (and 60 ms more for a late one).
bool sideAct(SideRig &rig, const std::function<void()> &act)
{
    const int before = rig.fires;
    act();
    QElapsedTimer timer;
    timer.start();
    while (rig.fires == before && timer.elapsed() < 3000)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    sidePump(60);
    return rig.fires > before;
}

} // namespace

int runNestSequenceSelftest()
{
    selftest::GateTally tally;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        std::string message = name;
        if (!detail.isEmpty())
            message += ": " + detail.toStdString();
        tally.gate(gate, ok, message);
    };

    check(1, "QApplication is available", QApplication::instance() != nullptr);
    if (tally.failed)
        return tally.finish("nest-sequence");

    QTemporaryDir tmpDir;
    check(2, "temporary directory", tmpDir.isValid());
    if (!tmpDir.isValid())
        return tally.finish("nest-sequence");

    QString error;
    const QString mediaPath = tmpDir.filePath(QStringLiteral("nest_sequence_src.mp4"));
    check(3, "synthetic media", writeSyntheticClip(mediaPath, &error), error);
    if (tally.failed)
        return tally.finish("nest-sequence");

    const ClipInfo media = makeMediaClip(mediaPath, QStringLiteral("media"));
    const double duration = media.duration;
    const TimelineSequence leaf =
        makeSequence(QStringLiteral("leaf"), oneTrack(media), oneTrack(media));
    const TimelineSequence mid =
        makeSequence(QStringLiteral("mid"),
                     oneTrack(makeSequenceRef(QStringLiteral("leaf"), duration)),
                     oneTrack(makeSequenceRef(QStringLiteral("leaf"), duration)));
    const TimelineSequence main =
        makeSequence(QStringLiteral("main"),
                     oneTrack(makeSequenceRef(QStringLiteral("mid"), duration)),
                     oneTrack(makeSequenceRef(QStringLiteral("mid"), duration)));

    Timeline nestedTimeline;
    nestedTimeline.setSequences(QVector<TimelineSequence>{main, mid, leaf},
                                QStringLiteral("main"));

    Timeline directTimeline;
    directTimeline.videoTracks().first()->setClips(QVector<ClipInfo>{media});

    const QSize outSize(kClipW, kClipH);
    const qint64 usec = 250000;
    const QImage nested =
        tlrender::renderFrameAt(&nestedTimeline, usec, outSize);
    const QImage direct =
        tlrender::renderFrameAt(&directTimeline, usec, outSize);
    check(4, "recursive nested render matches direct leaf media",
          !nested.isNull() && equalRgbaBytes(nested, direct));

    const QVector<PlaybackEntry> videoEntries =
        nestedTimeline.computePlaybackSequence();
    check(5, "recursive video flatten resolves to media",
          videoEntries.size() == 1
              && videoEntries.first().filePath == mediaPath
              && !timeline_nesting::isSequenceClipFilePath(videoEntries.first().filePath));

    const QVector<PlaybackEntry> audioEntries =
        nestedTimeline.computeAudioPlaybackSequence();
    check(6, "recursive audio flatten resolves to media",
          audioEntries.size() == 1 && audioEntries.first().filePath == mediaPath);

    const QHash<QString, QString> carriers = nestedTimeline.clipParentEntries();
    const QJsonObject store = timeline_nesting::decodeSequenceStoreObject(
        carriers.value(timeline_nesting::sequenceStoreParentKey()));
    check(7, "sequence store roundtrip payload",
          store.value(QStringLiteral("activeSequenceId")).toString() == QStringLiteral("main")
              && containsSequenceId(store, QStringLiteral("main"))
              && containsSequenceId(store, QStringLiteral("mid"))
              && containsSequenceId(store, QStringLiteral("leaf")));

    Timeline addClipTimeline;
    check(8, "addSequence succeeds",
          addClipTimeline.addSequence(leaf)
              && addClipTimeline.addSequenceClip(QStringLiteral("leaf"), 0));
    check(9, "addSequenceClip creates audio reference",
          !addClipTimeline.audioTracks().isEmpty()
              && addClipTimeline.audioTracks().first()
              && addClipTimeline.audioTracks().first()->clipCount() == 1
              && addClipTimeline.audioTracks().first()->clips().first().isSequenceReference());

    Timeline cycleTimeline;
    const TimelineSequence cycleA =
        makeSequence(QStringLiteral("cycle-a"),
                     oneTrack(makeSequenceRef(QStringLiteral("cycle-b"), duration)));
    const TimelineSequence cycleB =
        makeSequence(QStringLiteral("cycle-b"),
                     oneTrack(makeSequenceRef(QStringLiteral("cycle-a"), duration)));
    const TimelineSequence cycleMain =
        makeSequence(QStringLiteral("cycle-main"),
                     oneTrack(makeSequenceRef(QStringLiteral("cycle-a"), duration)));
    cycleTimeline.setSequences(QVector<TimelineSequence>{cycleMain, cycleA, cycleB},
                               QStringLiteral("cycle-main"));
    const QImage cycleFrame =
        tlrender::renderFrameAt(&cycleTimeline, usec, outSize);
    check(10, "cycle guard returns transparent fallback",
          !cycleFrame.isNull() && isTransparent(cycleFrame));

    QVector<TimelineSequence> depthSequences;
    depthSequences.append(makeSequence(QStringLiteral("depth-leaf"), oneTrack(media)));
    for (int i = 9; i >= 0; --i) {
        const QString id = QStringLiteral("depth-%1").arg(i);
        const QString next = (i == 9)
            ? QStringLiteral("depth-leaf")
            : QStringLiteral("depth-%1").arg(i + 1);
        depthSequences.append(
            makeSequence(id, oneTrack(makeSequenceRef(next, duration))));
    }
    depthSequences.append(
        makeSequence(QStringLiteral("depth-main"),
                     oneTrack(makeSequenceRef(QStringLiteral("depth-0"), duration))));
    Timeline depthTimeline;
    depthTimeline.setSequences(depthSequences, QStringLiteral("depth-main"));
    const QImage depthFrame =
        tlrender::renderFrameAt(&depthTimeline, usec, outSize);
    check(11, "depth guard returns transparent fallback",
          !depthFrame.isNull() && isTransparent(depthFrame));

    const QImage decoded =
        tlrender::detail::decodeClipFrameNativeForTest(mediaPath, 0.25)
            .scaled(outSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QString identitySha = direct.isNull() ? QString()
        : QString::fromLatin1(QCryptographicHash::hash(
              QByteArray::fromRawData(reinterpret_cast<const char *>(direct.constBits()),
                                      static_cast<qsizetype>(direct.sizeInBytes())),
              QCryptographicHash::Sha256).toHex());
    check(12, "legacy no-nest render remains decode-byte-identical",
          !direct.isNull() && equalRgbaBytes(direct, decoded),
          QStringLiteral("identity frame sha256=%1").arg(identitySha));

    QString precomposeUndoDetail;
    check(13, "single precompose undo clears sequence store",
          singlePrecomposeUndoClearsSequenceStore(media, &precomposeUndoDetail),
          precomposeUndoDetail);

    precomposeUndoDetail.clear();
    check(14, "double precompose two undos clear sequence store",
          doublePrecomposeUndoClearsSequenceStore(media, &precomposeUndoDetail),
          precomposeUndoDetail);

    // G15 / G16: a nested child's parked roto / matte / parent sidecars reach the
    // export (G15) and the preview frames the nested-sequence SSOT serves (G16).
    // Production code is not touched: this pins the behaviour.
    struct SideKind {
        const char *name;
        SideLayout layout;
        Timeline::SequenceSidecars with;
        Timeline::SequenceSidecars variant;   // another value of the same sidecar (refresh leg)
    };
    QVector<SideKind> sideKinds;
    {
        SideKind roto{"roto", SideLayout(), sideRoto(80.0), sideRoto(40.0)};
        SideKind matte{"matte", SideLayout(), sideMatte(TrackMatteType::AlphaMatte),
                       sideMatte(TrackMatteType::AlphaInvertedMatte)};
        matte.layout.tracks = 3;
        const double matteScale[3] = {0.25, 0.5, 0.25};
        const double matteDx[3] = {-0.3, 0.25, 0.25};
        const double matteDy[3] = {-0.3, 0.0, 0.0};
        SideKind parent{"parent", SideLayout(), sideParent(true), sideParent(false)};
        parent.layout.tracks = 3;
        const double parentScale[3] = {0.25, 0.5, 0.25};
        const double parentDx[3] = {-0.3, 0.25, -0.25};
        const double parentDy[3] = {-0.3, 0.0, 0.3};
        for (int t = 0; t < 3; ++t) {
            matte.layout.scale[t] = matteScale[t];
            matte.layout.dx[t] = matteDx[t];
            matte.layout.dy[t] = matteDy[t];
            parent.layout.scale[t] = parentScale[t];
            parent.layout.dx[t] = parentDx[t];
            parent.layout.dy[t] = parentDy[t];
        }
        sideKinds = {roto, matte, parent};
    }

    QString sideEncoderWhy;
    const QString sideA = tmpDir.filePath(QStringLiteral("nest_side_a.mp4"));
    const QString sideB = tmpDir.filePath(QStringLiteral("nest_side_b.mp4"));
    QString sideError;
    if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &sideEncoderWhy)) {
        tally.skip(15, ("mpeg4 encoder unavailable (environment): "
                        + sideEncoderWhy).toStdString().c_str());
        tally.skip(16, ("mpeg4 encoder unavailable (environment): "
                        + sideEncoderWhy).toStdString().c_str());
    } else if (!writeSideClip(sideA, 60, &sideError) || !writeSideClip(sideB, 220, &sideError)) {
        check(15, "sidecar fixture media", false, sideError);
        check(16, "sidecar fixture media", false, sideError);
    } else {
        // G15 (export, CPU): with the child's sidecars parked, renderFrameAt differs
        // from the sidecar-free render in more than 100 pixels, on both child clips;
        // the roto keeps the inside of its mask and removes the outside.
        {
            QString detail;
            bool all = true;
            for (const SideKind &kind : std::as_const(sideKinds)) {
                Timeline tl;
                buildSideTimeline(tl, sideA, sideB, kind.layout);
                for (const qint64 at : {qint64(500000), qint64(1500000)}) {
                    const QImage with = sideRender(tl, sideParked(kind.with), at);
                    const QImage none = sideRender(tl, SideSidecars(), at);
                    const int px = sideDiffPixels(with, none);
                    bool ok = !with.isNull() && !none.isNull() && px > 100;
                    QString extra;
                    if (QByteArray(kind.name) == "roto") {
                        const int inside = sideDiffPixels(with, none, QRect(2, 2, 72, 86));
                        const int outside = sideDiffPixels(with, none, QRect(88, 2, 68, 86));
                        const int remaining = sideMaterialPixels(with, QRect(88, 2, 68, 86));
                        ok = ok && inside == 0 && outside >= (68 * 86 * 95) / 100
                             && remaining == 0;
                        extra = QStringLiteral(" inside differs %1 outside differs %2/%3"
                                               " outside material %4")
                                    .arg(inside).arg(outside).arg(68 * 86).arg(remaining);
                    }
                    all = all && ok;
                    detail += QStringLiteral(" [%1 %2us differs %3 px%4 ok=%5]")
                                  .arg(QString::fromLatin1(kind.name)).arg(at).arg(px)
                                  .arg(extra).arg(ok);
                }
            }
            check(15, "a nested child's parked roto / matte / parent act in renderFrameAt",
                  all, detail);
        }

        // G16 (preview, GL): the frames the VideoPlayer shows through its public
        // entry points (a forward tick, a paused seek, a frame step inside an entry,
        // a frame step across the entry boundary, a redraw) equal renderFrameAt
        // within 1 and differ from the sidecar-free render in more than 100 pixels.
        {
            QString detail;
            bool all = true;
            bool anyNoGl = false;
            for (const SideKind &kind : std::as_const(sideKinds)) {
                SideRig rig;
                const bool opened =
                    openSideRig(rig, sideA, sideB, kind.layout, sideParked(kind.with));
                if (!rig.gl) {
                    anyNoGl = true;
                    break;
                }
                bool ok = opened;
                detail += QStringLiteral(" [%1 opened=%2]").arg(QString::fromLatin1(kind.name))
                              .arg(opened);
                const auto frameNo = [](qint64 us) { return qRound(double(us) / kSideFrameUs); };
                // `image` against renderFrameAt at one of `times` (within 1) and against
                // the sidecar-free render at that time (more than 100 pixels).
                const auto judge = [&](const char *leg, const QImage &image,
                                       const QVector<qint64> &times) {
                    int bestDiff = 999;
                    qint64 used = times.first();
                    for (const qint64 at : times) {
                        const int diff =
                            sideMaxDiff(image, sideRender(rig.tl, sideParked(kind.with), at));
                        if (diff < bestDiff) {
                            bestDiff = diff;
                            used = at;
                        }
                    }
                    const int ctl = sideDiffPixels(image, sideRender(rig.tl, SideSidecars(), used));
                    const bool legOk = bestDiff <= 1 && ctl > 100;
                    detail += QStringLiteral(" %1 ref@%2f maxDiff=%3 vsNoSidecar=%4px ok=%5")
                                  .arg(QString::fromLatin1(leg)).arg(frameNo(used))
                                  .arg(bestDiff).arg(ctl).arg(legOk);
                    return legOk;
                };
                const auto position = [&]() { return qint64(rig.player->timelinePositionUs()); };

                // Paused seek.
                bool legOk = sideAct(rig, [&] { rig.player->seek(500); });
                const qint64 seekPos = position();
                {
                    const bool judged = judge("seek", rig.shown, {seekPos});
                    legOk = legOk && std::abs(seekPos - 500000) <= kSideFrameUs && judged;
                }
                ok = ok && legOk;

                // Frame step inside the entry (no boundary crossed).
                legOk = sideAct(rig, [&] { rig.player->stepForward(); });
                const qint64 inPos = position();
                {
                    const bool judged = judge("step-inside", rig.shown, {inPos});
                    legOk = legOk && inPos > seekPos && inPos - seekPos <= 2 * kSideFrameUs
                        && inPos < 1000000 && judged;
                }
                ok = ok && legOk;

                // Frame step across the boundary between the child's two clips; the
                // frame shown right after an entry change may be the previous one.
                legOk = sideAct(rig, [&] { rig.player->seek(917); });
                const qint64 beforeCross = position();
                legOk = sideAct(rig, [&] { rig.player->stepForward(); }) && legOk;
                const qint64 crossPos = position();
                {
                    const bool judged =
                        judge("step-across", rig.shown, {crossPos, crossPos - kSideFrameUs});
                    legOk = legOk && beforeCross < 1000000 && crossPos >= 999000 && judged;
                }
                ok = ok && legOk;

                // Forward tick inside the first clip.
                legOk = sideAct(rig, [&] { rig.player->seek(300); });
                struct Capture {
                    QImage image;
                    qint64 at;
                };
                std::vector<Capture> captured;
                const auto conn = QObject::connect(
                    rig.player.get(), &VideoPlayer::frameComposited, rig.player.get(),
                    [&](const QImage &image) { captured.push_back({image, position()}); });
                QElapsedTimer tickTimer;
                tickTimer.start();
                rig.player->play();
                while (tickTimer.elapsed() < 4000
                       && !(captured.size() >= 5 && captured.back().at > 700000))
                    QApplication::processEvents(QEventLoop::AllEvents, 2);
                rig.player->pause();
                QObject::disconnect(conn);
                sidePump(60);
                bool tickOk = legOk && captured.size() >= 3;
                int judged = 0;
                for (const Capture &frame : captured) {
                    if (frame.at >= 1000000 - kSideFrameUs)
                        continue;
                    tickOk = judge("tick", frame.image,
                                   {frame.at, frame.at - kSideFrameUs, frame.at + kSideFrameUs})
                        && tickOk;
                    ++judged;
                }
                tickOk = tickOk && judged >= 3;
                detail += QStringLiteral(" tick captured=%1 judged=%2").arg(captured.size()).arg(judged);
                ok = ok && tickOk;

                // Redraw after the sidecar changed: it goes through the SSOT again.
                legOk = sideAct(rig, [&] { rig.player->seek(500); });
                const qint64 redrawPos = position();
                const QImage beforeRedraw = rig.shown;
                legOk = legOk && judge("redraw-before", beforeRedraw, {redrawPos});
                rig.tl.setParkedSidecarsBySequence(sideParked(kind.variant));
                legOk = sideAct(rig, [&] { rig.player->setHiddenTextOverlayIndex(0); }) && legOk;
                const QImage afterRedraw = rig.shown;
                const int varDiff =
                    sideMaxDiff(afterRedraw,
                                sideRender(rig.tl, sideParked(kind.variant), redrawPos));
                const int moved = sideDiffPixels(afterRedraw, beforeRedraw);
                const int ctl = sideDiffPixels(afterRedraw, sideRender(rig.tl, SideSidecars(), redrawPos));
                const bool redrawOk = varDiff <= 1 && moved > 100 && ctl > 100;
                detail += QStringLiteral(" redraw maxDiff=%1 movedFromBefore=%2px vsNoSidecar=%3px ok=%4")
                              .arg(varDiff).arg(moved).arg(ctl).arg(redrawOk);
                legOk = legOk && redrawOk;
                ok = ok && legOk;
                rig.tl.setParkedSidecarsBySequence(sideParked(kind.with));
                rig.player->setHiddenTextOverlayIndex(-1);

                detail += QStringLiteral(" => %1 ok=%2").arg(QString::fromLatin1(kind.name)).arg(ok);
                all = all && ok;
            }
            if (anyNoGl) {
                tally.skip(16, "GL preview unavailable (environment): the nested-sequence "
                               "preview legs need a GL context");
            } else {
                check(16, "a nested child's parked roto / matte / parent act in the preview "
                          "frames of a tick, a paused seek, frame steps and a redraw",
                      all, detail);
            }
        }
    }

    return tally.finish("nest-sequence");
}
