#include "VideoPlayer.h"
#include <QSet>
#include "GLPreview.h"
#include "TimelineFrameRenderer.h"
#include "Timeline.h"
#include "LayerStyle.h"
#include "EffectParamSchema.h"
#include "UndoTrace.h"
#include "ProxyManager.h"
#include "TextOverlayBake.h"   // textbake::bakeOverlays (shared text baker SSOT)
#include "ClipGeometry.h"      // clipgeom::renderLayer (shared clip-placement SSOT)
#include "clipanim/ClipAnim.h" // S1 — motion/opacity keyframe evaluation
#include "TrackMatteKey.h"     // STAGE4B: trackMatteClipKey (canonical matte clip key)
#include "playback/LiveMatteResolve.h" // STAGE4B: clipstack::resolveLiveMatteSources
#include "playback/clipidt_flag.h"
#include "playback/hdrexport16_flag.h"
#include "playback/hdrmatte16_flag.h"
#include "playback/hdroverlay_flag.h"
#include "playback/idtgpu_flag.h"   // Stage9-GPU-IDT Story2: VEDITOR_HDR_IDT_GPU
#include "playback/PixFmtDepth.h"   // pixfmtdepth::bitDepthFromPixFmt (null-safe per-component depth)
#include "playback/SnsFit.h"
#include "playback/TlrCompose16.h"
#include "playback/HdrCompositeMath.h"
#include "color/ClipColorTransform.h"
#include "color/ClipOdt.h"
#include "gpucomposite_flag.h"
#include <algorithm>           // std::stable_sort

// Free comparator for the production layer sort. Extracted from the inline
// lambda in handlePlaybackTick so the S3-STACK predicate sub-assertion in
// main.cpp can exercise the EXACT same comparator and catch a re-inversion.
// Returns true when `a` should paint BEFORE `b` (i.e. is deeper in the stack).
// V1-wins stacking (PlaybackTypes.h:34): V1 (sourceTrack 0) paints LAST so it
// ends up ON TOP; the highest track (largest sourceTrack) paints FIRST so it is
// the backmost layer. SourceOver draws the last image frontmost, so to make V1
// frontmost the comparator sorts DESCENDING by sourceTrack (V_max first → back,
// V1 last → front).
// Lives in namespace clipstack to avoid GLOBAL-namespace ODR/symbol pollution.
namespace clipstack {
bool layerPaintOrderLess(const VideoPlayer::DecodedLayer &a,
                         const VideoPlayer::DecodedLayer &b)
{
    return a.sourceTrack > b.sourceTrack;
}
} // namespace clipstack

#if defined(_WIN32)
// Phase 1e — d3d11.h pulls in WinSDK 10.0.26100 headers that fight Qt's
// QCborTag operator==, so we keep the AVD3D11VADeviceContext peek inside
// D3D11Bridge.cpp where d3d11.h is the only public header. Here we just
// forward-declare the bridge entry point.
extern "C" void *veditor_avHwDeviceCtxToD3D11Device(AVBufferRef *ref);
#endif
#include <QSettings>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QUrl>
#include <QtGlobal>
#include <QDebug>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QtConcurrent>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QPen>
#include <QFontMetrics>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace veditor {
bool parentingEnabledFromEnv();
}

namespace clipmask {
QImage applyRasterAlphaMask(const QImage &sourceImage, const QVector<Mask> &masks);
}

namespace {

// Phase 1e — playback preview divisor for the playback decode path.
// Default 2 (half-res preview) so playback at 1440p / 4K sources hits the
// frame budget on systems where HW transfer + sws_scale is the bottleneck.
// QSettings controls it at runtime; VEDITOR_PLAYBACK_PROXY is used only as
// the first-run seed when no saved preference exists.
// Pause and export stay at full resolution because they bypass these
// decode wrappers — the user only sees aliasing during active playback.
std::atomic<int> g_playbackProxyPref{-1};

inline int normalizePlaybackProxyDivisor(int v)
{
    if (v != 1 && v != 2 && v != 4 && v != 8)
        return 2;
    return v;
}

inline int playbackProxyDivisor()
{
    int v = g_playbackProxyPref.load(std::memory_order_relaxed);
    if (v != -1)
        return v;

    QSettings prefs("VSimpleEditor", "Preferences");
    if (prefs.contains("playbackProxyDivisor")) {
        v = prefs.value("playbackProxyDivisor", 2).toInt();
    } else {
        const QByteArray raw = qgetenv("VEDITOR_PLAYBACK_PROXY");
        bool ok = false;
        const int envDivisor = raw.toInt(&ok);
        v = ok ? envDivisor : 2;
    }

    v = normalizePlaybackProxyDivisor(v);
    int expected = -1;
    if (g_playbackProxyPref.compare_exchange_strong(expected, v, std::memory_order_relaxed))
        return v;
    return expected;
}

bool scaleFrameToQImagePadded(SwsContext *ctx,
                              const AVFrame *frame,
                              AVPixelFormat dstPixFmt,
                              QImage &image)
{
    if (!ctx || !frame || image.isNull())
        return false;

    const int rowBytes = av_image_get_linesize(dstPixFmt, image.width(), 0);
    if (rowBytes <= 0 || rowBytes > image.bytesPerLine())
        return false;

    uint8_t *tmpData[4] = { nullptr, nullptr, nullptr, nullptr };
    int tmpStride[4] = { 0, 0, 0, 0 };
    if (av_image_alloc(tmpData, tmpStride, image.width(), image.height(),
                       dstPixFmt, 64) < 0)
        return false;

    sws_scale(ctx, frame->data, frame->linesize, 0, frame->height,
              tmpData, tmpStride);
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(image.scanLine(y), tmpData[0] + y * tmpStride[0],
                    static_cast<std::size_t>(rowBytes));
    }
    av_freep(&tmpData[0]);
    return true;
}

const ClipInfo *clipForPlaybackEntry(const Timeline *timeline, const PlaybackEntry &entry)
{
    if (!timeline)
        return nullptr;
    const auto &tracks = timeline->videoTracks();
    const auto *track = tracks.value(entry.sourceTrack, nullptr);
    if (!track)
        return nullptr;
    const auto &clips = track->clips();
    if (entry.sourceClipIndex < 0 || entry.sourceClipIndex >= clips.size())
        return nullptr;
    return &clips[entry.sourceClipIndex];
}

bool isNullObjectPath(const QString &path)
{
    return clipgeom::isNullObjectFilePath(path);
}

bool isNullObjectEntry(const PlaybackEntry &entry)
{
    return isNullObjectPath(entry.filePath);
}

bool isAdjustmentEntry(const Timeline *timeline, const PlaybackEntry &entry)
{
    const ClipInfo *clip = clipForPlaybackEntry(timeline, entry);
    return clip && clip->isAdjustment;
}

bool trackHasActiveSequenceReference(const QVector<ClipInfo> &clips, double targetSec)
{
    if (clips.isEmpty())
        return false;

    double cursor = 0.0;
    for (const ClipInfo &clip : clips) {
        const double clipStart = cursor + qMax(0.0, clip.leadInSec);
        const double clipEnd = clipStart + clip.effectiveDuration();
        if (targetSec >= clipStart && targetSec < clipEnd)
            return clip.isSequenceReference();
        cursor = clipEnd;
    }
    return targetSec <= 0.0 && clips.first().isSequenceReference();
}

bool timelineHasActiveSequenceReference(const Timeline *timeline, qint64 timelineUsec)
{
    if (!timeline)
        return false;
    const double targetSec = static_cast<double>(timelineUsec) / AV_TIME_BASE;
    for (const TimelineTrack *track : timeline->videoTracks()) {
        if (!track || track->isHidden())
            continue;
        if (trackHasActiveSequenceReference(track->clips(), targetSec))
            return true;
    }
    return false;
}

int previewPrimaryEntryIndexAt(const Timeline *timeline,
                               const QVector<PlaybackEntry> &sequence,
                               int preferredIdx,
                               qint64 timelineUsec)
{
    auto usableMediaEntry = [&](int idx) {
        return idx >= 0 && idx < sequence.size()
            && !isAdjustmentEntry(timeline, sequence.at(idx))
            && !isNullObjectEntry(sequence.at(idx));
    };
    if (usableMediaEntry(preferredIdx))
        return preferredIdx;

    const double tSec = static_cast<double>(timelineUsec) / AV_TIME_BASE;
    int bestIdx = -1;
    for (int i = 0; i < sequence.size(); ++i) {
        const PlaybackEntry &entry = sequence.at(i);
        if (tSec < entry.timelineStart || tSec >= entry.timelineEnd)
            continue;
        if (!usableMediaEntry(i))
            continue;
        if (bestIdx < 0 || entry.sourceTrack < sequence.at(bestIdx).sourceTrack)
            bestIdx = i;
    }
    return bestIdx >= 0 ? bestIdx : preferredIdx;
}

double entryClipLocalSeconds(const PlaybackEntry &entry, qint64 timelineUsec)
{
    return (static_cast<double>(timelineUsec) - entry.timelineStart * 1'000'000.0)
        / 1'000'000.0;
}

QPointF previewMaskTrackDelta(const ClipInfo &clip, double sourceSec)
{
    const TrackingResult &trk = clip.maskTrackingData;
    if (trk.isEmpty())
        return QPointF();

    const QRect now0 = trk.positionAtTime(sourceSec);
    const QRect base0 = trk.regions.first().rect;
    if (now0.isNull() || base0.isNull())
        return QPointF();

    const QPointF nowC(now0.x() + now0.width() / 2.0,
                       now0.y() + now0.height() / 2.0);
    const QPointF baseC(base0.x() + base0.width() / 2.0,
                        base0.y() + base0.height() / 2.0);
    return nowC - baseC;
}

void translateMask(Mask &mask, const QPointF &delta)
{
    if (delta.isNull())
        return;
    mask.rect.translate(delta);
    for (QPointF &point : mask.points)
        point += delta;
}

void scaleMask(Mask &mask, double sx, double sy)
{
    if (qFuzzyCompare(sx, 1.0) && qFuzzyCompare(sy, 1.0))
        return;
    mask.rect = QRectF(mask.rect.x() * sx,
                       mask.rect.y() * sy,
                       mask.rect.width() * sx,
                       mask.rect.height() * sy);
    for (QPointF &point : mask.points)
        point = QPointF(point.x() * sx, point.y() * sy);
}

QVector<Mask> previewMasksForFrame(const ClipInfo &clip,
                                   double sourceSec,
                                   QSize sourceSize,
                                   QSize frameSize)
{
    QVector<Mask> masks = clip.maskSystem.masks();
    if (masks.isEmpty())
        return masks;

    const QPointF delta = previewMaskTrackDelta(clip, sourceSec);
    for (Mask &mask : masks)
        translateMask(mask, delta);

    if (sourceSize.width() > 0 && sourceSize.height() > 0
        && frameSize.width() > 0 && frameSize.height() > 0
        && sourceSize != frameSize) {
        const double sx = static_cast<double>(frameSize.width())
            / static_cast<double>(sourceSize.width());
        const double sy = static_cast<double>(frameSize.height())
            / static_cast<double>(sourceSize.height());
        if (std::isfinite(sx) && std::isfinite(sy) && sx > 0.0 && sy > 0.0) {
            for (Mask &mask : masks)
                scaleMask(mask, sx, sy);
        }
    }

    return masks;
}

QImage applyPreviewClipMask(const QImage &frame,
                            const Timeline *timeline,
                            const PlaybackEntry &entry,
                            double sourceSec,
                            QSize sourceSize)
{
    if (frame.isNull())
        return frame;
    const ClipInfo *clip = clipForPlaybackEntry(timeline, entry);
    if (!clip || !clip->hasMask())
        return frame;

    const QVector<Mask> masks =
        previewMasksForFrame(*clip, sourceSec, sourceSize, frame.size());
    if (masks.isEmpty())
        return frame;
    return clipmask::applyRasterAlphaMask(frame, masks);
}

struct ActivePreviewAdjustmentClip {
    int trackIndex = 0;
    const ClipInfo *clip = nullptr;
    double localSec = 0.0;
};

bool appendPreviewAdjustmentClip(const Timeline *timeline,
                                 const PlaybackEntry &entry,
                                 qint64 timelineUsec,
                                 QVector<ActivePreviewAdjustmentClip> *out)
{
    if (!out)
        return false;
    const ClipInfo *clip = clipForPlaybackEntry(timeline, entry);
    if (!clip || !clip->isAdjustment)
        return false;
    out->append(ActivePreviewAdjustmentClip{
        entry.sourceTrack, clip, entryClipLocalSeconds(entry, timelineUsec)});
    return true;
}

QImage applyPreviewAdjustmentClipStack(
    const QImage &lowerComposite,
    const ActivePreviewAdjustmentClip &adjustment)
{
    if (lowerComposite.isNull() || !adjustment.clip)
        return lowerComposite;
    if (adjustment.clip->effects.isEmpty())
        return lowerComposite;

    const QVector<VideoEffect> effects =
        clipanim::effectiveEffectsAt(*adjustment.clip, adjustment.localSec);
    if (effects.isEmpty())
        return lowerComposite;

    const QImage adjusted = VideoEffectProcessor::applyEffectStack(
        lowerComposite, ColorCorrection(), effects);
    if (adjusted.isNull() || adjusted.size() != lowerComposite.size())
        return adjusted;

    QImage out = adjusted.convertToFormat(QImage::Format_ARGB32);
    const QImage alphaSource =
        lowerComposite.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < out.height(); ++y) {
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        const QRgb *src =
            reinterpret_cast<const QRgb *>(alphaSource.constScanLine(y));
        for (int x = 0; x < out.width(); ++x)
            dst[x] = qRgba(qRed(dst[x]), qGreen(dst[x]), qBlue(dst[x]), qAlpha(src[x]));
    }
    return out.convertToFormat(QImage::Format_RGBA8888);
}

QImage applyPreviewAdjustmentClipsAtTrack(
    const QImage &input,
    const QVector<ActivePreviewAdjustmentClip> &adjustments,
    int trackIndex)
{
    QImage out = input;
    for (const ActivePreviewAdjustmentClip &adjustment : adjustments) {
        if (adjustment.trackIndex != trackIndex)
            continue;
        out = applyPreviewAdjustmentClipStack(
            out.convertToFormat(QImage::Format_RGBA8888),
            adjustment);
    }
    return out;
}

bool hasPreviewAdjustmentAtTrack(
    const QVector<ActivePreviewAdjustmentClip> &adjustments,
    int trackIndex)
{
    for (const ActivePreviewAdjustmentClip &adjustment : adjustments) {
        if (adjustment.trackIndex == trackIndex)
            return true;
    }
    return false;
}

bool hasDecodedTrackMatte(const QVector<VideoPlayer::DecodedLayer> &layers)
{
    return std::any_of(
        layers.cbegin(), layers.cend(),
        [](const VideoPlayer::DecodedLayer &layer) {
            return layer.matteType != TrackMatteType::None;
        });
}

clipgeom::ClipTransform transformForLayer(const VideoPlayer::DecodedLayer &layer);
QVector<clipgeom::ClipTransform> effectiveLayerTransforms(
    const QVector<VideoPlayer::DecodedLayer> &layers,
    QSize canvas);

void composePreviewFrameWithAdjustmentClips(
    QImage &canvas,
    const QVector<VideoPlayer::DecodedLayer> &layers,
    const QVector<ActivePreviewAdjustmentClip> &adjustments)
{
    if (canvas.isNull())
        return;
    if (adjustments.isEmpty())
        return;

    Q_ASSERT(canvas.format() == QImage::Format_ARGB32_Premultiplied);
    if (canvas.format() != QImage::Format_ARGB32_Premultiplied)
        return;

    canvas.fill(Qt::transparent);
    const QSize cs(canvas.width(), canvas.height());
    const QVector<clipgeom::ClipTransform> effectiveTransforms =
        veditor::parentingEnabledFromEnv()
            ? effectiveLayerTransforms(layers, cs)
            : QVector<clipgeom::ClipTransform>{};

    auto paintTrack = [&](int trackIndex) {
        QPainter p(&canvas);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const bool kSmooth = false;
        for (int i = 0; i < layers.size(); ++i) {
            const VideoPlayer::DecodedLayer &L = layers.at(i);
            if (L.sourceTrack != trackIndex)
                continue;
            if (L.isNullObject || L.rgb.isNull() || L.opacity <= 0.001)
                continue;
            const clipgeom::ClipTransform t = effectiveTransforms.isEmpty()
                ? transformForLayer(L)
                : effectiveTransforms.value(i, transformForLayer(L));
            QImage placed = clipgeom::renderLayer(L.rgb, t, cs, kSmooth);
            if (!L.layerStyle.isIdentity())
                placed = layerstyle::apply(placed, L.layerStyle);
            p.setOpacity(qBound(0.0, L.opacity, 1.0));
            p.drawImage(0, 0, placed);
        }
    };

    auto applyAdjustmentTrack = [&](int trackIndex) {
        if (!hasPreviewAdjustmentAtTrack(adjustments, trackIndex))
            return;
        QImage adjusted = applyPreviewAdjustmentClipsAtTrack(
            canvas.convertToFormat(QImage::Format_RGBA8888),
            adjustments,
            trackIndex);
        canvas = adjusted.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    };

    int maxTrack = 0;
    for (const VideoPlayer::DecodedLayer &layer : layers)
        maxTrack = qMax(maxTrack, layer.sourceTrack);
    for (const ActivePreviewAdjustmentClip &adjustment : adjustments)
        maxTrack = qMax(maxTrack, adjustment.trackIndex);

    for (int track = maxTrack; track >= 1; --track) {
        paintTrack(track);
        applyAdjustmentTrack(track);
    }

    if (hasPreviewAdjustmentAtTrack(adjustments, 0))
        applyAdjustmentTrack(0);
    else
        paintTrack(0);
}

QImage applyPreviewAdjustmentClipsAfterMatte(
    const QImage &input,
    const QVector<ActivePreviewAdjustmentClip> &adjustments)
{
    QImage out = input;
    int maxTrack = 0;
    for (const ActivePreviewAdjustmentClip &adjustment : adjustments)
        maxTrack = qMax(maxTrack, adjustment.trackIndex);
    for (int track = 1; track <= maxTrack; ++track)
        out = applyPreviewAdjustmentClipsAtTrack(out, adjustments, track);
    return out;
}

clipgeom::ClipTransform transformForLayer(const VideoPlayer::DecodedLayer &layer)
{
    return clipgeom::ClipTransform{layer.videoScale, layer.videoDx,
                                   layer.videoDy, layer.rotation2DDegrees};
}

QString clipIdForLayer(const VideoPlayer::DecodedLayer &layer)
{
    if (layer.sourceTrack < 0 || layer.sourceClipIndex < 0)
        return QString();
    return trackMatteClipKey(layer.sourceTrack, layer.sourceClipIndex);
}

void resolveDecodedLayerParentSources(QVector<VideoPlayer::DecodedLayer> &layers)
{
    QVector<int> order;
    order.reserve(layers.size());
    for (int i = 0; i < layers.size(); ++i)
        order.append(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) {
                         return layers.at(a).sourceTrack < layers.at(b).sourceTrack;
                     });

    QHash<QString, int> canonicalByClipId;
    QVector<int> canonicalByActual(layers.size(), -1);
    QVector<int> actualByCanonical(layers.size(), -1);
    for (int ci = 0; ci < order.size(); ++ci) {
        const int actual = order.at(ci);
        canonicalByActual[actual] = ci;
        actualByCanonical[ci] = actual;
        const QString clipId = clipIdForLayer(layers[actual]);
        if (!clipId.isEmpty())
            canonicalByClipId.insert(clipId, ci);
        layers[actual].parentSourceIndex = -1;
    }

    for (int actual = 0; actual < layers.size(); ++actual) {
        if (layers[actual].parentClipId.isEmpty())
            continue;
        const int parentCanonical =
            canonicalByClipId.value(layers[actual].parentClipId, -1);
        const int childCanonical = canonicalByActual.value(actual, -1);
        if (parentCanonical <= 0 || parentCanonical == childCanonical)
            continue;
        const int parentActual = actualByCanonical.value(parentCanonical, -1);
        if (parentActual >= 0 && parentActual != actual)
            layers[actual].parentSourceIndex = parentActual;
    }
}

QVector<clipgeom::ClipTransform> effectiveLayerTransforms(
    const QVector<VideoPlayer::DecodedLayer> &layers,
    QSize canvas)
{
    QVector<clipgeom::ClipTransform> effective;
    effective.reserve(layers.size());
    for (const auto &layer : layers)
        effective.append(transformForLayer(layer));

    QVector<int> state(layers.size(), 0);
    QVector<char> valid(layers.size(), 1);

    std::function<bool(int, int)> resolve = [&](int idx, int depth) -> bool {
        if (idx < 0 || idx >= layers.size())
            return false;
        if (state[idx] == 1) {
            valid[idx] = 0;
            effective[idx] = transformForLayer(layers[idx]);
            state[idx] = 2;
            return false;
        }
        if (state[idx] == 2)
            return valid[idx] != 0;
        if (depth > 8) {
            effective[idx] = transformForLayer(layers[idx]);
            return true;
        }

        state[idx] = 1;
        const int parentIdx = layers[idx].parentSourceIndex;
        if (parentIdx < 0 || parentIdx >= layers.size()
            || parentIdx == idx || layers[parentIdx].sourceTrack <= 0) {
            effective[idx] = transformForLayer(layers[idx]);
            valid[idx] = 1;
            state[idx] = 2;
            return true;
        }
        if (!resolve(parentIdx, depth + 1)) {
            effective[idx] = transformForLayer(layers[idx]);
            valid[idx] = 0;
            state[idx] = 2;
            return false;
        }

        effective[idx] = clipgeom::composeParented(
            transformForLayer(layers[idx]), effective[parentIdx], canvas);
        valid[idx] = 1;
        state[idx] = 2;
        return true;
    };

    for (int i = 0; i < layers.size(); ++i)
        resolve(i, 0);
    return effective;
}

bool sameEffectValue(const VideoEffect &a, const VideoEffect &b)
{
    return a.type == b.type
        && a.enabled == b.enabled
        && a.param1 == b.param1
        && a.param2 == b.param2
        && a.param3 == b.param3
        && a.keyColor == b.keyColor;
}

QVector<int> resolvePreviewEffectSourceIndices(const QVector<VideoEffect> &source,
                                               const QVector<VideoEffect> &preview)
{
    QVector<int> indices;
    indices.reserve(preview.size());

    int searchFrom = 0;
    for (const VideoEffect &effect : preview) {
        int found = -1;
        for (int i = searchFrom; i < source.size(); ++i) {
            if (sameEffectValue(source[i], effect)) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            for (int i = searchFrom; i < source.size(); ++i) {
                if (source[i].type == effect.type) {
                    found = i;
                    break;
                }
            }
        }
        indices.append(found);
        if (found >= 0)
            searchFrom = found + 1;
    }

    return indices;
}

KeyframeTrack remapKeyframeTrack(const KeyframeTrack &sourceTrack,
                                 const QString &propertyName)
{
    KeyframeTrack remapped(propertyName, sourceTrack.defaultValue());
    for (const KeyframePoint &kf : sourceTrack.keyframes()) {
        remapped.addKeyframe(kf.time,
                             kf.value,
                             kf.interpolation,
                             kf.bezX1,
                             kf.bezY1,
                             kf.bezX2,
                             kf.bezY2,
                             kf.hasSpatialTangent,
                             kf.spatialOutX,
                             kf.spatialOutY,
                             kf.spatialInX,
                             kf.spatialInY);
    }
    return remapped;
}

QVector<VideoEffect> effectivePreviewEffectsAt(
    const QVector<VideoEffect> &staticEffects,
    const Timeline *timeline,
    const QVector<PlaybackEntry> &sequence,
    qint64 timelineUsec)
{
    if (staticEffects.isEmpty() || !timeline)
        return staticEffects;

    const int selectedClip = timeline->selectedVideoClipIndex();
    if (selectedClip < 0)
        return staticEffects;

    const PlaybackEntry *entry = nullptr;
    for (const PlaybackEntry &candidate : sequence) {
        if (candidate.sourceTrack == 0
            && candidate.sourceClipIndex == selectedClip) {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
        return staticEffects;

    const ClipInfo *clip = clipForPlaybackEntry(timeline, *entry);
    if (!clip)
        return staticEffects;

    const QVector<int> sourceIndices =
        resolvePreviewEffectSourceIndices(clip->effects, staticEffects);
    const double clipLocalSec =
        (static_cast<double>(timelineUsec) - entry->timelineStart * 1'000'000.0)
        / 1'000'000.0;

    ClipInfo previewClip = *clip;
    previewClip.effects.clear();
    previewClip.keyframes = KeyframeManager();

    for (int i = 0; i < staticEffects.size() && i < sourceIndices.size(); ++i) {
        const int sourceIndex = sourceIndices[i];
        if (sourceIndex < 0 || sourceIndex >= clip->effects.size()) {
            VideoEffect unmapped = staticEffects[i];
            unmapped.startSec = -1.0;
            unmapped.endSec = -1.0;
            previewClip.effects.append(unmapped);
            continue;
        }

        VideoEffect effect = staticEffects[i];
        effect.startSec = clip->effects[sourceIndex].startSec;
        effect.endSec = clip->effects[sourceIndex].endSec;

        const int previewIndex = previewClip.effects.size();
        previewClip.effects.append(effect);
        const auto schema = effectctrl::paramSchemaFor(effect.type);
        for (const auto &def : schema) {
            const QString sourceTrackName =
                QStringLiteral("effect.%1.%2").arg(sourceIndex).arg(def.name);
            const KeyframeTrack *sourceTrack = clip->keyframes.track(sourceTrackName);
            if (!sourceTrack || sourceTrack->count() <= 0)
                continue;

            const QString previewTrackName =
                QStringLiteral("effect.%1.%2").arg(previewIndex).arg(def.name);
            previewClip.keyframes.addTrack(
                remapKeyframeTrack(*sourceTrack, previewTrackName));

            const LoopMode loopMode = clip->keyframes.loopOutMode(sourceTrackName);
            if (loopMode != LoopMode::None)
                previewClip.keyframes.setLoopOutMode(previewTrackName, loopMode);
        }
    }

    return clipanim::effectiveEffectsAt(previewClip, clipLocalSec);
}

void applyLayerMotionOpacity(const Timeline *timeline,
                             const PlaybackEntry &entry,
                             qint64 timelineUsec,
                             double staticOpacity,
                             VideoPlayer::DecodedLayer *layer)
{
    if (!layer)
        return;

    layer->opacity = staticOpacity;
    layer->videoScale = entry.videoScale;
    layer->videoDx = entry.videoDx;
    layer->videoDy = entry.videoDy;
    layer->rotation2DDegrees = entry.rotation2DDegrees;

    const ClipInfo *clip = clipForPlaybackEntry(timeline, entry);
    if (!clip || !clip->keyframes.hasAnyKeyframes())
        return;

    const double clipLocalSec =
        (static_cast<double>(timelineUsec) - entry.timelineStart * 1'000'000.0)
        / 1'000'000.0;
    const clipgeom::ClipTransform transform =
        clipanim::effectiveTransformAt(*clip, clipLocalSec);
    layer->opacity =
        clipanim::effectiveOpacityAt(*clip, clipLocalSec, staticOpacity);
    layer->videoScale = transform.videoScale;
    layer->videoDx = transform.videoDx;
    layer->videoDy = transform.videoDy;
    layer->rotation2DDegrees = transform.rotationDeg;
}

void populateLayerMetadata(const Timeline *timeline,
                           const PlaybackEntry &entry, int seqIdx,
                           VideoPlayer::DecodedLayer *layer)
{
    if (!layer)
        return;
    layer->sourceTrack = entry.sourceTrack;
    layer->sourceClipIndex = entry.sourceClipIndex;
    layer->sequenceIdx = seqIdx;
    layer->layerStyle = entry.layerStyle;
    if (layer->layerStyle.isIdentity()) {
        if (const ClipInfo *clip = clipForPlaybackEntry(timeline, entry))
            layer->layerStyle = clip->layerStyle;
    }
    layer->fitContain = entry.fitContain;
    layer->fitCover = entry.fitCover;
    layer->matteType = static_cast<TrackMatteType>(entry.matteTypeOrdinal);
    layer->matteSourceClipId = entry.matteSourceClipId;
    layer->parentClipId = entry.parentClipId;
    layer->isNullObject = isNullObjectEntry(entry);
}

// Phase 1e Win #9 — canvas proxy divisor for the multi-track compose path.
// playbackProxyDivisor() shrinks the *source* layer images via sws_scale dst
// dims; that already cuts decode bandwidth, but the compositor canvas
// (m_canvasBase) was still allocated at full m_canvasWidth/m_canvasHeight.
// The result: every multi-track tick paid for an 8 MB ARGB32 fill, a 4x
// pixel-write upscale during QPainter::drawImage (540p layer -> 1080p
// canvas), and a 4x larger texture upload. By matching the canvas size to
// the decoded layer resolution we collapse the upscale to a 1:1 blit and
// quarter the fill / upload bandwidth — GLPreview's fragment shader still
// stretches the smaller texture to the widget viewport with bilinear
// sampling, so the visible output is the same as PLAYBACK_PROXY=2 already
// produces for the V1-only fast path.
//
// Default: divisor follows playbackProxyDivisor() so the canvas always
// matches the decoded layer res (PLAYBACK_PROXY=1 keeps the canvas
// full-res, preserving legacy behaviour for users who opt out of decode
// proxy). VEDITOR_CANVAS_PROXY_DISABLE=1 forces full-res canvas
// regardless, in case the proxy interaction surfaces a regression.
//
// Caller-side guard (handlePlaybackTick): when m_textOverlays is non-empty
// we revert to full-res because composeFrameWithOverlays paints text at
// pixel coordinates over m_canvasBase — shrinking the canvas would reposition
// the text after GL upscale. Multi-track + text overlays therefore stays
// on the legacy full-res path; no regression for that minority case.
inline int canvasProxyDivisor()
{
    static const bool kDisabled = []() {
        return qEnvironmentVariableIntValue("VEDITOR_CANVAS_PROXY_DISABLE") != 0;
    }();
    if (kDisabled)
        return 1;
    return qMax(1, playbackProxyDivisor());
}

// Phase 1e — opaque reference into FFmpeg's D3D11 frame pool. Only valid
// while the originating AVFrame is held; we hand it to GLPreview which
// registers it once via WGL_NV_DX_interop2 and caches by (texture, subres).
struct D3D11FrameRef {
    void *texture = nullptr;
    int   subresource = 0;
    int   width = 0;
    int   height = 0;
};

bool extractD3D11FrameRef(const AVFrame *frame, D3D11FrameRef *out)
{
    if (!out || !frame || frame->format != AV_PIX_FMT_D3D11)
        return false;
    out->texture = reinterpret_cast<void*>(frame->data[0]);
    out->subresource = static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));
    out->width = frame->width;
    out->height = frame->height;
    return out->texture != nullptr;
}

// US-WIRE-3: transparent overlay for rubber-band region selection on the
// preview widget. The overlay is a child of VideoPlayer, positioned over
// GLPreview, and paints a translucent drag rect with 2px white outline.
class RegionPickerOverlay : public QWidget
{
public:
    using Callback = std::function<void(QRect)>;

    explicit RegionPickerOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::CrossCursor);
        hide();
    }

    void setCallback(Callback cb) { m_callback = std::move(cb); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, 30));
        if (m_dragging && m_currentRect.width() > 0 && m_currentRect.height() > 0) {
            QColor fill(255, 255, 255, 38);
            p.fillRect(m_currentRect, fill);
            p.setPen(QPen(Qt::white, 2));
            p.drawRect(m_currentRect);
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            m_dragging = true;
            m_origin = e->pos();
            m_currentRect = QRect();
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_dragging) {
            m_currentRect = QRect(m_origin, e->pos()).normalized();
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && m_dragging) {
            m_dragging = false;
            m_currentRect = QRect(m_origin, e->pos()).normalized();
            update();
            if (m_currentRect.width() >= 4 && m_currentRect.height() >= 4 && m_callback) {
                m_callback(m_currentRect);
            }
        }
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) {
            if (m_callback) m_callback(QRect());
        }
    }

private:
    Callback m_callback;
    bool m_dragging = false;
    QPoint m_origin;
    QRect m_currentRect;
};

class WbEyedropperOverlay : public QWidget
{
public:
    using Callback = std::function<void(QPoint)>;

    explicit WbEyedropperOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::CrossCursor);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        hide();
    }

    void setCallback(Callback cb) { m_callback = std::move(cb); }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && m_callback) {
            m_callback(e->pos());
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape && m_callback) {
            m_callback(QPoint(-1, -1));
            e->accept();
            return;
        }
        QWidget::keyPressEvent(e);
    }

private:
    Callback m_callback;
};


// VEDITOR_TICK_TRACE=1 turns on per-tick wall-time accumulators that print a
// 30-tick rollup line to qInfo. Default off — read once per process so the
// hot path stays at one branch on a cached bool.
inline bool tickTraceEnabled()
{
    static const bool enabled = qEnvironmentVariableIntValue("VEDITOR_TICK_TRACE") != 0;
    return enabled;
}

// Phase 1e Win #10 — VEDITOR_STALL_TRACE=1 logs wall-time around the
// known multi-second stall candidates (cold openTrackDecoder probing
// avformat_find_stream_info, V2 prefetch waitForFinished, blockingMap
// overlay decode, clip transition advanceToEntry, AudioMixer's
// av_read_frame inside refillRingForEntry). Default off; emits
// qWarning only when a section exceeds its per-section threshold so
// the log stays short enough to attach to a bug report. The intent
// is to identify whether stalls are caused by cold decoder open
// (avformat_find_stream_info on a 4h H.264 source), prefetch
// synchronization (Win #6 worker waiting on FFmpeg HW transfer),
// blockingMap overlay decode contention, clip transition
// resetDecoder, or audio refill blocking I/O.
inline bool stallTraceEnabled()
{
    static const bool enabled = qEnvironmentVariableIntValue("VEDITOR_STALL_TRACE") != 0;
    return enabled;
}

inline constexpr qint64 kStallThresholdOpenMs = 100;
inline constexpr qint64 kStallThresholdWaitMs = 200;
inline constexpr qint64 kStallThresholdTickMs = 200;

inline qint64 nsToMs(qint64 ns) { return ns / 1000000LL; }

QString formatTimestamp(int64_t positionUs)
{
    const int totalSeconds = static_cast<int>(qMax<int64_t>(0, positionUs / AV_TIME_BASE));
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;

    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }

    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

}

void setPlaybackProxyDivisorPreference(int div)
{
    div = normalizePlaybackProxyDivisor(div);
    g_playbackProxyPref.store(div, std::memory_order_relaxed);
    QSettings("VSimpleEditor", "Preferences").setValue("playbackProxyDivisor", div);
}

VideoPlayer::VideoPlayer(QWidget *parent)
    : QWidget(parent)
    , m_gpuEffectsEnabled(QSettings("VSimpleEditor", "Preferences")
                              .value("gpuEffectsEnabled", true).toBool())
{
    qInfo() << "VideoPlayer::ctor";
    setupUI();
    m_playbackTimer = new QTimer(this);
    m_playbackTimer->setSingleShot(true);
    // PreciseTimer keeps the actual fire interval close to the requested
    // value; default CoarseTimer on Windows snaps to 16 ms steps which
    // capped multi-track playback at ~30 fps even when each tick fits
    // inside an 8 ms budget.
    m_playbackTimer->setTimerType(Qt::PreciseTimer);
    connect(m_playbackTimer, &QTimer::timeout, this, &VideoPlayer::handlePlaybackTick);

    m_seekTimer = new QTimer(this);
    m_seekTimer->setSingleShot(true);
    m_seekTimer->setInterval(15);
    connect(m_seekTimer, &QTimer::timeout, this, &VideoPlayer::performPendingSeek);

    m_mixer = new AudioMixer(this);
    connect(m_mixer, &AudioMixer::decoderError, this,
            [](const QString &msg) { qWarning() << "AudioMixer:" << msg; });

    // ADAPTIVE-1: 既定 ON。VEDITOR_ADAPTIVE_PREVIEW_DISABLE=1 で完全バイパス
    // (scaleFactor 強制 1.0 + キャッシュ無効) → 従来挙動とビット同一。
    m_adaptivePreviewEnabled =
        qEnvironmentVariableIntValue("VEDITOR_ADAPTIVE_PREVIEW_DISABLE") == 0;
}

VideoPlayer::~VideoPlayer()
{
    qInfo() << "VideoPlayer::dtor";
    // Stop any in-flight timers before decoder teardown so a queued
    // handlePlaybackTick doesn't fire on a half-destroyed object.
    if (m_playbackTimer) m_playbackTimer->stop();
    if (m_seekTimer)     m_seekTimer->stop();
    // US-WIRE-3: clean up region picker overlay if active
    if (m_regionPickerOverlay) {
        m_regionPickerOverlay->deleteLater();
        m_regionPickerOverlay = nullptr;
    }
    if (m_wbEyedropperOverlay) {
        m_wbEyedropperOverlay->deleteLater();
        m_wbEyedropperOverlay = nullptr;
    }
    resetDecoder();
    // Tear down V2+ decoder pool after the legacy V1 decoder is gone, then
    // release the shared HW device context. The pool decoders only hold
    // av_buffer_ref'd handles to m_sharedPoolHwDeviceCtx, so freeing them
    // first decrements the refcount to 1 (held by VideoPlayer itself).
    clearAllPoolDecoders();
    if (m_glPreview) {
        // Pool textures the cache might still reference are about to be
        // freed alongside the device — drop them before that happens.
        m_glPreview->flushInteropCache();
        m_glPreview->setSharedD3D11Device(nullptr);
    }
    if (m_sharedPoolHwDeviceCtx)
        av_buffer_unref(&m_sharedPoolHwDeviceCtx);
    m_sharedPoolHwPixFmt = AV_PIX_FMT_NONE;
    // Final release of the V1 HW device context (resetDecoder no longer
    // touches it so it survives clip switches; release at process exit).
    if (m_hwDeviceCtx)
        av_buffer_unref(&m_hwDeviceCtx);
}

void VideoPlayer::setupUI()
{
    auto *layout = new QVBoxLayout(this);

    auto *displayStack = new QStackedWidget(this);

    m_videoDisplay = new QLabel(this);
    m_videoDisplay->setAlignment(Qt::AlignCenter);
    // Soft minimum kept small so the preview can be shrunk to give the
    // timeline more vertical room; setCanvasSize tightens this once a clip
    // is loaded.
    m_videoDisplay->setMinimumSize(240, 135);
    m_videoDisplay->setText("Drop a video file or use File > Open");
    m_videoDisplay->setStyleSheet("background-color: #1a1a1a; color: #888; font-size: 16px;");

    m_glPreview = new GLPreview(this);
    m_glPreview->setMinimumSize(240, 135);
    // PV-B: プレビュー右クリック → VideoPlayer 経由で MainWindow へ転送。
    connect(m_glPreview, &GLPreview::contextMenuRequested,
            this, &VideoPlayer::previewContextMenuRequested);
    connect(m_glPreview, &GLPreview::textRectRequested,
            this, &VideoPlayer::textRectRequested);
    connect(m_glPreview, &GLPreview::textInlineCommitted,
            this, &VideoPlayer::textInlineCommitted);
    connect(m_glPreview, &GLPreview::textOverlayEditCommitted,
            this, &VideoPlayer::textOverlayEditCommitted);
    connect(m_glPreview, &GLPreview::textOverlayEditStarted,
            this, [this](int idx) { setHiddenTextOverlayIndex(idx); });
    connect(m_glPreview, &GLPreview::textOverlayEditEnded,
            this, [this]() { setHiddenTextOverlayIndex(-1); });
    connect(m_glPreview, &GLPreview::textOverlayRectChanged,
            this, &VideoPlayer::textOverlayRectChanged);
    connect(m_glPreview, &GLPreview::videoSourceTransformChanged,
            this, [this](double scale, double dx, double dy) {
                // V3 sprint: prefer m_editTargetEntry when set (Timeline 上で
                // V2/V3 clip を選択している状態)。invalid なら m_activeEntry
                // にフォールバックして V1 編集の従来挙動を維持。
                int targetIdx = (m_editTargetEntry >= 0
                                 && m_editTargetEntry < m_sequence.size())
                                ? m_editTargetEntry
                                : m_activeEntry;
                if (targetIdx < 0 || targetIdx >= m_sequence.size())
                    return;
                const auto &entry = m_sequence[targetIdx];
                static bool loggedFirstDragEmit = false;
                if (!loggedFirstDragEmit) {
                    qInfo() << "[drag-emit] first emit since startup — m_editTargetEntry="
                            << m_editTargetEntry
                            << "m_activeEntry=" << m_activeEntry
                            << "targetIdx=" << targetIdx
                            << "entry.sourceTrack=" << (targetIdx >= 0 ? m_sequence[targetIdx].sourceTrack : -1)
                            << "entry.sourceClipIndex=" << (targetIdx >= 0 ? m_sequence[targetIdx].sourceClipIndex : -1);
                    loggedFirstDragEmit = true;
                }
                emit videoSourceTransformChanged(entry.sourceTrack, entry.sourceClipIndex,
                                                 scale, dx, dy);
            });

    displayStack->addWidget(m_videoDisplay); // index 0: software
    displayStack->addWidget(m_glPreview);    // index 1: GL
    displayStack->setCurrentIndex(m_useGL ? 1 : 0);

    layout->addWidget(displayStack, 1);

    auto *controls = new QHBoxLayout();

    m_proxyButton = new QPushButton(this);
    // Frame step: |◀ and ▶|
    m_stepBackButton = new QPushButton(QString::fromUtf8("\xE2\x8F\xAE"), this); // ⏮
    m_stepFwdButton  = new QPushButton(QString::fromUtf8("\xE2\x8F\xAD"), this); // ⏭
    // Unicode media controls: ▶ U+25B6 / ⏸ U+23F8 / ⏹ U+23F9
    // m_playButton doubles as pause: updatePlayButton() flips its glyph
    // and tooltip based on m_playing, and the click is wired to togglePlay()
    // so a single press swaps the state instead of needing two buttons.
    m_playButton = new QPushButton(QString::fromUtf8("\xE2\x96\xB6"), this);
    m_stopButton = new QPushButton(QString::fromUtf8("\xE2\x8F\xB9"), this);
    m_seekBar = new QSlider(Qt::Horizontal, this);
    m_timeLabel = new QLabel("00:00 / 00:00", this);

    const QString mediaBtnStyle =
        "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
        "  border-radius: 4px; font-size: 16px; padding: 0; }"
        "QPushButton:hover { background-color: #555; }"
        "QPushButton:pressed { background-color: #666; }"
        "QPushButton:disabled { color: #777; background-color: #383838; }";
    m_playButton->setFixedSize(40, 32);
    m_stopButton->setFixedSize(40, 32);
    m_playButton->setStyleSheet(mediaBtnStyle);
    m_stopButton->setStyleSheet(mediaBtnStyle);
    m_playButton->setToolTip(QStringLiteral("再生"));
    m_stopButton->setToolTip(QStringLiteral("停止"));
    m_stepBackButton->setFixedSize(40, 32);
    m_stepFwdButton->setFixedSize(40, 32);
    m_stepBackButton->setStyleSheet(mediaBtnStyle);
    m_stepFwdButton->setStyleSheet(mediaBtnStyle);
    m_stepBackButton->setToolTip(QStringLiteral("1フレーム戻る (←)"));
    m_stepFwdButton->setToolTip(QStringLiteral("1フレーム進む (→)"));
    connect(m_stepBackButton, &QPushButton::clicked, this, &VideoPlayer::stepBackward);
    connect(m_stepFwdButton,  &QPushButton::clicked, this, &VideoPlayer::stepForward);

    {
        QSettings prefs("VSimpleEditor", "Preferences");
        int saved = prefs.value("proxyDivisor", 1).toInt();
        if (saved != 1 && saved != 2 && saved != 4 && saved != 8) saved = 1;
        m_proxyDivisor = saved;
    }
    auto proxyLabel = [](int d) {
        switch (d) {
        case 2: return QStringLiteral("1/2");
        case 4: return QStringLiteral("1/4");
        case 8: return QStringLiteral("1/8");
        default: return QStringLiteral("Full");
        }
    };
    m_proxyButton->setText(proxyLabel(m_proxyDivisor));
    m_proxyButton->setFixedSize(56, 32);
    m_proxyButton->setStyleSheet(mediaBtnStyle);
    m_proxyButton->setToolTip(QStringLiteral("プロキシ設定 (再生用プロキシ ON/OFF と プレビュー解像度)"));
    // Forward to MainWindow's proxy settings dialog so this seekbar-left
    // affordance and the toolbar's "プロキシモード切替" / "プロキシ生成..."
    // entries all drive the same configuration surface.
    connect(m_proxyButton, &QPushButton::clicked, this, &VideoPlayer::proxySettingsRequested);
    m_timeLabel->setFixedWidth(120);
    m_seekBar->setRange(0, 0);
    m_seekBar->setTracking(false);

    // Iteration 15 — preview maximize toggle button. Sits at the right end
    // of the controls row (the bottom-right of the preview area). Toggling
    // emits previewMaximizeChanged; MainWindow hides/shows the timeline
    // and its Esc shortcut routes back into setPreviewMaximized(false).
    m_maximizeButton = new QPushButton(QString::fromUtf8("\xE2\x9B\xB6"), this); // ⛶
    m_maximizeButton->setCheckable(true);
    m_maximizeButton->setFixedSize(40, 32);
    m_maximizeButton->setStyleSheet(mediaBtnStyle);
    m_maximizeButton->setToolTip(QStringLiteral("プレビュー最大化 (Esc で解除)"));
    connect(m_maximizeButton, &QPushButton::toggled,
            this, &VideoPlayer::setPreviewMaximized);

    controls->addWidget(m_proxyButton);
    controls->addWidget(m_stepBackButton);
    controls->addWidget(m_playButton);
    controls->addWidget(m_stopButton);
    controls->addWidget(m_stepFwdButton);
    controls->addWidget(m_seekBar);
    controls->addWidget(m_timeLabel);
    controls->addWidget(m_maximizeButton);

    layout->addLayout(controls);

    connect(m_playButton, &QPushButton::clicked, this, &VideoPlayer::togglePlay);
    connect(m_stopButton, &QPushButton::clicked, this, &VideoPlayer::stop);
    connect(m_seekBar, &QSlider::sliderMoved, this, [this](int pos) {
        m_lastDragMs = pos;
        previewSeek(pos);
    });
    connect(m_seekBar, &QSlider::sliderReleased, this, [this]() {
        if (m_lastDragMs >= 0)
            seek(m_lastDragMs);
    });
    connect(m_seekBar, &QSlider::valueChanged, this, [this](int value) {
        if (m_lastDragMs >= 0) {
            m_lastDragMs = -1;
            return;
        }
        if (!m_seekBar->isSliderDown())
            seek(value);
    });
}

void VideoPlayer::setProxyDivisor(int divisor)
{
    static const QSet<int> allowed = {1, 2, 4, 8};
    if (!allowed.contains(divisor) || divisor == m_proxyDivisor)
        return;
    m_proxyDivisor = divisor;
    if (m_proxyButton) {
        const QString text = (divisor == 1) ? QStringLiteral("Full")
                            : QStringLiteral("1/%1").arg(divisor);
        m_proxyButton->setText(text);
    }
    QSettings("VSimpleEditor", "Preferences").setValue("proxyDivisor", m_proxyDivisor);
    refreshDisplayedFrame();
}

void VideoPlayer::applyPlaybackQualityChanged()
{
    ++m_timelineRevision;
    m_frameCache.invalidateRevision(m_timelineRevision);
    refreshDisplayedFrame();
}

void VideoPlayer::loadFile(const QString &filePath)
{
    undotrace::log("loadFile:enter");
    qInfo() << "VideoPlayer::loadFile BEGIN" << filePath;
    // Audio is now driven by the independent A-track schedule via
    // setAudioSequence -> AudioMixer. loadFile only reloads the FFmpeg
    // video decoder. No audio touching here — audio is owned by the mixer
    // and is independent of which video file the FFmpeg decoder is on, since
    // the audio schedule owns the audio channel the old 200ms mute lockout
    // is no longer applicable.
    resetDecoder();
    qInfo() << "  resetDecoder done";
    m_loadedFilePath = filePath;

    const QByteArray pathUtf8 = filePath.toUtf8();
    if (avformat_open_input(&m_formatCtx, pathUtf8.constData(), nullptr, nullptr) != 0) {
        qWarning() << "avformat_open_input failed for" << filePath;
        m_videoDisplay->setText("Failed to open file");
        undotrace::log("loadFile:exit");
        return;
    }
    qInfo() << "  avformat_open_input ok";

    undotrace::log("loadFile:beforeFindStream");
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        qWarning() << "avformat_find_stream_info failed";
        resetDecoder();
        m_videoDisplay->setText("Failed to read stream info");
        undotrace::log("loadFile:exit");
        return;
    }
    undotrace::log("loadFile:afterFindStream");
    qInfo() << "  avformat_find_stream_info ok, nb_streams=" << m_formatCtx->nb_streams;

    m_videoStreamIndex = -1;
    for (unsigned i = 0; i < m_formatCtx->nb_streams; i++) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = static_cast<int>(i);
            break;
        }
    }

    if (m_videoStreamIndex < 0) {
        resetDecoder();
        m_videoDisplay->setText("No video stream found");
        undotrace::log("loadFile:exit");
        return;
    }

    auto *codecpar = m_formatCtx->streams[m_videoStreamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        resetDecoder();
        m_videoDisplay->setText("Unsupported codec");
        undotrace::log("loadFile:exit");
        return;
    }

    m_hdrInfo = {};
    m_hdrInfo.primaries = codecpar->color_primaries;
    m_hdrInfo.trc = codecpar->color_trc;
    m_hdrInfo.colorspace = codecpar->color_space;
    // Per-component (luma) depth — null-safe and subsampling-correct.
    // av_get_bits_per_pixel(desc)/3 collapsed 10/12-bit 4:2:0 HDR to 8-bit and
    // would deref a null descriptor for unknown formats; pixfmtdepth reads
    // comp[0].depth directly. Mirrors Timeline::addClip's ingest probe.
    m_hdrInfo.bitDepth = pixfmtdepth::bitDepthFromPixFmt(codecpar->format);
    m_hdrInfo.isHdr = (codecpar->color_trc == AVCOL_TRC_SMPTE2084
                       || codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67);

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx || avcodec_parameters_to_context(m_codecCtx, codecpar) < 0) {
        resetDecoder();
        m_videoDisplay->setText("Failed to initialize codec");
        undotrace::log("loadFile:exit");
        return;
    }

    m_hwPixFmt = AV_PIX_FMT_NONE;
    // Reuse the D3D11VA device context across loadFile calls — it is
    // process-global and stateless between clips. Recreating it every
    // switch added ~22 ms of main-thread block per loadFile (debugger
    // breakdown of the 120 ms freeze on track switch).
    undotrace::log("loadFile:beforeHwDevice");
    const bool hwDeviceReady = m_hwDeviceCtx
        || av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA,
                                   nullptr, nullptr, 0) >= 0;
    undotrace::log("loadFile:afterHwDevice");
    if (hwDeviceReady) {
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                m_hwPixFmt = cfg->pix_fmt;
                break;
            }
        }
    }

    if (m_hwPixFmt != AV_PIX_FMT_NONE && m_hwDeviceCtx) {
        m_codecCtx->opaque = this;
        m_codecCtx->get_format = &VideoPlayer::getHwFormatCallback;
        m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        qInfo() << "  HW decode enabled via D3D11VA";
        if (m_glPreview)
            m_glPreview->setSharedD3D11Device(sharedD3D11Device());
    } else {
        if (m_hwDeviceCtx) {
            av_buffer_unref(&m_hwDeviceCtx);
            m_hwDeviceCtx = nullptr;
        }
        qInfo() << "  HW decode unavailable, using software decoding";
        if (m_glPreview)
            m_glPreview->setSharedD3D11Device(nullptr);
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        resetDecoder();
        m_videoDisplay->setText("Failed to open codec");
        undotrace::log("loadFile:exit");
        return;
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    if (!m_packet || !m_frame || !m_swFrame) {
        resetDecoder();
        m_videoDisplay->setText("Failed to allocate decode buffers");
        undotrace::log("loadFile:exit");
        return;
    }
    qInfo() << "  packet/frame allocated";

    if (m_formatCtx->duration > 0) {
        m_durationUs = m_formatCtx->duration;
    } else {
        AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
        if (stream->duration > 0)
            m_durationUs = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
    }

    m_frameDurationUs = streamFrameDurationUs();
    m_displayAspectRatio = streamDisplayAspectRatio();
    qInfo() << "  duration=" << m_durationUs << "frameDur=" << m_frameDurationUs
            << "aspect=" << m_displayAspectRatio;
    if (m_glPreview)
        m_glPreview->setDisplayAspectRatio(
            m_projectOutputSize.isValid() ? effectiveDisplayAspectRatio()
                                          : m_displayAspectRatio);
    if (!m_suppressUiUpdates) {
        m_seekBar->setRange(0, sliderPositionForUs(m_durationUs));
        emit durationChanged(static_cast<double>(m_durationUs) / AV_TIME_BASE);
    }

    qInfo() << "  entering seekInternal(0)";
    if (!seekInternal(0, true, true)) {
        qWarning() << "seekInternal(0) failed";
        resetDecoder();
        m_videoDisplay->setText("Failed to decode first frame");
        undotrace::log("loadFile:exit");
        return;
    }
    qInfo() << "  seekInternal(0) ok";

    // NOTE: AudioMixer owns the audio path now. loadFile only touches the
    // video decoder; audio entry switching, mute, and seek are all driven
    // by setAudioSequence -> m_mixer.

    // If a sequence is active, restore its slider range so the seekbar shows
    // the full timeline rather than just this file's duration.
    if (sequenceActive()) {
        applySequenceSliderRange();
        emit durationChanged(static_cast<double>(m_sequenceDurationUs) / AV_TIME_BASE);
    }

    updatePositionUi();
    qInfo() << "VideoPlayer::loadFile END";
    undotrace::log("loadFile:exit");
}

void VideoPlayer::setSequence(const QVector<PlaybackEntry> &entries)
{
    undotrace::log("setSeq:enter");
    qInfo() << "VideoPlayer::setSequence count=" << entries.size();
    // ADAPTIVE-1: タイムライン編集 / undo / クリップ変更はすべて setSequence に
    // 集約される (sequenceChanged → MainWindow → setSequence)。世代カウンタを進め、
    // 旧世代の合成キャッシュを破棄する。これにより編集後に同一プレイヘッドへ戻った
    // 際に古い (編集前の) 合成フレームが誤って serve されることを防ぐ。
    ++m_timelineRevision;
    m_frameCache.invalidateRevision(m_timelineRevision);
    m_loggedCullState = false; // re-emit [cull] diagnostic for the new project
    // Phase 1e Win #16 (Iteration 9) — invalidate any pending preroll
    // de-dup. The new sequence may renumber entries, so the cached
    // prerolled index can point at an unrelated clip after reconciliation.
    m_prerolledEntryIdx = -1;
    // Iteration 10 — capture wasEmpty before the assignment below so the
    // empty -> non-empty auto-play branch at function tail can fire only on
    // the very first sequence delivery (app launch + first clip drop).
    // Subsequent re-emits (proxy promote, edit, scrub) already carry the
    // user's prior playing/paused state via Iteration 10 boundary auto-resume.
    const bool seqWasEmpty = m_sequence.isEmpty();

    // V3 sprint — preserve drag-edit target across sequenceChanged round-trips.
    // Timeline::setClipVideoTransform fires sequenceChanged on every drag event,
    // which lands here. If we just clear m_editTargetEntry, the second-and-later
    // emits of a drag gesture silently retarget to m_activeEntry (V1) and the
    // drag scales the wrong clip.
    int prevEditTrack = -1;
    int prevEditClip = -1;
    if (m_editTargetEntry >= 0 && m_editTargetEntry < m_sequence.size()) {
        const auto &prev = m_sequence[m_editTargetEntry];
        prevEditTrack = prev.sourceTrack;
        prevEditClip = prev.sourceClipIndex;
    }
    m_editTargetEntry = -1;    // cleared now; re-resolved on the new sequence below

    // Topmost-track-wins via single-decoder pipeline: take Timeline's
    // sequence as-is. Timeline::computePlaybackSequence sorts by
    // (timelineStart asc, sourceTrack asc), so findActiveEntryAt naturally
    // hits V1 first wherever V1 exists; in V1-empty gaps only V2 (or V3,
    // ...) matches — that's what lets seeks into V2-only regions load V2's
    // source instead of bouncing back to V1's head. The Stage 2 multi-layer
    // compositor will replace this with per-sourceTrack LayerDecoders.
    int64_t totalUs = 0;
    for (const auto &e : entries) {
        const int64_t entryEndUs = static_cast<int64_t>(e.timelineEnd * AV_TIME_BASE);
        if (entryEndUs > totalUs) totalUs = entryEndUs;
    }

    m_sequence = entries;
    const Timeline *timeline = m_glPreview ? m_glPreview->timeline() : nullptr;
    if (timeline) {
        for (PlaybackEntry &entry : m_sequence) {
            if (!entry.layerStyle.isIdentity())
                continue;
            if (const ClipInfo *clip = clipForPlaybackEntry(timeline, entry))
                entry.layerStyle = clip->layerStyle;
        }
    }
    m_sequenceDurationUs = totalUs;

    // Phase 1d pool reconciliation. For every active V2+ TrackDecoder, drop
    // it to the eviction grace pool when the (filePath, clipIn, sourceTrack)
    // key disappears from the new sequence. The grace TTL keeps lastFrameRgb
    // available for a few ticks after eviction so harvestOverlayLayer can
    // fall back to it while the replacement decoder catches up. The slot
    // manager release mirrors the qHash-based packing used in
    // acquireDecoderForClip.
    {
        QSet<TrackKey> newKeys;
        for (const auto &e : entries) {
            // Include every entry (V1 included). V1 enters the pool when
            // it isn't m_activeEntry — Scenario E: V2 starts earlier than
            // V1 so m_sequence sorts V2 first; the legacy decoder rides
            // V2 while V1 has to come from the pool to paint on top.
            // Excluding V1 here would force-evict its pool decoder on
            // every Timeline edit and the user sees a stutter every
            // drag/trim commit.
            newKeys.insert(TrackKey{e.filePath, qRound64(e.clipIn * 1000.0),
                                    e.sourceTrack, e.sourceClipIndex});
        }
        for (auto it = m_trackDecoders.begin(); it != m_trackDecoders.end(); ) {
            const TrackKey &k = it.key();
            if (newKeys.contains(k)) {
                ++it;
                continue;
            }
            TrackDecoder *d = it.value();
            const int slotClipId = static_cast<int>(qHash(k));
            m_slotManager.releaseSlot(slotClipId);
            m_slotIdToKey.remove(slotClipId);
            it = m_trackDecoders.erase(it);
            if (d) {
                d->graceTtlTicks = 4;
                m_evictionGracePool.append(d);
            }
        }
    }

    if (entries.isEmpty()) {
        // Timeline emptied (all clips deleted). Pause and clear the slider so
        // the player doesn't keep ticking against a stale file. We don't tear
        // down the decoder — preview path may still be using it.
        // V3 sprint visibility-toggle fix: keep m_timelinePositionUs intact so
        // toggling the only visible video track OFF (which produces an empty
        // sequence) doesn't reset the playhead to 0; toggling back ON resumes
        // at the previous position.
        m_activeEntry = -1;
        if (m_playing) pause();
        // V3 sprint visibility-toggle fix (part 2): block the seek bar's
        // valueChanged → seek(0) side effect. Shrinking the range to (0,0)
        // forces the slider value to 0, and the valueChanged handler
        // (ctor) would otherwise call seek(0) — which emits positionChanged(0)
        // (snapping the Timeline playhead/timebar back to the start) AND
        // overwrites m_timelinePositionUs to 0, defeating the position
        // preservation intended above so re-showing the only video track no
        // longer resumes at the prior playhead. Mirrors the QSignalBlocker
        // already guarding setValue() in updatePositionUi()/seek-commit.
        {
            const QSignalBlocker seekBarBlocker(m_seekBar);
            m_seekBar->setRange(0, 0);
        }
        emit durationChanged(0.0);
        // US-T35 clear any OBS-style transform so a fresh import starts at
        // identity instead of inheriting the last clip's scale/offset.
        if (m_glPreview)
            m_glPreview->resetVideoSourceTransform();
        // Drop the cached compositor base on Timeline-empty too.
        // Otherwise a re-import after clearing keeps the previous V1 frame
        // around and the first compositor tick on the new clip flashes
        // the stale picture (paired with resetDecoder's clear).
        m_lastV1RawFrame = QImage();
        // Defect-2 fix: 空タイムラインは黒プレビューでなければならない。
        // 以前は m_lastSourceFrame が残り、GL プレビューも最後にアップロードした
        // テクスチャを保持したままだった。直前の resetVideoSourceTransform() が
        // アスペクト/コンテンツインセット補正を外すため、その stale テクスチャが
        // アスペクト無視で引き伸ばし描画され、「undo で全クリップが消えたのに縦伸び
        // 映像が再生され続ける」残像になっていた。source キャッシュをクリアし
        // (refreshDisplayedFrame の null ガードで再描画を抑止)、GL プレビューを
        // 黒フレームで上書きして stale 表示を断つ。可視トラックを全 OFF にした
        // ケースでも「非表示 = 黒」で正しく、m_timelinePositionUs は不変。
        m_lastSourceFrame = QImage();
        if (m_glPreview) {
            const QSize blankSize = m_projectOutputSize.isValid()
                ? m_projectOutputSize
                : QSize(m_canvasWidth  > 0 ? m_canvasWidth  : 16,
                        m_canvasHeight > 0 ? m_canvasHeight : 16);
            QImage blank(blankSize, QImage::Format_ARGB32_Premultiplied);
            blank.fill(Qt::black);
            m_glPreview->displayFrame(blank);
        }
        undotrace::log("setSeq:exit");
        return;
    }

    // Slider range follows the sequence total.
    applySequenceSliderRange();
    emit durationChanged(static_cast<double>(m_sequenceDurationUs) / AV_TIME_BASE);

    // Clamp current timeline position into the new sequence and pick an
    // active entry. Default to 0 if we have no prior position (e.g. first add).
    int64_t clamped = qBound<int64_t>(0, m_timelinePositionUs, m_sequenceDurationUs);
    int desiredIdx = findActiveEntryAt(clamped);
    if (desiredIdx < 0) {
        // V3 sprint visibility-toggle fix: don't snap to first entry's head
        // (which would reset to 0 when first entry starts at timeline 0).
        // Try to find an entry whose range encompasses or is closest to the
        // current playhead, preserving user position across visibility toggles.
        int bestIdx = 0;
        qint64 bestDistance = std::numeric_limits<qint64>::max();
        for (int i = 0; i < m_sequence.size(); ++i) {
            const auto &e = m_sequence[i];
            const qint64 startUs = static_cast<qint64>(e.timelineStart * AV_TIME_BASE);
            const qint64 endUs   = static_cast<qint64>(e.timelineEnd   * AV_TIME_BASE);
            if (clamped >= startUs && clamped <= endUs) {
                bestIdx = i;
                bestDistance = 0;
                break;
            }
            const qint64 d = qMin(qAbs(clamped - startUs), qAbs(clamped - endUs));
            if (d < bestDistance) {
                bestDistance = d;
                bestIdx = i;
            }
        }
        desiredIdx = bestIdx;
        // Clamp into the chosen entry's range so the playhead stays inside a
        // valid clip segment.
        const auto &chosen = m_sequence[bestIdx];
        const qint64 cStart = static_cast<qint64>(chosen.timelineStart * AV_TIME_BASE);
        const qint64 cEnd   = static_cast<qint64>(chosen.timelineEnd   * AV_TIME_BASE);
        clamped = qBound(cStart, clamped, cEnd);
    }
    desiredIdx = previewPrimaryEntryIndexAt(timeline, m_sequence, desiredIdx, clamped);

    const auto &target = m_sequence[desiredIdx];
    const bool needFileSwitch = (target.filePath != m_loadedFilePath) || !m_formatCtx;
    // Idempotency: if the structurally identical entry is already active and
    // the file is already loaded, skip the seek entirely so back-to-back
    // sequenceChanged emissions (e.g. video track + audio track both
    // emitting modified() during a single addClip) don't repeatedly disturb
    // the mixer and cause audible artifacts.
    const bool entryStructurallyChanged = needFileSwitch || (m_activeEntry != desiredIdx);

    // Snapshot playback state BEFORE loadFile+resetDecoder tear down the
    // playback timer, so we can resurrect it after the file swap completes.
    const bool wasPlaying = m_playing;
    qInfo() << "VideoPlayer::setSequence flow"
            << "wasPlaying=" << wasPlaying
            << "needFileSwitch=" << needFileSwitch
            << "entryStructurallyChanged=" << entryStructurallyChanged
            << "desiredIdx=" << desiredIdx
            << "m_activeEntry(before)=" << m_activeEntry;

    if (needFileSwitch) {
        const bool prevRetain = m_retainLastFrameAcrossLoad;
        m_retainLastFrameAcrossLoad = true;   // preserve m_lastV1RawFrame during undo file-switch
        undotrace::log("setSeq:beforeLoadFile");
        loadFile(target.filePath);  // resets decoder, clears m_playing
        undotrace::log("setSeq:afterLoadFile");
        m_retainLastFrameAcrossLoad = prevRetain;
        if (wasPlaying)
            m_playing = true;
    }

    m_activeEntry = desiredIdx;
    m_timelinePositionUs = clamped;

    if (entryStructurallyChanged) {
        const int64_t localUs = entryLocalPositionUs(desiredIdx, clamped);
        undotrace::log("setSeq:beforeSeek");
        seekInternal(localUs, true, true);
        undotrace::log("setSeq:afterSeek");
    }

    // If a sequenceChanged arrived mid-playback (e.g. a linked-clip drag was
    // released while playing), loadFile+resetDecoder halted the playback
    // timer. Resurrect it here — without this the player sits in a zombie
    // state where m_playing=true but nothing actually ticks, which looks
    // like the pause button stopped responding.
    if (wasPlaying && needFileSwitch) {
        qInfo() << "VideoPlayer::setSequence resurrecting playback timer";
        scheduleNextFrame();
    }
    // Audio-side resume is handled by setAudioSequence (called separately
    // via MainWindow's audioSequenceChanged wiring) and by the per-tick
    // Mixer's master clock + decode thread.

    // Keep the play/pause button enabled states in sync with m_playing.
    // Without this, the button may show the wrong enabled state after we
    // restore m_playing post-loadFile — and the user experiences a "dead"
    // pause button even though the click is wired.
    updatePlayButton();

    // Re-resolve edit target on the new sequence. setEditTargetByClip falls
    // back to -1 if no matching clip exists (project change / clip deleted).
    if (prevEditTrack >= 0 && prevEditClip >= 0) {
        setEditTargetByClip(prevEditTrack, prevEditClip);
    }

    updatePositionUi();

    // Iteration 10 — auto-play on the first non-empty sequence delivery.
    // User-accepted side effect of the boundary auto-resume request:
    // "アプリ立ち上げて最初にクリップ貼った時も自動で再生始まるかも". The
    // QTimer::singleShot defers play() to the next event loop tick so any
    // pending loadFile / seekInternal(0) inside this setSequence call has
    // settled before play() arms the playback timer (avoids the cold-open
    // !advanced safety-net that motivated Fix J in the first place).
    static const bool kAutoplayDisabled =
        qEnvironmentVariableIntValue("VEDITOR_AUTOPLAY_ON_FIRST_SEQUENCE_DISABLE") != 0;
    // Iteration 12: gate auto-play behind a user preference (default OFF).
    // User feedback after Iteration 10 shipped: the auto-play side effect on
    // first clip drop was useful but should be opt-in via 環境設定 menu, not
    // forced on. The envvar override still wins (force-disable for CI/test
    // automation); the QSettings key is the user-facing toggle.
    const bool autoplayPref = QSettings("VSimpleEditor", "Preferences")
        .value("autoPlayOnFirstSequence", false).toBool();
    if (!kAutoplayDisabled && autoplayPref
        && seqWasEmpty && !entries.isEmpty() && !m_playing) {
        // NIT-1 (architect Iteration 10): re-check guard inside the lambda
        // because the user could delete the just-dropped clip before the
        // next event loop tick, leaving an empty sequence that would
        // otherwise generate one spurious tick + redundant pause() chain.
        QTimer::singleShot(0, this, [this]() {
            if (!m_playing && !m_sequence.isEmpty()) play();
        });
    }
    undotrace::log("setSeq:exit");
}

void VideoPlayer::setAudioSequence(const QVector<PlaybackEntry> &entries)
{
    qInfo() << "VideoPlayer::setAudioSequence count=" << entries.size();
    const bool hadEntries = m_audioSequenceHadEntries;
    m_audioSequenceHadEntries = !entries.isEmpty();
    if (!m_mixer) return;
    m_mixer->setSequence(entries);
    if (entries.isEmpty()) {
        m_mixer->stop();
        return;
    }
    // Don't seek on every re-emit: AudioMixer::setSequence already flushes
    // rings + sets seekPending for any entry whose timeline range moved,
    // and an unconditional seek here would close+reopen the QAudioSink on
    // every volume slider drag or mute toggle (audible click). Only seek
    // when transitioning empty->non-empty so the mixer lands on the
    // current playhead the first time it gets a schedule.
    if (!hadEntries) {
        m_mixer->seekTo(m_timelinePositionUs);
    }
    if (m_playing && m_playbackSpeed >= 0.0) {
        m_mixer->play();
    } else {
        m_mixer->pause();
    }
}

void VideoPlayer::setMuted(bool muted)
{
    if (m_muted == muted) return;
    m_muted = muted;
    if (!m_mixer) return;
    const int trackCount = m_audioSequenceHadEntries ? 16 : 1;
    for (int i = 0; i < trackCount; ++i) {
        m_mixer->setTrackMute(i, muted);
    }
}

// US-INT-2 Phase A: store per-entry speed ramps parallel to m_sequence.
// The active consumer is entryLocalPositionUs (video reprojection); audio
// time-stretching is deferred to Phase B. Caller (MainWindow) MUST invoke
// setSpeedRamps in lockstep with setSequence to keep parallel arrays
// consistent. A size mismatch is logged but tolerated — entryLocalPositionUs
// guards out-of-range indices and falls back to identity.
void VideoPlayer::setSpeedRamps(const QVector<speedramp::SpeedRamp> &ramps)
{
    qInfo() << "VideoPlayer::setSpeedRamps count=" << ramps.size()
            << "(sequence size=" << m_sequence.size() << ")";
    m_speedRamps = ramps;
    if (m_speedRamps.size() != m_sequence.size()) {
        qWarning()
            << "VideoPlayer::setSpeedRamps SIZE MISMATCH — entries past"
            << m_speedRamps.size() << "fall back to identity ramp";
    }
}

int VideoPlayer::findActiveEntryAt(int64_t timelineUs) const
{
    if (m_sequence.isEmpty()) return -1;
    const double tSec = static_cast<double>(timelineUs) / AV_TIME_BASE;
    for (int i = 0; i < m_sequence.size(); ++i) {
        const auto &e = m_sequence[i];
        if (tSec >= e.timelineStart && tSec < e.timelineEnd)
            return i;
    }
    // At or past the very end → last entry.
    if (tSec >= m_sequence.last().timelineEnd - 1e-6)
        return m_sequence.size() - 1;
    // Before the first → first.
    if (tSec < m_sequence.first().timelineStart)
        return 0;
    // Gap between clips — snap forward to the NEXT clip. Without this, a
    // 1-frame step that lands on / just past clip[i].timelineEnd but
    // before clip[i+1].timelineStart returned -1 → caller's fallback
    // collapsed it to idx=0 (or the last clamped position) and the user
    // got stuck at the boundary. Forward stepping was the visible victim
    // because backward-into-a-loaded-prev-clip happened to switch files
    // anyway via needSwitch, but the seek still failed — same root cause.
    for (int i = 0; i < m_sequence.size() - 1; ++i) {
        if (tSec >= m_sequence[i].timelineEnd
            && tSec < m_sequence[i + 1].timelineStart) {
            return i + 1;
        }
    }
    return -1;
}

QVector<int> VideoPlayer::findActiveEntriesAt(int64_t timelineUs) const
{
    QVector<int> result;
    if (m_sequence.isEmpty()) return result;
    const double tSec = static_cast<double>(timelineUs) / AV_TIME_BASE;
    for (int i = 0; i < m_sequence.size(); ++i) {
        const auto &e = m_sequence[i];
        if (tSec >= e.timelineStart && tSec < e.timelineEnd)
            result.append(i);
    }
    return result;
}

int64_t VideoPlayer::entryLocalPositionUs(int entryIdx, int64_t timelineUs) const
{
    if (entryIdx < 0 || entryIdx >= m_sequence.size()) return 0;
    const auto &e = m_sequence[entryIdx];
    const double tSec = static_cast<double>(timelineUs) / AV_TIME_BASE;
    const double offsetIntoEntry = qMax(0.0, tSec - e.timelineStart);
    const double speed = (e.speed > 0.0) ? e.speed : 1.0;
    // US-INT-2 Phase A: ramp-aware reprojection. The legacy uniform
    // `clip.speed` (carried on PlaybackEntry as `e.speed`) and the
    // per-clip SpeedRamp curve compose multiplicatively — first scale
    // timeline-time into the already-uniformly-stretched clip frame
    // (offsetIntoEntry * speed), then bend that linear axis through the
    // ramp via timelineToSourceUs(). Identity ramp short-circuits to
    // the legacy uniform formula so identity clips and clips on builds
    // without ramps are bit-exact unchanged. The size guard handles the
    // brief window after setSequence runs but before setSpeedRamps lands.
    const bool haveRamp = (entryIdx < m_speedRamps.size())
                       && !m_speedRamps[entryIdx].isIdentity();
    if (!haveRamp) {
        const double localSec = e.clipIn + offsetIntoEntry * speed;
        return static_cast<int64_t>(localSec * AV_TIME_BASE);
    }
    const qint64 scaledTlUs =
        static_cast<qint64>(offsetIntoEntry * speed * AV_TIME_BASE);
    const qint64 srcOffsetUs =
        m_speedRamps[entryIdx].timelineToSourceUs(scaledTlUs);
    const int64_t clipInUs = static_cast<int64_t>(e.clipIn * AV_TIME_BASE);
    return clipInUs + srcOffsetUs;
}

int64_t VideoPlayer::fileLocalToTimelineUs(int entryIdx, int64_t fileLocalUs) const
{
    if (entryIdx < 0 || entryIdx >= m_sequence.size()) return 0;
    const auto &e = m_sequence[entryIdx];
    const double fileLocalSec = static_cast<double>(fileLocalUs) / AV_TIME_BASE;
    const double speed = (e.speed > 0.0) ? e.speed : 1.0;
    // US-INT-2 Phase B: ramp-aware inverse. Mirror the entryLocalPositionUs
    // forward path (offsetIntoEntry * speed → timelineToSourceUs → +clipIn).
    // Mathematical inverse modulo ~1 us integer truncation; stepForward's
    // qMax(...+1, projected) floor absorbs that residual on the user surface.
    // Identity ramps short-circuit to the legacy uniform formula so identity
    // clips and clips on builds without ramps remain bit-exact unchanged.
    const bool haveRamp = (entryIdx < m_speedRamps.size())
                       && !m_speedRamps[entryIdx].isIdentity();
    if (!haveRamp) {
        const double offsetIntoEntry = qMax(0.0, (fileLocalSec - e.clipIn) / speed);
        const double timelineSec = e.timelineStart + offsetIntoEntry;
        return static_cast<int64_t>(timelineSec * AV_TIME_BASE);
    }
    const int64_t clipInUs = static_cast<int64_t>(e.clipIn * AV_TIME_BASE);
    const qint64 srcOffsetUs = qMax<qint64>(0, fileLocalUs - clipInUs);
    const qint64 scaledTlUs =
        m_speedRamps[entryIdx].sourceToTimelineUs(srcOffsetUs);
    const double offsetIntoEntry =
        qMax(0.0, static_cast<double>(scaledTlUs) / speed / AV_TIME_BASE);
    const double timelineSec = e.timelineStart + offsetIntoEntry;
    return static_cast<int64_t>(timelineSec * AV_TIME_BASE);
}

void VideoPlayer::applySequenceSliderRange()
{
    const int64_t sliderMaxMs = qMin<int64_t>(m_sequenceDurationUs / 1000,
                                              std::numeric_limits<int>::max());
    m_seekBar->setRange(0, static_cast<int>(sliderMaxMs));
}

int VideoPlayer::sliderTimelinePosition(int64_t timelineUs) const
{
    const int64_t ms = qMax<int64_t>(0, timelineUs / 1000);
    return static_cast<int>(qMin<int64_t>(ms, std::numeric_limits<int>::max()));
}

bool VideoPlayer::seekToTimelineUs(int64_t timelineUs, bool precise)
{
    if (m_sequence.isEmpty()) return false;
    timelineUs = qBound<int64_t>(0, timelineUs, m_sequenceDurationUs);
    int idx = findActiveEntryAt(timelineUs);
    if (idx < 0) idx = 0;
    if (idx >= m_sequence.size()) return false;
    idx = previewPrimaryEntryIndexAt(
        m_glPreview ? m_glPreview->timeline() : nullptr,
        m_sequence,
        idx,
        timelineUs);
    if (idx < 0 || idx >= m_sequence.size()) return false;

    const auto &e = m_sequence[idx];
    const bool needSwitch = (e.filePath != m_loadedFilePath) || !m_formatCtx;
    // Freeze UI updates across the loadFile → seek chain so the slider
    // doesn't flash back to 0 while the intermediate seekInternal(0) runs
    // inside loadFile. We explicitly call updatePositionUi at the end.
    const bool prevSuppress = m_suppressUiUpdates;
    m_suppressUiUpdates = needSwitch;
    const bool wasPlayingOuter = m_playing;
    if (needSwitch) {
        // Phase 1e Win #16 (Iteration 9) — try hot-swap before legacy
        // loadFile. When the target entry's TrackDecoder is already warm
        // (V2+ pool entries usually are; V1 becomes warm via boundary
        // preroll), the swap is a sub-ms pointer move and the entire
        // resetDecoder→avformat_open_input chain is skipped.
        if (!tryPromotePoolDecoderTo(idx)) {
            // Phase 1e Win #15 (Fix M) — same retention rationale as
            // advanceToEntry: keep the displayed frame across the loadFile
            // latency so the cross-file seek doesn't flash a black GLPreview.
            m_retainLastFrameAcrossLoad = true;
            loadFile(e.filePath); // reloads the video decoder; audio routes through the mixer
            m_retainLastFrameAcrossLoad = false;
        }
        if (wasPlayingOuter)
            m_playing = true;
    }

    m_activeEntry = idx;
    m_timelinePositionUs = timelineUs;
    const int64_t localUs = entryLocalPositionUs(idx, timelineUs);
    const bool ok = seekInternal(localUs, true, precise);
    // Push the seeked-to clip's own transform so cross-clip seeks don't
    // inherit the previously-active clip's OBS-style scale/offset.
    // V3 sprint fix (architect NIT) — prefer edit-target transform on seek so
    // a seek while V3 is selected doesn't briefly flash V1's transform.
    if (m_glPreview) {
        const int displayIdx = (m_editTargetEntry >= 0
                                && m_editTargetEntry < m_sequence.size())
                               ? m_editTargetEntry
                               : idx;
        const auto &targetE = m_sequence[displayIdx];
        m_glPreview->setVideoSourceTransform(targetE.videoScale, targetE.videoDx, targetE.videoDy);
        // Drop composite-baked mode here too: a seek-while-paused that
        // followed a composite tick (flag = true) would otherwise render
        // the seek-displayed raw V1 frame at viewport identity even
        // though it now needs the entry's scale applied. The next
        // playback tick re-sets the flag based on willComposite, so this
        // is always safe.
        m_glPreview->setCompositeBakedMode(false);
    }
    m_suppressUiUpdates = prevSuppress;

    // resetDecoder (called from loadFile when needSwitch) ran updatePlayButton
    // with m_playing=false. Resurrect the video playback timer.
    if (wasPlayingOuter && needSwitch) {
        scheduleNextFrame();
    }
    // Audio side stays in step via the mixer's master clock — a seek
    // updates m_writeCursorUs and lazily re-seeks every active entry.
    if (m_mixer) m_mixer->seekTo(timelineUs);
    updatePlayButton();

    updatePositionUi();
    return ok;
}

bool VideoPlayer::advanceToEntry(int newEntryIdx)
{
    if (newEntryIdx < 0 || newEntryIdx >= m_sequence.size())
        return false;
    const qint64 nextTimelineUs =
        static_cast<qint64>(m_sequence.at(newEntryIdx).timelineStart * AV_TIME_BASE);
    newEntryIdx = previewPrimaryEntryIndexAt(
        m_glPreview ? m_glPreview->timeline() : nullptr,
        m_sequence,
        newEntryIdx,
        nextTimelineUs);
    if (newEntryIdx < 0 || newEntryIdx >= m_sequence.size())
        return false;

    const auto &next = m_sequence[newEntryIdx];
    qInfo() << "VideoPlayer::advanceToEntry idx=" << newEntryIdx
            << "file=" << next.filePath
            << "timelineStart=" << next.timelineStart
            << "clipIn=" << next.clipIn;

    const bool wasPlaying = m_playing;
    const bool needSwitch = (next.filePath != m_loadedFilePath);
    // Suppress intermediate UI updates across the loadFile → seek chain so the
    // slider doesn't flash back to 0 while resetDecoder/seekInternal(0) inside
    // loadFile temporarily clear the slider range.
    const bool prevSuppress = m_suppressUiUpdates;
    m_suppressUiUpdates = needSwitch;
    if (needSwitch) {
        // Phase 1e Win #16 (Iteration 9) — try the Premiere Pro-style
        // hot-swap first. When handlePlaybackTick prerolled the next
        // entry's pool decoder before the boundary, the swap is a sub-ms
        // pointer move and the loadFile / resetDecoder chain (with its
        // 60-200 ms stall + black-frame window) is bypassed entirely.
        // Fall back to legacy loadFile when no warm pool decoder exists.
        if (!tryPromotePoolDecoderTo(newEntryIdx)) {
            // Phase 1e Win #15 (Fix M) — keep the previous clip's last
            // displayed frame alive through resetDecoder so the GLPreview
            // doesn't flash black/empty during the loadFile latency window
            // (~60-145 ms for a typical proxy). Cleared again right after.
            m_retainLastFrameAcrossLoad = true;
            loadFile(next.filePath); // loadFile only reloads the video decoder; audio is driven by setAudioSequence
            m_retainLastFrameAcrossLoad = false;
        }
    }

    m_activeEntry = newEntryIdx;
    // US-T35 apply the new entry's per-clip video source transform to the
    // GL preview so each clip keeps its own scale/offset.
    // V3 sprint fix (architect NIT) — prefer edit-target transform on boundary
    // cross so a seek/advance while V3 is selected doesn't briefly flash V1's transform.
    if (m_glPreview) {
        const int displayIdx = (m_editTargetEntry >= 0
                                && m_editTargetEntry < m_sequence.size())
                               ? m_editTargetEntry
                               : newEntryIdx;
        const auto &targetE = m_sequence[displayIdx];
        m_glPreview->setVideoSourceTransform(targetE.videoScale, targetE.videoDx, targetE.videoDy);
        // Match the seek path's defensive un-bake — a paused boundary
        // crossing followed by a single-clip displayFrame would otherwise
        // render at viewport identity if the prior tick was composite.
        m_glPreview->setCompositeBakedMode(false);
    }
    m_timelinePositionUs = static_cast<int64_t>(next.timelineStart * AV_TIME_BASE);
    const int64_t startLocalUs = static_cast<int64_t>(next.clipIn * AV_TIME_BASE);
    if (!seekInternal(startLocalUs, true, true)) {
        m_suppressUiUpdates = prevSuppress;
        return false;
    }

    // Audio is driven by the AudioMixer's master clock now — re-anchor it
    // so the new entry's local position is reached on the next refill tick.
    m_timelinePositionUs = static_cast<int64_t>(next.timelineStart * AV_TIME_BASE);

    // Run the auto-resume play() BEFORE the mixer seekTo below. AudioMixer
    // dedup branches all gate on m_playing.load(); calling seekTo first when
    // playback was paused mid-boundary would land in the unguarded full
    // sinkSnap->reset()+start() path and produce an OS-level click.
    //
    // Iteration 10 — auto-resume invariant on clip boundary. User report:
    // "クリップの切り替えの所で止まる、クリップの最初所に来たら自動で再生
    // される様には出来る？" Even with Iteration 9 hot-swap clean, a
    // subtle race in the playback chain (AudioMixer state callback,
    // scheduleNextFrame !advanced safety-net, downstream UI signal) can
    // still flip m_playing back to false during the transition. Forcing
    // wasPlaying invariant here delivers the user's stated guarantee:
    // landing on a clip's first frame means playback continues. We bypass
    // play() to avoid Fix J's 200ms debounce + Fix L's re-anchor logic
    // (m_activeEntry is already correct so re-anchor is unnecessary).
    static const bool kBoundaryAutoresumeDisabled =
        qEnvironmentVariableIntValue("VEDITOR_BOUNDARY_AUTORESUME_DISABLE") != 0;
    if (!kBoundaryAutoresumeDisabled && wasPlaying) {
        const bool wasNotPlaying = !m_playing;
        m_playing = true;
        if (wasNotPlaying) emit stateChanged(true);
        if (m_mixer) m_mixer->play();
        scheduleNextFrame();
    }

    if (m_mixer) m_mixer->seekTo(m_timelinePositionUs);

    if (wasPlaying)
        m_playing = true;
    m_suppressUiUpdates = prevSuppress;
    // resetDecoder (called from loadFile) ran updatePlayButton with
    // m_playing=false and disabled the pause button. After we restore
    // m_playing above, sync the button enabled state again — otherwise
    // the user sees the pause button as un-clickable across every clip
    // boundary that crosses a file switch.
    updatePlayButton();
    updatePositionUi();
    return true;
}

bool VideoPlayer::tryPromotePoolDecoderTo(int newEntryIdx)
{
    // Phase 1e Win #16 (Iteration 9) — Premiere Pro-style decoder hot-swap.
    // Iterations 5-8 (Fix J/K/L/M) layered debounce / dedup / re-anchor /
    // last-frame-retention on top of advanceToEntry's loadFile call,
    // because that call took 60-200 ms on the main thread and forced the
    // UI / AudioMixer / GL pipeline into a series of stop-restart cycles
    // that each had their own race conditions. The actual fix is to
    // remove the loadFile from the boundary path entirely: when the next
    // entry's TrackDecoder has already been opened by handlePlaybackTick's
    // boundary preroll (or by V2+ overlay rendering), move its
    // formatCtx/codecCtx/frame/swFrame/packet straight into the V1 primary
    // slots and demote the old primary to the eviction grace pool. The
    // sub-ms pointer swap eliminates the underlying race.
    static const bool kHotSwapDisabled =
        qEnvironmentVariableIntValue("VEDITOR_HOTSWAP_DISABLE") != 0;
    if (kHotSwapDisabled) return false;
    if (newEntryIdx < 0 || newEntryIdx >= m_sequence.size()) return false;

    const PlaybackEntry &next = m_sequence[newEntryIdx];
    const TrackKey nextKey{next.filePath, qRound64(next.clipIn * 1000.0),
                           next.sourceTrack, next.sourceClipIndex};

    auto it = m_trackDecoders.find(nextKey);
    if (it == m_trackDecoders.end()) return false;

    TrackDecoder *target = it.value();
    if (!target || !target->formatCtx || !target->codecCtx) return false;

    QElapsedTimer swapTimer;
    swapTimer.start();
    const int prevEntry = m_activeEntry;

    // Release the slot the pool decoder was holding — primary doesn't go
    // through DecoderSlotManager (V1's slot is intentionally unmanaged).
    const int targetSlotId = static_cast<int>(qHash(nextKey));
    m_slotManager.releaseSlot(targetSlotId);
    m_slotIdToKey.remove(targetSlotId);
    m_trackDecoders.erase(it);

    // Demote the old primary into the eviction grace pool so any in-flight
    // tick that still has a stale pointer to its codec can finish before
    // the heavy avformat_close_input runs (graceTtlTicks=4 ≈ 4 ticks).
    if (prevEntry >= 0 && prevEntry < m_sequence.size()
        && m_formatCtx && m_codecCtx) {
        const PlaybackEntry &old = m_sequence[prevEntry];
        TrackDecoder *evicted = new TrackDecoder();
        evicted->sourceClipIndex = old.sourceClipIndex;
        evicted->sourceTrack = old.sourceTrack;
        evicted->filePath = m_loadedFilePath;
        evicted->clipIn = old.clipIn;
        evicted->formatCtx = m_formatCtx;
        evicted->codecCtx = m_codecCtx;
        evicted->swsCtx = m_swsCtx;
        evicted->frame = m_frame;
        evicted->swFrame = m_swFrame;
        evicted->packet = m_packet;
        evicted->videoStreamIndex = m_videoStreamIndex;
        evicted->hwPixFmt = m_hwPixFmt;
        evicted->durationUs = m_durationUs;
        evicted->frameDurationUs = m_frameDurationUs;
        evicted->displayAspect = m_displayAspectRatio;
        evicted->currentPositionUs = m_currentPositionUs;
        evicted->lastFrameRgb = m_lastV1RawFrame;
        evicted->firstFrameDecoded = !m_lastV1RawFrame.isNull();
        evicted->graceTtlTicks = 4;

        // Cap the grace pool at 4 — same policy as the existing
        // acquireDecoderForClip eviction path. Pop the oldest if full.
        while (m_evictionGracePool.size() >= 4) {
            TrackDecoder *oldest = m_evictionGracePool.takeFirst();
            if (oldest) {
                closeTrackDecoder(oldest);
                delete oldest;
            }
        }
        m_evictionGracePool.append(evicted);
    } else {
        // No prior primary to demote — drop any stray state.
        if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
        if (m_frame) av_frame_free(&m_frame);
        if (m_swFrame) av_frame_free(&m_swFrame);
        if (m_packet) av_packet_free(&m_packet);
        if (m_codecCtx) avcodec_free_context(&m_codecCtx);
        if (m_formatCtx) avformat_close_input(&m_formatCtx);
    }

    // Move target's contexts into primary slots. target's codecCtx already
    // has hw_device_ctx ref'd to m_sharedPoolHwDeviceCtx; legacy m_hwDeviceCtx
    // stays separate and continues to ref V1-only HW state. GLPreview's
    // interop binding is updated explicitly below (NIT-1) so the cache
    // re-keys on the pool device — sharedD3D11Device()'s legacy-first
    // fallback would otherwise leave the cache pointing at m_hwDeviceCtx.
    m_formatCtx = target->formatCtx;
    m_codecCtx = target->codecCtx;
    // swsCtx is canvas-size-dependent — pool's may not match the primary
    // canvas. Free target's, leave m_swsCtx null so frameToImage rebuilds.
    if (target->swsCtx) sws_freeContext(target->swsCtx);
    m_swsCtx = nullptr;
    m_frame = target->frame;
    m_swFrame = target->swFrame;
    m_packet = target->packet;
    m_videoStreamIndex = target->videoStreamIndex;
    m_hwPixFmt = target->hwPixFmt;
    m_durationUs = target->durationUs;
    m_frameDurationUs = target->frameDurationUs;
    m_displayAspectRatio = target->displayAspect;
    m_currentPositionUs = target->currentPositionUs;
    m_loadedFilePath = next.filePath;

    // The pool's get_format callback was poolGetHwFormatCallback with
    // opaque == target. We delete `target` below; a stray re-probe would
    // dereference a dangling pointer. Codec is already open so get_format
    // normally won't fire again, but null the opaque defensively.
    if (m_codecCtx)
        m_codecCtx->opaque = nullptr;

    // Rebuild HDR metadata from the new primary's codec parameters,
    // mirroring loadFile's logic so downstream code sees identical fields.
    m_hdrInfo = {};
    if (m_formatCtx && m_videoStreamIndex >= 0
        && m_videoStreamIndex < static_cast<int>(m_formatCtx->nb_streams)) {
        auto *codecpar = m_formatCtx->streams[m_videoStreamIndex]->codecpar;
        m_hdrInfo.primaries = codecpar->color_primaries;
        m_hdrInfo.trc = codecpar->color_trc;
        m_hdrInfo.colorspace = codecpar->color_space;
        // Per-component (luma) depth — see loadFile note. pixfmtdepth is
        // null-safe and subsampling-correct (was av_get_bits_per_pixel/3).
        m_hdrInfo.bitDepth = pixfmtdepth::bitDepthFromPixFmt(codecpar->format);
        m_hdrInfo.isHdr = (codecpar->color_trc == AVCOL_TRC_SMPTE2084
                           || codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67);
    }

    // UI updates that loadFile would normally fire — seekbar range and
    // duration signal anchored to the freshly-active file.
    if (!m_suppressUiUpdates) {
        m_seekBar->setRange(0, sliderPositionForUs(m_durationUs));
        if (sequenceActive()) {
            applySequenceSliderRange();
            emit durationChanged(static_cast<double>(m_sequenceDurationUs) / AV_TIME_BASE);
        } else {
            emit durationChanged(static_cast<double>(m_durationUs) / AV_TIME_BASE);
        }
    }
    if (m_glPreview) {
        m_glPreview->setDisplayAspectRatio(
            m_projectOutputSize.isValid() ? effectiveDisplayAspectRatio()
                                          : m_displayAspectRatio);
        // Architect NIT-1: route GLPreview to the device backing the NEW
        // primary (pool D3D11Device*), not sharedD3D11Device()'s default
        // preference for legacy m_hwDeviceCtx. Without this override the
        // first ~5-10 post-swap frames see GLPreview's interop cache
        // keyed on the wrong D3D11Device pointer and miss the GL_BGRA
        // fast path. flushInteropCache drops stale texture handles bound
        // to the legacy device so the next paint rebuilds against the
        // pool device cleanly.
        void *poolDevice = veditor_avHwDeviceCtxToD3D11Device(m_sharedPoolHwDeviceCtx);
        if (poolDevice) {
            m_glPreview->flushInteropCache();
            m_glPreview->setSharedD3D11Device(poolDevice);
        } else {
            m_glPreview->setSharedD3D11Device(sharedD3D11Device());
        }
    }

    // target shell is empty — its inner contexts moved to primary or were
    // freed above; safe to delete the wrapper.
    delete target;

    qInfo() << "VideoPlayer::tryPromotePoolDecoderTo SWAPPED entry"
            << prevEntry << "->" << newEntryIdx
            << "file=" << next.filePath
            << "elapsed=" << swapTimer.elapsed() << "ms";
    return true;
}

void VideoPlayer::setCanvasSize(int width, int height)
{
    m_canvasWidth = width;
    m_canvasHeight = height;
    double ar = static_cast<double>(width) / height;
    // Per-canvas floor matches the constructor minimum so loading a clip
    // never grows the floor back up and trapping the user above 240x135.
    m_videoDisplay->setMinimumSize(
        qMin(240, width / 2),
        qMin(135, height / 2));
    if (m_currentFrameImage.isNull()) {
        QString orientation = (ar > 1.0) ? "Landscape" : (ar < 1.0) ? "Portrait" : "Square";
        m_videoDisplay->setText(QString("%1x%2 %3\nDrop a video file or use File > Open")
            .arg(width).arg(height).arg(orientation));
    } else {
        if (m_projectOutputSize.isValid()) {
            if (!m_pendingSizeRefresh) {
                m_pendingSizeRefresh = true;
                QTimer::singleShot(0, this, [this]() {
                    m_pendingSizeRefresh = false;
                    refreshDisplayedFrame();
                });
            }
        } else {
            refreshDisplayedFrame();   // default project: synchronous, bit-identical
        }
    }
}

void VideoPlayer::setProjectOutputSize(const QSize &size)
{
    const QSize normalized = size.isValid() ? size : QSize();
    if (m_projectOutputSize == normalized)
        return;
    m_projectOutputSize = normalized;
    if (m_glPreview)
        m_glPreview->setDisplayAspectRatio(
            m_projectOutputSize.isValid() ? effectiveDisplayAspectRatio()
                                          : m_displayAspectRatio);
    if (m_projectOutputSize.isValid()) {
        if (!m_pendingSizeRefresh) {
            m_pendingSizeRefresh = true;
            QTimer::singleShot(0, this, [this]() {
                m_pendingSizeRefresh = false;
                refreshDisplayedFrame();
            });
        }
    } else {
        refreshDisplayedFrame();   // default project: synchronous, bit-identical
    }
}

void VideoPlayer::play()
{
    qInfo() << "VideoPlayer::play() entry m_playing=" << m_playing
            << "speed=" << m_playbackSpeed
            << "tlPos=" << m_timelinePositionUs
            << "activeEntry=" << m_activeEntry;
    if (!m_formatCtx || !m_codecCtx) {
        qWarning() << "VideoPlayer::play() no decoder, abort";
        return;
    }

    if (m_playing)
        return;

    // ADAPTIVE-1: 新しい再生セッションはフル品質から開始する。前回の再生で
    // 下がった scaleFactor を引き継がないよう policy をリセットする (guard 通過後
    // = 実際に再生を開始するときだけ。debounce/既再生の早期 return では触らない)。
    // 以降の tick が updateAdaptiveQuality() で実測に応じて再度ラダーを下げる。
    m_qualityPolicy.reset();
    m_adaptiveCanvasDivisor = 1;
    m_adaptiveQualityTier = 0;
    m_lastTickRenderMs = 0.0;

    // Phase 1e Win #12 — Fix J: collapse rapid play() bursts. The
    // scheduleNextFrame !advanced safety-net (line 2911-2923) auto-pauses
    // and stops the AudioMixer when a tick can't decode in time, which can
    // happen on the first tick after play() during a cold-open. Each
    // pause→play→pause cycle synchronously dispatches a QAudioSink stop/
    // start (~10-20 ms on the main thread) and the UI's button feedback
    // can re-emit play() before the cycle settles. Log
    // veditor_20260501_091710.log @ 09:20:18-19 caught 4 entries in 934 ms
    // → 10.7 s paintGL stall. A 200 ms window collapses the cascade while
    // still letting deliberate user pause→play sequences (~250 ms reaction
    // floor) through.
    static const bool kPlayDebounceDisabled =
        qEnvironmentVariableIntValue("VEDITOR_PLAY_DEBOUNCE_DISABLE") != 0;
    if (!kPlayDebounceDisabled
        && m_lastPlayCallTimer.isValid()
        && m_lastPlayCallTimer.elapsed() < 200) {
        qInfo() << "VideoPlayer::play() debounced — within 200 ms of last call (elapsed="
                << m_lastPlayCallTimer.elapsed() << "ms)";
        return;
    }
    m_lastPlayCallTimer.restart();

    // Phase 1e Win #14 — Fix L: re-anchor m_activeEntry against the current
    // playhead before the first tick. Empirical log
    // veditor_20260501_105849.log @ 11:02:26-28 caught play() three times
    // in a row with `m_playing=false speed=1 tlPos=7540076009 activeEntry=0`,
    // i.e. the playhead is past entry 0's clipOut (ナイトレイン 6386 s) but
    // m_activeEntry still pointed at entry 0. handlePlaybackTick's
    // reachedEntryEnd path (line 2748) sees m_currentPositionUs ≥
    // entryEndLocalUs immediately, drops into the Stage 1+2 candidate
    // search, and — because findActiveEntryAt's strict `<` boundary at
    // line 778 misses the next entry whose timelineStart equals the
    // current entry's timelineEnd — falls into the end-of-sequence
    // pause() at line 2880. That leaves m_currentPositionUs pinned at
    // clipOut, m_activeEntry untouched, and the next play() click
    // re-enters the same loop. User-visible: "前の clip 終端で停止、再生
    // 押しなおしても動かない、seek bar を次 clip 上まで持って行ってから
    // 再生で 2 clip まで動く".
    //
    // Resolution: when the cached activeEntry is out-of-range or its
    // file-local clipOut has been reached, resolve the entry from the
    // current m_timelinePositionUs and route through seekToTimelineUs —
    // which handles loadFile + decoder seek + m_activeEntry assignment
    // atomically, and also kicks AudioMixer::seekTo so the master clock
    // re-anchors. Fall back to the entry's timelineStart if the cached
    // tlPos pre-dates it (defensive).
    static const bool kPlayReanchorDisabled =
        qEnvironmentVariableIntValue("VEDITOR_PLAY_REANCHOR_DISABLE") != 0;
    if (!kPlayReanchorDisabled && sequenceActive() && !m_sequence.isEmpty()) {
        bool entryStale = (m_activeEntry < 0 || m_activeEntry >= m_sequence.size());
        if (!entryStale) {
            const auto &ae = m_sequence[m_activeEntry];
            // Phase 1e Win #15 (Fix M): judge stale on the TIMELINE axis, not
            // file-local. The original Fix L test
            // `m_currentPositionUs >= ae.clipOut * AV_TIME_BASE` mixed two
            // different position spaces — m_currentPositionUs is the
            // *currently-loaded file's* local cursor, but resetDecoder
            // (VideoPlayer.cpp:1597) clears it back to 0 on every
            // loadFile, including loadFiles invoked from a cross-file seek
            // toward a later entry. Empirical log
            // veditor_20260501_113335.log @ 11:35:42-49 caught 12 play()
            // entries with `tlPos=6386090998 activeEntry=0` (entry 0
            // ナイトレイン timelineEnd=6386.09 s) and zero `Fix L:
            // re-anchor` lines — m_currentPositionUs had been reset to 0,
            // entryStale stayed false, the re-anchor never fired. Comparing
            // the timeline cursor against the active entry's timeline
            // [start, end) range removes that dependency: as long as the
            // playhead is outside the entry's own timeline range, the
            // entry is stale regardless of which file's cursor lives in
            // m_currentPositionUs.
            const int64_t aeStartUs = static_cast<int64_t>(ae.timelineStart * AV_TIME_BASE);
            const int64_t aeEndUs = static_cast<int64_t>(ae.timelineEnd * AV_TIME_BASE);
            entryStale = (m_timelinePositionUs < aeStartUs
                       || m_timelinePositionUs >= aeEndUs);
        }
        if (entryStale) {
            const int resolved = findActiveEntryAt(m_timelinePositionUs);
            if (resolved >= 0 && resolved < m_sequence.size()
                && resolved != m_activeEntry) {
                const int64_t startUs = static_cast<int64_t>(
                    m_sequence[resolved].timelineStart * AV_TIME_BASE);
                qInfo() << "VideoPlayer::play() Fix L/M: re-anchor activeEntry"
                        << m_activeEntry << "->" << resolved
                        << "tlPos=" << m_timelinePositionUs
                        << "currentPos=" << m_currentPositionUs;
                seekToTimelineUs(qMax(m_timelinePositionUs, startUs), /*precise=*/true);
            }
        }
    }

    m_playing = true;
    updatePlayButton();
    emit stateChanged(true);
    scheduleNextFrame();

    // Kick the AudioMixer into play. If the audio schedule is empty the
    // mixer already stopped itself; otherwise this resumes from the
    // current master-clock position. Reverse playback mutes audio in
    // setPlaybackSpeed.
    if (m_mixer && m_playbackSpeed >= 0.0)
        m_mixer->play();
}

void VideoPlayer::pause()
{
    if (m_playbackTimer)
        m_playbackTimer->stop();

    m_playing = false;
    updatePlayButton();
    emit stateChanged(false);

    if (m_mixer)
        m_mixer->pause();
}

void VideoPlayer::stepForward()
{
    pause();
    // Frame-step is an immediate explicit action. Any deferred slider /
    // timeline seek left armed by m_seekTimer would otherwise fire
    // ~15 ms later and overwrite the stepped position with a stale
    // target. pause() does NOT stop m_seekTimer.
    if (m_seekTimer)
        m_seekTimer->stop();
    m_pendingSeekMs = -1;
    m_pendingSeekPrecise = false;

    const int64_t step = m_frameDurationUs > 0 ? m_frameDurationUs : AV_TIME_BASE / 30;

    if (!sequenceActive()) {
        // Single-file mode — step within file-local coordinates.
        const int64_t target = qMin(m_durationUs > 0 ? m_durationUs - 1 : INT64_MAX,
                                    m_currentPositionUs + step);
        if (target <= m_currentPositionUs) return;
        seekInternal(target, /*displayFrame=*/true, /*precise=*/true);
        m_currentPositionUs = target;
        updatePositionUi();
        return;
    }

    // Sequence mode — guarantee m_timelinePositionUs advances by `step`,
    // crossing clip boundaries / gaps as needed. The user invariant is
    // "1F never gets stuck except at timeline 0 / sequence end". To honour
    // it we (a) compute the desired new timeline position, (b) drive the
    // seek/advance through the existing infrastructure, then (c) FORCE
    // m_timelinePositionUs to the desired position even if updatePositionUi
    // reprojected it backward from m_currentPositionUs (the historic
    // forward-step revert path).
    const int64_t maxUs = m_sequenceDurationUs > 0
        ? m_sequenceDurationUs - 1 : INT64_MAX;
    // US-INT-2 Phase B: under a non-identity ramp, advancing the playhead
    // by `step` (= source-frame duration) in TIMELINE space does not land
    // on the next decoded source frame — a 2x ramp puts the next decoded
    // frame only step/2 timeline-us away, while a 0.5x ramp puts it 2*step
    // away. Walk through source space (entryLocalPositionUs → +step →
    // fileLocalToTimelineUs) so 1F→ matches the visible decoder advance for
    // ramped clips. Identity ramps short-circuit to the legacy timeline
    // arithmetic so the historic non-ramp invariants ("never stuck at
    // boundary", gap-cross behaviour) stay bit-exact.
    int64_t newPos;
    {
        const bool haveRampHere = (m_activeEntry >= 0
                && m_activeEntry < static_cast<int>(m_speedRamps.size()))
            && !m_speedRamps[m_activeEntry].isIdentity();
        if (haveRampHere) {
            const int64_t curSrc =
                entryLocalPositionUs(m_activeEntry, m_timelinePositionUs);
            const int64_t projected =
                fileLocalToTimelineUs(m_activeEntry, curSrc + step);
            // qMax(...+1, projected): the inverse-then-forward round trip
            // through SpeedRamp can land on the same us as m_timelinePositionUs
            // for sub-microsecond ramp+truncation residuals — guarantee at
            // least 1us forward progress so the user is never frame-stuck.
            newPos = qMin(maxUs,
                qMax<int64_t>(m_timelinePositionUs + 1, projected));
        } else {
            newPos = qMin(maxUs, m_timelinePositionUs + step);
        }
    }
    if (newPos == m_timelinePositionUs) return; // at end of sequence

    const int idx = findActiveEntryAt(newPos);
    if (idx >= 0 && idx < static_cast<int>(m_sequence.size())
        && m_activeEntry >= 0 && m_activeEntry < static_cast<int>(m_sequence.size())
        && idx != m_activeEntry) {
        const auto &cur = m_sequence[m_activeEntry];
        const auto &next = m_sequence[idx];
        if (next.timelineStart >= cur.timelineEnd - 1e-6) {
            // Non-overlap cross — clean snap to next entry's start.
            advanceToEntry(idx);
            // advanceToEntry sets m_timelinePositionUs to next.timelineStart.
            // For a gap-stretched step (target landed past next.timelineStart),
            // force forward so the user actually advances by `step`.
            if (newPos > m_timelinePositionUs) {
                m_timelinePositionUs = newPos;
                forceTimelineUiToCurrent();
            }
            return;
        }
    }

    // Same-entry or overlap step — natural decode advances the file
    // decoder by exactly 1 frame, sidestepping precise-seek-within-GOP
    // rounding. Reproject from the actual decoded PTS so the timeline
    // axis tracks frame-rate accurately (avoids ~1-frame skew compounding
    // on VFR or 23.976 sources). Force-forward to newPos ONLY when the
    // reprojection went backward (reproject regression scenario).
    if (decodeNextFrame(true)) {
        const int64_t projected = (m_activeEntry >= 0
                && m_activeEntry < static_cast<int>(m_sequence.size()))
            ? fileLocalToTimelineUs(m_activeEntry, m_currentPositionUs)
            : newPos;
        m_timelinePositionUs = qMax(projected, newPos);
        forceTimelineUiToCurrent();
        return;
    }

    // EOF on the active source — fall back to seekToTimelineUs which
    // will hot-swap or loadFile if the resolved entry differs. After
    // it returns, force m_timelinePositionUs forward in case
    // updatePositionUi snapped it back.
    seekToTimelineUs(newPos, /*precise=*/true);
    if (m_timelinePositionUs < newPos) {
        m_timelinePositionUs = newPos;
        forceTimelineUiToCurrent();
    }
}

void VideoPlayer::stepBackward()
{
    pause();
    if (m_seekTimer)
        m_seekTimer->stop();
    m_pendingSeekMs = -1;
    m_pendingSeekPrecise = false;
    const int64_t step = m_frameDurationUs > 0 ? m_frameDurationUs : AV_TIME_BASE / 30;
    if (sequenceActive()) {
        // US-INT-2 Phase B: ramp-aware backward step. Walk one source-frame
        // back through entryLocalPositionUs → -step → fileLocalToTimelineUs
        // so 1F← matches the previous decoded frame on ramped clips.
        // Identity ramps and entries outside m_speedRamps fall back to the
        // legacy timeline arithmetic for bit-exact compatibility.
        int64_t target;
        {
            const bool haveRampHere = (m_activeEntry >= 0
                    && m_activeEntry < static_cast<int>(m_speedRamps.size()))
                && !m_speedRamps[m_activeEntry].isIdentity();
            if (haveRampHere) {
                const int64_t curSrc =
                    entryLocalPositionUs(m_activeEntry, m_timelinePositionUs);
                const int64_t prevSrc = qMax<int64_t>(0, curSrc - step);
                const int64_t projected =
                    fileLocalToTimelineUs(m_activeEntry, prevSrc);
                // Mirror of stepForward's +1 floor: under near-zero local
                // speed the backward project can land on the same us; force
                // at least 1us regress so the user doesn't get stuck.
                target = qMax<int64_t>(0,
                    qMin<int64_t>(m_timelinePositionUs - 1, projected));
            } else {
                target = qMax<int64_t>(0, m_timelinePositionUs - step);
            }
        }
        if (target == m_timelinePositionUs) return; // at start
        seekToTimelineUs(target, /*precise=*/true);
        // Symmetric force: ensure m_timelinePositionUs ends at target even
        // if reprojection snapped it forward (e.g., into next overlap).
        if (m_timelinePositionUs > target) {
            m_timelinePositionUs = target;
            forceTimelineUiToCurrent();
        }
    } else {
        const int64_t target = qMax<int64_t>(0, m_currentPositionUs - step);
        seekInternal(target, /*displayFrame=*/true, /*precise=*/true);
        m_currentPositionUs = target;
        updatePositionUi();
    }
}

// Force seekbar / time label / positionChanged signal to reflect the
// CURRENT m_timelinePositionUs without reprojecting from m_currentPositionUs.
// stepForward / stepBackward use this when they need to override the
// reproject-from-file-local that updatePositionUi() does in sequence mode.
void VideoPlayer::forceTimelineUiToCurrent()
{
    if (m_suppressUiUpdates) return;
    const int64_t displayUs = sequenceActive()
        ? m_timelinePositionUs : m_currentPositionUs;
    const int64_t totalUs = sequenceActive()
        ? m_sequenceDurationUs : m_durationUs;
    const int sliderValue = qMin(sliderTimelinePosition(displayUs),
                                 m_seekBar->maximum());
    {
        const QSignalBlocker blocker(m_seekBar);
        if (!m_seekBar->isSliderDown())
            m_seekBar->setValue(sliderValue);
    }
    m_timeLabel->setText(QString("%1 / %2")
        .arg(formatTimestamp(displayUs))
        .arg(formatTimestamp(totalUs)));
    emit positionChanged(static_cast<double>(displayUs) / AV_TIME_BASE);
}

void VideoPlayer::stop()
{
    pause();
    // ADAPTIVE-1: 停止は静止画フル品質提示。品質ポリシーを 1.0 にリセットし、
    // 続く sequence-aware seek (performPendingSeek → seekInternal → displayFrame)
    // がフル解像度で 1 回だけ再描画されるようにする。新たな displayFrame 呼び出しや
    // 再描画ループはここでは起こさない (停止フリッカー修正の不変条件を維持)。
    m_qualityPolicy.reset();
    m_adaptiveCanvasDivisor = 1;
    m_adaptiveQualityTier = 0;
    m_lastTickRenderMs = 0.0;
    if (m_seekTimer)
        m_seekTimer->stop();
    // Rewind to the head. In sequence (timeline) mode the head is TIMELINE 0,
    // which MUST go through the sequence-aware seek path (seekToTimelineUs via
    // performPendingSeek). A raw file-local seekInternal(0) leaves the timeline
    // position projection stale — updatePositionUi() then rejects the large
    // delta, so the seek bar never returns to 0 and the preview can desync to a
    // black frame. Run the seek synchronously here exactly once.
    if (sequenceActive()) {
        m_pendingSeekMs = 0;
        m_pendingSeekPrecise = true;
        performPendingSeek();
    } else {
        seekInternal(0, true, true);
    }
    // Discard any deferred scrub/preview seek left armed so it can't repaint
    // over the head frame after this returns (defence against the post-stop
    // repaint loop).
    if (m_seekTimer)
        m_seekTimer->stop();
    m_pendingSeekMs = -1;
    m_pendingSeekPrecise = false;
    if (m_mixer) {
        m_mixer->stop();
        m_mixer->seekTo(0);
    }
}

void VideoPlayer::seek(int positionMs)
{
    // In sequence mode positionMs is interpreted as TIMELINE ms (the slider
    // and Timeline both speak timeline coordinates). In legacy single-file
    // mode it's the file-local ms — performPendingSeek picks the right path.
    if (!sequenceActive() && (!m_formatCtx || !m_codecCtx))
        return;

    m_pendingSeekMs = qMax(0, positionMs);
    m_pendingSeekPrecise = true;
    if (m_playbackTimer)
        m_playbackTimer->stop();

    if (!m_seekInProgress && m_seekTimer && !m_seekTimer->isActive())
        m_seekTimer->start();
}

void VideoPlayer::previewSeek(int positionMs)
{
    if (!sequenceActive() && (!m_formatCtx || !m_codecCtx))
        return;

    m_pendingSeekMs = qMax(0, positionMs);
    if (m_playbackTimer)
        m_playbackTimer->stop();

    if (!m_seekInProgress && m_seekTimer && !m_seekTimer->isActive())
        m_seekTimer->start();
}

void VideoPlayer::setPlaybackSpeed(double speed)
{
    if (qFuzzyIsNull(speed))
        speed = 1.0;

    const bool wasReverse = (m_playbackSpeed < 0.0);
    const double absSpeed = qBound(0.25, std::abs(speed), 16.0);
    m_playbackSpeed = (speed < 0.0) ? -absSpeed : absSpeed;
    emit playbackSpeedChanged(m_playbackSpeed);

    if (m_playing)
        scheduleNextFrame();

    if (m_mixer) {
        if (m_playbackSpeed < 0.0) {
            // Reverse playback: silence the mixer. We don't yet support
            // reverse audio — pause until forward playback resumes.
            m_mixer->pause();
        } else {
            // Re-anchor the mixer if we're coming out of reverse: the
            // video timeline cursor moved backward while the mixer was
            // paused, so resuming without a seek would play audio from
            // the pre-reverse position.
            if (wasReverse) {
                m_mixer->seekTo(m_timelinePositionUs);
            }
            if (m_playing) {
                m_mixer->play();
            }
        }
    }
}

void VideoPlayer::speedUp()
{
    if (!m_playing) {
        setPlaybackSpeed(1.0);
        play();
        return;
    }

    if (m_playbackSpeed < 0.0)
        setPlaybackSpeed(1.0);
    else
        setPlaybackSpeed(qMin(16.0, m_playbackSpeed * 2.0));
}

void VideoPlayer::speedDown()
{
    if (!m_playing) {
        setPlaybackSpeed(-1.0);
        play();
        return;
    }

    if (m_playbackSpeed > 0.0)
        setPlaybackSpeed(-1.0);
    else
        setPlaybackSpeed(qMax(-16.0, m_playbackSpeed * 2.0));
}

void VideoPlayer::togglePlay()
{
    if (m_playing) {
        pause();
    } else {
        setPlaybackSpeed(1.0);
        play();
    }
}

void VideoPlayer::setPreviewMaximized(bool maximized)
{
    if (m_previewMaximized == maximized) return;
    m_previewMaximized = maximized;
    if (m_maximizeButton) {
        QSignalBlocker block(m_maximizeButton);
        m_maximizeButton->setChecked(maximized);
        // Glyph swap: ⛶ (4 corners outward) when normal, ⤡ (4 corners
        // inward) when maximized — mirrors common video-player UX.
        m_maximizeButton->setText(maximized
            ? QString::fromUtf8("\xE2\xA4\xA1")    // ⤡
            : QString::fromUtf8("\xE2\x9B\xB6")); // ⛶
        m_maximizeButton->setToolTip(maximized
            ? QStringLiteral("プレビュー最大化解除 (Esc)")
            : QStringLiteral("プレビュー最大化 (Esc で解除)"));
    }
    emit previewMaximizeChanged(maximized);
}

void VideoPlayer::updatePlayButton()
{
    // The play button doubles as pause: flip its glyph and tooltip
    // depending on the current state. Stop stays always enabled.
    if (m_playButton) {
        m_playButton->setText(m_playing
            ? QString::fromUtf8("\xE2\x8F\xB8")    // ⏸ pause
            : QString::fromUtf8("\xE2\x96\xB6")); // ▶ play
        m_playButton->setToolTip(m_playing
            ? QStringLiteral("一時停止")
            : QStringLiteral("再生"));
        m_playButton->setEnabled(true);
    }
    if (m_stopButton) m_stopButton->setEnabled(true);
}

void *VideoPlayer::sharedD3D11Device() const noexcept
{
#if defined(_WIN32)
    if (void *dev = veditor_avHwDeviceCtxToD3D11Device(m_hwDeviceCtx)) return dev;
    return veditor_avHwDeviceCtxToD3D11Device(m_sharedPoolHwDeviceCtx);
#else
    return nullptr;
#endif
}

bool VideoPlayer::canUseInteropFastPath(const AVFrame *frame) const
{
    // Section C dual-register (Y as R8, UV as RG8 against the same
    // ID3D11Texture2D) crashed the NVIDIA driver during wglDXRegisterObjectNV.
    // Three independent reviews (architect/codex/gemini) agreed the WGL spec
    // does not permit plane-split views; the production-proven path is a
    // D3D11 NV12->RGBA blit (VideoProcessor or HLSL compute) followed by a
    // single register. That work is Section D scope. Until then the fast
    // path is hard-off — the surrounding infra (device open, AVFrame
    // extraction, cache) stays so Section D can drop in.
    Q_UNUSED(frame);
    return false;
}

void VideoPlayer::displaySeekFrameConformed(const QImage &v1Image)
{
    if (!m_projectOutputSize.isValid()) {
        displayFrame(v1Image);
        return;
    }

    QImage canvas(m_projectOutputSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);

    const PlaybackEntry *entry = nullptr;
    if (sequenceActive()
        && m_activeEntry >= 0
        && m_activeEntry < m_sequence.size()) {
        entry = &m_sequence[m_activeEntry];
    }

    const bool fitContain = entry ? entry->fitContain : false;
    const bool fitCover = entry ? entry->fitCover : false;
    {
        double insetFw = 1.0, insetFh = 1.0;
        if (fitContain && !fitCover) {
            snsfit::containContentInset(v1Image.size(), m_projectOutputSize, insetFw, insetFh);
        }
        if (m_glPreview)
            m_glPreview->setVideoContentInset(insetFw, insetFh);
    }
    QImage conformed = snsfit::maybeFit(v1Image, fitContain, fitCover, m_projectOutputSize);

    DecodedLayer layer;
    layer.rgb = conformed;
    layer.opacity = 1.0;
    if (entry) {
        layer.colorMeta = entry->colorMeta;
        applyLayerMotionOpacity(m_glPreview ? m_glPreview->timeline() : nullptr,
                                *entry, m_timelinePositionUs, layer.opacity, &layer);
        populateLayerMetadata(m_glPreview ? m_glPreview->timeline() : nullptr,
                              *entry, m_activeEntry, &layer);
    }
    layer.fitContain = fitContain;
    layer.fitCover = fitCover;
    layer.isFresh = true;

    QVector<DecodedLayer> singleLayer;
    singleLayer.append(layer);
    composeMultiTrackFrameInto(canvas, singleLayer);
    if (m_glPreview)
        m_glPreview->setCompositeBakedMode(true);
    m_lastFrameOdtApplied = false;
    displayFrame(canvas);
}

bool VideoPlayer::pushActiveClipColorCorrectionToGlPreview()
{
    // Baked/composited frames already include per-clip grade. When the
    // user disables GPU preview effects, keep GLPreview's shader state
    // unchanged so static grade remains available through the legacy path.
    if (!m_useGL || !m_glPreview)
        return false;
    if (!m_gpuEffectsEnabled) {
        return false;
    }
    if (m_glPreview->compositeBakedMode())
        return false;
    if (!sequenceActive()
        || m_activeEntry < 0
        || m_activeEntry >= m_sequence.size()) {
        return false;
    }

    const Timeline *timeline = m_glPreview->timeline();
    const PlaybackEntry &entry = m_sequence[m_activeEntry];
    const ClipInfo *clip = clipForPlaybackEntry(timeline, entry);
    if (!clip)
        return false;

    const double clipLocalSec = entryClipLocalSeconds(entry, m_timelinePositionUs);
    const ColorCorrection effective =
        clipanim::effectiveColorCorrectionAt(*clip, clipLocalSec);
    m_glPreview->setColorCorrection(effective);
    return true;
}

bool VideoPlayer::pushActiveClipColorCorrectionToGlPreviewForTest(qint64 timelineUsec)
{
    const qint64 previousTimelineUsec = m_timelinePositionUs;
    m_timelinePositionUs = timelineUsec;
    const bool pushed = pushActiveClipColorCorrectionToGlPreview();
    m_timelinePositionUs = previousTimelineUsec;
    return pushed;
}

void VideoPlayer::displayFrame(const QImage &image)
{
    undotrace::log("displayFrame:enter");
    m_lastSourceFrame = image;
    QImage composed = composeFrameWithOverlays(image);

    // Edge-attached transitions: FadeIn / FadeOut alpha-ramp against black.
    // CrossDissolve uses the timeline-overlap path — Timeline overlaps the
    // CrossDissolve pair so B is in m_sequence with timelineStart pulled
    // back by D, and the blend happens via harvestOverlayLayer below.
    // Wipes / slides need path geometry not yet wired through displayFrame.
    if (!composed.isNull() && sequenceActive()
        && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
        const auto &e = m_sequence[m_activeEntry];
        const double T = static_cast<double>(m_timelinePositionUs)
                       / static_cast<double>(AV_TIME_BASE);
        const double elapsed = T - e.timelineStart;
        const double remaining = e.timelineEnd - T;
        double alpha = 1.0;
        if (e.leadInType == TransitionType::FadeIn
            && e.leadInDuration > 0.0
            && elapsed >= 0.0 && elapsed < e.leadInDuration) {
            const double raw = qBound(0.0, elapsed / e.leadInDuration, 1.0);
            alpha = applyEasing(raw, e.leadInEasing);
        } else if (e.trailOutType == TransitionType::FadeOut
            && e.trailOutDuration > 0.0
            && remaining >= 0.0 && remaining < e.trailOutDuration) {
            const double raw = qBound(0.0, remaining / e.trailOutDuration, 1.0);
            alpha = applyEasing(raw, e.trailOutEasing);
        }
        if (alpha < 0.999) {
            QImage faded(composed.size(), QImage::Format_ARGB32_Premultiplied);
            faded.fill(Qt::black);
            QPainter pp(&faded);
            pp.setOpacity(alpha);
            pp.drawImage(0, 0, composed);
            pp.end();
            composed = faded;
        }

        // Overlap blend for boundary transitions (CrossDissolve / Wipe /
        // Slide). Timeline pulled the next clip's timelineStart back by D
        // so we look up the same-track neighbour with a matching leadIn
        // type and blend its currently-playing frame via the type-specific
        // path inside OverlayRenderer::applyTransition.
        if (isOverlapTransition(e.trailOutType)
            && e.trailOutDuration > 0.0
            && remaining >= 0.0 && remaining < e.trailOutDuration) {
            int nextIdx = -1;
            const double overlapStart = e.timelineEnd - e.trailOutDuration;
            // eps covers floating-point rounding only — the search range
            // [overlapStart, e.timelineEnd] is the actual overlap window.
            const double eps = 1e-3;
            for (int i = 0; i < m_sequence.size(); ++i) {
                if (i == m_activeEntry) continue;
                const auto &c = m_sequence[i];
                if (c.sourceTrack != e.sourceTrack) continue;
                if (c.leadInType != e.trailOutType) continue;
                if (c.timelineStart >= overlapStart - eps
                    && c.timelineStart <= e.timelineEnd + eps) {
                    if (nextIdx < 0
                        || m_sequence[nextIdx].timelineStart > c.timelineStart) {
                        nextIdx = i;
                    }
                }
            }
            if (nextIdx >= 0) {
                DecodedLayer layer;
                if (harvestOverlayLayer(m_sequence[nextIdx], nextIdx, &layer)
                    && !layer.rgb.isNull()) {
                    const double rawProgress = qBound(0.0,
                                                   1.0 - remaining / e.trailOutDuration,
                                                   1.0);
                    const double progress = applyEasing(rawProgress, e.trailOutEasing);
                    composed = OverlayRenderer::applyTransition(
                        composed, layer.rgb,
                        e.trailOutType, progress);
                }
            }
        }
    }

    // AR-2: ACES シーンリファード色管理を最終フレームへ適用する。enabled=false の
    // ときは一切呼ばず (applyPipelineToImage 自体も enabled=false なら no-op だが、
    // 関数呼び出しと QImage 共有コピーすら避けて従来パスとビット同一を厳守する)。
    // レガシー Exporter は Exporter 側でのみ適用するため、composeFrameWithOverlays /
    // renderFrameAt の共通コアには ACES を入れていない (二重適用回避)。
    if (m_acesPipeline.enabled && !composed.isNull() && !m_lastFrameOdtApplied) {
        composed = aces::applyPipelineToImage(composed, m_acesPipeline);
    }

    m_currentFrameImage = composed;
    emit frameComposited(composed);

    // EXP-AID: 露出/フォーカス確認エイド (フォルスカラー / ゼブラ / フォーカスピーキング)。
    // これは画面確認専用のオーバーレイなので「表示用の一時コピー」にだけ適用し、
    // m_currentFrameImage / frameComposited のペイロード / 合成キャッシュ
    // (cachePreviewComposite は displayFrame より前で素のフレームを put 済み) は
    // 一切汚さない。mode == None (既定) のときは apply を呼ばず composed をそのまま
    // 使うので従来パスとビット同一・性能無影響。書き出し (renderFrameAt /
    // TimelineFrameRenderer / RenderQueue / Exporter) はこの経路を通らないため、
    // エイドが書き出し画に焼き込まれることはない。displayFrame 1 回あたりの画像
    // 加工が 1 つ増えるだけで、displayFrame の発火回数は不変 (1 tick = 最大 1 frame)。
    // 精度が必要なエッジ/輝度判定のためエイドは PV-C 縮小前の full res に適用し、
    // aid=None + cap=0 の既定パスは従来どおり composed をそのまま表示へ渡す。
    QImage display = composed;
    if (m_exposureAidMode != exposureaid::AidMode::None && !display.isNull()) {
        display = exposureaid::apply(display, m_exposureAidMode, m_exposureAidConfig);
    }
    auto applyPreviewCap = [this](const QImage &src) {
        if (m_previewMaxLongSide > 0 && !src.isNull()
            && qMax(src.width(), src.height()) > m_previewMaxLongSide) {
            return (src.width() >= src.height())
                ? src.scaledToWidth(m_previewMaxLongSide, Qt::SmoothTransformation)
                : src.scaledToHeight(m_previewMaxLongSide, Qt::SmoothTransformation);
        }
        return src;
    };

    if (m_onionSkin.enabled && !display.isNull()) {
        // SAFE-ZONE: onion skin 有効時はユーザー指定どおり full-res の表示用コピーへ
        // セーフゾーンを先に焼き、ghost 合成後に PV-C 縮小する。書き出しは非通過。
        if (m_safeZonePlatform != safezone::Platform::None)
            display = safezone::apply(display, m_safeZonePlatform);

        // ONION-SKIN: renderFrameAt は独立した libav decoder を開く SSOT 経路。
        // 再生中は重い前後フレーム decode を避け、通常のプレビュー描画だけ継続する。
        if (!m_playing) {
            QVector<QImage> before;
            QVector<QImage> after;
            const Timeline *tl = m_glPreview ? m_glPreview->timeline() : nullptr;
            const qint64 durationUs = (m_sequenceDurationUs > 0)
                ? m_sequenceDurationUs
                : (tl ? static_cast<qint64>(tl->totalDuration() * AV_TIME_BASE) : 0);
            const qint64 frameUs = (m_frameDurationUs > 0)
                ? m_frameDurationUs
                : (AV_TIME_BASE / 30);
            const qint64 posUs = sequenceActive() ? m_timelinePositionUs : m_currentPositionUs;

            auto collectGhosts = [&](int count, int direction, QVector<QImage> *out) {
                if (!tl || !out || count <= 0 || durationUs <= 0 || frameUs <= 0)
                    return;
                for (int k = 1; k <= count; ++k) {
                    const qint64 targetUs = posUs + static_cast<qint64>(direction) * frameUs * k;
                    if (targetUs < 0 || targetUs > durationUs)
                        continue;
                    const QImage ghost = tlrender::renderFrameAt(tl, targetUs, display.size());
                    if (!ghost.isNull())
                        out->append(ghost);
                }
            };
            collectGhosts(qMax(0, m_onionSkin.framesBefore), -1, &before);
            collectGhosts(qMax(0, m_onionSkin.framesAfter), 1, &after);
            if (!before.isEmpty() || !after.isEmpty())
                display = onionskin::compose(display, before, after, m_onionSkin);
        }

        // PV-C: プレビュー最大解像度キャップ。onion skin 合成後の display 専用コピーにだけ
        // 縮小を掛け、GL アップロード/描画/スコープ前段の負荷を軽くする。
        display = applyPreviewCap(display);
    } else {
        // PV-C: プレビュー最大解像度キャップ。エイド適用後の display 専用コピーにだけ
        // 縮小を掛け、GL アップロード/描画/スコープ前段の負荷を軽くする。
        // m_currentFrameImage / frameComposited (上で full res 送出済) と書き出し
        // (renderFrameAt 系=この経路を通らない) は不変。0=無制限で従来ビット同一。
        display = applyPreviewCap(display);
        // SAFE-ZONE: SNS セーフゾーンオーバーレイ。display-local 適用のみ。export 非通過。
        if (m_safeZonePlatform != safezone::Platform::None && !display.isNull()) {
            display = safezone::apply(display, m_safeZonePlatform);
        }
    }

    if (m_useGL && m_glPreview) {
        m_glPreview->setDisplayAspectRatio(effectiveDisplayAspectRatio());
        int hdrTransfer = 0;
        if (m_hdrInfo.isHdr) {
            if (m_hdrInfo.trc == AVCOL_TRC_SMPTE2084) hdrTransfer = 1;
            else if (m_hdrInfo.trc == AVCOL_TRC_ARIB_STD_B67) hdrTransfer = 2;
        }
        m_glPreview->setHdrTransfer(hdrTransfer);
        // US-INT-4: push the active entry's stabilizer keyframes (V1 only;
        // higher-track stabilization is a follow-up) and the current source
        // time so GLPreview's binary-search lookup can pick the matching
        // keyframe and bake the inverse 2D affine pre-warp.
        if (sequenceActive() && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
            const auto &ae = m_sequence[m_activeEntry];
            if (ae.sourceTrack == 0) {
                m_glPreview->setStabilizerKeyframes(ae.stabilizerKeyframes);
                const double T = static_cast<double>(m_timelinePositionUs)
                               / static_cast<double>(AV_TIME_BASE);
                const double srcSec = ae.clipIn + (T - ae.timelineStart) * ae.speed;
                m_glPreview->setStabilizerSourceTimeUs(
                    static_cast<qint64>(srcSec * 1.0e6));
            } else {
                m_glPreview->setStabilizerKeyframes({});
            }
        } else {
            m_glPreview->setStabilizerKeyframes({});
        }
        pushActiveClipColorCorrectionToGlPreview();
        undotrace::log("displayFrame:beforeGL");
        m_glPreview->displayFrame(display);
        undotrace::log("displayFrame:afterGL");
    } else {
        const QSize targetSize = fittedDisplaySize(m_videoDisplay->size());
        const QPixmap pixmap = QPixmap::fromImage(display);
        m_videoDisplay->setPixmap(pixmap.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }
}

void VideoPlayer::setTextOverlays(const QVector<EnhancedTextOverlay> &overlays)
{
    m_textOverlays = overlays;
    // Drop the hidden-during-edit index if the overlay list shrank under it.
    if (m_hiddenTextOverlayIndex >= m_textOverlays.size())
        m_hiddenTextOverlayIndex = -1;
    refreshTextOverlayHits();
    refreshDisplayedFrame();
}

void VideoPlayer::refreshTextOverlayHits()
{
    if (!m_glPreview)
        return;
    QVector<GLPreview::TextOverlayHit> hits;
    hits.reserve(m_textOverlays.size());
    for (int i = 0; i < m_textOverlays.size(); ++i) {
        const auto &ov = m_textOverlays[i];
        if (!ov.visible || ov.text.isEmpty())
            continue;
        // Skip overlays whose rect dimensions are zero — they're auto-sized
        // at paint time and don't have a stable hit area until the frame
        // is rendered. Click-to-edit works for explicit-size overlays only.
        if (ov.width <= 0.0 || ov.height <= 0.0)
            continue;
        GLPreview::TextOverlayHit hit;
        hit.index = i;
        hit.text = ov.text;
        hit.normalizedRect = QRectF(ov.x, ov.y, ov.width, ov.height);
        hits.append(hit);
    }
    m_glPreview->setTextOverlayHitList(hits);
}

QImage VideoPlayer::composeFrameWithOverlays(const QImage &source) const
{
    if (source.isNull())
        return source;

    const bool wantsPreviewFx = !m_previewEffects.isEmpty()
                                && (m_previewEffectsLive || !m_playing);
    const qint64 previewTimelineUsec =
        sequenceActive() ? m_timelinePositionUs : m_currentPositionUs;
    const QVector<VideoEffect> previewEffects = wantsPreviewFx
        ? effectivePreviewEffectsAt(m_previewEffects,
                                    m_glPreview ? m_glPreview->timeline() : nullptr,
                                    m_sequence,
                                    previewTimelineUsec)
        : QVector<VideoEffect>();
    const bool applyPreviewFx = !previewEffects.isEmpty();

    // Preview proxy: only shrink during playback, keep paused frames full-res.
    const int proxy = (applyPreviewFx && m_playing) ? qMax(1, m_proxyDivisor) : 1;
    auto runProxy = [proxy, &previewEffects](const QImage &img) {
        if (proxy <= 1)
            return VideoEffectProcessor::applyEffectStack(img, {}, previewEffects);
        const QImage small = img.scaled(img.width() / proxy, img.height() / proxy,
                                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QImage processed = VideoEffectProcessor::applyEffectStack(
            small, {}, previewEffects);
        return processed.scaled(img.width(), img.height(),
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    };

    if (m_textOverlays.isEmpty()) {
        if (!applyPreviewFx) return source;
        return runProxy(source);
    }

    const double nowSec = static_cast<double>(previewTimelineUsec) / AV_TIME_BASE;

    // US-T32 WYSIWYG: the inline text tool draws at the literal pointSize
    // in widget coordinates. When compose bakes overlays into the source
    // image at (W,H) and GLPreview later scales it to the current letterbox
    // for display, the display scale is letterboxH/H. To land at the same
    // visible pointSize we inverse-scale by H/letterboxH so the committed
    // text matches the inline input size 1:1 on screen. (Headless / export:
    // m_glPreview is null -> letterboxH = H -> fontScale = 1.0, which is
    // exactly what the SSOT renderer passes textbake::bakeOverlays directly.)
    const double H = static_cast<double>(source.height());
    double letterboxH = H;
    if (m_glPreview) {
        const QRectF lb = m_glPreview->letterboxRect();
        if (lb.height() > 0.0 && std::isfinite(lb.height()))
            letterboxH = lb.height();
    }
    const double fontScale = (letterboxH > 0.0) ? (H / letterboxH) : 1.0;

    // The actual fonts/keyframe/outline/gradient bake is the SINGLE shared
    // implementation textbake::bakeOverlays (extracted verbatim from this
    // function). The SSOT renderer's worker-thread export path calls the
    // SAME function directly (no VideoPlayer / no QWidget off the GUI
    // thread), so an exported frame's text pixel-matches this preview bake
    // by construction.
    const QImage composed = textbake::bakeOverlays(
        source, m_textOverlays, nowSec, m_hiddenTextOverlayIndex, fontScale);
    if (applyPreviewFx)
        return runProxy(composed);
    return composed;
}

void VideoPlayer::setHiddenTextOverlayIndex(int index)
{
    if (m_hiddenTextOverlayIndex == index) return;
    m_hiddenTextOverlayIndex = index;
    refreshDisplayedFrame();
}

// US-WIRE-3: enter region picker mode. Creates a transparent overlay on
// top of GLPreview; the user drags a rectangle on it and the callback
// receives the QRect in source-frame pixel coordinates.
void VideoPlayer::enterRegionPickerMode(std::function<void(QRect)> callback)
{
    exitWbEyedropperMode();
    exitRegionPickerMode();
    if (!callback || !m_glPreview) return;
    m_regionPickerCallback = std::move(callback);
    m_regionPickerActive = true;

    // Find the QStackedWidget that parents GLPreview so we can match
    // the overlay geometry exactly to GLPreview's widget rect.
    QStackedWidget *stack = qobject_cast<QStackedWidget *>(m_glPreview->parentWidget());
    QRect overlayGeo = stack ? stack->geometry() : m_glPreview->geometry();

    auto *picker = new RegionPickerOverlay(this);
    m_regionPickerOverlay = picker;
    m_regionPickerOverlay->setGeometry(overlayGeo);

    picker->setCallback([this](QRect widgetRect) {
        if (!m_regionPickerCallback) return;

        if (widgetRect.isNull()) {
            m_regionPickerCallback = nullptr;
            exitRegionPickerMode();
            return;
        }

        // Map from widget (overlay) coordinates → source-frame pixel
        // coordinates using GLPreview's letterbox rect.
        QRectF lb = m_glPreview ? m_glPreview->letterboxRect() : QRectF();
        int srcW = (m_codecCtx && m_codecCtx->width > 0)
                       ? m_codecCtx->width : m_canvasWidth;
        int srcH = (m_codecCtx && m_codecCtx->height > 0)
                       ? m_codecCtx->height : m_canvasHeight;

        QRect mapped = widgetRect;
        if (lb.width() > 1.0 && lb.height() > 1.0) {
            double sx = static_cast<double>(srcW) / lb.width();
            double sy = static_cast<double>(srcH) / lb.height();
            int mx = qMax(0, qRound((widgetRect.x() - lb.x()) * sx));
            int my = qMax(0, qRound((widgetRect.y() - lb.y()) * sy));
            int mw = qMin(srcW - mx, qMax(1, qRound(widgetRect.width() * sx)));
            int mh = qMin(srcH - my, qMax(1, qRound(widgetRect.height() * sy)));
            mapped = QRect(mx, my, mw, mh);
        }

        m_regionPickerCallback(mapped);
        exitRegionPickerMode();
    });

    m_regionPickerOverlay->show();
    m_regionPickerOverlay->raise();
    m_regionPickerOverlay->setFocus();
}

void VideoPlayer::exitRegionPickerMode()
{
    m_regionPickerActive = false;
    m_regionPickerCallback = nullptr;
    if (m_regionPickerOverlay) {
        m_regionPickerOverlay->deleteLater();
        m_regionPickerOverlay = nullptr;
    }
}

void VideoPlayer::enterWbEyedropperMode(std::function<void(QColor)> callback)
{
    exitRegionPickerMode();
    exitWbEyedropperMode();
    if (!callback || !m_glPreview)
        return;

    m_wbEyedropperCallback = std::move(callback);
    m_wbEyedropperActive = true;

    QStackedWidget *stack = qobject_cast<QStackedWidget *>(m_glPreview->parentWidget());
    const QRect overlayGeo = stack ? stack->geometry() : m_glPreview->geometry();

    auto *picker = new WbEyedropperOverlay(this);
    m_wbEyedropperOverlay = picker;
    m_wbEyedropperOverlay->setGeometry(overlayGeo);

    picker->setCallback([this](QPoint widgetPos) {
        if (!m_wbEyedropperCallback)
            return;

        QColor sampled;
        if (widgetPos.x() >= 0 && widgetPos.y() >= 0
            && m_glPreview && !m_currentFrameImage.isNull()) {
            const QRectF lb = m_glPreview->letterboxRect();
            if (lb.width() > 1.0 && lb.height() > 1.0
                && lb.contains(QPointF(widgetPos))) {
                const int srcW = m_currentFrameImage.width();
                const int srcH = m_currentFrameImage.height();
                const double sx = static_cast<double>(srcW) / lb.width();
                const double sy = static_cast<double>(srcH) / lb.height();
                const int px = qBound(0, static_cast<int>((widgetPos.x() - lb.x()) * sx), srcW - 1);
                const int py = qBound(0, static_cast<int>((widgetPos.y() - lb.y()) * sy), srcH - 1);
                sampled = m_currentFrameImage.pixelColor(px, py);
            }
        }

        auto cb = std::move(m_wbEyedropperCallback);
        m_wbEyedropperCallback = nullptr;
        exitWbEyedropperMode();
        cb(sampled);
    });

    m_wbEyedropperOverlay->show();
    m_wbEyedropperOverlay->raise();
    m_wbEyedropperOverlay->setFocus();
}

void VideoPlayer::exitWbEyedropperMode()
{
    m_wbEyedropperActive = false;
    m_wbEyedropperCallback = nullptr;
    if (m_wbEyedropperOverlay) {
        m_wbEyedropperOverlay->deleteLater();
        m_wbEyedropperOverlay = nullptr;
    }
}

// US-EF-2: enter mask drawing mode. Reuses the US-WIRE-3 region picker
// overlay (same drag-rect UX) but the callback receives a normalized
// QRectF in [0..1] vTexCoord space — derived from the source-frame pixel
// rect by dividing by the codec's frame size. This matches what GLPreview's
// uMaskRect uniform expects, so callers can feed the result into
// ColorGradingPanel::setMaskRect or GLPreview::setMask directly.
void VideoPlayer::enterMaskEditMode(std::function<void(QRectF)> callback)
{
    if (!callback) return;
    // Wrap the user callback so the QRect → QRectF conversion happens here
    // and the rest of the picker plumbing stays unchanged.
    enterRegionPickerMode([this, cb = std::move(callback)](QRect srcRect) {
        if (srcRect.isNull() || srcRect.width() <= 0 || srcRect.height() <= 0) {
            cb(QRectF());
            return;
        }
        const int srcW = (m_codecCtx && m_codecCtx->width > 0)
                             ? m_codecCtx->width  : m_canvasWidth;
        const int srcH = (m_codecCtx && m_codecCtx->height > 0)
                             ? m_codecCtx->height : m_canvasHeight;
        if (srcW <= 0 || srcH <= 0) {
            cb(QRectF());
            return;
        }
        QRectF normalized(static_cast<double>(srcRect.x()) / srcW,
                          static_cast<double>(srcRect.y()) / srcH,
                          static_cast<double>(srcRect.width())  / srcW,
                          static_cast<double>(srcRect.height()) / srcH);
        cb(normalized);
    });
}

void VideoPlayer::exitMaskEditMode()
{
    exitRegionPickerMode();
}

void VideoPlayer::setColorCorrection(const ColorCorrection &cc)
{
    if (m_glPreview)
        m_glPreview->setColorCorrection(cc);
}

void VideoPlayer::setAcesPipeline(const aces::AcesPipeline &pipeline)
{
    // AR-2: MainWindow SSOT (m_acesPipeline) をプレビューへ反映する。displayFrame は
    // enabled=true のときだけ最終合成結果へ aces::applyPipelineToImage を適用するので、
    // ここでは設定を保持し、表示中フレームを即再描画して反映するだけでよい。
    // refreshDisplayedFrame は m_lastSourceFrame (合成前の生フレーム) から
    // composeFrameWithOverlays を再実行 → displayFrame へ流すため、新しい
    // ACES 設定が即座に効く。null キャッシュ時は次の通常 tick / seek で反映される。
    m_acesPipeline = pipeline;
    refreshDisplayedFrame();
}

void VideoPlayer::setExposureAidMode(exposureaid::AidMode mode)
{
    // EXP-AID: 露出/フォーカス確認エイドのモードを切り替える。値を保持するだけで、
    // 実際の加工は displayFrame が表示直前に「表示用 QImage の一時コピー」へ行う
    // (キャッシュ / 保持フレーム / 書き出しには非適用)。refreshDisplayedFrame は
    // m_lastSourceFrame (合成前の生フレーム) から composeFrameWithOverlays を
    // 再実行 → displayFrame へ流すので、一時停止中でも即座に反映される。
    if (m_exposureAidMode == mode)
        return;
    m_exposureAidMode = mode;
    refreshDisplayedFrame();
}

void VideoPlayer::setPreviewMaxLongSide(int px)
{
    // PV-C: 0=無制限。負値は 0 に丸める。変化時のみ現在フレームを再描画。
    const int next = (px > 0) ? px : 0;
    if (m_previewMaxLongSide == next)
        return;
    m_previewMaxLongSide = next;
    refreshDisplayedFrame();
}

void VideoPlayer::setSafeZonePlatform(safezone::Platform p)
{
    // SAFE-ZONE: SNS セーフゾーンオーバーレイのプラットフォームを切り替える。
    // 実際の描画は displayFrame が表示直前に「表示用 QImage の一時コピー」へ行う
    // (キャッシュ / 保持フレーム / 書き出しには非適用)。
    if (m_safeZonePlatform == p)
        return;
    m_safeZonePlatform = p;
    refreshDisplayedFrame();
}

void VideoPlayer::setOnionSkinConfig(const onionskin::Config &cfg)
{
    // ONION-SKIN: 前後フレームの半透明オーバーレイを切り替える。表示用 QImage の
    // 一時コピーだけを加工し、renderFrameAt / Exporter / RenderQueue には焼き込まない。
    const bool same = m_onionSkin.enabled == cfg.enabled
        && m_onionSkin.framesBefore == cfg.framesBefore
        && m_onionSkin.framesAfter == cfg.framesAfter
        && std::abs(m_onionSkin.opacity - cfg.opacity) < 1e-9
        && m_onionSkin.tintColors == cfg.tintColors;
    if (same)
        return;
    m_onionSkin = cfg;
    refreshDisplayedFrame();
}

void VideoPlayer::setExposureAidConfig(const exposureaid::AidConfig &cfg)
{
    // EXP-AID: しきい値・縞幅・感度・色などのパラメータを更新する。モードと同じく
    // 表示直前の一時加工にしか使われない。
    m_exposureAidConfig = cfg;
    refreshDisplayedFrame();
}

void VideoPlayer::setPreviewEffects(const QVector<VideoEffect> &effects, bool live)
{
    m_previewEffectsLive = live;

    const bool gpuEnabled = m_gpuEffectsEnabled && m_useGL && m_glPreview;

    QVector<VideoEffect> gpu;
    QVector<VideoEffect> cpu;
    for (const VideoEffect &e : effects) {
        const bool gpuCapable =
            e.type == VideoEffectType::Blur      ||
            e.type == VideoEffectType::Noise     ||
            e.type == VideoEffectType::Sepia     ||
            e.type == VideoEffectType::Grayscale ||
            e.type == VideoEffectType::Invert    ||
            e.type == VideoEffectType::Vignette;
        const bool hasTiming = e.startSec >= 0.0 || e.endSec >= 0.0;
        if (gpuEnabled && gpuCapable && !hasTiming) gpu.append(e);
        else                                        cpu.append(e);
    }

    m_previewEffects = cpu;
    if (m_glPreview)
        m_glPreview->setVideoEffects(gpu);

    refreshDisplayedFrame();
}

void VideoPlayer::setGpuEffectsEnabled(bool enabled)
{
    if (m_gpuEffectsEnabled == enabled)
        return;
    m_gpuEffectsEnabled = enabled;
}

void VideoPlayer::setGLAcceleration(bool enabled)
{
    m_useGL = enabled;
    auto *stack = qobject_cast<QStackedWidget*>(m_videoDisplay->parentWidget());
    if (stack)
        stack->setCurrentIndex(enabled ? 1 : 0);
    refreshDisplayedFrame();
}

void VideoPlayer::setTextToolActive(bool active)
{
    if (m_textToolActive == active)
        return;
    m_textToolActive = active;
    // Forward the mode to GLPreview (owns the drag capture + I-beam cursor)
    // and mirror the cursor on the QLabel fallback for non-GL software mode.
    if (m_glPreview)
        m_glPreview->setTextToolActive(active);
    if (m_videoDisplay) {
        if (active)
            m_videoDisplay->setCursor(Qt::IBeamCursor);
        else
            m_videoDisplay->unsetCursor();
    }
}

void VideoPlayer::clearTextToolRect()
{
    if (m_glPreview)
        m_glPreview->clearTextToolRect();
}

void VideoPlayer::setTextToolStyle(const QFont &font, const QColor &color)
{
    if (m_glPreview)
        m_glPreview->setTextToolStyle(font, color);
}

void VideoPlayer::setSnapStrength(double pixels)
{
    if (m_glPreview)
        m_glPreview->setSnapStrength(pixels);
}

bool VideoPlayer::isTextToolEditing() const
{
    return m_glPreview && m_glPreview->isTextToolEditing();
}

QString VideoPlayer::currentTextToolInputText() const
{
    return m_glPreview ? m_glPreview->currentTextToolInputText() : QString();
}

void VideoPlayer::commitCurrentTextToolEdit()
{
    if (m_glPreview)
        m_glPreview->commitCurrentTextToolEdit();
}

void VideoPlayer::resetDecoder()
{
    if (m_playbackTimer)
        m_playbackTimer->stop();
    if (m_seekTimer)
        m_seekTimer->stop();

    // Audio lifecycle belongs to AudioMixer now — do NOT touch the mixer
    // here. resetDecoder is called from loadFile() on every video entry
    // boundary; the mixer keeps its own decoder pool keyed by AudioTrackKey
    // and stays in step via setAudioSequence + the master clock.

    m_playing = false;
    m_videoStreamIndex = -1;
    m_hdrInfo = {};
    m_durationUs = 0;
    m_currentPositionUs = 0;
    m_frameDurationUs = 0;
    m_displayAspectRatio = 0.0;
    // Phase 1e Win #15 (Fix M): retain the last presented QImage across
    // an advanceToEntry/seekToTimelineUs cross-file hand-off so the
    // GLPreview keeps showing the previous clip's final frame until the
    // new file's first decoded frame replaces it (≈60-145 ms window,
    // dominated by avformat_find_stream_info + first seek). Honoured
    // only when VideoPlayer::advanceToEntry has set
    // m_retainLastFrameAcrossLoad; the freshly-imported / Timeline-
    // empty path still clears as before.
    static const bool kLastFrameRetentionDisabled =
        qEnvironmentVariableIntValue("VEDITOR_LAST_FRAME_RETENTION_DISABLE") != 0;
    if (kLastFrameRetentionDisabled || !m_retainLastFrameAcrossLoad)
        m_currentFrameImage = QImage();
    m_pendingSeekMs = -1;
    m_pendingSeekPrecise = false;
    m_seekInProgress = false;

    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_frame)
        av_frame_free(&m_frame);
    if (m_swFrame)
        av_frame_free(&m_swFrame);
    if (m_packet)
        av_packet_free(&m_packet);
    if (m_codecCtx)
        avcodec_free_context(&m_codecCtx);
    if (m_formatCtx)
        avformat_close_input(&m_formatCtx);
    // Do NOT unref m_hwDeviceCtx here — it's reused across loadFile calls
    // (see hwDeviceReady gate in loadFile). Final release happens in the
    // destructor after clearAllPoolDecoders. Skipping per-clip teardown
    // saves ~22 ms per track switch (avoid av_hwdevice_ctx_create rebuild)
    // and also keeps the GL interop cache valid — the cache is keyed by
    // the D3D11 device pointer, so destroying the ctx invalidates every
    // cached pool texture handle.
    m_hwPixFmt = AV_PIX_FMT_NONE;
    // Skip interop cache flush when m_hwDeviceCtx survives the reset
    // (the common case under Fix G). The cache is keyed on the D3D11
    // device pointer, which we keep, so retaining it spares ~5-10 ms
    // of texture re-register on the next paint after a clip switch.
    // Only flush when the device is going away (HW failure path or
    // never-opened state).
    if (m_glPreview && !m_hwDeviceCtx) {
        m_glPreview->flushInteropCache();
        m_glPreview->setSharedD3D11Device(nullptr);
    }

    // Drop the cached compositor base. Otherwise the next tick after a
    // clip switch (advanceToEntry → loadFile → resetDecoder → fresh
    // decode) hands the compositor the previous clip's V1 frame as the
    // canvas base, and the user briefly sees the old clip behind any
    // V2+ overlays.
    // Phase 1e Win #15 (Fix M) — same retention window as
    // m_currentFrameImage above. The compositor's V1 base canvas keeps
    // the previous frame so a still-running V2/V3/V4 overlay doesn't see
    // a black backdrop while the new V1 file warms up.
    if (kLastFrameRetentionDisabled || !m_retainLastFrameAcrossLoad)
        m_lastV1RawFrame = QImage();

    updatePlayButton();
    if (!m_suppressUiUpdates)
        m_seekBar->setRange(0, 0);
    if (m_glPreview)
        m_glPreview->setDisplayAspectRatio(0.0);
    updatePositionUi();
}

void VideoPlayer::scheduleNextFrame()
{
    if (!m_playing || !m_playbackTimer)
        return;

    const double absSpeed = qMax(0.25, std::abs(m_playbackSpeed));
    const int64_t baseFrameUs = (m_frameDurationUs > 0) ? m_frameDurationUs : (AV_TIME_BASE / 30);
    const int64_t frameIntervalUs = static_cast<int64_t>(baseFrameUs / absSpeed);
    const int frameIntervalMs = qMax(1, static_cast<int>(frameIntervalUs / 1000));
    int intervalMs = frameIntervalMs;

    // Audio-paced scheduling: pace the next frame against the AudioMixer's
    // audible clock (master clock minus OS-buffered samples) so video
    // tracks the playhead the user actually hears. The mixer publishes its
    // clock in TIMELINE microseconds (independent of which entry it's
    // currently sourcing from), so the J-cut/L-cut filePath-guard the old
    // QMediaPlayer path needed is gone — every active video entry can pace
    // off the same single audio clock.
    //
    // US-VFF-009 cold-start warmup: skip audio-paced gating for the first
    // 200 ms after play(). audibleClockUs can briefly report cursor (write
    // position, ~50–170 ms ahead of actual audible position) before the
    // sink's first readData publishes a stable lag. That makes
    // videoAheadUs deeply negative → waitUs deeply negative → "audio
    // meaningfully ahead" branch → intervalMs = floorMs (≈ frameInterval/2)
    // → timer fires at 2x rate → user-visible cold-start fast-forward.
    constexpr int kAudioPaceWarmupMs = 200;
    const bool inWarmup = m_lastPlayCallTimer.isValid()
        && m_lastPlayCallTimer.elapsed() < kAudioPaceWarmupMs;
    // VEDITOR_FORCE_FRAME_INTERVAL=1: bypass audio-paced gating entirely
    // (debug switch for cold-start fast-forward bisection). Forces
    // intervalMs = frameIntervalMs every tick → 1x video pace regardless
    // of audio clock readings. If the user's reported fast-forward
    // disappears with this flag set, the bug is in the audio-paced gating;
    // if it persists, the bug is elsewhere (timer, decode, m_currentPositionUs).
    // US-VFF-010 (default-on): video timer paces at frameIntervalMs and
    // skip-decode is disabled. AudioMixer alone owns A/V sync via the
    // sample-accurate sink output — video chasing the audio clock made
    // the cold-start audibleClockUs() inaccuracy (cursor reporting as
    // audible 50–170 ms before sink drains) trigger a 6-frame skip-decode
    // burst on every play(), the user-visible "early fast-forward" bug.
    // Empirical bisection (VEDITOR_FORCE_FRAME_INTERVAL=1 fixed the bug
    // outright) confirmed audio-paced gating was the source. Opt-out via
    // VEDITOR_FORCE_FRAME_INTERVAL_DISABLE=1 to restore the legacy chase
    // path for diagnostics.
    static const bool kForceFrameInterval =
        qEnvironmentVariableIntValue("VEDITOR_FORCE_FRAME_INTERVAL_DISABLE") == 0;
    if (m_mixer && m_mixer->isPlaying() && m_playbackSpeed >= 0.0
        && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()
        && !inWarmup
        && !kForceFrameInterval) {
        const int64_t audioTlUs = m_mixer->audibleClockUs();
        const int64_t videoAheadUs = m_timelinePositionUs - audioTlUs;
        const int64_t waitUs = static_cast<int64_t>(videoAheadUs / absSpeed);
        // Floor the interval at half-frame so catchup mode doesn't peg the
        // timer at 1 ms — every tick already runs up to 7 frame decodes
        // (correctVideoDriftAgainstAudioClock + decodeNextFrame), so a 1 ms
        // tick rate saturates one CPU thread without giving the decoder room
        // to actually catch up. Cap the regular case at frameIntervalMs so a
        // chronic audio-buffer lag (audible clock running behind audio write
        // by ~50 ms on Windows) cannot turn every tick into a >50 ms wait
        // and starve playback to ~19 fps; the audio clock catches up
        // organically over the next ticks since each tick still advances
        // the video timeline by exactly one frame. Larger seek-induced
        // drift is fixed by correctVideoDriftAgainstAudioClock's skip-
        // decode loop (and the optional VEDITOR_SEEK_ON_DRIFT seek), not
        // by the timer interval, so we don't need a multi-second ceiling.
        const int floorMs = qMax(1, frameIntervalMs / 2);
        // Bug fix (V1→V2 sequential boundary): AudioMixer::seekTo resets
        // m_audibleLagUs to 0, so right after a clip-boundary mixer seek
        // audibleClockUs == m_timelinePositionUs (both at V2_START) and
        // videoAheadUs is exactly 0. The previous "waitUs <= 0 → floorMs"
        // branch then paced video at half the frame interval, causing 2x
        // playback until the audio buffer's lag re-grew to ~50 ms (V1
        // steady state hides this because the chronic buffer lag keeps
        // videoAhead ≈ +50 ms, capping at frameIntervalMs). Use floorMs
        // only when audio is meaningfully ahead (>1 frame behind in
        // wait terms); aligned/ahead → frame-interval pace.
        const int64_t kAlignedThresholdUs = -baseFrameUs;
        if (waitUs <= kAlignedThresholdUs) {
            // Audio meaningfully ahead (>1 frame behind): catch up at floor
            intervalMs = floorMs;
        } else if (waitUs <= 0) {
            // Aligned within (-1 frame, 0]: pace at frame interval. AudioMixer
            // ::seekTo resets m_audibleLagUs to 0 (AudioMixer.cpp:1030,1064),
            // so right after a clip-boundary mixer seek audibleClockUs ==
            // m_timelinePositionUs (both V2_START) and waitUs is exactly 0;
            // the previous "waitUs <= 0 → floorMs" branch then paced video
            // at half the frame interval, causing 2x playback at V1→V2
            // sequential boundaries. V1 steady state hides this because the
            // chronic ~50 ms buffer lag keeps videoAhead positive.
            intervalMs = frameIntervalMs;
        } else {
            // Graduated catchup for waitUs in (0, frameInterval]: original
            // behaviour preserved so audio buffer lag oscillation can
            // converge instead of step-changing every tick.
            intervalMs = static_cast<int>(qBound<int64_t>(
                static_cast<int64_t>(floorMs),
                waitUs / 1000,
                static_cast<int64_t>(frameIntervalMs)));
        }
        // DIAG (V2 fast-forward bug investigation): emit one log line per
        // tick when audio-paced gate is engaged. Will be pruned after empirical.
        static int s_diagCount = 0;
        if (s_diagCount++ < 80) {
            qInfo().nospace() << "[v2diag] entry=" << m_activeEntry
                              << " baseFrameUs=" << baseFrameUs
                              << " frameIntervalMs=" << frameIntervalMs
                              << " audioTlUs=" << audioTlUs
                              << " timelineUs=" << m_timelinePositionUs
                              << " videoAheadUs=" << videoAheadUs
                              << " waitUs=" << waitUs
                              << " intervalMs=" << intervalMs;
        }
    }
    // Body-time correction only when the cap actually fired. In the
    // synced case the audio-paced wait already equals frameInterval -
    // body_time (audible advances by body_time during the tick body),
    // so subtracting body_time again would over-pace the video and
    // desync against audio. When the cap fired (audio buffer lag pushed
    // waitUs > frameInterval), the wait was clamped to frameInterval
    // and body_time wasn't subtracted; subtract it here so the cycle
    // matches frameInterval rather than body+frameInterval.
    if (m_tickWallStart.isValid() && intervalMs == frameIntervalMs) {
        const qint64 bodyMs = m_tickWallStart.elapsed();
        intervalMs = qMax(1, intervalMs - static_cast<int>(bodyMs));
    }
    m_playbackTimer->start(intervalMs);
}

int VideoPlayer::correctVideoDriftAgainstAudioClock()
{
    // Match the AudioMixer master clock against the video timeline cursor.
    // Reverse playback or a missing video entry short-circuits to keep the
    // legacy single-file path untouched. With the mixer publishing
    // timeline-unified microseconds, J-cut/L-cut no longer needs a filePath
    // guard — every active entry shares one clock source.
    if (!m_mixer || m_playbackSpeed < 0.0)
        return 0;
    if (!m_mixer->isPlaying())
        return 0;
    if (m_activeEntry < 0 || m_activeEntry >= m_sequence.size())
        return 0;

    // VEDITOR_FORCE_FRAME_INTERVAL=1: also short-circuit drift correction
    // (debug bisection for cold-start fast-forward).
    // US-VFF-010 (default-on): video timer paces at frameIntervalMs and
    // skip-decode is disabled. AudioMixer alone owns A/V sync via the
    // sample-accurate sink output — video chasing the audio clock made
    // the cold-start audibleClockUs() inaccuracy (cursor reporting as
    // audible 50–170 ms before sink drains) trigger a 6-frame skip-decode
    // burst on every play(), the user-visible "early fast-forward" bug.
    // Empirical bisection (VEDITOR_FORCE_FRAME_INTERVAL=1 fixed the bug
    // outright) confirmed audio-paced gating was the source. Opt-out via
    // VEDITOR_FORCE_FRAME_INTERVAL_DISABLE=1 to restore the legacy chase
    // path for diagnostics.
    static const bool kForceFrameInterval =
        qEnvironmentVariableIntValue("VEDITOR_FORCE_FRAME_INTERVAL_DISABLE") == 0;
    if (kForceFrameInterval)
        return 0;

    const int64_t audioTlUs = m_mixer->audibleClockUs();
    const int64_t frameUs = (m_frameDurationUs > 0) ? m_frameDurationUs : (AV_TIME_BASE / 30);

    // VEDITOR_SEEK_ON_DRIFT=1 opt-in (Phase 1e Sprint US-2): when drift
    // exceeds ~30 video frames (~1 sec at 30fps), do a single keyframe
    // seek instead of chewing through up to 6 throwaway decodes per tick.
    // Trades a brief audio glitch on the seek for eliminating the wasted
    // decode time observed in baseline tick traces. Default off so the
    // legacy skip-decode path runs identically to before.
    static const bool seekOnDrift =
        qEnvironmentVariableIntValue("VEDITOR_SEEK_ON_DRIFT") != 0;
    if (seekOnDrift) {
        const int64_t driftUs = audioTlUs - m_timelinePositionUs;
        if (driftUs > frameUs * 30) {
            seekToTimelineUs(audioTlUs, /*precise=*/false);
            return 0;
        }
    }

    // US-VFF-009 cold-start warmup guard: suppress skip-decode for the
    // first 200 ms after play() starts. The audio sink takes ~1-3 readData
    // callbacks to publish a stable lag from real OS-buffer state; before
    // that, audibleClockUs() can briefly report a position 50–170 ms ahead
    // of what's physically audible (the sample-write cursor minus a stale
    // pre-fill lag of 0). This window must NOT trigger the skip-decode
    // catch-up loop, otherwise video discards 6 frames in one tick and
    // the user sees the cold-start fast-forward burst, even on single
    // clip playback. m_lastPlayCallTimer.restart() is invoked in play()
    // so it tracks "ms since this play started".
    constexpr int kWarmupSuppressMs = 200;
    if (m_lastPlayCallTimer.isValid()
        && m_lastPlayCallTimer.elapsed() < kWarmupSuppressMs) {
        return 0;
    }

    // Threshold > 1.5 frames avoids skipping on natural scheduler jitter.
    const int64_t catchupThresholdUs = (frameUs * 3) / 2;
    const int maxSkips = 6; // bounded so one tick can't block the UI

    int skipped = 0;
    while (skipped < maxSkips) {
        if (audioTlUs - m_timelinePositionUs <= catchupThresholdUs)
            break;
        // Skip-decode WITHOUT displaying — handlePlaybackTick calls
        // decodeNextFrame(true) right after this returns, so the frame the
        // user sees is always the freshest, never a stale skipped one.
        if (!decodeNextFrame(false))
            break;
        ++skipped;
    }
    // [v2ff-skip-probe] log first 30 boundary or fast-forward events so we
    // can prove whether skip-decode is firing during the empirical reproduction.
    if (skipped > 0) {
        static int s_probeCount = 0;
        if (s_probeCount++ < 30) {
            qInfo().nospace()
                << "[v2ff-skip] tick skipped=" << skipped
                << " audioTl=" << audioTlUs
                << " timelineUs=" << m_timelinePositionUs
                << " drift=" << (audioTlUs - m_timelinePositionUs)
                << " catchupThresholdUs=" << catchupThresholdUs
                << " entry=" << m_activeEntry;
        }
    }
    return skipped;
}

void VideoPlayer::performPendingSeek()
{
    if (m_seekInProgress || m_pendingSeekMs < 0)
        return;

    const bool seqMode = sequenceActive();
    if (!seqMode && (!m_formatCtx || !m_codecCtx)) {
        m_pendingSeekMs = -1;
        return;
    }

    m_seekInProgress = true;

    const bool wasPlaying = m_playing;
    const int requestedMs = m_pendingSeekMs;
    const bool precise = m_pendingSeekPrecise;
    m_pendingSeekMs = -1;
    m_pendingSeekPrecise = false;

    bool seekOk;
    if (seqMode) {
        const int64_t timelineUs = static_cast<int64_t>(requestedMs) * 1000;
        // Preview seeks (non-precise / drag) MUST NOT switch files. Each
        // file switch reloads the FFmpeg video decoder and the user
        // hears the old audio briefly overlapping with the new — perceived
        // as "double playback". Refuse cross-file preview seeks; the final
        // committed seek (sliderReleased / valueChanged) handles the switch.
        if (!precise) {
            const int64_t clampedTimelineUs = qBound<int64_t>(0, timelineUs, m_sequenceDurationUs);
            const int idx = findActiveEntryAt(clampedTimelineUs);
            if (idx >= 0 && idx < m_sequence.size()
                && m_sequence[idx].filePath != m_loadedFilePath
                && m_formatCtx) {
                qInfo() << "VideoPlayer: skipping preview file switch idx=" << idx;
                seekOk = false;
            } else {
                seekOk = seekToTimelineUs(timelineUs, precise);
            }
        } else {
            seekOk = seekToTimelineUs(timelineUs, precise);
        }
    } else {
        const int64_t targetUs = static_cast<int64_t>(requestedMs) * 1000;
        seekOk = seekInternal(targetUs, true, precise);
    }

    m_seekInProgress = false;
    // After a seek completes, V2+ pool decoders may still hold lastFrameRgb
    // and currentPositionUs from before the seek. Ask the next playback tick
    // to clear firstFrameDecoded on every active pool decoder so
    // harvestOverlayLayer drops into its catch-up loop and lands on the
    // correct file-local position for the new playhead.
    m_postSeekResyncRequested = true;

    // ADAPTIVE-1: シーク確定 (scrub 終了 / 明示シーク) は静止画提示でもある。
    // 品質ポリシーを 1.0 にリセットし、次 tick / 静止再描画がフル解像度で合成される
    // ようにする (停止画くっきり)。seekInternal/seekToTimelineUs が既に displayFrame
    // を 1 回呼んでいるので、ここでは新たな displayFrame / 再描画ループは起こさない
    // (フリッカー再発防止: 1 経路 = 最大 1 frame を維持)。
    m_qualityPolicy.reset();
    m_adaptiveCanvasDivisor = 1;
    m_adaptiveQualityTier = 0;
    m_lastTickRenderMs = 0.0;

    if (m_pendingSeekMs >= 0) {
        if (m_seekTimer)
            m_seekTimer->start();
        return;
    }

    // NOTE: audio is owned by AudioMixer's master clock — seekToTimelineUs
    // calls m_mixer->seekTo with the timeline position. Do NOT push a
    // setPosition equivalent here; the previous shim used VIDEO file-local
    // coordinates as an AUDIO player position which caused drift after seek.

    if (seekOk && wasPlaying)
        scheduleNextFrame();
}

void VideoPlayer::updatePositionUi()
{
    if (m_suppressUiUpdates) return;
    int64_t displayUs;
    int64_t totalUs;
    if (sequenceActive()) {
        // Reproject the current file-local position into timeline coordinates
        // so the slider, time label and positionChanged signal all speak the
        // same timeline-space the Timeline widget uses.
        // US-VFF-003: reject stale-timelineStart reprojections. v2diag captured
        // a transient V2 timelineStart=7649.7s that settled to 6386.09s;
        // reprojecting the same file-local cursor through that stale start
        // produced the bogus 10628s seekTo leak into AudioMixer (see
        // .omc/state/v2ff_rca.md). The 2-frame acceptance window admits
        // decoder-driven progression; explicit seeks bypass via the seek flags.
        if (m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
            const int64_t projected =
                fileLocalToTimelineUs(m_activeEntry, m_currentPositionUs);
            const int64_t baseFrameUs = (m_frameDurationUs > 0)
                ? m_frameDurationUs : (AV_TIME_BASE / 30);
            const int64_t deltaUs = projected - m_timelinePositionUs;
            const int64_t acceptUs = baseFrameUs * 2;
            if ((deltaUs >= -acceptUs && deltaUs <= acceptUs)
                || m_seekInProgress || m_pendingSeekMs >= 0) {
                m_timelinePositionUs = projected;
            } else {
                static QElapsedTimer s_warnThrottle;
                if (!s_warnThrottle.isValid() || s_warnThrottle.elapsed() > 1000) {
                    qWarning().nospace()
                        << "[VFF] rejected stale reprojection: projected=" << projected
                        << " canonical=" << m_timelinePositionUs
                        << " delta=" << deltaUs << " entry=" << m_activeEntry;
                    s_warnThrottle.start();
                }
            }
        }
        displayUs = m_timelinePositionUs;
        totalUs = m_sequenceDurationUs;
    } else {
        displayUs = m_currentPositionUs;
        totalUs = m_durationUs;
    }

    const int sliderValue = qMin(sliderTimelinePosition(displayUs), m_seekBar->maximum());
    const QSignalBlocker blocker(m_seekBar);
    if (!m_seekBar->isSliderDown())
        m_seekBar->setValue(sliderValue);
    m_timeLabel->setText(QString("%1 / %2")
        .arg(formatTimestamp(displayUs))
        .arg(formatTimestamp(totalUs)));
    emit positionChanged(static_cast<double>(displayUs) / AV_TIME_BASE);
}

bool VideoPlayer::seekInternal(int64_t positionUs, bool displayFrame, bool precise)
{
    undotrace::log("seek:enter");
    if (!m_formatCtx || !m_codecCtx || m_videoStreamIndex < 0)
        return false;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    const int64_t targetUs = qMax<int64_t>(0, (m_durationUs > 0) ? qMin(positionUs, m_durationUs) : positionUs);
    const int64_t targetTimestamp = streamTimestampForPosition(targetUs);

    if (av_seek_frame(m_formatCtx, m_videoStreamIndex, targetTimestamp, AVSEEK_FLAG_BACKWARD) < 0)
        return false;

    avcodec_flush_buffers(m_codecCtx);
    m_currentPositionUs = targetUs;

    if (!displayFrame) {
        updatePositionUi();
        return true;
    }

    bool foundFrame = false;
    undotrace::log("seek:beforeDecodeLoop");
    while (decodeNextFrame(false)) {
        undotrace::log("seek:decodeIter");
        foundFrame = true;
        if (!precise || m_currentPositionUs >= targetUs)
            break;
    }
    undotrace::log("seek:afterDecodeLoop");

    if (foundFrame && m_frame) {
        AVFrame *displayable = ensureSwFrame(m_frame);
        if (!displayable)
            return false;
        const QImage image = frameToImage(displayable);
        if (image.isNull())
            return false;
        // This path displays a non-baked frame.
        QImage displayImage = image;
        if (sequenceActive()
            && m_activeEntry >= 0
            && m_activeEntry < m_sequence.size()) {
            const auto &entry = m_sequence[m_activeEntry];
            displayImage = applyPreviewClipMask(
                image,
                m_glPreview ? m_glPreview->timeline() : nullptr,
                entry,
                static_cast<double>(targetUs) / AV_TIME_BASE,
                QSize(displayable->width, displayable->height));
        }
        m_lastFrameOdtApplied = false;
        this->displaySeekFrameConformed(displayImage);
        m_currentPositionUs = targetUs;
        updatePositionUi();
        return true;
    }

    updatePositionUi();
    return false;
}

bool VideoPlayer::decodeNextFrame(bool displayFrame)
{
    if (!m_formatCtx || !m_codecCtx || !m_packet || !m_frame)
        return false;

    const auto receiveFrame = [this, displayFrame]() -> bool {
        const int receiveResult = avcodec_receive_frame(m_codecCtx, m_frame);
        if (receiveResult == 0)
            return presentDecodedFrame(m_frame, displayFrame);
        return false;
    };

    if (receiveFrame())
        return true;

    while (av_read_frame(m_formatCtx, m_packet) >= 0) {
        if (m_packet->stream_index != m_videoStreamIndex) {
            av_packet_unref(m_packet);
            continue;
        }

        const int sendResult = avcodec_send_packet(m_codecCtx, m_packet);
        av_packet_unref(m_packet);
        if (sendResult < 0)
            continue;

        if (receiveFrame())
            return true;
    }

    if (avcodec_send_packet(m_codecCtx, nullptr) >= 0 && receiveFrame())
        return true;

    return false;
}

bool VideoPlayer::presentDecodedFrame(AVFrame *frame, bool displayFrameRequested)
{
    int64_t positionUs = m_currentPositionUs;
    const int64_t bestEffortTimestamp =
        (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : frame->pts;

    if (bestEffortTimestamp != AV_NOPTS_VALUE) {
        positionUs = positionFromStreamTimestamp(bestEffortTimestamp);
    } else if (m_frameDurationUs > 0) {
        positionUs += m_frameDurationUs;
    }

    positionUs = qMax<int64_t>(0, positionUs);
    if (m_durationUs > 0)
        positionUs = qMin(positionUs, m_durationUs);
    m_currentPositionUs = positionUs;

    if (displayFrameRequested) {
        // Phase 1e fast path — when the narrow set of preconditions holds
        // we hand the FFmpeg D3D11 texture to GLPreview without an
        // av_hwframe_transfer_data + sws_scale round-trip. m_lastSourceFrame
        // is intentionally NOT updated here; pause/resize during fast path
        // shows whatever was last cached by the legacy path. Acceptable for
        // V1-only narrow conditions; revisit if user reports staleness.
        if (canUseInteropFastPath(frame) && !m_projectOutputSize.isValid()) {
            D3D11FrameRef ref;
            if (extractD3D11FrameRef(frame, &ref)) {
                static bool loggedFastPathEngage = false;
                if (!loggedFastPathEngage) {
                    qInfo() << "[interop] fast path engaged for V1 (texture="
                            << ref.texture << "subres=" << ref.subresource
                            << ref.width << "x" << ref.height << ")";
                    loggedFastPathEngage = true;
                }
                m_glPreview->displayD3D11Frame(ref.texture, ref.subresource,
                                               ref.width, ref.height);
                updatePositionUi();
                return true;
            }
        }
        AVFrame *displayable = ensureSwFrame(frame);
        if (!displayable)
            return false;
        const QImage image = frameToImage(displayable);
        if (image.isNull())
            return false;
        // Cache the raw V1 source BEFORE the display gate so the Phase 1d
        // compositor in handlePlaybackTick has the latest frame to paint
        // overlays on, even when we defer the displayFrame call below.
        // m_lastV1RawFrame is the dedicated compositor base — it stays a
        // pristine V1 frame regardless of what displayFrame writes back
        // into m_lastSourceFrame later in the pipeline.
        m_lastV1RawFrame = image;
        m_lastSourceFrame = image;
        if (!m_deferDisplayThisTick) {
            QImage displayImage = image;
            if (sequenceActive()
                && m_activeEntry >= 0
                && m_activeEntry < m_sequence.size()) {
                const auto &entry = m_sequence[m_activeEntry];
                const double sourceSec =
                    static_cast<double>(
                        entryLocalPositionUs(m_activeEntry, m_timelinePositionUs))
                    / AV_TIME_BASE;
                displayImage = applyPreviewClipMask(
                    image,
                    m_glPreview ? m_glPreview->timeline() : nullptr,
                    entry,
                    sourceSec,
                    QSize(displayable->width, displayable->height));
            }
            // This path displays a non-baked frame.
            m_lastFrameOdtApplied = false;
            displaySeekFrameConformed(displayImage);
        }
        updatePositionUi();
    }

    return true;
}

enum AVPixelFormat VideoPlayer::getHwFormatCallback(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts)
{
    auto *self = static_cast<VideoPlayer*>(ctx->opaque);
    if (!self || self->m_hwPixFmt == AV_PIX_FMT_NONE)
        return pixFmts[0];
    for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == self->m_hwPixFmt)
            return *p;
    }
    qWarning() << "HW pixel format not offered by decoder, falling back to SW";
    return pixFmts[0];
}

AVFrame *VideoPlayer::ensureSwFrame(AVFrame *frame)
{
    if (!frame)
        return nullptr;
    if (m_hwPixFmt == AV_PIX_FMT_NONE || frame->format != m_hwPixFmt)
        return frame;
    if (!m_swFrame)
        return nullptr;

    av_frame_unref(m_swFrame);
    if (av_hwframe_transfer_data(m_swFrame, frame, 0) < 0) {
        qWarning() << "av_hwframe_transfer_data failed";
        return nullptr;
    }
    m_swFrame->pts = frame->pts;
    m_swFrame->best_effort_timestamp = frame->best_effort_timestamp;
    return m_swFrame;
}

QImage VideoPlayer::frameToImage(const AVFrame *frame)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        qWarning() << "frameToImage: invalid frame";
        return {};
    }

    const bool hdr = m_hdrInfo.isHdr;
    const AVPixelFormat dstPixFmt = hdr ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB24;
    const QImage::Format qFmt = hdr ? QImage::Format_RGBA64 : QImage::Format_RGB888;

    // Phase 1e proxy: optionally downscale during sws_scale so the
    // destination QImage is 1/N each axis. Compose draws this small
    // image at full canvas dst rect with nearest sampling.
    const int proxy = playbackProxyDivisor();
    const int dstW = qMax(2, frame->width  / proxy);
    const int dstH = qMax(2, frame->height / proxy);

    m_swsCtx = sws_getCachedContext(
        m_swsCtx,
        frame->width,
        frame->height,
        static_cast<AVPixelFormat>(frame->format),
        dstW,
        dstH,
        dstPixFmt,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (!m_swsCtx) {
        qWarning() << "frameToImage: sws_getCachedContext failed";
        return {};
    }

    QImage image(dstW, dstH, qFmt);
    if (image.isNull()) {
        qWarning() << "frameToImage: QImage alloc failed" << dstW << "x" << dstH;
        return {};
    }

    if (!scaleFrameToQImagePadded(m_swsCtx, frame, dstPixFmt, image)) {
        qWarning() << "frameToImage: padded sws_scale failed";
        return {};
    }
    return image;
}

int64_t VideoPlayer::streamFrameDurationUs() const
{
    if (!m_formatCtx || m_videoStreamIndex < 0)
        return AV_TIME_BASE / 30;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    AVRational frameRate = stream->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0)
        frameRate = stream->r_frame_rate;
    if (frameRate.num > 0 && frameRate.den > 0)
        return qMax<int64_t>(1, av_rescale_q(1, av_inv_q(frameRate), AV_TIME_BASE_Q));

    return AV_TIME_BASE / 30;
}

int64_t VideoPlayer::streamTimestampForPosition(int64_t positionUs) const
{
    if (!m_formatCtx || m_videoStreamIndex < 0)
        return positionUs;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    int64_t timestamp = av_rescale_q(positionUs, AV_TIME_BASE_Q, stream->time_base);
    if (stream->start_time != AV_NOPTS_VALUE)
        timestamp += stream->start_time;
    return timestamp;
}

int64_t VideoPlayer::positionFromStreamTimestamp(int64_t timestamp) const
{
    if (!m_formatCtx || m_videoStreamIndex < 0)
        return timestamp;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    if (stream->start_time != AV_NOPTS_VALUE)
        timestamp -= stream->start_time;
    return av_rescale_q(timestamp, stream->time_base, AV_TIME_BASE_Q);
}

int VideoPlayer::sliderPositionForUs(int64_t positionUs) const
{
    const int64_t positionMs = qMax<int64_t>(0, positionUs / 1000);
    return static_cast<int>(qMin<int64_t>(positionMs, std::numeric_limits<int>::max()));
}

double VideoPlayer::streamDisplayAspectRatio() const
{
    if (!m_formatCtx || !m_codecCtx || m_videoStreamIndex < 0 || m_codecCtx->height <= 0)
        return 0.0;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    AVRational sampleAspect = av_guess_sample_aspect_ratio(m_formatCtx, stream, nullptr);
    if (sampleAspect.num <= 0 || sampleAspect.den <= 0)
        sampleAspect = stream->sample_aspect_ratio;
    if (sampleAspect.num <= 0 || sampleAspect.den <= 0)
        sampleAspect = m_codecCtx->sample_aspect_ratio;

    double aspectRatio = static_cast<double>(m_codecCtx->width) / m_codecCtx->height;
    if (sampleAspect.num > 0 && sampleAspect.den > 0)
        aspectRatio *= av_q2d(sampleAspect);

    return (aspectRatio > 0.0 && std::isfinite(aspectRatio)) ? aspectRatio : 0.0;
}

double VideoPlayer::effectiveDisplayAspectRatio() const
{
    if (m_projectOutputSize.isValid() && m_projectOutputSize.height() > 0)
        return static_cast<double>(m_projectOutputSize.width()) / m_projectOutputSize.height();

    if (m_displayAspectRatio > 0.0 && std::isfinite(m_displayAspectRatio))
        return m_displayAspectRatio;

    if (!m_currentFrameImage.isNull() && m_currentFrameImage.height() > 0)
        return static_cast<double>(m_currentFrameImage.width()) / m_currentFrameImage.height();

    return 0.0;
}

QSize VideoPlayer::fittedDisplaySize(const QSize &bounds) const
{
    if (!bounds.isValid())
        return QSize(1, 1);

    const double aspectRatio = effectiveDisplayAspectRatio();
    if (!(aspectRatio > 0.0) || !std::isfinite(aspectRatio))
        return bounds;

    int targetWidth = bounds.width();
    int targetHeight = qRound(targetWidth / aspectRatio);
    if (targetHeight > bounds.height()) {
        targetHeight = bounds.height();
        targetWidth = qRound(targetHeight * aspectRatio);
    }

    return QSize(qMax(1, targetWidth), qMax(1, targetHeight));
}

void VideoPlayer::refreshDisplayedFrame()
{
    undotrace::log("refresh:enter");
    // Use the raw source cache, NOT m_currentFrameImage — the latter is
    // already composited, so re-running composeFrameWithOverlays on it
    // would burn the overlays in twice (visible as duplicated text when
    // an existing overlay is selected and setTextOverlays re-pushes).
    if (m_lastSourceFrame.isNull())
        return;
    undotrace::log("refresh:conform");
    if (m_projectOutputSize.isValid()
        && sequenceActive()
        && !m_lastV1RawFrame.isNull()
        && m_activeEntry >= 0
        && m_activeEntry < m_sequence.size()) {
        const QVector<int> activeIdxs = findActiveEntriesAt(m_timelinePositionUs);
        const auto &e = m_sequence[m_activeEntry];
        if (!hasOverlayActive(activeIdxs) && e.sourceTrack == 0) {
            QImage canvas(m_projectOutputSize, QImage::Format_ARGB32_Premultiplied);
            if (!canvas.isNull()) {
                canvas.fill(Qt::black);
                DecodedLayer layer;
                const Timeline *previewTimeline =
                    m_glPreview ? m_glPreview->timeline() : nullptr;
                const QSize sourceSize =
                    (m_codecCtx && m_codecCtx->width > 0 && m_codecCtx->height > 0)
                        ? QSize(m_codecCtx->width, m_codecCtx->height)
                        : m_lastV1RawFrame.size();
                const double sourceSec =
                    static_cast<double>(
                        entryLocalPositionUs(m_activeEntry, m_timelinePositionUs))
                    / AV_TIME_BASE;
                layer.rgb = applyPreviewClipMask(
                    m_lastV1RawFrame, previewTimeline, e, sourceSec, sourceSize);
                layer.isFresh = true;
                layer.colorMeta = e.colorMeta;
                applyLayerMotionOpacity(previewTimeline,
                                        e, m_timelinePositionUs, e.opacity, &layer);
                populateLayerMetadata(previewTimeline, e, m_activeEntry, &layer);
                {
                    double insetFw = 1.0, insetFh = 1.0;
                    if (layer.fitContain && !layer.fitCover) {
                        snsfit::containContentInset(layer.rgb.size(), m_projectOutputSize, insetFw, insetFh);
                    }
                    if (m_glPreview)
                        m_glPreview->setVideoContentInset(insetFw, insetFh);
                }
                layer.rgb = snsfit::maybeFit(layer.rgb, layer.fitContain, layer.fitCover, m_projectOutputSize);
                QVector<DecodedLayer> singleLayer;
                singleLayer.append(layer);
                composeMultiTrackFrameInto(canvas, singleLayer);
                if (m_glPreview)
                    m_glPreview->setCompositeBakedMode(true);
                m_lastFrameOdtApplied = false;
                displayFrame(canvas);
                return;
            }
        }
    }
    // Phase 1e Win #9 — m_lastSourceFrame may hold a canvas-proxy
    // smaller image cached during multi-track playback (when
    // m_textOverlays was empty at compose time, the canvas was sized
    // to the decoded layer res). composeFrameWithOverlays paints text
    // at pixel coordinates on the source — a smaller cached image
    // would shift those positions after GL bilinear upscale to the
    // widget viewport. When text overlays are now present and the
    // cache is undersized, scale it up to full canvas dims so text
    // lands where the user expects (the prior multi-track tick's
    // composited pixels survive the upscale; only resolution drops).
    // Skipped for size-matched frames: single-track playback,
    // PLAYBACK_PROXY=1, or text-overlay-active multi-track which
    // already forced full-res via the gate at handlePlaybackTick.
    if (!m_textOverlays.isEmpty()
        && m_canvasWidth  > 0
        && m_canvasHeight > 0
        && (m_lastSourceFrame.width()  != m_canvasWidth
            || m_lastSourceFrame.height() != m_canvasHeight)) {
        m_lastSourceFrame = m_lastSourceFrame.scaled(
            m_canvasWidth, m_canvasHeight,
            Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    // This path displays a non-baked frame.
    m_lastFrameOdtApplied = false;
    undotrace::log("refresh:fallthrough");
    displayFrame(m_lastSourceFrame);
}

void VideoPlayer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_regionPickerOverlay && m_glPreview) {
        QStackedWidget *stack = qobject_cast<QStackedWidget *>(m_glPreview->parentWidget());
        m_regionPickerOverlay->setGeometry(stack ? stack->geometry() : m_glPreview->geometry());
    }
    if (m_wbEyedropperOverlay && m_glPreview) {
        QStackedWidget *stack = qobject_cast<QStackedWidget *>(m_glPreview->parentWidget());
        m_wbEyedropperOverlay->setGeometry(stack ? stack->geometry() : m_glPreview->geometry());
    }
    refreshDisplayedFrame();
}

void VideoPlayer::recordTickTrace(qint64 workNs)
{
    if (!tickTraceEnabled())
        return;
    m_tickTraceWorkNs += workNs;
    if (++m_tickTraceCount < 30)
        return;
    qInfo().nospace()
        << "[tick] ticks=" << m_tickTraceCount
        << " workMs=" << nsToMs(m_tickTraceWorkNs)
        << " decodeMs=" << nsToMs(m_tickTraceDecodeNs)
        << " composeMs=" << nsToMs(m_tickTraceComposeNs)
        << " driftMs=" << nsToMs(m_tickTraceDriftNs)
        << " skipped=" << m_tickTraceSkipped;
    m_tickTraceCount = 0;
    m_tickTraceWorkNs = 0;
    m_tickTraceDecodeNs = 0;
    m_tickTraceComposeNs = 0;
    m_tickTraceDriftNs = 0;
    m_tickTraceSkipped = 0;
}

void VideoPlayer::updateAdaptiveQuality()
{
    // ADAPTIVE-1: 直近 tick の合成ウォール時間とフレーム予算を policy に相談し、
    // 次 tick で使うキャンバス縮小係数 (m_adaptiveCanvasDivisor) と
    // 品質 tier (m_adaptiveQualityTier) を決める。書き出し経路には一切触れない。
    if (!m_adaptivePreviewEnabled) {
        m_adaptiveCanvasDivisor = 1;
        m_adaptiveQualityTier = 0;
        return;
    }

    playback::PlaybackMetrics metrics;
    metrics.trackCount = m_sequence.size();
    metrics.activeClipCount =
        findActiveEntriesAt(m_timelinePositionUs).size();
    metrics.lastFrameRenderMs = m_lastTickRenderMs;
    const int64_t baseFrameUs =
        (m_frameDurationUs > 0) ? m_frameDurationUs : (AV_TIME_BASE / 30);
    const double absSpeed = qMax(0.25, std::abs(m_playbackSpeed));
    // フレーム予算 = 1 フレーム間隔 (ms)。倍速再生時は予算も縮む。
    metrics.targetFrameMs =
        qMax(1.0, (static_cast<double>(baseFrameUs) / 1000.0) / absSpeed);
    metrics.isPlaying = m_playing;
    // プロキシ候補判定: 再生用プレビュープロキシ divisor (環境設定
    // / シークバー左のプロキシボタン m_proxyDivisor) のいずれかが等倍 (1) 以外なら
    // 「プロキシ的な縮小が利用可能」と見なす。ProxyManager のグローバルトグル状態は
    // ここでは参照せず、VideoPlayer 内で確定している縮小設定だけで判定する
    // (依存を最小化し、判定が外部状態に左右されないようにするため)。
    metrics.proxyAvailable = (playbackProxyDivisor() > 1) || (m_proxyDivisor > 1);

    const playback::PlaybackQualityDecision d = m_qualityPolicy.decide(metrics);

    // scaleFactor (1.0 / 0.5 / minScale=0.25) を整数キャンバス縮小係数へ。
    // 1.0→1, 0.5→2, それ未満→4。最低 1px は canvasW/H 側で qMax 済み。
    int divisor = 1;
    if (d.scaleFactor >= 0.99)      divisor = 1;
    else if (d.scaleFactor >= 0.49) divisor = 2;
    else                            divisor = 4;
    m_adaptiveCanvasDivisor = divisor;
    // qualityTier はキャッシュキーの一部。同一プレイヘッドでも tier が違えば別物。
    m_adaptiveQualityTier = divisor;
}

void VideoPlayer::cachePreviewComposite(const QImage &composed)
{
    if (!m_adaptivePreviewEnabled || composed.isNull())
        return;
    playback::CompositeFrameKey key;
    key.timelineRevision = m_timelineRevision;
    key.timeMs       = m_timelinePositionUs / 1000;
    key.width        = composed.width();
    key.height       = composed.height();
    key.qualityTier  = m_adaptiveQualityTier;
    key.useProxy     = (m_proxyDivisor > 1) || (playbackProxyDivisor() > 1);
    m_frameCache.put(key, composed);
}

void VideoPlayer::handlePlaybackTick()
{
    if (!m_playing)
        return;

    m_lastFrameOdtApplied = false;

    // Anchor the tick body so scheduleNextFrame can fire the next tick at
    // tickWallStart + frameInterval (constant cadence) rather than
    // bodyEnd + frameInterval (sliding cadence that drifts ~body_time
    // slower than real time).
    m_tickWallStart.start();

    // ADAPTIVE-1: 直前 tick の合成時間に基づき今 tick のプレビュー品質を確定する。
    // policy はヒステリシス付きなので毎 tick で振動しない。停止/シーク時は
    // updateAdaptiveQuality を呼ばず (この関数は再生 tick からのみ)、停止経路は
    // policy.reset() 相当 (m_lastTickRenderMs を 0 に戻すのは stop()/seek 側) に委ねる。
    updateAdaptiveQuality();
    // 今 tick の合成ウォール時間を実測するタイマー。displayFrame までを測る。
    QElapsedTimer adaptiveRenderTimer;
    adaptiveRenderTimer.start();
    auto recordAdaptiveRenderTime = [&]() {
        m_lastTickRenderMs = static_cast<double>(adaptiveRenderTimer.nsecsElapsed())
                             / 1000000.0;
    };

    QElapsedTimer tickTimer;
    const bool traceTick = tickTraceEnabled();
    const bool traceStall = stallTraceEnabled();
    qint64 sectionMark = 0;
    if (traceTick || traceStall)
        tickTimer.start();
    auto reportStallIfSlow = [&]() {
        if (!traceStall || !tickTimer.isValid()) return;
        const qint64 elapsedMs = tickTimer.nsecsElapsed() / 1000000LL;
        if (elapsedMs >= kStallThresholdTickMs) {
            qWarning().noquote()
                << QStringLiteral("[stall>=%1ms] tick %2ms tlUs=%3 activeEntry=%4")
                       .arg(kStallThresholdTickMs)
                       .arg(elapsedMs)
                       .arg(m_timelinePositionUs)
                       .arg(m_activeEntry);
        }
    };

    // Feed the slot manager's distance-from-playhead eviction heuristic.
    // Without this every slot looks equidistant from a stale playhead at
    // 0, so eviction degenerates to "drop the slot with the largest
    // clipStartSec" regardless of what's actually playing.
    m_slotManager.setPlayheadPosition(
        static_cast<double>(m_timelinePositionUs) / static_cast<double>(AV_TIME_BASE));

    // Sweep the eviction grace pool first so any decoder whose graceTtl
    // dropped to 0 last tick is freed before we touch the pool again. Safe
    // here because no decode loop has started this tick.
    tickEvictionGracePool();

    // Post-seek resync: clear firstFrameDecoded on every V2+ pool decoder
    // so harvestOverlayLayer drops into its long catch-up path on the very
    // next call, instead of trusting stale lastFrameRgb from before the
    // seek. Cheap — sets a bool, doesn't free anything.
    if (m_postSeekResyncRequested) {
        for (auto it = m_trackDecoders.begin(); it != m_trackDecoders.end(); ++it) {
            if (it.value())
                it.value()->firstFrameDecoded = false;
        }
        m_postSeekResyncRequested = false;
    }

    // Phase 1e Win #16 (Iteration 9) — boundary preroll. When the playhead
    // is within kPrerollUs of the active entry's timelineEnd, ask
    // acquireDecoderForClip to open + slot-allocate the NEXT same-track
    // entry's TrackDecoder. The reachedEntryEnd → advanceToEntry path then
    // finds a warm decoder via tryPromotePoolDecoderTo and skips the
    // 60-200 ms loadFile stall. De-duped by m_prerolledEntryIdx; reset
    // whenever the prerolled entry has rotated into the active slot.
    static const bool kPrerollDisabled =
        qEnvironmentVariableIntValue("VEDITOR_BOUNDARY_PREROLL_DISABLE") != 0;
    if (!kPrerollDisabled && sequenceActive()
        && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
        if (m_prerolledEntryIdx == m_activeEntry) {
            m_prerolledEntryIdx = -1;
        }
        constexpr int64_t kPrerollUs = 1'000'000LL; // 1.0 s
        const auto &active = m_sequence[m_activeEntry];
        const int64_t activeEndUs = static_cast<int64_t>(active.timelineEnd * AV_TIME_BASE);
        if (m_timelinePositionUs >= activeEndUs - kPrerollUs) {
            // Track-aware next-entry search so boundary preroll doesn't
            // accidentally warm up an unrelated V2/V3 layer that happens
            // to sort after the active entry in m_sequence's flat order.
            const double eps = 1e-3;
            int prerollIdx = -1;
            for (int i = 0; i < m_sequence.size(); ++i) {
                if (i == m_activeEntry) continue;
                const auto &e = m_sequence[i];
                if (e.sourceTrack != active.sourceTrack) continue;
                if (e.timelineStart >= active.timelineEnd - eps) {
                    if (prerollIdx < 0
                        || m_sequence[prerollIdx].timelineStart > e.timelineStart) {
                        prerollIdx = i;
                    }
                }
            }
            if (prerollIdx >= 0 && prerollIdx != m_prerolledEntryIdx) {
                const auto &nextE = m_sequence[prerollIdx];
                TrackDecoder *warm = acquireDecoderForClip(nextE);
                if (warm) {
                    qInfo() << "VideoPlayer: boundary preroll entry"
                            << prerollIdx << "file=" << nextE.filePath
                            << "tlPos=" << m_timelinePositionUs;
                    m_prerolledEntryIdx = prerollIdx;
                }
            }
        }
    }

    // Decide upfront whether the V1 frame for this tick will be composited
    // with V2+ overlays. When yes, presentDecodedFrame caches the V1 frame
    // into m_lastSourceFrame but skips displayFrame so the compositor step
    // below can blend the overlays before pushing the final image.
    const Timeline *previewTimeline = m_glPreview ? m_glPreview->timeline() : nullptr;
    const QVector<int> activeForComposite = findActiveEntriesAt(m_timelinePositionUs);
    const bool nestedSequenceActive =
        m_playbackSpeed >= 0.0
        && sequenceActive()
        && timelineHasActiveSequenceReference(previewTimeline, m_timelinePositionUs);
    const bool forceProjectOutputComposite =
        m_projectOutputSize.isValid() && !activeForComposite.isEmpty();
    const bool willComposite = m_playbackSpeed >= 0.0
                               && sequenceActive()
                               && !m_seekInProgress
                               && (hasOverlayActive(activeForComposite)
                                   || forceProjectOutputComposite
                                   || nestedSequenceActive);
    m_deferDisplayThisTick = willComposite;

    // When leaving the compositor path, restore the active entry's
    // OBS-style transform on the GL viewport. The compositor switches the
    // GL viewport to "composite-baked" mode (skipping its own scale apply)
    // so the canvas-final composite isn't transformed again — but a
    // non-composite tick ships the raw frame straight to GL, so the
    // viewport has to carry the entry's scale/dx/dy or the user's pan /
    // zoom snaps off the moment the overlay ends.
    if (m_lastTickWasComposite && !willComposite && m_glPreview
        && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
        const auto &v1e = m_sequence[m_activeEntry];
        // V3 sprint fix — push the EDIT TARGET's transform when one is set so
        // the preview drag handle reflects (and edits) the user-selected layer.
        // Falls back to m_activeEntry (V1) when no explicit edit target is held.
        const int displayIdx = (m_editTargetEntry >= 0
                                && m_editTargetEntry < m_sequence.size())
                               ? m_editTargetEntry
                               : m_activeEntry;
        const auto &targetE = m_sequence[displayIdx];
        m_glPreview->setVideoSourceTransform(targetE.videoScale, targetE.videoDx, targetE.videoDy);
        m_glPreview->setCompositeBakedMode(false);
        (void)v1e; // retained for adjacent compose path; intentionally unused here
    }
    m_lastTickWasComposite = willComposite;

    // Phase 1e Win #6 — parallel V1 + V2 decode.
    // Hoist V2+ overlay catch-up onto a worker thread so it overlaps with
    // V1's main-thread decodeNextFrame call below. acquireDecoderForClip
    // touches main-only state (m_trackDecoders, m_slotManager) so it must
    // run here, not inside the worker. The decode itself only mutates the
    // per-decoder TrackDecoder, which the threading contract on
    // runOverlayDecodeForDecoder already permits.
    static const bool prefetchV2Enabled =
        qEnvironmentVariableIntValue("VEDITOR_PREFETCH_V2_DISABLE") == 0;
    // MAJOR-1 mutual-exclusion gate: read VEDITOR_THREADED_POOL_DISABLE here too so
    // we can suppress this prefetch when the compositor branch's threadedPool
    // path will fan out the same V2 jobs via blockingMap. Without the gate,
    // 2+ overlays would be decoded twice per tick (worker prefetch + Pass 2
    // blockingMap), consuming an extra frame from each pool decoder.
    // V3 sprint — opt-out default-ON: V2+V3 等 2+ overlay の blockingMap
    // N-way fan-out を default で発火させる。VEDITOR_THREADED_POOL_DISABLE=1
    // で legacy serial path (非並列) に戻れる。
    // BREAKING: legacy `VEDITOR_THREADED_POOL=0` (旧 default-OFF と同等を
    // 望んでいた user) は今後無視される。代わりに VEDITOR_THREADED_POOL_DISABLE=1
    // を設定する必要あり。
    static const bool s_prefetchGateThreadedPoolEnabled =
        qEnvironmentVariableIntValue("VEDITOR_THREADED_POOL_DISABLE") == 0;
    struct V2PrefetchJob {
        TrackDecoder *d = nullptr;
        qint64 expectedFileLocalUs = 0;
        qint64 clipInUs = 0;
        qint64 clipOutUs = 0;
    };
    QVector<V2PrefetchJob> v2PrefetchJobs;
    QFuture<void> v2PrefetchFuture;
    if (prefetchV2Enabled && willComposite && !nestedSequenceActive
        && m_playbackSpeed >= 0.0) {
        const QVector<int> prefetchIdxs = findActiveEntriesAt(m_timelinePositionUs);
        for (int idx : prefetchIdxs) {
            if (idx < 0 || idx >= m_sequence.size())
                continue;
            if (idx == m_activeEntry)
                continue;  // V1 main path handles itself.
            const auto &e = m_sequence[idx];
            if (isAdjustmentEntry(previewTimeline, e))
                continue;
            if (e.sourceTrack <= 0)
                continue;  // Only V2+ overlays go through pool path.
            TrackDecoder *d = acquireDecoderForClip(e);
            if (!d)
                continue;
            V2PrefetchJob job;
            job.d = d;
            job.expectedFileLocalUs = entryLocalPositionUs(idx, m_timelinePositionUs);
            job.clipInUs  = static_cast<int64_t>(e.clipIn  * AV_TIME_BASE);
            job.clipOutUs = static_cast<int64_t>(e.clipOut * AV_TIME_BASE);
            v2PrefetchJobs.append(job);
        }
        // MAJOR-1: when 2+ V2 overlays are active AND threadedPool gate is on
        // (default), the compositor branch's threadedPool path (Pass 2 blockingMap)
        // will N-way fan-out the same decoders. Skip prefetch in that case to
        // avoid double-decoding. Win #6 prefetch is the "1 V2 overlay"
        // complement to that legacy parallel fan-out.
        const bool threadedPoolWillFire =
            s_prefetchGateThreadedPoolEnabled && v2PrefetchJobs.size() >= 2;
        if (!v2PrefetchJobs.isEmpty() && !threadedPoolWillFire) {
            // MAJOR-4: a single QtConcurrent worker iterates the jobs serially.
            // This is intentional — for 1 overlay it overlaps neatly with the
            // V1 main-thread decodeNextFrame below. The N-way fan-out for 2+
            // overlays is the threadedPool branch's blockingMap; the gate
            // above ensures these two paths never both fire on the same tick.
            v2PrefetchFuture = QtConcurrent::run([this, jobs = v2PrefetchJobs]() mutable {
                for (V2PrefetchJob &j : jobs) {
                    runOverlayDecodeForDecoder(j.d, j.expectedFileLocalUs,
                                               j.clipInUs, j.clipOutUs);
                }
            });
        }
    }

    const auto waitForV2Prefetch = [&](const char *site) {
        if (!v2PrefetchFuture.isStarted())
            return;
        QElapsedTimer waitTimer;
        if (stallTraceEnabled())
            waitTimer.start();
        v2PrefetchFuture.waitForFinished();
        if (stallTraceEnabled() && waitTimer.isValid()) {
            const qint64 elapsedMs = waitTimer.elapsed();
            if (elapsedMs >= kStallThresholdWaitMs) {
                qWarning().noquote()
                    << QStringLiteral("[stall>=%1ms] v2PrefetchFuture.waitForFinished %2ms site=%3")
                           .arg(kStallThresholdWaitMs)
                           .arg(elapsedMs)
                           .arg(QString::fromLatin1(site));
            }
        }
    };

    bool advanced = false;
    if (m_playbackSpeed >= 0.0) {
        // Drift correction runs BEFORE the display decode so the frame the
        // user sees this tick is always the freshest one, not the tail of a
        // skip-decode batch. Helper short-circuits on J-cut/L-cut.
        if (traceTick)
            sectionMark = tickTimer.nsecsElapsed();
        const int driftSkipped = correctVideoDriftAgainstAudioClock();
        if (traceTick) {
            m_tickTraceDriftNs += tickTimer.nsecsElapsed() - sectionMark;
            m_tickTraceSkipped += driftSkipped;
            sectionMark = tickTimer.nsecsElapsed();
        }
        QElapsedTimer decodeTimer;
        if (traceStall)
            decodeTimer.start();
        advanced = decodeNextFrame(true);
        if (traceTick)
            m_tickTraceDecodeNs += tickTimer.nsecsElapsed() - sectionMark;
        if (traceStall && decodeTimer.isValid()) {
            const qint64 elapsedMs = decodeTimer.elapsed();
            if (elapsedMs >= kStallThresholdWaitMs) {
                qWarning().noquote()
                    << QStringLiteral("[stall>=%1ms] decodeNextFrame(V1) %2ms tlUs=%3 entry=%4")
                           .arg(kStallThresholdWaitMs)
                           .arg(elapsedMs)
                           .arg(m_timelinePositionUs)
                           .arg(m_activeEntry);
            }
        }
    } else {
        const int64_t stepUs = qMax<int64_t>(1, m_frameDurationUs);
        const int64_t targetUs = qMax<int64_t>(0, m_currentPositionUs - stepUs);
        if (traceTick)
            sectionMark = tickTimer.nsecsElapsed();
        advanced = seekInternal(targetUs, true, true);
        if (traceTick)
            m_tickTraceDecodeNs += tickTimer.nsecsElapsed() - sectionMark;
    }
    // Always clear the flag so any subsequent displayFrame call (boundary
    // jump, seekInternal in reverse, etc.) is unguarded.
    m_deferDisplayThisTick = false;

    // Compositor step: paint V2+ overlays on top of the cached V1 frame and
    // push to the GLPreview. Only runs forward, in sequence mode, when not
    // in a preview seek. V1-only timelines short-circuit via hasOverlayActive
    // (overlays.isEmpty -> displayFrame is bypassed entirely on the legacy
    // path because m_deferDisplayThisTick was false above).
    bool servedByNestedSequenceSsot = false;
    if (advanced && willComposite && nestedSequenceActive && previewTimeline) {
        waitForV2Prefetch("nested-sequence-ssot");
        QSize renderSize = m_projectOutputSize.isValid()
            ? m_projectOutputSize
            : QSize(m_canvasWidth, m_canvasHeight);
        if (renderSize.isEmpty() && !m_lastV1RawFrame.isNull())
            renderSize = m_lastV1RawFrame.size();
        if (!renderSize.isEmpty()) {
            const QImage ssotFrame =
                tlrender::renderFrameAt(previewTimeline,
                                        m_timelinePositionUs,
                                        renderSize);
            if (!ssotFrame.isNull()) {
                if (m_glPreview)
                    m_glPreview->setCompositeBakedMode(true);
                cachePreviewComposite(ssotFrame);
                displayFrame(ssotFrame);
                servedByNestedSequenceSsot = true;
            }
        }
    }
    if (!servedByNestedSequenceSsot
        && advanced && willComposite && !m_lastV1RawFrame.isNull()) {
        // Phase 1e Win #6: wait for V2 prefetch to finish before the
        // compositor branch reads pool-decoder state.
        waitForV2Prefetch("compositor-entry");
        // ADAPTIVE-1: 合成キャッシュ参照。同一 (timelineRevision, playhead, size,
        // tier, useProxy) の最終合成が既にあれば再合成 (overlay decode + compose) を
        // スキップし、キャッシュ画像を 1 回だけ displayFrame する。prefetch-wait は
        // 上で既に通過済みなのでワーカー寿命は tick 内に収まる。再生中は playhead が
        // 毎 tick 進むため通常 miss (= decode/compose を従来どおり実行); ポーズ中の
        // 再 tick や同一位置反復でのみ hit する純最適化。displayFrame 発火回数は不変
        // (この tick も最大 1 frame)。テキストオーバーレイ時は合成解像度が等倍なので
        // size キーもそれに合わせる。
        bool servedFromCache = false;
        if (m_adaptivePreviewEnabled) {
            const int cacheProxy = m_textOverlays.isEmpty()
                ? (canvasProxyDivisor() * m_adaptiveCanvasDivisor) : 1;
            const QSize cacheCanvasBase = m_projectOutputSize.isValid()
                ? m_projectOutputSize
                : QSize(m_canvasWidth, m_canvasHeight);
            playback::CompositeFrameKey hitKey;
            hitKey.timelineRevision = m_timelineRevision;
            hitKey.timeMs       = m_timelinePositionUs / 1000;
            hitKey.width        = qMax(2, cacheCanvasBase.width()  / cacheProxy);
            hitKey.height       = qMax(2, cacheCanvasBase.height() / cacheProxy);
            hitKey.qualityTier  = m_adaptiveQualityTier;
            hitKey.useProxy     = (m_proxyDivisor > 1) || (playbackProxyDivisor() > 1);
            QImage cached;
            if (m_frameCache.tryGet(hitKey, cached)
                && !cached.isNull()
                && cached.size() == QSize(hitKey.width, hitKey.height)) {
                if (m_glPreview)
                    m_glPreview->setCompositeBakedMode(true);
                // Cache hits can re-serve ODT-baked frames; without a cached baked flag, paused same-playhead hits can double-apply ACES.
                displayFrame(cached);
                servedFromCache = true;
            }
        }
      if (!servedFromCache) {
        const QVector<int> activeIdxs = findActiveEntriesAt(m_timelinePositionUs);
        // GL viewport's setVideoSourceTransform is normally driven by the
        // active entry's videoScale/Dx/Dy in advanceToEntry. When we hand
        // GL a composed canvas-final image we have to (a) bake the
        // entry's transform into the canvas via a base layer, and (b)
        // reset GL viewport to identity so it doesn't apply the same
        // transform a second time on top of the already-baked composite.
        // Build the layer list. The legacy decoder owns whichever entry is
        // currently m_activeEntry (sorted V1-first, so usually V1 — but in
        // a V1-gap V2-active section it can be V2). All other active
        // entries come from the pool via harvestOverlayLayer.
        if (traceTick)
            sectionMark = tickTimer.nsecsElapsed();
        QVector<DecodedLayer> layers;
        layers.reserve(activeIdxs.size());
        QVector<ActivePreviewAdjustmentClip> activeAdjustments;
        activeAdjustments.reserve(activeIdxs.size());
        bool overlayPresent = false;

        // VEDITOR_THREADED_POOL_DISABLE (Phase 1e Sprint US-3, default-on per
        // V3 sprint flip): when 2+ non-V1 overlays are active this tick, fan
        // out their decode steps onto QtConcurrent worker threads so they run
        // in parallel instead of serializing on the main thread. Single-
        // overlay ticks short-circuit to Win #6's prefetch path or the legacy
        // serial path when prefetch is disabled.
        // V3 sprint — opt-out default-ON. See gate at top of handlePlaybackTick
        // for envvar rationale. Both statics read the same VEDITOR_THREADED_POOL_DISABLE
        // so the prefetch path and the compositor path agree on whether
        // threadedPool will fire.
        static const bool threadedPool =
            qEnvironmentVariableIntValue("VEDITOR_THREADED_POOL_DISABLE") == 0;
        int nonV1Count = 0;
        if (threadedPool) {
            for (int idx : activeIdxs) {
                if (idx >= 0 && idx < m_sequence.size() && idx != m_activeEntry
                    && !isAdjustmentEntry(previewTimeline, m_sequence.at(idx)))
                    ++nonV1Count;
            }
        }

        // Phase 1e occlusion culling: V1 is drawn last (on top) under the
        // descending V_max -> V1 sort (V1-wins stacking), so when V1 is fully
        // opaque AND fills the canvas (scale=1, dx=dy=0) every V2+ overlay
        // underneath it is invisible.
        // Skip the V2+ decode loop entirely in that case — it dominates the
        // multi-track stutter cost (decodeMs ~1685ms / 30 ticks).
        // Default ON because the cull is logically conservative: the math
        // matches what compose+blend would have produced (V2 invisible
        // pixels). VEDITOR_OCCLUSION_CULL=0 opts back into the legacy
        // always-decode path for diff-testing.
        static const bool occlusionCull = []() {
            const QByteArray v = qgetenv("VEDITOR_OCCLUSION_CULL");
            return v.isEmpty() ? true : (v != "0");
        }();
        bool v1FullyCoversCanvas = false;
        if (occlusionCull && m_activeEntry >= 0
            && m_activeEntry < m_sequence.size()) {
            const auto &v1e = m_sequence[m_activeEntry];
            v1FullyCoversCanvas = qFuzzyCompare(v1e.opacity, 1.0)
                && qFuzzyCompare(v1e.videoScale, 1.0)
                && qFuzzyIsNull(v1e.videoDx)
                && qFuzzyIsNull(v1e.videoDy)
                && v1e.sourceTrack == 0;
        }
        // One-shot diagnostic so user (and log readers) can confirm
        // whether the cull engaged for this project, and if not, why.
        // Reset on each project load via setSequence so multi-project
        // sessions emit one [cull] line per project.
        if (occlusionCull && !m_loggedCullState && m_activeEntry >= 0
            && m_activeEntry < m_sequence.size()) {
            const auto &v1e = m_sequence[m_activeEntry];
            qInfo() << "[cull] occlusion cull check: v1FullyCovers="
                    << v1FullyCoversCanvas
                    << "v1Track=" << v1e.sourceTrack
                    << "v1Opacity=" << v1e.opacity
                    << "v1Scale=" << v1e.videoScale
                    << "v1Dx=" << v1e.videoDx
                    << "v1Dy=" << v1e.videoDy
                    << "activeIdxs=" << activeIdxs.size();
            m_loggedCullState = true;
        }

        if (threadedPool && nonV1Count >= 2) {
            struct OverlayJob {
                TrackDecoder *d = nullptr;
                int seqIdx = -1;
                qint64 expectedFileLocalUs = 0;
                qint64 clipInUs = 0;
                qint64 clipOutUs = 0;
                bool decodedOk = false;
            };
            QVector<OverlayJob> overlayJobs;
            overlayJobs.reserve(nonV1Count);
            // Defense-in-depth: assert TrackDecoder uniqueness across this
            // tick's parallel jobs. acquireDecoderForClip is keyed on
            // (filePath, clipIn, sourceTrack, sourceClipIndex) and active
            // sequence indices are distinct, so this set should never see
            // a duplicate today — but if a future change to the slot pool
            // ever returned the same decoder for two acquire calls, the
            // worker fan-out would race on d->swsCtx. Skipping a duplicate
            // here turns that future bug into a deterministic missed
            // overlay instead of a heisenbug.
            QSet<TrackDecoder*> seenDecoders;
            // Pass 1 (main thread): acquire decoders, build V1 layer.
            for (int idx : activeIdxs) {
                if (idx < 0 || idx >= m_sequence.size())
                    continue;
                // v1FullyCoversCanvas occlusion-cull removed: export always
                // composites V2+ over V1 regardless of V1 opacity/scale, so
                // skipping overlay decode here created a preview≠export
                // divergence on opaque V1 + PiP/title overlay scenes.
                // (nonV1Count==0 means only V1 is active; idx!=m_activeEntry
                // is then always false so the old skip never fired then
                // anyway — no perf regression from removing it here.)
                const auto &e = m_sequence[idx];
                if (appendPreviewAdjustmentClip(
                        previewTimeline, e, m_timelinePositionUs, &activeAdjustments)) {
                    if (e.sourceTrack > 0)
                        overlayPresent = true;
                    continue;
                }
                if (isNullObjectEntry(e)) {
                    DecodedLayer layer;
                    layer.colorMeta = e.colorMeta;
                    applyLayerMotionOpacity(previewTimeline,
                                            e, m_timelinePositionUs, e.opacity, &layer);
                    populateLayerMetadata(previewTimeline,
                                          e, idx, &layer);
                    layers.append(layer);
                    if (e.sourceTrack > 0)
                        overlayPresent = true;
                    continue;
                }
                if (idx == m_activeEntry) {
                    if (m_lastV1RawFrame.isNull())
                        continue;
                    DecodedLayer layer;
                    const QSize sourceSize =
                        (m_codecCtx && m_codecCtx->width > 0 && m_codecCtx->height > 0)
                            ? QSize(m_codecCtx->width, m_codecCtx->height)
                            : m_lastV1RawFrame.size();
                    const double sourceSec =
                        static_cast<double>(
                            entryLocalPositionUs(idx, m_timelinePositionUs))
                        / AV_TIME_BASE;
                    layer.rgb = applyPreviewClipMask(
                        m_lastV1RawFrame, previewTimeline, e, sourceSec, sourceSize);
                    layer.isFresh = true;
                    layer.colorMeta = e.colorMeta;
                    applyLayerMotionOpacity(previewTimeline,
                                            e, m_timelinePositionUs, e.opacity, &layer);
                    populateLayerMetadata(previewTimeline,
                                          e, idx, &layer);
                    layers.append(layer);
                    if (e.sourceTrack > 0)
                        overlayPresent = true;
                    continue;
                }
                TrackDecoder *d = acquireDecoderForClip(e);
                if (!d)
                    continue;
                if (seenDecoders.contains(d))
                    continue; // duplicate decoder across overlays — skip to keep workers race-free
                seenDecoders.insert(d);
                OverlayJob job;
                job.d = d;
                job.seqIdx = idx;
                job.expectedFileLocalUs = entryLocalPositionUs(idx, m_timelinePositionUs);
                job.clipInUs = static_cast<int64_t>(e.clipIn * AV_TIME_BASE);
                job.clipOutUs = static_cast<int64_t>(e.clipOut * AV_TIME_BASE);
                overlayJobs.append(job);
            }
            // Pass 2 (workers): parallel decode. Each job touches only its
            // own TrackDecoder so there is no shared mutable state across
            // workers — see runOverlayDecodeForDecoder contract.
            if (!overlayJobs.isEmpty()) {
                QElapsedTimer mapTimer;
                if (stallTraceEnabled())
                    mapTimer.start();
                QtConcurrent::blockingMap(overlayJobs, [this](OverlayJob &job) {
                    job.decodedOk = runOverlayDecodeForDecoder(
                        job.d, job.expectedFileLocalUs,
                        job.clipInUs, job.clipOutUs);
                });
                if (stallTraceEnabled() && mapTimer.isValid()) {
                    const qint64 elapsedMs = mapTimer.elapsed();
                    if (elapsedMs >= kStallThresholdWaitMs) {
                        qWarning().noquote()
                            << QStringLiteral("[stall>=%1ms] blockingMap %2ms jobs=%3")
                                   .arg(kStallThresholdWaitMs)
                                   .arg(elapsedMs)
                                   .arg(overlayJobs.size());
                    }
                }
            }
            // Pass 3 (main thread): finalize layers + grace-pool fallback.
            for (const OverlayJob &job : overlayJobs) {
                if (job.seqIdx < 0 || job.seqIdx >= m_sequence.size())
                    continue;
                const auto &e = m_sequence[job.seqIdx];
                DecodedLayer layer;
                if (!finalizeOverlayFromDecoder(e, job.seqIdx, job.d,
                                                 job.decodedOk, &layer))
                    continue;
                layers.append(layer);
                if (e.sourceTrack > 0)
                    overlayPresent = true;
            }
        } else {
            for (int idx : activeIdxs) {
                if (idx < 0 || idx >= m_sequence.size())
                    continue;
                // v1FullyCoversCanvas occlusion-cull removed (same reasoning
                // as parallel path above): export always composites V2+, so
                // this skip was a preview≠export divergence on opaque V1 +
                // overlay scenes.
                const auto &e = m_sequence[idx];
                if (appendPreviewAdjustmentClip(
                        previewTimeline, e, m_timelinePositionUs, &activeAdjustments)) {
                    if (e.sourceTrack > 0)
                        overlayPresent = true;
                    continue;
                }
                DecodedLayer layer;
                if (isNullObjectEntry(e)) {
                    layer.isFresh = true;
                } else if (idx == m_activeEntry) {
                    if (m_lastV1RawFrame.isNull())
                        continue;
                    const QSize sourceSize =
                        (m_codecCtx && m_codecCtx->width > 0 && m_codecCtx->height > 0)
                            ? QSize(m_codecCtx->width, m_codecCtx->height)
                            : m_lastV1RawFrame.size();
                    const double sourceSec =
                        static_cast<double>(
                            entryLocalPositionUs(idx, m_timelinePositionUs))
                        / AV_TIME_BASE;
                    layer.rgb = applyPreviewClipMask(
                        m_lastV1RawFrame, previewTimeline, e, sourceSec, sourceSize);
                    layer.isFresh = true;
                } else {
                    if (!harvestOverlayLayer(e, idx, &layer))
                        continue;
                }
                layer.colorMeta = e.colorMeta;
                applyLayerMotionOpacity(previewTimeline,
                                        e, m_timelinePositionUs, e.opacity, &layer);
                populateLayerMetadata(previewTimeline,
                                      e, idx, &layer);
                layers.append(layer);
                if (e.sourceTrack > 0)
                    overlayPresent = true;
            }
        }
        // Export-matching paint order (V1-wins stacking, PlaybackTypes.h:34):
        // V1 (sourceTrack 0) paints LAST so it ends up ON TOP; higher tracks
        // (V2, V3, …) paint UNDERNEATH it — DESCENDING by sourceTrack so the
        // highest track is drawn first (backmost) and V1 last (frontmost).
        // This mirrors renderFrameAt (TimelineFrameRenderer.cpp matte-free
        // branch paints overlays first then V1 base last), so an opaque V1
        // occludes V2 in preview exactly as in export.
        // stable_sort: deterministic tie-break for same-sourceTrack clips
        // (preserves input order for equal keys); behaviour-identical to sort
        // for the common distinct-track case.
        // clipstack::layerPaintOrderLess is a named free function (declared
        // in VideoPlayer.h, namespace clipstack) so the S3-STACK predicate
        // sub-assertion in main.cpp exercises the SAME comparator — a
        // re-inversion breaks the selftest.
        std::stable_sort(layers.begin(), layers.end(), clipstack::layerPaintOrderLess);
        if (veditor::parentingEnabledFromEnv())
            resolveDecodedLayerParentSources(layers);
        for (DecodedLayer &L : layers) {
            {
                double insetFw = 1.0, insetFh = 1.0;
                if (L.fitContain && !L.fitCover) {
                    snsfit::containContentInset(L.rgb.size(), m_projectOutputSize, insetFw, insetFh);
                }
                if (m_glPreview)
                    m_glPreview->setVideoContentInset(insetFw, insetFh);
            }
            L.rgb = snsfit::maybeFit(L.rgb, L.fitContain, L.fitCover, m_projectOutputSize);
        }
        const bool layerStylePresent =
            std::any_of(layers.cbegin(), layers.cend(),
                        [](const DecodedLayer &L) {
                            return !L.layerStyle.isIdentity();
                        });
        if (overlayPresent || m_projectOutputSize.isValid() || layerStylePresent) {
            // Reuse a canvas-sized scratch buffer so we don't allocate
            // ~8MB (1080p ARGB) every tick. Re-allocates only when the
            // canvas size or format changes; otherwise we just refill it
            // with black. composeMultiTrackFrame still promotes through
            // convertToFormat, but starting with a matching format keeps
            // that to a no-op detach.
            // Phase 1e Win #9 — canvas proxy (rationale + invariants
            // in canvasProxyDivisor() at top of file). m_textOverlays
            // runtime gate keeps text overlays on the full-res path so
            // composeFrameWithOverlays paints text at correct coords.
            // ADAPTIVE-1: 既存の env ベース canvasProxyDivisor() に policy が決めた
            // 追加縮小係数 (m_adaptiveCanvasDivisor: 1/2/4) を乗算する。テキスト
            // オーバーレイがある場合は座標ずれ回避のため従来どおり等倍 (1) 固定。
            // m_adaptivePreviewEnabled=false 時は m_adaptiveCanvasDivisor=1 なので
            // 従来挙動とビット同一。
            const int canvasProxy = m_textOverlays.isEmpty()
                                    ? (canvasProxyDivisor() * m_adaptiveCanvasDivisor)
                                    : 1;
            const QSize outputCanvasBase = m_projectOutputSize.isValid()
                ? m_projectOutputSize
                : QSize(m_canvasWidth, m_canvasHeight);
            const int canvasW = qMax(2, outputCanvasBase.width()  / canvasProxy);
            const int canvasH = qMax(2, outputCanvasBase.height() / canvasProxy);
            if (m_canvasBase.size() != QSize(canvasW, canvasH)
                || m_canvasBase.format() != QImage::Format_ARGB32_Premultiplied) {
                m_canvasBase = QImage(canvasW, canvasH,
                                      QImage::Format_ARGB32_Premultiplied);
            }
            if (m_canvasBase.isNull()) {
                // Allocation failed (extreme — e.g. canvas not yet known).
                // Fall back to legacy V1-only display, including the
                // viewport restore so V1 keeps its own scale/dx/dy
                // instead of staying on the compositor's identity reset.
                if (m_glPreview && m_activeEntry >= 0
                    && m_activeEntry < m_sequence.size()) {
                    const auto &v1e = m_sequence[m_activeEntry];
                    // V3 sprint fix — push the EDIT TARGET's transform when set
                    // so V2/V3 drag-handles edit the right layer. Falls back to
                    // V1 (m_activeEntry) when no explicit edit target is held.
                    const int displayIdx = (m_editTargetEntry >= 0
                                            && m_editTargetEntry < m_sequence.size())
                                           ? m_editTargetEntry
                                           : m_activeEntry;
                    const auto &targetE = m_sequence[displayIdx];
                    m_glPreview->setVideoSourceTransform(
                        targetE.videoScale, targetE.videoDx, targetE.videoDy);
                    m_glPreview->setCompositeBakedMode(false);
                    (void)v1e;
                }
                if (traceTick)
                    m_tickTraceDecodeNs += tickTimer.nsecsElapsed() - sectionMark;
                QImage fallbackFrame = m_lastV1RawFrame;
                if (m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
                    const auto &entry = m_sequence[m_activeEntry];
                    const QSize sourceSize =
                        (m_codecCtx && m_codecCtx->width > 0 && m_codecCtx->height > 0)
                            ? QSize(m_codecCtx->width, m_codecCtx->height)
                            : m_lastV1RawFrame.size();
                    const double sourceSec =
                        static_cast<double>(
                            entryLocalPositionUs(m_activeEntry, m_timelinePositionUs))
                        / AV_TIME_BASE;
                    fallbackFrame = applyPreviewClipMask(
                        m_lastV1RawFrame, previewTimeline, entry, sourceSec, sourceSize);
                }
                displayFrame(fallbackFrame);
            } else {
                if (traceTick) {
                    m_tickTraceDecodeNs += tickTimer.nsecsElapsed() - sectionMark;
                    sectionMark = tickTimer.nsecsElapsed();
                }
                m_canvasBase.fill(Qt::black);
                // STAGE3-GPU: 既定 OFF。VEDITOR_GPU_COMPOSITE="1" の時だけ多トラック
                // (matte-free) を GpuLayerCompositor で GPU 合成する。成功時はこの分岐
                // 1 本で「合成 → baked mode → cache → displayFrame (1 回)」を完結し、
                // 下の CPU 経路 (in-place / legacy) はスキップする。GPU が空を返す
                // (GL 失敗) / flag OFF / 単一トラックなら gpuComposed は null となり、
                // 従来 CPU 経路へそのままフォールバックする (displayFrame は依然 1 回)。
                // m_canvasBase.size() を canvas として渡すので CPU と同じ適応縮小解像度。
                const bool adjustmentClipPresent = !activeAdjustments.isEmpty();
                const bool trackMattePresent = hasDecodedTrackMatte(layers);
                const bool canTryGpuComposite =
                    !adjustmentClipPresent || trackMattePresent;
                const QImage gpuComposed = canTryGpuComposite
                    ? tryGpuComposeLayers(layers, m_canvasBase.size())
                    : QImage();
                if (!gpuComposed.isNull()) {
                    const QImage displayedComposite =
                        adjustmentClipPresent && trackMattePresent
                            ? applyPreviewAdjustmentClipsAfterMatte(
                                  gpuComposed, activeAdjustments)
                            : gpuComposed;
                    if (traceTick)
                        m_tickTraceComposeNs += tickTimer.nsecsElapsed() - sectionMark;
                    // CPU 経路と同じ baked-mode: paintGL が m_videoSourceScale を
                    // 二重適用しないよう viewport を identity 扱いにする。
                    if (m_glPreview)
                        m_glPreview->setCompositeBakedMode(true);
                    cachePreviewComposite(displayedComposite);  // CPU 経路と同じキーで put
                    displayFrame(displayedComposite);           // 1 tick = 最大 1 displayFrame
                } else {
                // Phase 1e Win #7 — in-place compose. Default ON; opt out
                // via VEDITOR_INPLACE_COMPOSE_DISABLE=1 to fall back to the
                // legacy composeMultiTrackFrame path that allocates a
                // separate composed buffer through convertToFormat shallow
                // share + QPainter detach (~8MB alloc + memcpy / 1080p
                // tick).
                static const bool inplaceComposeEnabled =
                    qEnvironmentVariableIntValue("VEDITOR_INPLACE_COMPOSE_DISABLE") == 0;
                if (adjustmentClipPresent) {
                    composePreviewFrameWithAdjustmentClips(
                        m_canvasBase, layers, activeAdjustments);
                    if (traceTick)
                        m_tickTraceComposeNs += tickTimer.nsecsElapsed() - sectionMark;
                    if (m_glPreview)
                        m_glPreview->setCompositeBakedMode(true);
                    cachePreviewComposite(m_canvasBase);
                    displayFrame(m_canvasBase);
                } else if (inplaceComposeEnabled) {
                    composeMultiTrackFrameInto(m_canvasBase, layers);
                    if (traceTick)
                        m_tickTraceComposeNs += tickTimer.nsecsElapsed() - sectionMark;
                    // Switch the GL viewport into baked mode so paintGL skips
                    // applying m_videoSourceScale on top of the canvas (which
                    // already has every layer's transform baked in). This
                    // intentionally does NOT touch m_videoSourceScale itself
                    // — that field is the active drag state for the OBS-style
                    // resize handles, and clobbering it 30 times a second
                    // (the prior behavior) made resize/move drags impossible
                    // during multi-track playback.
                    if (m_glPreview)
                        m_glPreview->setCompositeBakedMode(true);
                    // ADAPTIVE-1: 合成結果を (timelineRevision, playhead, size, tier)
                    // でキャッシュ。再生は毎 tick playhead が進むのでキーは毎回変わり、
                    // ライブ再生で hit することはない (= decode をスキップしないので
                    // A/V sync を壊さない)。同一プレイヘッドの再描画 (リサイズ /
                    // scrub 戻り) でのみ将来 hit する純キャッシュ。displayFrame の
                    // 発火回数は不変 (1 tick = 最大 1 frame)。
                    cachePreviewComposite(m_canvasBase);
                    displayFrame(m_canvasBase);
                } else {
                    // Legacy path (VEDITOR_INPLACE_COMPOSE_DISABLE=1): allocates
                    // a separate `composed` via composeMultiTrackFrame's
                    // convertToFormat shallow share + QPainter detach. Same
                    // baked-mode rationale as the in-place branch above —
                    // setCompositeBakedMode(true) keeps paintGL from applying
                    // m_videoSourceScale on top of the already-baked canvas.
                    const QImage composed = composeMultiTrackFrame(m_canvasBase, layers);
                    if (traceTick)
                        m_tickTraceComposeNs += tickTimer.nsecsElapsed() - sectionMark;
                    if (m_glPreview)
                        m_glPreview->setCompositeBakedMode(true);
                    cachePreviewComposite(composed); // ADAPTIVE-1: 上の in-place 分岐と同旨
                    displayFrame(composed);
                }
                } // STAGE3-GPU: close `else` (CPU フォールバック経路)
            }
        } else {
            // Every overlay bailed (decoder open failed, slot exhausted).
            // Fall back to the V1-only display, and restore V1's transform
            // on the GL viewport — composite branch puts it in baked mode
            // to keep the transform from being applied twice, but we're
            // back on the raw V1 frame now so V1 has to carry its own
            // transform again or it snaps to identity.
            if (m_glPreview && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
                const auto &v1e = m_sequence[m_activeEntry];
                // V3 sprint fix — push the EDIT TARGET's transform when set
                // so V2/V3 drag-handles edit the right layer. Falls back to
                // V1 (m_activeEntry) when no explicit edit target is held.
                const int displayIdx = (m_editTargetEntry >= 0
                                        && m_editTargetEntry < m_sequence.size())
                                       ? m_editTargetEntry
                                       : m_activeEntry;
                const auto &targetE = m_sequence[displayIdx];
                m_glPreview->setVideoSourceTransform(targetE.videoScale, targetE.videoDx, targetE.videoDy);
                m_glPreview->setCompositeBakedMode(false);
                (void)v1e;
            }
            if (traceTick)
                m_tickTraceDecodeNs += tickTimer.nsecsElapsed() - sectionMark;
            QImage fallbackFrame = m_lastV1RawFrame;
            if (m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
                const auto &entry = m_sequence[m_activeEntry];
                const QSize sourceSize =
                    (m_codecCtx && m_codecCtx->width > 0 && m_codecCtx->height > 0)
                        ? QSize(m_codecCtx->width, m_codecCtx->height)
                        : m_lastV1RawFrame.size();
                const double sourceSec =
                    static_cast<double>(
                        entryLocalPositionUs(m_activeEntry, m_timelinePositionUs))
                    / AV_TIME_BASE;
                fallbackFrame = applyPreviewClipMask(
                    m_lastV1RawFrame, previewTimeline, entry, sourceSec, sourceSize);
            }
            displayFrame(fallbackFrame);
        }
      } // ADAPTIVE-1: close `if (!servedFromCache)`
    }

    // A/V sync: video owns m_currentPositionUs via the FFmpeg decoder;
    // AudioMixer publishes its own master clock. Coupling is audio-as-master
    // via correctVideoDriftAgainstAudioClock + audio-paced scheduleNextFrame.
    // The mixer's clock is timeline-unified, so J-cut/L-cut paths no longer
    // need to fall back to independent clocks.

    // Sequence mode: detect when we've crossed the active entry's outPoint or
    // run off the end of the file, and switch to the next entry.
    if (sequenceActive() && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()
        && m_playbackSpeed >= 0.0) {
        const auto &active = m_sequence[m_activeEntry];
        const int64_t entryEndLocalUs = static_cast<int64_t>(active.clipOut * AV_TIME_BASE);
        const bool reachedEntryEnd = (m_currentPositionUs >= entryEndLocalUs);
        // NB: previously also triggered on `decodeStopped = !advanced`, but
        // that fires for any transient decode stall (cold open of the new
        // primary after overlay rotation, V2 prefetch worker starving the
        // main decoder, single dropped frame on a heavy 1080p source). The
        // false positives caused (a) cross-track sideways jumps when the
        // sequence was sorted by (timelineStart, trackIdx) and (b) immediate
        // end-of-sequence after rotation while the new primary's decoder
        // was still warming up. Limit to the genuine end-of-clip signal.

        if (reachedEntryEnd) {
            // PiP regression fix: previous code used `nextIdx = m_activeEntry + 1`,
            // which conflated flat sequence index with "next clip on this track".
            // With ParallelTrack mode (V1-V4 stacked at timelineStart=0), +1
            // jumps sideways into a different track's entry — the user saw
            // V1's transient stall load Xマッチ.mp4 (V4 raw 1080p) instead.
            //
            // Two-stage candidate selection:
            //   (1) Prefer the next clip on the SAME sourceTrack at/past the
            //       active entry's timelineEnd (sequential play within track).
            //   (2) If none, rotate primary to the OTHER sourceTrack overlay
            //       that is still alive at the current timeline position with
            //       the largest remaining duration. This keeps PiP playback
            //       going when the V1 primary clip is shorter than overlays
            //       (V2/V3/V4) — without this, V1 ending stopped everything.
            const int activeTrack = active.sourceTrack;
            const double nextTlSec = active.timelineEnd;
            int nextIdx = -1;
            for (int i = 0; i < m_sequence.size(); ++i) {
                if (i == m_activeEntry) continue;
                if (m_sequence[i].sourceTrack != activeTrack) continue;
                if (m_sequence[i].timelineStart >= nextTlSec - 1e-6) {
                    nextIdx = i;
                    break;
                }
            }
            if (nextIdx < 0) {
                const double tlSec = m_timelinePositionUs
                                   / static_cast<double>(AV_TIME_BASE);
                // Prefer proxy over raw when rotating primary: raw 1080p+
                // sources block the main decoder thread and cause every
                // overlay's compose tick to stutter (user-reported "V4
                // 再生されて V2 がカクつく"). Picking a proxy primary keeps
                // the legacy decoder light. Among same-class candidates,
                // pick the longest-remaining timelineEnd.
                // Proxy detection uses ProxyManager::proxyDir() so custom
                // storage (`D:/proxies/...`) is honored — substring match
                // on `/.veditor/proxies/` would silently false-negative
                // there and degrade to longest-remaining-only (regressing
                // V4 raw → primary). Both forward and back slash variants
                // are checked to defend against QDir::toNativeSeparators
                // ever leaking native paths into m_sequence.
                const QString proxyRoot = ProxyManager::proxyDir();
                const auto isProxyPath = [&proxyRoot](const QString &p) {
                    return p.startsWith(proxyRoot + QChar('/'))
                        || p.startsWith(proxyRoot + QChar('\\'));
                };
                double bestEnd = active.timelineEnd;
                bool bestIsProxy = false;
                bool bestSet = false;
                for (int i = 0; i < m_sequence.size(); ++i) {
                    if (i == m_activeEntry) continue;
                    const auto &e = m_sequence[i];
                    // Fix L (c): boundary inclusive on the upper bound. The
                    // earlier `e.timelineEnd > tlSec + 1e-6` check disqualified
                    // entries whose timelineEnd exactly equaled tlSec (= the
                    // active entry's timelineEnd at reachedEntryEnd time),
                    // which excluded the legitimate next-clip whose own
                    // timelineStart equals tlSec. Loosening to `>=` keeps the
                    // same lower-bound semantics while covering the boundary
                    // case the empirical 110238 log exposed (entry 1 テスト
                    // [0,7540] vs tlSec=7540 → entry 3 参加型 [7540,14372]).
                    if (!(e.timelineStart <= tlSec + 1e-6
                          && e.timelineEnd >= tlSec + 1e-6))
                        continue;
                    const bool isProxy = isProxyPath(e.filePath);
                    bool prefer = false;
                    if (!bestSet) {
                        prefer = true;
                    } else if (isProxy && !bestIsProxy) {
                        prefer = true;            // proxy class always wins
                    } else if (isProxy == bestIsProxy
                               && e.timelineEnd > bestEnd) {
                        prefer = true;            // tie → longer remaining
                    }
                    if (prefer) {
                        nextIdx = i;
                        bestEnd = e.timelineEnd;
                        bestIsProxy = isProxy;
                        bestSet = true;
                    }
                }
            }
            // self-rotation guard: refuse to re-load the same active entry,
            // which would otherwise re-trigger AudioMixer::seekTo + sink
            // stop/start every tick during the new primary's cold-open
            // window and produce audible clicks.
            if (nextIdx == m_activeEntry)
                nextIdx = -1;
            if (nextIdx >= 0 && nextIdx < m_sequence.size()) {
                const auto &nextE = m_sequence[nextIdx];
                const bool isOverlayRotation =
                    (nextE.sourceTrack != active.sourceTrack);
                // Boundary advances can promote/reseek pool decoders. Fence
                // the tick-local V2 worker before touching that pool so the
                // decoder wrapper and FFmpeg contexts cannot be moved/deleted
                // while the worker still holds them.
                waitForV2Prefetch("boundary-advance");
                QElapsedTimer advTimer;
                if (stallTraceEnabled())
                    advTimer.start();
                // Overlay rotation: keep the current timeline position and
                // let seekToTimelineUs pick the new primary — findActiveEntryAt
                // skips the just-ended active entry and picks the next alive
                // overlay. Sequential same-track advance goes through
                // advanceToEntry which seeks to the next clip's clipIn.
                const bool advanced = isOverlayRotation
                    ? seekToTimelineUs(m_timelinePositionUs, /*precise=*/true)
                    : advanceToEntry(nextIdx);
                if (stallTraceEnabled() && advTimer.isValid()) {
                    const qint64 elapsedMs = advTimer.elapsed();
                    if (elapsedMs >= kStallThresholdWaitMs) {
                        qWarning().noquote()
                            << QStringLiteral("[stall>=%1ms] advance(%2) %3ms next=%4")
                                   .arg(kStallThresholdWaitMs)
                                   .arg(isOverlayRotation ? "rotate" : "seq")
                                   .arg(elapsedMs)
                                   .arg(nextIdx);
                    }
                }
                if (advanced) {
                    recordAdaptiveRenderTime();
                    updatePositionUi();
                    if (traceTick)
                        recordTickTrace(tickTimer.nsecsElapsed());
                    reportStallIfSlow();
                    scheduleNextFrame();
                    return;
                }
            } else {
                // End of sequence.
                waitForV2Prefetch("boundary-end");
                m_timelinePositionUs = m_sequenceDurationUs;
                m_currentPositionUs = entryEndLocalUs;
                updatePositionUi();
                if (traceTick)
                    recordTickTrace(tickTimer.nsecsElapsed());
                reportStallIfSlow();
                pause();
                if (m_mixer) m_mixer->stop();
                return;
            }
        }
    }

    // ADAPTIVE-1: 今 tick の合成ウォール時間 (decode+compose+display) を記録し、
    // 次 tick の updateAdaptiveQuality() が品質ラダー判定に使う。早期 return する
    // 経路 (end-of-sequence で pause→return する分岐) は再生停止なので測定不要
    // (停止後は updateAdaptiveQuality が呼ばれず、stop()/play() が状態を整える)。
    recordAdaptiveRenderTime();

    // Phase 1e Win #6 — safety-net: every exit path of handlePlaybackTick MUST
    // pass through this wait so worker lifetime is bounded by the tick.
    // Compositor branch already waits internally at its entry; this catches
    // paths where willComposite=true but compositor was skipped (advanced=false
    // or lastV1RawFrame became null mid-tick), AND any future early-return
    // inserted into handlePlaybackTick. Do NOT add a return between the
    // prefetch dispatch and this wait without porting the wait to the new
    // return site.
    waitForV2Prefetch("tick-exit");
    Q_ASSERT(!v2PrefetchFuture.isStarted() || v2PrefetchFuture.isFinished());

    if (!advanced) {
        // Iteration 11 — walking playhead. User-stated invariant
        // 「ユーザーが止めない限りシークバー及びヘッドが進み続ける」: only
        // explicit user pause should stop forward motion. In sequence
        // mode the decoder can transiently fail to deliver a frame
        // (cold-open after hot-swap miss, slot-manager eviction race,
        // proxy retry, etc.); the previous auto-pause here turned every
        // such transient stall into a hard stop. Walking the playhead by
        // one frame interval keeps the seekbar visually advancing while
        // the decoder catches up — the next tick either decodes
        // successfully (presentDecodedFrame overwrites m_timelinePositionUs
        // with the actual decoded PTS) or walks again. Single-file (legacy)
        // mode keeps the original auto-pause because a decode failure
        // there is a real end-of-file event, not a transient.
        static const bool kWalkingPlayheadDisabled =
            qEnvironmentVariableIntValue("VEDITOR_WALKING_PLAYHEAD_DISABLE") != 0;
        // NIT-1 (architect): forward-only. Reverse playback's transient
        // stall would otherwise snap the playhead toward 0 instead of
        // continuing backward. Reverse falls through to the legacy pause
        // (rare path).
        if (!kWalkingPlayheadDisabled && sequenceActive() && m_playbackSpeed >= 0.0) {
            const double speed = m_playbackSpeed;
            const int64_t frameStep = m_frameDurationUs > 0
                ? m_frameDurationUs : (AV_TIME_BASE / 30);
            const int64_t deltaUs = static_cast<int64_t>(
                static_cast<double>(frameStep) * speed);
            const int64_t maxUs = m_sequenceDurationUs > 0
                ? m_sequenceDurationUs : (m_timelinePositionUs + deltaUs);
            // NIT-2 (architect, CRITICAL): updatePositionUi recomputes
            // m_timelinePositionUs = fileLocalToTimelineUs(m_activeEntry,
            // m_currentPositionUs), so a timeline-only walk is silently
            // overwritten and the seekbar never advances. Walk
            // m_currentPositionUs by the same delta — overshoot past the
            // active entry's clipOut is fine: the next tick's
            // reachedEntryEnd path will retry advanceToEntry (which may
            // hot-swap successfully now that more time has passed).
            // presentDecodedFrame overwrites m_currentPositionUs with the
            // real PTS once the decoder catches up, healing any drift.
            m_timelinePositionUs = qBound<int64_t>(
                0, m_timelinePositionUs + deltaUs, maxUs);
            m_currentPositionUs = m_currentPositionUs + deltaUs;
            updatePositionUi();
            // Throttled diagnostic — a sustained stall would otherwise
            // flood the log at 60 Hz.
            static QElapsedTimer walkLogThrottle;
            if (!walkLogThrottle.isValid() || walkLogThrottle.elapsed() >= 1000) {
                qInfo() << "VideoPlayer: walking playhead (decoder behind) tlPos="
                        << m_timelinePositionUs << "deltaUs=" << deltaUs;
                walkLogThrottle.start();
            }
            if (traceTick) recordTickTrace(tickTimer.nsecsElapsed());
            reportStallIfSlow();
            scheduleNextFrame();
            return;
        }
        if (m_playbackSpeed >= 0.0 && m_durationUs > 0) {
            m_currentPositionUs = m_durationUs;
            updatePositionUi();
        }
        if (traceTick)
            recordTickTrace(tickTimer.nsecsElapsed());
        reportStallIfSlow();
        pause();
        if (m_mixer)
            m_mixer->stop();
        return;
    }

    // Audio is paced internally by AudioMixer's master clock — no per-tick
    // reposition is needed during natural playback. Cross-entry boundaries
    // and seeks are handled separately via m_mixer->seekTo from the
    // seekToTimelineUs / advanceToEntry call sites.

    if (traceTick)
        recordTickTrace(tickTimer.nsecsElapsed());
    reportStallIfSlow();
    scheduleNextFrame();
}

// ---- Per-clip decoder pool ---------------------------------------------------
//
// The active sequence entry (m_activeEntry) is owned by the legacy decoder
// (m_formatCtx/m_codecCtx/...). Every other active entry — V1 included when
// the legacy decoder is sitting on V2 because V2 starts earlier on the
// timeline — comes through this pool. The pool keeps overlay clips' own
// AVFormatContext / AVCodecContext open across short cuts without re-opening
// the file every time the playhead crosses a clip boundary, while
// DecoderSlotManager bounds the number of concurrent HW-decoder sessions on
// the GPU.
//
// Lifecycle invariant (US-2 contract):
//   - Acquire returns a TrackDecoder for the requested entry; refresh path
//     for already-open clips, eviction-with-grace path otherwise.
//   - Eviction does NOT immediately tear down the displaced decoder; it
//     drops to m_evictionGracePool with graceTtlTicks=4 so a stale
//     reference held inside decodeNextFrame / presentDecodedFrame survives
//     long enough to finish its current frame.
//   - tickEvictionGracePool runs from a deferred / next-tick context only
//     (US-3 wiring) — never from inside the decode loop.

VideoPlayer::TrackDecoder *VideoPlayer::acquireDecoderForClip(const PlaybackEntry &entry)
{
    // The active sequence entry (m_activeEntry) is owned by the legacy
    // decoder; the layer-construction loop in handlePlaybackTick gates
    // that case before calling here. Every other active entry — including
    // V1 entries that aren't the active one (Scenario: V2 starts earlier
    // than V1 so m_sequence sorts V2 first; m_activeEntry tracks V2 even
    // while V1 overlaps) — goes through the pool so V1 can still paint
    // last in V1-wins ordering.
    if (isNullObjectEntry(entry))
        return nullptr;

    const TrackKey key{entry.filePath, qRound64(entry.clipIn * 1000.0),
                       entry.sourceTrack, entry.sourceClipIndex};

    // DecoderSlotManager indexes by a single int. Hash the TrackKey down to
    // a 32-bit slot id — qHash gives us a path/clipIn-stable identifier
    // that survives clip reorders (unlike the previous (track<<16 | index)
    // packing which shifted with sourceClipIndex). Hash collisions are
    // possible but vanishingly rare for the small handful of overlay clips
    // we run concurrently; the worst case is one decoder being evicted a
    // little earlier than ideal.
    const int slotClipId = static_cast<int>(qHash(key));

    DecoderSlotManager::SlotRequest req;
    req.clipId = slotClipId;
    // Slot manager treats trackIdx==0 as "the protected V1 slot — never
    // evict". Pool entries are by construction NOT m_activeEntry (the
    // layer-construction loop gates that case), so a V1 entry that lands
    // in the pool is just another overlay layer, not the main timeline
    // video. Map V1's 0 → 1 so the protection logic doesn't pin a pool
    // slot indefinitely; the legacy decoder still owns the primary V1
    // instance when V1 IS m_activeEntry.
    req.trackIdx = (entry.sourceTrack == 0) ? 1 : entry.sourceTrack;
    req.clipStartSec = entry.timelineStart;

    int evictedClipId = -1;
    if (!m_slotManager.requestSlot(req, &evictedClipId)) {
        // Every slot is held by V1 clips — caller must skip rendering this
        // V2+ layer this tick. Should be rare since V1 is single-decoder.
        return nullptr;
    }

    if (evictedClipId != -1) {
        // Look the evicted slot up in the reverse map instead of
        // re-hashing every TrackKey and comparing the hash — re-hash
        // compare evicts the wrong decoder on any hash collision (same
        // int packing for two different keys).
        auto kit = m_slotIdToKey.find(evictedClipId);
        if (kit != m_slotIdToKey.end()) {
            const TrackKey evictedKey = kit.value();
            m_slotIdToKey.erase(kit);
            auto dit = m_trackDecoders.find(evictedKey);
            if (dit != m_trackDecoders.end()) {
                TrackDecoder *evicted = dit.value();
                m_trackDecoders.erase(dit);
                if (evicted) {
                    evicted->graceTtlTicks = 4;
                    m_evictionGracePool.append(evicted);
                }
            }
        }
    }

    // Refresh path: slot manager refreshed an existing entry. Return the
    // already-open decoder unchanged.
    if (auto it = m_trackDecoders.find(key); it != m_trackDecoders.end())
        return it.value();

    // No existing decoder for this (file, clipIn, track) — open one.
    TrackDecoder *fresh = openTrackDecoder(entry);
    if (!fresh) {
        // Open failed: free the slot we just claimed so a future retry can
        // succeed. Pool stays consistent.
        m_slotManager.releaseSlot(slotClipId);
        return nullptr;
    }
    m_trackDecoders.insert(key, fresh);
    m_slotIdToKey.insert(slotClipId, key);
    return fresh;
}

void VideoPlayer::releaseDecoderForClip(const TrackKey &key)
{
    auto it = m_trackDecoders.find(key);
    if (it == m_trackDecoders.end())
        return;
    TrackDecoder *d = it.value();
    m_trackDecoders.erase(it);

    const int slotClipId = static_cast<int>(qHash(key));
    m_slotManager.releaseSlot(slotClipId);
    m_slotIdToKey.remove(slotClipId);

    // Synchronous teardown is OK here: callers reach this path from
    // setSequence reconciliation or explicit clip removal — never from
    // inside decodeNextFrame / presentDecodedFrame.
    if (d) {
        closeTrackDecoder(d);
        delete d;
    }
}

void VideoPlayer::clearAllPoolDecoders()
{
    for (auto it = m_trackDecoders.begin(); it != m_trackDecoders.end(); ++it) {
        TrackDecoder *d = it.value();
        if (!d)
            continue;
        closeTrackDecoder(d);
        delete d;
    }
    m_trackDecoders.clear();
    m_slotIdToKey.clear();

    for (TrackDecoder *d : m_evictionGracePool) {
        if (!d)
            continue;
        closeTrackDecoder(d);
        delete d;
    }
    m_evictionGracePool.clear();

    m_slotManager.clear();
}

void VideoPlayer::tickEvictionGracePool()
{
    // Walk in reverse so removal indices stay valid.
    for (int i = m_evictionGracePool.size() - 1; i >= 0; --i) {
        TrackDecoder *d = m_evictionGracePool[i];
        if (!d) {
            m_evictionGracePool.removeAt(i);
            continue;
        }
        if (d->graceTtlTicks > 0)
            --d->graceTtlTicks;
        if (d->graceTtlTicks <= 0) {
            closeTrackDecoder(d);
            delete d;
            m_evictionGracePool.removeAt(i);
        }
    }
}

VideoPlayer::TrackDecoder *VideoPlayer::openTrackDecoder(const PlaybackEntry &entry)
{
    // Phase 1e Win #10 stall trace — wall-time around the cold open
    // (avformat_open_input + avformat_find_stream_info on the source
    // file). On a 4h sparse-keyframe H.264 source this can take 1-3
    // seconds and runs synchronously on the main thread, fitting the
    // user-reported "数秒" intermittent stall pattern at clip
    // boundaries. Default off.
    QElapsedTimer stallTimer;
    if (stallTraceEnabled())
        stallTimer.start();
    auto *d = new TrackDecoder();
    d->wantRgba64Overlay =
        hdroverlay::wantRgba64(hdroverlay::enabledFromEnv(), entry.colorMeta.isHdr);
    d->sourceClipIndex = entry.sourceClipIndex;
    d->sourceTrack = entry.sourceTrack;
    d->filePath = entry.filePath;
    d->clipIn = entry.clipIn;

    const QByteArray pathUtf8 = entry.filePath.toUtf8();
    if (avformat_open_input(&d->formatCtx, pathUtf8.constData(), nullptr, nullptr) != 0) {
        qWarning() << "openTrackDecoder: avformat_open_input failed for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }
    if (avformat_find_stream_info(d->formatCtx, nullptr) < 0) {
        qWarning() << "openTrackDecoder: avformat_find_stream_info failed for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    d->videoStreamIndex = av_find_best_stream(d->formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d->videoStreamIndex < 0) {
        qWarning() << "openTrackDecoder: no video stream in" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    auto *codecpar = d->formatCtx->streams[d->videoStreamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        qWarning() << "openTrackDecoder: unsupported codec for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    d->codecCtx = avcodec_alloc_context3(codec);
    if (!d->codecCtx || avcodec_parameters_to_context(d->codecCtx, codecpar) < 0) {
        qWarning() << "openTrackDecoder: codec context alloc failed for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    // Lazily create the shared HW device context the first time a pool
    // decoder actually needs HW. Subsequent decoders just bump its refcount.
    // V1's m_hwDeviceCtx is intentionally separate so any V2 HW failure
    // never destabilises V1's decoder.
    d->hwPixFmt = AV_PIX_FMT_NONE;
    if (!m_sharedPoolHwDeviceCtx) {
        AVBufferRef *fresh = nullptr;
        if (av_hwdevice_ctx_create(&fresh, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
            m_sharedPoolHwDeviceCtx = fresh;
        }
    }
    if (m_sharedPoolHwDeviceCtx) {
        // Find the codec-specific HW pixel format for D3D11VA.
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                d->hwPixFmt = cfg->pix_fmt;
                break;
            }
        }
        if (d->hwPixFmt != AV_PIX_FMT_NONE) {
            // av_buffer_ref can fail (allocator OOM). Fall back to SW decode
            // instead of trusting a null hw_device_ctx — the codec would
            // otherwise crash inside avcodec_open2 / avcodec_send_packet.
            AVBufferRef *hwRef = av_buffer_ref(m_sharedPoolHwDeviceCtx);
            if (!hwRef) {
                qWarning() << "openTrackDecoder: av_buffer_ref failed, falling back to SW";
                d->hwPixFmt = AV_PIX_FMT_NONE;
            } else {
                d->codecCtx->opaque = d;
                d->codecCtx->get_format = &VideoPlayer::poolGetHwFormatCallback;
                d->codecCtx->hw_device_ctx = hwRef;
            }
        }
    }
    // Software fallback path: leave hwPixFmt = AV_PIX_FMT_NONE, no get_format
    // hook, no hw_device_ctx. Decoder will emit SW frames directly.

    if (avcodec_open2(d->codecCtx, codec, nullptr) < 0) {
        qWarning() << "openTrackDecoder: avcodec_open2 failed for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->swFrame = av_frame_alloc();
    if (!d->packet || !d->frame || !d->swFrame) {
        qWarning() << "openTrackDecoder: frame/packet alloc failed";
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    // Duration / frame timing. Mirror loadFile's resolution order so the
    // numbers downstream code expects line up.
    if (d->formatCtx->duration > 0) {
        d->durationUs = d->formatCtx->duration;
    } else {
        AVStream *stream = d->formatCtx->streams[d->videoStreamIndex];
        if (stream->duration > 0)
            d->durationUs = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
    }

    AVStream *vs = d->formatCtx->streams[d->videoStreamIndex];
    AVRational fr = vs->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0)
        fr = vs->r_frame_rate;
    if (fr.num > 0 && fr.den > 0)
        d->frameDurationUs = av_rescale_q(1, AVRational{fr.den, fr.num}, AV_TIME_BASE_Q);
    if (d->frameDurationUs <= 0)
        d->frameDurationUs = AV_TIME_BASE / 30;

    if (vs->codecpar->width > 0 && vs->codecpar->height > 0) {
        AVRational sar = vs->sample_aspect_ratio;
        double sarVal = (sar.num > 0 && sar.den > 0)
            ? static_cast<double>(sar.num) / sar.den : 1.0;
        d->displayAspect = (static_cast<double>(vs->codecpar->width) * sarVal)
            / static_cast<double>(vs->codecpar->height);
    }

    // Seek to the entry's clipIn so the first decoded frame is meaningful.
    // Pool decoders are demand-decoded by US-3 wiring, but seeding the seek
    // here means the first decoded packet won't be from t=0 of the source
    // file when clipIn > 0.
    const int64_t clipInUs = static_cast<int64_t>(entry.clipIn * AV_TIME_BASE);
    if (clipInUs > 0) {
        av_seek_frame(d->formatCtx, -1, clipInUs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(d->codecCtx);
    }
    d->currentPositionUs = clipInUs;

    // performPendingSeek() may have already set m_postSeekResyncRequested
    // before this fresh decoder existed. The flag is cleared on the next
    // handlePlaybackTick — any decoder opened in that window would
    // otherwise miss the firstFrameDecoded reset and skip its catch-up
    // loop on the very first harvest. Mirror the flag here so the new
    // decoder is treated like every other pool decoder.
    if (m_postSeekResyncRequested) {
        d->firstFrameDecoded = false;
    }

    // V3 sprint diagnostic — codec / HW path log so we can spot AV1 SW
    // fallback or other non-HW pool decoders when the user reports stutter
    // on a specific overlay track.
    qInfo() << "[pool] openTrackDecoder"
            << "filePath=" << entry.filePath
            << "codec=" << avcodec_get_name(d->codecCtx->codec_id)
            << "width=" << d->codecCtx->width
            << "height=" << d->codecCtx->height
            << "hwPixFmt=" << (d->hwPixFmt != AV_PIX_FMT_NONE
                               ? av_get_pix_fmt_name(d->hwPixFmt) : "SW")
            << "sourceTrack=" << entry.sourceTrack;

    if (stallTraceEnabled() && stallTimer.isValid()) {
        const qint64 elapsedMs = stallTimer.elapsed();
        if (elapsedMs >= kStallThresholdOpenMs) {
            qWarning().noquote() << QStringLiteral("[stall>=%1ms] openTrackDecoder %2ms file=%3 track=%4")
                                        .arg(kStallThresholdOpenMs)
                                        .arg(elapsedMs)
                                        .arg(entry.filePath)
                                        .arg(entry.sourceTrack);
        }
    }
    return d;
}

void VideoPlayer::closeTrackDecoder(TrackDecoder *d)
{
    if (!d)
        return;
    // sws first since it doesn't depend on codec lifetime.
    if (d->swsCtx) {
        sws_freeContext(d->swsCtx);
        d->swsCtx = nullptr;
    }
    if (d->frame)
        av_frame_free(&d->frame);
    if (d->swFrame)
        av_frame_free(&d->swFrame);
    if (d->packet)
        av_packet_free(&d->packet);
    if (d->codecCtx)
        avcodec_free_context(&d->codecCtx);
    if (d->formatCtx)
        avformat_close_input(&d->formatCtx);
    // m_sharedPoolHwDeviceCtx is intentionally NOT touched here. Each pool
    // decoder holds its own av_buffer_ref'd handle through codecCtx; freeing
    // the codec context above already drops that ref. The shared device
    // itself stays alive until ~VideoPlayer.
    d->hwPixFmt = AV_PIX_FMT_NONE;
    d->videoStreamIndex = -1;
    d->firstFrameDecoded = false;
    d->lastFrameRgb = QImage();
    d->lastFramePresentedTimelineUs = -1;
    // Caller owns the heap allocation — closeTrackDecoder only does state
    // cleanup so it can be called from openTrackDecoder's failure path
    // before the pointer is even handed to the caller.
}

enum AVPixelFormat VideoPlayer::poolGetHwFormatCallback(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts)
{
    auto *d = static_cast<TrackDecoder*>(ctx->opaque);
    if (!d || d->hwPixFmt == AV_PIX_FMT_NONE)
        return pixFmts[0];
    for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == d->hwPixFmt)
            return *p;
    }
    qWarning() << "poolGetHwFormatCallback: HW pixel format not offered, falling back to SW";
    return pixFmts[0];
}

// ---- Phase 1d software compositor ------------------------------------------
//
// composeMultiTrackFrame paints the supplied layer stack onto the canvas in
// vector order using QPainter::CompositionMode_SourceOver against an
// ARGB32_Premultiplied canvas. The caller (handlePlaybackTick) sorts layers
// DESCENDING by sourceTrack (clipstack::layerPaintOrderLess), so the highest
// track is painted first (backmost) and V1 last (frontmost) — V1-wins
// stacking per PlaybackTypes.h:34. Each layer's videoScale/videoDx/videoDy
// follows the OBS-style transform convention used elsewhere in the player:
// dx/dy in canvas-relative 0..1 units, scale around the canvas center.
//
// V1-only timeline regression safety: when overlayLayers is empty we return
// v1Frame unchanged so the legacy displayFrame path (composeFrameWithOverlays
// for text + GLPreview push) sees the exact same QImage it always saw.

QImage VideoPlayer::composeMultiTrackFrame(const QImage &v1Frame,
                                           const QVector<DecodedLayer> &overlayLayers) const
{
    if (v1Frame.isNull() || overlayLayers.isEmpty())
        return v1Frame;

    // Write canvas-relative dst rects directly (no translate/scale stack)
    // so the geometry matches GLPreview's setVideoSourceTransform 1:1.
    // dx/dy are normalized canvas units (0..1); positive dy moves the
    // layer DOWN visually. QImage Y is top-down so a larger cy paints
    // further down the canvas — and in paintGL positive dy decreases
    // viewportY, which on an OpenGL Y-up framebuffer puts the rendered
    // region nearer the bottom of the screen. Both pipelines therefore
    // agree on the user-facing dy direction with `cy = h/2 + dy * h`.
    // We fill the entire canvas rect with the (scaled) layer image so a
    // 100% opaque base layer paints over the canvas just like GL does.
    QImage composed = v1Frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&composed);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Per-layer placement now goes through the clipgeom SSOT so live preview
    // geometry (translate -> rotate -> scale, normalized dx/dy, canvas-center
    // anchor) is byte-identical to every export path — and rotation is no
    // longer dropped. Nearest-neighbor sampling (smooth=false) is retained
    // for live perf: software bilinear is ~5-10x more expensive at 1080p+ and
    // was the dominant contributor to the multi-track "video heavy"
    // regression. The sampling-only difference vs. the smooth export is
    // absorbed by the PARITY S3 MSE tolerance; the geometry is exact.
    const bool kSmooth = false;
    const QSize canvas(composed.width(), composed.height());
    const QVector<clipgeom::ClipTransform> effectiveTransforms =
        veditor::parentingEnabledFromEnv()
            ? effectiveLayerTransforms(overlayLayers, canvas)
            : QVector<clipgeom::ClipTransform>{};
    for (int i = 0; i < overlayLayers.size(); ++i) {
        const DecodedLayer &L = overlayLayers[i];
        if (L.isNullObject || L.rgb.isNull() || L.opacity <= 0.001)
            continue;
        const clipgeom::ClipTransform t = effectiveTransforms.isEmpty()
            ? transformForLayer(L)
            : effectiveTransforms.value(i, transformForLayer(L));
        QImage placed = clipgeom::renderLayer(
            L.rgb, t, canvas, kSmooth);
        if (!L.layerStyle.isIdentity())
            placed = layerstyle::apply(placed, L.layerStyle);
        p.setOpacity(qBound(0.0, L.opacity, 1.0));
        p.drawImage(0, 0, placed);
    }
    p.end();
    return composed;
}

// Test-only forwarder (declared public in VideoPlayer.h). Repackages
// primitive overlay params into the private DecodedLayer struct and calls
// the REAL composeMultiTrackFrame above so the PARITY S3 selftest measures
// the genuine authoritative compositor. No compositing math is duplicated.
QImage VideoPlayer::composeMultiTrackFrameForTest(
    const QImage &v1Frame,
    const QVector<QImage> &overlayRgb,
    const QVector<double> &overlayOpacity,
    const QVector<double> &overlayScale,
    const QVector<double> &overlayDx,
    const QVector<double> &overlayDy,
    const QVector<double> &overlayRotationDeg,
    const QVector<LayerStyle> &overlayStyle) const
{
    QVector<DecodedLayer> layers;
    layers.reserve(overlayRgb.size());
    for (int i = 0; i < overlayRgb.size(); ++i) {
        DecodedLayer L;
        L.rgb              = overlayRgb.value(i);
        L.opacity          = overlayOpacity.value(i, 1.0);
        L.videoScale       = overlayScale.value(i, 1.0);
        L.videoDx          = overlayDx.value(i, 0.0);
        L.videoDy          = overlayDy.value(i, 0.0);
        L.rotation2DDegrees = overlayRotationDeg.value(i, 0.0);
        L.layerStyle       = overlayStyle.value(i, LayerStyle{});
        layers.append(L);
    }
    return composeMultiTrackFrame(v1Frame, layers);
}

// Phase 1e Win #7 — in-place variant. Same paint logic as
// composeMultiTrackFrame but avoids the convertToFormat indirection that
// shallow-shares with v1Frame and forces QPainter to detach (~0.5-1 ms /
// 1080p tick for the alloc + memcpy of the implicit copy). Caller passes
// a pre-filled canvas (typically m_canvasBase set to Qt::black) that is
// already in ARGB32_Premultiplied. The QPainter binds the caller's
// buffer directly, so when refcount == 1 there is no detach. Geometry
// rules and SourceOver/SmoothPixmapTransform settings match the legacy
// path 1:1 to keep visual output identical.
void VideoPlayer::composeMultiTrackFrameInto(QImage &canvas,
                                              const QVector<DecodedLayer> &overlayLayers) const
{
    if (canvas.isNull() || overlayLayers.isEmpty())
        return;
    // Caller's contract: canvas is ARGB32_Premultiplied. Painting into
    // any other format would silently degrade overlay alpha behaviour,
    // so guard with a debug assert and fast-bail in release.
    Q_ASSERT(canvas.format() == QImage::Format_ARGB32_Premultiplied);
    if (canvas.format() != QImage::Format_ARGB32_Premultiplied)
        return;
    QPainter p(&canvas);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Same clipgeom SSOT placement as composeMultiTrackFrame (geometry exact,
    // rotation included); smooth=false preserved from the legacy live-preview
    // setting for perf — the sampling-only delta vs. export is inside S3's
    // MSE tolerance.
    const bool kSmooth = false;
    const QSize cs(canvas.width(), canvas.height());
    const QVector<clipgeom::ClipTransform> effectiveTransforms =
        veditor::parentingEnabledFromEnv()
            ? effectiveLayerTransforms(overlayLayers, cs)
            : QVector<clipgeom::ClipTransform>{};
    for (int i = 0; i < overlayLayers.size(); ++i) {
        const DecodedLayer &L = overlayLayers[i];
        if (L.isNullObject || L.rgb.isNull() || L.opacity <= 0.001)
            continue;
        const clipgeom::ClipTransform t = effectiveTransforms.isEmpty()
            ? transformForLayer(L)
            : effectiveTransforms.value(i, transformForLayer(L));
        QImage placed = clipgeom::renderLayer(
            L.rgb, t, cs, kSmooth);
        if (!L.layerStyle.isIdentity())
            placed = layerstyle::apply(placed, L.layerStyle);
        p.setOpacity(qBound(0.0, L.opacity, 1.0));
        p.drawImage(0, 0, placed);
    }
    p.end();
}

// STAGE3-GPU: マルチトラックプレビュー GPU 合成のフラグを 1 回だけ解決する。
// 既定 (VEDITOR_GPU_COMPOSITE 未設定 or "1"以外) は false → 従来 CPU 経路を
// 一切変えずに通る (ビット同一)。QSettings "gpuComposite" による任意のメニュー
// 切替も尊重するが、env が明示されていればそちらを優先する (テスト容易性)。
void VideoPlayer::ensureGpuCompositeFlag()
{
    if (m_gpuCompositeChecked)
        return;
    m_gpuCompositeChecked = true;
    const QByteArray env = qgetenv("VEDITOR_GPU_COMPOSITE");
    QSettings settings;
    m_gpuCompositeEnabled = resolveGpuCompositeEnabled(
        !env.isEmpty(),
        env,
        settings.value(QStringLiteral("gpuComposite"), false).toBool());
    if (m_gpuCompositeEnabled)
        qInfo() << "[gpu-composite] enabled (preview multi-track GPU compositing ON)";
}

// STAGE3-GPU: GPU 合成を試みる。対象外 (flag OFF / 単一トラック / GL 不可) や
// 失敗時は null QImage を返し、呼び出し側は従来 CPU 経路へフォールバックする。
// displayFrame は一切呼ばない (表示は呼び出し側の共通 1 箇所が担う)。
// canvas は CPU 経路と同じ適応縮小後サイズ。layers は CPU 経路と同じ
// (V1 base + overlays、paint order ソート済) DecodedLayer 群。
QImage VideoPlayer::tryGpuComposeLayers(const QVector<DecodedLayer> &layers,
                                        QSize canvas)
{
    m_lastFrameOdtApplied = false;
    ensureGpuCompositeFlag();  // 初回 tick で 1 回だけ env/QSettings を解決
    if (!m_gpuCompositeEnabled)
        return QImage();
    // 多トラックのみ対象 (overlay が無いと CPU 経路と同じ単層描画で意味が無い)。
    if (layers.size() < 2 || canvas.isEmpty())
        return QImage();

    // STAGE4B: ライブ・トラックマット (フラグ ON 時のみ到達)。
    //   - マット無しシーン: 従来 Stage3 の高速経路をビット同一で維持 (matteType
    //     を None に固定し、null/透明レイヤーは事前スキップ)。共通ホットパス。
    //   - マット有りシーン: GpuLayerInput を sorted layers と 1:1 に構築する
    //     (事前スキップ禁止)。composite() は内部で gpucomposite::isLayerComposited
    //     により描画対象外を弾くが、マット SOURCE レイヤーは画像 (alpha/luma) が
    //     消費されるため inputs に必ず残す必要があり、かつ matteSourceIndex は
    //     composite() が走査する vector のインデックスでなければならない
    //     (INDEX-SPACE 安全性)。よって 1:1 構築でインデックスを末端まで保つ。
    //   GL 不可時 (isAvailable()==false) は null を返し CPU フォールバック
    //   (matte-free) が走る。すなわちライブ・マットプレビューには GL が必須であり、
    //   GL 不在環境ではフラグ ON でもマットは適用されない (= フラグ OFF と同等)。
    //   これは実験的 opt-in フラグの許容される劣化であり、本タスクでは CPU 側の
    //   ライブ・マット合成は追加しない。
    const bool hasMatte = std::any_of(
        layers.cbegin(), layers.cend(),
        [](const DecodedLayer &L) { return L.matteType != TrackMatteType::None; });
    if (std::any_of(layers.cbegin(), layers.cend(),
                    [](const DecodedLayer &L) {
                        return !L.layerStyle.isIdentity();
                    })) {
        return QImage();
    }
    const QVector<clipgeom::ClipTransform> effectiveTransforms =
        veditor::parentingEnabledFromEnv()
            ? effectiveLayerTransforms(layers, canvas)
            : QVector<clipgeom::ClipTransform>{};

    if (!m_gpuCompositor)
        m_gpuCompositor = std::make_unique<GpuLayerCompositor>();
    if (!m_gpuCompositor->isAvailable())
        return QImage();  // GL 不可 → CPU フォールバック

    // GpuLayerInput を構築する順序 (order) を決める。
    //   - マット無し: 受領順 (paintOrder 降順) のまま (Stage3 とビット同一)。
    //   - マット有り: V1-FIRST (sourceTrack 昇順) に並べ替える【C1 修正】。
    // 理由: matteSourceIndex の検証規則 (index 0 == V1 base、srcIdx>0 && srcIdx!=i
    // のみ採用) は export 経路 (TimelineFrameRenderer の renderLayers = V1 先頭・
    // track 昇順) と同一の index 空間を前提とする。受領した layers は paintOrder 用に
    // sourceTrack 降順 (V1 が末尾) へソート済みのため、そのまま 1:1 で詰めると index 0
    // が最上位トラックになり、正当なマット源 (通常は被マット層より上のトラック) が
    // srcIdx==0 で誤って弾かれてマットが黙って無効化される。composite() は内部で
    // gpucomposite::paintOrder により描画順を再導出するので inputs の並びは出力順に
    // 影響せず index 空間のみを決める。よってマット時のみ V1 先頭昇順へ整列し、export
    // と index 規約を一致させる (preview≡export を担保)。
    QVector<int> order;
    order.reserve(layers.size());
    for (int i = 0; i < layers.size(); ++i)
        order.append(i);
    if (hasMatte) {
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) {
                             // V1 (sourceTrack 0) を先頭へ。安定ソートで同一トラックの
                             // 相対順は保持 (実際は tick あたり 1 トラック 1 クリップ)。
                             return layers.at(a).sourceTrack < layers.at(b).sourceTrack;
                         });
    }

    // CPU の composeMultiTrackFrameInto が渡すのと同じ layers を GpuLayerInput に
    // 詰める。フィールドは DecodedLayer → gpucomposite::LayerDesc に 1:1 写像。
    QVector<GpuLayerInput> inputs;
    inputs.reserve(layers.size());
    for (int oi = 0; oi < order.size(); ++oi) {
        const int layerIdx = order.at(oi);
        const DecodedLayer &L = layers.at(layerIdx);
        // マット無し経路のみ事前スキップ (Stage3 とビット同一)。マット有り経路は
        // インデックス整合のため 1:1 構築し、スキップ判定は composite() 内部に委ねる。
        if (!hasMatte && (L.isNullObject || L.rgb.isNull() || L.opacity <= 0.001))
            continue;  // CPU 経路と同じスキップ条件
        const clipgeom::ClipTransform t = effectiveTransforms.isEmpty()
            ? transformForLayer(L)
            : effectiveTransforms.value(layerIdx, transformForLayer(L));
        GpuLayerInput in;
        in.image = L.rgb;
        in.colorMeta = L.colorMeta;
        in.desc.sourceTrack        = L.sourceTrack;
        in.desc.srcSize            = L.rgb.size();
        in.desc.opacity            = L.opacity;
        in.desc.videoScale         = t.videoScale;
        in.desc.videoDx            = t.videoDx;
        in.desc.videoDy            = t.videoDy;
        in.desc.rotation2DDegrees  = t.rotationDeg;
        in.desc.visible            = !L.isNullObject;
        in.desc.matteSourceIndex   = -1;  // 既定 matte-free; マット有り経路で後段解決
        // TrackMatteType ordinal == gpucomposite::MatteType ordinal
        // (0=None,1=Alpha,2=AlphaInverted,3=Luma/Luminance,4=LumaInverted)。
        // マット無し経路では常に None (ビット同一性のため明示固定)。
        in.desc.matteType = hasMatte
            ? static_cast<gpucomposite::MatteType>(static_cast<int>(L.matteType))
            : gpucomposite::MatteType::None;
        inputs.append(in);
    }
    if (inputs.size() < 2)
        return QImage();  // スキップ後に多トラック条件を満たさない → CPU 経路

    // STAGE4B: マット有り経路のみ、各レイヤーの matteSourceClipId を inputs ベクトル
    // 内のインデックスへ解決する。マット時は事前スキップ無しで inputs は order と 1:1
    // (V1 先頭昇順) なので、i 番目の input は layers.at(order.at(i)) に対応する。
    // clipId は trackMatteClipKey(sourceTrack, sourceClipIndex)。sourceClipIndex は
    // m_sequence[sequenceIdx] から取得 (範囲ガード)。解決規則は export 経路
    // (TimelineFrameRenderer) と同一: srcIdx>0 && srcIdx!=i のみ採用。
    if (hasMatte) {
        clipstack::resolveLiveMatteSources(
            inputs.size(),
            [&](int i) -> QString {  // clipIdOf
                const DecodedLayer &L = layers.at(order.at(i));
                const int sq = L.sequenceIdx;
                if (sq < 0 || sq >= m_sequence.size())
                    return QString();  // 範囲外 → 解決不能 (map に載らず srcIdx=-1)
                return trackMatteClipKey(L.sourceTrack, m_sequence.at(sq).sourceClipIndex);
            },
            [&](int i) -> QString {  // matteSrcClipIdOf
                return layers.at(order.at(i)).matteSourceClipId;
            },
            [&](int i) -> bool {     // hasMatteOf
                return layers.at(order.at(i)).matteType != TrackMatteType::None;
            },
            [&](int idx, int srcIdx) {  // setMatteSrcIndex
                inputs[idx].desc.matteSourceIndex = srcIdx;
            });
    }

    // GpuLayerCompositor::composite は内部で gpucomposite::paintOrder により
    // sourceTrack 降順 (V1-wins) に並べ替えるので、CPU 側の stable_sort と
    // 同じ V1-frontmost スタッキングになる。空 QImage は GL 失敗のシグナル。
    const bool allRgba64 = std::all_of(
        inputs.cbegin(), inputs.cend(),
        [](const GpuLayerInput& in) {
            return tlrcompose16::isRgba64Format(in.image.format());
        });
    if (hdrexport16::preview16Applicable(hdrexport16::enabledFromEnv(),
                                         hasMatte,
                                         static_cast<int>(inputs.size()),
                                         allRgba64)) {
        const bool odtEnabled = clipodt::enabledFromEnv();
        const bool idtEnabled = clipidt::enabledFromEnv();
        aces::ColorSpace v1OutputSpace = aces::ColorSpace::sRGB;
        if (odtEnabled || idtEnabled) {
            int v1Index = 0;
            for (int i = 1; i < inputs.size(); ++i) {
                if (inputs.at(i).desc.sourceTrack < inputs.at(v1Index).desc.sourceTrack)
                    v1Index = i;
            }
            v1OutputSpace = clipcolor::acesSpaceFor(inputs.at(v1Index).colorMeta);
        }

        // Story2: IDT-only (ODT OFF) で VEDITOR_HDR_IDT_GPU が ON なら、CPU の
        // toUnifiedSpace フルフレーム変換を省き、GPU フラグメントシェーダ
        // (composite16Idt) に per-fragment IDT を行わせる。outSpace は
        // composite16Idt が内部で V1 (最小 sourceTrack) から導出し v1OutputSpace と
        // 一致する。GL 失敗時は下の既存 CPU conv 経路へフォールスルー。
        // ODT ON 時はこの分岐を踏まず従来の toLinearWorking+applyOdt16 を維持。
        if (idtEnabled && !odtEnabled && idtgpu::enabledFromEnv()) {
            QImage outIdt = m_gpuCompositor->composite16Idt(inputs, canvas);
            if (!outIdt.isNull() && outIdt.size() == canvas) {
                const QImage out8 = hdrcomposite::to8bit(outIdt);
                if (!out8.isNull() && out8.size() == canvas)
                    return out8;
            }
        }

        const QVector<GpuLayerInput>* composite16Inputs = &inputs;
        QVector<GpuLayerInput> conv;
        if (odtEnabled || idtEnabled) {
            conv = inputs;
            for (int i = 0; i < conv.size(); ++i) {
                if (odtEnabled) {
                    conv[i].image = clipcolor::toLinearWorking(inputs.at(i).image,
                                                               inputs.at(i).colorMeta);
                } else {
                    conv[i].image = clipcolor::toUnifiedSpace(inputs.at(i).image,
                                                              inputs.at(i).colorMeta,
                                                              v1OutputSpace);
                }
            }
            composite16Inputs = &conv;
        }

        QImage out16 = m_gpuCompositor->composite16(*composite16Inputs, canvas);
        if (!out16.isNull() && out16.size() == canvas) {
            if (odtEnabled) {
                out16 = clipodt::applyOdt16(
                    out16, clipodt::OdtParams{v1OutputSpace, true});
            }
            const QImage out8 = hdrcomposite::to8bit(out16);
            if (!out8.isNull() && out8.size() == canvas) {
                if (odtEnabled)
                    m_lastFrameOdtApplied = true;
                return out8;
            }
        }
    }
    if (hdrmatte16::matte16Applicable(hdrmatte16::enabledFromEnv(),
                                      hasMatte,
                                      static_cast<int>(inputs.size()),
                                      allRgba64)) {
        const bool odtEnabled = clipodt::enabledFromEnv();
        const bool idtEnabled = clipidt::enabledFromEnv();
        aces::ColorSpace v1OutputSpace = aces::ColorSpace::sRGB;
        if (odtEnabled || idtEnabled) {
            int v1Index = 0;
            for (int i = 1; i < inputs.size(); ++i) {
                if (inputs.at(i).desc.sourceTrack < inputs.at(v1Index).desc.sourceTrack)
                    v1Index = i;
            }
            v1OutputSpace = clipcolor::acesSpaceFor(inputs.at(v1Index).colorMeta);
        }

        // Stage9b: IDT-only matte preview can skip CPU toUnifiedSpace by using
        // per-fragment IDT on both the matte'd source and the matte source before
        // luma extraction. ODT ON is excluded to avoid double-application.
        if (idtEnabled && !odtEnabled && idtgpu::enabledFromEnv()) {
            QImage out = m_gpuCompositor->composite16IdtMatte(inputs, canvas);
            if (!out.isNull() && out.size() == canvas) {
                const QImage out8 = hdrcomposite::to8bit(out);
                if (!out8.isNull() && out8.size() == canvas)
                    return out8;
            }
        }

        QVector<GpuLayerInput> conv = inputs;
        for (int i = 0; i < conv.size(); ++i) {
            if (odtEnabled) {
                conv[i].image = clipcolor::toLinearWorking(inputs.at(i).image,
                                                           inputs.at(i).colorMeta);
            } else if (idtEnabled) {
                conv[i].image = clipcolor::toUnifiedSpace(inputs.at(i).image,
                                                          inputs.at(i).colorMeta,
                                                          v1OutputSpace);
            }
        }

        if (odtEnabled) {
            QSet<int> encoded;
            for (int i = 0; i < conv.size(); ++i) {
                const gpucomposite::MatteType mt = conv.at(i).desc.matteType;
                const bool usesLuma =
                    mt == gpucomposite::MatteType::Luminance ||
                    mt == gpucomposite::MatteType::LuminanceInverted;
                const int k = conv.at(i).desc.matteSourceIndex;
                if (usesLuma && k >= 0 && k < conv.size() && !encoded.contains(k)) {
                    conv[k].image = clipodt::applyOdt16(
                        conv.at(k).image,
                        clipodt::OdtParams{v1OutputSpace, false});
                    encoded.insert(k);
                }
            }
        }

        QImage out16 = m_gpuCompositor->composite16Matte(conv, canvas);
        if (!out16.isNull() && out16.size() == canvas) {
            if (odtEnabled) {
                out16 = clipodt::applyOdt16(
                    out16, clipodt::OdtParams{v1OutputSpace, true});
            }
            const QImage out8 = hdrcomposite::to8bit(out16);
            if (!out8.isNull() && out8.size() == canvas) {
                if (odtEnabled)
                    m_lastFrameOdtApplied = true;
                return out8;
            }
        }
    }
    QImage out = m_gpuCompositor->composite(inputs, canvas);
    if (out.isNull() || out.size() != canvas)
        return QImage();  // 合成失敗 → CPU フォールバック
    return out;
}

// decodePoolFrame is decodeNextFrame's V2+ twin: same FFmpeg loop logic but
// runs against a TrackDecoder* instead of the legacy m_formatCtx/m_codecCtx
// pair. It does NOT call displayFrame — the compositor in handlePlaybackTick
// owns the display step. On success the pool decoder's lastFrameRgb /
// currentPositionUs / firstFrameDecoded are updated; on failure (EOF) the
// caller falls back to the previous lastFrameRgb so the overlay stays sticky
// instead of going black at clip ends.
bool VideoPlayer::decodePoolFrame(TrackDecoder *d, bool ensureRgb)
{
    if (!d || !d->formatCtx || !d->codecCtx || !d->packet || !d->frame)
        return false;

    const auto receiveFrame = [this, d, ensureRgb]() -> bool {
        // avcodec_receive_frame doesn't always reset the frame before
        // writing — leftover side-data buffers from a prior frame can
        // leak the HW-frame ref it overwrites. Explicit unref makes the
        // contract symmetric with av_read_frame / av_packet_unref.
        av_frame_unref(d->frame);
        const int receiveResult = avcodec_receive_frame(d->codecCtx, d->frame);
        if (receiveResult != 0)
            return false;

        // Compute file-local position in microseconds. We do NOT commit
        // it to d->currentPositionUs here — see the bottom of the lambda.
        // The commit is gated on sws_scale success so any failure path
        // between here and there leaves currentPositionUs at its pre-call
        // value; otherwise a HW-transfer or sws_scale hiccup advances the
        // anchor past the real frame and harvest's drift gate stops being
        // able to detect that the decoder is stuck.
        AVStream *stream = d->formatCtx->streams[d->videoStreamIndex];
        const int64_t bestEffortTimestamp =
            (d->frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? d->frame->best_effort_timestamp
                : d->frame->pts;
        int64_t positionUs = d->currentPositionUs;
        if (bestEffortTimestamp != AV_NOPTS_VALUE) {
            int64_t ts = bestEffortTimestamp;
            if (stream->start_time != AV_NOPTS_VALUE)
                ts -= stream->start_time;
            positionUs = av_rescale_q(ts, stream->time_base, AV_TIME_BASE_Q);
        } else if (d->frameDurationUs > 0) {
            positionUs += d->frameDurationUs;
        }
        positionUs = qMax<int64_t>(0, positionUs);
        if (d->durationUs > 0)
            positionUs = qMin(positionUs, d->durationUs);

        // Fast path for catch-up intermediates: the caller has told us
        // this frame won't be displayed, so just advance the position
        // anchor and skip the HW->SW transfer (~5-8 ms) and sws_scale
        // (~3-5 ms). The decoder state is now at this PTS; the next
        // receive_frame produces the next frame as usual.
        if (!ensureRgb) {
            d->currentPositionUs = positionUs;
            d->firstFrameDecoded = true;
            return true;
        }

        // HW->SW transfer if the decoder is on D3D11VA.
        AVFrame *displayable = d->frame;
        if (d->hwPixFmt != AV_PIX_FMT_NONE && d->frame->format == d->hwPixFmt) {
            if (!d->swFrame)
                return false;
            av_frame_unref(d->swFrame);
            if (av_hwframe_transfer_data(d->swFrame, d->frame, 0) < 0) {
                qWarning() << "decodePoolFrame: av_hwframe_transfer_data failed";
                return false;
            }
            d->swFrame->pts = d->frame->pts;
            d->swFrame->best_effort_timestamp = d->frame->best_effort_timestamp;
            displayable = d->swFrame;
        }

        if (!displayable || displayable->width <= 0 || displayable->height <= 0)
            return false;

        // HDR Stage4: RGBA64 container only — transfer curve is NOT tone-mapped here (F1).
        const bool wantHdr = d->wantRgba64Overlay;
        const AVPixelFormat dstPixFmt = wantHdr ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB24;
        const QImage::Format qFmt = wantHdr ? QImage::Format_RGBA64 : QImage::Format_RGB888;

        const int proxy = playbackProxyDivisor();
        const int dstW = qMax(2, displayable->width  / proxy);
        const int dstH = qMax(2, displayable->height / proxy);

        d->swsCtx = sws_getCachedContext(
            d->swsCtx,
            displayable->width,
            displayable->height,
            static_cast<AVPixelFormat>(displayable->format),
            dstW,
            dstH,
            dstPixFmt,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);

        if (!d->swsCtx)
            return false;

        QImage image(dstW, dstH, qFmt);
        if (image.isNull())
            return false;

        if (!scaleFrameToQImagePadded(d->swsCtx, displayable, dstPixFmt, image))
            return false;

        // Commit position + frame only after the full pipeline succeeded.
        d->currentPositionUs = positionUs;
        d->lastFrameRgb = image;
        d->firstFrameDecoded = true;
        return true;
    };

    if (receiveFrame())
        return true;

    while (av_read_frame(d->formatCtx, d->packet) >= 0) {
        if (d->packet->stream_index != d->videoStreamIndex) {
            av_packet_unref(d->packet);
            continue;
        }
        const int sendResult = avcodec_send_packet(d->codecCtx, d->packet);
        av_packet_unref(d->packet);
        if (sendResult < 0)
            continue;
        if (receiveFrame())
            return true;
    }

    if (avcodec_send_packet(d->codecCtx, nullptr) >= 0 && receiveFrame())
        return true;

    return false;
}

// Parallelizable decode-only step (Phase 1e Sprint US-3). Touches ONLY the
// per-decoder TrackDecoder state, so multiple instances may run on QtConcurrent
// worker threads when threadedPool gate is on (default) + 2+ overlays are active.
//
// Threading contract for callers:
//   - FFmpeg per-context calls (av_seek_frame, avcodec_send_packet/receive_frame,
//     av_hwframe_transfer_data, sws_scale, sws_getCachedContext) are thread-safe
//     when each thread uses its own AVFormatContext / AVCodecContext / SwsContext
//     — which is the case here because TrackDecoder owns those contexts privately.
//   - The shared D3D11 device context (m_sharedPoolHwDeviceCtx) is created at
//     decoder open and read-only thereafter; FFmpeg's d3d11va hwaccel sets
//     D3D11_CREATE_DEVICE_VIDEO_SUPPORT so multi-threaded video-context access
//     is officially supported.
//   - If hw_device_ctx ever becomes mutable from this hot path, revisit the
//     parallel contract.
bool VideoPlayer::runOverlayDecodeForDecoder(TrackDecoder *d,
                                              qint64 expectedFileLocalUs,
                                              qint64 clipInUs,
                                              qint64 clipOutUs)
{
    if (!d)
        return false;

    const int64_t halfFrame = qMax<int64_t>(1, d->frameDurationUs / 2);
    const int64_t drift = expectedFileLocalUs - d->currentPositionUs;

    // Case B: large drift (rewind / non-1x speed mismatch / post-seek). Re-seek.
    if (d->firstFrameDecoded
        && (drift < -halfFrame * 2 || drift > d->frameDurationUs * 4)) {
        const int64_t targetUs = qBound<int64_t>(clipInUs, expectedFileLocalUs, clipOutUs);
        av_seek_frame(d->formatCtx, -1, targetUs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(d->codecCtx);
        d->currentPositionUs = targetUs;
    }

    // Catch-up loop. Intermediate frames skip HW transfer + sws_scale
    // because they're not displayed; only the frame that lands at/past
    // the target needs RGB. This roughly halves overlay decode wall
    // time when V2 is more than 1 frame behind the playhead.
    int catchupCap = d->firstFrameDecoded ? 4 : 8;
    bool ok = false;
    while (catchupCap-- > 0) {
        const bool nearTarget = (d->currentPositionUs + halfFrame * 2 >= expectedFileLocalUs);
        const bool wantRgb = nearTarget || (catchupCap == 0);
        ok = decodePoolFrame(d, wantRgb);
        if (!ok)
            break;
        if (d->currentPositionUs + halfFrame >= expectedFileLocalUs) {
            if (!wantRgb) {
                // We landed at target with a no-RGB decode; one more
                // pass to refresh d->lastFrameRgb for display.
                ok = decodePoolFrame(d, true);
            }
            break;
        }
    }
    return ok;
}

// Main-thread finalize: builds DecodedLayer fields from the decoder, falling
// back to m_evictionGracePool when the decoder produced nothing. Walks the
// grace pool, which is main-thread state — must NOT run from a worker.
bool VideoPlayer::finalizeOverlayFromDecoder(const PlaybackEntry &e, int seqIdx,
                                              TrackDecoder *d, bool decodedOk,
                                              DecodedLayer *out) const
{
    if (!out || !d)
        return false;

    QSize sourceSize;
    if (!d->lastFrameRgb.isNull()) {
        out->rgb     = d->lastFrameRgb;
        out->isFresh = decodedOk;
        if (d->codecCtx && d->codecCtx->width > 0 && d->codecCtx->height > 0)
            sourceSize = QSize(d->codecCtx->width, d->codecCtx->height);
    } else {
        // Fresh decoder with no successful decode yet — try the eviction
        // grace pool for the same identity. Match the TrackKey contract
        // exactly: sourceTrack + sourceClipIndex + qRound64(clipIn*1000)
        // + filePath. Using qFuzzyCompare on raw doubles here would
        // re-introduce the hash/equality skew that breaks QHash's contract.
        const qint64 eClipMs = qRound64(e.clipIn * 1000.0);
        for (TrackDecoder *g : m_evictionGracePool) {
            if (!g)
                continue;
            if (g->sourceTrack == e.sourceTrack
                && g->sourceClipIndex == e.sourceClipIndex
                && qRound64(g->clipIn * 1000.0) == eClipMs
                && g->filePath == e.filePath
                && !g->lastFrameRgb.isNull()) {
                out->rgb     = g->lastFrameRgb;
                out->isFresh = false;
                if (g->codecCtx && g->codecCtx->width > 0 && g->codecCtx->height > 0)
                    sourceSize = QSize(g->codecCtx->width, g->codecCtx->height);
                break;
            }
        }
        if (out->rgb.isNull())
            return false;
    }

    if (sourceSize.width() <= 0 || sourceSize.height() <= 0)
        sourceSize = out->rgb.size();
    const Timeline *previewTimeline = m_glPreview ? m_glPreview->timeline() : nullptr;
    if (seqIdx >= 0 && seqIdx < m_sequence.size()) {
        const double sourceSec =
            static_cast<double>(entryLocalPositionUs(seqIdx, m_timelinePositionUs))
            / AV_TIME_BASE;
        out->rgb = applyPreviewClipMask(
            out->rgb, previewTimeline, e, sourceSec, sourceSize);
    }

    out->colorMeta          = e.colorMeta;
    applyLayerMotionOpacity(previewTimeline, e, m_timelinePositionUs, e.opacity, out);
    populateLayerMetadata(previewTimeline, e, seqIdx, out);
    return true;
}

bool VideoPlayer::harvestOverlayLayer(const PlaybackEntry &e, int seqIdx, DecodedLayer *out)
{
    if (!out)
        return false;

    TrackDecoder *d = acquireDecoderForClip(e);
    if (!d)
        return false;

    const int64_t expectedFileLocalUs = entryLocalPositionUs(seqIdx, m_timelinePositionUs);
    const int64_t halfFrame = qMax<int64_t>(d->frameDurationUs / 2, 1);
    bool ok = true;
    // Phase 1e Win #6: skip catch-up entirely when a worker prefetch already
    // brought the decoder current. Without this, the parallel-path optimization
    // would still re-enter the decode loop on the main thread and decode an
    // extra frame past the playhead. firstFrameDecoded gates against fresh
    // decoders that have no usable lastFrameRgb yet.
    if (d->firstFrameDecoded
        && !d->lastFrameRgb.isNull()
        && d->currentPositionUs + halfFrame >= expectedFileLocalUs) {
        // Already current — no decode needed; just finalize.
    } else {
        const int64_t clipInUs  = static_cast<int64_t>(e.clipIn  * AV_TIME_BASE);
        const int64_t clipOutUs = static_cast<int64_t>(e.clipOut * AV_TIME_BASE);
        ok = runOverlayDecodeForDecoder(d, expectedFileLocalUs, clipInUs, clipOutUs);
    }
    const bool finalized = finalizeOverlayFromDecoder(e, seqIdx, d, ok, out);
    if (finalized)
        out->colorMeta = e.colorMeta;
    return finalized;
}

bool VideoPlayer::hasOverlayActive(const QVector<int> &activeIdxs) const
{
    for (int idx : activeIdxs) {
        if (idx >= 0 && idx < m_sequence.size() && m_sequence[idx].sourceTrack > 0)
            return true;
    }
    return false;
}

// V3 sprint — Timeline で V2/V3 clip を選択したときに preview drag handle が
// 編集する layer を切り替える。再生 source の m_activeEntry には触れない。
void VideoPlayer::setEditTargetByClip(int sourceTrack, int sourceClipIndex)
{
    const int oldTarget = m_editTargetEntry;

    // Push transform to GLPreview whenever edit target changes (paused or not).
    auto pushTransform = [&]() {
        const bool hasExplicitTarget = (m_editTargetEntry >= 0
                                         && m_editTargetEntry < m_sequence.size());
        const int displayIdx = hasExplicitTarget
                               ? m_editTargetEntry
                               : m_activeEntry;
        if (displayIdx >= 0 && displayIdx < m_sequence.size() && m_glPreview) {
            const auto &targetE = m_sequence[displayIdx];
            m_glPreview->setVideoSourceTransform(targetE.videoScale,
                                                  targetE.videoDx,
                                                  targetE.videoDy);
            // V3 sprint NIT — arm handle gate ONLY for explicit Timeline-driven
            // selection (m_editTargetEntry >= 0). Active-entry fallback keeps
            // the legacy "click preview to arm" UX for V1 so handles don't
            // appear unprompted on project load.
            if (hasExplicitTarget) {
                m_glPreview->setVideoTransformSelected(true);
            }
        } else if (m_glPreview) {
            // No valid index at all — clear the gate so stale handles vanish.
            m_glPreview->setVideoTransformSelected(false);
        }
    };

    if (sourceTrack < 0 || sourceClipIndex < 0) {
        m_editTargetEntry = -1;
        if (oldTarget != m_editTargetEntry) {
            qInfo() << "[edit-target] cleared (was seq=" << oldTarget << ")";
            pushTransform();
        }
        return;
    }
    for (int i = 0; i < m_sequence.size(); ++i) {
        const auto &e = m_sequence[i];
        if (e.sourceTrack == sourceTrack && e.sourceClipIndex == sourceClipIndex) {
            m_editTargetEntry = i;
            if (oldTarget != m_editTargetEntry) {
                qInfo() << "[edit-target] set seq=" << i
                        << "track=" << sourceTrack
                        << "clip=" << sourceClipIndex
                        << "(was seq=" << oldTarget << ")";
                pushTransform();
            }
            return;
        }
    }
    // No matching entry — fall back to follow active.
    m_editTargetEntry = -1;
    if (oldTarget != m_editTargetEntry) {
        qInfo() << "[edit-target] no match for track=" << sourceTrack
                << "clip=" << sourceClipIndex << "(was seq=" << oldTarget << ")";
        pushTransform();
    }
}
