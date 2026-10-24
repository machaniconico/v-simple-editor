#include "../AudioMixer.h"
#include "../BrushAnimation.h"
#include "../MainWindow.h"
#include "../ProjectDiff.h"
#include "../ProjectFile.h"
#include "../RenderInPlace.h"
#include "../Timeline.h"
#include "../Text3DLayer.h"
#include "../ThreePointEdit.h"
#include "../TrackMatteKey.h"
#include "../UndoManager.h"
#include "../libavcore/Encode.h"
#include "../libavcore/Probe.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpToolRegistry.h"
#include "SelftestGate.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QColor>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <utility>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

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
// US-1202 (connect UI): the actions timeline.connect_to_storyline / timeline.disconnect
// (Edit menu actions action_connect_to_storyline / action_disconnect_from_storyline, no
// shortcut) and the storyline-mode drop connection.
//   G23: the connect action connects a V2 clip to the V1 clip it overlaps (and, past
//        the V1 end, to the preceding V1 clip), one undo point named by the action text.
//   G24: the disconnect action detaches it in one undo point; undo reconnects, redo
//        detaches again; disconnecting an unconnected clip is rejected.
//   G25: negative controls: a V1 clip and a V2 clip with no V1 clip under or before it
//        are rejected with the reason in the status bar and nothing changes.
//   G26: with setStorylineMode(true) a media file dropped on V2 / A2 and a clip dropped
//        from V3 onto V2 connect to the V1 clip under the drop; with the mode off the media
//        drop is refused (also on V1) and the clip drop leaves no connection. US-1304: with
//        the mode on a media drop in the middle of a V1 clip snaps to the next boundary and
//        pushes the clips after it (it was refused before).
// US-1304 (media drop on V1 in storyline mode):
//   G29: a drop with audio on the V1 boundary of [X|Y] gives three V1 clips; Y, the V2 clip
//        connected to Y and Y's linked audio move by the new length, the unlinked A1 BGM does
//        not, the new audio is on a free audio track at the new V1 clip's start with no
//        overlap, one undo restores everything, and a selected Y stays selected. A variant
//        with a one-frame gap in front of the drop point closes the gap and keeps the new
//        audio on the new V1 clip's start.
//   G30: a drop on a locked V1 is refused with the lock text and changes nothing; with every
//        audio track locked, or full at the landing time, it is refused with the free-track
//        text and V1 is unchanged.
// US-1303 (clip-key remap signal, no roto / matte / parent on the fixtures):
//   G31: Timeline::clipKeysRemapped fires once per key-changing mutation with the identity
//        pairs removed: delete V2 clip 0 ({1:1->1:0}, dropped {1:0}), a V2 reorder, a drop
//        onto V3, a storyline insert followed by the reconcile of saveUndoState, the external
//        remap called twice with one snapshot, the same delete after an undo; a track removal
//        gives trackIndicesRemapped only, key-preserving edits and the undo give nothing.
//        Detects: the emit removed (0 emits), the memo not cleared by undo (the second delete
//        silent), identity pairs kept (key-preserving edits emit, the delete map not exact).
// US-1302 (position-keyed MainWindow sidecars, no roto / matte on the fixture):
//   G32: timeRemap / wiggle / text3D / expression / brush entries (and the clipId inside the
//        timeRemap and brush values) on V2 clip b follow (1) a delete of the clip in front of
//        b to "1:0", (3) undo / redo to "1:1" / "1:0", (3') the same delete after an undo to
//        "1:0", (2) a drop of b onto V3 to "2:0", (4) a save / load, (5) the removal of V2 to
//        "1:0"; (6) a committed sidecar edit (US-1401) survives the undo of an unrelated
//        edit. Detects: no rekey slot (HEAD: entries stay on the old key, on a's neighbour),
//        the sidecar applier only on structural undo (3), the memo kept over an undo (3').
// US-1320 (wave 14 cumulative review C-03 / C-04):
//   G33: a storyline media drop on the V1 boundary of [X|Y] whose reconcile pushes c (on V2,
//        connected to Y) past the unconnected d emits clipKeysRemapped once with the composed
//        map {0:1->0:2, 1:0->1:1, 1:1->1:0} and no identity pair. Detects: no key-remap batch
//        in importMedia (HEAD: 2 emits, the V1 shift and the V2 reorder).
//   G34: after a storyline drop at the V1 head moves the selection, the brush preview is bound
//        through the rekeyed entries: (a) a selected with no brush -> nothing bound, (b) b
//        selected with the brush -> bound on b's new key "0:2". Detects: no rebind at the end
//        of rekeyPositionKeyedSidecars (HEAD: (a) binds b's brush via "0:1", (b) stays cleared).
//        (c) Timeline::undo / redo of each drop keep the brush on b ("0:1" / "0:2"). Detects:
//        the drop's undo point saved before the batched remap reached the sidecars (redo puts
//        b's brush on a at "0:1").
//   G35 (US-1400): the source monitor's 3-point insert / overwrite rekey the roto, matte, parent and
//        timeRemap carriers of b ("0:1" -> "0:2" / "0:3", one clipKeysRemapped, one undo), and a
//        locked V1 refuses both (and the MainWindow slots keep the refusal in the status bar).
//        Detects: no snapshot / remap in the *Active functions (HEAD: roto stays on "0:1"; a locked
//        V1 is edited; the slots overwrite the refusal with the success message).
//   G36 (US-1401): the timeRemap / text3D / expression / wiggle commits record one undo point each
//        (none for an equal value), and undo / redo carry the four by value: the remap entry and the
//        clip's curve stay together, two same-uid-order tracks moved and undone keep their own values,
//        a shared-uid track removed twice and undone twice gets both back, save / load keeps them,
//        the real wiggle dialog records one, an undo across a sequence switch restores them, and a
//        direct map write without an undo point is rolled back by an unrelated undo (the former
//        G32 (6) contract, inverted).
//        Detects: commits that record no undo point / a sidecar state without the four values (HEAD).
// US-1214 (copy paths, explicit delete, track operations):
//   G12: blading the connected clip leaves the right piece in place, connected,
//       offset = original + left length; with setRederiveDisabledForTest the
//       right piece is pulled back onto the left one (control fails); cutting the
//       anchor and the connected clip together (blade all at 5, all-track ripple
//       of [4,6)) connects the right piece to the V1 piece starting at its start
//       at offset 0; one all-track ripple of [2,3) and [5,6) through a linked
//       V2+A2 pair connected at 0 connects each cut's remnants to the V1 piece
//       starting with them, and deleting the piece at 2 takes only its remnants.
//   G13: blading the anchor moves the connections at / after the cut to the
//       right piece (offset - cut), the left ones stay; a V1-only ripple of
//       [6,7] on a reversed anchor keeps the timeline-3.5 connection on the left
//       piece, deletes the one at local [6,7) and moves the one at 8 with the
//       right piece to 7 (a source-second judge deletes the 3.5 one, no
//       reconcile keeps the [6,7) one); the [0,4) / [4,8) remnants of a V2-only
//       ripple of [4,6) through a linked V2 clip go to the left / right piece of
//       a later blade at 3 by their own points, and deleting the left piece
//       takes only [0,4) (one point per linked group fails this).
//   G14: duplicate / paste / createSequenceFromClip results carry no anchorUid
//       (and no warning) while the original stays connected (a value copy
//       keeps the anchor).
//   G15: baking a connected V2+A2 group keeps both uids and connections, and
//       decompose keeps them; an anchor reconnected after the bake survives the
//       decompose (restoring the snapshot's anchor fails); baking the V1 anchor
//       (handles change its source mapping) keeps the connection in place.
//   G16: delete_clip (MCP) / overwrite cover / V1-only ripple of the anchor
//       delete the connected V2+A2 group and keep an unconnected clip; R1: a
//       connection outside the deleted span ([3,6] with offset 8, [0,3] with
//       offset -1) stays detached with one warning.
//   G17: a V1 replaced through setClips (simulated MultiCam, no undo point)
//       detaches the connection on the next unrelated saveUndoState with one
//       warning (deleting on a baseline difference loses the clip).
//   G18: pre-compose across the anchor and a connected clip: the connection
//       after the range stays on the left remainder in place with no warning,
//       and in the nested sequence the range piece is connected to the nested
//       V1 piece.
//   G19: connecting a linked V2+A2 pair connects both; a V1 insert moves both
//       and leaves an unlinked A2 clip; a V1 split between the members of a
//       non-overlapping linked pair keeps both on the left piece (the group's
//       earliest point) in place, and deleting that piece deletes both.
//   G20: removeTrack(V1) / moveTrack(V1 -> V3) / moveTrack(V2 -> V1) are
//       rejected unchanged with connections (succeed without: control); a
//       locked V2 connection detaches with a warning on a V1 insert and on an
//       explicit anchor delete; a cross-track drop of the anchor detaches the
//       linked group with one warning.
//   G21: insertAudioClipAtPlayhead splitting a connected A2 clip keeps both
//       pieces where the edit put them, connected (control as in G12).
//   G22: a silence cut of a V1 anchor (3 pieces): the piece-1 connection stays,
//       the one in a removed span detaches with one warning, the piece-3 one
//       follows the packed piece 3 at the same local position.
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

// restoreFromProject repairs a uid shared across video tracks (US-1405), so a fixture that needs
// a twin uid on another track overwrites it afterwards: setClips keeps one clip per track as is.
void shareUid(Timeline &timeline, int track, int index, const QString &uid)
{
    QVector<ClipInfo> clips = timeline.trackAt(false, track)->clips();
    clips[index].uid = uid;
    timeline.trackAt(false, track)->setClips(clips);
}

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

// The first clip named `name` whose uid is not `exceptUid` (a split's other piece).
Located locateOther(const Timeline &timeline, bool audio, const QString &name,
                    const QString &exceptUid)
{
    const auto &tracks = audio ? timeline.audioTracks() : timeline.videoTracks();
    for (int t = 0; t < tracks.size(); ++t) {
        double cursor = 0.0;
        const auto &clips = tracks[t]->clips();
        for (int i = 0; i < clips.size(); ++i) {
            cursor += clips[i].leadInSec;
            if (clips[i].displayName == name && clips[i].uid != exceptUid)
                return {true, t, i, cursor, clips[i]};
            cursor += clips[i].effectiveDuration();
        }
    }
    return {};
}

QString describe(const Located &l, const QString &anchorUid)
{
    return l.found ? QStringLiteral("track=%1 start=%2 offset=%3 anchored=%4")
                         .arg(l.track).arg(l.startSec, 0, 'f', 6)
                         .arg(l.clip.anchorOffsetSec, 0, 'f', 6).arg(int(l.clip.anchorUid == anchorUid))
                   : QStringLiteral("missing");
}

// 48 kHz mono 16-bit PCM: each segment is (seconds, 440 Hz tone or silence).
bool writeWav(const QString &path, const QVector<QPair<double, bool>> &segments)
{
    constexpr int kRate = 48000;
    QVector<qint16> samples;
    for (const auto &segment : segments) {
        const int count = qRound(segment.first * kRate);
        for (int i = 0; i < count; ++i) {
            const double t = double(samples.size()) / kRate;
            samples.append(segment.second
                ? qint16(12000.0 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t)) : qint16(0));
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataBytes = quint32(samples.size() * 2);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataBytes);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(kRate) << quint32(kRate * 2)
        << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << dataBytes;
    for (const qint16 sample : samples) out << sample;
    return out.status() == QDataStream::Ok;
}

// Answers every message box that `run` opens: Yes when offered, else closes it.
QStringList answeringMessageBoxes(QObject *context, const std::function<void()> &run)
{
    QStringList seen;
    QTimer timer;
    timer.setInterval(20);
    QObject::connect(&timer, &QTimer::timeout, context, [&seen] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) return;
        seen.append(box->text());
        if (QAbstractButton *yes = box->button(QMessageBox::Yes)) yes->click();
        else box->done(QMessageBox::Ok);
    });
    timer.start();
    run();
    timer.stop();
    return seen;
}

// US-1300: closes the modal prompts a real MainWindow slot opens, on a periodic timer (the
// slot may open several in a row). A QMessageBox with a Yes button is answered Yes; a
// QInputDialog gets `inputText` and is accepted when one was given; anything else is
// rejected and counted in `unexpected`, so a stray dialog fails the gate instead of
// hanging it. The timer stops with the object.
struct ModalDriver {
    explicit ModalDriver(QObject *context, const QString &inputText = QString())
        : text(inputText)
    {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, context, [this] {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal) return;
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                seen.append(box->text());
                if (QAbstractButton *yes = box->button(QMessageBox::Yes)) {
                    ++answered;
                    yes->click();
                } else {
                    ++unexpected;
                    box->reject();
                }
            } else if (auto *input = qobject_cast<QInputDialog *>(modal)) {
                if (text.isEmpty()) {
                    ++unexpected;
                    input->reject();
                } else {
                    ++answered;
                    input->setTextValue(text);
                    input->accept();
                }
            } else {
                ++unexpected;
                if (auto *dialog = qobject_cast<QDialog *>(modal)) dialog->reject();
                else modal->close();
            }
        });
    }
    ~ModalDriver() { timer.stop(); }
    void start() { timer.start(); }

    QTimer timer;
    QString text;
    QStringList seen;
    int answered = 0;
    int unexpected = 0;
};

// MainWindow::onMultiCamApplyToTimeline is a private slot: reach it by explicit instantiation
// (audio_fixture_selftest.cpp does the same) so MainWindow.h stays unchanged.
template<class Tag, typename Tag::Type Member>
struct Access {
    friend typename Tag::Type member(Tag) { return Member; }
};
struct MultiCamApplyTag {
    using Type = void (MainWindow::*)(const MultiCamProject &);
    friend Type member(MultiCamApplyTag);
};
template struct Access<MultiCamApplyTag, &MainWindow::onMultiCamApplyToTimeline>;
struct VideoTracksTag {
    using Type = QVector<TimelineTrack *> Timeline::*;
    friend Type member(VideoTracksTag);
};
template struct Access<VideoTracksTag, &Timeline::m_videoTracks>;

// 40x24 10 fps mpeg4 clip of `frames` frames (the media a drop imports).
bool writeVideoFile(const QString &path, int frames)
{
    libavcore::EncodeRequest req;
    req.width = 40;
    req.height = 24;
    req.fps = 10;
    req.fpsNum = 10;
    req.fpsDen = 1;
    req.videoBitrateBits = 600000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(req)) return false;
    for (int i = 0; i < frames; ++i) {
        QImage frame(40, 24, QImage::Format_RGB888);
        frame.fill(QColor((i * 23) & 255, 90, 160));
        if (!encoder.pushFrame(frame, i)) return false;
    }
    return !encoder.finalize();
}

// 40x24 10 fps mpeg4 clip of `frames` frames with a silent AAC track of the same length.
bool writeVideoWithAudioFile(const QString &path, int frames)
{
    libavcore::EncodeRequest req;
    req.width = 40;
    req.height = 24;
    req.fps = 10;
    req.fpsNum = 10;
    req.fpsDen = 1;
    req.videoBitrateBits = 600000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;
    req.audioEncode = true;
    libavcore::FrameEncoder encoder;
    if (encoder.open(req)) return false;
    for (int i = 0; i < frames; ++i) {
        QImage frame(40, 24, QImage::Format_RGB888);
        frame.fill(QColor((i * 23) & 255, 90, 160));
        if (!encoder.pushFrame(frame, i)) return false;
    }
    AVFrame *audio = av_frame_alloc();
    if (!audio) return false;
    audio->format = AV_SAMPLE_FMT_FLTP;
    audio->sample_rate = 48000;
    audio->nb_samples = 1024;
    av_channel_layout_default(&audio->ch_layout, 2);
    bool ok = av_frame_get_buffer(audio, 0) >= 0;
    for (int i = 0; ok && i < frames * 4800; i += 1024) {
        ok = av_frame_make_writable(audio) >= 0;
        if (ok) {
            av_samples_set_silence(audio->extended_data, 0, 1024, 2, AV_SAMPLE_FMT_FLTP);
            ok = encoder.pushAudioFrame(audio, i);
        }
    }
    av_frame_free(&audio);
    return ok && !encoder.finalize();
}

void selectOnly(Timeline &timeline, bool audio, int track, int index)
{
    for (const bool a : {false, true})
        for (TimelineTrack *t : a ? timeline.audioTracks() : timeline.videoTracks())
            if (t) t->clearClipSelection();
    timeline.trackAt(audio, track)->setSelectedClip(index);
}

// Dispatches a text/uri-list drop at timeSec through QObject::event (QApplication::notify
// does not deliver a Drop outside an active QDrag); returns whether it was accepted.
bool sendMediaDrop(TimelineTrack *dest, const QString &path, double timeSec)
{
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    QDropEvent event(QPointF(timeSec * dest->pixelsPerSecond(), 4.0), Qt::CopyAction, &mime,
                     Qt::LeftButton, Qt::NoModifier);
    event.ignore();
    static_cast<QObject *>(dest)->event(&event);
    return event.isAccepted();
}

// Same as sendMediaDrop with several URLs in one drop.
bool sendMediaDropUrls(TimelineTrack *dest, const QStringList &paths, double timeSec)
{
    QMimeData mime;
    QList<QUrl> urls;
    for (const QString &path : paths) urls.append(QUrl::fromLocalFile(path));
    mime.setUrls(urls);
    QDropEvent event(QPointF(timeSec * dest->pixelsPerSecond(), 4.0), Qt::CopyAction, &mime,
                     Qt::LeftButton, Qt::NoModifier);
    event.ignore();
    static_cast<QObject *>(dest)->event(&event);
    return event.isAccepted();
}

// Moves src's clip srcIdx onto dest at timeSec through the cross-track drop path.
bool sendClipDropOnTrack(TimelineTrack *src, int srcIdx, TimelineTrack *dest, double timeSec)
{
    QMimeData mime;
    mime.setData(QStringLiteral("application/x-vse-clip"),
                 QByteArray::number(srcIdx) + QByteArray(":0"));
    QDropEvent event(QPointF(timeSec * dest->pixelsPerSecond(), 4.0), Qt::MoveAction, &mime,
                     Qt::LeftButton, Qt::NoModifier);
    event.ignore();
    TimelineTrack::setSelftestDropSource(src);
    static_cast<QObject *>(dest)->event(&event);
    TimelineTrack::setSelftestDropSource(nullptr);
    return event.isAccepted();
}

// G31 (US-1303): clipKeysRemapped once per key-changing mutation, identity pairs removed.
void keyRemapGate(selftest::GateTally &tally)
{
    // No fixture has roto, matte or parent entries, so every emit comes from the key map alone.
    using Remap = Timeline::ClipKeyRemap;
    auto show = [](const QVector<Remap> &log) {
        QStringList parts;
        for (const Remap &r : log) {
            QStringList pairs;
            for (auto it = r.oldToNew.cbegin(); it != r.oldToNew.cend(); ++it)
                pairs.append(it.key() + QStringLiteral(">") + it.value());
            pairs.sort();
            QStringList dropped(r.dropped.cbegin(), r.dropped.cend());
            dropped.sort();
            parts.append(QStringLiteral("{%1 | dropped %2}").arg(pairs.join(','), dropped.join(',')));
        }
        return QStringLiteral("%1 emit(s) %2").arg(log.size()).arg(parts.join(' '));
    };
    auto record = [](Timeline &tl, QVector<Remap> &log) {
        QObject::connect(&tl, &Timeline::clipKeysRemapped, &tl,
                         [&log](const Remap &r) { log.append(r); });
    };
    auto remapOf = [](const QVector<QPair<QString, QString>> &pairs, const QStringList &dropped) {
        Remap r;
        for (const auto &p : pairs) r.oldToNew.insert(p.first, p.second);
        for (const QString &key : dropped) r.dropped.insert(key);
        return r;
    };
    // V1 [x 0-10], V2 [a 0-2 | b 3-5], V3 [c 10-12].
    auto fixture = [](Timeline &tl) {
        load(tl, {{clipFor("x", 10.0)}, {clipFor("a", 2.0), clipFor("b", 2.0, 1.0)},
                  {clipFor("c", 2.0, 10.0)}});
    };
    const Remap deleteA = remapOf({{"1:1", "1:0"}}, {"1:0"});

    // (1) delete V2 clip 0, then (7) undo and the same delete again.
    Timeline delTl;
    fixture(delTl);
    QVector<Remap> delLog;
    record(delTl, delLog);
    QString err;
    const bool deleted1 = delTl.deleteClipByIndex(false, 1, 0, false, &err);
    const QString delShown = show(delLog);
    const bool ok1 = deleted1 && delLog.size() == 1 && delLog[0] == deleteA;
    delLog.clear();
    delTl.undo();
    const QString undoShown = show(delLog);
    const bool undoSilent = delLog.isEmpty() && delTl.trackAt(false, 1)->clipCount() == 2;
    const bool deleted2 = delTl.deleteClipByIndex(false, 1, 0, false, &err);
    const QString redelShown = show(delLog);
    const bool ok7 = undoSilent && deleted2 && delLog.size() == 1 && delLog[0] == deleteA;

    // (9) delete the V2 tail, append a clip whose key map is identity only, then delete
    // it: the second delete is a new mutation and announces its drop again.
    const Remap deleteTail = remapOf({}, {"1:1"});
    Timeline tailTl;
    fixture(tailTl);
    QVector<Remap> tailLog;
    record(tailTl, tailLog);
    QString tailErr;
    const bool tailDeleted1 = tailTl.deleteClipByIndex(false, 1, 1, false, &tailErr);
    const QString tailDelShown = show(tailLog);
    const bool tailFirst = tailDeleted1 && tailLog.size() == 1 && tailLog[0] == deleteTail;
    tailLog.clear();
    ClipInfo shapeClip = clipFor("s", 1.0);
    Shape square;
    square.type = ShapeType::Rectangle;
    square.properties.size = QSizeF(10.0, 10.0);
    shapeClip.shapes = {square};
    tailTl.trackAt(false, 1)->setSelectedClip(0);
    tailTl.setPlayheadPosition(6.0);
    const bool appended = tailTl.insertShapeClipAtPlayhead(shapeClip)
        && tailTl.trackAt(false, 1)->clipCount() == 2;
    const QString appendShown = show(tailLog);
    const bool appendSilent = appended && tailLog.isEmpty();
    const bool tailDeleted2 = tailTl.deleteClipByIndex(false, 1, 1, false, &tailErr);
    const QString tailRedelShown = show(tailLog);
    const bool ok9 = tailFirst && appendSilent && tailDeleted2 && tailLog.size() == 1
        && tailLog[0] == deleteTail;

    // (2) V2 reorder: b to the front.
    Timeline moveTl;
    fixture(moveTl);
    QVector<Remap> moveLog;
    record(moveTl, moveLog);
    moveTl.trackAt(false, 1)->moveClip(1, 0);
    const QString moveShown = show(moveLog);
    const bool ok2 = moveTl.trackAt(false, 1)->clips().value(0).displayName == QStringLiteral("b")
        && moveLog.size() == 1 && moveLog[0] == remapOf({{"1:1", "1:0"}, {"1:0", "1:1"}}, {});

    // (3) b dropped from V2 onto V3 at 0 (before c).
    Timeline dropTl;
    fixture(dropTl);
    QVector<Remap> dropLog;
    record(dropTl, dropLog);
    const bool dropped = sendClipDropOnTrack(dropTl.trackAt(false, 1), 1, dropTl.trackAt(false, 2), 0.0)
        && dropTl.trackAt(false, 2)->clips().value(0).displayName == QStringLiteral("b");
    const QString dropShown = show(dropLog);
    const bool ok3 = dropped && dropLog.size() == 1
        && dropLog[0] == remapOf({{"1:1", "2:0"}, {"2:0", "2:1"}}, {});

    // (4) storyline mode: insert 5 s at the V1 head, then saveUndoState (whose reconcile
    // moves c, connected to y at 2, past the unconnected d: V2 [c | d] -> [d | c]).
    Timeline insTl;
    load(insTl, {{clipFor("y", 10.0)}, {clipFor("c", 1.0, 2.0), clipFor("d", 1.0, 1.0)}});
    const QString yUid = uidAt(insTl, 0, 0);
    const QString cUid = uidAt(insTl, 1, 0);
    const bool connected = insTl.connectClip(TrackKind::Video, 1, 0, yUid);
    insTl.setStorylineMode(true);
    QVector<Remap> insLog;
    record(insTl, insLog);
    insTl.storylineInsertClip(insTl.trackAt(false, 0), 0.0, clipFor("ins", 5.0));
    const int insertEmits = insLog.size();
    insTl.saveUndoState(QStringLiteral("G31 storyline insert"));
    const Located c = locate(insTl, false, cUid);
    const QString insShown = show(insLog);
    const bool ok4 = connected && c.track == 1 && c.index == 1 && near(c.startSec, 7.0)
        && insLog.size() == 1 && insLog[0] == remapOf({{"1:0", "1:1"}, {"1:1", "1:0"}}, {});

    // (5) the external catch-all twice with one snapshot.
    Timeline extTl;
    fixture(extTl);
    QVector<Remap> extLog;
    record(extTl, extLog);
    const TrackClipSnapshot snap = snapshotTrackClips(&extTl);
    TimelineTrack *extV2 = extTl.trackAt(false, 1);
    extV2->setClips({extV2->clips().at(1)});
    extTl.remapCarriersAfterExternalMutation(snap);
    const int extFirst = extLog.size();
    extTl.remapCarriersAfterExternalMutation(snap);
    const QString extShown = show(extLog);
    const bool ok5 = extFirst == 1 && extLog.size() == 1 && extLog[0] == deleteA;

    // (6) removing V2 renumbers tracks only.
    Timeline trkTl;
    fixture(trkTl);
    QVector<Remap> trkLog;
    record(trkTl, trkLog);
    int trackRemaps = 0;
    QObject::connect(&trkTl, &Timeline::trackIndicesRemapped, &trkTl,
                     [&trackRemaps](bool audio, QVector<int>) { if (!audio) ++trackRemaps; });
    QString trkErr;
    const bool removed = trkTl.removeTrack(false, 1, &trkErr);
    const QString trkShown = show(trkLog);
    const bool ok6 = removed && trackRemaps == 1 && trkLog.isEmpty();

    // (8) key-preserving edits through the paired remap sites.
    Timeline keepTl;
    fixture(keepTl);
    QVector<Remap> keepLog;
    record(keepTl, keepLog);
    const bool disabled = keepTl.setClipEnabled(TrackKind::Video, 1, 0, false);
    QString keepErr;
    const bool sped = keepTl.setClipPropertyByIndex(false, 1, 1, QStringLiteral("speed"), 2.0, &keepErr);
    const QString keepShown = show(keepLog);
    const bool ok8 = disabled && sped && keepLog.isEmpty();

    std::fprintf(stderr, "connected-clip G31 (1) delete=%d %s (expect 1 {1:1>1:0 | dropped 1:0}) ok=%d\n",
                 deleted1, qPrintable(delShown), ok1);
    std::fprintf(stderr, "connected-clip G31 (2) reorder %s (expect 1 {1:0>1:1,1:1>1:0}) ok=%d\n",
                 qPrintable(moveShown), ok2);
    std::fprintf(stderr, "connected-clip G31 (3) drop=%d %s (expect 1 {1:1>2:0,2:0>2:1}) ok=%d\n",
                 dropped, qPrintable(dropShown), ok3);
    std::fprintf(stderr, "connected-clip G31 (4) connected=%d insertEmits=%d (expect 0) c track=%d index=%d "
                 "start=%.6f (expect 1/1/7) %s (expect 1 {1:0>1:1,1:1>1:0}) ok=%d\n", connected,
                 insertEmits, c.track, c.index, c.startSec, qPrintable(insShown), ok4);
    std::fprintf(stderr, "connected-clip G31 (5) first=%d %s (expect 1 {1:1>1:0 | dropped 1:0}) ok=%d\n",
                 extFirst, qPrintable(extShown), ok5);
    std::fprintf(stderr, "connected-clip G31 (6) removed=%d [%s] trackIndicesRemapped=%d (expect 1) %s "
                 "(expect 0) ok=%d\n", removed, qPrintable(trkErr), trackRemaps, qPrintable(trkShown), ok6);
    std::fprintf(stderr, "connected-clip G31 (7) undo %s (expect 0) redelete=%d %s (expect 1 {1:1>1:0 | "
                 "dropped 1:0}) ok=%d\n", qPrintable(undoShown), deleted2, qPrintable(redelShown), ok7);
    std::fprintf(stderr, "connected-clip G31 (8) enabled=%d speed=%d [%s] %s (expect 0) ok=%d\n", disabled,
                 sped, qPrintable(keepErr), qPrintable(keepShown), ok8);
    std::fprintf(stderr, "connected-clip G31 (9) delete-tail=%d %s (expect 1 { | dropped 1:1}) append=%d %s "
                 "(expect 0) redelete=%d [%s] %s (expect 1 { | dropped 1:1}) ok=%d\n", tailDeleted1,
                 qPrintable(tailDelShown), appended, qPrintable(appendShown), tailDeleted2,
                 qPrintable(tailErr), qPrintable(tailRedelShown), ok9);
    tally.gate(31, ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9,
               "clipKeysRemapped was not emitted exactly once with the identity-free map for a delete / "
               "reorder / cross-track drop / storyline insert + reconcile / repeated external remap / "
               "delete after undo / delete after a key-preserving append, or it fired for a track removal "
               "or a key-preserving edit");
}

// US-1405 / US-1401 G36 (4): V2 = [a, b] and V3 = [a', b'] with a'.uid == a.uid, b'.uid == b.uid.
bool loadSharedSameOrder(Timeline &tl)
{
    ClipInfo a = clipFor("a", 2.0);
    ClipInfo b = clipFor("b", 2.0);
    ClipInfo a2 = clipFor("a2", 2.0);
    ClipInfo b2 = clipFor("b2", 2.0);
    a.uid = a2.uid = QStringLiteral("selftest-same-order-a");
    b.uid = b2.uid = QStringLiteral("selftest-same-order-b");
    load(tl, {{clipFor("x", 10.0)}, {a, b}, {a2, b2}});
    tl.trackAt(false, 1)->setClips({a, b});
    tl.trackAt(false, 2)->setClips({a2, b2});
    const QString v2a = tl.trackAt(false, 1)->clips().value(0).uid;
    const QString v2b = tl.trackAt(false, 1)->clips().value(1).uid;
    const QString v3a = tl.trackAt(false, 2)->clips().value(0).uid;
    const QString v3b = tl.trackAt(false, 2)->clips().value(1).uid;
    fprintf(stderr, "[connected-clip] loadSharedSameOrder a'.uid==a.uid:%d b'.uid==b.uid:%d\n",
            int(v3a == v2a), int(v3b == v2b));
    return v3a == v2a && v3b == v2b;
}

// G32 (US-1302) reaches MainWindow's private sidecars through the addresses the friend
// runConnectedClipSelftest hands over.
struct SidecarView {
    QHash<QString, TimeRemapClipEntry> *timeRemap = nullptr;
    QHash<QString, QJsonObject> *text3D = nullptr;
    QHash<QString, exprbind::ClipExpressionBindings> *expressions = nullptr;
    QHash<QString, wiggle::WiggleParams> *wiggle = nullptr;
    QVector<BrushAnimationEntry> *brush = nullptr;
    QHash<QString, BrushAnimation *> *liveBrush = nullptr;
    BrushAnimation *(MainWindow::*materializeBrush)(const QString &) = nullptr;
    void (MainWindow::*addBrush)() = nullptr;
    bool (MainWindow::*save)(const QString &, QString *) = nullptr;
    bool (MainWindow::*open)(const QString &, QString *, bool) = nullptr;
    QString (MainWindow::*brushPreviewClipId)() const = nullptr;
    void (MainWindow::*sourceInsert)(const threepoint::SourceSelection &) = nullptr;
    void (MainWindow::*sourceOverwrite)(const threepoint::SourceSelection &) = nullptr;
    void (MainWindow::*commitTimeRemap)(const QString &, const timeremap::TimeRemapCurve &) = nullptr;
    void (MainWindow::*commitText3D)(const QString &, const std::optional<QJsonObject> &) = nullptr;
    void (MainWindow::*commitExpressions)(const QString &,
                                          const std::optional<exprbind::ClipExpressionBindings> &) = nullptr;
    void (MainWindow::*commitWiggle)(const QString &, const std::optional<wiggle::WiggleParams> &) = nullptr;
    void (MainWindow::*editWiggle)() = nullptr;
};

// G32 (US-1302): the position-keyed sidecars follow the clip they were put on.
void sidecarRekeyGate(selftest::GateTally &tally, MainWindow &window, Timeline &tl,
                      const SidecarView &view)
{
    auto clearSidecars = [&] {
        view.timeRemap->clear();
        view.text3D->clear();
        view.expressions->clear();
        view.wiggle->clear();
        view.brush->clear();
        for (BrushAnimation *live : std::as_const(*view.liveBrush))
            if (live) live->deleteLater();
        view.liveBrush->clear();
    };
    auto sortedKeys = [](QStringList keys) { keys.sort(); return keys.join(','); };
    // Where each kind sits, the timeRemap value's own clipId in brackets.
    auto where = [&] {
        QStringList remaps;
        for (auto it = view.timeRemap->cbegin(); it != view.timeRemap->cend(); ++it)
            remaps.append(it.key() + QStringLiteral("(") + it.value().clipId + QStringLiteral(")"));
        QStringList brushes;
        for (const BrushAnimationEntry &entry : std::as_const(*view.brush))
            brushes.append(entry.clipId);
        return QStringLiteral("tr=%1 wg=%2 t3=%3 ex=%4 br=%5")
            .arg(sortedKeys(remaps), sortedKeys(view.wiggle->keys()), sortedKeys(view.text3D->keys()),
                 sortedKeys(view.expressions->keys()), sortedKeys(brushes));
    };
    auto at = [](const QString &key) {
        return QStringLiteral("tr=%1(%1) wg=%1 t3=%1 ex=%1 br=%1").arg(key);
    };
    auto owner = [&tl](const QString &key) {
        const QStringList parts = key.split(QLatin1Char(':'));
        const TimelineTrack *track = parts.size() == 2 ? tl.trackAt(false, parts[0].toInt()) : nullptr;
        const int index = parts.size() == 2 ? parts[1].toInt() : -1;
        return track && index >= 0 && index < track->clipCount()
            ? track->clips().at(index).displayName : QString();
    };
    // b's values: they move with b, so a value found on another key is b's.
    auto attach = [&](const QString &key) {
        TimeRemapClipEntry remap;
        remap.clipId = key;
        remap.curve.sourceFps = 10.0;
        remap.curve.addKey(0.0, 0.0);
        remap.curve.addKey(2.0, 1.0);
        view.timeRemap->insert(key, remap);
        wiggle::WiggleParams params;
        params.enabled = true;
        params.seed = 32;
        params.positionAmplitude = QPointF(4.0, 0.0);
        view.wiggle->insert(key, params);
        view.text3D->insert(key, QJsonObject{{QStringLiteral("text"), QStringLiteral("G32")}});
        exprbind::ClipExpressionBindings bindings;
        bindings.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("50"));
        view.expressions->insert(key, bindings);
        BrushAnimationEntry brush;
        brush.clipId = key;
        brush.brushData = QJsonObject{{QStringLiteral("text"), QStringLiteral("G32")}};
        view.brush->append(brush);
    };
    auto valuesIntact = [&](const QString &key) {
        return view.wiggle->value(key).seed == 32u
            && view.text3D->value(key).value(QStringLiteral("text")).toString() == QStringLiteral("G32")
            && view.expressions->value(key).expression(QStringLiteral("transform.opacity")) == QStringLiteral("50")
            && view.timeRemap->value(key).curve.keys.size() == 2
            && std::any_of(view.brush->cbegin(), view.brush->cend(), [&key](const BrushAnimationEntry &entry) {
                   return entry.clipId == key
                       && entry.brushData.value(QStringLiteral("text")).toString() == QStringLiteral("G32");
               });
    };

    // V1 [x 0-10], V2 [a 0-2 | b 3-5], V3 [c 10-12]; the sidecars go on b ("1:1").
    clearSidecars();
    load(tl, {{clipFor("x", 10.0)}, {clipFor("a", 2.0), clipFor("b", 2.0, 1.0)},
              {clipFor("c", 2.0, 10.0)}});
    attach(QStringLiteral("1:1"));
    BrushAnimation *live = (window.*view.materializeBrush)(QStringLiteral("1:1"));
    // Adding a brush animation records an undo point (its progress keyframes), so the
    // fixture does the same after attaching.
    tl.saveUndoState(QStringLiteral("G32 sidecars"));

    // (1) delete a: b becomes "1:0".
    QString err;
    const bool deleted = tl.deleteClipByIndex(false, 1, 0, false, &err);
    const QString after1 = where();
    const QString owner1 = owner(QStringLiteral("1:0"));
    const bool liveMoved = live && view.liveBrush->value(QStringLiteral("1:0")) == live
        && !view.liveBrush->contains(QStringLiteral("1:1"));
    const bool ok1 = deleted && after1 == at(QStringLiteral("1:0")) && owner1 == QStringLiteral("b")
        && valuesIntact(QStringLiteral("1:0")) && liveMoved;

    // (3) undo / redo.
    tl.undo();
    const QString afterUndo = where();
    const bool undoOk = afterUndo == at(QStringLiteral("1:1")) && owner(QStringLiteral("1:1")) == QStringLiteral("b")
        && valuesIntact(QStringLiteral("1:1"));
    tl.redo();
    const QString afterRedo = where();
    const bool redoOk = afterRedo == at(QStringLiteral("1:0")) && owner(QStringLiteral("1:0")) == QStringLiteral("b")
        && valuesIntact(QStringLiteral("1:0"));
    const bool ok3 = undoOk && redoOk;

    // (3') undo, then the same delete again.
    tl.undo();
    const QString afterUndo2 = where();
    QString err2;
    const bool redeleted = tl.deleteClipByIndex(false, 1, 0, false, &err2);
    const QString afterRedelete = where();
    const bool ok3b = afterUndo2 == at(QStringLiteral("1:1")) && redeleted
        && afterRedelete == at(QStringLiteral("1:0")) && owner(QStringLiteral("1:0")) == QStringLiteral("b")
        && valuesIntact(QStringLiteral("1:0"));

    // (2) b dropped from V2 onto V3 at 0 (before c): "2:0".
    const bool dropped = sendClipDropOnTrack(tl.trackAt(false, 1), 0, tl.trackAt(false, 2), 0.0);
    const QString after2 = where();
    const QString owner2 = owner(QStringLiteral("2:0"));
    const bool ok2 = dropped && after2 == at(QStringLiteral("2:0")) && owner2 == QStringLiteral("b")
        && valuesIntact(QStringLiteral("2:0"));

    // (4) save, then load into the same window.
    QTemporaryDir dir;
    const QString projectPath = dir.filePath(QStringLiteral("g32.veditor"));
    QString saveErr, openErr;
    const bool saved = dir.isValid() && (window.*view.save)(projectPath, &saveErr);
    clearSidecars();
    const bool opened = saved && (window.*view.open)(projectPath, &openErr, false);
    const QString after4 = where();
    const QString owner4 = owner(QStringLiteral("2:0"));
    const bool ok4 = opened && after4 == at(QStringLiteral("2:0")) && owner4 == QStringLiteral("b")
        && valuesIntact(QStringLiteral("2:0"));

    // (5) remove V2 (empty now): V3 becomes V2, b "1:0".
    QString removeErr;
    const bool removed = tl.removeTrack(false, 1, &removeErr);
    const QString after5 = where();
    const QString owner5 = owner(QStringLiteral("1:0"));
    const bool ok5 = removed && after5 == at(QStringLiteral("1:0")) && owner5 == QStringLiteral("b")
        && valuesIntact(QStringLiteral("1:0"));

    // (6) wiggle / text3D / timeRemap / expression edits committed (US-1401: one undo point
    // each), then an unrelated undoable edit (x at speed 2), then one undo: the speed goes back,
    // the sidecar values stay.
    wiggle::WiggleParams editedWiggle = view.wiggle->value(QStringLiteral("1:0"));
    editedWiggle.seed = 77;
    (window.*view.commitWiggle)(QStringLiteral("1:0"), editedWiggle);
    (window.*view.commitText3D)(QStringLiteral("1:0"),
                                QJsonObject{{QStringLiteral("text"), QStringLiteral("edited")}});
    timeremap::TimeRemapCurve editedCurve = view.timeRemap->value(QStringLiteral("1:0")).curve;
    editedCurve.sourceFps = 24.0;
    editedCurve.addKey(1.0, 0.5);
    (window.*view.commitTimeRemap)(QStringLiteral("1:0"), editedCurve);
    exprbind::ClipExpressionBindings editedBindings = view.expressions->value(QStringLiteral("1:0"));
    editedBindings.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("75"));
    (window.*view.commitExpressions)(QStringLiteral("1:0"), editedBindings);
    QString speedErr;
    const bool sped = tl.setClipPropertyByIndex(false, 0, 0, QStringLiteral("speed"), 2.0, &speedErr);
    tl.undo();
    const double speedAfterUndo = tl.trackAt(false, 0)->clips().value(0).speed;
    const QString after6 = where();
    const unsigned seedAfterUndo = view.wiggle->value(QStringLiteral("1:0")).seed;
    const QString textAfterUndo = view.text3D->value(QStringLiteral("1:0")).value(QStringLiteral("text")).toString();
    const TimeRemapClipEntry remapAfterUndo = view.timeRemap->value(QStringLiteral("1:0"));
    const int remapKeysAfterUndo = static_cast<int>(remapAfterUndo.curve.keys.size());
    const double fpsAfterUndo = remapAfterUndo.curve.sourceFps;
    const QString exprAfterUndo =
        view.expressions->value(QStringLiteral("1:0")).expression(QStringLiteral("transform.opacity"));
    const bool ok6 = sped && near(speedAfterUndo, 1.0) && after6 == at(QStringLiteral("1:0"))
        && seedAfterUndo == 77u && textAfterUndo == QStringLiteral("edited") && remapKeysAfterUndo == 3
        && near(fpsAfterUndo, 24.0) && exprAfterUndo == QStringLiteral("75");

    // (7) delete b itself, then its track: each undo brings all five back on b, each redo
    // takes them off again.
    clearSidecars();
    load(tl, {{clipFor("x", 10.0)}, {clipFor("a", 2.0), clipFor("b", 2.0, 1.0)},
              {clipFor("c", 2.0, 10.0)}});
    attach(QStringLiteral("1:1"));
    tl.saveUndoState(QStringLiteral("G32 sidecars"));
    const QString none = QStringLiteral("tr= wg= t3= ex= br=");
    auto onB = [&](const QString &shown) {
        return shown == at(QStringLiteral("1:1")) && owner(QStringLiteral("1:1")) == QStringLiteral("b")
            && valuesIntact(QStringLiteral("1:1"));
    };
    QString errB;
    const bool bDeleted = tl.deleteClipByIndex(false, 1, 1, false, &errB);
    const QString afterBDelete = where();
    tl.undo();
    const QString afterBUndo = where();
    const bool bUndoOk = onB(afterBUndo);
    tl.redo();
    const QString afterBRedo = where();
    tl.undo();
    const QString afterBUndo2 = where();
    const bool bUndo2Ok = onB(afterBUndo2);
    const bool ok7 = bDeleted && afterBDelete == none && bUndoOk && afterBRedo == none && bUndo2Ok;
    QString errT;
    const bool v2Removed = tl.removeTrack(false, 1, &errT);
    const QString afterTrackRemove = where();
    tl.undo();
    const QString afterTrackUndo = where();
    const bool trackUndoOk = onB(afterTrackUndo);
    tl.redo();
    const QString afterTrackRedo = where();
    tl.undo();
    const QString afterTrackUndo2 = where();
    const bool trackUndo2Ok = onB(afterTrackUndo2);
    const bool ok8 = v2Removed && afterTrackRemove == none && trackUndoOk && afterTrackRedo == none
        && trackUndo2Ok;

    // (8) the real brush add with no clip selected (the dialog accepted as it opens): the
    // brush goes on x ("0:0") and survives an unrelated edit and its undo.
    tl.clearSelection();
    const int selectedBefore = tl.selectedVideoClipIndex();
    int brushDialogs = 0;
    int otherModals = 0;
    {
        QTimer acceptBrush;
        acceptBrush.setInterval(20);
        QObject::connect(&acceptBrush, &QTimer::timeout, &window, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal) return;
            auto *dialog = qobject_cast<QDialog *>(modal);
            if (dialog && dialog->inherits("BrushAnimationDialog")) {
                ++brushDialogs;
                dialog->accept();
            } else {
                ++otherModals;
                if (dialog) dialog->reject();
                else modal->close();
            }
        });
        acceptBrush.start();
        (window.*view.addBrush)();
    }
    auto brushOnX = [&] {
        return std::any_of(view.brush->cbegin(), view.brush->cend(), [](const BrushAnimationEntry &entry) {
            return entry.clipId == QStringLiteral("0:0") && !entry.brushData.isEmpty();
        });
    };
    const bool brushAdded = brushOnX();
    QString speedErr2;
    const bool sped2 = tl.setClipPropertyByIndex(false, 0, 0, QStringLiteral("speed"), 2.0, &speedErr2);
    tl.undo();
    const double speedAfterUndo2 = tl.trackAt(false, 0)->clips().value(0).speed;
    const QString afterBrushUndo = where();
    const bool brushKept = brushOnX();
    const bool ok9 = selectedBefore < 0 && brushDialogs == 1 && otherModals == 0 && brushAdded && sped2
        && near(speedAfterUndo2, 1.0) && brushKept && QApplication::activeModalWidget() == nullptr;

    // (7b) the only clip s ("0:0") deleted, undone and redone in one sequence: the redo takes
    // all five off s again, and a new clip put at "0:0" afterwards carries none of them.
    clearSidecars();
    load(tl, {{clipFor("s", 2.0)}});
    attach(QStringLiteral("0:0"));
    tl.saveUndoState(QStringLiteral("G32 sidecars"));
    QString errS;
    const bool sDeleted = tl.deleteClipByIndex(false, 0, 0, false, &errS);
    const QString afterSDelete = where();
    tl.undo();
    const QString afterSUndo = where();
    const bool sUndoOk = afterSUndo == at(QStringLiteral("0:0")) && owner(QStringLiteral("0:0")) == QStringLiteral("s")
        && valuesIntact(QStringLiteral("0:0"));
    tl.redo();
    const QString afterSRedo = where();
    tl.trackAt(false, 0)->addClip(clipFor("n", 2.0));
    const QString afterNewClip = where();
    const QString newOwner = owner(QStringLiteral("0:0"));
    const bool ok7b = sDeleted && afterSDelete == none && sUndoOk && afterSRedo == none
        && newOwner == QStringLiteral("n") && afterNewClip == none;

    // (9) uids are unique only per track: V3's c shares b's uid (load fixture). (i) delete a,
    // undo, redo: the values follow b ("1:1" / "1:0"), never c ("2:0"). (ii) delete b itself,
    // undo: they come back on b although c keeps the uid live.
    auto loadShared = [&] {
        clearSidecars();
        ClipInfo b = clipFor("b", 2.0, 1.0);
        ClipInfo c = clipFor("c", 2.0, 10.0);
        b.uid = c.uid = QStringLiteral("selftest-g32-shared-uid");
        // restoreFromProject repairs a uid shared across tracks (US-1405), so load first and
        // overwrite V3's clip afterwards with setClips (which keeps it, one clip per track).
        load(tl, {{clipFor("x", 10.0)}, {clipFor("a", 2.0), b}, {c}});
        tl.trackAt(false, 2)->setClips({c});
        attach(QStringLiteral("1:1"));
        tl.saveUndoState(QStringLiteral("G32 sidecars"));
        return tl.trackAt(false, 1)->clips().value(1).uid == b.uid
            && tl.trackAt(false, 2)->clips().value(0).uid == b.uid;
    };
    clearSidecars();
    (void)loadSharedSameOrder(tl);
    const bool sharedLoaded1 = loadShared();
    QString errSa;
    const bool saDeleted = tl.deleteClipByIndex(false, 1, 0, false, &errSa);
    tl.undo();
    const QString afterSaUndo = where();
    const bool saUndoOk = onB(afterSaUndo);
    tl.redo();
    const QString afterSaRedo = where();
    const bool saRedoOk = afterSaRedo == at(QStringLiteral("1:0")) && owner(QStringLiteral("1:0")) == QStringLiteral("b")
        && valuesIntact(QStringLiteral("1:0"));
    const bool ok9i = sharedLoaded1 && saDeleted && saUndoOk && saRedoOk;
    const bool sharedLoaded2 = loadShared();
    QString errSb;
    const bool sbDeleted = tl.deleteClipByIndex(false, 1, 1, false, &errSb);
    const QString afterSbDelete = where();
    tl.undo();
    const QString afterSbUndo = where();
    const bool sbUndoOk = onB(afterSbUndo);
    // The redo takes them off b again (nothing left on "1:1"), a new clip appended to V2
    // carries none of them, and the next undo brings them back on b.
    tl.redo();
    const QString afterSbRedo = where();
    tl.trackAt(false, 1)->addClip(clipFor("n", 2.0, 1.0));
    const QString afterSbNewClip = where();
    const QString sbNewOwner = owner(QStringLiteral("1:1"));
    tl.undo();
    const QString afterSbUndo2 = where();
    const bool sbUndo2Ok = onB(afterSbUndo2);
    const bool ok9ii = sharedLoaded2 && sbDeleted && afterSbDelete == none && sbUndoOk
        && afterSbRedo == none && sbNewOwner == QStringLiteral("n") && afterSbNewClip == none
        && sbUndo2Ok;

    // (9iii) the shared-uid fixture with distinct values on b ("1:1") and c ("2:0"), then V2
    // removed: c's values follow c to "1:0" and none of b's stay; the undo puts each back on
    // its own clip (no swap through the shared uid); the redo moves c's to "1:0" again.
    auto attachC = [&](const QString &key) {
        TimeRemapClipEntry remap;
        remap.clipId = key;
        remap.curve.sourceFps = 10.0;
        remap.curve.addKey(0.0, 0.0);
        remap.curve.addKey(1.0, 0.25);
        remap.curve.addKey(2.0, 1.0);
        view.timeRemap->insert(key, remap);
        wiggle::WiggleParams params;
        params.enabled = true;
        params.seed = 33;
        view.wiggle->insert(key, params);
        view.text3D->insert(key, QJsonObject{{QStringLiteral("text"), QStringLiteral("G32c")}});
        exprbind::ClipExpressionBindings bindings;
        bindings.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("60"));
        view.expressions->insert(key, bindings);
        BrushAnimationEntry brush;
        brush.clipId = key;
        brush.brushData = QJsonObject{{QStringLiteral("text"), QStringLiteral("G32c")}};
        view.brush->append(brush);
    };
    auto cIntact = [&](const QString &key) {
        return view.wiggle->value(key).seed == 33u
            && view.text3D->value(key).value(QStringLiteral("text")).toString() == QStringLiteral("G32c")
            && view.expressions->value(key).expression(QStringLiteral("transform.opacity")) == QStringLiteral("60")
            && view.timeRemap->value(key).curve.keys.size() == 3
            && std::any_of(view.brush->cbegin(), view.brush->cend(), [&key](const BrushAnimationEntry &entry) {
                   return entry.clipId == key
                       && entry.brushData.value(QStringLiteral("text")).toString() == QStringLiteral("G32c");
               });
    };
    const bool sharedLoaded3 = loadShared();
    attachC(QStringLiteral("2:0"));
    tl.saveUndoState(QStringLiteral("G32 sidecars on c"));
    const QString onBAndC = QStringLiteral("tr=1:1(1:1),2:0(2:0) wg=1:1,2:0 t3=1:1,2:0 ex=1:1,2:0 br=1:1,2:0");
    QString errSt;
    const bool stRemoved = tl.removeTrack(false, 1, &errSt);
    const QString afterStRemove = where();
    const bool stRemoveOk = afterStRemove == at(QStringLiteral("1:0")) && owner(QStringLiteral("1:0")) == QStringLiteral("c")
        && cIntact(QStringLiteral("1:0"));
    tl.undo();
    const QString afterStUndo = where();
    const bool stUndoOk = afterStUndo == onBAndC && owner(QStringLiteral("1:1")) == QStringLiteral("b")
        && owner(QStringLiteral("2:0")) == QStringLiteral("c") && valuesIntact(QStringLiteral("1:1"))
        && cIntact(QStringLiteral("2:0"));
    tl.redo();
    const QString afterStRedo = where();
    const bool stRedoOk = afterStRedo == at(QStringLiteral("1:0")) && owner(QStringLiteral("1:0")) == QStringLiteral("c")
        && cIntact(QStringLiteral("1:0"));
    const bool ok9iii = sharedLoaded3 && stRemoved && stRemoveOk && stUndoOk && stRedoOk;

    std::fprintf(stderr, "connected-clip G32 (1) delete=%d %s owner=%s liveMoved=%d (expect %s on b) ok=%d\n",
                 deleted, qPrintable(after1), qPrintable(owner1), liveMoved,
                 qPrintable(at(QStringLiteral("1:0"))), ok1);
    std::fprintf(stderr, "connected-clip G32 (3) undo %s (expect %s) redo %s (expect %s) ok=%d\n",
                 qPrintable(afterUndo), qPrintable(at(QStringLiteral("1:1"))), qPrintable(afterRedo),
                 qPrintable(at(QStringLiteral("1:0"))), ok3);
    std::fprintf(stderr, "connected-clip G32 (3') undo %s redelete=%d [%s] %s (expect %s) ok=%d\n",
                 qPrintable(afterUndo2), redeleted, qPrintable(err2), qPrintable(afterRedelete),
                 qPrintable(at(QStringLiteral("1:0"))), ok3b);
    std::fprintf(stderr, "connected-clip G32 (2) drop=%d %s owner=%s (expect %s on b) ok=%d\n", dropped,
                 qPrintable(after2), qPrintable(owner2), qPrintable(at(QStringLiteral("2:0"))),
                 ok2);
    std::fprintf(stderr, "connected-clip G32 (4) saved=%d [%s] opened=%d [%s] %s owner=%s (expect %s on b) ok=%d\n",
                 saved, qPrintable(saveErr), opened, qPrintable(openErr), qPrintable(after4),
                 qPrintable(owner4), qPrintable(at(QStringLiteral("2:0"))), ok4);
    std::fprintf(stderr, "connected-clip G32 (5) removed=%d [%s] %s owner=%s (expect %s on b) ok=%d\n", removed,
                 qPrintable(removeErr), qPrintable(after5), qPrintable(owner5),
                 qPrintable(at(QStringLiteral("1:0"))), ok5);
    std::fprintf(stderr, "connected-clip G32 (6) sped=%d [%s] speedAfterUndo=%.3f (expect 1) %s seed=%u (expect 77) "
                 "text3D=%s (expect edited) timeRemapKeys=%d (expect 3) sourceFps=%.3f (expect 24) "
                 "expression=%s (expect 75) ok=%d\n", sped, qPrintable(speedErr), speedAfterUndo, qPrintable(after6),
                 seedAfterUndo, qPrintable(textAfterUndo), remapKeysAfterUndo, fpsAfterUndo,
                 qPrintable(exprAfterUndo), ok6);
    std::fprintf(stderr, "connected-clip G32 (7) delete b=%d [%s] %s (expect %s) undo %s redo %s undo %s "
                 "(expect %s on b) | remove V2=%d [%s] %s undo %s redo %s undo %s ok=%d\n", bDeleted,
                 qPrintable(errB), qPrintable(afterBDelete), qPrintable(none), qPrintable(afterBUndo),
                 qPrintable(afterBRedo), qPrintable(afterBUndo2), qPrintable(at(QStringLiteral("1:1"))),
                 v2Removed, qPrintable(errT), qPrintable(afterTrackRemove), qPrintable(afterTrackUndo),
                 qPrintable(afterTrackRedo), qPrintable(afterTrackUndo2), ok7 && ok8);
    std::fprintf(stderr, "connected-clip G32 (7b) delete s=%d [%s] %s (expect %s) undo %s (expect %s on s) "
                 "redo %s (expect %s) new clip %s at 0:0 %s (expect %s) ok=%d\n", sDeleted, qPrintable(errS),
                 qPrintable(afterSDelete), qPrintable(none), qPrintable(afterSUndo),
                 qPrintable(at(QStringLiteral("0:0"))), qPrintable(afterSRedo), qPrintable(none),
                 qPrintable(newOwner), qPrintable(afterNewClip), qPrintable(none), ok7b);
    std::fprintf(stderr, "connected-clip G32 (8) selected=%d (expect -1) brushDialogs=%d (expect 1) otherModals=%d "
                 "added=%d sped=%d [%s] speedAfterUndo=%.3f (expect 1) %s brushKept=%d ok=%d\n", selectedBefore,
                 brushDialogs, otherModals, brushAdded, sped2, qPrintable(speedErr2), speedAfterUndo2,
                 qPrintable(afterBrushUndo), brushKept, ok9);
    std::fprintf(stderr, "connected-clip G32 (9i) shared uid b/c loaded=%d delete a=%d [%s] undo %s (expect %s on b) "
                 "redo %s (expect %s on b) ok=%d\n", sharedLoaded1, saDeleted, qPrintable(errSa),
                 qPrintable(afterSaUndo), qPrintable(at(QStringLiteral("1:1"))), qPrintable(afterSaRedo),
                 qPrintable(at(QStringLiteral("1:0"))), ok9i);
    std::fprintf(stderr, "connected-clip G32 (9ii) shared uid b/c loaded=%d delete b=%d [%s] %s (expect %s) undo %s "
                 "(expect %s on b) redo %s (expect %s) new clip %s at 1:1 %s (expect %s) undo %s (expect %s on b) "
                 "ok=%d\n", sharedLoaded2, sbDeleted, qPrintable(errSb),
                 qPrintable(afterSbDelete), qPrintable(none), qPrintable(afterSbUndo),
                 qPrintable(at(QStringLiteral("1:1"))), qPrintable(afterSbRedo), qPrintable(none),
                 qPrintable(sbNewOwner), qPrintable(afterSbNewClip), qPrintable(none), qPrintable(afterSbUndo2),
                 qPrintable(at(QStringLiteral("1:1"))), ok9ii);
    std::fprintf(stderr, "connected-clip G32 (9iii) shared uid b/c distinct values loaded=%d remove V2=%d [%s] %s "
                 "(expect %s on c) undo %s (expect %s, b's on 1:1 and c's on 2:0) redo %s (expect %s on c) ok=%d\n",
                 sharedLoaded3, stRemoved, qPrintable(errSt), qPrintable(afterStRemove),
                 qPrintable(at(QStringLiteral("1:0"))), qPrintable(afterStUndo), qPrintable(onBAndC),
                 qPrintable(afterStRedo), qPrintable(at(QStringLiteral("1:0"))), ok9iii);
    tally.gate(32, ok1 && ok2 && ok3 && ok3b && ok4 && ok5 && ok6 && ok7 && ok7b && ok8 && ok9 && ok9i && ok9ii
                   && ok9iii,
               "a position-keyed sidecar (timeRemap / wiggle / text3D / expression / brush) did not follow b "
               "through a delete, undo / redo, the delete after an undo, a cross-track drop, a save / load or a "
               "track removal, a committed edit was rolled back by an unrelated undo, deleting b "
               "or its track and undoing did not bring them back, redoing the delete of a sequence's only clip "
               "left them on its key, a brush added with no selection was lost "
               "to an unrelated undo, or a uid shared with a clip on another track sent them to the wrong "
               "clip or lost them on undo / redo or a track removal");
    clearSidecars();
}

// G36 (US-1401): the timeRemap / text3D / expression / wiggle commits record one undo point
// each, and an undo / redo takes the four back by value with the clips.
void sidecarUndoValuesGate(selftest::GateTally &tally, MainWindow &window, Timeline &tl,
                           const SidecarView &view)
{
    auto clearFour = [&] {
        view.timeRemap->clear();
        view.text3D->clear();
        view.expressions->clear();
        view.wiggle->clear();
    };
    auto depth = [&tl] { return tl.undoManager()->currentIndex(); };
    auto clipAt = [&tl](int track, int index) {
        const TimelineTrack *t = tl.trackAt(false, track);
        return t && index >= 0 && index < t->clipCount() ? t->clips().at(index) : ClipInfo{};
    };
    // Tag n: a remap through (1, n / 1000), wiggle seed n, text "G36-n", opacity "n".
    auto curveFor = [](int n) {
        timeremap::TimeRemapCurve curve;
        curve.sourceFps = 10.0;
        curve.addKey(0.0, 0.0);
        curve.addKey(1.0, n / 1000.0);
        curve.addKey(2.0, 1.0);
        return curve;
    };
    auto wiggleFor = [](int n) {
        wiggle::WiggleParams params;
        params.enabled = true;
        params.seed = unsigned(n);
        params.positionAmplitude = QPointF(3.0, 0.0);
        return params;
    };
    auto textFor = [](int n) { return QJsonObject{{QStringLiteral("text"), QStringLiteral("G36-%1").arg(n)}}; };
    auto bindingsFor = [](int n) {
        exprbind::ClipExpressionBindings bindings;
        bindings.setExpression(QStringLiteral("transform.opacity"), QString::number(n));
        return bindings;
    };
    auto put = [&](const QString &key, int n) {
        view.timeRemap->insert(key, TimeRemapClipEntry{key, curveFor(n)});
        view.wiggle->insert(key, wiggleFor(n));
        view.text3D->insert(key, textFor(n));
        view.expressions->insert(key, bindingsFor(n));
    };
    auto commitAll = [&](const QString &key, int n) {
        (window.*view.commitTimeRemap)(key, curveFor(n));
        (window.*view.commitWiggle)(key, wiggleFor(n));
        (window.*view.commitText3D)(key, textFor(n));
        (window.*view.commitExpressions)(key, bindingsFor(n));
    };
    // The tag each kind carries on `key` ("-" when absent; "!id" when the remap's clipId differs).
    auto tagsAt = [&](const QString &key) {
        const auto tr = view.timeRemap->constFind(key);
        const QString trTag = tr == view.timeRemap->cend() ? QStringLiteral("-")
            : QString::number(qRound(tr->curve.srcTimeAt(1.0) * 1000.0))
                + (tr->clipId == key ? QString() : QStringLiteral("!id"));
        const QString wgTag = view.wiggle->contains(key) ? QString::number(view.wiggle->value(key).seed)
                                                          : QStringLiteral("-");
        const QString t3Tag = view.text3D->contains(key)
            ? view.text3D->value(key).value(QStringLiteral("text")).toString().mid(4) : QStringLiteral("-");
        const QString exTag = view.expressions->contains(key)
            ? view.expressions->value(key).expression(QStringLiteral("transform.opacity")) : QStringLiteral("-");
        return QStringLiteral("tr=%1 wg=%2 t3=%3 ex=%4").arg(trTag, wgTag, t3Tag, exTag);
    };
    auto tagged = [](int n) { return QStringLiteral("tr=%1 wg=%1 t3=%1 ex=%1").arg(n); };
    const QString absent = QStringLiteral("tr=- wg=- t3=- ex=-");

    // (1) / (1') / (2): V1 [x 0-10], V2 [a 0-2 | b 3-5], b forward over source [0.5, 2.5]; the
    // curve's source seconds stay in [0, out - in] = [0, 2].
    clearFour();
    ClipInfo b = clipFor("b", 3.0, 1.0);
    b.inPoint = 0.5;
    b.outPoint = 2.5;
    load(tl, {{clipFor("x", 10.0)}, {clipFor("a", 2.0), b}});
    const QString bKey = QStringLiteral("1:1");
    timeremap::TimeRemapCurve curve;
    curve.sourceFps = 10.0;
    curve.addKey(0.0, 0.0);
    curve.addKey(1.0, 0.4);
    curve.addKey(2.0, 1.6);
    auto entryOnClip = [&] {
        return view.timeRemap->contains(bKey) && view.timeRemap->value(bKey).clipId == bKey
            && view.timeRemap->value(bKey).curve.toJson() == curve.toJson()
            && clipAt(1, 1).timeRemapCurve.toJson() == curve.toJson();
    };
    const int d0 = depth();
    (window.*view.commitTimeRemap)(bKey, curve);
    const int d1 = depth();
    const bool committed = entryOnClip();
    tl.undo();
    const bool undoGone = !view.timeRemap->contains(bKey) && !clipAt(1, 1).hasTimeRemap();
    tl.redo();
    const bool redoBack = entryOnClip();
    const bool ok1 = d1 == d0 + 1 && committed && undoGone && redoBack;
    (window.*view.commitTimeRemap)(bKey, curve);
    const int d1b = depth();
    const bool ok1b = d1b == d1 && entryOnClip();

    // (2) an unrelated undoable edit (x at speed 2) after the remap, then one undo.
    QString speedErr;
    const bool sped = tl.setClipPropertyByIndex(false, 0, 0, QStringLiteral("speed"), 2.0, &speedErr);
    tl.undo();
    const ClipInfo b2 = clipAt(1, 1);
    const TimeRemapClipEntry entry2 = view.timeRemap->value(bKey);
    bool ok2 = sped && near(clipAt(0, 0).speed, 1.0) && b2.displayName == QStringLiteral("b") && !b2.reversed
        && view.timeRemap->contains(bKey) && entry2.curve.toJson() == b2.timeRemapCurve.toJson();
    QStringList samples;
    const double localSecs[] = {0.5, 1.0, 1.5};
    const double expectSecs[] = {0.2, 0.4, 1.0};
    for (int i = 0; i < 3; ++i) {
        const double fromEntry = entry2.curve.srcTimeAt(localSecs[i]);
        const double fromClip = b2.sourceSecondAtLocalTime(localSecs[i]) - b2.inPoint;
        samples.append(QStringLiteral("local=%1 entry=%2 clip=%3 (expect %4)").arg(localSecs[i])
                           .arg(fromEntry, 0, 'f', 6).arg(fromClip, 0, 'f', 6).arg(expectSecs[i]));
        ok2 = ok2 && std::abs(fromEntry - fromClip) < 1e-6 && std::abs(fromEntry - expectSecs[i]) < 1e-6;
    }

    // (3) text3D / expression / wiggle: commit then remove (nullopt), +1 each; two undos and two
    // redos walk present / absent.
    QStringList kinds;
    auto roundTrip = [&](const char *name, const std::function<void()> &commit,
                         const std::function<void()> &remove, const std::function<bool()> &present) {
        const int before = depth();
        commit();
        const int afterCommit = depth();
        const bool p1 = present();
        remove();
        const int afterRemove = depth();
        const bool p2 = present();
        tl.undo();
        const bool u1 = present();
        tl.undo();
        const bool u2 = present();
        tl.redo();
        const bool r1 = present();
        tl.redo();
        const bool r2 = present();
        const bool ok = afterCommit == before + 1 && afterRemove == before + 2 && p1 && !p2 && u1 && !u2
            && r1 && !r2;
        kinds.append(QStringLiteral("%1 depth %2->%3->%4 present commit=%5 remove=%6 undo=%7 undo=%8 redo=%9 "
                                    "redo=%10 ok=%11").arg(QLatin1String(name)).arg(before).arg(afterCommit)
                         .arg(afterRemove).arg(int(p1)).arg(int(p2)).arg(int(u1)).arg(int(u2)).arg(int(r1))
                         .arg(int(r2)).arg(int(ok)));
        return ok;
    };
    const bool ok3t = roundTrip("text3D", [&] { (window.*view.commitText3D)(bKey, textFor(3)); },
        [&] { (window.*view.commitText3D)(bKey, std::nullopt); },
        [&] { return view.text3D->value(bKey) == textFor(3); });
    const bool ok3e = roundTrip("expression", [&] { (window.*view.commitExpressions)(bKey, bindingsFor(3)); },
        [&] { (window.*view.commitExpressions)(bKey, std::nullopt); },
        [&] { return view.expressions->contains(bKey)
                  && view.expressions->value(bKey).toJson() == bindingsFor(3).toJson(); });
    const bool ok3w = roundTrip("wiggle", [&] { (window.*view.commitWiggle)(bKey, wiggleFor(3)); },
        [&] { (window.*view.commitWiggle)(bKey, std::nullopt); },
        [&] { return view.wiggle->contains(bKey)
                  && wiggle::toJson(view.wiggle->value(bKey)) == wiggle::toJson(wiggleFor(3)); });
    const bool ok3 = ok3t && ok3e && ok3w;

    // (4) V2 = [a, b] and V3 = [a', b'] with the same uids in the same order (tags 10 / 11 and
    // 20 / 21): moving V2 below V3 and undoing does not swap the two tracks' values.
    clearFour();
    const bool sameOrder = loadSharedSameOrder(tl);
    put(QStringLiteral("1:0"), 10);
    put(QStringLiteral("1:1"), 11);
    put(QStringLiteral("2:0"), 20);
    put(QStringLiteral("2:1"), 21);
    tl.saveUndoState(QStringLiteral("G36 sidecars"));
    auto fourKeys = [&] {
        return QStringLiteral("1:0[%1] 1:1[%2] 2:0[%3] 2:1[%4] owners=%5,%6,%7,%8")
            .arg(tagsAt(QStringLiteral("1:0")), tagsAt(QStringLiteral("1:1")), tagsAt(QStringLiteral("2:0")),
                 tagsAt(QStringLiteral("2:1")), clipAt(1, 0).displayName, clipAt(1, 1).displayName,
                 clipAt(2, 0).displayName, clipAt(2, 1).displayName);
    };
    const QString placed = QStringLiteral("1:0[%1] 1:1[%2] 2:0[%3] 2:1[%4] owners=a,b,a2,b2")
                               .arg(tagged(10), tagged(11), tagged(20), tagged(21));
    const QString beforeMove = fourKeys();
    QString moveErr;
    const bool moved = tl.moveTrack(false, 1, 2, &moveErr);
    const QString afterMove = fourKeys();
    tl.undo();
    const QString afterMoveUndo = fourKeys();
    tl.redo();
    const QString afterMoveRedo = fourKeys();
    const bool ok4 = sameOrder && beforeMove == placed && moved && afterMoveUndo == placed
        && afterMoveRedo == afterMove;

    // (5) V1 [x], V2 [a | b], V3 [c] with c sharing b's uid (US-1405: load, then setClips):
    // V2 removed, then the track c moved up to; two undos bring back b's 31 and c's 32.
    clearFour();
    load(tl, {{clipFor("x", 10.0)}, {clipFor("a", 2.0), clipFor("b", 2.0, 1.0)}, {clipFor("c", 2.0, 10.0)}});
    const QString sharedUid = uidAt(tl, 1, 1);
    shareUid(tl, 2, 0, sharedUid);
    const bool shared = !sharedUid.isEmpty() && uidAt(tl, 2, 0) == sharedUid;
    put(QStringLiteral("1:1"), 31);
    put(QStringLiteral("2:0"), 32);
    tl.saveUndoState(QStringLiteral("G36 sidecars"));
    QString removeErr1, removeErr2;
    const bool removed1 = tl.removeTrack(false, 1, &removeErr1);
    const QString afterRemove1 = tagsAt(QStringLiteral("1:0")) + QStringLiteral(" owner=") + clipAt(1, 0).displayName;
    const bool removed2 = tl.removeTrack(false, 1, &removeErr2);
    const int tracksAfterRemove2 = tl.videoTrackCount();
    // Step 0 (r2 risk 11): what restoreState's removeTrackInternal / addVideoTrack announce.
    int keyRemaps = 0;
    int trackRemaps = 0;
    const auto keySpy = QObject::connect(&tl, &Timeline::clipKeysRemapped, &tl, [&](const Timeline::ClipKeyRemap &) {
        ++keyRemaps;
    });
    const auto trackSpy = QObject::connect(&tl, &Timeline::trackIndicesRemapped, &tl, [&](bool, const QVector<int> &) {
        ++trackRemaps;
    });
    tl.undo();
    const QString afterUndo1 = tagsAt(QStringLiteral("1:0")) + QStringLiteral(" owner=") + clipAt(1, 0).displayName;
    tl.undo();
    const QString afterUndo2 = tagsAt(QStringLiteral("1:1")) + QStringLiteral(" owner=") + clipAt(1, 1).displayName
        + QStringLiteral(" | ") + tagsAt(QStringLiteral("2:0")) + QStringLiteral(" owner=") + clipAt(2, 0).displayName;
    const qsizetype sidecarBytes =
        QJsonDocument(tl.currentState().sidecarState).toJson(QJsonDocument::Compact).size();
    const int keyRemapsUndo = keyRemaps;
    const int trackRemapsUndo = trackRemaps;
    // The redo of the first removal goes through removeTrackInternal.
    tl.redo();
    const QString afterRedo1 = tagsAt(QStringLiteral("1:0")) + QStringLiteral(" owner=") + clipAt(1, 0).displayName;
    QObject::disconnect(keySpy);
    QObject::disconnect(trackSpy);
    const QString expectRemoved = tagged(32) + QStringLiteral(" owner=c");
    const QString expectRestored = tagged(31) + QStringLiteral(" owner=b | ") + tagged(32) + QStringLiteral(" owner=c");
    const bool ok5 = shared && removed1 && removed2 && tracksAfterRemove2 == 1 && afterRemove1 == expectRemoved
        && afterUndo1 == expectRemoved && afterUndo2 == expectRestored && afterRedo1 == expectRemoved;
    // Step 0: one snapshot's sidecar JSON with these values plus a default 3D text config.
    Text3DLayer defaultText;
    const qsizetype text3DBytes = QJsonDocument(defaultText.toJson()).toJson(QJsonDocument::Compact).size();

    // (6) the four committed on b, saved and opened: they are on b again.
    clearFour();
    load(tl, {{clipFor("x", 10.0)}, {clipFor("a", 2.0), clipFor("b", 2.0, 1.0)}});
    commitAll(bKey, 41);
    QTemporaryDir dir;
    const QString projectPath = dir.filePath(QStringLiteral("g36.veditor"));
    QString saveErr, openErr;
    const bool saved = dir.isValid() && (window.*view.save)(projectPath, &saveErr);
    clearFour();
    const bool opened = saved && (window.*view.open)(projectPath, &openErr, false);
    const QString afterOpen = tagsAt(bKey) + QStringLiteral(" owner=") + clipAt(1, 1).displayName;
    const bool ok6 = opened && afterOpen == tagged(41) + QStringLiteral(" owner=b");

    // (7) the real wiggle dialog on the selected b: accepted as it opens (the inert default, no
    // entry: no undo point), then with the X amplitude set to 5 by the timer first (+1).
    clearFour();
    load(tl, {{clipFor("x", 10.0)}, {clipFor("a", 2.0), clipFor("b", 2.0, 1.0)}});
    QString selectErr;
    const bool selected = tl.selectClipByIndex(false, 1, 1, &selectErr);
    auto runWiggleDialog = [&](double amplitude, int *dialogs, int *others) {
        QTimer driver;
        driver.setInterval(20);
        QObject::connect(&driver, &QTimer::timeout, &window, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal) return;
            auto *dialog = qobject_cast<QDialog *>(modal);
            auto *spin = modal->findChild<QDoubleSpinBox *>(QStringLiteral("wigglePositionAmplitudeX"));
            if (!dialog || !spin) {
                ++*others;
                if (dialog) dialog->reject();
                else modal->close();
                return;
            }
            if (!qFuzzyCompare(1.0 + spin->value(), 1.0 + amplitude)) {
                spin->setValue(amplitude);
                return;
            }
            ++*dialogs;
            dialog->accept();
        });
        driver.start();
        (window.*view.editWiggle)();
        driver.stop();
    };
    int dialogs = 0;
    int others = 0;
    const int d7 = depth();
    runWiggleDialog(0.0, &dialogs, &others);
    const int d7Default = depth();
    const bool defaultNoEntry = !view.wiggle->contains(bKey);
    runWiggleDialog(5.0, &dialogs, &others);
    const int d7Set = depth();
    const bool setEntry = view.wiggle->contains(bKey) && near(view.wiggle->value(bKey).positionAmplitude.x(), 5.0);
    tl.undo();
    const bool undoNoEntry = !view.wiggle->contains(bKey);
    const bool ok7 = selected && dialogs == 2 && others == 0 && d7Default == d7 && defaultNoEntry
        && d7Set == d7 + 1 && setEntry && undoNoEntry && QApplication::activeModalWidget() == nullptr;

    // (8) the four committed on b ("1:0") in this sequence (tag 81), a switch to sequence G36, a
    // wiggle commit there (82), one undo: the four are the ones from before the switch.
    clearFour();
    load(tl, {{clipFor("x", 10.0)}, {clipFor("b", 2.0, 1.0)}});
    const QString b8Key = QStringLiteral("1:0");
    commitAll(b8Key, 81);
    TimelineSequence sequence;
    sequence.id = QStringLiteral("seq-g36");
    sequence.name = QStringLiteral("G36");
    sequence.videoTracks = {{clipFor("y", 4.0)}, {clipFor("z", 2.0)}};
    sequence.audioTracks = {{}};
    const bool added = tl.addSequence(sequence);
    QString seqId;
    for (const TimelineSequence &s : tl.sequences())
        if (s.name == QStringLiteral("G36")) seqId = s.id;
    const QString seqBefore = tl.activeSequenceId();
    const bool switched = !seqId.isEmpty() && tl.setActiveSequence(seqId);
    (window.*view.commitWiggle)(b8Key, wiggleFor(82));
    const QString inOther = tagsAt(b8Key);
    tl.undo();
    const QString afterSeqUndo = tagsAt(b8Key);
    const bool ok8 = added && switched && inOther == QStringLiteral("tr=81 wg=82 t3=81 ex=81")
        && afterSeqUndo == tagged(81);
    if (!seqBefore.isEmpty()) tl.setActiveSequence(seqBefore);

    // (9) the former G32 (6) contract inverted: the four committed on b (91), then written
    // straight into the maps (92, no commit function, no undo point), an unrelated undoable
    // edit (x at speed 2), one undo: the four are the committed ones again.
    clearFour();
    load(tl, {{clipFor("x", 10.0)}, {clipFor("b", 2.0, 1.0)}});
    const QString b9Key = QStringLiteral("1:0");
    commitAll(b9Key, 91);
    const QString committed9 = tagsAt(b9Key);
    const int d9 = depth();
    put(b9Key, 92);
    const int d9Put = depth();
    const QString written9 = tagsAt(b9Key);
    QString speedErr9;
    const bool sped9 = tl.setClipPropertyByIndex(false, 0, 0, QStringLiteral("speed"), 2.0, &speedErr9);
    tl.undo();
    const double speed9 = clipAt(0, 0).speed;
    const QString afterUndo9 = tagsAt(b9Key);
    const bool ok9 = committed9 == tagged(91) && d9Put == d9 && written9 == tagged(92) && sped9 && near(speed9, 1.0)
        && afterUndo9 == tagged(91);

    std::fprintf(stderr, "connected-clip G36 (1) depth %d->%d (expect +1) committed=%d undo gone=%d redo back=%d "
                 "ok=%d\n", d0, d1, committed, undoGone, redoBack, ok1);
    std::fprintf(stderr, "connected-clip G36 (1') same curve again depth %d->%d (expect same) ok=%d\n", d1, d1b, ok1b);
    std::fprintf(stderr, "connected-clip G36 (2) sped=%d [%s] x speed after undo=%.3f (expect 1) entry==clip curve=%d "
                 "%s ok=%d\n", sped, qPrintable(speedErr), clipAt(0, 0).speed,
                 int(entry2.curve.toJson() == b2.timeRemapCurve.toJson()), qPrintable(samples.join(QStringLiteral("; "))),
                 ok2);
    for (const QString &line : std::as_const(kinds))
        std::fprintf(stderr, "connected-clip G36 (3) %s\n", qPrintable(line));
    std::fprintf(stderr, "connected-clip G36 (4) sameOrder=%d before %s | moved=%d [%s] %s | undo %s (expect %s) | "
                 "redo %s (expect the moved one) ok=%d\n", sameOrder, qPrintable(beforeMove), moved,
                 qPrintable(moveErr), qPrintable(afterMove), qPrintable(afterMoveUndo), qPrintable(placed),
                 qPrintable(afterMoveRedo), ok4);
    std::fprintf(stderr, "connected-clip G36 (5) shared=%d remove V2=%d [%s] %s (expect %s) remove V2 again=%d [%s] "
                 "tracks=%d (expect 1) | undo %s (expect %s) | undo %s (expect %s) ok=%d\n", shared, removed1,
                 qPrintable(removeErr1), qPrintable(afterRemove1), qPrintable(expectRemoved), removed2,
                 qPrintable(removeErr2), tracksAfterRemove2, qPrintable(afterUndo1), qPrintable(expectRemoved),
                 qPrintable(afterUndo2), qPrintable(expectRestored), ok5);
    std::fprintf(stderr, "connected-clip G36 step0 restoreState over removeTrackInternal / addVideoTrack: "
                 "clipKeysRemapped=%d trackIndicesRemapped=%d (two undos, adding tracks) %d / %d (then one redo, removing "
                 "one) | redo %s (expect %s) | sidecar JSON %lld bytes (two keys), default Text3DLayer config %lld "
                 "bytes\n", keyRemapsUndo, trackRemapsUndo, keyRemaps - keyRemapsUndo, trackRemaps - trackRemapsUndo,
                 qPrintable(afterRedo1), qPrintable(expectRemoved),
                 static_cast<long long>(sidecarBytes), static_cast<long long>(text3DBytes));
    std::fprintf(stderr, "connected-clip G36 (6) saved=%d [%s] opened=%d [%s] %s (expect %s owner=b) ok=%d\n", saved,
                 qPrintable(saveErr), opened, qPrintable(openErr), qPrintable(afterOpen), qPrintable(tagged(41)), ok6);
    std::fprintf(stderr, "connected-clip G36 (7) selected=%d [%s] dialogs=%d (expect 2) others=%d depth %d->%d "
                 "(default, expect same) noEntry=%d ->%d (amplitude 5, expect +1) entry=%d undo noEntry=%d ok=%d\n",
                 selected, qPrintable(selectErr), dialogs, others, d7, d7Default, defaultNoEntry, d7Set, setEntry,
                 undoNoEntry, ok7);
    std::fprintf(stderr, "connected-clip G36 (8) added=%d switched=%d [%s -> %s] in G36 %s (expect tr=81 wg=82 t3=81 "
                 "ex=81) undo %s (expect %s) ok=%d\n", added, switched, qPrintable(seqBefore), qPrintable(seqId),
                 qPrintable(inOther), qPrintable(afterSeqUndo), qPrintable(tagged(81)), ok8);
    std::fprintf(stderr, "connected-clip G36 (9) committed %s, written directly %s (expect %s) depth %d->%d (expect same) "
                 "| sped=%d [%s] x speed after undo=%.3f (expect 1) undo %s (expect %s, the direct write gone) ok=%d\n",
                 qPrintable(committed9), qPrintable(written9), qPrintable(tagged(92)), d9, d9Put, sped9,
                 qPrintable(speedErr9), speed9, qPrintable(afterUndo9), qPrintable(tagged(91)), ok9);
    tally.gate(36, ok1 && ok1b && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9,
               "a timeRemap / text3D / expression / wiggle commit recorded no undo point (or one for an equal "
               "value), an undo / redo left the four sidecars at their live values instead of the snapshot's "
               "(the remap entry and the clip's curve apart, two same-order tracks swapped, a twice-removed "
               "shared-uid track lost), a save / load moved them, the real wiggle dialog recorded none, or a "
               "direct map write without an undo point survived an unrelated undo");
    clearFour();
}

// G33 (US-1320): one composed clipKeysRemapped for a storyline media drop on V1 whose
// reconcile reorders V2 (avPath: the G29 fixture media).
void composedRemapGate(selftest::GateTally &tally, const QString &avPath, const QString &avName)
{
    // The media dropped on the V1 boundary of [X|Y] with c on V2 connected to Y in front of
    // the unconnected d: the import's V1 shift and the reconcile's V2 reorder (c is pushed
    // past d) leave as one composed clipKeysRemapped.
    Timeline batchTl;
    load(batchTl, {{clipFor("x", 5.0), clipFor("y", 10.0)}, {clipFor("c", 0.5, 5.5), clipFor("d", 0.5, 0.3)}},
         {{clipFor("bgm", 3.0)}, {}});
    const bool batchConnected = batchTl.connectClip(TrackKind::Video, 1, 0, uidAt(batchTl, 0, 1));
    batchTl.setStorylineMode(true);
    batchTl.undoManager()->clear();
    batchTl.saveUndoState(QStringLiteral("baseline"));
    batchTl.takeConnectionWarnings();
    const QString batchCUid = uidAt(batchTl, 1, 0);
    QVector<Timeline::ClipKeyRemap> batchLog;
    QObject::connect(&batchTl, &Timeline::clipKeysRemapped, &batchTl,
                     [&batchLog](const Timeline::ClipKeyRemap &r) { batchLog.append(r); });
    QString batchErr;
    const bool batchDropped = batchTl.dropMediaOnTrack(batchTl.trackAt(false, 0), avPath, 5.0, &batchErr);
    const Located batchC = locate(batchTl, false, batchCUid);
    const double batchLen = locateByName(batchTl, false, avName).clip.effectiveDuration();
    QStringList batchShown;
    bool batchIdentity = false;
    for (const Timeline::ClipKeyRemap &r : std::as_const(batchLog)) {
        QStringList pairs;
        for (auto it = r.oldToNew.cbegin(); it != r.oldToNew.cend(); ++it) {
            pairs.append(it.key() + QStringLiteral(">") + it.value());
            if (it.key() == it.value()) batchIdentity = true;
        }
        pairs.sort();
        QStringList dropped(r.dropped.cbegin(), r.dropped.cend());
        dropped.sort();
        batchShown.append(QStringLiteral("{%1 | dropped %2}").arg(pairs.join(','), dropped.join(',')));
    }
    Timeline::ClipKeyRemap batchExpected;
    batchExpected.oldToNew = {{QStringLiteral("0:1"), QStringLiteral("0:2")},
                              {QStringLiteral("1:0"), QStringLiteral("1:1")},
                              {QStringLiteral("1:1"), QStringLiteral("1:0")}};
    const bool batchOk = batchConnected && batchDropped && batchLen > 0.5 && batchC.track == 1
        && batchC.index == 1 && near(batchC.startSec, 5.5 + batchLen) && batchLog.size() == 1
        && batchLog[0] == batchExpected && !batchIdentity;
    std::fprintf(stderr, "connected-clip G33 connected=%d drop=%d err='%s' len=%.4f c track=%d index=%d "
                 "start=%.4f (expect 1 1 %.4f) %lld emit(s) %s (expect 1 {0:1>0:2,1:0>1:1,1:1>1:0 | dropped }) "
                 "identity=%d ok=%d\n",
                 batchConnected, batchDropped, qPrintable(batchErr), batchLen, batchC.track, batchC.index,
                 batchC.startSec, 5.5 + batchLen, static_cast<long long>(batchLog.size()),
                 qPrintable(batchShown.join(' ')), batchIdentity, batchOk);
    tally.gate(33, batchOk,
               "a storyline media drop on V1 whose reconcile moves a connected V2 clip did not emit "
               "clipKeysRemapped exactly once with the composed V1 shift + V2 reorder (identity-free)");
}

// G34 (US-1320): after a storyline insert at the V1 head moves the selection, the brush
// preview is bound through the rekeyed brush entries: V1 [a 0-5 | b 5-10], b carries a brush.
void brushPreviewRebindGate(selftest::GateTally &tally, MainWindow &window, Timeline &tl,
                            const SidecarView &view)
{
    QTemporaryDir dir;
    const QString avPath = dir.filePath(QStringLiteral("g34.mp4"));
    QString whyNot;
    if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &whyNot)) {
        tally.skip(34, qPrintable(whyNot));
        return;
    }
    if (!dir.isValid() || !writeVideoWithAudioFile(avPath, 20)) {
        tally.gate(34, false, "the G34 fixture media could not be written");
        return;
    }
    auto clearBrushes = [&] {
        view.brush->clear();
        for (BrushAnimation *live : std::as_const(*view.liveBrush))
            if (live) live->deleteLater();
        view.liveBrush->clear();
    };
    struct Run {
        bool dropped = false;
        QString err;
        int selectedAfter = -2;
        QString bound;
        QString brushKeys;
        QString undoBrush; // "key=owner" after Timeline::undo
        QString undoBound;
        QString redoBrush; // "key=owner" after Timeline::redo
        QString redoBound;
    };
    // Each brush entry as "key=name of the V1 clip at that key".
    auto brushOwners = [&] {
        QStringList owners;
        for (const BrushAnimationEntry &entry : std::as_const(*view.brush)) {
            const QStringList parts = entry.clipId.split(QLatin1Char(':'));
            const TimelineTrack *track = parts.size() == 2 ? tl.trackAt(false, parts[0].toInt()) : nullptr;
            const int index = parts.size() == 2 ? parts[1].toInt() : -1;
            const QString owner = track && index >= 0 && index < track->clips().size()
                ? track->clips()[index].displayName : QStringLiteral("?");
            owners.append(entry.clipId + QLatin1Char('=') + owner);
        }
        return owners.join(',');
    };
    // Selects V1 clip `selectIndex` the way a click does, then drops the media at t=0.
    auto run = [&](int selectIndex) {
        Run r;
        clearBrushes();
        load(tl, {{clipFor("a", 5.0), clipFor("b", 5.0)}});
        tl.setStorylineMode(true);
        BrushAnimationEntry brush;
        brush.clipId = QStringLiteral("0:1");
        brush.brushData = QJsonObject{{QStringLiteral("text"), QStringLiteral("G34")}};
        view.brush->append(brush);
        tl.undoManager()->clear();
        tl.saveUndoState(QStringLiteral("G34 baseline"));
        tl.trackAt(false, 0)->setSelectedClip(selectIndex);
        emit tl.trackAt(false, 0)->selectionChanged(selectIndex, false);
        r.dropped = tl.dropMediaOnTrack(tl.trackAt(false, 0), avPath, 0.0, &r.err);
        r.selectedAfter = tl.trackAt(false, 0)->selectedClip();
        r.bound = (window.*view.brushPreviewClipId)();
        QStringList keys;
        for (const BrushAnimationEntry &entry : std::as_const(*view.brush))
            keys.append(entry.clipId);
        r.brushKeys = keys.join(',');
        // The drop's undo point must carry the rekeyed brush (HEAD: the old key "0:1",
        // so the redo puts b's brush on a).
        tl.undo();
        r.undoBrush = brushOwners();
        r.undoBound = (window.*view.brushPreviewClipId)();
        tl.redo();
        r.redoBrush = brushOwners();
        r.redoBound = (window.*view.brushPreviewClipId)();
        return r;
    };
    // (a) a selected, no brush: nothing bound (HEAD: b's brush through its old key "0:1").
    const Run a = run(0);
    const bool okA = a.dropped && a.selectedAfter == 1 && a.brushKeys == QStringLiteral("0:2") && a.bound.isEmpty();
    // (b) b selected with the brush: bound on b's new key (HEAD: cleared at "0:2", never rebound).
    const Run b = run(1);
    const bool okB = b.dropped && b.selectedAfter == 2 && b.brushKeys == QStringLiteral("0:2")
        && b.bound == QStringLiteral("0:2");
    // (c) undo / redo of each drop: the brush stays on b ("0:1" before the drop, "0:2" after,
    // a never owns it); the preview stays unbound with a selected and follows b's key with b selected.
    const bool okC = a.undoBrush == QStringLiteral("0:1=b") && a.undoBound.isEmpty()
        && a.redoBrush == QStringLiteral("0:2=b") && a.redoBound.isEmpty()
        && b.undoBrush == QStringLiteral("0:1=b") && b.undoBound == QStringLiteral("0:1")
        && b.redoBrush == QStringLiteral("0:2=b") && b.redoBound == QStringLiteral("0:2");
    std::fprintf(stderr, "connected-clip G34 (c) (a) undo brush=%s (expect 0:1=b) bound='%s' (expect '') "
                 "redo brush=%s (expect 0:2=b) bound='%s' (expect '') | (b) undo brush=%s (expect 0:1=b) "
                 "bound='%s' (expect '0:1') redo brush=%s (expect 0:2=b) bound='%s' (expect '0:2') ok=%d\n",
                 qPrintable(a.undoBrush), qPrintable(a.undoBound), qPrintable(a.redoBrush), qPrintable(a.redoBound),
                 qPrintable(b.undoBrush), qPrintable(b.undoBound), qPrintable(b.redoBrush), qPrintable(b.redoBound),
                 okC);
    std::fprintf(stderr, "connected-clip G34 (a) a selected: drop=%d err='%s' selected=%d (expect 1) brush=%s "
                 "(expect 0:2) bound='%s' (expect '') ok=%d | (b) b selected: drop=%d err='%s' selected=%d "
                 "(expect 2) brush=%s (expect 0:2) bound='%s' (expect '0:2') ok=%d\n",
                 a.dropped, qPrintable(a.err), a.selectedAfter, qPrintable(a.brushKeys), qPrintable(a.bound), okA,
                 b.dropped, qPrintable(b.err), b.selectedAfter, qPrintable(b.brushKeys), qPrintable(b.bound), okB);
    tally.gate(34, okA && okB && okC,
               "after a storyline insert at the V1 head moved the selection, the brush preview stayed on "
               "another clip's brush (old key) or was not bound to the selected clip's own rekeyed brush, "
               "or the drop's undo / redo moved b's brush onto another clip");
    tl.setStorylineMode(false);
    clearBrushes();
}

// G35 (US-1400): the source monitor's 3-point insert / overwrite rekey the roto, matte, parent and
// timeRemap carriers of the clips they shift (one clipKeysRemapped), and are refused on a locked V1.
void threePointCarrierGate(selftest::GateTally &tally, Timeline &tl, const SidecarView &view,
                           MainWindow &window)
{
    auto place = [&tl](bool overwrite, double at, const ClipInfo &clip) {
        return overwrite ? tl.overwriteClip3PointActive(at, clip) : tl.insertClip3PointActive(at, clip);
    };
    // V1 [a 0-5 | b 5-15], V2 [x connected to b]; roto, matte, parent and timeRemap sit on b "0:1".
    const QString bKey = QStringLiteral("0:1");
    auto setup = [&] {
        view.timeRemap->clear();
        load(tl, {{clipFor("a", 5.0), clipFor("b", 10.0)}, {clipFor("x", 2.0, 7.0)}});
        tl.connectClip(TrackKind::Video, 1, 0, uidAt(tl, 0, 1));
        RotoClipEntry roto;
        roto.brushMask = QImage(2, 2, QImage::Format_ARGB32);
        tl.setRotoEntries({{bKey, roto}});
        TimelineTrackMatteEntry matte;
        matte.matteType = TrackMatteType::AlphaMatte;
        matte.matteSourceClipId = QStringLiteral("1:0");
        tl.setTrackMatteEntries({{bKey, matte}});
        tl.setClipParent(QStringLiteral("1:0"), bKey);
        TimeRemapClipEntry remap;
        remap.clipId = bKey;
        remap.curve.sourceFps = 10.0;
        remap.curve.addKey(0.0, 0.0);
        remap.curve.addKey(2.0, 1.0);
        view.timeRemap->insert(bKey, remap);
        tl.saveUndoState(QStringLiteral("G35 carriers"));
    };
    // Where the four carriers sit: roto / matte keys, the parent's value, the timeRemap key.
    auto where = [&] {
        QStringList parts;
        QStringList roto = tl.rotoEntries().keys();
        roto.sort();
        QStringList matte = tl.trackMatteEntries().keys();
        matte.sort();
        QStringList remap = view.timeRemap->keys();
        remap.sort();
        QString matteSource;
        const auto mattes = tl.trackMatteEntries();
        if (!mattes.isEmpty()) matteSource = mattes.cbegin().value().matteSourceClipId;
        return QStringLiteral("roto=%1 matte=%2(src %3) parent[1:0]=%4 timeRemap=%5(%6)")
            .arg(roto.join(','), matte.join(','), matteSource, tl.clipParentEntries().value(QStringLiteral("1:0")),
                 remap.join(','),
                 view.timeRemap->isEmpty() ? QString() : view.timeRemap->cbegin().value().clipId);
    };
    auto expect = [](const QString &key) {
        return QStringLiteral("roto=%1 matte=%1(src 1:0) parent[1:0]=%1 timeRemap=%1(%1)").arg(key);
    };

    struct Case {
        bool overwrite;
        double at;
        const char *newKey;
        const char *label;
    };
    const Case cases[] = {{false, 0.0, "0:2", "insert"}, {true, 2.5, "0:3", "overwrite"}};
    bool okEdits = true;
    for (const Case &c : cases) {
        setup();
        const QString bUid = uidAt(tl, 0, 1);
        const QString startedAt = where();
        QVector<Timeline::ClipKeyRemap> log;
        const QMetaObject::Connection conn = QObject::connect(
            &tl, &Timeline::clipKeysRemapped, &tl, [&log](const Timeline::ClipKeyRemap &r) { log.append(r); });
        const bool placed = place(c.overwrite, c.at, clipFor("ins", c.overwrite ? 1.0 : 3.0));
        QObject::disconnect(conn);
        const Located b = locate(tl, false, bUid);
        const QString shown = where();
        const bool moved = placed && b.found && b.track == 0 && b.index == QString(c.newKey).mid(2).toInt()
            && uidAt(tl, 1, 0) != bUid && tl.trackAt(false, 1)->clips().at(0).anchorUid == bUid;
        const bool followed = shown == expect(QString::fromLatin1(c.newKey));
        const bool once = log.size() == 1;
        tl.undo();
        const QString undone = where();
        const bool undoOk = undone == expect(bKey) && startedAt == undone
            && uidAt(tl, 0, 1) == bUid && tl.trackAt(false, 1)->clips().at(0).anchorUid == bUid;
        std::fprintf(stderr, "connected-clip G35 (%s) placed=%d b=%d:%d (expect %s) connected=%d "
                     "after=[%s] (expect %s) remaps=%lld (expect 1) undone=[%s] undoOk=%d\n",
                     c.label, placed, b.track, b.index, c.newKey, moved, qPrintable(shown),
                     qPrintable(expect(QString::fromLatin1(c.newKey))), static_cast<long long>(log.size()),
                     qPrintable(undone), undoOk);
        okEdits = okEdits && moved && followed && once && undoOk;
    }

    // (2') One edit, one emit: V1 [a 0-5 | b 5-15], V2 [x 5.5-6.5 connected to b, y 7-8]. The edit shifts b, x
    // follows and V2 reorders to [y, x], so the roto remap and the connected-clip reconcile both rekey in the
    // same edit; the composed map must reach clipKeysRemapped once and carry the V1 and the V2 key change.
    struct ReorderCase {
        bool overwrite;
        bool storyline;
        double at;
        double len;
        int xIndex;
        int xAfter;
        const char *v2Map;
        const char *label;
    };
    // insert: b 5-15 moves to 8-18, x 5.5 -> 8.5 passes y at 7. overwrite keeps every downstream clip at its
    // absolute time (it never moves x, so V2 cannot reorder); it only rekeys b, 0:1 -> 0:3, in the same fixture.
    const ReorderCase reorderCases[] = {
        {false, false, 0.0, 3.0, 0, 1, "1:0->1:1", "insert"},
        {true, false, 2.5, 1.0, 0, 0, "", "overwrite"}};
    bool okReorder = true;
    for (const ReorderCase &c : reorderCases) {
        load(tl, {{clipFor("a", 5.0), clipFor("b", 10.0)}, {clipFor("x", 1.0, 5.5), clipFor("y", 1.0, 0.5)}});
        tl.setStorylineMode(c.storyline);
        tl.connectClip(TrackKind::Video, 1, c.xIndex, uidAt(tl, 0, 1));
        tl.saveUndoState(QStringLiteral("G35 reorder"));
        const QString xUid = uidAt(tl, 1, c.xIndex);
        QVector<Timeline::ClipKeyRemap> log;
        const QMetaObject::Connection conn = QObject::connect(
            &tl, &Timeline::clipKeysRemapped, &tl, [&log](const Timeline::ClipKeyRemap &r) { log.append(r); });
        const bool placed = place(c.overwrite, c.at, clipFor("ins", c.len));
        QObject::disconnect(conn);
        const Located x = locate(tl, false, xUid);
        QStringList mapText;
        for (const auto &r : log) {
            QStringList pairs;
            for (auto it = r.oldToNew.cbegin(); it != r.oldToNew.cend(); ++it)
                pairs.append(it.key() + QStringLiteral("->") + it.value());
            pairs.sort();
            mapText.append(QStringLiteral("{%1 dropped=%2}").arg(pairs.join(','), QString::number(r.dropped.size())));
        }
        const bool reordered = placed && x.found && x.track == 1 && x.index == c.xAfter;
        const bool one = log.size() == 1;
        const QStringList v2Pair = QString::fromLatin1(c.v2Map).split(QStringLiteral("->"));
        const bool v2Key = one && (v2Pair.size() < 2 || log.first().oldToNew.value(v2Pair.value(0)) == v2Pair.value(1));
        const bool v1Key = one && log.first().oldToNew.value(QStringLiteral("0:1"))
            == QString::fromLatin1(c.overwrite ? "0:3" : "0:2");
        std::fprintf(stderr, "connected-clip G35 (2' %s) placed=%d x=%d:%d (expect 1:%d) remaps=%lld (expect 1) "
                     "map=%s v2Key(%s)=%d v1Key=%d\n", c.label, placed, x.track, x.index, c.xAfter,
                     static_cast<long long>(log.size()), qPrintable(mapText.join(" ")), c.v2Map, v2Key, v1Key);
        okReorder = okReorder && reordered && one && v2Key && v1Key;
        tl.setStorylineMode(false);
    }
    okEdits = okEdits && okReorder;

    // (3') A locked V1 refuses both, changing nothing and saying so once each.
    setup();
    tl.trackAt(false, 0)->setLocked(true);
    QStringList messages;
    const QMetaObject::Connection msgConn = QObject::connect(
        &tl, &Timeline::statusMessageRequested, &tl, [&messages](const QString &m, int) { messages.append(m); });
    const int clipsBefore = tl.trackAt(false, 0)->clipCount();
    const int indexBefore = tl.undoManager()->currentIndex();
    const QString carriersBefore = where();
    const bool insertRefused = !place(false, 0.0, clipFor("ins", 3.0));
    const int afterInsertMessages = messages.size();
    const bool overwriteRefused = !place(true, 2.5, clipFor("ins", 1.0));
    QObject::disconnect(msgConn);
    const bool unchanged = tl.trackAt(false, 0)->clipCount() == clipsBefore
        && tl.undoManager()->currentIndex() == indexBefore && where() == carriersBefore
        && tl.trackAt(false, 0)->clips().at(0).displayName == QStringLiteral("a");
    const bool lockText = afterInsertMessages == 1 && messages.size() == 2
        && messages.value(0) == messages.value(1)
        && messages.value(0) == QStringLiteral("ロック中のトラックには配置できません");
    std::fprintf(stderr, "connected-clip G35 (3') locked V1: insertRefused=%d overwriteRefused=%d clips %d->%d "
                 "undoIndex %d->%d unchanged=%d messages=%lld [%s] lockText=%d\n", insertRefused,
                 overwriteRefused, clipsBefore, tl.trackAt(false, 0)->clipCount(), indexBefore,
                 tl.undoManager()->currentIndex(), unchanged, static_cast<long long>(messages.size()),
                 qPrintable(messages.join('|')), lockText);

    // (3'') The same through the MainWindow slots: the status bar keeps the refusal.
    threepoint::SourceSelection sel;
    sel.filePath = QStringLiteral("/selftests/connected-ins.mp4");
    sel.displayName = QStringLiteral("ins");
    sel.durationSec = 3.0;
    sel.sourceInSec = 0.0;
    sel.sourceOutSec = 3.0;
    QStringList slotLines;
    bool slotsOk = true;
    for (int overwrite = 0; overwrite < 2; ++overwrite) {
        window.statusBar()->clearMessage();
        ((window).*(overwrite ? view.sourceOverwrite : view.sourceInsert))(sel);
        const QString shown = window.statusBar()->currentMessage();
        const bool ok = shown == QStringLiteral("ロック中のトラックには配置できません")
            && tl.trackAt(false, 0)->clipCount() == clipsBefore;
        slotLines.append(QStringLiteral("%1 status='%2' clips=%3 ok=%4")
                             .arg(overwrite ? "overwrite" : "insert", shown)
                             .arg(tl.trackAt(false, 0)->clipCount())
                             .arg(ok));
        slotsOk = slotsOk && ok;
    }
    std::fprintf(stderr, "connected-clip G35 (3'') source monitor slots on a locked V1 (clips before %d): %s\n",
                 clipsBefore, qPrintable(slotLines.join(" ; ")));

    tl.trackAt(false, 0)->setLocked(false);

    // (3''') No V1 at all: both entry points refuse with the no-timeline message and change nothing.
    QStringList noV1Messages;
    const QMetaObject::Connection noV1Conn = QObject::connect(
        &tl, &Timeline::statusMessageRequested, [&noV1Messages](const QString &m) { noV1Messages.append(m); });
    const int noV1ClipsBefore = tl.trackAt(false, 0)->clipCount();
    const int noV1IndexBefore = tl.undoManager()->currentIndex();
    const auto noV1CarriersBefore = where();
    QVector<TimelineTrack *> savedVideoTracks;
    std::swap(savedVideoTracks, tl.*member(VideoTracksTag{}));
    const bool noV1Insert = place(false, 0.0, clipFor("ins", 3.0));
    const bool noV1Overwrite = place(true, 2.5, clipFor("ins", 1.0));
    std::swap(savedVideoTracks, tl.*member(VideoTracksTag{}));
    QObject::disconnect(noV1Conn);
    const bool noV1Ok = !noV1Insert && !noV1Overwrite && noV1Messages.size() == 2
        && noV1Messages.value(0) == QStringLiteral("タイムラインがありません")
        && noV1Messages.value(1) == QStringLiteral("タイムラインがありません")
        && tl.trackAt(false, 0)->clipCount() == noV1ClipsBefore
        && tl.undoManager()->currentIndex() == noV1IndexBefore && where() == noV1CarriersBefore;
    std::fprintf(stderr, "connected-clip G35 (no V1) videoTracks=0 insertPlaced=%d overwritePlaced=%d messages=%lld [%s] "
                 "clips %d->%d undoIndex %d->%d noV1Ok=%d\n", noV1Insert, noV1Overwrite,
                 static_cast<long long>(noV1Messages.size()), qPrintable(noV1Messages.join('|')),
                 noV1ClipsBefore, tl.trackAt(false, 0)->clipCount(), noV1IndexBefore,
                 tl.undoManager()->currentIndex(), noV1Ok);

    view.timeRemap->clear();
    tally.gate(35, okEdits && insertRefused && overwriteRefused && unchanged && lockText && slotsOk && noV1Ok,
               "a source-monitor 3-point insert / overwrite left the roto, matte, parent or timeRemap carrier "
               "of the shifted clip on its old key (or emitted clipKeysRemapped other than once, or "
               "undo did not restore all four), or a locked V1 accepted the edit / kept a success message, or a missing V1 did not refuse with タイムラインがありません");
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
            shareUid(tl, 2, 0, x.uid);
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
            shareUid(tl, 2, 0, x.uid);
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
            shareUid(tl, 2, 0, x.uid);
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

    // G12: blade the connected clip (V2 [2,6] connected at offset 2) at 4.
    {
        auto blade = [](bool disableRederive, QString *detail) {
            Timeline tl;
            load(tl, {{clipFor("a", 10.0)}, {clipFor("x", 4.0, 2.0)}, {}});
            const QString anchor = uidAt(tl, 0, 0);
            const QString xUid = uidAt(tl, 1, 0);
            tl.connectClip(TrackKind::Video, 1, 0, anchor);
            tl.setRederiveDisabledForTest(disableRederive);
            QString err;
            const bool split = tl.splitClipByIndex(false, 1, 0, 4.0, &err);
            const Located left = locate(tl, false, xUid);
            const Located right = locateOther(tl, false, QStringLiteral("x"), xUid);
            *detail = QStringLiteral("split=%1 [%2] left[%3] right[%4]").arg(int(split)).arg(err,
                describe(left, anchor), describe(right, anchor));
            return split && left.track == 1 && near(left.startSec, 2.0) && left.clip.anchorUid == anchor
                && near(left.clip.anchorOffsetSec, 2.0) && right.found && right.track == 1
                && near(right.startSec, 4.0) && right.clip.anchorUid == anchor
                && near(right.clip.anchorOffsetSec, 4.0);
        };
        QString detail, controlDetail;
        const bool ok = blade(false, &detail);
        const bool control = blade(true, &controlDetail);
        std::fprintf(stderr, "connected-clip G12 %s (expect left 1/2/offset 2, right 1/4/offset 4, both "
                     "anchored) ok=%d | control (re-derive disabled) %s ok=%d (expect 0)\n",
                     qPrintable(detail), ok, qPrintable(controlDetail), control);
        // Anchor and connected clip cut together (V1 [0,10), V2 [2,8) at offset 2): the
        // connected right piece goes to the V1 piece that now starts at its start.
        auto combined = [](bool ripple, QString *detail) {
            Timeline tl;
            load(tl, {{clipFor("a", 10.0)}, {clipFor("x", 6.0, 2.0)}}, {{clipFor("r", 2.0, 4.0)}});
            const QString anchor = uidAt(tl, 0, 0);
            const QString xUid = uidAt(tl, 1, 0);
            tl.connectClip(TrackKind::Video, 1, 0, anchor);
            double expectStart = 5.0;
            if (ripple) {
                // (ii) all-track ripple of [4,6) through the selected A1 clip.
                tl.trackAt(true, 0)->setSelectedClip(0);
                tl.rippleDeleteSelectedClip();
                expectStart = 4.0;
            } else {
                // (i) blade every track at 5.
                tl.setPlayheadPosition(5.0);
                tl.bladeAllTracksAtPlayhead();
            }
            const QString pieceUid = uidAt(tl, 0, 1);
            const Located right = locateOther(tl, false, QStringLiteral("x"), xUid);
            *detail = QStringLiteral("%1: v1Clips=%2 v1Piece=%3 right[%4] anchorUid=%5 anchorOffsetSec=%6 "
                                     "(expect start %7, anchorUid == v1Piece, offset 0)")
                .arg(ripple ? QStringLiteral("(ii) ripple [4,6)") : QStringLiteral("(i) blade all at 5"))
                .arg(tl.trackAt(false, 0)->clipCount()).arg(pieceUid, describe(right, pieceUid),
                     right.clip.anchorUid).arg(right.clip.anchorOffsetSec, 0, 'f', 6).arg(expectStart);
            return tl.trackAt(false, 0)->clipCount() == 2 && !pieceUid.isEmpty() && pieceUid != anchor
                && right.found && right.track == 1 && near(right.startSec, expectStart)
                && right.clip.anchorUid == pieceUid && near(right.clip.anchorOffsetSec, 0.0);
        };
        QString bladeDetail, rippleDetail;
        const bool bladeOk = combined(false, &bladeDetail);
        const bool rippleOk = combined(true, &rippleDetail);
        std::fprintf(stderr, "connected-clip G12 combined %s ok=%d | %s ok=%d\n",
                     qPrintable(bladeDetail), bladeOk, qPrintable(rippleDetail), rippleOk);
        // One all-track ripple of [2,3) and [5,6) (two selected A clips) through V1 [0,10)
        // and a linked V2+A2 pair [0,10) connected at 0: each cut's remnants go to the
        // V1 piece that starts with them, and deleting one piece takes only its own.
        bool cutsOk = false;
        QString cutsDetail;
        {
            ClipInfo x = clipFor("x", 10.0);
            ClipInfo xa = clipFor("xa", 10.0);
            x.linkGroup = xa.linkGroup = 51;
            Timeline tl;
            load(tl, {{clipFor("a", 10.0)}, {x}},
                 {{clipFor("r1", 1.0, 2.0)}, {xa}, {clipFor("r2", 1.0, 5.0)}});
            const QString anchor = uidAt(tl, 0, 0);
            tl.connectClip(TrackKind::Video, 1, 0, anchor);
            tl.clearSelection();
            tl.trackAt(true, 0)->toggleClipSelection(0);
            tl.trackAt(true, 2)->toggleClipSelection(0);
            tl.rippleDeleteSelectedClip();
            const int v1Pieces = tl.trackAt(false, 0)->clipCount();
            const QString piece2 = uidAt(tl, 0, 1);
            const QString piece4 = uidAt(tl, 0, 2);
            // The x / xa pieces starting at `startSec`, each anchored to `pieceUid` at 0.
            auto at = [&](double startSec, const QString &pieceUid, int *count) {
                bool anchored = true;
                *count = 0;
                for (const bool audio : {false, true}) {
                    const TimelineTrack *track = tl.trackAt(audio, 1);
                    double cursor = 0.0;
                    for (const ClipInfo &clip : track->clips()) {
                        cursor += clip.leadInSec;
                        if (near(cursor, startSec)) {
                            ++*count;
                            anchored = anchored && clip.anchorUid == pieceUid
                                && near(clip.anchorOffsetSec, 0.0);
                        }
                        cursor += clip.effectiveDuration();
                    }
                }
                return anchored && *count == 2;
            };
            int n2 = 0, n4 = 0;
            const bool at2 = at(2.0, piece2, &n2);
            const bool at4 = at(4.0, piece4, &n4);
            QString err;
            const bool deleted = tl.deleteClipByIndex(false, 0, 1, false, &err);
            int gone2 = 0, kept4 = 0;
            at(2.0, piece2, &gone2);
            const bool stay4 = at(4.0, piece4, &kept4);
            cutsDetail = QStringLiteral("v1Pieces=%1 (expect 3) (i) at2=%2 n=%3 (ii) at4=%4 n=%5 (iii) deleted=%6 [%7] "
                                        "left at2=%8 (expect 0) at4 connected=%9 n=%10")
                .arg(v1Pieces).arg(int(at2)).arg(n2).arg(int(at4)).arg(n4)
                .arg(int(deleted)).arg(err).arg(gone2).arg(int(stay4)).arg(kept4);
            cutsOk = v1Pieces == 3 && !piece2.isEmpty() && !piece4.isEmpty() && piece2 != piece4 && at2 && at4
                && deleted && gone2 == 0 && stay4;
        }
        std::fprintf(stderr, "connected-clip G12 two-cut ripple %s ok=%d\n", qPrintable(cutsDetail),
                     cutsOk);
        // An all-track ripple of [1,2) (selected unconnected A3 clip) through V1 [0,10) and a
        // linked V2 [3,8) + A2 [0,10) pair connected at 0: the known V2 clip and the A2 right
        // piece it overlaps go to the right V1 piece together, the A2 left piece stays on the
        // left one, and deleting the left V1 piece leaves the V2 clip.
        bool overlapOk = false;
        QString overlapDetail;
        {
            ClipInfo x = clipFor("x", 5.0, 3.0);
            ClipInfo xa = clipFor("xa", 10.0);
            x.linkGroup = xa.linkGroup = 52;
            Timeline tl;
            load(tl, {{clipFor("a", 10.0)}, {x}}, {{}, {xa}, {clipFor("r", 1.0, 1.0)}});
            const QString anchor = uidAt(tl, 0, 0);
            const QString xUid = uidAt(tl, 1, 0);
            const QString xaUid = tl.trackAt(true, 1)->clips().value(0).uid;
            tl.connectClip(TrackKind::Video, 1, 0, anchor);
            tl.clearSelection();
            tl.trackAt(true, 2)->toggleClipSelection(0);
            tl.rippleDeleteSelectedClip();
            const int v1Pieces = tl.trackAt(false, 0)->clipCount();
            const QString rightPiece = uidAt(tl, 0, 1);
            const Located xNow = locate(tl, false, xUid);
            const Located xaLeft = locate(tl, true, xaUid);
            const Located xaRight = locateOther(tl, true, QStringLiteral("xa"), xaUid);
            const bool together = xNow.found && xNow.track == 1 && near(xNow.startSec, 2.0)
                && xaRight.found && xaRight.track == 1 && near(xaRight.startSec, 1.0)
                && !rightPiece.isEmpty() && rightPiece != anchor
                && xNow.clip.anchorUid == rightPiece && xaRight.clip.anchorUid == rightPiece;
            const bool leftKept = xaLeft.found && xaLeft.track == 1 && near(xaLeft.startSec, 0.0)
                && xaLeft.clip.anchorUid == anchor;
            QString err;
            const bool deleted = tl.deleteClipByIndex(false, 0, 0, false, &err);
            const Located xAfter = locate(tl, false, xUid);
            const Located xaRightAfter = locateOther(tl, true, QStringLiteral("xa"), xaUid);
            const bool xKept = xAfter.found && near(xAfter.startSec, 2.0) && xaRightAfter.found;
            overlapDetail = QStringLiteral("v1Pieces=%1 (expect 2) rightPiece=%2 (i) x[%3] (expect start 2) "
                                           "xaRight[%4] (expect start 1) together=%5 (ii) xaLeft[%6] (expect "
                                           "start 0 on the left piece) kept=%7 (iii) deleted=%8 [%9] x[%10] "
                                           "xaRight found=%11 (expect x at 2, both found)")
                .arg(v1Pieces).arg(rightPiece, describe(xNow, rightPiece), describe(xaRight, rightPiece))
                .arg(int(together)).arg(describe(xaLeft, anchor)).arg(int(leftKept)).arg(int(deleted))
                .arg(err, describe(xAfter, rightPiece)).arg(int(xaRightAfter.found));
            overlapOk = v1Pieces == 2 && together && leftKept && deleted && xKept;
        }
        std::fprintf(stderr, "connected-clip G12 asymmetric overlap %s ok=%d\n", qPrintable(overlapDetail),
                     overlapOk);
        tally.gate(12, ok && !control && bladeOk && rippleOk && cutsOk && overlapOk,
                   "the right piece of a bladed connected clip moved or did not get offset = "
                   "original + left length, the re-derive-disabled control still passed, or a "
                   "connected clip cut together with its anchor left its right piece on the left "
                   "anchor piece, or the remnants of two cuts in one ripple were judged as one "
                   "group, or a linked member and the split piece it overlaps went to different "
                   "anchor pieces");
    }

    // G13: blade the anchor; V1-only ripple through a reversed anchor.
    {
        Timeline tl;
        load(tl, {{clipFor("a", 10.0)}, {clipFor("p", 1.0, 2.0), clipFor("q", 1.0, 3.0)}});
        const QString aUid = uidAt(tl, 0, 0);
        const QString pUid = uidAt(tl, 1, 0);
        const QString qUid = uidAt(tl, 1, 1);
        tl.connectClip(TrackKind::Video, 1, 0, aUid);
        tl.connectClip(TrackKind::Video, 1, 1, aUid);
        QString err;
        const bool split = tl.splitClipByIndex(false, 0, 0, 5.0, &err);
        const QString rightUid = uidAt(tl, 0, 1);
        const Located p = locate(tl, false, pUid);
        const Located q = locate(tl, false, qUid);
        const bool bladeOk = split && !rightUid.isEmpty() && rightUid != aUid
            && near(p.startSec, 2.0) && p.clip.anchorUid == aUid && near(p.clip.anchorOffsetSec, 2.0)
            && near(q.startSec, 6.0) && q.clip.anchorUid == rightUid && near(q.clip.anchorOffsetSec, 1.0);
        std::fprintf(stderr, "connected-clip G13 (1) blade at 5: split=%d [%s] p[%s] (expect left, start 2, "
                     "offset 2) q[%s] onRight=%d (expect start 6, offset 1 on the right piece) ok=%d\n",
                     split, qPrintable(err), qPrintable(describe(p, aUid)), qPrintable(describe(q, rightUid)),
                     q.clip.anchorUid == rightUid, bladeOk);

        // Reversed anchor in 0 / out 10 / speed 1; V2: c35 [3.5,3.8], c65 [6.5,6.8],
        // c8 [8,8.5] connected, u [9,9.5] unconnected. Ripple [6,7] on V1 only.
        ClipInfo reversed = clipFor("rev", 10.0);
        reversed.reversed = true;
        Timeline rt;
        load(rt, {{reversed}, {clipFor("c35", 0.3, 3.5), clipFor("c65", 0.3, 2.7),
                               clipFor("c8", 0.5, 1.2), clipFor("u", 0.5, 0.5)}});
        const QString revUid = uidAt(rt, 0, 0);
        for (int i = 0; i < 3; ++i) rt.connectClip(TrackKind::Video, 1, i, revUid);
        rt.takeConnectionWarnings();
        rt.rippleDeleteTimeRangeActive(6.0, 7.0);
        const QStringList warnings = rt.takeConnectionWarnings();
        const QString revRight = uidAt(rt, 0, 1);
        const Located c35 = locateByName(rt, false, QStringLiteral("c35"));
        const Located c65 = locateByName(rt, false, QStringLiteral("c65"));
        const Located c8 = locateByName(rt, false, QStringLiteral("c8"));
        const Located u = locateByName(rt, false, QStringLiteral("u"));
        const bool leftOk = rt.trackAt(false, 0)->clipCount() == 2 && c35.found
            && near(c35.startSec, 3.5) && c35.clip.anchorUid == revUid && near(c35.clip.anchorOffsetSec, 3.5)
            && c8.found && near(c8.startSec, 7.0) && c8.clip.anchorUid == revRight
            && near(c8.clip.anchorOffsetSec, 1.0) && u.found && near(u.startSec, 9.0)
            && warnings.isEmpty();
        const bool deletedOk = !c65.found;
        std::fprintf(stderr, "connected-clip G13 (2) reversed ripple [6,7]: v1Clips=%d (expect 2) c35[%s] "
                     "(expect start 3.5 offset 3.5 on the left piece) c8[%s] onRight=%d (expect start 7 "
                     "offset 1 on the right piece) u start=%.6f (expect 9) warnings=%lld (expect 0) ok=%d "
                     "(3) c65 found=%d (expect 0, local [6,7) deleted)\n",
                     rt.trackAt(false, 0)->clipCount(), qPrintable(describe(c35, revUid)),
                     qPrintable(describe(c8, revRight)), c8.clip.anchorUid == revRight, u.startSec,
                     static_cast<long long>(warnings.size()), leftOk, c65.found);

        // (4) Remnants of an earlier edit: a V2-only ripple of [4,6) through a linked V2 [0,10)
        // connected at 0 leaves pieces [0,4) / [4,8) that the commit records at offsets 0 / 4.
        // Blading V1 at 3 then routes each by its own point (the [4,8) piece to the right
        // piece at 1), and deleting the left V1 piece takes only the [0,4) piece.
        bool remnantOk = false;
        QString remnantDetail;
        {
            ClipInfo x = clipFor("x", 10.0);
            x.linkGroup = 53;
            Timeline tl;
            load(tl, {{clipFor("a", 10.0)}, {x}});
            const QString anchor = uidAt(tl, 0, 0);
            const QString xUid = uidAt(tl, 1, 0);
            tl.connectClip(TrackKind::Video, 1, 0, anchor);
            QString selectErr;
            const bool selected = tl.selectClipByIndex(false, 1, 0, &selectErr);
            tl.rippleDeleteTimeRangeActive(4.0, 6.0);
            tl.clearSelection();
            const Located left0 = locate(tl, false, xUid);
            const Located right0 = locateOther(tl, false, QStringLiteral("x"), xUid);
            const bool rippled = selected && tl.trackAt(false, 0)->clipCount() == 1
                && left0.found && near(left0.startSec, 0.0) && near(left0.clip.anchorOffsetSec, 0.0)
                && right0.found && near(right0.startSec, 4.0) && near(right0.clip.anchorOffsetSec, 4.0)
                && left0.clip.anchorUid == anchor && right0.clip.anchorUid == anchor
                && left0.clip.linkGroup == 53 && right0.clip.linkGroup == 53;
            QString err;
            const bool cut = tl.splitClipByIndex(false, 0, 0, 3.0, &err);
            const QString rightPiece = uidAt(tl, 0, 1);
            const Located left1 = locate(tl, false, xUid);
            const Located right1 = locateOther(tl, false, QStringLiteral("x"), xUid);
            const bool routed = cut && !rightPiece.isEmpty() && rightPiece != anchor
                && left1.found && near(left1.startSec, 0.0) && left1.clip.anchorUid == anchor
                && near(left1.clip.anchorOffsetSec, 0.0)
                && right1.found && near(right1.startSec, 4.0) && right1.clip.anchorUid == rightPiece
                && near(right1.clip.anchorOffsetSec, 1.0);
            QString delErr;
            const bool deleted = tl.deleteClipByIndex(false, 0, 0, false, &delErr);
            const Located left2 = locate(tl, false, xUid);
            const Located right2 = locateOther(tl, false, QStringLiteral("x"), xUid);
            const bool deleteOk = deleted && !left2.found && right2.found && near(right2.startSec, 4.0)
                && right2.clip.anchorUid == rightPiece && near(right2.clip.anchorOffsetSec, 1.0);
            remnantDetail = QStringLiteral("(i) selected=%1 [%2] ripple left[%3] (expect start 0 offset 0) "
                                           "right[%4] (expect start 4 offset 4) ok=%5 (ii) blade at 3 "
                                           "split=%6 [%7] left[%8] (expect start 0 offset 0 on the left "
                                           "piece) right[%9] (expect start 4 offset 1 on the right piece) "
                                           "ok=%10 (iii) delete left piece=%11 [%12] left found=%13 "
                                           "(expect 0) right[%14] (expect start 4 offset 1 on the right "
                                           "piece) ok=%15")
                .arg(int(selected)).arg(selectErr, describe(left0, anchor), describe(right0, anchor))
                .arg(int(rippled)).arg(int(cut)).arg(err, describe(left1, anchor),
                                                     describe(right1, rightPiece))
                .arg(int(routed)).arg(int(deleted)).arg(delErr).arg(int(left2.found))
                .arg(describe(right2, rightPiece)).arg(int(deleteOk));
            remnantOk = rippled && routed && deleteOk;
        }
        std::fprintf(stderr, "connected-clip G13 (4) earlier-edit remnants %s ok=%d\n",
                     qPrintable(remnantDetail), remnantOk);
        tally.gate(13, bladeOk && leftOk && deletedOk && remnantOk,
                   "an anchor split did not move the connections at / after the cut to the right "
                   "piece, or a middle ripple of a reversed anchor dropped a connection outside "
                   "the span, kept the one inside it or left the one after it behind, or the "
                   "remnants of an earlier edit of a linked clip were routed or deleted by one "
                   "shared point");
    }

    // G14: duplicate / paste / createSequenceFromClip do not copy the connection.
    {
        Timeline tl;
        load(tl, {{clipFor("a", 10.0), clipFor("p", 2.0)}, {clipFor("x", 1.0, 2.0)}});
        const QString aUid = uidAt(tl, 0, 0);
        const QString xUid = uidAt(tl, 1, 0);
        tl.connectClip(TrackKind::Video, 1, 0, aUid);
        tl.takeConnectionWarnings();
        // Duplicate the connected V2 clip.
        for (int t = 0; t < tl.videoTrackCount(); ++t) tl.trackAt(false, t)->setSelectedClip(t == 1 ? 0 : -1);
        for (int t = 0; t < tl.audioTrackCount(); ++t) tl.trackAt(true, t)->setSelectedClip(-1);
        tl.duplicateSelectedClips();
        const Located dup = locateOther(tl, false, QStringLiteral("x"), xUid);
        const Located original = locate(tl, false, xUid);
        const bool dupOk = dup.found && dup.clip.anchorUid.isEmpty() && original.clip.anchorUid == aUid;
        // Paste a clipboard clip that carries a connection (seeded on the V1 source).
        TimelineTrack *v1 = tl.trackAt(false, 0);
        QVector<ClipInfo> v1Clips = v1->clips();
        v1Clips[1].anchorUid = aUid;
        v1Clips[1].anchorOffsetSec = 10.0;
        v1->setClips(v1Clips);
        for (int t = 0; t < tl.videoTrackCount(); ++t) tl.trackAt(false, t)->setSelectedClip(t == 0 ? 1 : -1);
        tl.copySelectedClip();
        tl.pasteClip();
        int pastedVideo = 0, pastedAnchored = 0, pastedAudio = 0;
        for (const bool audio : {false, true}) {
            const auto &tracks = audio ? tl.audioTracks() : tl.videoTracks();
            for (const TimelineTrack *track : tracks) {
                for (const ClipInfo &clip : track->clips()) {
                    if (clip.displayName != QStringLiteral("p")) continue;
                    (audio ? pastedAudio : pastedVideo) += 1;
                    pastedAnchored += !clip.anchorUid.isEmpty();
                }
            }
        }
        const Located afterPaste = locate(tl, false, xUid);
        const bool pasteOk = pastedVideo == 2 && pastedAudio == 1 && pastedAnchored == 0
            && afterPaste.clip.anchorUid == aUid;
        const QStringList pasteWarnings = tl.takeConnectionWarnings();
        // A new sequence from the connected clip (real media for the probe).
        ClipInfo media = clipFor("media", 1.0, 2.0);
        media.filePath = QFileInfo(QStringLiteral("test_assets/e2e_clip.mp4")).absoluteFilePath();
        Timeline st;
        load(st, {{clipFor("a", 10.0)}, {media}});
        const QString sAnchor = uidAt(st, 0, 0);
        st.connectClip(TrackKind::Video, 1, 0, sAnchor);
        st.takeConnectionWarnings();
        const ClipInfo source = st.trackAt(false, 1)->clips().at(0);
        const QString mainId = st.activeSequenceId().isEmpty() ? QStringLiteral("main") : st.activeSequenceId();
        const bool created = st.createSequenceFromClip(source);
        const QStringList seqWarnings = st.takeConnectionWarnings();
        int newClips = 0, newAnchored = 0;
        for (const bool audio : {false, true})
            for (const TimelineTrack *track : audio ? st.audioTracks() : st.videoTracks())
                for (const ClipInfo &clip : track->clips()) {
                    ++newClips;
                    newAnchored += !clip.anchorUid.isEmpty();
                }
        bool sourceKept = false;
        for (const TimelineSequence &sequence : st.sequences())
            if (sequence.id != st.activeSequenceId() && sequence.videoTracks.size() > 1
                && !sequence.videoTracks[1].isEmpty())
                sourceKept = sequence.videoTracks[1][0].anchorUid == sAnchor;
        const bool sequenceOk = created && newClips >= 1 && newAnchored == 0 && seqWarnings.isEmpty()
            && sourceKept;
        std::fprintf(stderr, "connected-clip G14 duplicate found=%d anchorUid=[%s] (expect empty) original "
                     "anchored=%d ok=%d | paste video=%d audio=%d anchored=%d (expect 2/1/0) original "
                     "anchored=%d warnings=%lld ok=%d | createSequenceFromClip created=%d mainId=%s "
                     "clips=%d anchored=%d (expect 0) warnings=%lld (expect 0) sourceKept=%d ok=%d\n",
                     dup.found, qPrintable(dup.clip.anchorUid), original.clip.anchorUid == aUid, dupOk,
                     pastedVideo, pastedAudio, pastedAnchored, afterPaste.clip.anchorUid == aUid,
                     static_cast<long long>(pasteWarnings.size()), pasteOk, created, qPrintable(mainId),
                     newClips, newAnchored, static_cast<long long>(seqWarnings.size()), sourceKept,
                     sequenceOk);
        tally.gate(14, dupOk && pasteOk && sequenceOk,
                   "a duplicate / paste / new sequence copied the source's connection, or the "
                   "original lost it");
    }

    // G15: bake / decompose a connected V2+A2 group, then the V1 anchor.
    {
        QTemporaryDir dir;
        const QString mediaPath = QFileInfo(QStringLiteral("test_assets/e2e_clip.mp4")).absoluteFilePath();
        const auto mediaUs = libavcore::probeDurationMicroseconds(mediaPath.toStdString());
        const QString tonePath = dir.filePath(QStringLiteral("g15-tone.wav"));
        const bool ready = dir.isValid() && mediaUs && *mediaUs >= 3000000
            && writeWav(tonePath, {{4.0, true}});
        bool ok = false;
        if (!ready) {
            std::fprintf(stderr, "connected-clip G15 setup failed: dir=%d media=%s duration=%lld\n",
                         dir.isValid(), qPrintable(mediaPath),
                         static_cast<long long>(mediaUs ? *mediaUs : -1));
        } else {
            auto media = [&](const QString &name, double in, double out, double leadIn) {
                ClipInfo clip = clipFor(name, out - in, leadIn);
                clip.filePath = mediaPath;
                clip.duration = double(*mediaUs) / 1000000.0;
                clip.inPoint = in;
                clip.outPoint = out;
                return clip;
            };
            ClipInfo x = media(QStringLiteral("x"), 1.0, 2.0, 0.5);
            x.linkGroup = 71;
            ClipInfo xa = clipFor(QStringLiteral("xa"), 1.0, 0.5);
            xa.filePath = tonePath;
            xa.duration = 4.0;
            xa.inPoint = 1.0;
            xa.outPoint = 2.0;
            xa.linkGroup = 71;
            Timeline tl;
            load(tl, {{media(QStringLiteral("a"), 0.0, 1.5, 0.0), media(QStringLiteral("b"), 1.0, 2.0, 0.0)}, {x}},
                 {{}, {xa}});
            const QString aUid = uidAt(tl, 0, 0);
            const QString bUid = uidAt(tl, 0, 1);
            const QString xUid = uidAt(tl, 1, 0);
            const QString xaUid = tl.trackAt(true, 1)->clips().at(0).uid;
            tl.connectClip(TrackKind::Video, 1, 0, aUid);
            tl.takeConnectionWarnings();
            auto groupState = [&](const QString &anchor) {
                const Located v = locate(tl, false, xUid);
                const Located a = locate(tl, true, xaUid);
                return std::make_pair(v.found && a.found && near(v.startSec, 0.5) && near(a.startSec, 0.5)
                                          && v.clip.anchorUid == anchor && a.clip.anchorUid == anchor,
                                      QStringLiteral("x[%1] baked=%2 xa[%3] baked=%4")
                                          .arg(describe(v, anchor)).arg(int(bool(v.clip.renderInPlaceOriginal)))
                                          .arg(describe(a, anchor)).arg(int(bool(a.clip.renderInPlaceOriginal))));
            };
            renderinplace::Options options;
            options.outputDir = dir.path();
            options.outputSize = QSize(640, 360);
            options.fps = 30;
            QString path, error;
            const bool baked = renderinplace::renderClipInPlace(tl, 1, 0, options, &path, &error);
            const auto afterBake = groupState(aUid);
            const bool bakedFlags = locate(tl, false, xUid).clip.renderInPlaceOriginal
                && locate(tl, true, xaUid).clip.renderInPlaceOriginal;
            const bool decomposed = renderinplace::decomposeRenderInPlace(tl, 1, 0);
            const auto afterDecompose = groupState(aUid);
            // Back to the baked state, reconnect to B, decompose: B must win.
            tl.undo();
            const Located bakedAgain = locate(tl, false, xUid);
            const bool reconnected = tl.disconnectClip(TrackKind::Video, 1, bakedAgain.index)
                && tl.connectClip(TrackKind::Video, 1, bakedAgain.index, bUid);
            const bool decomposedAgain = renderinplace::decomposeRenderInPlace(tl, 1, locate(tl, false, xUid).index);
            const auto afterReconnect = groupState(bUid);
            const double reconnectOffset = locate(tl, false, xUid).clip.anchorOffsetSec;
            // Bake the V1 anchor B (in 1.0) with handles: its source mapping changes.
            renderinplace::Options handles = options;
            handles.handlesSec = 0.25;
            QString anchorPath, anchorError;
            tl.takeConnectionWarnings();
            const bool anchorBaked = renderinplace::renderClipInPlace(tl, 0, 1, handles, &anchorPath, &anchorError);
            const ClipInfo bakedB = tl.trackAt(false, 0)->clips().value(1);
            const auto afterAnchorBake = groupState(bUid);
            const int anchorWarnings = tl.takeConnectionWarnings().size();
            ok = baked && bakedFlags && afterBake.first && decomposed && afterDecompose.first
                && reconnected && decomposedAgain && afterReconnect.first && near(reconnectOffset, -1.0)
                && anchorBaked && bakedB.uid == bUid && bool(bakedB.renderInPlaceOriginal)
                && afterAnchorBake.first && anchorWarnings == 0;
            std::fprintf(stderr, "connected-clip G15 bake=%d [%s] flags=%d %s | decompose=%d %s | undo + "
                         "reconnect B=%d decompose=%d %s offset=%.6f (expect -1) | anchor bake=%d [%s] "
                         "uidKept=%d in=%.6f %s warnings=%d (expect 0) ok=%d\n", baked, qPrintable(error),
                         bakedFlags, qPrintable(afterBake.second), decomposed,
                         qPrintable(afterDecompose.second), reconnected, decomposedAgain,
                         qPrintable(afterReconnect.second), reconnectOffset, anchorBaked,
                         qPrintable(anchorError), bakedB.uid == bUid, bakedB.inPoint,
                         qPrintable(afterAnchorBake.second), anchorWarnings, ok);
        }
        tally.gate(15, ok, "bake / decompose lost a connection or its uid, the decompose restored the "
                           "bake-time anchor, or baking the anchor moved its connection");
    }

    // G16: explicit deletes of the anchor delete the connected V2+A2 group.
    {
        auto groupFixture = [](Timeline &tl) {
            ClipInfo x = clipFor("x", 1.0, 2.0);
            ClipInfo xa = clipFor("xa", 1.0, 2.0);
            x.linkGroup = xa.linkGroup = 31;
            load(tl, {{clipFor("a", 10.0)}, {x, clipFor("u", 1.0, 2.0)}}, {{}, {xa}});
            tl.connectClip(TrackKind::Video, 1, 0, uidAt(tl, 0, 0));
            tl.takeConnectionWarnings();
        };
        auto groupGone = [](const Timeline &tl, const char *label, bool done) {
            const Located x = locateByName(tl, false, QStringLiteral("x"));
            const Located xa = locateByName(tl, true, QStringLiteral("xa"));
            const Located u = locateByName(tl, false, QStringLiteral("u"));
            const bool ok = done && !x.found && !xa.found && u.found && near(u.startSec, 5.0)
                && u.clip.anchorUid.isEmpty() && tl.trackAt(false, 0)->clipCount() <= 1;
            std::fprintf(stderr, "connected-clip G16 %s done=%d x=%d xa=%d (expect 0 0) u found=%d "
                         "start=%.6f (expect 5) v1Clips=%d ok=%d\n", label, done, x.found, xa.found,
                         u.found, u.startSec, tl.trackAt(false, 0)->clipCount(), ok);
            return ok;
        };
        bool mcpOk = false;
        if (mainTimeline) {
            groupFixture(*mainTimeline);
            QString err;
            const QJsonObject deleted = callTool(registry, QStringLiteral("delete_clip"),
                QJsonObject{{"trackIndex", 0}, {"clipIndex", 0}}, &err);
            mcpOk = groupGone(*mainTimeline, "delete_clip", deleted.value("ok").toBool() && err.isEmpty());
        }
        Timeline ow;
        groupFixture(ow);
        ow.overwriteClip3PointActive(0.0, clipFor("cover", 10.0));
        const bool overwriteOk = groupGone(ow, "overwrite cover", locateByName(ow, false, QStringLiteral("cover")).found);
        Timeline rd;
        groupFixture(rd);
        rd.rippleDeleteTimeRangeActive(0.0, 10.0);
        const bool rippleOk = groupGone(rd, "ripple range", rd.trackAt(false, 0)->clipCount() == 0);
        // R1: connections outside the deleted span stay, detached with one warning.
        auto outside = [](double anchorLeadIn, double connectionStart, double from, double to,
                          const char *label) {
            Timeline tl;
            ClipInfo w = clipFor("w", 0.5, connectionStart);
            load(tl, {{clipFor("a", 6.0, anchorLeadIn)}, {w}});
            tl.connectClip(TrackKind::Video, 1, 0, uidAt(tl, 0, 0));
            const double offset = tl.trackAt(false, 1)->clips().at(0).anchorOffsetSec;
            tl.takeConnectionWarnings();
            tl.rippleDeleteTimeRangeActive(from, to);
            const QStringList warnings = tl.takeConnectionWarnings();
            const Located wl = locateByName(tl, false, QStringLiteral("w"));
            const bool ok = wl.found && near(wl.startSec, connectionStart) && wl.clip.anchorUid.isEmpty()
                && warnings.size() == 1;
            std::fprintf(stderr, "connected-clip G16 R1%s anchor local [%.1f,%.1f] deleted, offset=%.6f: "
                         "w found=%d start=%.6f anchorUid=[%s] (expect empty) warnings=%lld (expect 1) "
                         "ok=%d\n", label, from - anchorLeadIn, to - anchorLeadIn, offset, wl.found,
                         wl.startSec, qPrintable(wl.clip.anchorUid),
                         static_cast<long long>(warnings.size()), ok);
            return ok;
        };
        const bool r1a = outside(0.0, 8.0, 3.0, 6.0, "(a)");
        const bool r1b = outside(1.0, 0.0, 1.0, 4.0, "(b)");
        tally.gate(16, mcpOk && overwriteOk && rippleOk && r1a && r1b,
                   "an explicit delete of the anchor kept its connected group or deleted an "
                   "unconnected clip, or a connection outside the deleted span was deleted");
    }

    // G17: simulated MultiCam (V1 replaced through setClips, no undo point).
    {
        Timeline tl;
        load(tl, {{clipFor("a", 10.0)}, {clipFor("x", 1.0, 2.0)}});
        const QString aUid = uidAt(tl, 0, 0);
        const QString xUid = uidAt(tl, 1, 0);
        tl.connectClip(TrackKind::Video, 1, 0, aUid);
        tl.takeConnectionWarnings();
        tl.trackAt(false, 0)->setClips({clipFor("multicam", 10.0)});
        const bool newUid = uidAt(tl, 0, 0) != aUid;
        tl.saveUndoState(QStringLiteral("unrelated"));
        const QStringList warnings = tl.takeConnectionWarnings();
        const Located x = locate(tl, false, xUid);
        std::fprintf(stderr, "connected-clip G17 newUid=%d x found=%d start=%.6f (expect 2) anchorUid=[%s] "
                     "(expect empty) warnings=%lld (expect 1) [%s]\n", newUid, x.found, x.startSec,
                     qPrintable(x.clip.anchorUid), static_cast<long long>(warnings.size()),
                     qPrintable(warnings.join('|')));
        tally.gate(17, newUid && x.found && near(x.startSec, 2.0) && x.clip.anchorUid.isEmpty()
                       && warnings.size() == 1,
                   "a V1 replaced without an undo point deleted the connected clip or detached it "
                   "without exactly one warning");
    }

    // G19: a linked V2+A2 pair connects and follows as a group.
    {
        ClipInfo x = clipFor("x", 1.0, 3.0);
        ClipInfo xa = clipFor("xa", 1.0, 3.0);
        x.linkGroup = xa.linkGroup = 41;
        Timeline tl;
        load(tl, {{clipFor("a", 10.0)}, {x}}, {{}, {xa, clipFor("n", 1.0, 2.0)}});
        const QString aUid = uidAt(tl, 0, 0);
        tl.connectClip(TrackKind::Video, 1, 0, aUid);
        const Located xc = locateByName(tl, false, QStringLiteral("x"));
        const Located ac = locateByName(tl, true, QStringLiteral("xa"));
        const bool bothConnected = xc.clip.anchorUid == aUid && ac.clip.anchorUid == aUid;
        tl.insertClip3PointActive(0.0, clipFor("ins", 2.0));
        const Located xl = locateByName(tl, false, QStringLiteral("x"));
        const Located al = locateByName(tl, true, QStringLiteral("xa"));
        const Located nl = locateByName(tl, true, QStringLiteral("n"));
        std::fprintf(stderr, "connected-clip G19 connected x=%d xa=%d | after V1 insert x[%s] xa[%s] (expect "
                     "start 5) n start=%.6f anchorUid=[%s] (expect 6, empty)\n", xc.clip.anchorUid == aUid,
                     ac.clip.anchorUid == aUid, qPrintable(describe(xl, aUid)),
                     qPrintable(describe(al, aUid)), nl.startSec, qPrintable(nl.clip.anchorUid));
        // A grouped V2 [1.25,2.5) + A2 [3.75,5) pair that does not overlap: a V1 split at
        // 2.5 lies between the members, and the group still goes to one piece.
        ClipInfo y = clipFor("y", 1.25, 1.25);
        ClipInfo ya = clipFor("ya", 1.25, 3.75);
        y.linkGroup = ya.linkGroup = 42;
        Timeline gt;
        load(gt, {{clipFor("a", 5.0)}, {y}}, {{}, {ya}});
        const QString leftUid = uidAt(gt, 0, 0);
        gt.connectClip(TrackKind::Video, 1, 0, leftUid);
        const Located yc = locateByName(gt, false, QStringLiteral("y"));
        const Located yac = locateByName(gt, true, QStringLiteral("ya"));
        const bool pairConnected = yc.clip.anchorUid == leftUid && yac.clip.anchorUid == leftUid;
        QString selectErr;
        const bool selected = gt.selectClipByIndex(false, 0, 0, &selectErr);
        gt.setPlayheadPosition(2.5);
        gt.splitAtPlayhead();
        gt.clearSelection();
        const QString rightUid = uidAt(gt, 0, 1);
        const int v1Clips = gt.trackAt(false, 0)->clipCount();
        const Located ys = locateByName(gt, false, QStringLiteral("y"));
        const Located yas = locateByName(gt, true, QStringLiteral("ya"));
        const bool splitOk = selected && v1Clips == 2 && !rightUid.isEmpty()
            && rightUid != leftUid && ys.found && yas.found && near(ys.startSec, 1.25)
            && near(yas.startSec, 3.75) && ys.clip.anchorUid == leftUid && yas.clip.anchorUid == leftUid;
        QString deleteErr;
        const bool deleted = gt.deleteClipByIndex(false, 0, 0, false, &deleteErr);
        const Located yd = locateByName(gt, false, QStringLiteral("y"));
        const Located yad = locateByName(gt, true, QStringLiteral("ya"));
        const bool deleteOk = deleted && !yd.found && !yad.found;
        std::fprintf(stderr, "connected-clip G19 non-overlapping pair: connected y=%d ya=%d | V1 split at 2.5 "
                     "selected=%d [%s] v1Clips=%d left=%s right=%s y[%s] ya[%s] (expect start 1.25 / 3.75, "
                     "both on the left piece) ok=%d | delete left piece=%d [%s] y found=%d ya found=%d "
                     "(expect 0 0) ok=%d\n", yc.clip.anchorUid == leftUid, yac.clip.anchorUid == leftUid,
                     selected, qPrintable(selectErr), v1Clips, qPrintable(leftUid),
                     qPrintable(rightUid), qPrintable(describe(ys, leftUid)), qPrintable(describe(yas, leftUid)),
                     splitOk, deleted, qPrintable(deleteErr), yd.found, yad.found, deleteOk);
        tally.gate(19, bothConnected && near(xl.startSec, 5.0) && near(al.startSec, 5.0)
                       && xl.clip.anchorUid == aUid && al.clip.anchorUid == aUid && nl.found
                       && near(nl.startSec, 6.0) && nl.clip.anchorUid.isEmpty()
                       && pairConnected && splitOk && deleteOk,
                   "a linked pair was not connected / moved as a group, an unlinked A2 clip moved, or a "
                   "V1 split between the members of a linked pair sent them to different pieces");
    }

    // G20: track operations, locks, anchor cross-track drop.
    {
        auto trackOp = [](int op, bool connected, QString *err, bool *unchanged) {
            Timeline tl;
            load(tl, {{clipFor("a", 10.0)}, {clipFor("x", 1.0, 2.0)}, {clipFor("z", 1.0)}});
            if (connected) tl.connectClip(TrackKind::Video, 1, 0, uidAt(tl, 0, 0));
            const quint64 serial = tl.undoManager()->saveSerial();
            bool ok = false;
            if (op == 0) ok = tl.removeTrack(false, 0, err);
            else if (op == 1) ok = tl.moveTrack(false, 0, 2, err);
            else ok = tl.moveTrack(false, 1, 0, err);
            *unchanged = tl.videoTrackCount() == 3
                && tl.trackAt(false, 0)->clips().value(0).displayName == QStringLiteral("a")
                && tl.trackAt(false, 1)->clips().value(0).displayName == QStringLiteral("x")
                && tl.undoManager()->saveSerial() == serial;
            return ok;
        };
        bool opsOk = true;
        const char *names[] = {"removeTrack(V1)", "moveTrack(V1->V3)", "moveTrack(V2->V1)"};
        for (int op = 0; op < 3; ++op) {
            QString err, controlErr;
            bool unchanged = false, controlUnchanged = false;
            const bool done = trackOp(op, true, &err, &unchanged);
            const bool controlDone = trackOp(op, false, &controlErr, &controlUnchanged);
            const bool ok = !done && unchanged && !err.isEmpty() && controlDone;
            std::fprintf(stderr, "connected-clip G20 %s connected: done=%d (expect 0) unchanged=%d err=[%s] | "
                         "unconnected control: done=%d (expect 1) ok=%d\n", names[op], done, unchanged,
                         qPrintable(err), controlDone, ok);
            opsOk = opsOk && ok;
        }
        auto lockedCase = [](bool deleteAnchor) {
            Timeline tl;
            load(tl, {{clipFor("a", 10.0)}, {clipFor("x", 1.0, 2.0)}});
            const QString xUid = uidAt(tl, 1, 0);
            tl.connectClip(TrackKind::Video, 1, 0, uidAt(tl, 0, 0));
            tl.trackAt(false, 1)->setLocked(true);
            tl.takeConnectionWarnings();
            QString err;
            bool done = true;
            if (deleteAnchor) done = tl.deleteClipByIndex(false, 0, 0, false, &err);
            else tl.insertClip3PointActive(0.0, clipFor("ins", 2.0));
            const QStringList warnings = tl.takeConnectionWarnings();
            const Located x = locate(tl, false, xUid);
            const bool ok = done && x.found && near(x.startSec, 2.0) && x.clip.anchorUid.isEmpty()
                && warnings.size() == 1;
            std::fprintf(stderr, "connected-clip G20 locked V2, %s: done=%d [%s] x found=%d start=%.6f "
                         "(expect 2) anchorUid=[%s] (expect empty) warnings=%lld (expect 1) ok=%d\n",
                         deleteAnchor ? "anchor deleted" : "V1 insert", done, qPrintable(err), x.found,
                         x.startSec, qPrintable(x.clip.anchorUid), static_cast<long long>(warnings.size()), ok);
            return ok;
        };
        const bool lockInsertOk = lockedCase(false);
        const bool lockDeleteOk = lockedCase(true);
        // The anchor dropped onto V2 leaves V1: the linked group detaches.
        ClipInfo x = clipFor("x", 1.0, 2.0);
        ClipInfo xa = clipFor("xa", 1.0, 2.0);
        x.linkGroup = xa.linkGroup = 61;
        Timeline tl;
        load(tl, {{clipFor("a", 10.0)}, {x}}, {{}, {xa}});
        const QString aUid = uidAt(tl, 0, 0);
        tl.connectClip(TrackKind::Video, 1, 0, aUid);
        tl.takeConnectionWarnings();
        Timeline::MoveClipResult result;
        QString err;
        const bool dropped = tl.moveClipByIndex(false, 0, 0, 20.0, 1, &result, &err);
        const QStringList warnings = tl.takeConnectionWarnings();
        const Located al = locate(tl, false, aUid);
        const Located xl = locateByName(tl, false, QStringLiteral("x"));
        const Located xal = locateByName(tl, true, QStringLiteral("xa"));
        const bool dropOk = dropped && al.track == 1 && xl.found && xal.found && near(xl.startSec, 2.0)
            && near(xal.startSec, 2.0) && xl.clip.anchorUid.isEmpty() && xal.clip.anchorUid.isEmpty()
            && warnings.size() == 1;
        std::fprintf(stderr, "connected-clip G20 anchor drop onto V2: done=%d [%s] anchorTrack=%d x[%s] "
                     "xa[%s] (expect start 2, detached) warnings=%lld (expect 1) ok=%d\n", dropped,
                     qPrintable(err), al.track, qPrintable(describe(xl, aUid)),
                     qPrintable(describe(xal, aUid)), static_cast<long long>(warnings.size()), dropOk);
        tally.gate(20, opsOk && lockInsertOk && lockDeleteOk && dropOk,
                   "a V1 track removal / move was accepted with connections (or rejected without), "
                   "a locked connection moved / was deleted, or an anchor leaving V1 kept its group");
    }

    // G21: insertAudioClipAtPlayhead splits a connected A2 clip.
    {
        QTemporaryDir dir;
        const QString voPath = dir.filePath(QStringLiteral("g21-vo.wav"));
        const bool ready = dir.isValid() && writeWav(voPath, {{1.0, true}});
        auto insert = [&](bool disableRederive, QString *detail) {
            Timeline tl;
            load(tl, {{clipFor("a", 10.0)}}, {{}, {clipFor("c", 4.0, 1.0)}});
            const QString aUid = uidAt(tl, 0, 0);
            const QString cUid = tl.trackAt(true, 1)->clips().at(0).uid;
            tl.connectClip(TrackKind::Audio, 1, 0, aUid);
            tl.setRederiveDisabledForTest(disableRederive);
            tl.setPlayheadPosition(3.0);
            tl.insertAudioClipAtPlayhead(voPath, 2);
            const Located left = locate(tl, true, cUid);
            const Located right = locateOther(tl, true, QStringLiteral("c"), cUid);
            const Located vo = locateByName(tl, true, QFileInfo(voPath).fileName());
            const double voLength = vo.clip.effectiveDuration();
            *detail = QStringLiteral("left[%1] vo start=%2 length=%3 anchored=%4 right[%5]")
                .arg(describe(left, aUid)).arg(vo.startSec, 0, 'f', 6).arg(voLength, 0, 'f', 6)
                .arg(int(!vo.clip.anchorUid.isEmpty())).arg(describe(right, aUid));
            return vo.found && voLength > 0.9 && voLength < 1.1 && vo.clip.anchorUid.isEmpty()
                && left.track == 1 && near(left.startSec, 1.0) && left.clip.anchorUid == aUid
                && near(left.clip.anchorOffsetSec, 1.0) && right.found && right.track == 1
                && near(right.startSec, 3.0 + voLength) && right.clip.anchorUid == aUid
                && near(right.clip.anchorOffsetSec, 3.0 + voLength);
        };
        QString detail, controlDetail;
        const bool ok = ready && insert(false, &detail);
        const bool control = ready && insert(true, &controlDetail);
        std::fprintf(stderr, "connected-clip G21 ready=%d %s (expect left 1/1/offset 1, right on A2 at 3 + vo "
                     "with offset 3 + vo, both anchored) ok=%d | control (re-derive disabled) %s ok=%d "
                     "(expect 0)\n", ready, qPrintable(detail), ok, qPrintable(controlDetail), control);
        tally.gate(21, ok && !control,
                   "a voice-over insert that split a connected audio clip moved or disconnected a "
                   "piece, or the re-derive-disabled control still passed");
    }

    // G22: silence cut of a V1 anchor whose audio splits into 3 pieces.
    {
        QTemporaryDir dir;
        const QString wavPath = dir.filePath(QStringLiteral("g22-silences.wav"));
        const bool ready = dir.isValid()
            && writeWav(wavPath, {{1.0, true}, {1.0, false}, {1.0, true}, {1.0, false}, {1.0, true}});
        bool ok = false;
        if (!ready) {
            std::fprintf(stderr, "connected-clip G22 setup failed\n");
        } else {
            ClipInfo anchor = clipFor("s", 5.0);
            anchor.filePath = wavPath;
            Timeline tl;
            load(tl, {{anchor}, {clipFor("c1", 0.2, 0.5), clipFor("cg", 0.2, 0.8), clipFor("c3", 0.2, 2.5)}});
            const QString sUid = uidAt(tl, 0, 0);
            for (int i = 0; i < 3; ++i) tl.connectClip(TrackKind::Video, 1, i, sUid);
            tl.takeConnectionWarnings();
            const QStringList boxes = answeringMessageBoxes(&tl, [&tl] {
                tl.applySilenceCutToClip(tl.trackAt(false, 0), 0);
            });
            const QStringList warnings = tl.takeConnectionWarnings();
            const TimelineTrack *v1 = tl.trackAt(false, 0);
            const int pieces = v1->clipCount();
            const Located piece3 = pieces == 3 ? locate(tl, false, v1->clips().at(2).uid) : Located{};
            const double piece3Local = piece3.clip.inPoint;   // anchor in 0, speed 1
            const Located c1 = locateByName(tl, false, QStringLiteral("c1"));
            const Located cg = locateByName(tl, false, QStringLiteral("cg"));
            const Located c3 = locateByName(tl, false, QStringLiteral("c3"));
            const bool sane = pieces == 3 && piece3Local > 3.5 && piece3Local < 4.1
                && piece3.startSec > 1.9 && piece3.startSec < 2.7 && v1->clips().at(0).uid == sUid;
            ok = sane && c1.found && near(c1.startSec, 0.5) && c1.clip.anchorUid == sUid
                && near(c1.clip.anchorOffsetSec, 0.5) && cg.found && near(cg.startSec, 1.5)
                && cg.clip.anchorUid.isEmpty() && warnings.size() == 1 && c3.found
                && c3.clip.anchorUid == piece3.clip.uid
                && near(c3.startSec, piece3.startSec + (4.2 - piece3Local))
                && near(c3.clip.anchorOffsetSec, 4.2 - piece3Local);
            std::fprintf(stderr, "connected-clip G22 boxes=%lld [%s] pieces=%d (expect 3) piece3 start=%.6f "
                         "local=%.6f sane=%d | c1[%s] (expect 0.5 on piece 1) cg start=%.6f anchorUid=[%s] "
                         "(expect 1.5, empty) warnings=%lld (expect 1) [%s] | c3[%s] (expect start %.6f "
                         "offset %.6f on piece 3) ok=%d\n", static_cast<long long>(boxes.size()),
                         qPrintable(boxes.join('|')), pieces, piece3.startSec, piece3Local, sane,
                         qPrintable(describe(c1, sUid)), cg.startSec, qPrintable(cg.clip.anchorUid),
                         static_cast<long long>(warnings.size()), qPrintable(warnings.join('|')),
                         qPrintable(describe(c3, piece3.clip.uid)), piece3.startSec + (4.2 - piece3Local),
                         4.2 - piece3Local, ok);
        }
        tally.gate(22, ok, "a silence cut moved the piece-1 connection, deleted / kept connected the one "
                           "in a removed span, or left the piece-3 one off the packed piece 3");
    }

    // G18: pre-compose (MainWindow::precomposeSelectionWithName) across the
    // anchor and a connected clip.
    if (!mainTimeline) {
        tally.gate(18, false, "MainWindow has no mainTimeline");
    } else {
        load(*mainTimeline, {{clipFor("a", 10.0)}, {clipFor("x", 6.0, 2.0), clipFor("y", 0.5, 1.0)},
                             {clipFor("s", 2.0, 4.0)}});
        const QString aUid = uidAt(*mainTimeline, 0, 0);
        const QString xUid = uidAt(*mainTimeline, 1, 0);
        mainTimeline->connectClip(TrackKind::Video, 1, 0, aUid);
        mainTimeline->connectClip(TrackKind::Video, 1, 1, aUid);
        mainTimeline->takeConnectionWarnings();
        for (int t = 0; t < mainTimeline->videoTrackCount(); ++t)
            mainTimeline->trackAt(false, t)->setSelectedClip(t == 2 ? 0 : -1);
        for (int t = 0; t < mainTimeline->audioTrackCount(); ++t)
            mainTimeline->trackAt(true, t)->setSelectedClip(-1);
        const QString parentId = mainTimeline->activeSequenceId();
        const MainWindow::PrecomposeResult result = window.precomposeSelectionWithName(QStringLiteral("G18"));
        const QStringList parentWarnings = mainTimeline->takeConnectionWarnings();
        const Located left = locate(*mainTimeline, false, aUid);
        const Located y = locateByName(*mainTimeline, false, QStringLiteral("y"));
        const Located xLeft = locate(*mainTimeline, false, xUid);
        const bool parentOk = result.success && left.found && left.track == 0 && near(left.startSec, 0.0)
            && near(left.clip.effectiveDuration(), 4.0) && y.found && near(y.startSec, 9.0)
            && y.clip.anchorUid == aUid && near(y.clip.anchorOffsetSec, 9.0) && xLeft.found
            && near(xLeft.startSec, 2.0) && xLeft.clip.anchorUid == aUid && parentWarnings.isEmpty();
        bool nestedOk = false;
        QString nestedDetail;
        const QString activeParent = mainTimeline->activeSequenceId();
        if (result.success && mainTimeline->setActiveSequence(result.sequenceId)) {
            mainTimeline->saveUndoState(QStringLiteral("nested edit"));
            const QStringList nestedWarnings = mainTimeline->takeConnectionWarnings();
            const TimelineTrack *nv1 = mainTimeline->trackAt(false, 0);
            const TimelineTrack *nv2 = mainTimeline->trackAt(false, 1);
            const ClipInfo v1Piece = nv1 && nv1->clipCount() == 1 ? nv1->clips().at(0) : ClipInfo{};
            const ClipInfo v2Piece = nv2 && nv2->clipCount() == 1 ? nv2->clips().at(0) : ClipInfo{};
            nestedOk = !v1Piece.uid.isEmpty() && v2Piece.anchorUid == v1Piece.uid
                && near(v2Piece.leadInSec, 0.0) && near(v2Piece.anchorOffsetSec, 0.0)
                && nestedWarnings.isEmpty();
            nestedDetail = QStringLiteral("v1Clips=%1 v2Clips=%2 v2 anchorUid=%3 (expect the nested V1 uid "
                                          "%4) offset=%5 warnings=%6")
                .arg(nv1 ? nv1->clipCount() : -1).arg(nv2 ? nv2->clipCount() : -1)
                .arg(v2Piece.anchorUid, v1Piece.uid).arg(v2Piece.anchorOffsetSec, 0, 'f', 6)
                .arg(nestedWarnings.size());
            mainTimeline->setActiveSequence(activeParent);
        }
        std::fprintf(stderr, "connected-clip G18 precompose success=%d [%s] parentId=[%s] left remainder "
                     "start=%.6f length=%.6f (expect 0 / 4) y[%s] (expect start 9 offset 9 on the left "
                     "remainder) xLeft[%s] parentWarnings=%lld (expect 0) ok=%d | nested %s ok=%d\n",
                     result.success, qPrintable(result.failureReason), qPrintable(parentId),
                     left.startSec, left.clip.effectiveDuration(), qPrintable(describe(y, aUid)),
                     qPrintable(describe(xLeft, aUid)), static_cast<long long>(parentWarnings.size()),
                     parentOk, qPrintable(nestedDetail), nestedOk);
        tally.gate(18, parentOk && nestedOk,
                   "pre-compose moved / detached the connection after the range, warned in the "
                   "parent, or the nested range piece was not connected to the nested V1 piece");
    }

    // G23-G25: the connect / disconnect actions on the MainWindow timeline.
    QAction *connectAction = window.findChild<QAction *>(QStringLiteral("action_connect_to_storyline"));
    QAction *disconnectAction = window.findChild<QAction *>(QStringLiteral("action_disconnect_from_storyline"));
    const QString connectText = QStringLiteral("主ストーリーラインに接続");
    const QString disconnectText = QStringLiteral("接続を解除");
    if (!mainTimeline || !connectAction || !disconnectAction) {
        std::fprintf(stderr, "connected-clip G23-G25 missing: timeline=%d connect=%d disconnect=%d\n",
                     mainTimeline != nullptr, connectAction != nullptr, disconnectAction != nullptr);
        tally.gate(23, false, "the connect action is not registered on the MainWindow");
        tally.gate(24, false, "the disconnect action is not registered on the MainWindow");
        tally.gate(25, false, "the connect action is not registered on the MainWindow");
    } else {
        UndoManager *undo = mainTimeline->undoManager();
        // G23
        load(*mainTimeline, {{clipFor("a", 5.0), clipFor("b", 10.0)}, {clipFor("x", 2.0, 7.0)}});
        const QString bUid = uidAt(*mainTimeline, 0, 1);
        const QString xUid = uidAt(*mainTimeline, 1, 0);
        selectOnly(*mainTimeline, false, 1, 0);
        const quint64 serial0 = undo->saveSerial();
        connectAction->trigger();
        const Located x1 = locate(*mainTimeline, false, xUid);
        const quint64 serial1 = undo->saveSerial();
        const QString undoText = undo->undoDescription();
        // Past the V1 end (15 s): the preceding V1 clip b is the anchor.
        load(*mainTimeline, {{clipFor("a", 5.0), clipFor("b", 10.0)}, {clipFor("y", 2.0, 20.0)}});
        const QString b2Uid = uidAt(*mainTimeline, 0, 1);
        const QString yUid = uidAt(*mainTimeline, 1, 0);
        selectOnly(*mainTimeline, false, 1, 0);
        connectAction->trigger();
        const Located y1 = locate(*mainTimeline, false, yUid);
        const bool overlapOk = x1.found && x1.clip.anchorUid == bUid && near(x1.clip.anchorOffsetSec, 2.0)
            && near(x1.startSec, 7.0) && serial1 == serial0 + 1;
        const bool textOk = undoText == connectText && connectAction->text() == connectText
            && disconnectAction->text() == disconnectText;
        const bool precedingOk = y1.found && y1.clip.anchorUid == b2Uid
            && near(y1.clip.anchorOffsetSec, 15.0) && near(y1.startSec, 20.0);
        std::fprintf(stderr, "connected-clip G23 overlap: %s serial %llu -> %llu (expect +1) undo=[%s] "
                     "action=[%s] ok=%d textOk=%d | preceding: %s ok=%d\n",
                     qPrintable(describe(x1, bUid)), static_cast<unsigned long long>(serial0),
                     static_cast<unsigned long long>(serial1), qPrintable(undoText),
                     qPrintable(connectAction->text()), overlapOk, textOk,
                     qPrintable(describe(y1, b2Uid)), precedingOk);
        tally.gate(23, overlapOk && textOk && precedingOk,
                   "timeline.connect_to_storyline did not connect the V2 clip to the overlapping (or "
                   "preceding) V1 clip in one undo point named after the action text");

        // G24: disconnect, undo, redo (back on the first fixture).
        load(*mainTimeline, {{clipFor("a", 5.0), clipFor("b", 10.0)}, {clipFor("x", 2.0, 7.0)}});
        const QString b3Uid = uidAt(*mainTimeline, 0, 1);
        const QString x3Uid = uidAt(*mainTimeline, 1, 0);
        selectOnly(*mainTimeline, false, 1, 0);
        connectAction->trigger();
        const quint64 serialA = undo->saveSerial();
        disconnectAction->trigger();
        const Located detached = locate(*mainTimeline, false, x3Uid);
        const quint64 serialB = undo->saveSerial();
        const QString disconnectUndoText = undo->undoDescription();
        mainTimeline->undo();
        const Located restored = locate(*mainTimeline, false, x3Uid);
        mainTimeline->redo();
        const Located redone = locate(*mainTimeline, false, x3Uid);
        // Control: disconnecting a clip that is not connected is rejected.
        selectOnly(*mainTimeline, false, 1, 0);
        const quint64 serialC = undo->saveSerial();
        disconnectAction->trigger();
        const quint64 serialD = undo->saveSerial();
        const QString rejectMessage = window.statusBar()->currentMessage();
        const bool g24 = detached.found && detached.clip.anchorUid.isEmpty() && near(detached.startSec, 7.0)
            && serialB == serialA + 1 && disconnectUndoText == disconnectText
            && restored.clip.anchorUid == b3Uid && near(restored.clip.anchorOffsetSec, 2.0)
            && redone.clip.anchorUid.isEmpty() && serialD == serialC
            && rejectMessage == QStringLiteral("このクリップは接続されていません");
        std::fprintf(stderr, "connected-clip G24 detached: %s serial %llu -> %llu (expect +1) undo=[%s] | "
                     "undo: %s (expect anchored, offset 2) | redo: %s (expect detached) | control: serial "
                     "%llu -> %llu (expect same) status=[%s] ok=%d\n", qPrintable(describe(detached, b3Uid)),
                     static_cast<unsigned long long>(serialA), static_cast<unsigned long long>(serialB),
                     qPrintable(disconnectUndoText), qPrintable(describe(restored, b3Uid)),
                     qPrintable(describe(redone, b3Uid)), static_cast<unsigned long long>(serialC),
                     static_cast<unsigned long long>(serialD), qPrintable(rejectMessage), g24);
        tally.gate(24, g24,
                   "timeline.disconnect did not detach in one undo point, undo / redo did not toggle the "
                   "connection, or disconnecting an unconnected clip was not rejected");

        // G25: negative controls, each rejected unchanged with the reason in the status bar.
        auto rejectCase = [&](const QVector<QVector<ClipInfo>> &video, int track, int index,
                              const QString &reason, QString *detail) {
            load(*mainTimeline, video);
            selectOnly(*mainTimeline, false, track, index);
            const quint64 before = undo->saveSerial();
            window.statusBar()->clearMessage();
            connectAction->trigger();
            const QString message = window.statusBar()->currentMessage();
            bool anyAnchor = false;
            for (const TimelineTrack *t : mainTimeline->videoTracks())
                for (const ClipInfo &c : t->clips()) anyAnchor = anyAnchor || !c.anchorUid.isEmpty();
            *detail = QStringLiteral("serial %1 -> %2 anchored=%3 status=[%4]")
                .arg(before).arg(undo->saveSerial()).arg(anyAnchor).arg(message);
            return undo->saveSerial() == before && !anyAnchor && message == reason;
        };
        QString v1Detail, noAnchorDetail;
        const bool v1Rejected = rejectCase({{clipFor("a", 5.0), clipFor("b", 10.0)}, {clipFor("x", 2.0, 7.0)}},
                                           0, 1, QStringLiteral("V1 のクリップは接続できません"), &v1Detail);
        const bool noAnchorRejected = rejectCase({{clipFor("a", 3.0, 5.0)}, {clipFor("x", 2.0, 1.0)}},
                                                 1, 0, QStringLiteral("接続先の V1 クリップがありません"),
                                                 &noAnchorDetail);
        std::fprintf(stderr, "connected-clip G25 V1 clip: %s ok=%d | V2 clip with no V1 clip under or before "
                     "it: %s ok=%d\n", qPrintable(v1Detail), v1Rejected, qPrintable(noAnchorDetail),
                     noAnchorRejected);
        tally.gate(25, v1Rejected && noAnchorRejected,
                   "connecting a V1 clip, or a clip with no V1 clip under or before it, was not rejected "
                   "with its reason in the status bar and nothing unchanged");
    }

    // G26: storyline-mode drops connect (media file entry: a text/uri-list QDropEvent sent to
    // TimelineTrack::dropEvent; clip entry: the cross-track x-vse-clip drop).
    {
        QTemporaryDir dir;
        const QString wavPath = dir.filePath(QStringLiteral("g26.wav"));
        const QString videoPath = dir.filePath(QStringLiteral("g26.mp4"));
        QString whyNot;
        if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &whyNot)) {
            tally.skip(26, qPrintable(whyNot));
        } else if (!dir.isValid() || !writeWav(wavPath, {{1.0, true}}) || !writeVideoFile(videoPath, 20)) {
            std::fprintf(stderr, "connected-clip G26 fixture media could not be written\n");
            tally.gate(26, false, "the G26 fixture media could not be written");
        } else {
            auto build = [&](Timeline &tl) {
                load(tl, {{clipFor("a", 5.0), clipFor("b", 10.0)}, {}, {clipFor("y", 2.0, 7.0)}}, {{}, {}});
                tl.takeConnectionWarnings();
            };
            const QString videoName = QFileInfo(videoPath).fileName();
            const QString wavName = QFileInfo(wavPath).fileName();
            // Mode ON: media drops on V2 (t=7, inside b) and A2 (t=6), clip drop V3 -> V2 at 12.
            Timeline on;
            build(on);
            const QString bUid = uidAt(on, 0, 1);
            on.setStorylineMode(true);
            const bool v2Accepted = sendMediaDrop(on.trackAt(false, 1), videoPath, 7.0);
            const bool a2Accepted = sendMediaDrop(on.trackAt(true, 1), wavPath, 6.0);
            const Located v2Clip = locateByName(on, false, videoName);
            const Located a2Clip = locateByName(on, true, wavName);
            const bool clipAccepted = sendClipDropOnTrack(on.trackAt(false, 2), 0, on.trackAt(false, 1), 12.0);
            const Located movedOn = locateByName(on, false, QStringLiteral("y"));
            // US-1304: a drop in the middle of V1's first clip (t=3 in a [0,5)) snaps to the
            // boundary at 5 and pushes b by the new clip's length.
            const int v1CountBefore = on.trackAt(false, 0)->clipCount();
            const bool v1Accepted = sendMediaDrop(on.trackAt(false, 0), videoPath, 3.0);
            const Located v1Clip = locateByName(on, false, videoName);
            const Located bPushed = locateByName(on, false, QStringLiteral("b"));
            const bool v1Ok = v1CountBefore == 2 && v1Accepted && v1Clip.found && v1Clip.track == 0
                && v1Clip.clip.effectiveDuration() > 0.5 && near(v1Clip.startSec, 5.0)
                && on.trackAt(false, 0)->clipCount() == 3 && bPushed.found && bPushed.track == 0
                && near(bPushed.startSec, 5.0 + v1Clip.clip.effectiveDuration());
            const bool onOk = v2Accepted && a2Accepted && v1Ok && v2Clip.found && v2Clip.track == 1
                && near(v2Clip.startSec, 7.0) && v2Clip.clip.anchorUid == bUid
                && near(v2Clip.clip.anchorOffsetSec, 2.0) && a2Clip.found && a2Clip.track == 1
                && near(a2Clip.startSec, 6.0) && a2Clip.clip.anchorUid == bUid
                && near(a2Clip.clip.anchorOffsetSec, 1.0) && clipAccepted && movedOn.found && movedOn.track == 1 && near(movedOn.startSec, 12.0)
                && movedOn.clip.anchorUid == bUid && near(movedOn.clip.anchorOffsetSec, 7.0);
            // Mode OFF: the media drop is refused (MainWindow handles it as before), the clip
            // drop moves but connects nothing.
            Timeline off;
            build(off);
            off.setStorylineMode(false);
            const int v2Before = off.trackAt(false, 1)->clipCount();
            const int a2Before = off.trackAt(true, 1)->clipCount();
            const bool offV2Accepted = sendMediaDrop(off.trackAt(false, 1), videoPath, 7.0);
            const bool offA2Accepted = sendMediaDrop(off.trackAt(true, 1), wavPath, 6.0);
            const bool offClipAccepted = sendClipDropOnTrack(off.trackAt(false, 2), 0, off.trackAt(false, 1), 12.0);
            const bool offV1Accepted = sendMediaDrop(off.trackAt(false, 0), videoPath, 3.0);
            const Located movedOff = locateByName(off, false, QStringLiteral("y"));
            const bool offOk = !offV2Accepted && !offA2Accepted && !offV1Accepted
                && off.trackAt(false, 0)->clipCount() == 2
                && off.trackAt(false, 1)->clipCount() == v2Before + 1 && off.trackAt(true, 1)->clipCount() == a2Before
                && offClipAccepted && movedOff.found && movedOff.track == 1 && near(movedOff.startSec, 12.0)
                && movedOff.clip.anchorUid.isEmpty();
            // Sub-checks (review fixes): a two-URL drop and a .veditor drop are refused and leave the
            // track unchanged; a V2 drop past V1's end (t=20, V1 ends at 15) imports unconnected;
            // with A2 (the linked audio partner of V2) locked, a video-only drop on V2 is accepted
            // and leaves A2 untouched, while a drop that carries audio is refused; and the
            // press-and-hold of a V1 move drag settles its repaints (no overlay repaint loop).
            Timeline sub;
            build(sub);
            sub.setStorylineMode(true);
            const int subV2Before = sub.trackAt(false, 1)->clipCount();
            const bool multiAccepted = sendMediaDropUrls(sub.trackAt(false, 1), {videoPath, videoPath}, 7.0);
            // The .veditor name holds a real decodable file, so only the suffix filter refuses it.
            const QString projectPath = dir.filePath(QStringLiteral("g26.veditor"));
            QFile::remove(projectPath);
            const bool projectCopied = QFile::copy(videoPath, projectPath);
            const bool projectAccepted = sendMediaDrop(sub.trackAt(false, 1), projectPath, 7.0);
            const int subV2AfterRefused = sub.trackAt(false, 1)->clipCount();
            const bool gapAccepted = sendMediaDrop(sub.trackAt(false, 1), videoPath, 20.0);
            const Located gapClip = locateByName(sub, false, videoName);
            const bool gapOk = gapAccepted && gapClip.found && gapClip.track == 1 && near(gapClip.startSec, 20.0)
                && gapClip.clip.anchorUid.isEmpty();
            Timeline lockedTl;
            build(lockedTl);
            lockedTl.setStorylineMode(true);
            lockedTl.trackAt(true, 1)->setLocked(true);
            const int lockV2Before = lockedTl.trackAt(false, 1)->clipCount();
            const int lockA2Before = lockedTl.trackAt(true, 1)->clipCount();
            const bool lockAccepted = sendMediaDrop(lockedTl.trackAt(false, 1), videoPath, 7.0);
            const int lockV2After = lockedTl.trackAt(false, 1)->clipCount();
            const int lockA2After = lockedTl.trackAt(true, 1)->clipCount();
            // Control: a drop that carries audio (the wav) on V2 is refused while A2 is locked,
            // and accepted (A2 grows) once A2 is unlocked.
            const bool lockAudioAccepted = sendMediaDrop(lockedTl.trackAt(false, 1), wavPath, 8.0);
            const int lockCtlV2 = lockedTl.trackAt(false, 1)->clipCount();
            const int lockCtlA2 = lockedTl.trackAt(true, 1)->clipCount();
            lockedTl.trackAt(true, 1)->setLocked(false);
            const bool unlockedAudioAccepted = sendMediaDrop(lockedTl.trackAt(false, 1), wavPath, 8.0);
            const int unlockedA2 = lockedTl.trackAt(true, 1)->clipCount();
            // Repaint settle: hold a V1 move drag (press + small move, no release) on a shown timeline.
            Timeline paintTl;
            build(paintTl);
            paintTl.resize(900, 400);
            paintTl.show();
            TimelineTrack *paintV1 = paintTl.trackAt(false, 0);
            auto idleFor200ms = [] {
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < 200) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    QThread::msleep(5);
                }
            };
            // Precondition: the shown track has painted at least once before the drag starts.
            for (int i = 0; i < 10 && paintV1->paintCountForTest() == 0; ++i) idleFor200ms();
            const quint64 paintBase = paintV1->paintCountForTest();
            const QPointF pressPos(2.5 * paintV1->pixelsPerSecond(), paintV1->height() / 2.0);
            QMouseEvent press(QEvent::MouseButtonPress, pressPos, pressPos, Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(paintV1, &press);
            const QPointF movePos = pressPos + QPointF(6.0, 0.0);
            QMouseEvent move(QEvent::MouseMove, movePos, movePos, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(paintV1, &move);
            idleFor200ms();  // settle the press / move repaints
            idleFor200ms();
            const quint64 paintAfterFirst = paintV1->paintCountForTest();
            idleFor200ms();
            const quint64 paintAfterSecond = paintV1->paintCountForTest();
            QMouseEvent release(QEvent::MouseButtonRelease, movePos, movePos, Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QApplication::sendEvent(paintV1, &release);
            const bool paintOk = paintBase > 0
                && paintAfterSecond == paintAfterFirst;
            const bool lockOk = lockAccepted && lockV2After == lockV2Before + 1 && lockA2After == lockA2Before
                && !lockAudioAccepted && lockCtlV2 == lockV2After && lockCtlA2 == lockA2After
                && unlockedAudioAccepted && unlockedA2 > lockCtlA2;
            const bool subOk = projectCopied && !multiAccepted && !projectAccepted
                && subV2AfterRefused == subV2Before && gapOk && lockOk && paintOk;
            std::fprintf(stderr, "connected-clip G26 sub: multiUrlDrop=%d veditorDrop=%d (copied=%d; expect 0/0) V2 clips "
                         "%d -> %d (expect unchanged) gapDrop=%d gap[%s] anchorUid='%s' (expect empty) "
                         "lockedA2: videoDrop=%d V2 %d -> %d A2 %d -> %d (expect 1, V2 +1, A2 unchanged) "
                         "audioDrop=%d (expect 0) V2 %d A2 %d (expect unchanged) unlockedAudioDrop=%d A2 %d "
                         "(expect 1, A2 grows) lockOk=%d | paint counts base=%llu afterIdle1=%llu "
                         "afterIdle2=%llu (expect idle2 == idle1 > 0) paintOk=%d | ok=%d\n",
                         multiAccepted, projectAccepted, projectCopied, subV2Before, subV2AfterRefused,
                         gapAccepted, qPrintable(describe(gapClip, QString())),
                         qPrintable(gapClip.clip.anchorUid), lockAccepted, lockV2Before, lockV2After,
                         lockA2Before, lockA2After, lockAudioAccepted, lockCtlV2, lockCtlA2,
                         unlockedAudioAccepted, unlockedA2, lockOk, static_cast<unsigned long long>(paintBase),
                         static_cast<unsigned long long>(paintAfterFirst),
                         static_cast<unsigned long long>(paintAfterSecond), paintOk, subOk);
            std::fprintf(stderr, "connected-clip G26 ON: v2Drop=%d a2Drop=%d v1Drop=%d (expect 1/1/1) v2[%s] "
                         "a2[%s] clipDrop=%d y[%s] v1New[%s] bPushed[%s] v1Ok=%d ok=%d | OFF: v2Drop=%d a2Drop=%d "
                         "v1Drop=%d (expect 0/0/0) V2 clips "
                         "%d -> %d (expect +1 from the clip drop only) A2 clips %d -> %d clipDrop=%d y[%s] "
                         "ok=%d\n", v2Accepted, a2Accepted, v1Accepted, qPrintable(describe(v2Clip, bUid)),
                         qPrintable(describe(a2Clip, bUid)), clipAccepted, qPrintable(describe(movedOn, bUid)),
                         qPrintable(describe(v1Clip, bUid)), qPrintable(describe(bPushed, bUid)), v1Ok, onOk,
                         offV2Accepted, offA2Accepted, offV1Accepted, v2Before, off.trackAt(false, 1)->clipCount(),
                         a2Before, off.trackAt(true, 1)->clipCount(), offClipAccepted,
                         qPrintable(describe(movedOff, bUid)), offOk);
            tally.gate(26, onOk && offOk && subOk,
                       "a storyline-mode drop did not connect to the V1 clip under it, or a V1 media drop did not "
                       "snap to the boundary and push the next clip, or the mode-off "
                       "media drop (on V2 / A2 / V1) was taken / the mode-off clip drop connected / a multi-file or project drop was "
                       "taken, a gap drop connected, or a drop landed audio on a locked partner track / a video-only drop was refused by it, or a held V1 move drag kept repainting");
        }
    }

    // G27: the real MultiCam apply path (MainWindow::onMultiCamApplyToTimeline) with its
    // replace-V1 confirmation closed by a periodic timer (no production seam).
    if (!mainTimeline) {
        tally.gate(27, false, "MainWindow has no mainTimeline");
    } else {
        QTemporaryDir angleDir;
        const QString anglePath = angleDir.filePath(QStringLiteral("angle.png"));
        QImage(8, 8, QImage::Format_RGB888).save(anglePath);
        MultiCamProject project;
        project.angles.append({1, anglePath, 0, QStringLiteral("Cam 1")});
        project.switches.append({0, 1});
        project.defaultAngleId = 1;
        const auto apply = [&](const MultiCamProject &p) {
            (window.*member(MultiCamApplyTag{}))(p);
        };

        // (a) V1 holds a clip: the question is asked and answered Yes; the connected V2 clip
        // stays where it was and detaches with one warning on the next commit (as in G17).
        load(*mainTimeline, {{clipFor("a", 10.0)}, {clipFor("x", 1.0, 2.0)}});
        const QString aUid = uidAt(*mainTimeline, 0, 0);
        const QString xUid = uidAt(*mainTimeline, 1, 0);
        mainTimeline->connectClip(TrackKind::Video, 1, 0, aUid);
        mainTimeline->takeConnectionWarnings();
        int answered = 0, unexpected = 0;
        QStringList seen;
        {
            ModalDriver driver(&window);
            driver.start();
            apply(project);
            answered = driver.answered;
            unexpected = driver.unexpected;
            seen = driver.seen;
        }
        const bool v1Replaced = mainTimeline->trackAt(false, 0)->clipCount() == 1
            && uidAt(*mainTimeline, 0, 0) != aUid
            && mainTimeline->trackAt(false, 0)->clips().at(0).filePath == anglePath;
        mainTimeline->saveUndoState(QStringLiteral("unrelated"));
        const QStringList warnings = mainTimeline->takeConnectionWarnings();
        const Located x = locate(*mainTimeline, false, xUid);
        const bool noModalA = QApplication::activeModalWidget() == nullptr;
        const bool mainOk = answered == 1 && unexpected == 0 && seen.size() == 1 && v1Replaced
            && x.found && near(x.startSec, 2.0) && x.clip.anchorUid.isEmpty() && warnings.size() == 1;

        // (b) V1 empty: no question is asked, the apply still lands.
        load(*mainTimeline, {{}, {clipFor("x", 1.0, 2.0)}});
        int askedEmpty = -1;
        {
            ModalDriver driver(&window);
            driver.start();
            apply(project);
            askedEmpty = driver.seen.size() + driver.unexpected;
        }
        const bool emptyOk = askedEmpty == 0 && mainTimeline->trackAt(false, 0)->clipCount() == 1;

        // (c) control: a dialog without a Yes button is rejected and counted, not left open.
        int strayUnexpected = 0;
        {
            ModalDriver driver(&window);
            driver.start();
            QMessageBox::information(&window, QStringLiteral("G27"), QStringLiteral("no Yes button"));
            strayUnexpected = driver.unexpected;
        }
        const bool strayOk = strayUnexpected == 1;
        const bool noModal = noModalA && QApplication::activeModalWidget() == nullptr;
        std::fprintf(stderr, "connected-clip G27 answered=%d unexpected=%d questions=%lld (expect 1/0/1) v1Replaced=%d "
                     "x found=%d start=%.6f (expect 2) anchorUid=[%s] (expect empty) warnings=%lld (expect 1) | "
                     "empty V1 asked=%d (expect 0) ok=%d | stray dialog unexpected=%d (expect 1) | noModal=%d\n",
                     answered, unexpected, static_cast<long long>(seen.size()), v1Replaced, x.found, x.startSec,
                     qPrintable(x.clip.anchorUid), static_cast<long long>(warnings.size()), askedEmpty, emptyOk,
                     strayUnexpected, noModal);
        tally.gate(27, mainOk && emptyOk && strayOk && noModal,
                   "the real MultiCam apply path did not ask / apply after Yes, kept the connection, "
                   "asked with an empty V1, left a modal open, or a dialog without a Yes button was not rejected");
    }

    // G28: the real pre-compose menu action (QInputDialog closed by a periodic timer) with the
    // G18 fixture and expectations plus the nested sequence name.
    QAction *precomposeAction = nullptr;
    for (QAction *action : window.findChildren<QAction *>())
        if (action->text() == QStringLiteral("選択をプリコンポーズ...")) {
            precomposeAction = action;
            break;
        }
    if (!mainTimeline || !precomposeAction) {
        std::fprintf(stderr, "connected-clip G28 missing: timeline=%d action=%d\n", mainTimeline != nullptr,
                     precomposeAction != nullptr);
        tally.gate(28, false, "the pre-compose menu action is not registered on the MainWindow");
    } else {
        load(*mainTimeline, {{clipFor("a", 10.0)}, {clipFor("x", 6.0, 2.0), clipFor("y", 0.5, 1.0)},
                             {clipFor("s", 2.0, 4.0)}});
        const QString aUid = uidAt(*mainTimeline, 0, 0);
        const QString xUid = uidAt(*mainTimeline, 1, 0);
        mainTimeline->connectClip(TrackKind::Video, 1, 0, aUid);
        mainTimeline->connectClip(TrackKind::Video, 1, 1, aUid);
        mainTimeline->takeConnectionWarnings();
        for (int t = 0; t < mainTimeline->videoTrackCount(); ++t)
            mainTimeline->trackAt(false, t)->setSelectedClip(t == 2 ? 0 : -1);
        for (int t = 0; t < mainTimeline->audioTrackCount(); ++t)
            mainTimeline->trackAt(true, t)->setSelectedClip(-1);
        int answered = 0, unexpected = 0;
        {
            ModalDriver driver(&window, QStringLiteral("Precompose"));
            driver.start();
            precomposeAction->trigger();
            answered = driver.answered;
            unexpected = driver.unexpected;
        }
        const QStringList parentWarnings = mainTimeline->takeConnectionWarnings();
        const Located left = locate(*mainTimeline, false, aUid);
        const Located y = locateByName(*mainTimeline, false, QStringLiteral("y"));
        const Located xLeft = locate(*mainTimeline, false, xUid);
        const bool parentOk = left.found && left.track == 0 && near(left.startSec, 0.0)
            && near(left.clip.effectiveDuration(), 4.0) && y.found && near(y.startSec, 9.0)
            && y.clip.anchorUid == aUid && near(y.clip.anchorOffsetSec, 9.0) && xLeft.found
            && near(xLeft.startSec, 2.0) && xLeft.clip.anchorUid == aUid && parentWarnings.isEmpty();
        QString nestedId;
        for (const TimelineSequence &sequence : mainTimeline->sequences())
            if (sequence.name == QStringLiteral("Precompose")) nestedId = sequence.id;
        bool nestedOk = false;
        QString nestedDetail;
        const QString activeParent = mainTimeline->activeSequenceId();
        if (!nestedId.isEmpty() && mainTimeline->setActiveSequence(nestedId)) {
            mainTimeline->saveUndoState(QStringLiteral("nested edit"));
            const QStringList nestedWarnings = mainTimeline->takeConnectionWarnings();
            const TimelineTrack *nv1 = mainTimeline->trackAt(false, 0);
            const TimelineTrack *nv2 = mainTimeline->trackAt(false, 1);
            const ClipInfo v1Piece = nv1 && nv1->clipCount() == 1 ? nv1->clips().at(0) : ClipInfo{};
            const ClipInfo v2Piece = nv2 && nv2->clipCount() == 1 ? nv2->clips().at(0) : ClipInfo{};
            nestedOk = !v1Piece.uid.isEmpty() && v2Piece.anchorUid == v1Piece.uid
                && near(v2Piece.leadInSec, 0.0) && near(v2Piece.anchorOffsetSec, 0.0)
                && nestedWarnings.isEmpty();
            nestedDetail = QStringLiteral("v1Clips=%1 v2Clips=%2 v2 anchorUid=%3 (expect the nested V1 uid "
                                          "%4) offset=%5 warnings=%6")
                .arg(nv1 ? nv1->clipCount() : -1).arg(nv2 ? nv2->clipCount() : -1)
                .arg(v2Piece.anchorUid, v1Piece.uid).arg(v2Piece.anchorOffsetSec, 0, 'f', 6)
                .arg(nestedWarnings.size());
            mainTimeline->setActiveSequence(activeParent);
        }
        const bool noModal = QApplication::activeModalWidget() == nullptr;
        std::fprintf(stderr, "connected-clip G28 answered=%d unexpected=%d (expect 1/0) nested id=[%s] "
                     "(name Precompose) left start=%.6f length=%.6f (expect 0 / 4) y[%s] xLeft[%s] "
                     "parentWarnings=%lld (expect 0) parentOk=%d | nested %s ok=%d | noModal=%d\n",
                     answered, unexpected, qPrintable(nestedId), left.startSec, left.clip.effectiveDuration(),
                     qPrintable(describe(y, aUid)), qPrintable(describe(xLeft, aUid)),
                     static_cast<long long>(parentWarnings.size()), parentOk, qPrintable(nestedDetail),
                     nestedOk, noModal);
        tally.gate(28, answered == 1 && unexpected == 0 && parentOk && nestedOk && noModal,
                   "the real pre-compose action did not take the name from its input dialog, moved / "
                   "detached the connection after the range, did not name the nested sequence, or left a modal open");
    }

    // G29 / G30 (US-1304): a media drop on V1 with storyline mode on is a storyline insert at the
    // drop point (real TimelineTrack::dropEvent for G29; Timeline::dropMediaOnTrack for the texts of G30).
    {
        QTemporaryDir dir;
        const QString avPath = dir.filePath(QStringLiteral("g29.mp4"));
        QString whyNot;
        if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &whyNot)) {
            tally.skip(29, qPrintable(whyNot));
            tally.skip(30, qPrintable(whyNot));
            tally.skip(33, qPrintable(whyNot));
        } else if (!dir.isValid() || !writeVideoWithAudioFile(avPath, 20)) {
            std::fprintf(stderr, "connected-clip G29 fixture media could not be written\n");
            tally.gate(29, false, "the G29 fixture media could not be written");
            tally.gate(30, false, "the G30 fixture media could not be written");
            tally.gate(33, false, "the G33 fixture media could not be written");
        } else {
            const QString avName = QFileInfo(avPath).fileName();
            auto linked = [](ClipInfo c, int group) {
                c.linkGroup = group;
                return c;
            };
            // Start of the first clip named `name` on a track, -1 when absent.
            auto startOn = [](const Timeline &tl, bool audio, int track, const QString &name) {
                double cursor = 0.0;
                for (const ClipInfo &clip : tl.trackAt(audio, track)->clips()) {
                    cursor += clip.leadInSec;
                    if (clip.displayName == name) return cursor;
                    cursor += clip.effectiveDuration();
                }
                return -1.0;
            };
            auto layout = [](const Timeline &tl, bool audio, int track) {
                QStringList parts;
                double cursor = 0.0;
                for (const ClipInfo &clip : tl.trackAt(audio, track)->clips()) {
                    cursor += clip.leadInSec;
                    parts << QStringLiteral("%1@%2").arg(clip.displayName).arg(cursor, 0, 'f', 4);
                    cursor += clip.effectiveDuration();
                }
                return QStringLiteral("[") + parts.join(QLatin1Char(' ')) + QLatin1Char(']');
            };
            auto baseline = [](Timeline &tl) {
                tl.setStorylineMode(true);
                tl.undoManager()->clear();
                tl.saveUndoState(QStringLiteral("baseline"));
                tl.takeConnectionWarnings();
            };

            // G29 main: V1 [X 0-5 | Y 5-15], V2 clip c connected to Y at 7, A1 unlinked BGM 0-30,
            // A2 the linked audio of X / Y, A3 free. Y is selected, then a drop at t=5 (the X|Y boundary).
            Timeline tl;
            load(tl, {{linked(clipFor("x", 5.0), 61), linked(clipFor("y", 10.0), 62)}, {clipFor("c", 2.0, 7.0)}},
                 {{clipFor("bgm", 30.0)}, {linked(clipFor("xa", 5.0), 61), linked(clipFor("ya", 10.0), 62)}, {}});
            tl.connectClip(TrackKind::Video, 1, 0, uidAt(tl, 0, 1));
            baseline(tl);
            const QString yUid = uidAt(tl, 0, 1);
            tl.trackAt(false, 0)->setSelectedClip(1);
            // A click on Y: sets the timeline's active video row the way the GUI does.
            emit tl.trackAt(false, 0)->selectionChanged(1, false);
            const auto serialBefore = tl.undoManager()->saveSerial();
            int lastTrack = -2;
            int lastClip = -2;
            QObject::connect(&tl, &Timeline::clipSelectedOnTrack, &tl, [&](int trackIdx, int clipIdx) {
                lastTrack = trackIdx;
                lastClip = clipIdx;
            });
            const bool accepted = sendMediaDrop(tl.trackAt(false, 0), avPath, 5.0);
            bool mainMismatch = false;
            for (const QString &w : tl.takeConnectionWarnings())
                if (w.contains(QStringLiteral("landing mismatch"))) mainMismatch = true;
            const Located fresh = locateByName(tl, false, avName);
            const Located freshAudio = locateByName(tl, true, avName);
            const double len = fresh.clip.effectiveDuration();
            const bool serialOk = tl.undoManager()->saveSerial() == serialBefore + 1;
            const TimelineTrack *v1 = tl.trackAt(false, 0);
            const int selectedAfterDrop = v1->selectedClip();
            const bool mainOk = accepted && v1->clipCount() == 3 && fresh.found && fresh.track == 0 && fresh.index == 1
                && len > 0.5 && near(fresh.startSec, 5.0)
                && near(startOn(tl, false, 0, QStringLiteral("y")), 5.0 + len)
                && near(startOn(tl, false, 1, QStringLiteral("c")), 7.0 + len)
                && near(startOn(tl, true, 1, QStringLiteral("ya")), 5.0 + len)
                && near(startOn(tl, true, 1, QStringLiteral("xa")), 0.0)
                && near(startOn(tl, true, 0, QStringLiteral("bgm")), 0.0) && tl.trackAt(true, 0)->clipCount() == 1
                && freshAudio.found && freshAudio.track == 2 && near(freshAudio.startSec, fresh.startSec)
                && v1->clips().value(2).uid == yUid && selectedAfterDrop == 2 && serialOk
                && !mainMismatch && lastTrack == 0 && lastClip == 2;
            const int sigTrack = lastTrack;
            const int sigClip = lastClip;
            const QString mainV1 = layout(tl, false, 0);
            const QString mainV2 = layout(tl, false, 1);
            const QString mainA1 = layout(tl, true, 0);
            const QString mainA2 = layout(tl, true, 1);
            const QString mainA3 = layout(tl, true, 2);
            tl.undo();
            const bool undoOk = v1->clipCount() == 2 && near(startOn(tl, false, 0, QStringLiteral("y")), 5.0)
                && near(startOn(tl, false, 1, QStringLiteral("c")), 7.0)
                && near(startOn(tl, true, 1, QStringLiteral("ya")), 5.0) && tl.trackAt(true, 2)->clipCount() == 0
                && tl.trackAt(true, 0)->clipCount() == 1;

            // G29 sub-check (US-1304 fix round 3): the same fixture through insertClip3PointActive, a
            // non-import insertClipPreservingDownstream-style caller. Y keeps its selection (shifted),
            // the last clipSelectedOnTrack names V1 and Y (not -1,-1, which would clear the edit target).
            Timeline ins;
            load(ins, {{linked(clipFor("x", 5.0), 61), linked(clipFor("y", 10.0), 62)}, {clipFor("c", 2.0, 7.0)}},
                 {{clipFor("bgm", 30.0)}, {linked(clipFor("xa", 5.0), 61), linked(clipFor("ya", 10.0), 62)}, {}});
            ins.connectClip(TrackKind::Video, 1, 0, uidAt(ins, 0, 1));
            baseline(ins);
            const QString insYUid = uidAt(ins, 0, 1);
            ins.trackAt(false, 0)->setSelectedClip(1);
            emit ins.trackAt(false, 0)->selectionChanged(1, false);
            int insTrack = -2;
            int insClip = -2;
            QObject::connect(&ins, &Timeline::clipSelectedOnTrack, &ins, [&](int trackIdx, int clipIdx) {
                insTrack = trackIdx;
                insClip = clipIdx;
            });
            ins.insertClip3PointActive(5.0, clipFor("n", 3.0));
            const TimelineTrack *insV1 = ins.trackAt(false, 0);
            int insYIndex = -1;
            for (int i = 0; i < insV1->clipCount(); ++i)
                if (insV1->clips().at(i).uid == insYUid) insYIndex = i;
            const bool insOk = insYIndex >= 0 && insV1->selectedClip() == insYIndex
                && insTrack == 0 && insClip == insYIndex;
            std::fprintf(stderr, "connected-clip G29 insertClip3PointActive: yIndex=%d selected=%d lastTrack=%d (expect 0) "
                         "lastClip=%d (expect yIndex) insOk=%d\n",
                         insYIndex, insV1->selectedClip(), insTrack, insClip, insOk);

            // G29 sub-check (US-1304 fix round 4): the selected connected clip C on V2 is pushed past the
            // unconnected D by a V1 insert before Y. Reconcile moves C (removal + re-insert); its
            // selection must follow by uid and be reported as (V2, C's new index), never stale (V2, 0)
            // which now points at D.
            // Own call frame: one more Timeline local in this long function overflows the stack in
            // MainWindow's constructor further down.
            const bool mvOk = [&]() {
            Timeline mv;
            load(mv, {{linked(clipFor("x", 5.0), 61), linked(clipFor("y", 10.0), 62)},
                      {clipFor("c", 2.0, 7.0), clipFor("d", 2.0, 3.0)}},
                 {{clipFor("bgm", 30.0)}, {linked(clipFor("xa", 5.0), 61), linked(clipFor("ya", 10.0), 62)}, {}});
            mv.connectClip(TrackKind::Video, 1, 0, uidAt(mv, 0, 1));
            baseline(mv);
            const QString mvCUid = uidAt(mv, 1, 0);
            mv.trackAt(false, 1)->setSelectedClip(0);
            emit mv.trackAt(false, 1)->selectionChanged(0, false);
            int mvTrack = -2;
            int mvClip = -2;
            QObject::connect(&mv, &Timeline::clipSelectedOnTrack, &mv, [&](int trackIdx, int clipIdx) {
                mvTrack = trackIdx;
                mvClip = clipIdx;
            });
            mv.insertClip3PointActive(5.0, clipFor("n", 20.0));
            const TimelineTrack *mvV2 = mv.trackAt(false, 1);
            int mvCIndex = -1;
            for (int i = 0; i < mvV2->clipCount(); ++i)
                if (mvV2->clips().at(i).uid == mvCUid) mvCIndex = i;
            const bool mvExplicitDeselect = mvTrack == -1 && mvClip == -1;
            const bool ok = mvCIndex == 1 && mvV2->selectedClip() == mvCIndex
                && ((mvTrack == 1 && mvClip == mvCIndex) || mvExplicitDeselect)
                && !(mvTrack == 1 && mvClip == 0);
            std::fprintf(stderr, "connected-clip G29 moved-selection: V2 %s cIndex=%d (expect 1) selected=%d "
                         "lastTrack=%d lastClip=%d (expect 1,cIndex; stale 1,0 points at d) mvOk=%d\n",
                         qPrintable(layout(mv, false, 1)), mvCIndex, mvV2->selectedClip(), mvTrack, mvClip, ok);
            return ok;
            }();

            // G29 sub-check (US-1304 fix round 5): the same push, but C's linked audio partner Ca finds
            // no slot (an unlinked BGM covers its target on every audio track), so the pass that moved
            // C past D is rolled back and the group detached where it stands. C (index 0, D at 1) must
            // stay selected and be the last one announced; the rolled-back pass had it at index 1, which
            // on the restored V2 [C|D] names D.
            const bool rbOk = [&]() {
            Timeline rb;
            load(rb, {{clipFor("x", 5.0), clipFor("y", 10.0)},
                      {linked(clipFor("c", 2.0, 7.0), 81), clipFor("d", 2.0, 3.0)}},
                 {{linked(clipFor("ca", 2.0, 7.0), 81), clipFor("bgm1", 30.0)}, {clipFor("bgm2", 40.0)}});
            rb.connectClip(TrackKind::Video, 1, 0, uidAt(rb, 0, 1));
            baseline(rb);
            const QString rbCUid = uidAt(rb, 1, 0);
            const QString rbDUid = uidAt(rb, 1, 1);
            rb.trackAt(false, 1)->setSelectedClip(0);
            emit rb.trackAt(false, 1)->selectionChanged(0, false);
            int rbTrack = -2;
            int rbClip = -2;
            int rbClipSelected = -2;
            QObject::connect(&rb, &Timeline::clipSelectedOnTrack, &rb, [&](int trackIdx, int clipIdx) {
                rbTrack = trackIdx;
                rbClip = clipIdx;
            });
            QObject::connect(&rb, &Timeline::clipSelected, &rb, [&](int clipIdx) { rbClipSelected = clipIdx; });
            rb.insertClip3PointActive(5.0, clipFor("n", 20.0));
            bool detachWarned = false;
            for (const QString &w : rb.takeConnectionWarnings())
                if (w.contains(QStringLiteral("退避先のトラックがありません"))) detachWarned = true;
            const TimelineTrack *rbV2 = rb.trackAt(false, 1);
            int rbCIndex = -1;
            for (int i = 0; i < rbV2->clipCount(); ++i)
                if (rbV2->clips().at(i).uid == rbCUid) rbCIndex = i;
            const QString selectedUid = rbV2->selectedClip() >= 0
                ? rbV2->clips().value(rbV2->selectedClip()).uid : QString();
            const bool ok = detachWarned && rbCIndex == 0 && rbV2->clips().value(1).uid == rbDUid
                && rbV2->clips().at(0).anchorUid.isEmpty() && selectedUid == rbCUid
                && rbTrack == 1 && rbClip == rbCIndex && rbClipSelected == rbCIndex
                && rb.activeVideoTrackIndex() == 1;
            std::fprintf(stderr, "connected-clip G29 rollback-selection: V2 %s A1 %s A2 %s detachWarned=%d cIndex=%d "
                         "(expect 0) selected=%d selectedIsC=%d selectedIsD=%d lastTrack=%d lastClip=%d "
                         "lastClipSelected=%d (expect 1,0,0) activeTrack=%d (expect 1) rbOk=%d\n",
                         qPrintable(layout(rb, false, 1)), qPrintable(layout(rb, true, 0)),
                         qPrintable(layout(rb, true, 1)), detachWarned, rbCIndex, rbV2->selectedClip(),
                         int(selectedUid == rbCUid), int(selectedUid == rbDUid), rbTrack, rbClip,
                         rbClipSelected, rb.activeVideoTrackIndex(), ok);
            return ok;
            }();

            // G29 variant (C7): a one-frame gap in front of the drop point. V1 [x 0-5 | gap | y | z],
            // drop at t=14 (inside y): the landing is after y at 15 once the gap is closed, not at
            // the stale start of z (15 + gap). The new audio must start on the new V1 clip.
            const double gap = 1.0 / 30.0;
            Timeline gp;
            load(gp, {{linked(clipFor("x", 5.0), 71), linked(clipFor("y", 10.0, gap), 72), linked(clipFor("z", 5.0), 73)}},
                 {{linked(clipFor("xa", 5.0), 71), linked(clipFor("ya", 10.0, gap), 72), linked(clipFor("za", 5.0), 73)}, {}});
            baseline(gp);
            const bool gapAccepted = sendMediaDrop(gp.trackAt(false, 0), avPath, 14.0);
            const Located gapFresh = locateByName(gp, false, avName);
            const Located gapAudio = locateByName(gp, true, avName);
            const double gapLen = gapFresh.clip.effectiveDuration();
            bool gapMismatch = false;
            for (const QString &w : gp.takeConnectionWarnings())
                if (w.contains(QStringLiteral("landing mismatch"))) gapMismatch = true;
            const bool gapOk = gapAccepted && !gapMismatch && gp.trackAt(false, 0)->clipCount() == 4 && gapFresh.found
                && gapFresh.index == 2 && near(gapFresh.startSec, 15.0) && gapAudio.found
                && near(gapAudio.startSec, gapFresh.startSec)
                && near(startOn(gp, false, 0, QStringLiteral("y")), 5.0)
                && near(startOn(gp, false, 0, QStringLiteral("z")), 15.0 + gapLen)
                && near(startOn(gp, true, 0, QStringLiteral("ya")), 5.0)
                && near(startOn(gp, true, 0, QStringLiteral("za")), 15.0 + gapLen);
            std::fprintf(stderr, "connected-clip G29 main: accepted=%d len=%.4f serial+1=%d V1 %s V2 %s A1 %s A2 %s A3 %s "
                         "selected=%d (expect 2) selectionSignal=%d,%d (expect 0,2) mismatch=%d mainOk=%d undoOk=%d | gap variant: accepted=%d len=%.4f V1 %s A1 %s A2 %s "
                         "(expect new clip and its audio both @15.0000, z/za @15+len) gapMismatch=%d gapOk=%d\n",
                         accepted, len, serialOk, qPrintable(mainV1), qPrintable(mainV2),
                         qPrintable(mainA1), qPrintable(mainA2), qPrintable(mainA3),
                         selectedAfterDrop, sigTrack, sigClip, mainMismatch, mainOk, undoOk, gapAccepted, gapLen, qPrintable(layout(gp, false, 0)),
                         qPrintable(layout(gp, true, 0)), qPrintable(layout(gp, true, 1)), gapMismatch, gapOk);
            tally.gate(29, mainOk && undoOk && gapOk && insOk && mvOk && rbOk,
                       "a V1 media drop in storyline mode did not insert at the boundary with Y, its connection and "
                       "its linked audio pushed and the BGM fixed, put the new audio off the new V1 clip's start / "
                       "on top of other audio, took more or fewer than one undo point, lost the selection of Y, left a moved selected connected clip's selection stale or unreported (also after a rolled-back placement pass), or "
                       "(gap variant) placed the audio from the pre-insert start instead of the closed-gap landing");

            // G30: a locked V1, all audio tracks locked, and an audio track full at the landing time.
            auto fixture = [&](Timeline &t, bool oneAudioTrackFull) {
                load(t, {{clipFor("x", 5.0), clipFor("y", 10.0)}},
                     oneAudioTrackFull ? QVector<QVector<ClipInfo>>{{clipFor("bgm", 30.0)}}
                                       : QVector<QVector<ClipInfo>>{{clipFor("bgm", 3.0)}, {}});
                baseline(t);
            };
            auto snapshot = [&](const Timeline &t) {
                return layout(t, false, 0) + layout(t, true, 0) + (t.audioTracks().size() > 1 ? layout(t, true, 1) : QString())
                    + QString::number(t.videoTracks().size()) + QString::number(t.audioTracks().size());
            };
            const QString lockText = QStringLiteral("ロック中のトラックには配置できません");
            const QString slotText = QStringLiteral("音声を置ける空きトラックがありません");
            Timeline lockedV1;
            fixture(lockedV1, false);
            lockedV1.trackAt(false, 0)->setLocked(true);
            const QString lockedBefore = snapshot(lockedV1);
            const auto lockedSerial = lockedV1.undoManager()->saveSerial();
            QString lockedErr;
            const bool lockedDrop = lockedV1.dropMediaOnTrack(lockedV1.trackAt(false, 0), avPath, 3.0, &lockedErr);
            const bool lockedGui = sendMediaDrop(lockedV1.trackAt(false, 0), avPath, 3.0);
            const bool lockedOk = !lockedDrop && !lockedGui && lockedErr == lockText
                && snapshot(lockedV1) == lockedBefore && lockedV1.undoManager()->saveSerial() == lockedSerial;
            Timeline audioLocked;
            fixture(audioLocked, false);
            for (TimelineTrack *a : audioLocked.audioTracks()) a->setLocked(true);
            const QString audioLockedBefore = snapshot(audioLocked);
            const auto audioLockedSerial = audioLocked.undoManager()->saveSerial();
            QString audioLockedErr;
            const bool audioLockedDrop = audioLocked.dropMediaOnTrack(audioLocked.trackAt(false, 0), avPath, 3.0, &audioLockedErr);
            const bool audioLockedOk = !audioLockedDrop && audioLockedErr == slotText
                && snapshot(audioLocked) == audioLockedBefore
                && audioLocked.undoManager()->saveSerial() == audioLockedSerial;
            Timeline audioFull;
            fixture(audioFull, true);
            const QString audioFullBefore = snapshot(audioFull);
            const auto audioFullSerial = audioFull.undoManager()->saveSerial();
            QString audioFullErr;
            const bool audioFullDrop = audioFull.dropMediaOnTrack(audioFull.trackAt(false, 0), avPath, 3.0, &audioFullErr);
            const bool audioFullOk = !audioFullDrop && audioFullErr == slotText
                && snapshot(audioFull) == audioFullBefore && audioFull.undoManager()->saveSerial() == audioFullSerial;
            // Control: the same fixture with one free unlocked audio track accepts the drop.
            Timeline control;
            fixture(control, false);
            QString controlErr;
            const bool controlDrop = control.dropMediaOnTrack(control.trackAt(false, 0), avPath, 3.0, &controlErr);
            const bool controlOk = controlDrop && control.trackAt(false, 0)->clipCount() == 3;
            std::fprintf(stderr, "connected-clip G30 lockedV1: drop=%d gui=%d err='%s' unchanged=%d ok=%d | audioLocked: drop=%d "
                         "err='%s' unchanged=%d ok=%d | audioFull: drop=%d err='%s' unchanged=%d ok=%d | control (free track): "
                         "drop=%d V1=%d err='%s' ok=%d (expect refusals with the lock text / the free-track text, control 1)\n",
                         lockedDrop, lockedGui, qPrintable(lockedErr), snapshot(lockedV1) == lockedBefore, lockedOk,
                         audioLockedDrop, qPrintable(audioLockedErr), snapshot(audioLocked) == audioLockedBefore,
                         audioLockedOk, audioFullDrop, qPrintable(audioFullErr), snapshot(audioFull) == audioFullBefore,
                         audioFullOk, controlDrop, control.trackAt(false, 0)->clipCount(), qPrintable(controlErr),
                         controlOk);
            tally.gate(30, lockedOk && audioLockedOk && audioFullOk && controlOk,
                       "a V1 drop was taken on a locked V1 or refused with another text / changed state, or a drop with "
                       "every audio track locked or full at the landing time changed V1 / used another text, or the "
                       "free-track control was refused");

            // G33 (US-1320), in its own function to keep this frame small.
            composedRemapGate(tally, avPath, avName);
        }
    }

    // G31 (US-1303), in its own function to keep this frame small.
    keyRemapGate(tally);

    // G32 (US-1302): the friend hands the private sidecars to the gate function.
    if (!mainTimeline) {
        tally.gate(32, false, "MainWindow has no mainTimeline");
    } else {
        SidecarView view;
        view.timeRemap = &window.m_timeRemapClipEntries;
        view.text3D = &window.m_text3DClipConfigs;
        view.expressions = &window.m_clipExpressionBindings;
        view.wiggle = &window.m_clipWiggleParams;
        view.brush = &window.m_brushAnimationEntries;
        view.liveBrush = &window.m_liveBrushAnimations;
        view.materializeBrush = &MainWindow::materializeBrushAnimation;
        view.addBrush = &MainWindow::addBrushAnimation;
        view.save = &MainWindow::saveProjectToPath;
        view.open = &MainWindow::openProjectFromPath;
        view.commitTimeRemap = &MainWindow::commitTimeRemapEntry;
        view.commitText3D = &MainWindow::commitText3DConfig;
        view.commitExpressions = &MainWindow::commitExpressionBindings;
        view.commitWiggle = &MainWindow::commitWiggleParams;
        sidecarRekeyGate(tally, window, *mainTimeline, view);
    }

    // G34 (US-1320): the brush preview after a storyline insert that moves the selection.
    if (!mainTimeline) {
        tally.gate(34, false, "MainWindow has no mainTimeline");
    } else {
        SidecarView view;
        view.brush = &window.m_brushAnimationEntries;
        view.liveBrush = &window.m_liveBrushAnimations;
        view.brushPreviewClipId = &MainWindow::brushPreviewClipIdForTest;
        brushPreviewRebindGate(tally, window, *mainTimeline, view);
    }

    // G35 (US-1400): the source monitor's 3-point edits rekey the carriers and honour the V1 lock.
    if (!mainTimeline) {
        tally.gate(35, false, "MainWindow has no mainTimeline");
    } else {
        SidecarView view;
        view.timeRemap = &window.m_timeRemapClipEntries;
        view.sourceInsert = &MainWindow::onSourceInsertRequested;
        view.sourceOverwrite = &MainWindow::onSourceOverwriteRequested;
        threePointCarrierGate(tally, *mainTimeline, view, window);
    }

    // G36 (US-1401): the four sidecars' dialog commits are undo points and undo carries them by value.
    if (!mainTimeline) {
        tally.gate(36, false, "MainWindow has no mainTimeline");
    } else {
        SidecarView view;
        view.timeRemap = &window.m_timeRemapClipEntries;
        view.text3D = &window.m_text3DClipConfigs;
        view.expressions = &window.m_clipExpressionBindings;
        view.wiggle = &window.m_clipWiggleParams;
        view.save = &MainWindow::saveProjectToPath;
        view.open = &MainWindow::openProjectFromPath;
        view.commitTimeRemap = &MainWindow::commitTimeRemapEntry;
        view.commitText3D = &MainWindow::commitText3DConfig;
        view.commitExpressions = &MainWindow::commitExpressionBindings;
        view.commitWiggle = &MainWindow::commitWiggleParams;
        view.editWiggle = &MainWindow::editClipWiggle;
        sidecarUndoValuesGate(tally, window, *mainTimeline, view);
    }

    return tally.finish("connected-clip");
}
