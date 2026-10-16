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
