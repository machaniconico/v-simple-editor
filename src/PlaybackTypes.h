#pragma once

#include <QString>
#include <QMetaType>
#include <QVector>

// Resolved playback descriptor used to communicate the timeline schedule from
// Timeline to VideoPlayer. Independent from ClipInfo (which carries editor-side
// metadata like waveforms, effects, keyframes) so VideoPlayer stays lean and
// Timeline.h's heavy includes don't leak into the player.
struct PlaybackEntry {
    QString filePath;            // Path to the underlying media file
    double clipIn = 0.0;         // File-local start (in_point), seconds
    double clipOut = 0.0;        // File-local end (out_point or full duration), seconds
    double timelineStart = 0.0;  // Where this entry begins on the timeline, seconds
    double timelineEnd = 0.0;    // Where this entry ends on the timeline (exclusive), seconds
    double speed = 1.0;          // Playback speed multiplier (>0)
    int sourceTrack = 0;         // 0 = V1 (front, on top), 1 = V2, ... (higher = back) — V1-wins stacking
    bool audioMuted = false;     // Audio for this entry is muted (corresponding A track muted)
    // US-T35 per-clip video source transform, copied from ClipInfo at
    // Timeline::buildPlaybackEntries time. Applied by VideoPlayer when
    // the entry becomes active so each clip keeps its own scale/offset.
    double videoScale = 1.0;
    double videoDx = 0.0;
    double videoDy = 0.0;
    double opacity = 1.0;        // PiP alpha, propagated from ClipInfo::opacity
    double volume = 1.0;         // Per-clip audio gain (0.0-2.0), propagated from ClipInfo::volume
    int sourceClipIndex = -1;    // Index into TimelineTrack::m_clips
};

Q_DECLARE_METATYPE(PlaybackEntry)
Q_DECLARE_METATYPE(QVector<PlaybackEntry>)
