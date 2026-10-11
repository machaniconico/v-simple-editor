// src/selftests/video_player_parent_selftest.cpp
// --selftest=video-player-parent (US-910 PARENT-OBS)
//
// Observes VideoPlayer's own parent resolution (resolveDecodedLayerParentSources
// + effectiveLayerTransforms) in its two real layer-admission paths through
// VideoPlayer::setParentAdmissionObservationForTest, and compares the result
// with the renderer's gate tlrender::parentLayerAt (adopted = hit && decodable).
// US-1005: VideoPlayer adopts a parent only through that same gate; each record
// carries the parentLayerAt result VideoPlayer decided with (vpHit), printed
// next to the selftest's own parentLayerAt call (vpHitAgrees).
//   cpu-composite: refreshPreviewClipCpuComposite (paused, CPU preview FX)
//   multi-track:   handlePlaybackTick compositor (playing)
// Layout: V1 (track 0) real base clip, V2 (track 1) parent at clip index 1
// (index 0 is a spacer, so an index mix-up points at the wrong layer), V3
// (track 2) child.
//   G1 normal parent: both paths adopt the V2 clip-1 layer and the effective
//      transform equals clipgeom::composeParented(child, parent, canvas); with
//      playback proxy 2 and a 65x64 output the multi-track record canvas is the
//      proxy-reduced compositor canvas (32x32), not 65x64.
//   G2 V1 parent: neither side adopts (parentSourceIndex -1).
//   G3 adjustment-clip parent: neither side adopts.
//   G4 disabled parent, G5 overlap with the incoming clip as parent, G6
//      TransitionAlignment::Start borrowed trail, G7 offline parent: verdict
//      EQUAL / DIVERGES per path printed with timeline / local / source
//      seconds; each path's current verdict label (a divergence is labelled
//      KNOWN-DIVERGENCE and is never pinned since US-1005), adoption operands
//      {vpAdopts, vpCorrectLayer, rendererAdopts} and a case-specific check
//      are pinned, so a change in either side fails the gate. Pins: G4
//      cpu-composite EQUAL (neither adopts), multi-track SSOT-ROUTED; G5 both
//      paths EQUAL (parentLayerAt(incoming B) = none, neither adopts); G6 / G7
//      both paths EQUAL. A timeline holding a disabled clip makes
//      playback serve tlrender::renderFrameAt (timelineHasActiveShapeClip), so
//      the multi-track loop is never entered there: that state is pinned as
//      SSOT-ROUTED (no multi-track record, frames composited, renderFrameAt
//      admitted the child layer inside the window, and parentLayerAt rejected
//      the disabled parent at every one of those times).
//      Controls: G4 an enabled parent beside a disabled spacer (still routed,
//      renderer adopts -> not SSOT-ROUTED) and cpu-composite both adopt; G5
//      reversed divergence and both adopt, G6 both sides stop adopting, G7
//      both sides adopt (operands changed, pin must reject).
//   Fixture media failure fails all gates; only a missing mpeg4 encoder SKIPs.
//   G8 observation OFF records nothing while the paths still run.
//   G9 each admission path produced records.

#include "SelftestGate.h"
#include "../ClipGeometry.h"
#include "../CodecDetector.h"
#include "../GLPreview.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TrackMatteKey.h"
#include "../VideoEffect.h"
#include "../VideoPlayer.h"
#include "../libavcore/Encode.h"

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QImage>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

using Record = VideoPlayer::ParentAdmissionRecord;
using tlrender::ParentLayerHit;

const QSize kCanvas(64, 64);
constexpr int kFps = 10;
constexpr int kFrameCount = 60; // 6 s
constexpr int kChildTrack = 2;
constexpr int kChildClip = 0;
const char *const kCpuPath = "cpu-composite";
const char *const kTrackPath = "multi-track";

const clipgeom::ClipTransform kParentT{0.5, 0.25, -0.125, 30.0};
const clipgeom::ClipTransform kChildT{0.5, 0.1, 0.05, -10.0};

qint64 usecOf(double sec)
{
    return static_cast<qint64>(std::llround(sec * 1'000'000.0));
}

// Same recipe as ClipLutSelftest.cpp writeSyntheticClip (mpeg4 via
// libavcore::FrameEncoder); each file gets its own base colour.
bool writeSyntheticClip(const QString &path, const QColor &base, QString *error)
{
    libavcore::EncodeRequest req;
    req.width = kCanvas.width();
    req.height = kCanvas.height();
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
        *error = QStringLiteral("FrameEncoder::open failed: ") + QString::fromStdString(*err);
        return false;
    }
    for (int i = 0; i < kFrameCount; ++i) {
        QImage frame(kCanvas, QImage::Format_RGB888);
        frame.fill(QColor((base.red() + i * 3) & 255, base.green(), base.blue()));
        if (!encoder.pushFrame(frame, i)) {
            *error = QStringLiteral("FrameEncoder::pushFrame failed at frame %1").arg(i);
            return false;
        }
    }
    if (auto err = encoder.finalize()) {
        *error = QStringLiteral("FrameEncoder::finalize failed: ") + QString::fromStdString(*err);
        return false;
    }
    return true;
}

ClipInfo makeClip(const QString &path, const QString &name, double inPoint,
                  double outPoint, const clipgeom::ClipTransform &t = {})
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = name;
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = inPoint;
    clip.outPoint = outPoint;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    clip.visible = true;
    clip.videoScale = t.videoScale;
    clip.videoDx = t.videoDx;
    clip.videoDy = t.videoDy;
    clip.rotation2DDegrees = t.rotationDeg;
    return clip;
}

void addClip(Timeline &tl, int trackIdx, const ClipInfo &clip)
{
    while (tl.videoTracks().size() <= trackIdx)
        tl.addVideoTrack();
    if (TimelineTrack *track = tl.videoTracks().value(trackIdx, nullptr))
        track->addClip(clip);
}

void setTransition(ClipInfo &out, ClipInfo &in, TransitionAlignment alignment)
{
    out.trailOut.type = in.leadIn.type = TransitionType::CrossDissolve;
    out.trailOut.duration = in.leadIn.duration = 1.0;
    out.trailOut.alignment = in.leadIn.alignment = alignment;
}

struct Drive {
    QVector<Record> cpu;
    QVector<Record> track;
    int cpuFrames = 0;   // frameComposited emitted by the cpu-composite step(s)
    int cpuAttempt = 0;  // 1 = at the paused seek, 2 = after playback
    int trackFrames = 0; // frameComposited emitted while playing
    quint64 rendererCallsDuringPlay = 0; // tlrender::parentLayerAt calls while playing
    int rendererChildLayersDuringPlay = 0; // child layers renderFrameAt admitted while playing
    // Child layers renderFrameAt admitted while playing (the child sits at
    // [0, 6) with in = 0, so localSec == timelineSec) and parentLayerAt(parent)
    // at each of those times (filled by observeCase).
    QVector<tlrender::detail::ObservedLayerTime> rendererChildTimes;
    QVector<std::optional<ParentLayerHit>> rendererChildParentHits;
    bool seeked = false;
    bool trackWaitOk = false;
    qint64 pausedUs = -1;
};

// One fresh VideoPlayer per case:
//   1. seek (paused) and push a CPU-only preview stack (the effect-dialog
//      preview case) -> cpu-composite path at the seek position;
//   2. play until a multi-track record inside [windowStart, windowEnd) shows
//      up (or 5 frames were composited without one), then pause;
//   3. if step 1 produced no record in the window, push the preview stack
//      again at the paused playhead. A paused seek alone leaves the V1
//      compositor base (m_lastV1RawFrame) empty on a plain timeline; playback
//      fills it, as in the app.
Drive drive(Timeline &tl, double seekSec, double windowEndSec, bool observe,
            QSize outputSize = kCanvas)
{
    Drive out;
    tl.refreshPlaybackSequence();
    VideoPlayer player;
    player.resize(160, 120);
    player.setCanvasSize(kCanvas.width(), kCanvas.height());
    player.setProjectOutputSize(outputSize);
    player.glPreview()->setTimeline(&tl);
    player.setSequence(tl.computePlaybackSequence());

    QSignalSpy frames(&player, &VideoPlayer::frameComposited);
    const qint64 target = usecOf(seekSec);
    player.seek(static_cast<int>(std::llround(seekSec * 1000.0)));
    out.seeked = QTest::qWaitFor([&]() {
        return frames.count() > 0
            && std::llabs(player.timelinePositionUs() - target) < 50'000;
    }, 5000);

    const qint64 windowStart = target;
    const qint64 windowEnd = usecOf(windowEndSec);
    auto inWindow = [&](const QVector<Record> &records, const char *path) {
        for (const Record &r : records)
            if (r.path == QLatin1String(path)
                && r.timelineUs >= windowStart && r.timelineUs < windowEnd)
                return true;
        return false;
    };
    auto cpuStep = [&]() {
        player.setParentAdmissionObservationForTest(observe);
        const int before = frames.count();
        player.setPreviewEffects({VideoEffect::createFilmGrain(0.3, 1, 0.0, false)});
        const bool shown = QTest::qWaitFor([&]() { return frames.count() > before; }, 2000);
        out.cpuFrames += shown ? frames.count() - before : 0;
        const QVector<Record> records = player.parentAdmissionRecordsForTest();
        player.setPreviewEffects({});
        return records;
    };

    const QVector<Record> cpuAtSeek = cpuStep();

    player.setParentAdmissionObservationForTest(observe);
    const int beforePlay = frames.count();
    tlrender::detail::setLayerTimeObservationForTest(true);
    tlrender::detail::resetParentLayerAtCallCountForTest();
    player.play();
    out.trackWaitOk = QTest::qWaitFor([&]() {
        return (observe && inWindow(player.parentAdmissionRecordsForTest(), kTrackPath))
            || frames.count() >= beforePlay + 5;
    }, 5000);
    player.pause();
    out.rendererCallsDuringPlay = tlrender::detail::parentLayerAtCallCountForTest();
    for (const tlrender::detail::ObservedLayerTime &o : tlrender::detail::observedLayerTimesForTest())
        if (o.trackIdx == kChildTrack && o.clipIdx == kChildClip)
            out.rendererChildTimes.append(o);
    out.rendererChildLayersDuringPlay = out.rendererChildTimes.size();
    tlrender::detail::setLayerTimeObservationForTest(false);
    out.pausedUs = player.timelinePositionUs();
    out.trackFrames = frames.count() - beforePlay;
    out.track = player.parentAdmissionRecordsForTest();

    if (observe && inWindow(cpuAtSeek, kCpuPath)) {
        out.cpu = cpuAtSeek;
        out.cpuAttempt = 1;
    } else {
        out.cpu = cpuAtSeek + cpuStep();
        out.cpuAttempt = 2;
    }
    player.setParentAdmissionObservationForTest(false);
    return out;
}

struct PathResult {
    bool recorded = false;   // a resolve group with the child inside the window
    Record child;
    Record pointed;          // layer the child's parentSourceIndex points at
    bool pointedValid = false;
    std::optional<ParentLayerHit> hit;      // parentLayerAt(parent) at record time
    std::optional<ParentLayerHit> peerHit;  // parentLayerAt(peer clip) (overlap / trail)
    bool vpAdopts = false;
    bool vpCorrectLayer = false;
    bool rendererAdopts = false;
    bool vpHitAgrees = false; // VideoPlayer's parentLayerAt result == `hit`
    bool parentLayerPresent = false; // parent decoded in the child's resolve group
    QString verdict = QStringLiteral("NOT-DRIVEN");
};

PathResult evaluate(const Timeline &tl, const QVector<Record> &records,
                    const char *path, double windowStartSec, double windowEndSec,
                    int parentTrack, int parentClip, int peerClip)
{
    PathResult res;
    const qint64 lo = usecOf(windowStartSec);
    const qint64 hi = usecOf(windowEndSec);
    for (const Record &r : records) {
        if (r.path != QLatin1String(path)
            || r.sourceTrack != kChildTrack || r.sourceClipIndex != kChildClip)
            continue;
        if (r.timelineUs < lo || r.timelineUs >= hi)
            continue;
        res.recorded = true;
        res.child = r;
        break;
    }
    if (!res.recorded)
        return res;
    for (const Record &r : records) {
        if (r.resolveSerial == res.child.resolveSerial
            && r.layerIndex == res.child.parentSourceIndex) {
            res.pointed = r;
            res.pointedValid = true;
        }
        if (r.resolveSerial == res.child.resolveSerial
            && r.sourceTrack == parentTrack && r.sourceClipIndex == parentClip)
            res.parentLayerPresent = true;
    }
    res.hit = tlrender::parentLayerAt(&tl, res.child.timelineUs, parentTrack, parentClip);
    if (peerClip >= 0)
        res.peerHit = tlrender::parentLayerAt(&tl, res.child.timelineUs, parentTrack, peerClip);
    res.vpAdopts = res.child.parentSourceIndex >= 0;
    res.vpCorrectLayer = res.vpAdopts && res.pointedValid
        && res.pointed.sourceTrack == parentTrack
        && res.pointed.sourceClipIndex == parentClip;
    res.rendererAdopts = res.hit && res.hit->decodable;
    // Same gate, same (timeline, usec, track, clip): exact equality.
    res.vpHitAgrees = res.child.parentHit == res.hit.has_value()
        && (!res.hit
            || (res.child.parentHitDecodable == res.hit->decodable
                && res.child.parentLocalSeconds == res.hit->clipLocalSeconds
                && res.child.parentSourceSeconds == res.hit->sourceSeconds));
    if (res.vpAdopts && !res.vpCorrectLayer)
        res.verdict = QStringLiteral("WRONG-LAYER");
    else
        res.verdict = res.vpAdopts == res.rendererAdopts
            ? QStringLiteral("EQUAL") : QStringLiteral("DIVERGES");
    return res;
}

std::string hitText(const char *label, const std::optional<ParentLayerHit> &hit)
{
    std::ostringstream s;
    s.precision(17);
    s << label << '=';
    if (!hit) {
        s << "none";
    } else {
        s << "{timelineSec=" << hit->timelineSec
          << " localSec=" << hit->clipLocalSeconds
          << " sourceSec=" << hit->sourceSeconds
          << " decodable=" << (hit->decodable ? 1 : 0) << '}';
    }
    return s.str();
}

std::string tText(const clipgeom::ClipTransform &t)
{
    std::ostringstream s;
    s.precision(17);
    s << "{scale=" << t.videoScale << " dx=" << t.videoDx << " dy=" << t.videoDy
      << " rot=" << t.rotationDeg << '}';
    return s.str();
}

void report(int gate, const char *path, const PathResult &r)
{
    std::ostringstream s;
    s.precision(17);
    s << "  G" << gate << " [" << path << "] verdict=" << r.verdict.toStdString()
      << " recorded=" << (r.recorded ? 1 : 0);
    if (r.recorded) {
        s << " timelineSec=" << static_cast<double>(r.child.timelineUs) / 1e6
          << " child.parentClipId=" << r.child.parentClipId.toStdString()
          << " child.parentSourceIndex=" << r.child.parentSourceIndex
          << " pointsTo=";
        if (r.pointedValid)
            s << r.pointed.sourceTrack << ':' << r.pointed.sourceClipIndex;
        else
            s << "none";
        s << " vpAdopts=" << (r.vpAdopts ? 1 : 0)
          << " rendererAdopts=" << (r.rendererAdopts ? 1 : 0) << ' '
          << hitText("parentLayerAt(parent)", r.hit);
        s << ' ' << hitText("parentLayerAt(peer)", r.peerHit);
        s << " parentLayerPresent=" << (r.parentLayerPresent ? 1 : 0);
        if (!r.parentLayerPresent) {
            // VideoPlayer only asks parentLayerAt once the parent layer decoded.
            s << " vpHit=not-evaluated(parent layer not decoded) vpHitAgrees=n/a";
        } else {
            std::optional<ParentLayerHit> vpHit;
            if (r.child.parentHit) {
                vpHit = ParentLayerHit{static_cast<double>(r.child.timelineUs) / 1e6,
                                       r.child.parentLocalSeconds,
                                       r.child.parentSourceSeconds,
                                       r.child.parentHitDecodable};
            }
            s << ' ' << hitText("vpHit", vpHit)
              << " vpHitAgrees=" << (r.vpHitAgrees ? 1 : 0);
        }
        s << " child.effective=" << tText(r.child.effectiveTransform);
    }
    std::cerr << s.str() << '\n';
}

bool sameT(const clipgeom::ClipTransform &a, const clipgeom::ClipTransform &b, double eps)
{
    return std::fabs(a.videoScale - b.videoScale) <= eps
        && std::fabs(a.videoDx - b.videoDx) <= eps
        && std::fabs(a.videoDy - b.videoDy) <= eps
        && std::fabs(a.rotationDeg - b.rotationDeg) <= eps;
}

} // namespace

int runVideoPlayerParentSelftest()
{
    selftest::GateTally tally;

    const QByteArray oldParenting = qgetenv("VEDITOR_PARENTING");
    const bool hadParenting = qEnvironmentVariableIsSet("VEDITOR_PARENTING");
    qputenv("VEDITOR_PARENTING", QByteArray("1"));
    struct RestoreEnv {
        QByteArray value;
        bool had;
        ~RestoreEnv()
        {
            if (had) qputenv("VEDITOR_PARENTING", value);
            else qunsetenv("VEDITOR_PARENTING");
        }
    } restoreEnv{oldParenting, hadParenting};

    QTemporaryDir tmpDir;
    const QString basePath = tmpDir.filePath(QStringLiteral("base.mp4"));
    const QString spacerPath = tmpDir.filePath(QStringLiteral("spacer.mp4"));
    const QString parentPath = tmpDir.filePath(QStringLiteral("parent.mp4"));
    const QString childPath = tmpDir.filePath(QStringLiteral("child.mp4"));
    if (!CodecDetector::isEncoderAvailable(QStringLiteral("mpeg4"))) {
        for (int g = 1; g <= 9; ++g)
            tally.skip(g, "mpeg4 encoder not registered (CodecDetector::isEncoderAvailable)");
        return tally.finish("[video-player-parent]");
    }
    QString mediaError;
    if (!tmpDir.isValid()
        || !writeSyntheticClip(basePath, QColor(40, 40, 40), &mediaError)
        || !writeSyntheticClip(spacerPath, QColor(0, 200, 0), &mediaError)
        || !writeSyntheticClip(parentPath, QColor(0, 0, 220), &mediaError)
        || !writeSyntheticClip(childPath, QColor(220, 0, 0), &mediaError)) {
        const std::string reason = tmpDir.isValid()
            ? "mpeg4 fixture media could not be written: " + mediaError.toStdString()
            : std::string("temporary directory invalid");
        for (int g = 1; g <= 9; ++g)
            tally.gate(g, false, reason.c_str());
        return tally.finish("[video-player-parent]");
    }

    const QString parentKey = trackMatteClipKey(1, 1);
    const QString childKey = trackMatteClipKey(kChildTrack, kChildClip);

    // V1 base [0, 6), V3 child [0, 6) parented to `parentKey`; V2 is filled
    // by `buildV2`.
    auto buildTimeline = [&](Timeline &tl, const std::function<void(Timeline &)> &buildV2,
                             const QString &parentOf) {
        addClip(tl, 0, makeClip(basePath, QStringLiteral("base"), 0.0, 6.0));
        buildV2(tl);
        addClip(tl, kChildTrack, makeClip(childPath, QStringLiteral("child"), 0.0, 6.0, kChildT));
        tl.setClipParent(childKey, parentOf);
    };
    // V2: spacer [0, 1) + parent [1, 5) (clip index 1).
    auto spacerThen = [&](const ClipInfo &parent) {
        return [&, parent](Timeline &tl) {
            addClip(tl, 1, makeClip(spacerPath, QStringLiteral("spacer"), 0.0, 1.0, kParentT));
            addClip(tl, 1, parent);
        };
    };

    struct PathPair { PathResult cpu; PathResult track; Drive d; };
    auto observeCase = [&](Timeline &tl, double seekSec, double windowEndSec,
                           int parentTrack, int parentClip, int peerClip) {
        PathPair p;
        p.d = drive(tl, seekSec, windowEndSec, true);
        p.cpu = evaluate(tl, p.d.cpu, kCpuPath, seekSec, windowEndSec,
                         parentTrack, parentClip, peerClip);
        p.track = evaluate(tl, p.d.track, kTrackPath, seekSec, windowEndSec,
                           parentTrack, parentClip, peerClip);
        // Playback composited frames without ever entering the multi-track
        // admission loop while renderFrameAt admitted the child layer: the
        // tick served tlrender::renderFrameAt (canonical-compositor routing,
        // e.g. a disabled clip in the timeline), so the renderer decides.
        // SSOT-ROUTED additionally requires that the renderer rejected the
        // parent at every admitted child time inside the window.
        if (!p.track.recorded && p.d.track.isEmpty()) {
            const qint64 lo = usecOf(seekSec);
            const qint64 hi = usecOf(windowEndSec);
            bool allInWindow = true;
            bool anyParentHit = false;
            for (const tlrender::detail::ObservedLayerTime &o : p.d.rendererChildTimes) {
                const auto &clips = tl.videoTracks().at(o.trackIdx)->clips();
                double clipStartSec = 0.0;
                for (int i = 0; i < o.clipIdx; ++i)
                    clipStartSec += clips.at(i).leadInSec + clips.at(i).effectiveDuration();
                clipStartSec += clips.at(o.clipIdx).leadInSec;
                const qint64 timelineUs = usecOf(clipStartSec + o.localSec);
                const std::optional<ParentLayerHit> hit =
                    tlrender::parentLayerAt(&tl, timelineUs, parentTrack, parentClip);
                p.d.rendererChildParentHits.append(hit);
                const bool inWindow = timelineUs >= lo && timelineUs < hi;
                allInWindow = allInWindow && inWindow;
                anyParentHit = anyParentHit || hit.has_value();
                std::ostringstream s;
                s.precision(17);
                s << "  routed child layer timelineSec=" << clipStartSec + o.localSec
                  << " localSec=" << o.localSec << " sourceSec=" << o.sourceSec
                  << " inWindow=" << (inWindow ? 1 : 0) << ' '
                  << hitText("parentLayerAt(parent)", hit);
                std::cerr << s.str() << '\n';
            }
            p.track.rendererAdopts = anyParentHit;
            const bool routed = p.d.trackFrames > 0 && !p.d.rendererChildTimes.isEmpty()
                && allInWindow && !anyParentHit;
            std::cerr << "  [" << kTrackPath << "] routing check vpAdopts=n/a(routed)"
                      << " rendererAdopts=" << (anyParentHit ? 1 : 0)
                      << " childLayers=" << p.d.rendererChildTimes.size()
                      << " allInWindow=" << (allInWindow ? 1 : 0)
                      << " trackFrames=" << p.d.trackFrames
                      << " routed=" << (routed ? 1 : 0) << '\n';
            if (routed)
                p.track.verdict = QStringLiteral("SSOT-ROUTED");
        }
        std::cerr << "  drive seeked=" << (p.d.seeked ? 1 : 0)
                  << " trackWaitOk=" << (p.d.trackWaitOk ? 1 : 0)
                  << " pausedSec=" << static_cast<double>(p.d.pausedUs) / 1e6
                  << " rendererCallsDuringPlay=" << p.d.rendererCallsDuringPlay
                  << " rendererChildLayersDuringPlay=" << p.d.rendererChildLayersDuringPlay
                  << " cpuAttempt=" << p.d.cpuAttempt
                  << " cpuFrames=" << p.d.cpuFrames << " cpuRecords=" << p.d.cpu.size()
                  << " trackFrames=" << p.d.trackFrames
                  << " trackRecords=" << p.d.track.size() << '\n';
        return p;
    };

    // ── G1: normal parent (EQUAL control) ──────────────────────────────────
    Timeline g1;
    buildTimeline(g1, spacerThen(makeClip(parentPath, QStringLiteral("parent"), 0.0, 4.0, kParentT)),
                  parentKey);
    const PathPair p1 = observeCase(g1, 2.0, 4.0, 1, 1, 0);
    {
        auto pathOk = [&](const char *name, const PathResult &r) {
            report(1, name, r);
            const clipgeom::ClipTransform expected =
                clipgeom::composeParented(kChildT, kParentT, r.child.canvas);
            const bool parentLocalOk = r.pointedValid && sameT(r.pointed.localTransform, kParentT, 1e-12);
            const bool transformOk = r.recorded && sameT(r.child.effectiveTransform, expected, 1e-10);
            std::cerr << "  G1 [" << name << "] expected=" << tText(expected)
                      << " canvas=" << r.child.canvas.width() << 'x' << r.child.canvas.height()
                      << " parentLocalOk=" << (parentLocalOk ? 1 : 0)
                      << " transformOk=" << (transformOk ? 1 : 0) << '\n';
            // Sanity band on the expected value: parenting really moved the child.
            const bool expectedMoves = !sameT(expected, kChildT, 1e-3);
            return r.recorded && r.verdict == QStringLiteral("EQUAL") && r.vpCorrectLayer
                && r.rendererAdopts && r.vpHitAgrees && parentLocalOk && transformOk
                && expectedMoves;
        };
        const bool cpuOk = pathOk(kCpuPath, p1.cpu);
        const bool trackOk = pathOk(kTrackPath, p1.track);

        // Sub-check: the multi-track record uses the compositor's proxy-reduced
        // canvas. Playback proxy 2 on a 65x64 output composites into 32x32 (the
        // H/W aspect changes), so composeParented on the pre-proxy 65x64 canvas
        // gives a different transform than the one composited.
        bool proxyCanvasOk = false;
        {
            const QString prefKey = QStringLiteral("playbackProxyDivisor");
            QSettings prefs(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
            const bool hadPref = prefs.contains(prefKey);
            const int oldPref = prefs.value(prefKey, 2).toInt();
            const QSize oddOutput(65, 64);
            setPlaybackProxyDivisorPreference(2);
            const Drive d = drive(g1, 2.0, 4.0, true, oddOutput);
            if (hadPref) {
                setPlaybackProxyDivisorPreference(oldPref);
            } else {
                // Same fallback as playbackProxyDivisor(): env, else 2.
                bool envOk = false;
                const int envDivisor = qEnvironmentVariable("VEDITOR_PLAYBACK_PROXY").toInt(&envOk);
                setPlaybackProxyDivisorPreference(envOk ? envDivisor : 2);
                prefs.remove(prefKey);
            }
            const PathResult r = evaluate(g1, d.track, kTrackPath, 2.0, 4.0, 1, 1, 0);
            const PathResult rc = evaluate(g1, d.cpu, kCpuPath, 2.0, 4.0, 1, 1, 0);
            const int proxy = 2 * r.child.adaptiveCanvasDivisor;
            const QSize expectedCanvas(qMax(2, oddOutput.width() / proxy),
                                       qMax(2, oddOutput.height() / proxy));
            const clipgeom::ClipTransform atRecord =
                clipgeom::composeParented(kChildT, kParentT, r.child.canvas);
            const clipgeom::ClipTransform atOutput =
                clipgeom::composeParented(kChildT, kParentT, oddOutput);
            const bool canvasOk = r.recorded && r.child.canvas == expectedCanvas
                && r.child.canvas != oddOutput;
            const bool transformOk = r.recorded
                && sameT(r.child.effectiveTransform, atRecord, 1e-10);
            // Sanity band: the pre-proxy canvas gives a visibly different result.
            const bool outputDiffers = !sameT(atRecord, atOutput, 1e-6);
            std::ostringstream s;
            s.precision(17);
            s << "  G1 proxy sub-check output=65x64 playbackProxyPref=2"
              << " hadPref=" << (hadPref ? 1 : 0) << " restoredPref=" << (hadPref ? oldPref : -1)
              << " trackFrames=" << d.trackFrames << " recorded=" << (r.recorded ? 1 : 0)
              << " adaptiveCanvasDivisor=" << r.child.adaptiveCanvasDivisor
              << " record.canvas=" << r.child.canvas.width() << 'x' << r.child.canvas.height()
              << " expectedCanvas=" << expectedCanvas.width() << 'x' << expectedCanvas.height()
              << " cpu.record.canvas=" << rc.child.canvas.width() << 'x' << rc.child.canvas.height()
              << " vpCorrectLayer=" << (r.vpCorrectLayer ? 1 : 0)
              << "\n    effective=" << tText(r.child.effectiveTransform)
              << "\n    composeParented(record.canvas)=" << tText(atRecord)
              << "\n    composeParented(65x64)=" << tText(atOutput)
              << "\n    dx(record.canvas)-dx(65x64)=" << (atRecord.videoDx - atOutput.videoDx)
              << " dy(record.canvas)-dy(65x64)=" << (atRecord.videoDy - atOutput.videoDy)
              << "\n    canvasOk=" << (canvasOk ? 1 : 0) << " transformOk=" << (transformOk ? 1 : 0)
              << " outputDiffers=" << (outputDiffers ? 1 : 0);
            std::cerr << s.str() << '\n';
            proxyCanvasOk = canvasOk && transformOk && outputDiffers && r.vpCorrectLayer;
        }
        tally.gate(1, cpuOk && trackOk && proxyCanvasOk,
                   "normal V2 clip-1 parent not adopted identically by VideoPlayer and parentLayerAt, or the multi-track record canvas is not the proxy-reduced compositor canvas");
    }

    // ── G2: V1 parent -> neither side adopts ────────────────────────────────
    {
        Timeline tl;
        buildTimeline(tl, spacerThen(makeClip(parentPath, QStringLiteral("v2"), 0.0, 4.0, kParentT)),
                      trackMatteClipKey(0, 0));
        const PathPair p = observeCase(tl, 2.0, 4.0, 0, 0, -1);
        report(2, kCpuPath, p.cpu);
        report(2, kTrackPath, p.track);
        // Index check (not transform): the :959 guard would hide a V1 parent
        // in the transform even if the index were adopted.
        const bool ok = p.cpu.recorded && p.track.recorded
            && p.cpu.child.parentSourceIndex == -1 && p.track.child.parentSourceIndex == -1
            && !p.cpu.rendererAdopts && !p.track.rendererAdopts
            && p.cpu.verdict == QStringLiteral("EQUAL") && p.track.verdict == QStringLiteral("EQUAL");
        tally.gate(2, ok, "V1 parent adopted (parentSourceIndex != -1) or parentLayerAt hit V1");
    }

    // ── G3: adjustment-clip parent -> neither side adopts ──────────────────
    {
        ClipInfo adj = makeClip(QString(), QStringLiteral("adjustment"), 0.0, 4.0, kParentT);
        adj.isAdjustment = true;
        Timeline tl;
        buildTimeline(tl, spacerThen(adj), parentKey);
        const PathPair p = observeCase(tl, 2.0, 4.0, 1, 1, -1);
        report(3, kCpuPath, p.cpu);
        report(3, kTrackPath, p.track);
        const bool ok = p.cpu.recorded && p.track.recorded
            && !p.cpu.vpAdopts && !p.track.vpAdopts
            && !p.cpu.rendererAdopts && !p.track.rendererAdopts;
        tally.gate(3, ok, "adjustment-clip parent adopted by VideoPlayer or parentLayerAt");
    }

    // Pinned state (current behaviour): per path the verdict label, the
    // adoption operands {vpAdopts, vpCorrectLayer, rendererAdopts} (-1 = not
    // applicable) and a case-specific check. A change on either side fails the
    // gate and must be re-decided (Wave 11 W9-01c B).
    struct PathPin {
        const char *label;
        int vpAdopts;
        int vpCorrectLayer;
        int rendererAdopts;
    };
    struct CaseCheck {
        const char *what;
        std::function<bool(const PathResult &)> holds;
    };
    auto pinMatches = [&](int gate, const PathPair &p, const PathPin &cpuPin,
                          const PathPin &trackPin, const CaseCheck &check, const char *what) {
        auto pathOk = [&](const char *name, const PathResult &r, const PathPin &pin) {
            const QString label = r.verdict == QStringLiteral("DIVERGES")
                ? QStringLiteral("KNOWN-DIVERGENCE") : r.verdict;
            auto operandOk = [](int expected, bool observed) {
                return expected < 0 || (expected != 0) == observed;
            };
            auto expText = [](int expected) {
                return expected < 0 ? std::string("n/a") : std::to_string(expected);
            };
            const bool labelOk = label == QLatin1String(pin.label);
            const bool vpOk = operandOk(pin.vpAdopts, r.vpAdopts);
            const bool correctOk = operandOk(pin.vpCorrectLayer, r.vpCorrectLayer);
            const bool rendererOk = operandOk(pin.rendererAdopts, r.rendererAdopts);
            const bool checkOk = !check.holds || check.holds(r);
            std::cerr << "  G" << gate << ' ' << what << " [" << name << "]"
                      << " label expected=" << pin.label << " observed=" << label.toStdString()
                      << " | vpAdopts expected=" << expText(pin.vpAdopts)
                      << " observed=" << (r.vpAdopts ? 1 : 0)
                      << " | vpCorrectLayer expected=" << expText(pin.vpCorrectLayer)
                      << " observed=" << (r.vpCorrectLayer ? 1 : 0)
                      << " | rendererAdopts expected=" << expText(pin.rendererAdopts)
                      << " observed=" << (r.rendererAdopts ? 1 : 0);
            if (check.holds)
                std::cerr << " | " << check.what << " expected=1 observed=" << (checkOk ? 1 : 0);
            std::cerr << '\n';
            return labelOk && vpOk && correctOk && rendererOk && checkOk;
        };
        // NOT-DRIVEN / WRONG-LAYER are never pinned, so they always fail.
        const bool cpuOk = pathOk(kCpuPath, p.cpu, cpuPin);
        const bool trackOk = pathOk(kTrackPath, p.track, trackPin);
        return cpuOk && trackOk;
    };
    // Negative control: `p` with the cpu-composite operands replaced by a
    // change that keeps the verdict label (the pin must reject it).
    auto controlRejected = [&](int gate, PathPair p, bool vp, bool correct, bool renderer,
                               const PathPin &cpuPin, const PathPin &trackPin,
                               const CaseCheck &check, const char *what) {
        p.cpu.vpAdopts = vp;
        p.cpu.vpCorrectLayer = correct;
        p.cpu.rendererAdopts = renderer;
        const bool rejected = !pinMatches(gate, p, cpuPin, trackPin, check, what);
        std::cerr << "  G" << gate << " control " << what << " rejected=" << (rejected ? 1 : 0) << '\n';
        return rejected;
    };
    auto pinnedGate = [&](int gate, const PathPair &p, const PathPin &cpuPin,
                          const PathPin &trackPin, const CaseCheck &check, const char *what,
                          bool controlOk = true) {
        report(gate, kCpuPath, p.cpu);
        report(gate, kTrackPath, p.track);
        const bool ok = pinMatches(gate, p, cpuPin, trackPin, check, what);
        tally.gate(gate, ok && controlOk,
                   "verdict or adoption operands differ from the pinned EQUAL / SSOT-ROUTED state, or a control was not rejected");
    };

    // ── G4: disabled parent ────────────────────────────────────────────────
    {
        ClipInfo parent = makeClip(parentPath, QStringLiteral("parent"), 0.0, 4.0, kParentT);
        parent.visible = false;
        Timeline tl;
        buildTimeline(tl, spacerThen(parent), parentKey);
        // Control: the disabled clip is the spacer and the parent is
        // enabled. Playback is still routed to renderFrameAt, but the renderer
        // adopts the parent, so the multi-track cell must not be SSOT-ROUTED.
        Timeline ctl;
        buildTimeline(ctl, [&](Timeline &t) {
            ClipInfo spacer = makeClip(spacerPath, QStringLiteral("spacer"), 0.0, 1.0, kParentT);
            spacer.visible = false;
            addClip(t, 1, spacer);
            addClip(t, 1, makeClip(parentPath, QStringLiteral("parent"), 0.0, 4.0, kParentT));
        }, parentKey);
        const PathPair c = observeCase(ctl, 2.0, 4.0, 1, 1, -1);
        report(4, kTrackPath, c.track);
        const bool routedControlOk = c.track.verdict != QStringLiteral("SSOT-ROUTED")
            && c.track.rendererAdopts;
        std::cerr << "  G4 control enabled parent + disabled spacer: multi-track verdict="
                  << c.track.verdict.toStdString()
                  << " rendererAdopts=" << (c.track.rendererAdopts ? 1 : 0)
                  << " notRouted=" << (routedControlOk ? 1 : 0) << '\n';
        // cpu-composite: EQUAL (parentLayerAt rejects the disabled parent,
        // VideoPlayer does not adopt, as in export). multi-track: SSOT-ROUTED
        // (renderFrameAt rejected the disabled parent at every admitted child
        // time; VideoPlayer adoption n/a, so the check is n/a there too).
        const PathPair p = observeCase(tl, 2.0, 4.0, 1, 1, -1);
        const PathPin cpuPin{"EQUAL", 0, 0, 0};
        const PathPin trackPin{"SSOT-ROUTED", -1, -1, 0};
        const CaseCheck check{"vpHit agrees with parentLayerAt (n/a when routed)",
                              [](const PathResult &r) {
            return !r.recorded || r.vpHitAgrees;
        }};
        // Control: cpu-composite both adopt keeps the EQUAL label.
        const bool controlOk = controlRejected(4, p, true, true, true, cpuPin, trackPin,
                                               check, "cpu-composite both adopt");
        pinnedGate(4, p, cpuPin, trackPin, check, "disabled parent",
                   routedControlOk && controlOk);
    }

    // ── G5: overlap, incoming clip (V2 clip 1) as parent ───────────────────
    {
        // A nominal [0, 2), B nominal [2, 4); Center 1 s -> overlap [1.5, 2.5).
        Timeline tl;
        buildTimeline(tl, [&](Timeline &t) {
            ClipInfo a = makeClip(spacerPath, QStringLiteral("A"), 0.0, 2.0, kParentT);
            ClipInfo b = makeClip(parentPath, QStringLiteral("B"), 1.0, 3.0, kParentT);
            setTransition(a, b, TransitionAlignment::Center);
            addClip(t, 1, a);
            addClip(t, 1, b);
        }, parentKey);
        // The renderer picks the outgoing A (V2 clip 0) as the active clip;
        // parentLayerAt(B) = none, so neither side adopts B.
        const PathPair p = observeCase(tl, 1.6, 2.4, 1, 1, 0);
        const PathPin pin{"EQUAL", 0, 0, 0};
        const CaseCheck check{"peerHit(outgoing A) decodable && vpHit agrees (none)",
                              [](const PathResult &r) {
            return r.peerHit.has_value() && r.peerHit->decodable
                && !r.hit.has_value() && r.vpHitAgrees;
        }};
        // Controls: reversed divergence (VP rejects, renderer adopts) and both
        // adopt keep the pinned label (the pin must reject both).
        const bool reversedOk =
            controlRejected(5, p, false, false, true, pin, pin, check, "reversed divergence");
        const bool bothAdoptOk =
            controlRejected(5, p, true, true, true, pin, pin, check, "both adopt");
        pinnedGate(5, p, pin, pin, check, "overlap incoming parent", reversedOk && bothAdoptOk);
    }

    // ── G6: TransitionAlignment::Start borrowed trail ──────────────────────
    {
        // spacer [0, 1), A (clip 1) nominal [1, 3) + 1 s Start trail -> [3, 4),
        // B (clip 2) after it.
        Timeline tl;
        buildTimeline(tl, [&](Timeline &t) {
            addClip(t, 1, makeClip(spacerPath, QStringLiteral("spacer"), 0.0, 1.0, kParentT));
            ClipInfo a = makeClip(parentPath, QStringLiteral("A"), 0.0, 2.0, kParentT);
            ClipInfo b = makeClip(spacerPath, QStringLiteral("B"), 2.0, 4.0, kParentT);
            setTransition(a, b, TransitionAlignment::Start);
            addClip(t, 1, a);
            addClip(t, 1, b);
        }, parentKey);
        // localSec >= 2.0: the hit lies in the borrowed trail past A's
        // nominal 2 s.
        const PathPair p = observeCase(tl, 3.3, 3.9, 1, 1, 2);
        const PathPin pin{"EQUAL", 1, 1, 1};
        const CaseCheck check{"hit.localSec>=2.0 (borrowed trail)", [](const PathResult &r) {
            return r.hit.has_value() && r.hit->clipLocalSeconds >= 2.0;
        }};
        // Control: both sides stop adopting keeps the EQUAL label.
        const bool controlOk = controlRejected(6, p, false, false, false, pin, pin, check,
                                               "both stop adopting");
        pinnedGate(6, p, pin, pin, check, "Start trail parent", controlOk);
    }

    // ── G7: offline parent (file missing) ──────────────────────────────────
    {
        const QString offlinePath = tmpDir.filePath(QStringLiteral("offline-parent.mp4"));
        Timeline tl;
        buildTimeline(tl, spacerThen(makeClip(offlinePath, QStringLiteral("parent"), 0.0, 4.0, kParentT)),
                      parentKey);
        std::cerr << "  G7 offline parent exists=" << (QFile::exists(offlinePath) ? 1 : 0) << '\n';
        const PathPair p = observeCase(tl, 2.0, 4.0, 1, 1, -1);
        const PathPin pin{"EQUAL", 0, 0, 0};
        const CaseCheck check{"file missing && hit && !decodable", [offlinePath](const PathResult &r) {
            return !QFile::exists(offlinePath) && r.hit.has_value() && !r.hit->decodable;
        }};
        // Control: both sides adopt keeps the EQUAL label.
        const bool controlOk = controlRejected(7, p, true, true, true, pin, pin, check,
                                               "both adopt");
        pinnedGate(7, p, pin, pin, check, "offline parent", controlOk);
    }

    // ── G8: observation OFF records nothing ──────────────────────────────────
    {
        const Drive off = drive(g1, 2.0, 4.0, false);
        const bool offEmpty = off.cpu.isEmpty() && off.track.isEmpty();
        const bool pathsRan = off.cpuFrames > 0 && off.trackFrames > 0;
        // Control: the same drive with observation ON (G1) recorded both paths.
        const bool onRecorded = !p1.d.cpu.isEmpty() && !p1.d.track.isEmpty();

        std::cerr << "  G8 off.cpuRecords=" << off.cpu.size()
                  << " off.trackRecords=" << off.track.size()
                  << " off.cpuFrames=" << off.cpuFrames << " off.trackFrames=" << off.trackFrames
                  << " on.cpuRecords=" << p1.d.cpu.size() << " on.trackRecords=" << p1.d.track.size()
                  << '\n'
                  << "  G8 parent-gate call-count invariance (parentLayerAt) is evidenced by"
                     " --selftest=parent-gate 7 PASS (run by the acceptance lane)\n";
        tally.gate(8, offEmpty && pathsRan && onRecorded,
                   "records appeared with observation OFF, a path did not run, or observation ON recorded nothing");
    }

    // ── G9: each admission path produced records (both required) ──────────
    {
        std::cerr << "  G9 cpu-composite recorded=" << (p1.cpu.recorded ? 1 : 0)
                  << " records=" << p1.d.cpu.size()
                  << " | multi-track recorded=" << (p1.track.recorded ? 1 : 0)
                  << " records=" << p1.d.track.size() << '\n';
        tally.gate(9, p1.cpu.recorded && p1.track.recorded,
                   "a required admission path produced no record");
    }

    return tally.finish("[video-player-parent]");
}
