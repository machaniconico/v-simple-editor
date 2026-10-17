#include "../AudioMixer.h"
#include "../MainWindow.h"
#include "../ProjectDiff.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TrackMatteKey.h"
#include "../UndoManager.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpToolRegistry.h"
#include "SelftestGate.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>

// connected-clip (US-1201): primary-storyline connections (ClipInfo::anchorUid /
// anchorOffsetSec) reconciled inside Timeline::saveUndoState.
//   G1: connectClip stores the anchor uid and the start difference, keeps the
//       position; a V1 clip or a V2 anchor is rejected (controls).
//   G2: a 3-point insert at the head of V1 moves the connected V2 clip, and a
//       connected V3 clip sharing its uid, by the same amount while an
//       unconnected V2 clip stays (control). Detects a missing reconcile
//       (connected clip stays), a reconcile that moves everything (control
//       moves) and a baseline keyed without the track (the V2 clip re-derives).
//   G3: undo -> redo of G2 restores the followed position, and the edit took
//       exactly one undo point. Detects a reconcile placed after saveState
//       (redo returns the pre-follow position).
//   G4: MCP move_clip (setClips path) moves the anchor; the connection follows.
//       Detects a reconcile hooked to TimelineTrack::modified.
//   G5: an all-track ripple delete moves anchor and connected clip once; the
//       offset is unchanged. Detects a double move. Deleting a linked V1 anchor
//       whose longer A1 partner shares its uid detaches (with a warning) a
//       connection outside the V1 piece; recording the A1 length deletes it.
//   G6: (a) the connected clip overtakes an unconnected one with roto / parent
//       entries: both keep their entries (an alignedClipKeyMap remap drops
//       Y's), (b) a collision moves a matte-carrying clip to the free V3 with
//       its matte entry and the same track count; on the MainWindow timeline
//       (external undo hooks) the undo restores the pre-edit matte key and
//       keeps a solo toggled with no undo point (AudioMixer state == track
//       header; a new track makes undo apply the stale external mixer), (c) the
//       same with a nested sequence keeps sequences().size() (dropping the
//       store key empties it), (d) with no free track X goes back to its start,
//       detached with one warning, track count unchanged, undo reconnects (a
//       reconcile that adds a track fails the count), (e) an evacuation skips
//       V3 holding an unrelated Y with X's uid for the free V4, and deleting the
//       anchor deletes X and keeps Y (a first-uid-match removal deletes Y).
//       (f) connected X (V2, collides with Z) and connected Y (V3) share a
//       uid: X skips V3 reserved for the pending Y and goes back to its V2 start
//       detached, Y follows on V3, each roto entry stays with its clip, undo /
//       redo round-trip (an unreserved V3 gets both, and the uid remap swaps
//       Y's entry onto X and drops X's). (g) connected uid twins X (V2) and Y
//       (V3) both evacuate (Y to V4, X past V3 and V4 to V5): each roto /
//       parent entry stays with its clip through undo / redo (a track-order
//       pairing of the leftover keys swaps them). (h) a connected X with no
//       slot and its linked connected A1 partner whose slot is free, (i) the
//       same pair with X's track locked: both stay at 1, detached, one warning,
//       undo reconnects (a per-clip fallback moves the partner connected).
//   G7: save / load keeps both fields, a missing anchor detaches with one
//       warning, ProjectDiff ignores the offset but not filePath (control). A
//       uid held by two V1 clips is rejected by connectClip (state unchanged)
//       and its existing connection detaches with one warning on the next
//       reconcile (a first-match anchor lookup follows one of them).
//   G8: moving the connected clip on V2 re-derives the offset; moving it onto
//       V1 detaches it.
//   G9: after switching to sequence B (empty baseline) an unrelated edit leaves
//       B's connected clip in place and re-derives its stale offset.
//   G10: right after a load the undo stack has one entry, every position is
//       the file's, and a stale offset is re-derived (no snap).
//   G11: (a) offset -1 + unrelated edit, (b) end trim past the connection,
//       (c) linear anchor: ripple head trim d -> -d, non-ripple head trim -> 0,
//       speed 2x -> same source frame, (d) reversed anchor ripple head trim ->
//       no move and no warning.
namespace {

ClipInfo clipFor(const QString &name, double duration, double leadInSec = 0.0)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("/selftests/connected-%1.mp4").arg(name);
    clip.displayName = name;
    clip.duration = duration;
    clip.outPoint = duration;
    clip.leadInSec = leadInSec;
    return clip;
}

struct Located {
    bool found = false;
    int track = -1;
    int index = -1;
    double startSec = 0.0;
    ClipInfo clip;
};

Located locate(const Timeline &timeline, bool audio, const QString &uid)
{
    Located result;
    const auto &tracks = audio ? timeline.audioTracks() : timeline.videoTracks();
    for (int t = 0; t < tracks.size(); ++t) {
        double cursor = 0.0;
        const auto &clips = tracks[t]->clips();
        for (int i = 0; i < clips.size(); ++i) {
            cursor += clips[i].leadInSec;
            if (clips[i].uid == uid) {
                result = {true, t, i, cursor, clips[i]};
                return result;
            }
            cursor += clips[i].effectiveDuration();
        }
    }
    return result;
}

// By display name, for fixtures whose clips share a uid.
Located locateByName(const Timeline &timeline, bool audio, const QString &name)
{
    const auto &tracks = audio ? timeline.audioTracks() : timeline.videoTracks();
    for (int t = 0; t < tracks.size(); ++t) {
        double cursor = 0.0;
        const auto &clips = tracks[t]->clips();
        for (int i = 0; i < clips.size(); ++i) {
            cursor += clips[i].leadInSec;
            if (clips[i].displayName == name)
                return {true, t, i, cursor, clips[i]};
            cursor += clips[i].effectiveDuration();
        }
    }
    return {};
}

QString uidAt(const Timeline &timeline, int track, int index)
{
    const TimelineTrack *t = timeline.trackAt(false, track);
    return t && index < t->clipCount() ? t->clips().at(index).uid : QString();
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

void load(Timeline &timeline, const QVector<QVector<ClipInfo>> &video,
          const QVector<QVector<ClipInfo>> &audio = {{}})
{
    timeline.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    timeline.undoManager()->clear();
    timeline.saveUndoState(QStringLiteral("Initial"));
    timeline.takeConnectionWarnings();
}

// Replace V1 clip 0 through setClips (a simulated trim / speed edit) and commit.
void editAnchor(Timeline &timeline, const std::function<void(ClipInfo &)> &edit)
{
    TimelineTrack *v1 = timeline.trackAt(false, 0);
    QVector<ClipInfo> clips = v1->clips();
    edit(clips[0]);
    v1->setClips(clips);
    timeline.saveUndoState(QStringLiteral("anchor edit"));
}

QJsonObject callTool(mcp::McpToolRegistry &registry, const QString &name,
                     const QJsonObject &args, QString *err)
{
    bool found = false;
    err->clear();
    const QJsonObject result = registry.callTool(name, args, &found, err);
    if (!found) *err = QStringLiteral("tool not found: ") + name;
    return result;
}

} // namespace

int runConnectedClipSelftest()
{
    selftest::GateTally tally;

    // G1: connect, fields, position, rejected controls.
    {
        Timeline timeline;
        load(timeline, {{clipFor("a", 5.0), clipFor("b", 10.0)}, {clipFor("x", 2.0, 7.0)}});
        const QString bUid = uidAt(timeline, 0, 1);
        const QString xUid = uidAt(timeline, 1, 0);
        QString err;
        const bool connected = timeline.connectClip(TrackKind::Video, 1, 0, bUid, &err);
        const Located x = locate(timeline, false, xUid);
        QString v1Err, v2Err;
        const bool v1Rejected = !timeline.connectClip(TrackKind::Video, 0, 0, bUid, &v1Err);
        const bool v2AnchorRejected = !timeline.connectClip(TrackKind::Video, 1, 0, xUid, &v2Err);
        std::fprintf(stderr, "connected-clip G1 connected=%d anchorUid=%s offset=%.6f startSec=%.6f "
                     "v1Rejected=%d [%s] v2AnchorRejected=%d [%s]\n", connected,
                     qPrintable(x.clip.anchorUid), x.clip.anchorOffsetSec, x.startSec, v1Rejected,
                     qPrintable(v1Err), v2AnchorRejected, qPrintable(v2Err));
        tally.gate(1, connected && x.clip.anchorUid == bUid && near(x.clip.anchorOffsetSec, 2.0)
                   && near(x.startSec, 7.0) && v1Rejected && v2AnchorRejected,
                   "connectClip did not store anchorUid / offset 2 at the unchanged start 7, "
                   "or a V1 clip / V2 anchor was accepted");
    }

    // G2 / G3: 3-point insert at the V1 head, then undo / redo.
    {
        Timeline timeline;
        load(timeline, {{clipFor("b", 10.0)}, {clipFor("y", 1.0), clipFor("x", 2.0, 3.0)},
                        {clipFor("x3", 2.0, 3.0)}});
        const QString bUid = uidAt(timeline, 0, 0);
        const QString yUid = uidAt(timeline, 1, 0);
        const QString xUid = uidAt(timeline, 1, 1);
        // V3 holds a second connected clip sharing X's uid (setClips only
        // de-duplicates within a track); both must follow on their own track.
        TimelineTrack *v3 = timeline.trackAt(false, 2);
        QVector<ClipInfo> v3Clips = v3->clips();
        v3Clips[0].uid = xUid;
        v3->setClips(v3Clips);
        timeline.connectClip(TrackKind::Video, 1, 1, bUid);
        timeline.connectClip(TrackKind::Video, 2, 0, bUid);
        const quint64 serialBefore = timeline.undoManager()->saveSerial();
        timeline.insertClip3PointActive(0.0, clipFor("ins", 3.0));
        const quint64 serialAfter = timeline.undoManager()->saveSerial();
        const Located x = locate(timeline, false, xUid);
        const Located y = locate(timeline, false, yUid);
        const Located b = locate(timeline, false, bUid);
        const ClipInfo x3 = timeline.trackAt(false, 2)->clips().value(0);
        const double x3Start = x3.leadInSec;
        std::fprintf(stderr, "connected-clip G2 anchorStart=%.6f connectedStart=%.6f (expect 7) "
                     "unconnectedStart=%.6f (expect 0) offset=%.6f sharedUidV2Track=%d "
                     "sharedUidV3 uid=%d start=%.6f (expect 6) offset=%.6f (expect 3)\n",
                     b.startSec, x.startSec, y.startSec, x.clip.anchorOffsetSec, x.track,
                     x3.uid == xUid, x3Start, x3.anchorOffsetSec);
        tally.gate(2, near(b.startSec, 3.0) && near(x.startSec, 7.0) && near(y.startSec, 0.0)
                   && x.track == 1 && x.clip.anchorUid == bUid && near(x.clip.anchorOffsetSec, 4.0)
                   && x3.uid == xUid && x3.anchorUid == bUid && near(x3Start, 6.0)
                   && near(x3.anchorOffsetSec, 3.0),
                   "a connected clip (V2, or V3 sharing its uid) did not follow the 3 s insert "
                   "or the unconnected one moved");

        timeline.undo();
        const double undoneStart = locate(timeline, false, xUid).startSec;
        timeline.redo();
        const Located redone = locate(timeline, false, xUid);
        std::fprintf(stderr, "connected-clip G3 undone=%.6f (expect 4) redone=%.6f (expect 7) "
                     "serialDelta=%llu (expect 1) serialAfterRedo=%llu\n", undoneStart,
                     redone.startSec, static_cast<unsigned long long>(serialAfter - serialBefore),
                     static_cast<unsigned long long>(timeline.undoManager()->saveSerial()));
        tally.gate(3, near(undoneStart, 4.0) && near(redone.startSec, 7.0)
                   && serialAfter - serialBefore == 1
                   && timeline.undoManager()->saveSerial() == serialAfter,
                   "undo / redo did not restore the followed position or the edit took more "
                   "than one undo point");
    }

    MainWindow window;
    window.hide();
    Timeline *mainTimeline = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
    mcp::McpToolRegistry registry;
    mcp::McpEditorTools tools(&window, &registry);
    tools.registerReadTools();
    tools.registerWriteTools();

    // G4: MCP move_clip moves the anchor.
    if (!mainTimeline) {
        tally.gate(4, false, "MainWindow has no mainTimeline");
    } else {
        load(*mainTimeline, {{clipFor("b", 10.0)}, {clipFor("y", 1.0), clipFor("x", 2.0, 3.0)}});
        const QString bUid = uidAt(*mainTimeline, 0, 0);
        const QString yUid = uidAt(*mainTimeline, 1, 0);
        const QString xUid = uidAt(*mainTimeline, 1, 1);
        mainTimeline->connectClip(TrackKind::Video, 1, 1, bUid);
        QString err;
        const QJsonObject moved = callTool(registry, QStringLiteral("move_clip"),
            QJsonObject{{"trackIndex", 0}, {"clipIndex", 0}, {"newStartSec", 2.0}}, &err);
        const Located x = locate(*mainTimeline, false, xUid);
        const Located y = locate(*mainTimeline, false, yUid);
        std::fprintf(stderr, "connected-clip G4 ok=%d err=[%s] anchorStart=%.6f connectedStart=%.6f "
                     "(expect 6) unconnectedStart=%.6f (expect 0)\n",
                     moved.value("ok").toBool(), qPrintable(err),
                     locate(*mainTimeline, false, bUid).startSec, x.startSec, y.startSec);
        tally.gate(4, moved.value("ok").toBool() && near(x.startSec, 6.0) && near(y.startSec, 0.0),
                   "the connection did not follow an MCP move_clip of the anchor");
    }

    // G5: all-track ripple delete before the anchor.
    {
        Timeline timeline;
        load(timeline, {{clipFor("a", 3.0), clipFor("b", 10.0)}, {clipFor("x", 2.0, 6.0)}});
        const QString bUid = uidAt(timeline, 0, 1);
        const QString xUid = uidAt(timeline, 1, 0);
        timeline.connectClip(TrackKind::Video, 1, 0, bUid);
        timeline.trackAt(false, 0)->setSelectedClip(0);
        timeline.rippleDeleteSelectedClip();
        const Located x = locate(timeline, false, xUid);
        const Located b = locate(timeline, false, bUid);
        std::fprintf(stderr, "connected-clip G5 anchorStart=%.6f (expect 0) connectedStart=%.6f "
                     "(expect 3) offset=%.6f (expect 3)\n", b.startSec, x.startSec,
                     x.clip.anchorOffsetSec);

        // Linked V1 anchor (6 s) + A1 partner (10 s) sharing its uid: deleting
        // the group records only the V1 length, so a connection at offset 8
        // (outside the deleted V1 piece) is detached with a warning, not deleted.
        Timeline linked;
        load(linked, {{clipFor("v", 6.0)}, {clipFor("w", 2.0, 8.0)}}, {{clipFor("va", 10.0)}});
        const QString vUid = uidAt(linked, 0, 0);
        const QString wUid = uidAt(linked, 1, 0);
        TimelineTrack *v1 = linked.trackAt(false, 0);
        TimelineTrack *a1 = linked.trackAt(true, 0);
        QVector<ClipInfo> v1Clips = v1->clips();
        QVector<ClipInfo> a1Clips = a1->clips();
        v1Clips[0].linkGroup = 7;
        a1Clips[0].linkGroup = 7;
        a1Clips[0].uid = vUid;
        v1->setClips(v1Clips);
        a1->setClips(a1Clips);
        linked.connectClip(TrackKind::Video, 1, 0, vUid);
        linked.takeConnectionWarnings();
        QString deleteErr;
        const bool deleted = linked.deleteClipByIndex(false, 0, 0, false, &deleteErr);
        const Located w = locate(linked, false, wUid);
        const QStringList linkedWarnings = linked.takeConnectionWarnings();
        std::fprintf(stderr, "connected-clip G5 linked deleted=%d [%s] sharedUid=%d v1Clips=%d "
                     "a1Clips=%d w found=%d (expect 1) anchorUid=[%s] (expect empty) "
                     "start=%.6f (expect 8) warnings=%lld (expect 1)\n", deleted,
                     qPrintable(deleteErr), a1Clips[0].uid == vUid, v1->clipCount(),
                     a1->clipCount(), w.found, qPrintable(w.clip.anchorUid), w.startSec,
                     static_cast<long long>(linkedWarnings.size()));
        const bool linkedOk = deleted && v1->clipCount() == 0 && a1->clipCount() == 0 && w.found
            && w.clip.anchorUid.isEmpty() && near(w.startSec, 8.0) && linkedWarnings.size() == 1;
        tally.gate(5, near(b.startSec, 0.0) && near(x.startSec, 3.0)
                   && near(x.clip.anchorOffsetSec, 3.0) && x.clip.anchorUid == bUid && linkedOk,
                   "the ripple delete moved the connected clip twice or changed its offset, or "
                   "a linked A1 partner's length decided the deletion of a V1 anchor's connection");
    }

    // G6: carriers follow by uid.
    {
        // (a) X (connected, roto + parent -> Y) overtakes Y (unconnected, roto).
        Timeline timeline;
        load(timeline, {{clipFor("b", 10.0)}, {clipFor("x", 1.0, 1.0), clipFor("y", 1.0, 1.0)}});
        const QString bUid = uidAt(timeline, 0, 0);
        const QString xUid = uidAt(timeline, 1, 0);
        const QString yUid = uidAt(timeline, 1, 1);
        timeline.connectClip(TrackKind::Video, 1, 0, bUid);
        RotoClipEntry xRoto;
        xRoto.brushMask = QImage(1, 1, QImage::Format_ARGB32);
        RotoClipEntry yRoto;
        yRoto.brushMask = QImage(2, 2, QImage::Format_ARGB32);
        timeline.setRotoEntries({{QStringLiteral("1:0"), xRoto}, {QStringLiteral("1:1"), yRoto}});
        timeline.setClipParent(QStringLiteral("1:0"), QStringLiteral("1:1"));
        timeline.absorbIntoCurrentUndoPoint();
        Timeline::MoveClipResult result;
        timeline.moveClipByIndex(false, 0, 0, 5.0, 0, &result, nullptr);
        const Located x = locate(timeline, false, xUid);
        const Located y = locate(timeline, false, yUid);
        const auto roto = timeline.rotoEntries();
        const auto parents = timeline.clipParentEntries();
        const QString xKey = QStringLiteral("%1:%2").arg(x.track).arg(x.index);
        const QString yKey = QStringLiteral("%1:%2").arg(y.track).arg(y.index);
        const bool aOk = near(x.startSec, 6.0) && x.index == 1 && y.index == 0
            && roto.value(xKey).brushMask.width() == 1 && roto.value(yKey).brushMask.width() == 2
            && parents.value(xKey) == yKey;
        std::fprintf(stderr, "connected-clip G6a xStart=%.6f xKey=%s yKey=%s rotoX=%d rotoY=%d "
                     "parent[x]=%s rotoCount=%lld\n", x.startSec, qPrintable(xKey), qPrintable(yKey),
                     roto.value(xKey).brushMask.width(), roto.value(yKey).brushMask.width(),
                     qPrintable(parents.value(xKey)), static_cast<long long>(roto.size()));

        // (b) / (c) X (connected, matte with source Z, parent of Z) collides with Z.
        auto collision = [&](bool nested, const char *label) {
            Timeline tl;
            load(tl, {{clipFor("b", 10.0)}, {clipFor("x", 2.0, 1.0), clipFor("z", 2.0, 1.0)}, {}});
            if (nested) {
                TimelineSequence sequence;
                sequence.id = QStringLiteral("nested");
                sequence.name = QStringLiteral("Nested");
                sequence.videoTracks = {{clipFor("n", 3.0)}};
                sequence.audioTracks = {{}};
                tl.addSequence(sequence);
                tl.saveUndoState(QStringLiteral("nested"));
            }
            const QString anchor = uidAt(tl, 0, 0);
            const QString xId = uidAt(tl, 1, 0);
            const QString zId = uidAt(tl, 1, 1);
            tl.connectClip(TrackKind::Video, 1, 0, anchor);
            TimelineTrackMatteEntry matte;
            matte.matteType = TrackMatteType::AlphaMatte;
            matte.matteSourceClipId = QStringLiteral("1:1");
            tl.setTrackMatteEntries({{QStringLiteral("1:0"), matte}});
            tl.setClipParent(QStringLiteral("1:1"), QStringLiteral("1:0"));
            tl.absorbIntoCurrentUndoPoint();
            const int tracksBefore = tl.videoTrackCount();
            const int sequencesBefore = tl.sequences().size();
            Timeline::MoveClipResult moveResult;
            tl.moveClipByIndex(false, 0, 0, 2.5, 0, &moveResult, nullptr);
            const Located xl = locate(tl, false, xId);
            const Located zl = locate(tl, false, zId);
            const QString xKey2 = QStringLiteral("%1:%2").arg(xl.track).arg(xl.index);
            const QString zKey2 = QStringLiteral("%1:%2").arg(zl.track).arg(zl.index);
            const auto mattes = tl.trackMatteEntries();
            const auto parentMap = tl.clipParentEntries();
            const bool ok = xl.track == 2 && near(xl.startSec, 3.5)
                && tl.videoTrackCount() == tracksBefore
                && mattes.size() == 1 && mattes.contains(xKey2)
                && mattes.value(xKey2).matteSourceClipId == zKey2
                && parentMap.value(zKey2) == xKey2
                && tl.sequences().size() == sequencesBefore;
            std::fprintf(stderr, "connected-clip G6%s xKey=%s (track %d start %.6f) zKey=%s "
                         "tracks %d->%d matteKeys=%s matteSource=%s parent[z]=%s sequences %d->%lld\n",
                         label, qPrintable(xKey2), xl.track, xl.startSec, qPrintable(zKey2),
                         tracksBefore, tl.videoTrackCount(),
                         qPrintable(QStringList(mattes.keys()).join(',')),
                         qPrintable(mattes.value(xKey2).matteSourceClipId),
                         qPrintable(parentMap.value(zKey2)), sequencesBefore,
                         static_cast<long long>(tl.sequences().size()));
            return std::make_pair(ok, sequencesBefore);
        };
        const auto b = collision(false, "b");
        const auto c = collision(true, "c");

        // (d) No free track (V2 only): X goes back to its own start, detached
        // with one warning; the track count stays and undo brings the connection back.
        bool dOk = false;
        {
            Timeline tl;
            load(tl, {{clipFor("b", 10.0)}, {clipFor("x", 2.0, 1.0), clipFor("z", 2.0, 1.0)}});
            const QString anchor = uidAt(tl, 0, 0);
            const QString xId = uidAt(tl, 1, 0);
            tl.connectClip(TrackKind::Video, 1, 0, anchor);
            tl.takeConnectionWarnings();
            const int tracksBefore = tl.videoTrackCount();
            Timeline::MoveClipResult moveResult;
            tl.moveClipByIndex(false, 0, 0, 2.5, 0, &moveResult, nullptr);
            const Located xl = locate(tl, false, xId);
            const int warnings = tl.takeConnectionWarnings().size();
            const int tracksAfter = tl.videoTrackCount();
            tl.undo();
            const Located xu = locate(tl, false, xId);
            dOk = xl.track == 1 && near(xl.startSec, 1.0) && xl.clip.anchorUid.isEmpty()
                && warnings == 1 && tracksAfter == tracksBefore
                && xu.clip.anchorUid == anchor && near(xu.startSec, 1.0);
            std::fprintf(stderr, "connected-clip G6d xTrack=%d (expect 1) xStart=%.6f (expect 1) "
                         "anchorUid=[%s] (expect empty) warnings=%d (expect 1) tracks %d->%d "
                         "(expect unchanged) undo anchorUid=%d (expect 1) start=%.6f\n", xl.track,
                         xl.startSec, qPrintable(xl.clip.anchorUid), warnings, tracksBefore,
                         tracksAfter, xu.clip.anchorUid == anchor, xu.startSec);
        }

        // (e) V3 holds an unconnected Y with X's uid (load fixture; uids are
        // unique only per track). X collides with Z on V2 and evacuates past V3
        // to the free V4; deleting the anchor then deletes X and keeps Y.
        bool eOk = false;
        {
            ClipInfo x = clipFor("x", 2.0, 1.0);
            ClipInfo y = clipFor("y", 2.0);
            x.uid = y.uid = QStringLiteral("selftest-shared-uid");
            Timeline tl;
            load(tl, {{clipFor("b", 10.0)}, {x, clipFor("z", 2.0, 1.0)}, {y}, {}});
            const QString anchor = uidAt(tl, 0, 0);
            const QString xId = uidAt(tl, 1, 0);
            const QString zId = uidAt(tl, 1, 1);
            const bool sharedLoaded = xId == x.uid && uidAt(tl, 2, 0) == x.uid;
            tl.connectClip(TrackKind::Video, 1, 0, anchor);
            // X carries roto, a matte sourced from Z and a parent (Z) at its V2
            // key; Y carries roto at its V3 key.
            RotoClipEntry xRoto;
            xRoto.brushMask = QImage(3, 3, QImage::Format_ARGB32);
            RotoClipEntry yRoto;
            yRoto.brushMask = QImage(4, 4, QImage::Format_ARGB32);
            tl.setRotoEntries({{trackMatteClipKey(1, 0), xRoto}, {trackMatteClipKey(2, 0), yRoto}});
            TimelineTrackMatteEntry xMatte;
            xMatte.matteType = TrackMatteType::AlphaMatte;
            xMatte.matteSourceClipId = trackMatteClipKey(1, 1);
            tl.setTrackMatteEntries({{trackMatteClipKey(1, 0), xMatte}});
            tl.setClipParent(trackMatteClipKey(1, 0), trackMatteClipKey(1, 1));
            tl.absorbIntoCurrentUndoPoint();
            Timeline::MoveClipResult moveResult;
            tl.moveClipByIndex(false, 0, 0, 2.5, 0, &moveResult, nullptr);
            int maxSameUid = 0;
            int xTrack = -1;
            int xIndex = -1;
            bool xConnected = false;
            for (int t = 0; t < tl.videoTrackCount(); ++t) {
                int same = 0;
                const QVector<ClipInfo> &clips = tl.trackAt(false, t)->clips();
                for (int i = 0; i < clips.size(); ++i) {
                    if (clips[i].uid == xId) ++same;
                    if (clips[i].displayName == QStringLiteral("x")) {
                        xTrack = t;
                        xIndex = i;
                        xConnected = clips[i].anchorUid == anchor;
                    }
                }
                maxSameUid = qMax(maxSameUid, same);
            }
            const Located zl = locate(tl, false, zId);
            const QString xKeyE = trackMatteClipKey(3, xIndex);
            const QString zKeyE = trackMatteClipKey(zl.track, zl.index);
            const QString yKeyE = trackMatteClipKey(2, 0);
            const auto rotoE = tl.rotoEntries();
            const auto mattesE = tl.trackMatteEntries();
            const auto parentsE = tl.clipParentEntries();
            const bool carriersOk = rotoE.size() == 2 && rotoE.value(xKeyE).brushMask.width() == 3
                && rotoE.value(yKeyE).brushMask.width() == 4 && mattesE.size() == 1
                && mattesE.contains(xKeyE) && mattesE.value(xKeyE).matteSourceClipId == zKeyE
                && parentsE.value(xKeyE) == zKeyE;
            std::fprintf(stderr, "connected-clip G6e carriers xKey=%s zKey=%s rotoCount=%lld "
                         "(expect 2) roto[x]=%d (expect 3) roto[y %s]=%d (expect 4) matteKeys=%s "
                         "(expect x) matteSource=%s (expect z) parent[x]=%s (expect z)\n",
                         qPrintable(xKeyE), qPrintable(zKeyE), static_cast<long long>(rotoE.size()),
                         rotoE.value(xKeyE).brushMask.width(), qPrintable(yKeyE),
                         rotoE.value(yKeyE).brushMask.width(),
                         qPrintable(QStringList(mattesE.keys()).join(',')),
                         qPrintable(mattesE.value(xKeyE).matteSourceClipId),
                         qPrintable(parentsE.value(xKeyE)));
            for (int t = 0; t < tl.videoTrackCount(); ++t)
                tl.trackAt(false, t)->setSelectedClip(t == 0 ? 0 : -1);
            for (int t = 0; t < tl.audioTrackCount(); ++t)
                tl.trackAt(true, t)->setSelectedClip(-1);
            tl.takeConnectionWarnings();
            tl.deleteSelectedClip(); // saveUndoState("Delete clip") runs the reconcile
            const QStringList warnings = tl.takeConnectionWarnings();
            const QVector<ClipInfo> v3After = tl.trackAt(false, 2)->clips();
            const bool yKept = v3After.size() == 1 && v3After[0].uid == xId
                && v3After[0].displayName == QStringLiteral("y");
            int xLeft = 0;
            for (int t = 0; t < tl.videoTrackCount(); ++t)
                for (const ClipInfo &clip : tl.trackAt(false, t)->clips())
                    if (clip.displayName == QStringLiteral("x")) ++xLeft;
            eOk = sharedLoaded && xTrack == 3 && xConnected && maxSameUid == 1 && carriersOk && yKept
                && xLeft == 0 && warnings.isEmpty() && tl.trackAt(false, 0)->clipCount() == 0
                && tl.videoTrackCount() == 4;
            std::fprintf(stderr, "connected-clip G6e sharedLoaded=%d xTrack=%d (expect 3, not V3=2) "
                         "xConnected=%d maxSameUidPerTrack=%d (expect 1) carriersOk=%d afterDelete v3Clips=%lld "
                         "v3[0]=%s yKept=%d xLeft=%d (expect 0) warnings=%lld (expect 0) v1Clips=%d "
                         "tracks=%d (expect 4)\n", sharedLoaded, xTrack, xConnected, maxSameUid,
                         carriersOk, static_cast<long long>(v3After.size()),
                         qPrintable(v3After.isEmpty() ? QString() : v3After[0].displayName), yKept,
                         xLeft, static_cast<long long>(warnings.size()),
                         tl.trackAt(false, 0)->clipCount(), tl.videoTrackCount());
        }

        // (f) Connected X (V2) and connected Y (V3) share a uid; V2 also holds an
        // unconnected Z. X collides with Z, and V3 is still reserved for the
        // pending Y, so X goes back to its own start on V2 (detached, one
        // warning) and Y follows on V3 connected. Without the reservation X
        // lands on V3 first, Y falls back beside it with the same uid, and the
        // uid remap swaps Y's roto entry onto X and drops X's.
        bool fOk = false;
        {
            ClipInfo x = clipFor("x", 2.0, 1.0);
            ClipInfo y = clipFor("y", 2.0, 6.0);
            x.uid = y.uid = QStringLiteral("selftest-shared-uid");
            Timeline tl;
            load(tl, {{clipFor("b", 10.0)}, {x, clipFor("z", 2.0, 1.0)}, {y}});
            const QString anchor = uidAt(tl, 0, 0);
            const bool sharedLoaded = uidAt(tl, 1, 0) == x.uid && uidAt(tl, 2, 0) == x.uid;
            const bool xConnect = tl.connectClip(TrackKind::Video, 1, 0, anchor);
            const bool yConnect = tl.connectClip(TrackKind::Video, 2, 0, anchor);
            RotoClipEntry xRoto;
            xRoto.brushMask = QImage(1, 1, QImage::Format_ARGB32);
            RotoClipEntry yRoto;
            yRoto.brushMask = QImage(3, 3, QImage::Format_ARGB32);
            tl.setRotoEntries({{QStringLiteral("1:0"), xRoto}, {QStringLiteral("2:0"), yRoto}});
            tl.absorbIntoCurrentUndoPoint();
            tl.takeConnectionWarnings();
            const int tracksBefore = tl.videoTrackCount();
            // x and y share the uid; key = "track:index".
            auto byName = [&tl](const QString &name) { return locateByName(tl, false, name); };
            auto keyOf = [](const Located &l) { return QStringLiteral("%1:%2").arg(l.track).arg(l.index); };
            auto describe = [&](const Located &l) {
                return QStringLiteral("track=%1 start=%2 anchored=%3")
                    .arg(l.track).arg(l.startSec, 0, 'f', 6).arg(l.clip.anchorUid == anchor);
            };
            Timeline::MoveClipResult moveResult;
            const bool moved = tl.moveClipByIndex(false, 0, 0, 2.5, 0, &moveResult, nullptr);
            const int warnings = tl.takeConnectionWarnings().size();
            int maxSameUid = 0;
            for (int t = 0; t < tl.videoTrackCount(); ++t) {
                QHash<QString, int> perTrack;
                for (const ClipInfo &clip : tl.trackAt(false, t)->clips())
                    if (!clip.uid.isEmpty()) maxSameUid = qMax(maxSameUid, ++perTrack[clip.uid]);
            }
            const Located xl = byName(QStringLiteral("x"));
            const Located yl = byName(QStringLiteral("y"));
            const auto roto = tl.rotoEntries();
            const int rotoX = roto.value(keyOf(xl)).brushMask.width();
            const int rotoY = roto.value(keyOf(yl)).brushMask.width();
            const bool placedOk = xl.track == 1 && near(xl.startSec, 1.0) && xl.clip.anchorUid.isEmpty()
                && yl.track == 2 && near(yl.startSec, 8.5) && yl.clip.anchorUid == anchor;
            const bool carriersOk = rotoX == 1 && rotoY == 3 && roto.size() == 2;
            const int tracksAfter = tl.videoTrackCount();
            tl.undo();
            const Located xu = byName(QStringLiteral("x"));
            const Located yu = byName(QStringLiteral("y"));
            const bool undoOk = xu.track == 1 && near(xu.startSec, 1.0) && xu.clip.anchorUid == anchor
                && yu.track == 2 && near(yu.startSec, 6.0) && yu.clip.anchorUid == anchor;
            tl.redo();
            const Located xr = byName(QStringLiteral("x"));
            const Located yr = byName(QStringLiteral("y"));
            const bool redoOk = xr.track == xl.track && xr.index == xl.index
                && near(xr.startSec, xl.startSec) && xr.clip.anchorUid == xl.clip.anchorUid
                && yr.track == yl.track && yr.index == yl.index
                && near(yr.startSec, yl.startSec) && yr.clip.anchorUid == yl.clip.anchorUid;
            fOk = sharedLoaded && xConnect && yConnect && moved && maxSameUid == 1 && placedOk
                && warnings == 1 && carriersOk && tracksAfter == tracksBefore && undoOk && redoOk;
            std::fprintf(stderr, "connected-clip G6f sharedLoaded=%d connect x=%d y=%d moved=%d "
                         "maxSameUidPerTrack=%d (expect 1) x[%s] key=%s (expect track=1 start=1 "
                         "anchored=0) y[%s] key=%s (expect track=2 start=8.5 anchored=1) warnings=%d "
                         "(expect 1) rotoX=%d (expect 1) rotoY=%d (expect 3) rotoCount=%lld (expect 2) "
                         "tracks %d->%d undo x[%s] y[%s] undoOk=%d redo x[%s] y[%s] redoOk=%d\n",
                         sharedLoaded, xConnect, yConnect, moved, maxSameUid, qPrintable(describe(xl)),
                         qPrintable(keyOf(xl)), qPrintable(describe(yl)), qPrintable(keyOf(yl)),
                         warnings, rotoX, rotoY, static_cast<long long>(roto.size()), tracksBefore,
                         tracksAfter, qPrintable(describe(xu)), qPrintable(describe(yu)), undoOk,
                         qPrintable(describe(xr)), qPrintable(describe(yr)), redoOk);
        }

        // (g) Connected X (V2, collides with Z) and connected Y (V3, collides
        // with W) share a uid and both leave their own track: Y (earlier target)
        // goes to the free V4, X skips V3 (W) and V4 (Y's uid) for V5. Pairing
        // the leftover keys of the uid in track order swaps their roto / parent
        // entries; the reconcile's own old -> new keys keep each with its clip.
        bool gOk = false;
        {
            ClipInfo x = clipFor("x", 2.0, 1.0);
            ClipInfo y = clipFor("y", 2.0, 0.5);
            x.uid = y.uid = QStringLiteral("selftest-shared-uid");
            Timeline tl;
            load(tl, {{clipFor("b", 10.0)}, {x, clipFor("z", 2.0, 0.5)},
                      {y, clipFor("w", 2.0, 0.5)}, {}, {}});
            const QString anchor = uidAt(tl, 0, 0);
            const bool sharedLoaded = uidAt(tl, 1, 0) == x.uid && uidAt(tl, 2, 0) == x.uid;
            const bool xConnect = tl.connectClip(TrackKind::Video, 1, 0, anchor);
            const bool yConnect = tl.connectClip(TrackKind::Video, 2, 0, anchor);
            RotoClipEntry xRoto;
            xRoto.brushMask = QImage(1, 1, QImage::Format_ARGB32);
            RotoClipEntry yRoto;
            yRoto.brushMask = QImage(3, 3, QImage::Format_ARGB32);
            tl.setRotoEntries({{trackMatteClipKey(1, 0), xRoto}, {trackMatteClipKey(2, 0), yRoto}});
            tl.setClipParent(trackMatteClipKey(1, 0), trackMatteClipKey(1, 1)); // X -> Z
            tl.absorbIntoCurrentUndoPoint();
            tl.takeConnectionWarnings();
            struct State { Located x, y, z; int rotoX = 0, rotoY = 0; qsizetype rotoCount = 0; QString parentX; };
            auto capture = [&tl] {
                State s;
                s.x = locateByName(tl, false, QStringLiteral("x"));
                s.y = locateByName(tl, false, QStringLiteral("y"));
                s.z = locateByName(tl, false, QStringLiteral("z"));
                const auto roto = tl.rotoEntries();
                s.rotoX = roto.value(trackMatteClipKey(s.x.track, s.x.index)).brushMask.width();
                s.rotoY = roto.value(trackMatteClipKey(s.y.track, s.y.index)).brushMask.width();
                s.rotoCount = roto.size();
                s.parentX = tl.clipParentEntries().value(trackMatteClipKey(s.x.track, s.x.index));
                return s;
            };
            auto describe = [&anchor](const State &s) {
                return QStringLiteral("x[%1:%2 start=%3 anchored=%4 roto=%5 parent=%6] "
                                      "y[%7:%8 start=%9 anchored=%10 roto=%11] z=%12:%13 rotoCount=%14")
                    .arg(s.x.track).arg(s.x.index).arg(s.x.startSec, 0, 'f', 6)
                    .arg(s.x.clip.anchorUid == anchor).arg(s.rotoX).arg(s.parentX)
                    .arg(s.y.track).arg(s.y.index).arg(s.y.startSec, 0, 'f', 6)
                    .arg(s.y.clip.anchorUid == anchor).arg(s.rotoY)
                    .arg(s.z.track).arg(s.z.index).arg(s.rotoCount);
            };
            Timeline::MoveClipResult moveResult;
            const bool moved = tl.moveClipByIndex(false, 0, 0, 2.5, 0, &moveResult, nullptr);
            const State after = capture();
            const QString zKey = trackMatteClipKey(after.z.track, after.z.index);
            const bool afterOk = after.x.track == 4 && near(after.x.startSec, 3.5)
                && after.x.clip.anchorUid == anchor && after.y.track == 3 && near(after.y.startSec, 3.0)
                && after.y.clip.anchorUid == anchor && after.rotoX == 1 && after.rotoY == 3
                && after.rotoCount == 2 && after.parentX == zKey;
            tl.undo();
            const State undone = capture();
            const bool undoOk = undone.x.track == 1 && undone.x.index == 0 && near(undone.x.startSec, 1.0)
                && undone.x.clip.anchorUid == anchor && undone.y.track == 2 && undone.y.index == 0
                && near(undone.y.startSec, 0.5) && undone.y.clip.anchorUid == anchor
                && undone.rotoX == 1 && undone.rotoY == 3 && undone.rotoCount == 2
                && undone.parentX == trackMatteClipKey(1, 1);
            tl.redo();
            const State redone = capture();
            const bool redoOk = describe(redone) == describe(after);
            gOk = sharedLoaded && xConnect && yConnect && moved && afterOk && undoOk && redoOk;
            std::fprintf(stderr, "connected-clip G6g sharedLoaded=%d connect x=%d y=%d moved=%d "
                         "after %s zKey=%s (expect x 4:0 start 3.5 roto 1 parent z, y 3:0 start 3 "
                         "roto 3, rotoCount 2) afterOk=%d undo %s undoOk=%d redo %s redoOk=%d\n",
                         sharedLoaded, xConnect, yConnect, moved, qPrintable(describe(after)),
                         qPrintable(zKey), afterOk, qPrintable(describe(undone)), undoOk,
                         qPrintable(describe(redone)), redoOk);
        }

        // (h) / (i) Connected X (V2) is linked to connected XA (A1). (h) X
        // collides with Z and there is no V3 while XA's target is free; (i) no
        // collision but V2 is locked. The group is kept whole: both stay at 1,
        // detached, with one warning (a per-clip fallback moves XA to 3.5 and
        // keeps it connected); undo reconnects both.
        auto linkedGroup = [&](bool locked, const char *label) {
            ClipInfo x = clipFor("x", 2.0, 1.0);
            ClipInfo xa = clipFor("xa", 2.0, 1.0);
            x.linkGroup = xa.linkGroup = 11;
            QVector<ClipInfo> v2{x};
            if (!locked) v2.append(clipFor("z", 2.0, 1.0));
            Timeline tl;
            load(tl, {{clipFor("b", 10.0)}, v2}, {{xa}});
            const QString anchor = uidAt(tl, 0, 0);
            const bool connected = tl.connectClip(TrackKind::Video, 1, 0, anchor);
            const bool linked = locateByName(tl, false, QStringLiteral("x")).clip.linkGroup == 11
                && locateByName(tl, true, QStringLiteral("xa")).clip.anchorUid == anchor;
            if (locked) tl.trackAt(false, 1)->setLocked(true);
            tl.takeConnectionWarnings();
            const int videoTracks = tl.videoTrackCount();
            const int audioTracks = tl.audioTrackCount();
            auto describe = [&anchor](const Located &l) {
                return QStringLiteral("%1:%2 start=%3 anchored=%4").arg(l.track).arg(l.index)
                    .arg(l.startSec, 0, 'f', 6).arg(l.clip.anchorUid == anchor);
            };
            Timeline::MoveClipResult moveResult;
            const bool moved = tl.moveClipByIndex(false, 0, 0, 2.5, 0, &moveResult, nullptr);
            const QStringList warnings = tl.takeConnectionWarnings();
            const Located xl = locateByName(tl, false, QStringLiteral("x"));
            const Located al = locateByName(tl, true, QStringLiteral("xa"));
            const bool afterOk = xl.track == 1 && near(xl.startSec, 1.0) && xl.clip.anchorUid.isEmpty()
                && al.track == 0 && near(al.startSec, 1.0) && al.clip.anchorUid.isEmpty()
                && warnings.size() == 1 && tl.videoTrackCount() == videoTracks
                && tl.audioTrackCount() == audioTracks;
            tl.undo();
            const Located xu = locateByName(tl, false, QStringLiteral("x"));
            const Located au = locateByName(tl, true, QStringLiteral("xa"));
            const bool undoOk = xu.track == 1 && near(xu.startSec, 1.0) && xu.clip.anchorUid == anchor
                && au.track == 0 && near(au.startSec, 1.0) && au.clip.anchorUid == anchor;
            tl.redo();
            const Located xr = locateByName(tl, false, QStringLiteral("x"));
            const Located ar = locateByName(tl, true, QStringLiteral("xa"));
            const bool redoOk = describe(xr) == describe(xl) && describe(ar) == describe(al);
            const bool ok = connected && linked && moved && afterOk && undoOk && redoOk;
            std::fprintf(stderr, "connected-clip G6%s connected=%d linked=%d moved=%d x[%s] xa[%s] "
                         "(expect both start=1 anchored=0) warnings=%lld (expect 1) [%s] tracks v%d->%d "
                         "a%d->%d afterOk=%d undo x[%s] xa[%s] undoOk=%d redo x[%s] xa[%s] redoOk=%d\n",
                         label, connected, linked, moved, qPrintable(describe(xl)),
                         qPrintable(describe(al)), static_cast<long long>(warnings.size()),
                         qPrintable(warnings.join(QStringLiteral(" | "))), videoTracks,
                         tl.videoTrackCount(), audioTracks, tl.audioTrackCount(), afterOk,
                         qPrintable(describe(xu)), qPrintable(describe(au)), undoOk,
                         qPrintable(describe(xr)), qPrintable(describe(ar)), redoOk);
            return ok;
        };
        const bool hOk = linkedGroup(false, "h");
        const bool iOk = linkedGroup(true, "i");

        // (b, main) The same evacuation on the MainWindow timeline, whose
        // external undo hooks carry the mattes: V1 = [A (matte), B], X on V2 is
        // connected to A and collides with Z when A moves after B; it goes to
        // the free V3 and the track count stays. Undo must bring A's matte back
        // at 0:0 and keep a solo toggled with no undo point (a structural undo
        // would apply the stale external mixer state).
        bool bMainOk = false;
        if (!mainTimeline) {
            std::fprintf(stderr, "connected-clip G6b-main mainTimeline missing\n");
        } else {
            load(*mainTimeline, {{clipFor("a", 5.0), clipFor("b", 5.0)},
                                 {clipFor("x", 2.0, 1.0), clipFor("z", 2.0, 7.5)}, {}});
            const QString aId = uidAt(*mainTimeline, 0, 0);
            const QString xId = uidAt(*mainTimeline, 1, 0);
            mainTimeline->connectClip(TrackKind::Video, 1, 0, aId);
            TimelineTrackMatteEntry matte;
            matte.matteType = TrackMatteType::AlphaMatte;
            matte.matteSourceClipId = QStringLiteral("1:1");
            mainTimeline->setTrackMatteEntries({{QStringLiteral("0:0"), matte}});
            mainTimeline->absorbIntoCurrentUndoPoint();
            // A solo toggled with no undo point after this save must survive
            // the undo across the evacuation's new track.
            mainTimeline->saveUndoState(QStringLiteral("solo base"));
            mainTimeline->toggleSoloTrack(0);
            const int tracksBefore = mainTimeline->videoTrackCount();
            Timeline::MoveClipResult moveResult;
            const bool moved = mainTimeline->moveClipByIndex(false, 0, 0, 10.0, 0, &moveResult, nullptr);
            const Located xl = locate(*mainTimeline, false, xId);
            const int tracksAfterEdit = mainTimeline->videoTrackCount();
            const QStringList editKeys = mainTimeline->trackMatteEntries().keys();
            mainTimeline->undo();
            const auto undone = mainTimeline->trackMatteEntries();
            const Located al = locate(*mainTimeline, false, aId);
            const AudioMixer *mixer = mainTimeline->audioMixer();
            const QJsonArray mixerStates = mixer
                ? mixer->collectTrackState().value("states").toArray() : QJsonArray{};
            const bool mixerSolo = !mixerStates.isEmpty()
                && mixerStates.at(0).toObject().value("solo").toBool();
            const bool headerSolo = mainTimeline->audioTrackCount() > 0
                && mainTimeline->audioTracks()[0]->isSolo();
            bMainOk = moved && xl.track == 2 && near(xl.startSec, 11.0)
                && tracksAfterEdit == tracksBefore
                && al.track == 0 && al.index == 0 && near(al.startSec, 0.0)
                && undone.contains(QStringLiteral("0:0")) && !undone.contains(QStringLiteral("0:1"))
                && undone.value(QStringLiteral("0:0")).matteSourceClipId == QStringLiteral("1:1")
                && mainTimeline->videoTrackCount() == tracksBefore
                && mixerSolo && mixerSolo == headerSolo;
            std::fprintf(stderr, "connected-clip G6b-main moved=%d xTrack=%d xStart=%.6f (expect 11) "
                         "tracks %d->%d editMatteKeys=%s undoMatteKeys=%s (expect 0:0) "
                         "undoMatteSource=%s (expect 1:1) undoTracks=%d (expect %d) aKey=%d:%d "
                         "mixer=%d states=%lld mixerSolo0=%d (expect 1) headerSolo0=%d (expect 1)\n",
                         moved, xl.track, xl.startSec, tracksBefore, tracksAfterEdit,
                         qPrintable(editKeys.join(',')),
                         qPrintable(QStringList(undone.keys()).join(',')),
                         qPrintable(undone.value(QStringLiteral("0:0")).matteSourceClipId),
                         mainTimeline->videoTrackCount(), tracksBefore, al.track, al.index,
                         mixer != nullptr, static_cast<long long>(mixerStates.size()), mixerSolo,
                         headerSolo);
        }
        tally.gate(6, aOk && b.first && bMainOk && c.first && c.second == 2 && dOk && eOk && fOk
                       && gOk && hOk && iOk,
                   "a carrier entry did not follow its clip by uid (overtake / evacuation, also "
                   "two uid twins both leaving their tracks), undo of an evacuation lost the "
                   "pre-edit matte or a later solo, the nested sequence store was lost, reconcile "
                   "changed the track count, an evacuation shared a track with its uid twin (also "
                   "when the twin is a pending connected clip), or a link group was left partly "
                   "moved / connected (no slot, or a locked member)");
    }

    // G7: save / load, missing anchor, ProjectDiff.
    {
        Timeline timeline;
        load(timeline, {{clipFor("a", 10.0)}, {clipFor("x", 2.0, 4.0), clipFor("q", 1.0, 2.0)}});
        const QString aUid = uidAt(timeline, 0, 0);
        timeline.connectClip(TrackKind::Video, 1, 0, aUid);
        ProjectData data;
        data.videoTracks = timeline.allVideoTracks();
        data.audioTracks = timeline.allAudioTracks();
        ProjectData roundtrip;
        const QString json = ProjectFile::toJsonString(data);
        const bool parsed = ProjectFile::fromJsonString(json, roundtrip);
        const ClipInfo x0 = roundtrip.videoTracks.value(1).value(0);
        const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
        const QJsonArray v2 = root.value("videoTracks").toArray().at(1).toArray();
        const bool keysOnlyWhenConnected = v2.at(0).toObject().contains("anchorUid")
            && v2.at(0).toObject().contains("anchorOffsetSec")
            && !v2.at(1).toObject().contains("anchorUid")
            && !v2.at(1).toObject().contains("anchorOffsetSec");
        const bool roundtripOk = parsed && x0.anchorUid == aUid && near(x0.anchorOffsetSec, 4.0)
            && keysOnlyWhenConnected && root.value("version").toInt() == 2;

        // A file whose second V2 clip names an anchor that does not exist.
        ProjectData broken = data;
        broken.videoTracks[1][1].anchorUid = QStringLiteral("missing-anchor-uid");
        broken.videoTracks[1][1].anchorOffsetSec = 2.0;
        int warnings = -1;
        bool missingDetached = false;
        bool validKept = false;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("connected.veditor"));
        QString err;
        if (mainTimeline && dir.isValid() && ProjectFile::save(path, broken)) {
            mainTimeline->takeConnectionWarnings();
            callTool(registry, QStringLiteral("open_project"), QJsonObject{{"path", path}}, &err);
            warnings = mainTimeline->takeConnectionWarnings().size();
            const TimelineTrack *loadedV2 = mainTimeline->trackAt(false, 1);
            if (loadedV2 && loadedV2->clipCount() == 2) {
                missingDetached = loadedV2->clips().at(1).anchorUid.isEmpty();
                validKept = loadedV2->clips().at(0).anchorUid == aUid
                    && near(loadedV2->clips().at(0).anchorOffsetSec, 4.0);
            }
        }
        ProjectData offsetOnly = data;
        offsetOnly.videoTracks[1][0].anchorOffsetSec = 9.0;
        offsetOnly.videoTracks[1][0].anchorUid = QStringLiteral("other-anchor");
        ProjectData pathChanged = data;
        pathChanged.videoTracks[1][0].filePath = QStringLiteral("/selftests/connected-other.mp4");
        const int anchorDiff = projdiff::diff(data, offsetOnly).size();
        // ProjectDiff pairs clips by filePath: a new path is one removed / added pair.
        int pathDiff = 0;
        for (const projdiff::Change &change : projdiff::diff(data, pathChanged))
            if (change.type == projdiff::Change::Removed) ++pathDiff;
        // A uid held by two V1 clips (a cross-track drop onto V1 keeps its uid;
        // setClips only de-duplicates within a track, so insert it directly).
        bool ambiguousOk = false;
        {
            Timeline tl;
            load(tl, {{clipFor("a", 4.0), clipFor("c", 4.0)}, {clipFor("x", 2.0, 5.0), clipFor("w", 1.0, 1.0)}});
            const QString a = uidAt(tl, 0, 0);
            tl.connectClip(TrackKind::Video, 1, 0, a);
            ClipInfo twin = tl.trackAt(false, 0)->clips().at(0);
            twin.displayName = QStringLiteral("a-twin");
            tl.trackAt(false, 0)->insertClipPreservingDownstream(2, twin, 1.0);
            tl.takeConnectionWarnings();
            const quint64 serialBefore = tl.undoManager()->saveSerial();
            QString connectErr;
            const bool rejected = !tl.connectClip(TrackKind::Video, 1, 1, a, &connectErr);
            const bool unchanged = tl.undoManager()->saveSerial() == serialBefore
                && tl.trackAt(false, 1)->clips().at(1).anchorUid.isEmpty();
            tl.saveUndoState(QStringLiteral("unrelated edit"));
            const QStringList ambWarnings = tl.takeConnectionWarnings();
            const ClipInfo x = tl.trackAt(false, 1)->clips().at(0);
            const int twins = [&] {
                int n = 0;
                for (const ClipInfo &clip : tl.trackAt(false, 0)->clips()) n += clip.uid == a;
                return n;
            }();
            ambiguousOk = twins == 2 && rejected && unchanged && x.anchorUid.isEmpty()
                && ambWarnings.size() == 1;
            std::fprintf(stderr, "connected-clip G7 ambiguous v1Twins=%d (expect 2) rejected=%d [%s] "
                         "unchanged=%d existing anchorUid=[%s] (expect empty) warnings=%lld (expect 1) "
                         "[%s]\n", twins, rejected, qPrintable(connectErr), unchanged,
                         qPrintable(x.anchorUid), static_cast<long long>(ambWarnings.size()),
                         qPrintable(ambWarnings.join('|')));
        }
        std::fprintf(stderr, "connected-clip G7 roundtrip=%d keysOnlyWhenConnected=%d version=%d "
                     "openErr=[%s] validKept=%d missingDetached=%d warnings=%d (expect 1) "
                     "anchorDiff=%d (expect 0) pathDiffRemoved=%d (expect 1)\n", roundtripOk,
                     keysOnlyWhenConnected, root.value("version").toInt(), qPrintable(err), validKept,
                     missingDetached, warnings, anchorDiff, pathDiff);
        tally.gate(7, roundtripOk && validKept && missingDetached && warnings == 1
                   && anchorDiff == 0 && pathDiff == 1 && ambiguousOk,
                   "the connection did not survive save / load, a missing anchor was not "
                   "detached with one warning, ProjectDiff compared the connection, or a uid "
                   "held by two V1 clips was accepted / followed as an anchor");
    }

    // G8: moving the connected clip itself.
    {
        Timeline timeline;
        load(timeline, {{clipFor("b", 10.0)}, {clipFor("x", 2.0, 4.0)}});
        const QString bUid = uidAt(timeline, 0, 0);
        const QString xUid = uidAt(timeline, 1, 0);
        timeline.connectClip(TrackKind::Video, 1, 0, bUid);
        Timeline::MoveClipResult result;
        timeline.moveClipByIndex(false, 1, 0, 6.0, 1, &result, nullptr);
        const Located moved = locate(timeline, false, xUid);
        // Then the anchor moves: the re-derived offset is the one followed.
        editAnchor(timeline, [](ClipInfo &clip) { clip.leadInSec = 1.0; });
        const Located followed = locate(timeline, false, xUid);
        timeline.moveClipByIndex(false, 1, followed.index, 12.0, 0, &result, nullptr);
        const Located onV1 = locate(timeline, false, xUid);
        std::fprintf(stderr, "connected-clip G8 movedStart=%.6f offset=%.6f (expect 6) "
                     "afterAnchorMove=%.6f (expect 7) onV1 track=%d anchorUid=[%s] (expect empty)\n",
                     moved.startSec, moved.clip.anchorOffsetSec, followed.startSec, onV1.track,
                     qPrintable(onV1.clip.anchorUid));
        tally.gate(8, near(moved.startSec, 6.0) && moved.clip.anchorUid == bUid
                   && near(moved.clip.anchorOffsetSec, 6.0) && near(followed.startSec, 7.0)
                   && onV1.found && onV1.track == 0 && onV1.clip.anchorUid.isEmpty(),
                   "moving the connected clip did not re-derive its offset or a V1 drop kept "
                   "the connection");
    }

    // G9: sequence switch empties the baseline.
    {
        Timeline timeline;
        load(timeline, {{clipFor("main", 4.0)}});
        ClipInfo anchor = clipFor("anchor-b", 10.0);
        anchor.uid = QStringLiteral("anchor-b-uid");
        ClipInfo stale = clipFor("stale-b", 2.0, 4.0);
        stale.uid = QStringLiteral("stale-b-uid");
        stale.anchorUid = anchor.uid;
        stale.anchorOffsetSec = 1.0;   // the position says 4
        ClipInfo other = clipFor("other-b", 1.0, 2.0);
        TimelineSequence sequence;
        sequence.id = QStringLiteral("seq-b");
        sequence.name = QStringLiteral("B");
        sequence.videoTracks = {{anchor}, {stale, other}};
        sequence.audioTracks = {{}};
        timeline.addSequence(sequence);
        QString bId;
        for (const TimelineSequence &s : timeline.sequences())
            if (s.name == QStringLiteral("B")) bId = s.id;
        const bool switched = timeline.setActiveSequence(bId);
        TimelineTrack *v2 = timeline.trackAt(false, 1);
        QVector<ClipInfo> clips = v2->clips();
        clips[1].volume = 0.5;
        v2->setClips(clips);
        timeline.saveUndoState(QStringLiteral("音量"));
        const Located x = locate(timeline, false, QStringLiteral("stale-b-uid"));
        std::fprintf(stderr, "connected-clip G9 switched=%d startSec=%.6f (expect 4) offset=%.6f "
                     "(expect 4) anchorUid=[%s] warnings=%lld\n", switched, x.startSec,
                     x.clip.anchorOffsetSec, qPrintable(x.clip.anchorUid),
                     static_cast<long long>(timeline.connectionWarnings().size()));
        tally.gate(9, switched && near(x.startSec, 4.0) && near(x.clip.anchorOffsetSec, 4.0)
                   && x.clip.anchorUid == anchor.uid,
                   "the first edit after a sequence switch moved the connected clip or kept "
                   "its stale offset");
    }

    // G10: load keeps positions, one undo entry, stale offsets re-derived.
    {
        ClipInfo anchor = clipFor("load-a", 10.0);
        anchor.uid = QStringLiteral("load-anchor-uid");
        ClipInfo stale = clipFor("load-x", 2.0, 4.0);
        stale.uid = QStringLiteral("load-stale-uid");
        stale.anchorUid = anchor.uid;
        stale.anchorOffsetSec = 1.0;
        ClipInfo valid = clipFor("load-y", 1.0, 1.0);
        valid.uid = QStringLiteral("load-valid-uid");
        valid.anchorUid = anchor.uid;
        valid.anchorOffsetSec = 7.0;
        ProjectData data;
        data.videoTracks = ProjectTrackClips{{anchor}, {stale, valid}};
        data.audioTracks = ProjectTrackClips{{}};
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("stale.veditor"));
        QString err;
        bool ok = false;
        if (mainTimeline && dir.isValid() && ProjectFile::save(path, data)) {
            callTool(registry, QStringLiteral("open_project"), QJsonObject{{"path", path}}, &err);
            const Located a = locate(*mainTimeline, false, anchor.uid);
            const Located x = locate(*mainTimeline, false, stale.uid);
            const Located y = locate(*mainTimeline, false, valid.uid);
            const UndoManager *undo = mainTimeline->undoManager();
            std::fprintf(stderr, "connected-clip G10 openErr=[%s] undoIndex=%d canUndo=%d "
                         "anchor=%.6f stale=%.6f (expect 4) offset=%.6f (expect 4) valid=%.6f "
                         "(expect 7)\n", qPrintable(err), undo->currentIndex(), undo->canUndo(),
                         a.startSec, x.startSec, x.clip.anchorOffsetSec, y.startSec);
            ok = undo->currentIndex() == 0 && !undo->canUndo() && near(a.startSec, 0.0)
                && near(x.startSec, 4.0) && near(x.clip.anchorOffsetSec, 4.0)
                && x.clip.anchorUid == anchor.uid && near(y.startSec, 7.0)
                && near(y.clip.anchorOffsetSec, 7.0);
        } else {
            std::fprintf(stderr, "connected-clip G10 setup failed\n");
        }
        tally.gate(10, ok, "the load moved a clip, kept a stale offset or left more than one "
                           "undo entry");
    }

    // G11: linear / non-linear anchor following.
    {
        auto connectedStart = [](const std::function<void(Timeline &)> &setup,
                                 const std::function<void(Timeline &)> &edit,
                                 double connectedLeadIn, int *warnings) {
            Timeline timeline;
            load(timeline, {{clipFor("a", 10.0)}, {clipFor("x", 1.0, connectedLeadIn), clipFor("w", 1.0, 20.0)}});
            setup(timeline);
            const QString xUid = uidAt(timeline, 1, 0);
            timeline.connectClip(TrackKind::Video, 1, 0, uidAt(timeline, 0, 0));
            timeline.takeConnectionWarnings();
            edit(timeline);
            if (warnings) *warnings = timeline.takeConnectionWarnings().size();
            const Located x = locate(timeline, false, xUid);
            return x.clip.anchorUid.isEmpty() ? -1000.0 : x.startSec;
        };
        auto none = [](Timeline &) {};
        auto unrelated = [](Timeline &timeline) {
            TimelineTrack *v2 = timeline.trackAt(false, 1);
            QVector<ClipInfo> clips = v2->clips();
            clips[1].volume = 0.25;
            v2->setClips(clips);
            timeline.saveUndoState(QStringLiteral("音量"));
        };
        // (a) anchor starts at 5, connection at 4 (offset -1).
        const double a = connectedStart(
            [](Timeline &timeline) { editAnchor(timeline, [](ClipInfo &c) { c.leadInSec = 5.0; }); },
            unrelated, 4.0, nullptr);
        // (b) anchor [0, 6], connection at 8; trim the end to 4.
        const double b = connectedStart(
            [](Timeline &timeline) { editAnchor(timeline, [](ClipInfo &c) { c.outPoint = 6.0; }); },
            [](Timeline &timeline) { editAnchor(timeline, [](ClipInfo &c) { c.outPoint = 4.0; }); },
            8.0, nullptr);
        // (c) linear anchor [0, 10], connection at 6.
        const double rippleHead = connectedStart(none,
            [](Timeline &timeline) { editAnchor(timeline, [](ClipInfo &c) { c.inPoint = 2.0; }); },
            6.0, nullptr);
        const double plainHead = connectedStart(none,
            [](Timeline &timeline) {
                editAnchor(timeline, [](ClipInfo &c) { c.inPoint = 2.0; c.leadInSec = 2.0; });
            }, 6.0, nullptr);
        const double faster = connectedStart(none,
            [](Timeline &timeline) { editAnchor(timeline, [](ClipInfo &c) { c.speed = 2.0; }); },
            6.0, nullptr);
        // (d) reversed anchor: ripple head trim keeps the local offset.
        int reversedWarnings = -1;
        const double reversed = connectedStart(
            [](Timeline &timeline) { editAnchor(timeline, [](ClipInfo &c) { c.reversed = true; }); },
            [](Timeline &timeline) { editAnchor(timeline, [](ClipInfo &c) { c.inPoint = 2.0; }); },
            6.0, &reversedWarnings);
        std::fprintf(stderr, "connected-clip G11 (a) offset-1=%.6f (expect 4) (b) pastEnd=%.6f "
                     "(expect 8) (c) rippleHead=%.6f (expect 4) plainHead=%.6f (expect 6) "
                     "speed2x=%.6f (expect 3) (d) reversed=%.6f (expect 6) warnings=%d (expect 0)\n",
                     a, b, rippleHead, plainHead, faster, reversed, reversedWarnings);
        tally.gate(11, near(a, 4.0) && near(b, 8.0) && near(rippleHead, 4.0)
                   && near(plainHead, 6.0) && near(faster, 3.0) && near(reversed, 6.0)
                   && reversedWarnings == 0,
                   "a connection did not follow the anchor's source frame (linear) or offset "
                   "(non-linear)");
    }

    return tally.finish("connected-clip");
}
