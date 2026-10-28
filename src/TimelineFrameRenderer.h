#pragma once

#include "LayerCompositor.h"
#include "Light3D.h"

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QVector3D>
#include <QtGlobal>
#include <functional>
#include <optional>

class Timeline;
struct ClipInfo;
struct VideoEffect;
struct OverlapInterval;
struct PlaybackEntry;
namespace clipgeom { struct ClipTransform; }
namespace clipanim { struct EvalEnv; }

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
// Transitions share the preview post-overlay seam. Any failure (no
// timeline, no V1 clip, decode error) yields a null QImage so callers can
// fall back gracefully; an upper track that fails to decode is skipped
// rather than failing the whole frame.
namespace tlrender {

// Timeless source for transition-bearing stills; null for video/animation.
QImage readTransitionStillFrame(const QString &filePath);
// Place the neighbouring clip alone, without canvas overlays or adjustments.
QImage prepareTransitionLayer(QImage source, const clipgeom::ClipTransform &transform,
                              QSize canvasSize);
// Test switch also resets the calling thread's observation count.
void setTransitionStepsEnabledForTest(bool enabled);
int transitionStepCallCountForTest();
// Monotonic count of applyClipFxPack alpha-carry loop entries (translucent
// input, or a keyer in the stack); tests compare a before / after difference.
quint64 alphaCarryLoopEntryCountForTest();
QImage applyEdgeFadeStep(QImage composed, const OverlapInterval &entry, double T);
QImage applyEdgeFadeStep(QImage composed, const PlaybackEntry &entry, double T);
// Uses the outgoing interval edge parameters in OverlayRenderer::applyTransition.
QImage applyOverlapTransitionStep(QImage composed, const QImage &neighbourLayer,
                                 const OverlapInterval &entry, double T);
QImage applyOverlapTransitionStep(QImage composed, const QImage &neighbourLayer,
                                 const PlaybackEntry &entry, double T);

// Pure temporal-composite helper. `echoes[0]` is the frame at t-delay,
// `echoes[1]` at t-2*delay, and so on; each receives decay^(index+1).
// blend: 0=Add, 1=Screen, 2=Lighten, 3=Normal alpha composite.
QImage composeEcho(const QImage &base, const QVector<QImage> &echoes,
                   double decay, int blend);

using EchoFrameProvider =
    std::function<QImage(double sourceSeconds, double clipLocalSeconds)>;

// Clip mesh is applied once after the ordered FX stack, before masks/geometry.
bool hasActiveMeshWarp(const ClipInfo &clip);
void setMeshWarpDisabledForTesting(bool disabled);
void resetMeshWarpInvocationCountForTesting();
quint64 meshWarpInvocationCountForTesting();

// CPU-only temporal repair. Source and provider must have the same FX prefix.
// sourceFps=0 probes the source stream (generated sources fall back to 30 Hz).
bool hasActiveRollingShutter(const ClipInfo &clip, double clipLocalSeconds);
void setRollingShutterDisabledForTesting(bool disabled);
void resetRollingShutterInvocationCountForTesting();
quint64 rollingShutterInvocationCountForTesting();
QImage applyRollingShutterFromSource(
    const QImage &source, const VideoEffect &effect, const ClipInfo &clip,
    double clipLocalSeconds, double sourceSeconds,
    const EchoFrameProvider &frameProvider, double sourceFps = 0.0);

bool hasActiveEcho(const ClipInfo &clip, double clipLocalSeconds);
// Shared clip-local pre-Echo stage used by export and VideoPlayer. The source
// frame is processed as VFX footage (when enabled) and then receives the
// clip's effective HSL/grade/curves/LUT at `clipLocalSeconds`.
QImage prepareClipSourceForEcho(const QImage &source, const ClipInfo &clip,
                                double clipLocalSeconds);
QImage applyClipFxStackFromSource(const QImage &source, const ClipInfo &clip,
                                  double clipLocalSeconds);
QImage applyClipFxPackWithEcho(const QImage &graded, const ClipInfo &clip,
                               double clipLocalSeconds, double sourceSeconds,
                               const EchoFrameProvider &frameProvider);
// Full native clip stage for Echo/RollingShutterRepair: VFX footage controls ->
// grade/LUT -> ordered FX/Echo. Both export and preview call this function
// before mask, fit, transform, and canvas composition.
QImage applyClipFxStackWithEchoFromSource(
    const QImage &source, const ClipInfo &clip, double clipLocalSeconds,
    double sourceSeconds, const EchoFrameProvider &frameProvider);

// Random-access source-frame path shared with preview Echo. It mirrors the
// export provider's source decode/nested render -> VFX controls -> grade order.
QImage renderClipSourceFrameForEcho(const Timeline *timeline,
                                    const ClipInfo &clip,
                                    double sourceSeconds,
                                    double clipLocalSeconds,
                                    QSize outSize,
                                    qint64 timelineUsec);

// US-1707: the native layer source renderFrameAt builds for clip `clipIdx` on
// video track `trackIdx` at `usec` (roto -> VFX -> grade -> FX -> clip mask ->
// 3D text), before fit, projection and placement. Null when that clip is not
// the one the track shows at `usec` or decodes nothing. An empty `outSize`
// means the project output size.
QImage renderClipSourceAt(const Timeline *timeline, int trackIdx, int clipIdx,
                          qint64 usec, QSize outSize = QSize());
// Number of clip layer sources built so far (V1, upper layers, transition
// neighbours, renderClipSourceAt), for tests.
quint64 clipLayerSourceCallsForTest();

QImage renderFrameAt(const Timeline *timeline, qint64 usec, QSize outSize);
QImage renderFrameAt(const Timeline *timeline, qint64 usec, QSize outSize,
                     double frameDurationUs);

// US-804 PARENT-GATE-SSOT: "is video clip (trackIdx, clipIdx) composited as a
// layer (and so usable as a parent) at timeline time usec", with the times the
// renderer uses for that layer. The renderer's parent resolver calls the same
// gate. Semantics follow renderFrameAt's layer admission exactly: track 0 (V1)
// is the base layer and is never a parent; no hit when renderFrameAt returns
// an empty frame because visible V1 has no clip at usec (V1 empty without an
// active upper shape clip, or past V1's end); hidden tracks, disabled clips and
// adjustment clips yield no hit; on a borrowed overlap (transition) the
// outgoing clip wins unless disabled, and TransitionAlignment::Start's trail
// past the nominal end counts as the outgoing clip's time.
//   timelineSec      = usec / 1e6 (timeline seconds)
//   clipLocalSeconds = timelineSec - layer start (clip-local seconds)
//   sourceSeconds    = source-media seconds (clamped to the borrowed
//                      interval's clipOut inside an overlap, otherwise the
//                      clip's time-remap / inPoint + local * speed mapping)
//   decodable        = static check only: null object, sequence reference or
//                      the file exists. A runtime decode failure (file present
//                      but unreadable) still drops the layer in renderFrameAt
//                      and is NOT reported here (known limitation).
struct ParentLayerHit {
    double timelineSec = 0.0;
    double clipLocalSeconds = 0.0;
    double sourceSeconds = 0.0;
    bool decodable = false;
};
std::optional<ParentLayerHit> parentLayerAt(const Timeline *timeline, qint64 usec,
                                            int trackIdx, int clipIdx);

// US-1504: the clipanim::EvalEnv the drawing callers (renderFrameAt, GLPreview,
// VideoPlayer) evaluate the dynamic transform overlay with: the project fps and
// the project canvas Timeline::projectOutputSize() (never the output size). A
// null timeline yields an empty canvas (no wiggle offset).
clipanim::EvalEnv dynamicEvalEnv(const Timeline *timeline);

namespace detail {
// US-804 test seams (parent-gate selftest), all behind one switch that is
// OFF by default (setLayerTimeObservationForTest). While ON: the call
// counter counts every parentLayerAt evaluation, including the renderer's
// own parent resolver, and (track, clip, local, source) is recorded for each
// upper-track layer renderFrameAt admits. While OFF neither is updated.
struct ObservedLayerTime {
    int trackIdx = -1;
    int clipIdx = -1;
    double localSec = 0.0;
    double sourceSec = 0.0;
};
void resetParentLayerAtCallCountForTest();
quint64 parentLayerAtCallCountForTest();
void setLayerTimeObservationForTest(bool enabled);
QVector<ObservedLayerTime> observedLayerTimesForTest();

// Internal single-sample renderer. Public renderFrameAt overloads must route
// through this directly when motion blur is disabled.
QImage renderFrameAtSingle(const Timeline *timeline, qint64 usec, QSize outSize);

// Apply project lighting to already-placed layer images. `positionOffsets`
// are pixel offsets from the canvas centre and are intentionally separate
// from CompositeLayer::position, whose normalised form is used by clipgeom.
// The helper is shared by the export renderer and the preview parity seam.
QVector<QImage> applyLightingToLayerImages(
    const QVector<CompositeLayer> &layers,
    const QVector<QImage> &layerImages,
    const QSize &canvasSize,
    const QVector<QPointF> &positionOffsets,
    const QVector<Light3DState> &lights,
    const QVector3D &viewPos);

// Selftest-only seam proof. The renderer calls the lighting seam even when
// the evaluated light list is empty/disabled; the helper remains a strict
// no-op in that case. This proves the no-light byte-identity gate exercises
// the wired export path rather than only a detached helper call.
// Observation is OFF by default and toggled explicitly (not latched from an
// environment variable, which made the result depend on sweep order).
void setLightingSelftestObservation(bool enabled);
void resetLightingSelftestObservation();
bool lightingSelftestSeamWasCalled();

// Test-only seam (used by main.cpp's S3 parity stage). Decodes a single
// clip's frame at the given SOURCE-second position and returns it as a
// NATIVE-resolution Format_RGBA8888 QImage — the exact same libav + sws
// decode renderFrameAt uses per layer internally. Exposing it lets the
// parity selftest feed VideoPlayer::composeMultiTrackFrame (the
// authoritative comparator) byte-identical decoded frames so the measured
// MSE reflects compositing fidelity, not decode-path drift. NOT part of the
// production render API; do not call from the export/preview pipelines.
QImage decodeClipFrameNativeForTest(const QString &filePath, double sourceSec,
                                   bool usePreviousSourceFrame = false,
                                   double sourceInSec = 0.0);
} // namespace detail

} // namespace tlrender
