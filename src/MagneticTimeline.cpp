#include "MagneticTimeline.h"

#include <algorithm>

namespace magtl {

// ---------------------------------------------------------------------------
// closeGaps
// ---------------------------------------------------------------------------
// For each track, sort by startMs and close gaps larger than minGapMs by
// shifting subsequent clips so they begin exactly at the previous clip's end.
QList<Clip> closeGaps(const QList<Clip>& clips, qint64 minGapMs)
{
    if (clips.isEmpty())
        return clips;

    // Collect all unique track indices
    QList<int> tracks;
    for (const Clip& c : clips) {
        if (!tracks.contains(c.trackIndex))
            tracks.append(c.trackIndex);
    }

    // Build result by processing each track independently
    QList<Clip> result;
    result.reserve(clips.size());

    for (int track : tracks) {
        // Gather clips belonging to this track (preserve originals for id round-trip)
        QList<Clip> trackClips;
        for (const Clip& c : clips) {
            if (c.trackIndex == track)
                trackClips.append(c);
        }

        // Sort by startMs
        std::stable_sort(trackClips.begin(), trackClips.end(),
            [](const Clip& a, const Clip& b) { return a.startMs < b.startMs; });

        // Close gaps: if gap > minGapMs, shift clip forward to remove it
        for (int i = 1; i < trackClips.size(); ++i) {
            const qint64 gap = trackClips[i].startMs - trackClips[i - 1].endMs;
            if (gap > minGapMs) {
                const qint64 shift = gap - minGapMs;
                trackClips[i].startMs -= shift;
                trackClips[i].endMs   -= shift;
            }
        }

        for (const Clip& c : trackClips)
            result.append(c);
    }

    // Final sort: trackIndex asc, then startMs asc
    std::stable_sort(result.begin(), result.end(), [](const Clip& a, const Clip& b) {
        if (a.trackIndex != b.trackIndex) return a.trackIndex < b.trackIndex;
        return a.startMs < b.startMs;
    });

    return result;
}

// ---------------------------------------------------------------------------
// rippleInsert
// ---------------------------------------------------------------------------
// Shift all clips on newClip's track whose startMs >= newClip.startMs
// rightward by newClip.durationMs(), then append newClip.
QList<Clip> rippleInsert(const QList<Clip>& clips, const Clip& newClip)
{
    const qint64 shift    = newClip.durationMs();
    const int    track    = newClip.trackIndex;
    const qint64 insertAt = newClip.startMs;

    QList<Clip> result;
    result.reserve(clips.size() + 1);

    for (Clip c : clips) {
        if (c.trackIndex == track && c.startMs >= insertAt) {
            c.startMs += shift;
            c.endMs   += shift;
        }
        result.append(c);
    }

    result.append(newClip);

    // Sort: trackIndex asc, startMs asc
    std::stable_sort(result.begin(), result.end(), [](const Clip& a, const Clip& b) {
        if (a.trackIndex != b.trackIndex) return a.trackIndex < b.trackIndex;
        return a.startMs < b.startMs;
    });

    return result;
}

// ---------------------------------------------------------------------------
// rippleDelete
// ---------------------------------------------------------------------------
// Remove the clip at deleteIndex and shift all later clips on the same track
// forward by the deleted clip's duration.
QList<Clip> rippleDelete(const QList<Clip>& clips, int deleteIndex)
{
    if (deleteIndex < 0 || deleteIndex >= clips.size())
        return clips;

    const Clip deleted  = clips[deleteIndex];
    const qint64 shift  = deleted.durationMs();
    const int    track  = deleted.trackIndex;

    QList<Clip> result;
    result.reserve(clips.size() - 1);

    for (int i = 0; i < clips.size(); ++i) {
        if (i == deleteIndex)
            continue;

        Clip c = clips[i];
        if (c.trackIndex == track && c.startMs >= deleted.startMs) {
            c.startMs -= shift;
            c.endMs   -= shift;
            // Clamp against negative values (safety guard)
            if (c.startMs < 0) {
                c.endMs   -= c.startMs;  // shift endMs by the same overshoot
                c.startMs  = 0;
            }
        }
        result.append(c);
    }

    return result;
}

// ---------------------------------------------------------------------------
// findOverlaps
// ---------------------------------------------------------------------------
// Return (i, j) index pairs where clips share a track and their time ranges
// overlap: a.startMs < b.endMs && b.startMs < a.endMs.
QList<QPair<int,int>> findOverlaps(const QList<Clip>& clips)
{
    QList<QPair<int,int>> result;

    for (int i = 0; i < clips.size(); ++i) {
        for (int j = i + 1; j < clips.size(); ++j) {
            const Clip& a = clips[i];
            const Clip& b = clips[j];
            if (a.trackIndex != b.trackIndex)
                continue;
            if (a.startMs < b.endMs && b.startMs < a.endMs)
                result.append(qMakePair(i, j));
        }
    }

    return result;
}

} // namespace magtl
