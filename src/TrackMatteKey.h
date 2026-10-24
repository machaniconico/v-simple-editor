#pragma once

#include <QString>
#include <QHash>
#include <QVector>

// RM-1.1: the ONE canonical track-matte clip-key formula. Three producers
// must agree byte-for-byte on this string or the matte hash silently
// mis-applies: MainWindow::brushClipId (GUI authoritative map),
// tlrender::renderClipId (renderFrameAt layer ids), and
// RenderQueue::resolveTimeline (persisted-project carrier population).
// Previously each duplicated QStringLiteral("%1:%2").arg(track).arg(clip);
// any drift desynced the matte. Keep them all calling this.
inline QString trackMatteClipKey(int trackIdx, int clipIdx)
{
    return QStringLiteral("%1:%2").arg(trackIdx).arg(clipIdx);
}

// Re-canonicalises a persisted "track:clip" id through trackMatteClipKey so a
// hand-edited / legacy project cannot desync the consumers (RenderQueue::
// resolveTimeline, ProjectFile::applyTimeRemapEntries). Unparseable ids pass
// through unchanged.
inline QString canonicalClipKey(const QString &raw)
{
    const int colon = raw.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return raw;
    bool okT = false, okC = false;
    const int t = raw.left(colon).toInt(&okT);
    const int c = raw.mid(colon + 1).toInt(&okC);
    if (okT && okC && t >= 0 && c >= 0)
        return trackMatteClipKey(t, c);
    return raw;
}

// RM-4: hoisted remap types and functions (formerly MainWindow.cpp anon
// namespace). Declared here so the reindex selftest in main.cpp can drive
// them without coupling to MainWindow.
//
// These types are deliberately lightweight (no Qt MOC, no QObject) so they
// can be included from main.cpp's selftest without pulling in MainWindow.h.

class Timeline;
struct TrackMatteClipEntry;
struct TimelineTrackMatteEntry;

// Per-clip identity key used for order-preserving survivor alignment.
// US-1119: sameAs compares ClipInfo::uid when both keys carry one, so paste
// clones (equal filePath, linkGroup and inPoint, distinct uid) stay apart.
// When either uid is empty (a hand-made ClipInfo or snapshot, or a pre-uid
// key) it falls back to filePath, linkGroup and inPoint. A surviving clip's
// uid is never changed by a mutation; split keeps it on the left half.
struct ClipKeyId {
    QString filePath;
    int linkGroup = 0;
    double inPoint = 0.0;
    QString uid;

    bool sameAs(const ClipKeyId &o) const;
};

// Per video-track vector of clip keys (index == clip index at snapshot time).
using TrackClipSnapshot = QVector<QVector<ClipKeyId>>;

// Snapshot each video track's clip vector BEFORE a mutation. Call this
// immediately before the mutation so the snapshot reflects pre-mutation state.
TrackClipSnapshot snapshotTrackClips(const Timeline *timeline);

// Shared survivor alignment for matte, parent, and roto carriers.
QHash<QString, QString> alignedClipKeyMap(
    const Timeline *timeline, const TrackClipSnapshot &before);

// After a mutation, align old (snapshot) clip indices against the new
// Timeline state via an order-preserving monotonic two-pointer walk and
// rewrite every positional key in `entries` (MainWindow GUI map type).
// Entries whose owning clip or matte-source clip no longer exists are dropped.
// Selftest only: production keeps no GUI matte map (the Timeline carrier is
// the matte SSOT), so only the trackmatte reindex / RM-6 selftests call it.
// This and the two helpers below first call Timeline's roto remap. Its
// post-mutation snapshot memo skips repeated calls for the same placement.
void remapTrackMatteEntriesAfterMutation(
    Timeline *timeline,
    QHash<QString, TrackMatteClipEntry> &entries,
    const TrackClipSnapshot &before);

// RM-5: Timeline-carrier sibling — same alignment logic but operates on
// Timeline's own m_trackMatteEntries (TimelineTrackMatteEntry type, keyed by
// clipId, no separate clipId field inside the value). Timeline is the matte
// SSOT: its edit paths remap the carrier themselves (Timeline::
// remapTrackMatteLocked), and MainWindow edits that change clips outside
// Timeline call Timeline::remapCarriersAfterExternalMutation. MainWindow keeps
// no matte copy. Each helper calls Timeline::remapRotoEntriesAfterMutation
// first; its memo prevents a repeated roto remap when the clip placement has
// not changed.
void remapTimelineCarrierAfterMutation(
    Timeline *timeline,
    QHash<QString, TimelineTrackMatteEntry> &carrier,
    const TrackClipSnapshot &before);

void remapClipParentEntriesAfterMutation(
    Timeline *timeline,
    QHash<QString, QString> &entries,
    const TrackClipSnapshot &before);

// US-1201: carrier remap for Timeline::reconcileConnectedClips. Maps every
// old "t:i" key in `before` to the clip with the same uid in the current
// video tracks (a clip may change tracks), so a reorder that a monotonic walk
// cannot follow keeps its roto / matte / parent entries. Keys of clips that no
// longer exist are dropped; keys that named no clip are kept. `explicitMoves`
// (old video key -> new video key, from the caller that moved the clips) is
// applied first and wins. A clip whose uid is empty or repeated across tracks
// otherwise falls back to alignedClipKeyMap; the leftovers of a repeated uid
// are then paired by uid in track order. The parent carrier's sequence store
// key is taken out and put back unchanged.
void remapCarriersByUid(Timeline *timeline, const TrackClipSnapshot &before,
                        const QHash<QString, QString> &explicitMoves = {});
