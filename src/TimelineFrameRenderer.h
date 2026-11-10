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
struct AdjustmentLayer;
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

// US-2206: an adjustment clip active at a frame (renderFrameAt's activeAdjustments,
// also collected by MainWindow::buildSpecialClipComposite): it grades the partial
// composite at its track's z position at clip-local `localSec`.
struct ActiveAdjustmentClip {
    int trackIndex = 0;
    const ClipInfo *clip = nullptr;
    double localSec = 0.0;
};
// The adjustment clips on `trackIndex`, in order, over `input` (each through the clip's
// FX pack, keeping the input's alpha); `input` untouched when none is on that track.
QImage applyAdjustmentClipsAtTrack(const QImage &input,
                                   const QVector<ActiveAdjustmentClip> &adjustments,
                                   int trackIndex);
// The top-level adjustment-layer grade (lift / gain) at `usec`; the input untouched when
// the composite is identity, otherwise an opaque Format_RGBA8888 image.
QImage applyAdjustmentLayers(const QImage &composited,
                             const QVector<AdjustmentLayer> &layers,
                             qint64 usec);

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
// `prepare` false returns the raw sample (before prepareClipSourceForEcho) so the
// caller can put a stage between decode and VFX / grade (US-2204: V1 stabilizer).
QImage renderClipSourceFrameForEcho(const Timeline *timeline,
                                    const ClipInfo &clip,
                                    double sourceSeconds,
                                    double clipLocalSeconds,
                                    QSize outSize,
                                    qint64 timelineUsec,
                                    bool prepare = true);

// US-1707: the native layer source renderFrameAt builds for clip `clipIdx` on
// video track `trackIdx` at `usec` (roto -> VFX -> grade -> FX -> clip mask ->
// 3D text), before fit, projection and placement. Null when that clip is not
// the one the track shows at `usec` or decodes nothing. An empty `outSize`
// means the project output size.
QImage renderClipSourceAt(const Timeline *timeline, int trackIdx, int clipIdx,
                          qint64 usec, QSize outSize = QSize());
// US-1708: the stages renderClipLayerSourceStageForTest can stop at. Final is
// renderClipSourceAt; AfterBrush is the roto'd raw with the clip's brush
// strokes drawn, before VFX / grade (the image the preview uploads in its
// non-composite-baked mode).
enum class Stage { Final, AfterBrush };
QImage renderClipLayerSourceStageForTest(const Timeline *timeline, int trackIdx,
                                         int clipIdx, qint64 usec, Stage stage,
                                         QSize outSize = QSize());
// US-1708: the clip-local time export gives clip (trackIdx, clipIdx) at
// `timelineSec`: the selected overlap interval's start while the clip is shown
// inside a transition overlap, else its placement start (cursor + leadInSec
// walk; a nested-sequence parent uses the parent's own placement). When the
// track does not show the clip at `timelineSec` the value is the offset from the
// placement start clamped into [0, effectiveDuration]. False on a bad index.
bool clipLocalSecAt(const Timeline *timeline, int trackIdx, int clipIdx,
                    double timelineSec, double *localSec);
// US-2113: the (trackIdx, clipIdx) pairs the brush preview treats as active at
// `timelineSec`: export's per-track choice (overlapPairsAt + selectClipOnTrack,
// clampToFirst on V1), and inside a borrowed overlap window both the outgoing and
// the incoming clip of that track. Hidden tracks are not filtered here.
QVector<QPair<int, int>> brushActiveClipsAt(const Timeline *timeline, double timelineSec);
// US-2206: the single clip export draws on track `trackIdx` at `timelineSec`
// (overlapPairsAt + selectClipOnTrack, clampToFirst on V1: inside an overlap the
// outgoing member unless it is disabled, then the incoming one) and the start
// export measures its local time from (the selected overlap interval's start, or
// the clip's placement start). False when the track draws no clip there or on a
// bad index. Hidden tracks and disabled clips are not filtered here.
bool exportSelectedClipAt(const Timeline *timeline, int trackIdx, double timelineSec,
                          int *clipIdx, double *selectedStartSec);
// Number of clip layer sources built so far (V1, upper layers, transition
// neighbours, renderClipSourceAt), for tests.
quint64 clipLayerSourceCallsForTest();
// US-1805: the clip's brush strokes at `localSec` (clip-local seconds) drawn on
// its roto'd raw source, the layer-source brush stage (after roto, before VFX /
// grade). `clipId` is the render key (renderClipId == MainWindow::brushClipId)
// and keys the shared BrushAnimation cache. No strokes or progress 0 return
// `raw` unchanged.
QImage applyClipBrushSource(const QImage &raw, const ClipInfo &clip, double localSec,
                            const QString &clipId);
// US-1906: true when, among the clips active at `timelineSec` (one per visible
// video track, activeClipOnTrack, clampToFirst on V1), some clip carries brushData
// and none has a non-Normal blend, is an adjustment clip or has a clip mask (nor is
// a shape, nested-sequence or disabled clip, which that composite does not draw,
// nor has a keyframe track other than brush_progress, fitContain / fitCover or a
// layer style, which it places statically, nor is a parent or child while
// parenting is enabled). A transition excludes only its window: an active clip's
// own lead-in / trail-out, and the trail-out / lead-in of the clip adjacent on its
// track (any type, alignment ignored). Such a frame is previewed (paused) through
// MainWindow's special composite, which draws every clip's brush; any other frame
// keeps the GL's selected-clip brush.
bool timelineBrushWantsSpecialPath(const Timeline *timeline, double timelineSec);
// US-1805: one source frame by renderFrameAt's rule: the first frame at or after
// `sourceSec`, or with `usePreviousSourceFrame` (a reversed source position) the
// last frame before it, not before `sourceInSec`. Null on any failure.
QImage decodeClipSourceFrame(const QString &filePath, double sourceSec,
                             bool usePreviousSourceFrame, double sourceInSec);
// Dynamic bool property on the GLPreview: the frame on screen is a special
// composite that drew the brushes (US-1906: not any composite, a roto or matte one
// draws none). MainWindow sets it with each paused display and only the next
// ordinary frame clears it: a clip that decodes to no frame leaves the composite up.
inline constexpr char kSpecialCompositeHoldsBrushProperty[] = "veditorSpecialCompositeHoldsBrush";
// The clip on one track active at `targetSec` ([start, end), leadInSec walk),
// or -1; `clampToFirst` maps a request at/before t=0 onto clip 0 as
// renderFrameAt does for V1. `*clipTimelineStart` receives the clip's start.
int activeClipOnTrack(const QVector<ClipInfo> &clips, double targetSec,
                      bool clampToFirst, double *clipTimelineStart);

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
//                      the file exists and is non-empty (a zero-byte file is
//                      not decodable). A runtime decode failure (non-empty file
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
