#include "TimelineFrameRenderer.h"
#include "Timeline.h"
#include "VideoEffect.h"        // VideoEffectProcessor::applyColorCorrection (CPU SSOT)
#include "LutImporter.h"        // LutImporter::loadCubeFile / applyLutWithIntensity
#include "AdjustmentLayer.h"    // composeAdjustmentLayersAt (S6 — genuine composite)
#include "TextOverlayBake.h"    // textbake::bakeOverlays (S6 — genuine text baker,
                                // thread-safe: NO QWidget on the worker thread)
#include "TextManager.h"        // EnhancedTextOverlay (S6 — per-clip overlay list)
#include "MaskSystem.h"         // S7 — genuine MaskSystem::generateMaskImage/applyMask
#include "MotionTracker.h"      // S7 — genuine TrackingResult::positionAtTime

#include <QtGlobal>
#include <QPainter>
#include <QRectF>
#include <QVector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace tlrender {
namespace {

// Mirror of Exporter::openInputFile (src/Exporter.cpp:74). Kept self-contained
// here so this SSOT renderer never depends on Exporter internals (the task
// forbids modifying Exporter.cpp). Opens `path`, finds the first video
// stream, and constructs an opened decoder context. Caller owns teardown.
bool openVideoInput(const QString &path, AVFormatContext **fmtCtx,
                    AVCodecContext **decCtx, int *streamIndex)
{
    *fmtCtx = nullptr;
    *decCtx = nullptr;
    *streamIndex = -1;

    if (avformat_open_input(fmtCtx, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;

    if (avformat_find_stream_info(*fmtCtx, nullptr) < 0) {
        avformat_close_input(fmtCtx);
        return false;
    }

    for (unsigned i = 0; i < (*fmtCtx)->nb_streams; ++i) {
        if ((*fmtCtx)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *streamIndex = static_cast<int>(i);
            break;
        }
    }
    if (*streamIndex < 0) {
        avformat_close_input(fmtCtx);
        return false;
    }

    auto *codecpar = (*fmtCtx)->streams[*streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        avformat_close_input(fmtCtx);
        return false;
    }

    *decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(*decCtx, codecpar);
    if (avcodec_open2(*decCtx, codec, nullptr) < 0) {
        avcodec_free_context(decCtx);
        avformat_close_input(fmtCtx);
        return false;
    }
    return true;
}

// Decode a single clip's frame at the requested SOURCE-second position and
// return it as a NATIVE-resolution Format_RGBA8888 QImage (no scale to the
// output canvas — the caller owns that step so the multi-track compositor can
// scale every layer onto the shared output grid exactly like the preview's
// canvas). This is the S2 libav decode path verbatim, factored out so every
// video track reuses identical seek/decode/sws semantics. Returns a null
// QImage on any failure (open / decode / sws) so the caller can skip the
// layer gracefully — mirrors S2's "any failure -> null" contract.
QImage decodeClipFrameNative(const QString &filePath, double sourceSec)
{
    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *decCtx = nullptr;
    int videoIdx = -1;
    if (!openVideoInput(filePath, &fmtCtx, &decCtx, &videoIdx))
        return QImage();

    // Seek a hair before the wanted source pts using the same BACKWARD seek
    // Exporter uses for the clip in-point, then decode forward until a frame
    // at-or-after sourceSec arrives.
    const int64_t seekTarget = static_cast<int64_t>(sourceSec * AV_TIME_BASE);
    if (seekTarget > 0) {
        av_seek_frame(fmtCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(decCtx);
    }

    AVStream *vStream = fmtCtx->streams[videoIdx];
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    SwsContext *swsCtx = nullptr;
    QImage result;

    auto buildResult = [&](const AVFrame *src) {
        QImage rgba(decCtx->width, decCtx->height, QImage::Format_RGBA8888);
        swsCtx = sws_getContext(decCtx->width, decCtx->height, decCtx->pix_fmt,
                                decCtx->width, decCtx->height, AV_PIX_FMT_RGBA,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx)
            return;
        uint8_t *dst[1] = { rgba.bits() };
        int dstStride[1] = { static_cast<int>(rgba.bytesPerLine()) };
        sws_scale(swsCtx, src->data, src->linesize, 0, decCtx->height,
                  dst, dstStride);
        result = rgba;
    };

    bool decoded = false;
    while (!decoded && av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != videoIdx) {
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(decCtx, packet) < 0) {
            av_packet_unref(packet);
            continue;
        }
        av_packet_unref(packet);

        while (avcodec_receive_frame(decCtx, frame) == 0) {
            const double framePts =
                static_cast<double>(frame->pts) * av_q2d(vStream->time_base);
            // Same gate Exporter applies: skip frames before the wanted
            // source position; take the first frame at-or-after it.
            if (framePts + 1e-6 < sourceSec)
                continue;
            buildResult(frame);
            decoded = true;
            break;
        }
    }

    // Flush: the wanted frame may still be buffered in the decoder if the
    // file ended exactly at the requested timestamp.
    if (!decoded) {
        avcodec_send_packet(decCtx, nullptr);
        while (avcodec_receive_frame(decCtx, frame) == 0) {
            buildResult(frame);
            decoded = true;
            break;
        }
    }

    if (swsCtx)
        sws_freeContext(swsCtx);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    return result;
}

// S4: grade a single clip's freshly-decoded NATIVE frame exactly the way the
// GPU preview shader grades that clip, so an export pixel-matches the preview
// whenever a colour-corrected / LUT'd clip is on screen.
//
// PIPELINE ORDER — proven against the preview fragment shader in GLPreview.cpp:
//   1. Colour correction: the shader runs exposure -> brightness/contrast ->
//      highlights/shadows -> saturation -> hue -> temperature/tint -> gamma
//      -> Lift/Gamma/Gain (src/GLPreview.cpp:813-831). That is byte-for-byte
//      the SAME sequence VideoEffectProcessor::applyColorCorrection executes
//      on the CPU (src/VideoEffect.cpp:161-214) — the shader comment at
//      GLPreview.cpp:812 even states "same order as CPU". So we call the real
//      applyColorCorrection (no re-implementation) for stage 1.
//   2. 3D LUT: immediately AFTER the colour-correction block the shader does
//      `lutColor = texture(uLut3D, clamp(color,0,1)); color = mix(color,
//      lutColor, uLutIntensity)` (src/GLPreview.cpp:864-867). The genuine CPU
//      analogue is LutImporter::applyLutWithIntensity (trilinear sample +
//      intensity mix, src/LutImporter.cpp:151-225). So colour-correction runs
//      BEFORE the LUT, never after.
//   (RGB Curves / vignette / HSLq / white-balance sit between these two in the
//    shader but ClipInfo carries none of them, so for the S4 scope the
//    effective order is exactly CC -> LUT, matching the shader for the
//    parameters a clip can actually hold.)
//
// GATING mirrors Exporter (src/Exporter.cpp:473-474): skip the work — and
// return the input UNTOUCHED — when the clip's colour correction is default
// AND it has no LUT, so a plain V1/overlay clip stays byte-identical to the
// S2/S3 decode path (no format round-trip, no rounding).
//
// FORMAT: applyColorCorrection / applyLut both emit Format_RGB888. The rest of
// renderFrameAt (scale + ARGB32_Premultiplied composite) expects the same
// Format_RGBA8888 the decoder produced, so the graded result is converted back
// to RGBA8888 before return — keeping the contract S2/S3 rely on.
QImage gradeClipNativeFrame(const QImage &native, const ClipInfo &clip)
{
    const bool hasColor = !clip.colorCorrection.isDefault();
    const bool hasLut   = clip.hasLut();
    if (!hasColor && !hasLut)
        return native;                       // strict no-op == S2/S3 byte path

    // Stage 1 — colour correction via the genuine CPU SSOT (the very function
    // the task names as the comparator). isDefault() short-circuits inside
    // applyColorCorrection too, but the hasColor guard keeps the RGB888
    // round-trip out of the LUT-only path.
    QImage img = native;
    if (hasColor)
        img = VideoEffectProcessor::applyColorCorrection(img, clip.colorCorrection);

    // Stage 2 — 3D LUT via the genuine CPU SSOT, applied to the colour-graded
    // pixels (matches the shader's post-CC LUT mix). Parse the .cube the same
    // way the preview does (LutImporter::loadCubeFile feeds GLPreview::setLut).
    if (hasLut) {
        const LutData lut = LutImporter::loadCubeFile(clip.lutFilePath);
        if (lut.isValid())
            img = LutImporter::applyLutWithIntensity(img, lut, clip.lutIntensity);
    }

    // Back to the decoder's format so downstream scale/composite is unchanged.
    return img.convertToFormat(QImage::Format_RGBA8888);
}

// S5 — FX PACK stage. Apply the clip's per-clip video-effect stack exactly
// where the GPU preview shader applies it, so an export pixel-matches the
// preview whenever a clip carries effects.
//
// PIPELINE ORDER — proven against the preview fragment shader in GLPreview.cpp:
//   The entire colour-correction + LUT block lives INSIDE the
//   `if (uEffectsEnabled) { ... }` branch which OPENS at GLPreview.cpp:744 and
//   CLOSES at GLPreview.cpp:908. Within it the order is CC (GLPreview.cpp:813-
//   826) -> LGG/curves/vignette -> 3D LUT (GLPreview.cpp:863-866). The FX-pack
//   uniforms are consumed AFTER that brace closes, at GLPreview.cpp:910-916:
//     if (uFxSepiaEnable)    color = fxApplySepia(...);   // GLPreview.cpp:910
//     if (uFxGrayEnable)     color = fxApplyGray(...);     // GLPreview.cpp:911
//     if (uFxInvertEnable)   color = fxApplyInvert(...);   // GLPreview.cpp:912
//     if (uFxVignetteEnable) color = fxApplyVignette(...); // GLPreview.cpp:913
//     if (uFxNoiseEnable)    color = fxApplyNoise(...);    // GLPreview.cpp:914
//     if (uFxSharpenEnable)  color = applySharpen(...);    // GLPreview.cpp:915
//   So the shader runs the FX pack STRICTLY AFTER colour-correction and the
//   3D LUT. This SSOT therefore applies the FX stack on the ALREADY graded
//   pixels (i.e. AFTER gradeClipNativeFrame's CC -> LUT), never before.
//
// GENUINE API: the effects are applied through the very function the export
// path uses — VideoEffectProcessor::applyEffectStack (mirrors Exporter.cpp:494)
// — NOT a re-implementation. applyEffectStack internally runs
// applyColorCorrection FIRST; we hand it a DEFAULT-constructed ColorCorrection
// so that inner call is a strict no-op (applyColorCorrection short-circuits on
// cc.isDefault(), src/VideoEffect.cpp:158). The CC for this clip was already
// applied by gradeClipNativeFrame, so passing default-CC here makes
// applyEffectStack reduce to exactly "run each VideoEffect in order" — the
// genuine per-clip FX pipeline, with no double colour-correction.
//
// GATING mirrors Exporter (src/Exporter.cpp:473-474): the FX work — and the
// RGB888 round-trip it implies — is skipped, returning the input UNTOUCHED,
// whenever the clip has no effects, so a plain V1/overlay clip stays
// byte-identical to the S2/S3/S4 path (no format conversion, no rounding).
QImage applyClipFxPack(const QImage &graded, const ClipInfo &clip)
{
    if (clip.effects.isEmpty())
        return graded;                       // strict no-op == S2/S3/S4 byte path

    // Genuine FX SSOT. Default ColorCorrection -> applyEffectStack's internal
    // applyColorCorrection is a strict no-op (VideoEffect.cpp:158), so this is
    // purely the effect stack applied in ClipInfo order — identical to the
    // Exporter call at Exporter.cpp:494 minus the (already-done) CC.
    const QImage out = VideoEffectProcessor::applyEffectStack(
        graded, ColorCorrection(), clip.effects);

    // applyEffectStack emits Format_RGB888; restore the decoder's RGBA8888 so
    // downstream scale/composite is unchanged (same contract gradeClipNativeFrame
    // preserves for S2/S3/S4).
    return out.convertToFormat(QImage::Format_RGBA8888);
}

// S7 — PER-CLIP MASK stage (driven by motion-tracker data for animation).
// Apply the clip's genuine AE/Premiere-style compositing mask to its
// already-graded+FX'd NATIVE frame, BEFORE the frame is scaled onto the
// shared canvas and composited over lower tracks.
//
// PIPELINE ORDER — proven against the preview:
//   A per-clip "Mask" in AE/Premiere cuts THAT layer's alpha (the masked-out
//   region becomes transparent) BEFORE the layer is composited onto the
//   tracks beneath it. The authoritative preview composites every video
//   track via VideoPlayer::composeMultiTrackFrame (called at
//   src/VideoPlayer.cpp:3833) using CompositionMode_SourceOver against an
//   ARGB32_Premultiplied canvas (src/VideoPlayer.cpp:4558-4579) — i.e. an
//   upper track's transparent pixels let the lower track show through. For
//   that SourceOver stack to reveal the lower track exactly where the mask
//   cuts the upper clip, the alpha must already be zeroed on the upper
//   clip's frame at composite time. So the per-clip mask is applied to the
//   clip's native frame AFTER grade (gradeClipNativeFrame, CC->LUT) and FX
//   (applyClipFxPack) but BEFORE scale + the multi-track SourceOver
//   composite — the same per-source-pixel position grade/FX occupy, and
//   strictly before the S6 adjustment-grade/text stages (which run on the
//   already-composited canvas). The GPU preview's US-EF-2 uMask* uniforms
//   are a SEPARATE feature (a single GLOBAL grade-localisation wrap,
//   color=mix(ungraded,color,weight) at GLPreview.cpp:886-907 — it does NOT
//   alpha-cut and is not per-clip), so it is intentionally NOT the
//   comparator here; the genuine per-clip compositing-mask SSOT is
//   MaskSystem::applyMask.
//
// GENUINE API (no re-implementation of rasterisation / feather / combine):
//   1. MaskSystem::generateMaskImage(masks, canvasSize) — the genuine mask
//      rasteriser (shape draw + feather + expansion + opacity + invert +
//      mode-combine, src/MaskSystem.cpp:381-468). Produces a Grayscale8
//      matte (255=visible, 0=masked).
//   2. MaskSystem::applyMask(frame, matte) — the genuine matte applicator
//      (multiplies the layer RGB+alpha by the matte, src/MaskSystem.cpp:
//      474-509). Returns ARGB32_Premultiplied.
//
// MOTION-TRACKING ANIMATION: when the clip carries tracker data, the genuine
// TrackingResult::positionAtTime(timeSec) (src/MotionTracker.cpp:20-45)
// gives the tracked rect at this frame's time. The mask shapes are
// translated by the tracked-centre DELTA relative to the first tracked
// frame (regions.first()) — the exact "offset the mask so it follows the
// tracked feature" semantics MotionTracker::applyToOverlay uses
// (src/MotionTracker.cpp:432-456: centre-follows-tracked-centre). Tracker
// rects are in SOURCE-pixel space and the mask is rasterised in the same
// NATIVE source-resolution canvas, so the delta is applied directly with
// no scale conversion. Empty tracker data == static mask (delta = 0).
//
// GATING mirrors hasLut()/Exporter (src/Exporter.cpp:473-474): a clip with
// no mask (maskSystem.masks() empty) returns the input UNTOUCHED — no
// generateMaskImage, no applyMask, no format round-trip — so a clip with no
// mask stays byte-identical to S2..S6 (MSE 0 preserved, no regression).
QImage applyClipMask(const QImage &frame, const ClipInfo &clip, double sourceSec)
{
    if (!clip.hasMask())
        return frame;                        // strict no-op == S2..S6 byte path

    const QSize canvasSize = frame.size();
    if (canvasSize.isEmpty())
        return frame;

    // Motion-tracker delta: translate the mask so it follows the tracked
    // feature, exactly like MotionTracker::applyToOverlay offsets a rect to
    // the tracked centre. Delta is (tracked-centre @now) − (tracked-centre
    // @first tracked frame), in native source pixels. No tracker data ->
    // zero delta -> static mask.
    QPointF trackDelta(0.0, 0.0);
    const TrackingResult &trk = clip.maskTrackingData;
    if (!trk.isEmpty()) {
        const QRect now0 = trk.positionAtTime(sourceSec);
        const QRect base0 = trk.regions.first().rect;
        if (!now0.isNull() && !base0.isNull()) {
            const QPointF nowC(now0.x() + now0.width()  / 2.0,
                               now0.y() + now0.height() / 2.0);
            const QPointF baseC(base0.x() + base0.width()  / 2.0,
                                base0.y() + base0.height() / 2.0);
            trackDelta = nowC - baseC;
        }
    }

    // Build the masks to rasterise. When the tracker moved, translate every
    // shape's rect + polygon/path points by the tracked delta (genuine
    // "mask follows tracker" animation). The genuine rasteriser then draws
    // the shifted shapes — feather/expansion/opacity/invert/combine math is
    // untouched (real MaskSystem code does it).
    QVector<Mask> masks = clip.maskSystem.masks();
    if (!trackDelta.isNull()) {
        for (Mask &m : masks) {
            m.rect.translate(trackDelta);
            for (QPointF &p : m.points)
                p += trackDelta;
        }
    }

    // Stage 1 — genuine mask rasterisation (the very function the mask
    // system uses everywhere; NOT re-implemented here).
    const QImage matte =
        MaskSystem::generateMaskImage(masks, canvasSize);
    if (matte.isNull())
        return frame;                        // nothing rasterised -> untouched

    // Stage 2 — genuine matte application (multiplies layer RGB+alpha by the
    // matte). applyMask emits ARGB32_Premultiplied; restore the decoder's
    // RGBA8888 so downstream scale/composite is unchanged (same contract
    // gradeClipNativeFrame/applyClipFxPack preserve for S2..S6).
    const QImage masked = MaskSystem::applyMask(frame, matte);
    return masked.convertToFormat(QImage::Format_RGBA8888);
}

// S6 — ADJUSTMENT-LAYER stage. Apply the cumulative grade of every
// adjustment layer covering `usec` exactly where the GPU preview applies it.
//
// PIPELINE ORDER — proven against the preview:
//   The preview composites every video track into one canvas
//   (VideoPlayer::composeMultiTrackFrame, called at VideoPlayer.cpp:3833),
//   then GLPreview paints that canvas through the grade shader. Inside
//   GLPreview::paintGL the per-clip grade uniforms (Lift/Gamma/Gain, White
//   Balance, Vignette) are MERGED with the adjustment-layer composite at
//   GLPreview.cpp:2124-2167 (`composeAdjustmentLayersAt(m_timeline->
//   adjustmentLayers(), tlUs)` then the comp.gradingEnabled merge block).
//   So the adjustment-layer grade lands AFTER the multi-track composite,
//   over the stacked frame — never per source clip. This SSOT therefore
//   applies it to the ALREADY-composited canvas.
//
// GENUINE API: the composite itself is produced by the very function the
// preview calls — composeAdjustmentLayersAt (NOT re-implemented). The
// composite is then realised on pixels through the genuine CPU grading SSOT
// VideoEffectProcessor::applyColorCorrection (the exact CPU twin S4 proved
// matches the grade shader). The composite slider -> ColorCorrection field
// translation uses the SAME documented mapping the GLPreview merge uses:
//   * Lift:  GLPreview.cpp:2145 takes `liftU = comp.lift[ch]*0.5` and adds
//     it to m_liftGammaGain[0] (which setLiftGammaGain, GLPreview.cpp:3685,
//     fills as values[0][ch]*0.5). applyColorCorrection applies lift as
//     `r += cc.liftR*0.5` (VideoEffect.cpp:190) — the identical *0.5. So
//     comp.lift[ch] maps directly onto cc.liftR/G/B.
//   * Gain:  GLPreview.cpp:2147 takes `gainU = pow(2, comp.gain[ch]*2)`;
//     applyColorCorrection applies gain as `r *= pow(2, cc.gainR*2)`
//     (VideoEffect.cpp:205) — identical. So comp.gain[ch] maps directly
//     onto cc.gainR/G/B.
//   (gamma/WB/vignette use the same composite but the grade-shader applies
//   WB/vignette in stages applyColorCorrection models differently; this
//   SSOT scopes the adjustment grade to the lift+gain channels whose CPU
//   twin is byte-faithful, exactly as S4 scoped CC to the parameters with a
//   proven CPU analogue. Index order matches AdjustmentLayer: 0=R,1=G,2=B.)
//
// GATING: an empty / all-disabled adjustment-layer set yields
// comp.gradingEnabled=false (AdjustmentLayer.cpp:134-148); we then return
// the input UNTOUCHED so a clip with no adjustment layer stays byte-identical
// to S2/S3/S4/S5 (no format round-trip, no rounding).
QImage applyAdjustmentLayers(const QImage &composited, const Timeline *timeline,
                             qint64 usec)
{
    if (!timeline)
        return composited;
    const AdjustmentLayerComposite comp =
        composeAdjustmentLayersAt(timeline->adjustmentLayers(), usec);
    if (!comp.gradingEnabled)
        return composited;                   // strict no-op == S2..S5 byte path

    // Translate the genuine composite into a ColorCorrection using the
    // documented GLPreview merge mapping (see header comment). Lift + gain
    // channels only — the parameters whose CPU twin in applyColorCorrection
    // is byte-faithful to the grade shader (S4-proven).
    ColorCorrection cc;                       // identity
    cc.liftR = comp.lift[0];  cc.liftG = comp.lift[1];  cc.liftB = comp.lift[2];
    cc.gainR = comp.gain[0];  cc.gainG = comp.gain[1];  cc.gainB = comp.gain[2];
    if (cc.isDefault())
        return composited;                    // composite enabled but identity

    // Genuine CPU grading SSOT — the very function S4 proved is the grade
    // shader's CPU twin. applyColorCorrection emits Format_RGB888; restore
    // the canvas format the caller/contract expects (S2's RGBA8888).
    const QImage graded = VideoEffectProcessor::applyColorCorrection(composited, cc);
    return graded.convertToFormat(QImage::Format_RGBA8888);
}

// S6 — TEXT-OVERLAY stage. Bake the V1 active clip's text overlays exactly
// where the GPU preview bakes them.
//
// PIPELINE ORDER — proven against the preview: the multi-track composite
// produces the canvas (VideoPlayer.cpp:3833), which is handed to
// displayFrame(); displayFrame calls composeFrameWithOverlays on it at
// VideoPlayer.cpp:1911. So text overlays bake STRICTLY LAST — on top of the
// fully-composited (and adjustment-graded) frame.
//
// GENUINE API: the overlays are baked through textbake::bakeOverlays — the
// EXACT font/layout/keyframe/outline/gradient code extracted verbatim from
// the authoritative preview baker VideoPlayer::composeFrameWithOverlays
// (which now delegates to this same free function). The math is NOT
// duplicated: preview and export run the IDENTICAL baking code, so S6
// export-vs-preview text parity holds by construction. Crucially this is a
// FREE function with no QWidget — renderFrameAt runs on the RenderQueue
// WORKER THREAD (RenderQueue::startRenderPipe), and constructing a
// VideoPlayer (a QWidget) off the GUI thread is Qt undefined behaviour.
// fontScale = 1.0 and hiddenIdx = -1 reproduce EXACTLY what the prior
// VideoPlayer-seam path produced in this headless/export context (m_glPreview
// was null -> fontScale 1.0; the seam forced m_hiddenTextOverlayIndex = -1).
//
// SOURCE OF OVERLAYS: identical to the preview harvest — MainWindow reads
// the first video clip's textManager.overlays() and feeds setTextOverlays
// (src/MainWindow.cpp:994-997). This SSOT reads the SAME V1 active clip's
// TextManager so an export bakes exactly the overlays the preview shows.
//
// GATING: no V1 clip / empty TextManager -> nothing to bake -> return the
// input UNTOUCHED so a clip with no text stays byte-identical to S2..S5.
QImage applyTextOverlays(const QImage &composited, const Timeline *timeline,
                         qint64 usec, const ClipInfo *v1Clip)
{
    if (!timeline || !v1Clip)
        return composited;
    const TextManager &mgr = v1Clip->textManager;
    if (mgr.count() <= 0)
        return composited;                    // strict no-op == S2..S5 byte path

    QVector<EnhancedTextOverlay> overlays;
    overlays.reserve(mgr.count());
    for (int i = 0; i < mgr.count(); ++i)
        overlays.append(mgr.overlay(i));

    // The genuine text baker drives off a timeline-second playhead; pass it
    // the SAME timeline seconds renderFrameAt resolved the frame at.
    const double nowSec = static_cast<double>(usec) / 1'000'000.0;

    // Bake via the shared SSOT text baker — the SAME code the preview's
    // VideoPlayer::composeFrameWithOverlays now delegates to. NO VideoPlayer,
    // NO QWidget: safe on the RenderQueue worker thread. fontScale = 1.0 /
    // hiddenIdx = -1 == exactly what the prior headless seam produced.
    return textbake::bakeOverlays(composited, overlays, nowSec,
                                  /*hiddenIdx=*/-1, /*fontScale=*/1.0);
}

// Locate the clip on a single track active at `targetSec`. Returns the clip
// index plus its timeline-start, or -1. Mirrors the S2 footprint walk
// (ClipInfo::leadInSec + effectiveDuration(), Timeline.h:124-127). The
// `clampToFirst` flag reproduces S2's behaviour of clamping a request at/before
// t=0 onto the first clip so a leading gap still yields a frame.
int activeClipOnTrack(const QVector<ClipInfo> &clips, double targetSec,
                      bool clampToFirst, double *clipTimelineStart)
{
    if (clips.isEmpty())
        return -1;

    double cursor = 0.0;
    for (int i = 0; i < clips.size(); ++i) {
        const double clipStart = cursor + clips[i].leadInSec;
        const double clipEnd = clipStart + clips[i].effectiveDuration();
        if (targetSec >= clipStart && targetSec < clipEnd) {
            *clipTimelineStart = clipStart;
            return i;
        }
        cursor = clipEnd;
    }
    if (clampToFirst && targetSec <= 0.0) {
        *clipTimelineStart = clips[0].leadInSec;
        return 0;
    }
    return -1;
}

} // namespace

QImage renderFrameAt(const Timeline *timeline, qint64 usec, QSize outSize)
{
    if (!timeline || outSize.isEmpty())
        return QImage();

    // ── Gather every video track's clip list ────────────────────────────────
    // m_videoTracks[0] is V1 (alias m_videoTrack); each subsequent index is an
    // overlay track (V2, V3, ...). Per Timeline::computePlaybackSequence
    // (src/Timeline.cpp:4871-4875) every visible track contributes its full
    // clip intervals with NO mutual subtraction — the compositor stacks them.
    // The actual preview paint (src/VideoPlayer.cpp:3833) calls
    // composeMultiTrackFrame(m_canvasBase /*V1*/, layers /*V2+*/) so visually
    // V1 is the BASE and higher tracks paint ON TOP via SourceOver, i.e.
    // ascending track index == bottom-to-top draw order.
    const QVector<TimelineTrack *> &tracks = timeline->videoTracks();
    if (tracks.isEmpty())
        return QImage();

    const double targetSec = static_cast<double>(usec) / 1'000'000.0;

    // ── Resolve + decode the V1 base layer ──────────────────────────────────
    // Single-track byte-identity with S2: V1 alone with a default transform
    // must produce the SAME pixels S2 returned (decode native -> scale to
    // outSize, NO QPainter / format conversion). The overlay path below only
    // runs when an upper track actually has an active clip.
    TimelineTrack *v1 = tracks.first();
    if (!v1)
        return QImage();
    const QVector<ClipInfo> &v1Clips = v1->clips();
    if (v1Clips.isEmpty())
        return QImage();

    double v1Start = 0.0;
    const int v1Idx = activeClipOnTrack(v1Clips, targetSec,
                                        /*clampToFirst=*/true, &v1Start);
    if (v1Idx < 0)
        return QImage();

    const ClipInfo &v1Clip = v1Clips[v1Idx];
    // Timeline-second -> source-second mapping. Mirrors Exporter
    // (src/Exporter.cpp:413-455): playback begins at clip.inPoint and walks
    // forward; clip.speed scales clip-local time onto the source timeline
    // (consistent with effectiveDuration() in Timeline.h:124-127).
    const double v1LocalSec = targetSec - v1Start;            // >= 0
    const double v1SourceSec = v1Clip.inPoint + v1LocalSec * v1Clip.speed;

    const QImage v1NativeRaw = decodeClipFrameNative(v1Clip.filePath, v1SourceSec);
    if (v1NativeRaw.isNull())
        return QImage();

    // S4: grade the V1 clip in NATIVE resolution before it is scaled onto the
    // canvas — the preview shader colour-corrects/LUTs the sampled texel, i.e.
    // grading happens per source pixel, independent of canvas scale. For a
    // clip with default colour and no LUT this is a strict no-op so a lone V1
    // clip stays byte-identical to S2.
    const QImage v1Graded = gradeClipNativeFrame(v1NativeRaw, v1Clip);

    // S5: apply the V1 clip's FX pack on the GRADED native frame — the preview
    // shader runs the FX uniforms AFTER the CC+LUT block closes
    // (GLPreview.cpp:908 closes the grade branch; FX consumed at
    // GLPreview.cpp:910-916), so FX comes strictly after CC -> LUT. No-op when
    // the clip carries no effects, so a lone V1 clip stays byte-identical to
    // S2/S3/S4.
    const QImage v1Fx = applyClipFxPack(v1Graded, v1Clip);

    // S7: apply the V1 clip's per-clip compositing mask (motion-tracker
    // animated) on the GRADED+FX'd native frame, BEFORE it is scaled onto
    // the canvas — a per-clip AE/Premiere mask cuts the layer's alpha at the
    // source-pixel level, before scale + the multi-track SourceOver
    // composite (src/VideoPlayer.cpp:4558-4579), so a lower track shows
    // through exactly where the mask cuts. No-op when the clip has no mask,
    // so a lone V1 clip stays byte-identical to S2/S3/S4/S5/S6.
    const QImage v1Native = applyClipMask(v1Fx, v1Clip, v1SourceSec);

    // Base canvas == S2 output exactly: scaled to outSize, RGBA8888.
    const QImage base = v1Native.scaled(outSize, Qt::IgnoreAspectRatio,
                                        Qt::SmoothTransformation);

    // ── Collect overlay layers from tracks V2.. (ascending = bottom-to-top) ──
    struct OverlayLayer {
        QImage rgb;        // already scaled to outSize (the shared canvas grid)
        double opacity = 1.0;
        double videoScale = 1.0;
        double videoDx = 0.0;
        double videoDy = 0.0;
    };
    QVector<OverlayLayer> overlays;
    for (int t = 1; t < tracks.size(); ++t) {
        TimelineTrack *trk = tracks[t];
        if (!trk || trk->isHidden())
            continue;
        const QVector<ClipInfo> &clips = trk->clips();
        double start = 0.0;
        // Overlay tracks do NOT clamp-to-first: an upper track only
        // contributes where it genuinely has a clip under the playhead
        // (matches computePlaybackSequence's interval-only stacking).
        const int idx = activeClipOnTrack(clips, targetSec,
                                          /*clampToFirst=*/false, &start);
        if (idx < 0)
            continue;
        const ClipInfo &c = clips[idx];
        const double localSec = targetSec - start;            // >= 0
        const double srcSec = c.inPoint + localSec * c.speed;
        const QImage nativeRaw = decodeClipFrameNative(c.filePath, srcSec);
        if (nativeRaw.isNull())
            continue;
        // S4: grade each overlay clip in native resolution too (same per-clip
        // CC -> LUT order as V1 and the preview shader). No-op for un-graded
        // overlays, so S3's multi-track MSE stays ~0.
        // S5: then the overlay's FX pack, on the graded frame, in the same
        // CC -> LUT -> FX order as V1 and the shader (GLPreview.cpp:910-916).
        // No-op for effect-free overlays, so S3's multi-track MSE stays ~0.
        // S7: then the overlay's per-clip compositing mask (tracker-animated),
        // on the graded+FX'd native frame, BEFORE it is scaled onto the
        // shared canvas — so the masked-out region is transparent when this
        // overlay is SourceOver-composited and the layers beneath show
        // through (same CC -> LUT -> FX -> MASK per-clip order as V1). No-op
        // for un-masked overlays, so S3's multi-track MSE stays ~0.
        const QImage native = applyClipMask(
            applyClipFxPack(gradeClipNativeFrame(nativeRaw, c), c), c, srcSec);
        OverlayLayer L;
        // Scale the overlay source to the shared canvas grid first; the
        // compositor's videoScale then sizes the dst rect relative to the
        // canvas exactly as composeMultiTrackFrame does for L.rgb.
        L.rgb = native.scaled(outSize, Qt::IgnoreAspectRatio,
                              Qt::SmoothTransformation);
        // Straight ClipInfo -> layer copy, identical to the preview harvest
        // (src/VideoPlayer.cpp:4848-4850 finalizeOverlayFromDecoder and the
        // layer fill at VideoPlayer.cpp:3656/3734).
        L.opacity = c.opacity;
        L.videoScale = c.videoScale;
        L.videoDx = c.videoDx;
        L.videoDy = c.videoDy;
        overlays.append(L);
    }

    // No overlays -> the multi-track stack reduces to the V1 base. S6 still
    // runs the adjustment-layer grade + text-overlay bake on it (both strict
    // no-ops when the timeline has no adjustment layer / the V1 clip has no
    // text, so a lone V1 clip with a default transform stays byte-identical
    // to S2/S3/S4/S5 — applyAdjustmentLayers/applyTextOverlays return the
    // input UNTOUCHED in that case, no QPainter, no convert).
    if (overlays.isEmpty()) {
        const QImage adj = applyAdjustmentLayers(base, timeline, usec);
        return applyTextOverlays(adj, timeline, usec, &v1Clip);
    }

    // ── Composite: replicate VideoPlayer::composeMultiTrackFrame ────────────
    // (src/VideoPlayer.cpp:4542-4582). Replicated here rather than calling the
    // member directly because composeMultiTrackFrame is a non-static const
    // METHOD of VideoPlayer; this SSOT must run without owning a VideoPlayer
    // (the export path has none). The method body reads ONLY its arguments
    // plus a local QPainter (zero member-state access, verified at
    // VideoPlayer.cpp:4542-4582), so a faithful line-for-line port is
    // pixel-equivalent. Each formula is annotated with the exact source line:
    //   - canvas := ARGB32_Premultiplied copy of v1 base   (VideoPlayer.cpp:4558)
    //   - SmoothPixmapTransform = false                      (VideoPlayer.cpp:4565)
    //   - CompositionMode_SourceOver                         (VideoPlayer.cpp:4566)
    //   - skip layer when rgb null or opacity <= 0.001       (VideoPlayer.cpp:4570)
    //   - w  = canvas.width()  * L.videoScale                (VideoPlayer.cpp:4572)
    //   - h  = canvas.height() * L.videoScale                (VideoPlayer.cpp:4573)
    //   - cx = canvas.width()  * 0.5 + L.videoDx * width     (VideoPlayer.cpp:4574)
    //   - cy = canvas.height() * 0.5 + L.videoDy * height    (VideoPlayer.cpp:4575)
    //   - dst = QRectF(cx-w*0.5, cy-h*0.5, w, h)             (VideoPlayer.cpp:4576)
    //   - opacity = qBound(0.0, L.opacity, 1.0)               (VideoPlayer.cpp:4577)
    //   - p.drawImage(dst, L.rgb)                             (VideoPlayer.cpp:4578)
    // NOTE: composeMultiTrackFrame applies NO 2D rotation (ClipInfo
    // rotation2DDegrees is intentionally NOT consulted in the authoritative
    // multi-track compositor — see VideoPlayer.cpp:4569-4579). To pixel-match
    // the authoritative comparator this SSOT also omits rotation here.
    QImage composed = base.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&composed);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const QSize canvas(composed.width(), composed.height());
    for (const OverlayLayer &L : overlays) {
        if (L.rgb.isNull() || L.opacity <= 0.001)
            continue;
        const double w  = canvas.width()  * L.videoScale;
        const double h  = canvas.height() * L.videoScale;
        const double cx = canvas.width()  * 0.5 + L.videoDx * canvas.width();
        const double cy = canvas.height() * 0.5 + L.videoDy * canvas.height();
        const QRectF dst(cx - w * 0.5, cy - h * 0.5, w, h);
        p.setOpacity(qBound(0.0, L.opacity, 1.0));
        p.drawImage(dst, L.rgb);
    }
    p.end();

    // Public contract: Format_RGBA8888 at outSize (Timeline.h header / S2).
    // The composite ran on an ARGB32_Premultiplied canvas for blend-correct
    // SourceOver (matching VideoPlayer.cpp:4558); convert back so callers and
    // framediff::mse see the documented format.
    const QImage stacked = composed.convertToFormat(QImage::Format_RGBA8888);

    // S6 — over the fully-composited multi-track frame, apply the
    // adjustment-layer grade THEN bake text overlays, in the preview's order
    // (composite -> adjustment grade @GLPreview.cpp:2124-2167 -> text bake
    // @VideoPlayer.cpp:1911). Both are strict no-ops when the timeline has no
    // adjustment layer / the V1 clip has no text, preserving S3 multi-track
    // parity exactly.
    const QImage adj = applyAdjustmentLayers(stacked, timeline, usec);
    return applyTextOverlays(adj, timeline, usec, &v1Clip);
}

namespace detail {

QImage decodeClipFrameNativeForTest(const QString &filePath, double sourceSec)
{
    // Thin pass-through to the production decode helper so the parity
    // selftest's reference path uses the byte-identical libav+sws decode
    // renderFrameAt applies per layer. No logic is duplicated.
    return decodeClipFrameNative(filePath, sourceSec);
}


} // namespace detail

} // namespace tlrender
