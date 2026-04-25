#pragma once

#include <QVector>

// Tracks which clips currently hold an active hardware-decoder slot.
//
// Multi-track playback requires bounding the number of concurrent decoders
// because GPU HW-decoder sessions (D3D11VA on Windows, VideoToolbox on macOS)
// are capped per device generation. We give every active clip a slot up to
// VideoPlayer::MAX_ACTIVE_DECODERS, then evict whichever non-V1 clip is
// furthest from the playhead in seconds.
//
// V1 (track index 0) is treated as the main timeline and is never evicted —
// preserves the existing V1-wins playback semantics. If every slot is on V1
// when a new request arrives, the request is denied (returns false) so the
// caller can fall back to a software decoder or skip rendering that layer.
class DecoderSlotManager
{
public:
    struct SlotRequest {
        int clipId;
        int trackIdx;        // 0 = V1 (protected). 1+ = secondary tracks.
        double clipStartSec; // clip's timeline start time, used for distance to playhead.
    };

    // Allocates a slot for the requested clip. If the clip already has a slot,
    // its activity timestamp is refreshed and `evictedClipId` is left as -1.
    // If a slot needs to be evicted, `evictedClipId` is set to the displaced
    // clip ID and the caller is responsible for tearing down that decoder.
    // Returns true on success, false only when MAX_ACTIVE_DECODERS is exceeded
    // by V1 clips alone (caller must software-fallback or skip).
    bool requestSlot(const SlotRequest &req, int *evictedClipId);

    // Releases a previously-held slot. No-op if the clip ID is unknown.
    void releaseSlot(int clipId);

    // Update the playhead so subsequent eviction decisions use a fresh distance.
    void setPlayheadPosition(double sec);

    // Snapshot of currently-held clip IDs (in insertion order).
    QVector<int> activeClipIds() const;

    int activeCount() const { return m_slots.size(); }

    // Drop every slot. Used on timeline rebuild / project close.
    void clear();

private:
    struct Slot {
        int clipId = -1;
        int trackIdx = 0;
        double clipStartSec = 0.0;
    };

    int findEvictableIndex() const;

    QVector<Slot> m_slots;
    double m_playheadSec = 0.0;
};
