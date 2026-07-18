#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

class Timeline;

// Single Source Of Truth (SSOT) Timeline -> QImage renderer.
//
// The export pipeline and the GPU preview historically diverged because each
// re-implemented decode + compositing independently. This renderer is the
// shared path both will eventually call so an exported frame pixel-matches
// the preview. Story progression:
//
//   S2 (skeleton): decode the first V1 clip's frame at `usec`, scale to
//                  `outSize`.
//   S3 (this stage): multi-track compositing + per-clip transform —
//     - V1's active clip is the base layer (scaled to `outSize`); a lone V1
//       clip with a default transform stays byte-identical to S2.
//     - every upper video track (V2, V3, ...) that has a clip active at
//       `usec` is decoded, scaled to the shared canvas, and painted over the
//       base bottom-to-top (ascending track index) with that clip's
//       videoScale / videoDx / videoDy / opacity, reproducing
//       VideoPlayer::composeMultiTrackFrame's geometry + SourceOver blend
//       1:1 so the SSOT matches the authoritative preview compositor.
//
//   S6 (this stage): adjustment layers + text overlays. Over the fully
//     multi-track-composited frame, in the preview's order:
//     - adjustment-layer grade: the genuine composeAdjustmentLayersAt
//       composite (the same function GLPreview.cpp:2124-2167 calls) realised
//       on pixels via the genuine VideoEffectProcessor::applyColorCorrection
//       (the S4-proven grade-shader CPU twin);
//     - text overlays: the V1 active clip's textManager overlays baked via
//       the genuine shared baker textbake::bakeOverlays — the EXACT code
//       extracted verbatim from VideoPlayer::composeFrameWithOverlays (the
//       preview baker, which now delegates to it). It is a FREE function
//       with NO QWidget, so it is safe on the RenderQueue worker thread
//       renderFrameAt runs on (constructing a VideoPlayer there would be
//       Qt undefined behaviour).
//     Both are strict no-ops (input returned untouched) when the timeline
//     has no adjustment layer / the V1 clip has no text, so S2/S3/S4/S5 stay
//     byte-identical (MSE 0).
//
// Transitions and 2D rotation are still out of scope (the authoritative
// multi-track compositor itself applies no rotation). Any failure (no
// timeline, no V1 clip, decode error) yields a null QImage so callers can
// fall back gracefully; an upper track that fails to decode is skipped
// rather than failing the whole frame.
namespace tlrender {

QImage renderFrameAt(const Timeline *timeline, qint64 usec, QSize outSize);
QImage renderFrameAt(const Timeline *timeline, qint64 usec, QSize outSize,
                     double frameDurationUs);

namespace detail {
// Internal single-sample renderer. Public renderFrameAt overloads must route
// through this directly when motion blur is disabled.
QImage renderFrameAtSingle(const Timeline *timeline, qint64 usec, QSize outSize);

// Test-only seam (used by main.cpp's S3 parity stage). Decodes a single
// clip's frame at the given SOURCE-second position and returns it as a
// NATIVE-resolution Format_RGBA8888 QImage — the exact same libav + sws
// decode renderFrameAt uses per layer internally. Exposing it lets the
// parity selftest feed VideoPlayer::composeMultiTrackFrame (the
// authoritative comparator) byte-identical decoded frames so the measured
// MSE reflects compositing fidelity, not decode-path drift. NOT part of the
// production render API; do not call from the export/preview pipelines.
QImage decodeClipFrameNativeForTest(const QString &filePath, double sourceSec);
} // namespace detail

} // namespace tlrender
