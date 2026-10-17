#include "TrackMatteKey.h"
#include "Timeline.h"
#include "ProjectFile.h"
#include <QtGlobal>
#include <QSet>

bool ClipKeyId::sameAs(const ClipKeyId &o) const
{
    if (!uid.isEmpty() && !o.uid.isEmpty())
        return uid == o.uid;
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
                ids.append({c.filePath, c.linkGroup, c.inPoint, c.uid});
        }
        snap.append(ids);
    }
    return snap;
}

// Order-preserving alignment: walk old[] and new[] together with a monotonic
// nc pointer, recording old->new for each surviving clip.
//
// Duplicate-identity tie-break (RM-6): the walk must NOT use a lookahead probe
// that resets nc — that would let a later duplicate steal the slot of an
// earlier one. Instead nc always advances monotonically: after binding
// old[oc]->new[nc], nc++ immediately, so the next old clip can only match at
// nc or beyond.
// US-1119: ClipKeyId::sameAs compares the clip uid when both sides carry one,
// so a paste clone (same filePath, linkGroup and inPoint, its own uid) is told
// apart from its original and the first match is the clip itself. Only a key
// without a uid (a hand-made ClipInfo or snapshot) falls back to those three
// fields, where equal clones stay ambiguous: the k-th surviving old duplicate
// binds to the k-th new occurrence, and deleting the first clone can prune
// the second clone's entry.
QHash<QString, QString> alignedClipKeyMap(
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
                                    newClips[probe].inPoint,
                                    newClips[probe].uid};
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

// GUI-map variant (TrackMatteClipEntry, has clipId field). Selftest only:
// MainWindow keeps no matte map since the Timeline carrier became the SSOT.
// Each remap helper first asks Timeline to remap roto; its post-mutation
// snapshot memo skips repeated calls from the same mutation site.
void remapTrackMatteEntriesAfterMutation(
    Timeline *timeline,
    QHash<QString, TrackMatteClipEntry> &entries,
    const TrackClipSnapshot &before)
{
    if (timeline) timeline->remapRotoEntriesAfterMutation(before);
    if (!timeline || entries.isEmpty())
        return;
    const QHash<QString, QString> oldToNew = alignedClipKeyMap(timeline, before);
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
//   • cross-track drop of a video clip (TimelineTrack::clipMovedAcrossTracks):
//     handled by Timeline::remapCarriersAfterCrossTrackMove, an explicit
//     index map with no snapshot walk and no sameAs — this function is NOT
//     called there.
//   • linked-partner drop (handleCrossTrackLinkedDrop): returns at once when
//     linkGroup <= 0. Only an audio partner (the destIsVideo branch) reaches
//     this function, through remapTrackMatteLocked(snapBefore); a video
//     partner takes remapCarriersAfterCrossTrackMove instead.
//   • deleteSelectedClip and the other Timeline edit paths: Timeline calls
//     this function through remapTrackMatteLocked. MainWindow slots that edit
//     through Timeline (deleteClip, rippleDelete, ...) rely on that remap;
//     MainWindow edits made outside Timeline call
//     Timeline::remapCarriersAfterExternalMutation. The carrier is the SSOT and
//     nothing overwrites it after the remap.
// All three helpers call Timeline::remapRotoEntriesAfterMutation first, even
// when their own carrier is empty. Roto's post-mutation snapshot memo skips
// repeated remaps at the same site, including the carrier/parent pair.
void remapTimelineCarrierAfterMutation(
    Timeline *timeline,
    QHash<QString, TimelineTrackMatteEntry> &carrier,
    const TrackClipSnapshot &before)
{
    if (timeline) timeline->remapRotoEntriesAfterMutation(before);
    if (!timeline || carrier.isEmpty())
        return;
    const QHash<QString, QString> oldToNew = alignedClipKeyMap(timeline, before);
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
    if (timeline) timeline->remapRotoEntriesAfterMutation(before);
    if (!timeline || entries.isEmpty())
        return;
    const QHash<QString, QString> oldToNew = alignedClipKeyMap(timeline, before);
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

void remapCarriersByUid(Timeline *timeline, const TrackClipSnapshot &before,
                        const QHash<QString, QString> &explicitMoves)
{
    if (!timeline)
        return;
    const TrackClipSnapshot after = snapshotTrackClips(timeline);
    QHash<QString, int> beforeCount;
    QHash<QString, int> afterCount;
    for (const auto &track : before)
        for (const ClipKeyId &id : track) ++beforeCount[id.uid];
    for (const auto &track : after)
        for (const ClipKeyId &id : track) ++afterCount[id.uid];
    auto unique = [&](const QString &uid) {
        return !uid.isEmpty() && beforeCount.value(uid) == 1 && afterCount.value(uid) == 1;
    };
    QHash<QString, QString> uidToNewKey;
    for (int t = 0; t < after.size(); ++t)
        for (int i = 0; i < after[t].size(); ++i)
            if (unique(after[t][i].uid))
                uidToNewKey.insert(after[t][i].uid, trackMatteClipKey(t, i));
    QSet<QString> oldKeys;
    QHash<QString, QString> oldToNew;
    // The caller's own provenance first: a repeated uid cannot tell two moved
    // clips apart, the caller knows which old key went where.
    QSet<QString> reached;
    for (auto it = explicitMoves.cbegin(); it != explicitMoves.cend(); ++it) {
        oldToNew.insert(it.key(), it.value());
        reached.insert(it.value());
    }
    QHash<QString, QString> aligned;
    bool alignedBuilt = false;
    for (int t = 0; t < before.size(); ++t) {
        for (int i = 0; i < before[t].size(); ++i) {
            const QString oldKey = trackMatteClipKey(t, i);
            oldKeys.insert(oldKey);
            if (oldToNew.contains(oldKey)) continue;
            const QString &uid = before[t][i].uid;
            if (unique(uid)) {
                const auto it = uidToNewKey.constFind(uid);
                if (it != uidToNewKey.cend()) oldToNew.insert(oldKey, it.value());
                continue;
            }
            if (!alignedBuilt) {
                aligned = alignedClipKeyMap(timeline, before);
                alignedBuilt = true;
            }
            const auto it = aligned.constFind(oldKey);
            if (it != aligned.cend() && !reached.contains(it.value()))
                oldToNew.insert(oldKey, it.value());
        }
    }
    // alignedClipKeyMap pairs old[t] with new[t] only, so a clip that moved to
    // another track while its uid also sits on a third track is not in
    // `aligned`. Uids are unique per track: pair the leftover before / after
    // keys of each repeated uid in ascending track order when the counts match.
    if (alignedBuilt) {
        for (auto it = oldToNew.cbegin(); it != oldToNew.cend(); ++it) reached.insert(it.value());
        QHash<QString, QStringList> leftBefore;
        QHash<QString, QStringList> leftAfter;
        for (int t = 0; t < before.size(); ++t)
            for (int i = 0; i < before[t].size(); ++i) {
                const QString &uid = before[t][i].uid;
                const QString key = trackMatteClipKey(t, i);
                if (!uid.isEmpty() && !unique(uid) && !oldToNew.contains(key))
                    leftBefore[uid].append(key);
            }
        for (int t = 0; t < after.size(); ++t)
            for (int i = 0; i < after[t].size(); ++i) {
                const QString &uid = after[t][i].uid;
                const QString key = trackMatteClipKey(t, i);
                if (leftBefore.contains(uid) && !reached.contains(key))
                    leftAfter[uid].append(key);
            }
        for (auto it = leftBefore.cbegin(); it != leftBefore.cend(); ++it) {
            const QStringList newKeys = leftAfter.value(it.key());
            if (newKeys.size() != it.value().size()) continue;
            for (int k = 0; k < newKeys.size(); ++k) oldToNew.insert(it.value()[k], newKeys[k]);
        }
    }
    // Empty result = the key named a clip that is gone (drop the entry).
    auto mapKey = [&](const QString &key) {
        if (!oldKeys.contains(key)) return key;
        return oldToNew.value(key);
    };

    const auto roto = timeline->rotoEntries();
    if (!roto.isEmpty()) {
        QHash<QString, RotoClipEntry> rebuilt;
        rebuilt.reserve(roto.size());
        for (auto it = roto.cbegin(); it != roto.cend(); ++it) {
            const QString key = mapKey(it.key());
            if (!key.isEmpty()) rebuilt.insert(key, it.value());
        }
        timeline->setRotoEntries(rebuilt);
    }
    const auto mattes = timeline->trackMatteEntries();
    if (!mattes.isEmpty()) {
        QHash<QString, TimelineTrackMatteEntry> rebuilt;
        rebuilt.reserve(mattes.size());
        for (auto it = mattes.cbegin(); it != mattes.cend(); ++it) {
            const QString key = mapKey(it.key());
            TimelineTrackMatteEntry e = it.value();
            e.matteSourceClipId = mapKey(e.matteSourceClipId);
            if (!key.isEmpty() && !e.matteSourceClipId.isEmpty()) rebuilt.insert(key, e);
        }
        timeline->setTrackMatteEntries(rebuilt);
    }
    auto parents = timeline->clipParentEntries();
    const QString sequenceStoreKey = timeline_nesting::sequenceStoreParentKey();
    const QString sequenceStore = parents.take(sequenceStoreKey);
    if (!parents.isEmpty()) {
        QHash<QString, QString> rebuilt;
        rebuilt.reserve(parents.size() + 1);
        for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
            const QString child = mapKey(it.key());
            const QString parent = mapKey(it.value());
            if (!child.isEmpty() && !parent.isEmpty() && child != parent)
                rebuilt.insert(child, parent);
        }
        if (!sequenceStore.isEmpty())
            rebuilt.insert(sequenceStoreKey, sequenceStore);
        timeline->setClipParentEntries(rebuilt);
    }
}
