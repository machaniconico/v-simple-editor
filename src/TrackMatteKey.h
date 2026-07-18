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
// Invariant across all four mutation slots (delete / rippleDelete /
// splitAtPlayhead / pasteClip): a surviving clip's filePath, linkGroup, and
// inPoint are never mutated by those ops.
//
// NOTE: the walk below assumes per-track identity uniqueness. paste()
// deliberately violates this (byte-identical ClipInfo copy), so runs of
// equal-identity clips are broken by position-within-run (k-th surviving
// duplicate maps to k-th old duplicate still present). See
// remapTrackMatteEntriesAfterMutation for the tie-break logic.
struct ClipKeyId {
    QString filePath;
    int linkGroup = 0;
    double inPoint = 0.0;

    bool sameAs(const ClipKeyId &o) const;
};

// Per video-track vector of clip keys (index == clip index at snapshot time).
using TrackClipSnapshot = QVector<QVector<ClipKeyId>>;

// Snapshot each video track's clip vector BEFORE a mutation. Call this
// immediately before the mutation so the snapshot reflects pre-mutation state.
TrackClipSnapshot snapshotTrackClips(const Timeline *timeline);

// After a mutation, align old (snapshot) clip indices against the new
// Timeline state via an order-preserving monotonic two-pointer walk and
// rewrite every positional key in `entries` (MainWindow GUI map type).
// Entries whose owning clip or matte-source clip no longer exists are dropped.
// Ownership: called by MainWindow mutation slots on m_trackMatteClipEntries.
void remapTrackMatteEntriesAfterMutation(
    Timeline *timeline,
    QHash<QString, TrackMatteClipEntry> &entries,
    const TrackClipSnapshot &before);

// RM-5: Timeline-carrier sibling — same alignment logic but operates on
// Timeline's own m_trackMatteEntries (TimelineTrackMatteEntry type, keyed by
// clipId, no separate clipId field inside the value). Called by Timeline
// itself after moveClip / cross-track drop / deleteSelectedClip so the
// carrier stays correct without any MainWindow involvement.
// Ownership rule: Timeline owns its carrier remap on reorder/drop/delete
// paths; MainWindow slots own m_trackMatteClipEntries remap on their 4
// mutation paths and then sync the carrier via syncTrackMatteEntriesToTimeline.
// The two are never called on the same mutation event, so no double-remap.
void remapTimelineCarrierAfterMutation(
    Timeline *timeline,
    QHash<QString, TimelineTrackMatteEntry> &carrier,
    const TrackClipSnapshot &before);

void remapClipParentEntriesAfterMutation(
    Timeline *timeline,
    QHash<QString, QString> &entries,
    const TrackClipSnapshot &before);
