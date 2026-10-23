// src/selftests/parent_gate_selftest.cpp
// --selftest=parent-gate (US-804 PARENT-GATE-SSOT)
//
// Checks tlrender::parentLayerAt, the shared "is (track, clip) composited as a
// parent layer at time t" gate, against what renderFrameAt actually does:
//   G1 normal parent: hit, and its clip-local / source seconds equal the ones
//      the renderer used for that layer.
//   G2 offline parent: hit.decodable=false and the frame equals the
//      "no parent layer" reference.
//   G3 same-track overlap (transition): only the clip the renderer picks hits.
//   G4 TransitionAlignment::Start trail: hit past the nominal end, continuous
//      clip-local seconds with the time just before the end.
//   G5 outside the parent's interval: no hit.
//   G6 renderFrameAt calls the gate at least once per parented layer (0 for a
//      timeline without parent links).
//   G7 visible V1 that has ended at t: renderFrameAt returns an empty frame
//      before any upper layer, and the gate does not hit. A visible V1 with no
//      clip at all is a transparent base (user decision 3, US-1312): the frame
//      and the hit are both kept, as with an active upper shape clip.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>

#include <cmath>
#include <optional>
#include <sstream>
#include <string>

#include "SelftestGate.h"
#include "../ClipGeometry.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TrackMatteKey.h"

namespace {

using tlrender::ParentLayerHit;
using tlrender::detail::ObservedLayerTime;

const QSize kCanvas(64, 64);

bool near(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) <= eps;
}

bool makeSolidClip(const QString &outPath, const QString &color)
{
    QProcess ff;
    ff.start(QStringLiteral("ffmpeg"),
             { QStringLiteral("-hide_banner"),
               QStringLiteral("-loglevel"), QStringLiteral("error"),
               QStringLiteral("-y"),
               QStringLiteral("-f"), QStringLiteral("lavfi"),
               QStringLiteral("-i"),
               QStringLiteral("color=c=%1:s=64x64:r=10:d=6").arg(color),
               QStringLiteral("-c:v"), QStringLiteral("libx264rgb"),
               QStringLiteral("-qp"), QStringLiteral("0"),
               QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"),
               outPath });
    if (!ff.waitForStarted(15000))
        return false;
    ff.waitForFinished(60000);
    return ff.exitStatus() == QProcess::NormalExit
        && ff.exitCode() == 0
        && QFile::exists(outPath);
}

ClipInfo makeClip(const QString &path, const QString &name,
                  double inPoint, double outPoint,
                  const clipgeom::ClipTransform &t = {})
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = name;
    clip.duration = 6.0;
    clip.inPoint = inPoint;
    clip.outPoint = outPoint;
    clip.opacity = 1.0;
    clip.visible = true;
    clip.videoScale = t.videoScale;
    clip.videoDx = t.videoDx;
    clip.videoDy = t.videoDy;
    clip.rotation2DDegrees = t.rotationDeg;
    return clip;
}

ClipInfo makeBaseNull()
{
    ClipInfo clip = makeClip(clipgeom::nullObjectFilePath(),
                             QStringLiteral("base"), 0.0, 6.0);
    clip.opacity = 0.0;
    clip.visible = false;
    return clip;
}

void addClip(Timeline &tl, int trackIdx, const ClipInfo &clip)
{
    while (tl.videoTracks().size() <= trackIdx)
        tl.addVideoTrack();
    if (TimelineTrack *track = tl.videoTracks().value(trackIdx, nullptr))
        track->addClip(clip);
}

void ensureTracks(Timeline &tl, int count)
{
    while (tl.videoTracks().size() < count)
        tl.addVideoTrack();
}

void setTransition(ClipInfo &out, ClipInfo &in, TransitionAlignment alignment)
{
    out.trailOut.type = in.leadIn.type = TransitionType::CrossDissolve;
    out.trailOut.duration = in.leadIn.duration = 1.0;
    out.trailOut.alignment = in.leadIn.alignment = alignment;
}

qint64 usecOf(double sec)
{
    return static_cast<qint64>(std::llround(sec * 1'000'000.0));
}

QVector<ObservedLayerTime> observeRender(const Timeline &tl, double sec,
                                         QImage *frameOut = nullptr)
{
    tlrender::detail::setLayerTimeObservationForTest(true);
    const QImage frame = tlrender::renderFrameAt(&tl, usecOf(sec), kCanvas);
    const QVector<ObservedLayerTime> seen =
        tlrender::detail::observedLayerTimesForTest();
    tlrender::detail::setLayerTimeObservationForTest(false);
    if (frameOut)
        *frameOut = frame;
    return seen;
}

QVector<ObservedLayerTime> onTrack(const QVector<ObservedLayerTime> &seen, int track)
{
    QVector<ObservedLayerTime> out;
    for (const ObservedLayerTime &o : seen)
        if (o.trackIdx == track)
            out.append(o);
    return out;
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

std::string obsText(const char *label, const QVector<ObservedLayerTime> &seen)
{
    std::ostringstream s;
    s.precision(17);
    s << label << "=[";
    for (const ObservedLayerTime &o : seen)
        s << " (" << o.trackIdx << ':' << o.clipIdx << " localSec=" << o.localSec
          << " sourceSec=" << o.sourceSec << ')';
    s << " ]";
    return s.str();
}

void info(int gate, const std::string &text)
{
    std::cerr << "  G" << gate << ' ' << text << '\n';
}

bool sameFrame(const QImage &a, const QImage &b)
{
    return !a.isNull() && !b.isNull() && a == b;
}

} // namespace

int runParentGateSelftest()
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
    const QString redPath = tmpDir.filePath(QStringLiteral("red.mp4"));
    const QString bluePath = tmpDir.filePath(QStringLiteral("blue.mp4"));
    const QString greenPath = tmpDir.filePath(QStringLiteral("green.mp4"));
    if (!tmpDir.isValid()
        || !makeSolidClip(redPath, QStringLiteral("red"))
        || !makeSolidClip(bluePath, QStringLiteral("blue"))
        || !makeSolidClip(greenPath, QStringLiteral("green"))) {
        for (int g = 1; g <= 7; ++g)
            tally.skip(g, "ffmpeg.exe unavailable: cannot generate fixture media");
        return tally.finish("[parent-gate]");
    }

    const clipgeom::ClipTransform parentT{0.5, 0.25, 0.0, 0.0};
    const clipgeom::ClipTransform childT{0.5, 0.0, 0.0, 0.0};
    const QString childKey = trackMatteClipKey(3, 0);
    const QString parentKey = trackMatteClipKey(2, 0);
    const clipgeom::ClipTransform composedChildT =
        clipgeom::composeParented(childT, parentT, kCanvas);

    // ── G1: normal parent, times agree with the renderer ───────────────────
    {
        // Parent starts at timeline 0.5 s with inPoint 1.0 s, so at timeline
        // 1.2 s: local = 0.7 s, source = 1.7 s (three distinct values).
        Timeline tl;
        addClip(tl, 0, makeBaseNull());
        ClipInfo parent = makeClip(bluePath, QStringLiteral("parent"), 1.0, 3.0, parentT);
        parent.leadInSec = 0.5;
        addClip(tl, 2, parent);
        addClip(tl, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childT));
        tl.setClipParent(childKey, parentKey);

        const double t = 1.2;
        const auto hit = tlrender::parentLayerAt(&tl, usecOf(t), 2, 0);
        QImage frame;
        const QVector<ObservedLayerTime> seen = onTrack(observeRender(tl, t, &frame), 2);
        const bool renderedOnce = seen.size() == 1 && seen[0].clipIdx == 0;
        const bool ok = hit && renderedOnce && !frame.isNull()
            && hit->clipLocalSeconds == seen[0].localSec
            && hit->sourceSeconds == seen[0].sourceSec
            && near(hit->timelineSec, 1.2)
            && near(hit->clipLocalSeconds, 0.7)
            && near(hit->sourceSeconds, 1.7)
            && hit->decodable;
        info(1, hitText("parentLayerAt(2:0)", hit));
        info(1, obsText("renderer V2 layers", seen));
        tally.gate(1, ok, "parentLayerAt times differ from the renderer's parent layer times");
    }

    // ── G2: offline parent -> decodable=false, frame == no-parent reference ─
    {
        const QString offlinePath = tmpDir.filePath(QStringLiteral("offline-parent.mp4"));
        Timeline offline;
        addClip(offline, 0, makeBaseNull());
        addClip(offline, 2, makeClip(offlinePath, QStringLiteral("parent"), 0.0, 4.0, parentT));
        addClip(offline, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childT));
        offline.setClipParent(childKey, parentKey);

        Timeline online;
        addClip(online, 0, makeBaseNull());
        addClip(online, 2, makeClip(bluePath, QStringLiteral("parent"), 0.0, 4.0, parentT));
        addClip(online, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childT));
        online.setClipParent(childKey, parentKey);

        Timeline noParent;
        addClip(noParent, 0, makeBaseNull());
        ensureTracks(noParent, 3);
        addClip(noParent, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childT));

        const double t = 0.5;
        const auto hit = tlrender::parentLayerAt(&offline, usecOf(t), 2, 0);
        const auto onlineHit = tlrender::parentLayerAt(&online, usecOf(t), 2, 0);
        const QImage offlineFrame = tlrender::renderFrameAt(&offline, usecOf(t), kCanvas);
        const QImage refFrame = tlrender::renderFrameAt(&noParent, usecOf(t), kCanvas);
        const QImage onlineFrame = tlrender::renderFrameAt(&online, usecOf(t), kCanvas);
        const bool matchesRef = sameFrame(offlineFrame, refFrame);
        // Control: a decodable parent really moves the child, so the
        // reference comparison above is not vacuous.
        const bool onlineDiffers = !onlineFrame.isNull() && onlineFrame != refFrame;
        const bool ok = hit && !hit->decodable && onlineHit && onlineHit->decodable
            && matchesRef && onlineDiffers && !QFile::exists(offlinePath);
        info(2, hitText("offline parentLayerAt(2:0)", hit));
        info(2, hitText("online parentLayerAt(2:0)", onlineHit));
        info(2, std::string("offlineFrame==noParentRef=") + (matchesRef ? "1" : "0")
                + " onlineFrame!=noParentRef=" + (onlineDiffers ? "1" : "0"));
        tally.gate(2, ok, "offline parent not reported undecodable or frame differs from no-parent reference");
    }

    // ── G3: two clips overlap on one track (Center cross dissolve) ─────────
    {
        // A nominal [0, 2), B nominal [2, 4); Center 1 s -> overlap [1.5, 2.5).
        auto build = [&](Timeline &tl, bool aVisible, bool parented,
                         const clipgeom::ClipTransform &childTransform) {
            addClip(tl, 0, makeBaseNull());
            ClipInfo a = makeClip(bluePath, QStringLiteral("A"), 0.0, 2.0, parentT);
            ClipInfo b = makeClip(greenPath, QStringLiteral("B"), 1.0, 3.0, parentT);
            a.visible = aVisible;
            setTransition(a, b, TransitionAlignment::Center);
            addClip(tl, 2, a);
            addClip(tl, 2, b);
            addClip(tl, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childTransform));
            if (parented)
                tl.setClipParent(childKey, parentKey);
        };
        const double t = 1.8;
        Timeline tl;
        build(tl, true, true, childT);
        Timeline direct;
        build(direct, true, false, composedChildT);
        Timeline aOff;
        build(aOff, false, true, childT);

        const auto hitA = tlrender::parentLayerAt(&tl, usecOf(t), 2, 0);
        const auto hitB = tlrender::parentLayerAt(&tl, usecOf(t), 2, 1);
        QImage frame;
        const QVector<ObservedLayerTime> seen = onTrack(observeRender(tl, t, &frame), 2);
        const QImage directFrame = tlrender::renderFrameAt(&direct, usecOf(t), kCanvas);
        const auto offA = tlrender::parentLayerAt(&aOff, usecOf(t), 2, 0);
        const auto offB = tlrender::parentLayerAt(&aOff, usecOf(t), 2, 1);
        const QVector<ObservedLayerTime> seenOff = onTrack(observeRender(aOff, t), 2);

        const bool pickA = hitA && !hitB && seen.size() == 1 && seen[0].clipIdx == 0
            && hitA->clipLocalSeconds == seen[0].localSec;
        // B's layer starts at 1.5 (its borrowed lead), so 1.8 is inside the
        // overlap: with A disabled the renderer and the gate both pick B.
        const bool pickB = !offA && offB && seenOff.size() == 1 && seenOff[0].clipIdx == 1
            && offB->clipLocalSeconds == seenOff[0].localSec
            && near(offB->clipLocalSeconds, 0.3);
        const bool matchesRef = sameFrame(frame, directFrame);
        info(3, hitText("A", hitA) + ' ' + hitText("B", hitB));
        info(3, obsText("renderer V2 layers", seen));
        info(3, hitText("A(disabled)", offA) + ' ' + hitText("B", offB));
        info(3, obsText("renderer V2 layers (A disabled)", seenOff));
        info(3, std::string("parentedFrame==directComposedRef=") + (matchesRef ? "1" : "0"));
        tally.gate(3, pickA && pickB && matchesRef,
                   "gate picked a different overlapping clip than the renderer");
    }

    // ── G4: TransitionAlignment::Start borrowed trail ──────────────────────
    {
        // A nominal [0, 2) with a 1 s Start transition -> A's layer runs to 3.
        auto build = [&](Timeline &tl, bool withTransition, bool parented,
                         const clipgeom::ClipTransform &childTransform) {
            addClip(tl, 0, makeBaseNull());
            ClipInfo a = makeClip(bluePath, QStringLiteral("A"), 0.0, 2.0, parentT);
            ClipInfo b = makeClip(greenPath, QStringLiteral("B"), 0.0, 2.0, parentT);
            if (withTransition)
                setTransition(a, b, TransitionAlignment::Start);
            addClip(tl, 2, a);
            addClip(tl, 2, b);
            addClip(tl, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childTransform));
            if (parented)
                tl.setClipParent(childKey, parentKey);
        };
        const double before = 1.99;
        const double trail = 2.5;
        Timeline tl;
        build(tl, true, true, childT);
        Timeline direct;
        build(direct, true, false, composedChildT);
        Timeline plain;
        build(plain, false, true, childT);

        const auto hBefore = tlrender::parentLayerAt(&tl, usecOf(before), 2, 0);
        const auto hTrail = tlrender::parentLayerAt(&tl, usecOf(trail), 2, 0);
        const auto hTrailB = tlrender::parentLayerAt(&tl, usecOf(trail), 2, 1);
        const auto hPlain = tlrender::parentLayerAt(&plain, usecOf(trail), 2, 0);
        QImage frame;
        const QVector<ObservedLayerTime> seen = onTrack(observeRender(tl, trail, &frame), 2);
        const QImage directFrame = tlrender::renderFrameAt(&direct, usecOf(trail), kCanvas);

        const bool continuous = hBefore && hTrail
            && hTrail->clipLocalSeconds > 2.0
            && near(hTrail->clipLocalSeconds - hBefore->clipLocalSeconds, trail - before)
            && near(hTrail->sourceSeconds - hBefore->sourceSeconds, trail - before);
        const bool rendererAgrees = hTrail && seen.size() == 1 && seen[0].clipIdx == 0
            && hTrail->clipLocalSeconds == seen[0].localSec
            && hTrail->sourceSeconds == seen[0].sourceSec;
        const bool matchesRef = sameFrame(frame, directFrame);
        const bool ok = continuous && rendererAgrees && !hTrailB && !hPlain && matchesRef;
        info(4, hitText("before(1.99)", hBefore));
        info(4, hitText("trail(2.5)", hTrail) + ' ' + hitText("B@trail", hTrailB));
        info(4, hitText("noTransition@2.5", hPlain));
        info(4, obsText("renderer V2 layers @2.5", seen));
        info(4, std::string("parentedFrame==directComposedRef=") + (matchesRef ? "1" : "0"));
        tally.gate(4, ok, "Start-aligned trail not hit or clip-local seconds discontinuous");
    }

    // ── G5: outside the parent's interval -> no hit ────────────────────────
    {
        // Parent nominal [1, 3), no transitions.
        Timeline tl;
        addClip(tl, 0, makeBaseNull());
        ClipInfo parent = makeClip(bluePath, QStringLiteral("parent"), 0.0, 2.0, parentT);
        parent.leadInSec = 1.0;
        addClip(tl, 2, parent);
        addClip(tl, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childT));
        tl.setClipParent(childKey, parentKey);

        const auto hPre = tlrender::parentLayerAt(&tl, usecOf(0.5), 2, 0);
        const auto hIn = tlrender::parentLayerAt(&tl, usecOf(2.0), 2, 0);
        const auto hEnd = tlrender::parentLayerAt(&tl, usecOf(3.0), 2, 0);
        const auto hPost = tlrender::parentLayerAt(&tl, usecOf(3.5), 2, 0);
        const QVector<ObservedLayerTime> seenPre = onTrack(observeRender(tl, 0.5), 2);
        const QVector<ObservedLayerTime> seenPost = onTrack(observeRender(tl, 3.5), 2);
        const bool ok = !hPre && !hEnd && !hPost && hIn
            && near(hIn->clipLocalSeconds, 1.0)
            && seenPre.isEmpty() && seenPost.isEmpty();
        info(5, hitText("t=0.5", hPre) + ' ' + hitText("t=2.0", hIn));
        info(5, hitText("t=3.0(end)", hEnd) + ' ' + hitText("t=3.5", hPost));
        info(5, obsText("renderer V2 layers @0.5", seenPre) + ' '
                + obsText("@3.5", seenPost));
        tally.gate(5, ok, "gate hit outside the parent's interval");
    }

    // ── G6: renderer calls the gate per parented layer ─────────────────────
    {
        // Two children (V3, V4) parented to the same V2 parent.
        auto build = [&](Timeline &tl, bool parented) {
            addClip(tl, 0, makeBaseNull());
            addClip(tl, 2, makeClip(bluePath, QStringLiteral("parent"), 0.0, 4.0, parentT));
            addClip(tl, 3, makeClip(redPath, QStringLiteral("child1"), 0.0, 4.0, childT));
            addClip(tl, 4, makeClip(redPath, QStringLiteral("child2"), 0.0, 4.0,
                                    clipgeom::ClipTransform{0.25, -0.25, 0.0, 0.0}));
            if (parented) {
                tl.setClipParent(trackMatteClipKey(3, 0), parentKey);
                tl.setClipParent(trackMatteClipKey(4, 0), parentKey);
            }
        };
        constexpr quint64 kParentedLayers = 2;
        Timeline parented;
        build(parented, true);
        Timeline unparented;
        build(unparented, false);

        // The counter only runs while the test switch is ON.
        tlrender::detail::setLayerTimeObservationForTest(true);
        tlrender::detail::resetParentLayerAtCallCountForTest();
        const QImage parentedFrame = tlrender::renderFrameAt(&parented, usecOf(0.5), kCanvas);
        const quint64 parentedCalls = tlrender::detail::parentLayerAtCallCountForTest();
        tlrender::detail::resetParentLayerAtCallCountForTest();
        const QImage unparentedFrame = tlrender::renderFrameAt(&unparented, usecOf(0.5), kCanvas);
        const quint64 unparentedCalls = tlrender::detail::parentLayerAtCallCountForTest();
        tlrender::detail::setLayerTimeObservationForTest(false);
        // The parent links really changed the picture (layers were resolved).
        const bool resolved = !parentedFrame.isNull() && !unparentedFrame.isNull()
            && parentedFrame != unparentedFrame;
        const bool ok = parentedCalls >= kParentedLayers && unparentedCalls == 0 && resolved;
        info(6, "calls(parented)=" + std::to_string(parentedCalls)
                + " parentedLayers=" + std::to_string(kParentedLayers)
                + " calls(unparented)=" + std::to_string(unparentedCalls)
                + " parentedFrame!=unparentedFrame=" + (resolved ? "1" : "0"));
        tally.gate(6, ok, "renderer did not route each parented layer through parentLayerAt");
    }

    // ── G7: V1 ended at t -> empty frame and no hit; clip-less V1 -> both ──
    {
        // (a) V1 clip [0, 1), parent V2 [0, 4): at 2.0 V1 has ended.
        Timeline ended;
        addClip(ended, 0, makeClip(bluePath, QStringLiteral("v1"), 0.0, 1.0));
        addClip(ended, 2, makeClip(bluePath, QStringLiteral("parent"), 0.0, 4.0, parentT));
        addClip(ended, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childT));
        ended.setClipParent(childKey, parentKey);
        // (b) V1 track exists but has no clips: a transparent base (US-1312).
        Timeline emptyV1;
        ensureTracks(emptyV1, 1);
        addClip(emptyV1, 2, makeClip(bluePath, QStringLiteral("parent"), 0.0, 4.0, parentT));
        addClip(emptyV1, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childT));
        emptyV1.setClipParent(childKey, parentKey);
        // (c) Control: V1 empty but an upper shape clip is active, so the
        // renderer keeps compositing and the parent does hit.
        Timeline shapeKeeps;
        ensureTracks(shapeKeeps, 1);
        addClip(shapeKeeps, 2, makeClip(bluePath, QStringLiteral("parent"), 0.0, 4.0, parentT));
        addClip(shapeKeeps, 3, makeClip(redPath, QStringLiteral("child"), 0.0, 4.0, childT));
        {
            Shape rect;
            rect.type = ShapeType::Rectangle;
            rect.position = QPointF(kCanvas.width() / 2.0, kCanvas.height() / 2.0);
            rect.properties.size = QSizeF(kCanvas.width() * 0.25, kCanvas.height() * 0.25);
            rect.fill.color = Qt::white;
            rect.stroke.enabled = false;
            ClipInfo shapeClip;
            shapeClip.displayName = QStringLiteral("shape");
            shapeClip.duration = 4.0;
            shapeClip.outPoint = 4.0;
            shapeClip.shapes = {rect};
            addClip(shapeKeeps, 4, shapeClip);
        }
        shapeKeeps.setClipParent(childKey, parentKey);

        const auto hEndedIn = tlrender::parentLayerAt(&ended, usecOf(0.5), 2, 0);
        const auto hEnded = tlrender::parentLayerAt(&ended, usecOf(2.0), 2, 0);
        const QImage endedFrame = tlrender::renderFrameAt(&ended, usecOf(2.0), kCanvas);
        const QImage endedInFrame = tlrender::renderFrameAt(&ended, usecOf(0.5), kCanvas);
        const auto hEmpty = tlrender::parentLayerAt(&emptyV1, usecOf(0.5), 2, 0);
        const QImage emptyFrame = tlrender::renderFrameAt(&emptyV1, usecOf(0.5), kCanvas);
        const auto hShape = tlrender::parentLayerAt(&shapeKeeps, usecOf(0.5), 2, 0);
        QImage shapeFrame;
        const QVector<ObservedLayerTime> seenShape =
            onTrack(observeRender(shapeKeeps, 0.5, &shapeFrame), 2);
        const bool shapeAgrees = hShape && !shapeFrame.isNull()
            && seenShape.size() == 1 && seenShape[0].clipIdx == 0
            && hShape->clipLocalSeconds == seenShape[0].localSec;
        const bool ok = hEndedIn && !endedInFrame.isNull()
            && !hEnded && endedFrame.isNull()
            && hEmpty && !emptyFrame.isNull()
            && shapeAgrees;
        info(7, hitText("V1ended t=0.5", hEndedIn) + " frameNull="
                + (endedInFrame.isNull() ? "1" : "0"));
        info(7, hitText("V1ended t=2.0", hEnded) + " frameNull="
                + (endedFrame.isNull() ? "1" : "0"));
        info(7, hitText("V1empty t=0.5", hEmpty) + " frameNull="
                + (emptyFrame.isNull() ? "1" : "0"));
        info(7, hitText("V1empty+shape t=0.5", hShape) + " frameNull="
                + (shapeFrame.isNull() ? "1" : "0") + ' '
                + obsText("renderer V2 layers", seenShape));
        tally.gate(7, ok, "gate hit while renderFrameAt returned an empty frame (or vice versa)");
    }

    return tally.finish("[parent-gate]");
}
