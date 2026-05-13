#pragma once
#include <QList>
#include <QPair>
#include <QString>

namespace magtl {

struct Clip {
    int     trackIndex = 0;   // which track (V1/V2/A1 ...)
    qint64  startMs    = 0;   // start time (ms), inclusive
    qint64  endMs      = 0;   // end time (ms), exclusive
    QString id;               // optional identifier for round-trip

    qint64 durationMs() const { return endMs - startMs; }
    bool   isValid()    const { return endMs > startMs && trackIndex >= 0; }
};

// Collapse gaps between adjacent clips on each track.
// Any gap larger than minGapMs is removed by shifting later clips forward.
QList<Clip> closeGaps(const QList<Clip>& clips, qint64 minGapMs = 0);

// Insert newClip on its track, pushing all clips that start at or after
// newClip.startMs rightward by newClip.durationMs(). Other tracks untouched.
QList<Clip> rippleInsert(const QList<Clip>& clips, const Clip& newClip);

// Remove the clip at deleteIndex, then pull all later clips on the same track
// forward by the deleted clip's duration. Returns clips unchanged if index is out of range.
QList<Clip> rippleDelete(const QList<Clip>& clips, int deleteIndex);

// Return (i, j) pairs where clips i and j share a track and their time ranges overlap.
QList<QPair<int,int>> findOverlaps(const QList<Clip>& clips);

} // namespace magtl
