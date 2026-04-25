#include "DecoderSlotManager.h"
#include "VideoPlayer.h"

#include <cmath>

bool DecoderSlotManager::requestSlot(const SlotRequest &req, int *evictedClipId)
{
    if (evictedClipId)
        *evictedClipId = -1;

    // Refresh path: clip already owns a slot.
    for (auto &slot : m_slots) {
        if (slot.clipId == req.clipId) {
            slot.clipStartSec = req.clipStartSec;
            slot.trackIdx = req.trackIdx;
            return true;
        }
    }

    // Free slot available — append.
    if (m_slots.size() < VideoPlayer::MAX_ACTIVE_DECODERS) {
        Slot slot;
        slot.clipId = req.clipId;
        slot.trackIdx = req.trackIdx;
        slot.clipStartSec = req.clipStartSec;
        m_slots.append(slot);
        return true;
    }

    // Eviction path: find the furthest non-V1 slot.
    const int evictIdx = findEvictableIndex();
    if (evictIdx < 0) {
        // Every slot is on V1 — protected. Caller must handle the denial.
        return false;
    }

    if (evictedClipId)
        *evictedClipId = m_slots[evictIdx].clipId;

    Slot &replacement = m_slots[evictIdx];
    replacement.clipId = req.clipId;
    replacement.trackIdx = req.trackIdx;
    replacement.clipStartSec = req.clipStartSec;
    return true;
}

void DecoderSlotManager::releaseSlot(int clipId)
{
    for (int i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].clipId == clipId) {
            m_slots.removeAt(i);
            return;
        }
    }
}

void DecoderSlotManager::setPlayheadPosition(double sec)
{
    m_playheadSec = sec;
}

QVector<int> DecoderSlotManager::activeClipIds() const
{
    QVector<int> out;
    out.reserve(m_slots.size());
    for (const auto &slot : m_slots)
        out.append(slot.clipId);
    return out;
}

void DecoderSlotManager::clear()
{
    m_slots.clear();
}

int DecoderSlotManager::findEvictableIndex() const
{
    int bestIdx = -1;
    double bestDistance = -1.0;
    for (int i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].trackIdx == 0)
            continue; // V1 is protected.
        const double dist = std::abs(m_slots[i].clipStartSec - m_playheadSec);
        if (dist > bestDistance) {
            bestDistance = dist;
            bestIdx = i;
        }
    }
    return bestIdx;
}
