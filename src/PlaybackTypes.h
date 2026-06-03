#pragma once

#include <QString>
#include <QMetaType>
#include <QVector>
#include "Overlay.h"
#include "MotionStabilizer.h"
#include "color/ClipColor.h"

// Volume automation point.
//   time = clip-local TIMELINE-display seconds (0.0 == the entry's
//          timelineStart, AFTER speed/trim are applied). Range = 0..effective
//          duration. Final-Cut-style: speed changes stretch the envelope,
//          trim moves the right edge so points beyond effDur clamp to the
//          last gain. (Premiere uses source-time fixing, which we don't.)
//   gain = linear multiplier (0.0 = silent, 1.0 = unity, 2.0 = +6 dB cap).
// AudioMixer linearly interpolates between adjacent points; segments before
// the first / after the last point clamp to that point's gain.
struct AudioGainPoint {
    double time = 0.0;
    double gain = 1.0;
};

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
    double rotation2DDegrees = 0.0; // == ClipInfo::rotation2DDegrees, carried so
                                    // the compositor can place layers via the
                                    // clipgeom SSOT (rotate step) identically
                                    // to the export path.
    double opacity = 1.0;        // PiP alpha, propagated from ClipInfo::opacity
    clipcolor::ColorMeta colorMeta; // Per-clip input color metadata (Stage1 storage only)
    double volume = 1.0;         // Per-clip audio gain (0.0-2.0), propagated from ClipInfo::volume
    int sourceClipIndex = -1;    // Index into TimelineTrack::m_clips

    // STAGE4B (live GPU track-matte): carries the matte assignment from the
    // timeline to VideoPlayer so the live preview can apply it on the GPU the
    // same way the export path does. matteTypeOrdinal MIRRORS the
    // TrackMatteType enum ordinals (src/MaskSystem.h): 0=None, 1=AlphaMatte,
    // 2=AlphaInvertedMatte, 3=LumaMatte, 4=LumaInvertedMatte. Stored as a plain
    // int (NOT the enum) because PlaybackTypes.h is deliberately lean — it must
    // not pull in MaskSystem.h's QImage/QJsonObject UI weight (see the struct
    // comment above). The ordinal correspondence is enforced by a static_assert
    // in Timeline.cpp (the populating site, which already includes MaskSystem.h).
    // 0 (None) == no matte. matteSourceClipId is the trackMatteClipKey
    // ("trackIdx:clipIdx") of the matte SOURCE clip; empty == none.
    int matteTypeOrdinal = 0;
    QString matteSourceClipId;
    // Edge-attached transitions (FadeIn/FadeOut/CrossDissolve/...). Copied
    // from ClipInfo::leadIn / trailOut so VideoPlayer can window the alpha
    // (and AudioMixer the gain) over the duration without reading Timeline.
    TransitionType leadInType = TransitionType::None;
    double leadInDuration = 0.0;
    TransitionEasing leadInEasing = TransitionEasing::Linear;
    TransitionType trailOutType = TransitionType::None;
    double trailOutDuration = 0.0;
    TransitionEasing trailOutEasing = TransitionEasing::Linear;

    // Per-clip volume automation (the audio "rubber band" / pen-tool envelope
    // in pro NLEs). Empty = use static `volume` for the whole clip; non-empty
    // = AudioMixer interpolates between points using clip-local time.
    QVector<AudioGainPoint> volumeEnvelope;

    // US-INT-4: per-clip stabilizer keyframes copied from
    // ClipInfo::stabilizerKeyframes by Timeline::buildPlaybackEntries. Empty
    // = identity (no stabilization). VideoPlayer pushes the active entry's
    // vector to GLPreview so the inverse 2D affine pre-warp can run.
    QVector<StabilizerKeyframe> stabilizerKeyframes;
};

Q_DECLARE_METATYPE(AudioGainPoint)
Q_DECLARE_METATYPE(PlaybackEntry)
Q_DECLARE_METATYPE(QVector<PlaybackEntry>)
