#include "TrackMatteKey.h"
#include "Timeline.h"
#include "ProjectFile.h"
#include <QtGlobal>

bool ClipKeyId::sameAs(const ClipKeyId &o) const
{
    return filePath == o.filePath
        && linkGroup == o.linkGroup
        && qFuzzyCompare(inPoint + 1.0, o.inPoint + 1.0);
}

TrackClipSnapshot snapshotTrackClips(const Timeline *timeline)
{
    TrackClipSnapshot snap;
    if (!timeline)
        return snap;
    const QVector<TimelineTrack *> &tracks = timeline->videoTracks();
    snap.reserve(tracks.size());
    for (const TimelineTrack *trk : tracks) {
        QVector<ClipKeyId> ids;
        if (trk) {
            const QVector<ClipInfo> &clips = trk->clips();
            ids.reserve(clips.size());
            for (const ClipInfo &c : clips)
                ids.append({c.filePath, c.linkGroup, c.inPoint});
        }
        snap.append(ids);
    }
    return snap;
}

// Order-preserving alignment: walk old[] and new[] together with a monotonic
// nc pointer, recording old->new for each surviving clip.
//
// Duplicate-identity tie-break (RM-6): paste inserts a byte-identical
// ClipInfo, so equal-identity runs can exist within a track. The walk must
// NOT use a lookahead probe that resets nc — that would let a later duplicate
// steal the slot of an earlier one. Instead nc always advances monotonically:
// after binding old[oc]->new[nc], nc++ immediately, so the next old clip can
// only match at nc or beyond. Within an equal-identity run this means the
// k-th surviving old duplicate is bound to the k-th new occurrence of that
// identity, which is the correct position-within-run tie-break.
// LIMITATION: identity-only snapshot — paste-clone ambiguity.
// ClipKeyId compares {filePath, linkGroup, inPoint}. When two or more clips in
// the same track are byte-identical (e.g. after a paste that duplicates an
// existing clip), they are indistinguishable in the snapshot. The monotonic
// walk assigns the k-th surviving old duplicate to the k-th new occurrence of
// that identity; if the first clone is deleted, the entry for the second clone
// may be pruned rather than remapped. This is deliberate conservative behaviour
// (prune rather than misassign). A true fix requires a stable per-clip UID
// threaded through ClipInfo and ProjectFile serialization — out of scope here.
static QHash<QString, QString> buildOldToNewMap(
    const Timeline *timeline,
    const TrackClipSnapshot &before)
{
    QHash<QString, QString> oldToNew;
    if (!timeline) return oldToNew;
    const QVector<TimelineTrack *> &tracks = timeline->videoTracks();
    const int trackCount = qMin(before.size(),
                                static_cast<int>(tracks.size()));
    for (int t = 0; t < trackCount; ++t) {
        const TimelineTrack *trk = tracks[t];
        if (!trk) continue;
        const QVector<ClipKeyId> &oldIds  = before[t];
        const QVector<ClipInfo>  &newClips = trk->clips();
        int nc = 0;  // low-water mark: never scan below this index
        for (int oc = 0; oc < oldIds.size(); ++oc) {
            // Probe forward from nc without advancing nc permanently.
            // nc only advances when a match is found, keeping the walk
            // monotonic: each new slot is consumed at most once.
            // This handles paste-duplicate runs correctly: if old[oc] and
            // old[oc+1] have the same identity, the probe for old[oc] stops
            // at new[probe0] and consumes it (nc = probe0+1); the probe for
            // old[oc+1] starts at nc = probe0+1, so it cannot re-consume
            // new[probe0]. This is the RM-6 tie-break.
            int probe = nc;
            while (probe < newClips.size()) {
                const ClipKeyId cur{newClips[probe].filePath,
                                    newClips[probe].linkGroup,
                                    newClips[probe].inPoint};
                if (oldIds[oc].sameAs(cur))
                    break;
                ++probe;
            }
            if (probe < newClips.size()) {
                oldToNew.insert(trackMatteClipKey(t, oc),
                                trackMatteClipKey(t, probe));
                nc = probe + 1;     // monotonic: never reuse a consumed slot
            }
            // else: old clip oc was deleted — leave it out of the map.
        }
    }
    return oldToNew;
}

// MainWindow GUI-map variant (TrackMatteClipEntry, has clipId field).
// Ownership: called by the 4 MainWindow mutation slots on m_trackMatteClipEntries.
void remapTrackMatteEntriesAfterMutation(
    Timeline *timeline,
    QHash<QString, TrackMatteClipEntry> &entries,
    const TrackClipSnapshot &before)
{
    if (!timeline || entries.isEmpty())
        return;
    const QHash<QString, QString> oldToNew = buildOldToNewMap(timeline, before);
    QHash<QString, TrackMatteClipEntry> rebuilt;
    rebuilt.reserve(entries.size());
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const auto keyIt = oldToNew.constFind(it.key());
        if (keyIt == oldToNew.cend())
            continue;               // owning clip was deleted
        TrackMatteClipEntry e = it.value();
        e.clipId = keyIt.value();
        const auto srcIt = oldToNew.constFind(e.matteSourceClipId);
        if (srcIt == oldToNew.cend())
            continue;               // matte source clip deleted
        e.matteSourceClipId = srcIt.value();
        rebuilt.insert(e.clipId, e);
    }
    entries = rebuilt;
}

// RM-5: Timeline-carrier variant (TimelineTrackMatteEntry, key IS the clipId).
//
// Ownership rules:
//   • within-track reorder (moveClip): handled by the connectTrack clipMoved
//     lambda via direct index arithmetic — this function is NOT called there.
//   • cross-track drop (handleCrossTrackLinkedDrop): Timeline calls this
//     function directly via snapshot walk.
//   • deleteSelectedClip: Timeline calls this function directly. This path is
//     LOAD-BEARING for direct callers (context-menu, keyboard shortcut) that
//     never pass through a MainWindow slot. When a MainWindow slot (deleteClip,
//     rippleDelete) does call deleteSelectedClip, Timeline's internal remap runs
//     first, then MainWindow's remapTrackMatteEntriesAfterMutation + sync runs
//     after — the final syncTrackMatteEntriesToTimeline overwrites the carrier
//     with the GUI-map result, so the double-remap is redundant but harmless.
void remapTimelineCarrierAfterMutation(
    Timeline *timeline,
    QHash<QString, TimelineTrackMatteEntry> &carrier,
    const TrackClipSnapshot &before)
{
    if (!timeline || carrier.isEmpty())
        return;
    const QHash<QString, QString> oldToNew = buildOldToNewMap(timeline, before);
    QHash<QString, TimelineTrackMatteEntry> rebuilt;
    rebuilt.reserve(carrier.size());
    for (auto it = carrier.cbegin(); it != carrier.cend(); ++it) {
        const auto keyIt = oldToNew.constFind(it.key());
        if (keyIt == oldToNew.cend())
            continue;               // owning clip was deleted / unmapped
        TimelineTrackMatteEntry e = it.value();
        const auto srcIt = oldToNew.constFind(e.matteSourceClipId);
        if (srcIt == oldToNew.cend())
            continue;               // matte source clip deleted
        e.matteSourceClipId = srcIt.value();
        rebuilt.insert(keyIt.value(), e);
    }
    carrier = rebuilt;
}

void remapClipParentEntriesAfterMutation(
    Timeline *timeline,
    QHash<QString, QString> &entries,
    const TrackClipSnapshot &before)
{
    if (!timeline || entries.isEmpty())
        return;
    const QHash<QString, QString> oldToNew = buildOldToNewMap(timeline, before);
    QHash<QString, QString> rebuilt;
    rebuilt.reserve(entries.size());
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const auto childIt = oldToNew.constFind(it.key());
        if (childIt == oldToNew.cend())
            continue;
        const auto parentIt = oldToNew.constFind(it.value());
        if (parentIt == oldToNew.cend())
            continue;
        if (childIt.value() == parentIt.value())
            continue;
        rebuilt.insert(childIt.value(), parentIt.value());
    }
    entries = rebuilt;
}
