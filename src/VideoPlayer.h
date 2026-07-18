#pragma once

#include <QWidget>
#include <QLabel>
#include <QImage>
#include <QColor>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "AudioMixer.h"
#include "SpeedRampData.h"
#include <QVector>
#include <QHash>
#include <QRectF>
#include <functional>
#include "VideoEffect.h"
#include "PlaybackTypes.h"
#include "color/ClipColor.h"
#include "MaskSystem.h"  // STAGE4B: TrackMatteType for DecodedLayer matte fields
#include "TextManager.h"
#include "DecoderSlotManager.h"
#include "AcesColor.h"  // AR-2: ACES シーンリファード色管理パイプライン (SSOT は MainWindow)
#include "ExposureAids.h"  // EXP-AID: 露出/フォーカス確認エイド (プレビュー表示専用)
#include "SafeZone.h"      // SAFE-ZONE: SNS セーフゾーンオーバーレイ (プレビュー表示専用)
#include "OnionSkin.h"     // ONION-SKIN: 前後フレーム半透明オーバーレイ (プレビュー表示専用)
#include "playback/CompositeFrameCache.h"     // ADAPTIVE-1: 合成フレーム LRU キャッシュ
#include "playback/PlaybackQualityPolicy.h"   // ADAPTIVE-1: 再生品質ヒステリシスポリシー
#include "playback/GpuLayerCompositor.h"      // STAGE3-GPU: マルチトラック GPU 合成 (既定 OFF)
#include <memory>                             // STAGE3-GPU: m_gpuCompositor 遅延生成用

// Identifies a per-clip decoder in the V2+ pool. Keyed on
// (filePath, clipInMs, sourceTrack, sourceClipIndex) so:
//   - the key stays stable across Timeline::sequenceChanged re-emits;
//   - importing the same file with the same trim onto the same track
//     twice does NOT collide (each ClipInfo carries its own
//     sourceClipIndex, so two distinct clips own two distinct decoders
//     instead of stealing each other's slot);
//   - clipIn is quantized to 1ms (qint64) so operator== and qHash use
//     IDENTICAL bucketing — qFuzzyCompare on raw doubles can compare
//     equal while int(x*1000) hashes to neighbouring buckets, which
//     would silently break QHash's contract.
// The pool holds whatever entry is NOT m_activeEntry. The active entry
// (typically V1, but V2 in V2-earlier scenarios) stays on the legacy
// single-decoder path (m_formatCtx/m_codecCtx); every other active
// entry — V1 included when V2 owns the legacy decoder — comes from
// the pool so V1-wins paint order can layer it on top.
struct TrackKey {
    QString filePath;
    qint64 clipInMs = 0;        // qRound64(clipIn * 1000.0) — 1ms precision
    int sourceTrack = 0;
    int sourceClipIndex = -1;   // disambiguates same file+trim+track on different clips
    bool operator==(const TrackKey &other) const noexcept
    {
        return sourceTrack == other.sourceTrack
            && sourceClipIndex == other.sourceClipIndex
            && clipInMs == other.clipInMs
            && filePath == other.filePath;
    }
};

inline uint qHash(const TrackKey &k, uint seed = 0) noexcept
{
    return qHash(k.filePath, seed)
         ^ qHash(k.sourceTrack, seed + 0x9e3779b9u)
         ^ qHash(k.clipInMs, seed + 0x517cc1b7u)
         ^ qHash(k.sourceClipIndex, seed + 0x85ebca6bu);
}

class GLPreview;
class QResizeEvent;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

void setPlaybackProxyDivisorPreference(int div);

class VideoPlayer : public QWidget
{
    Q_OBJECT

public:
    // Multi-track decoder slot ceiling. The LRU manager (DecoderSlotManager)
    // evicts the slot whose clip is furthest from the playhead when this is
    // exceeded; V1 (track 0) is protected so the main video never gets bumped.
    static constexpr int MAX_ACTIVE_DECODERS = 4;

    explicit VideoPlayer(QWidget *parent = nullptr);
    ~VideoPlayer();

    void loadFile(const QString &filePath);
    // Replace the active playback schedule with a multi-clip sequence. When
    // non-empty, seek/playback are interpreted in timeline-space and files are
    // switched automatically at clip boundaries. Empty argument falls back to
    // single-file mode (current loaded file is left intact).
    void setSequence(const QVector<PlaybackEntry> &entries);
    // Parallel speed-ramp array for the video sequence. Must be called
    // after setSequence with the same index alignment. Identity ramps are
    // the default so callers may omit this call for non-ramped sequences.
    void setSpeedRamps(const QVector<speedramp::SpeedRamp> &ramps);
    // Audio-side schedule routed to AudioMixer. The mixer owns its own
    // FFmpeg decoder pool + ring buffers and mixes every active entry into
    // a single QAudioSink output, so unlinked J-cut/L-cut clips and stacked
    // A2/A3/... tracks all sound simultaneously.
    void setAudioSequence(const QVector<PlaybackEntry> &entries);
    AudioMixer *audioMixer() { return m_mixer; }
    void setMuted(bool muted);
    bool isMuted() const { return m_muted; }
    void setCanvasSize(int width, int height);
    void setProjectOutputSize(const QSize &size);
    QSize projectOutputSize() const { return m_projectOutputSize; }
    void setColorCorrection(const ColorCorrection &cc);
    // AR-2: ACES シーンリファード色管理。MainWindow の SSOT (m_acesPipeline) を
    // プレビューへ反映する。enabled=true のときのみ displayFrame の最終合成結果へ
    // aces::applyPipelineToImage を適用し、enabled=false なら一切呼ばず従来出力と
    // ビット同一を維持する (回帰ゼロ)。セット時に表示中フレームを即再描画する。
    void setAcesPipeline(const aces::AcesPipeline &pipeline);
    // EXP-AID: 露出/フォーカス確認エイド (フォルスカラー / ゼブラ / フォーカスピーキング)。
    // これは「画面確認専用」のオーバーレイで、displayFrame が GL / ラベルへ渡す直前に
    // 表示用 QImage の一時コピーへのみ適用する。m_currentFrameImage・frameComposited
    // のペイロード・合成キャッシュ (cachePreviewComposite) には素のフレームが入るので
    // 汚染しない。書き出し (renderFrameAt / TimelineFrameRenderer / RenderQueue /
    // Exporter) には一切関与せず、エイドは書き出し画には焼き込まれない。
    // mode == None (既定) のときは一切呼ばず従来パスとビット同一。set 時に表示中
    // フレームを即再描画する。
    void setExposureAidMode(exposureaid::AidMode mode);
    void setExposureAidConfig(const exposureaid::AidConfig &cfg);
    void setSafeZonePlatform(safezone::Platform p);  // SAFE-ZONE
    void setOnionSkinConfig(const onionskin::Config &cfg);
    onionskin::Config onionSkinConfig() const { return m_onionSkin; }
    // PV-C: プレビュー表示の長辺上限(px)。0=無制限。display専用(書き出し非変更)。
    void setPreviewMaxLongSide(int px);
    int previewMaxLongSide() const { return m_previewMaxLongSide; }
    exposureaid::AidMode exposureAidMode() const { return m_exposureAidMode; }
    safezone::Platform safeZonePlatform() const { return m_safeZonePlatform; }
    // Transient effect stack applied on top of every composed frame (live dialog preview).
    // Empty vector disables the path. Does not mutate timeline state.
    // live=true keeps CPU effects active during playback (committed state).
    // live=false (default) is pause-only — used during dialog editing so
    // heavy per-frame work doesn't stutter slider drags.
    void setPreviewEffects(const QVector<VideoEffect> &effects, bool live = false);
    void setGpuEffectsEnabled(bool enabled);
    bool isGLAccelerated() const { return m_useGL; }
    void setGLAcceleration(bool enabled);

    // Read-only accessor used by MainWindow when retargeting the sequence
    // (proxy generation finished, proxy mode toggled). Wrapping the swap in
    // pause/play stops the mixer and the frame timer from
    // running through the loadFile reset, which previously surfaced as a
    // visible rewind + speed-up when the path resolution changed underneath
    // an active playback.
    bool isPlaying() const { return m_playing; }

    // Iteration 15 — preview-maximize state. MainWindow listens for
    // previewMaximizeChanged and hides/shows the timeline accordingly.
    bool isPreviewMaximized() const { return m_previewMaximized; }

    // Current playhead in timeline microseconds. MainWindow calls this
    // before refreshPlaybackSequence so it can re-seek the player to the
    // same position once the path resolution settles, instead of relying
    // on setSequence's clamped restoration which drifts back to the head
    // when the new entries trigger a fresh decoder open.
    int64_t timelinePositionUs() const { return m_timelinePositionUs; }

    // Proxy preview divisor (1 = full, 2 = half, 4 = quarter, 8 = eighth)
    // exposed so the proxy settings dialog (opened from the seekbar-left
    // button) can read and update it without touching VideoPlayer's
    // internals directly.
    int proxyDivisor() const { return m_proxyDivisor; }
    void setProxyDivisor(int divisor);
    void applyPlaybackQualityChanged();
    GLPreview *glPreview() const { return m_glPreview; }
    // Phase 1e — returns the ID3D11Device* the FFmpeg HW context owns so
    // GLPreview can hand it to wglDXOpenDeviceNV. nullptr when no D3D11VA
    // device is currently active. Opaque void* keeps d3d11.h out of headers.
    void *sharedD3D11Device() const noexcept;

    struct HdrInfo {
        bool isHdr = false;
        AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
        AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
        AVColorSpace colorspace = AVCOL_SPC_UNSPECIFIED;
        int bitDepth = 8;
    };
    const HdrInfo &hdrInfo() const { return m_hdrInfo; }
    bool isHdrSource() const { return m_hdrInfo.isHdr; }
    // Text tool mode — flips the preview cursor to Qt::IBeamCursor and
    // enables the drag-to-rect capture inside GLPreview. When a drag
    // finishes, GLPreview emits textRectRequested (re-emitted here) with
    // the normalized 0.0–1.0 rect in widget-relative coordinates.
    void setTextToolActive(bool active);
    // Clear the persisted text-tool marquee drawn on the preview. Call
    // after the user commits text so the marquee disappears.
    void clearTextToolRect();
    // Forward the in-place text style (font + color) to GLPreview so the
    // live-typed text matches what will be committed via applyTextToolOverlay.
    void setTextToolStyle(const QFont &font, const QColor &color);
    // US-T39 snap-to-frame strength passthrough.
    void setSnapStrength(double pixels);
    // Rebuild the hit list GLPreview uses for click-to-edit on existing
    // rendered overlays. MainWindow calls this after every setTextOverlays.
    void refreshTextOverlayHits();
    // Query / force-commit the preview's in-place edit session. Used by
    // MainWindow's 適用 button to pull the text typed directly on the
    // preview (instead of reading a stale QLineEdit value).
    bool isTextToolEditing() const;
    QString currentTextToolInputText() const;
    void commitCurrentTextToolEdit();
    // Replace the set of text overlays composited on top of the video
    // during displayFrame. MainWindow rebuilds and pushes this list after
    // every text-tool commit so the preview picks up the new overlay.
    void setTextOverlays(const QVector<EnhancedTextOverlay> &overlays);
    // Hide overlay `index` during compose so its committed render doesn't
    // show behind the GLPreview inline edit layer (-1 = none). Re-composes
    // the cached source frame so the change is visible while paused.
    void setHiddenTextOverlayIndex(int index);
    // US-WIRE-3: region picker for motion tracking. The user drags a
    // rectangle on the preview; the callback receives the mapped rect
    // in source-frame pixel coordinates.
    void enterRegionPickerMode(std::function<void(QRect)> callback);
    void exitRegionPickerMode();
    bool isRegionPickerActive() const { return m_regionPickerActive; }
    // US-EF-2: Mask Animation drawing. Reuses the same overlay infrastructure
    // as enterRegionPickerMode but the callback receives a normalized
    // QRectF in [0..1] vTexCoord-space — derived by mapping the source-frame
    // pixel rect against the codec's frame size. Suitable for feeding back
    // into ColorGradingPanel::setMaskRect or GLPreview::setMask directly.
    void enterMaskEditMode(std::function<void(QRectF)> callback);
    void exitMaskEditMode();
    void enterWbEyedropperMode(std::function<void(QColor)> callback);
    void exitWbEyedropperMode();
    bool isWbEyedropperActive() const { return m_wbEyedropperActive; }

    // Test-only seam for the PARITY S3 selftest (src/main.cpp
    // runParitySelftest). composeMultiTrackFrame + the DecodedLayer struct
    // are private; this thin public forwarder builds DecodedLayer entries
    // from primitive overlay params and invokes the REAL private
    // composeMultiTrackFrame so the selftest's reference is the genuine
    // authoritative compositor (not a copy) without exposing private types.
    // Production code must keep using composeMultiTrackFrame directly.
    QImage composeMultiTrackFrameForTest(
        const QImage &v1Frame,
        const QVector<QImage> &overlayRgb,
        const QVector<double> &overlayOpacity,
        const QVector<double> &overlayScale,
        const QVector<double> &overlayDx,
        const QVector<double> &overlayDy,
        const QVector<double> &overlayRotationDeg = {},
        const QVector<LayerStyle> &overlayStyle = {}) const;
    // Test-only seam for grade-keyframe GPU-preview wiring.
    bool pushActiveClipColorCorrectionToGlPreviewForTest(qint64 timelineUsec);

    // NOTE: the genuine text baker is now the free function
    // textbake::bakeOverlays (src/TextOverlayBake.h), extracted verbatim from
    // composeFrameWithOverlays so the SSOT renderer's worker-thread export
    // path can bake text WITHOUT constructing a VideoPlayer (a QWidget) off
    // the GUI thread. composeFrameWithOverlays delegates to it; the S6 parity
    // selftest calls it directly. The old composeFrameWithOverlaysForTest
    // QWidget seam was removed (it forced off-GUI-thread QWidget construction
    // in the export path — Qt undefined behaviour).

public slots:
    void play();
    void pause();
    void stop();
    void seek(int positionMs);
    void previewSeek(int positionMs);
    void setPlaybackSpeed(double speed);
    void speedUp();    // L key
    void speedDown();  // J key
    void togglePlay(); // K key
    // Step one frame while paused (no-op during playback).
    void stepForward();
    void stepBackward();
    // Iteration 15 — preview-maximize toggle. Forwarded to MainWindow via
    // previewMaximizeChanged so the timeline can hide/show accordingly.
    void setPreviewMaximized(bool maximized);
    // V3 sprint — Timeline 上で V2/V3 clip を選択したときに preview の drag
    // handle が選択 layer を target にするための edit target setter。
    // sourceTrack, sourceClipIndex から m_sequence を逆引きして該当 seqIdx
    // を計算し m_editTargetEntry に設定する。-1, -1 で follow active に戻す。
    void setEditTargetByClip(int sourceTrack, int sourceClipIndex);

signals:
    void positionChanged(double positionSeconds);
    void durationChanged(double durationSeconds);
    void stateChanged(bool playing);
    void playbackSpeedChanged(double speed);
    // Emitted whenever the composited preview image is refreshed. Hot path
    // (called from each frame tick), so consumers must rate-limit if they
    // do non-trivial work — e.g. LumetriScopes throttles to ~10 fps.
    void frameComposited(const QImage &frame);
    // Emitted when the user clicks the seekbar-left proxy button. MainWindow
    // owns the settings dialog so it can drive both the file-level proxy
    // mode (ProxyManager) and the preview divisor (VideoPlayer) through
    // the same affordance.
    void proxySettingsRequested();
    void textRectRequested(const QRectF &normalizedRect);
    void textInlineCommitted(const QString &text, const QRectF &normalizedRect);
    void textOverlayEditCommitted(int overlayIndex, const QString &newText);
    void textOverlayRectChanged(int overlayIndex, const QRectF &normalizedRect);
    // US-T35 the active playback entry's per-clip video transform was
    // changed by the user dragging the preview. trackIdx/clipIdx identify
    // the source ClipInfo so MainWindow can persist to the timeline.
    void videoSourceTransformChanged(int trackIdx, int clipIdx,
                                     double scale, double dx, double dy);
    // Iteration 15 — emitted when the user clicks the maximize button or
    // when MainWindow's Esc shortcut calls setPreviewMaximized(false).
    void previewMaximizeChanged(bool maximized);
    // PV-B — プレビュー上で右クリックされた(グローバル座標)。MainWindow が
    // 受けて表示トグル+現クリップ操作メニューを構築する。
    void previewContextMenuRequested(const QPoint &globalPos);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    QSize fittedDisplaySize(const QSize &bounds) const;
    double effectiveDisplayAspectRatio() const;
    double streamDisplayAspectRatio() const;
    void refreshDisplayedFrame();
    void setupUI();
    void resetDecoder();
    void scheduleNextFrame();
    void performPendingSeek();
    void updatePositionUi();
    bool pushActiveClipColorCorrectionToGlPreview();
    // Like updatePositionUi() but does NOT reproject m_timelinePositionUs
    // from m_currentPositionUs. Used by stepForward/Backward when they need
    // the seekbar / time label / positionChanged signal to honour the
    // already-set m_timelinePositionUs verbatim (sequence-mode reprojection
    // would otherwise snap the playhead back to the active entry's
    // file-local PTS, undoing the step).
    void forceTimelineUiToCurrent();
    bool seekInternal(int64_t positionUs, bool displayFrame, bool precise);
    bool decodeNextFrame(bool displayFrame);
    bool presentDecodedFrame(AVFrame *frame, bool displayFrame);
    // Phase 1e — V1 fast path is opt-in via VEDITOR_GL_INTEROP=1 and limited
    // to a narrow set of conditions: HW-decoded D3D11 frame, no overlays
    // (text or V2+), no preview effects, no HDR, not deferred. Anything
    // outside this set falls through to the legacy sws_scale + QImage path.
    bool canUseInteropFastPath(const AVFrame *frame) const;
    QImage frameToImage(const AVFrame *frame);
    AVFrame *ensureSwFrame(AVFrame *frame);
    static enum AVPixelFormat getHwFormatCallback(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts);
    int64_t streamFrameDurationUs() const;
    int64_t streamTimestampForPosition(int64_t positionUs) const;
    int64_t positionFromStreamTimestamp(int64_t timestamp) const;
    int sliderPositionForUs(int64_t positionUs) const;
    void handlePlaybackTick();
    void updatePlayButton();
    void displayFrame(const QImage &image);
    void displaySeekFrameConformed(const QImage &v1Image);

    // Sequence helpers (Phase A/B). When m_sequence is empty, the player runs
    // in single-file legacy mode and these are unused.
    bool sequenceActive() const { return !m_sequence.isEmpty(); }
    int findActiveEntryAt(int64_t timelineUs) const;
    // Multi-track: every sequence index whose [timelineStart, timelineEnd)
    // contains timelineUs. Returned in source order (same as m_sequence),
    // which is sorted (timelineStart asc, sourceTrack asc) — so V1 sits at
    // the front, V2/V3/... follow. Empty if no entry is active at timelineUs
    // (no edge-case fallback to first/last; the composite path treats an
    // empty result as "no frame to draw").
    QVector<int> findActiveEntriesAt(int64_t timelineUs) const;
    int64_t entryLocalPositionUs(int entryIdx, int64_t timelineUs) const;
    int64_t fileLocalToTimelineUs(int entryIdx, int64_t fileLocalUs) const;
    bool seekToTimelineUs(int64_t timelineUs, bool precise);
    bool advanceToEntry(int newEntryIdx);
    // Phase 1e Win #16 (Iteration 9) — Premiere Pro-style decoder hot-swap.
    // When `m_sequence[newEntryIdx]` already has a warm TrackDecoder in
    // m_trackDecoders (typically prerolled by handlePlaybackTick before the
    // boundary), move its formatCtx/codecCtx/frame/swFrame/packet straight
    // into the legacy V1 primary slots and demote the old primary into the
    // eviction grace pool. Result: no avformat_open_input / find_stream_info
    // / avcodec_open2 on the main thread → boundary cross becomes a sub-ms
    // pointer swap instead of the previous 60-200 ms loadFile stall.
    // Returns true on a successful swap (caller must skip its loadFile),
    // false when no warm pool decoder is available (caller falls back to
    // legacy loadFile). Opt-out via VEDITOR_HOTSWAP_DISABLE=1.
    bool tryPromotePoolDecoderTo(int newEntryIdx);
    void applySequenceSliderRange();
    int sliderTimelinePosition(int64_t timelineUs) const;
    // A/V drift correction: pace video against the AudioMixer master clock
    // file as the active video entry, treat the audio clock as master and
    // skip-decode extra video frames if the video PTS has fallen behind.
    // No-op for J-cut/L-cut (audio file != video file) and for reverse
    // playback. Returns the number of extra frames actually skip-decoded.
    int correctVideoDriftAgainstAudioClock();

    // ---- Per-clip decoder pool ----------------------------------------------
    // The legacy single decoder (m_formatCtx/m_codecCtx/m_frame/...) owns
    // m_activeEntry's frames. Every other active entry opens its own
    // TrackDecoder lazily on demand — V1 included when V2 starts earlier
    // on the timeline so the legacy decoder is sitting on V2.
    struct TrackDecoder {
        int sourceClipIndex = -1;
        int sourceTrack = 0;
        QString filePath;
        // Source-file clip-in seconds. Cached so the eviction grace pool
        // can match decoders by stable identity (filePath + clipIn +
        // sourceTrack) across clip reorders without going through the
        // sequence index, which can shift independently.
        double clipIn = 0.0;
        AVFormatContext *formatCtx = nullptr;
        AVCodecContext *codecCtx = nullptr;
        SwsContext *swsCtx = nullptr;
        AVFrame *frame = nullptr;
        AVFrame *swFrame = nullptr;
        AVPacket *packet = nullptr;
        int videoStreamIndex = -1;
        AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
        int64_t durationUs = 0;
        int64_t frameDurationUs = 0;
        double displayAspect = 0.0;
        int64_t currentPositionUs = 0;
        QImage lastFrameRgb;
        int64_t lastFramePresentedTimelineUs = -1;
        bool firstFrameDecoded = false;
        // HDR Stage4: write-once in openTrackDecoder from clip HDR metadata
        // and VEDITOR_HDR_OVERLAY. Default false keeps the RGB888 path.
        bool wantRgba64Overlay = false;
        // > 0 means this decoder has been moved into the eviction grace
        // pool. Decremented per playback tick; reaches 0 → safe to free
        // (deferred outside decode loop).
        int graceTtlTicks = 0;
    };

    TrackDecoder *acquireDecoderForClip(const PlaybackEntry &entry);
    void releaseDecoderForClip(const TrackKey &key);

public:
    // ---- Phase 1d software compositor ---------------------------------------
    // One overlay layer harvested from a V2+ TrackDecoder for the current
    // tick. composeMultiTrackFrame paints these on top of the V1 frame in
    // m_sequence order (V1 base + V2/V3/... above), with isFresh=false
    // entries falling back to the previous decoded frame from the eviction
    // grace pool when a re-seek hasn't caught up yet.
    //
    // Public so that clipstack::layerPaintOrderLess (the sort comparator
    // extracted for testability) can be declared as a free function below
    // (inside namespace clipstack) and used in the S3-STACK predicate
    // sub-assertion in src/main.cpp without exposing the full private
    // compositor surface.
    struct DecodedLayer {
        QImage rgb;
        double opacity = 1.0;
        clipcolor::ColorMeta colorMeta;
        double videoScale = 1.0;
        double videoDx = 0.0;
        double videoDy = 0.0;
        double rotation2DDegrees = 0.0;
        int sourceTrack = 0;
        int sourceClipIndex = -1;
        int sequenceIdx = -1;
        LayerStyle layerStyle;
        bool fitContain = false;
        bool fitCover = false;
        bool isFresh = true;
        bool isNullObject = false;
        // STAGE4B (live GPU track-matte): consumed by tryGpuComposeLayers when
        // VEDITOR_GPU_COMPOSITE is ON and GL is available (else matte-free).
        // matteType + matteSourceClipId are copied from the PlaybackEntry; the
        // matteSourceClipId is the trackMatteClipKey ("trackIdx:clipIdx") of the
        // matte SOURCE clip. matteSourceIndex is the RESOLVED index into the
        // final composite() inputs vector, computed by
        // clipstack::resolveLiveMatteSources (-1 == composite normally / no
        // matte). Default values == today's matte-free live preview.
        TrackMatteType matteType = TrackMatteType::None;
        QString matteSourceClipId;
        int matteSourceIndex = -1;
        QString parentClipId;
        int parentSourceIndex = -1;
    };

private:

    QImage composeMultiTrackFrame(const QImage &v1Frame,
                                  const QVector<DecodedLayer> &overlayLayers) const;
    // Phase 1e Win #7 — in-place variant. Caller pre-fills `canvas` (typically
    // m_canvasBase with Qt::black) and we paint overlays into it without going
    // through convertToFormat shallow-share + QPainter detach. Saves the
    // ~8MB alloc + memcpy that the legacy composeMultiTrackFrame triggers
    // every tick at 1080p. Caller MUST guarantee:
    //   - canvas.format() == ARGB32_Premultiplied
    //   - canvas owned solely by caller at entry (refcount == 1 ideally; if
    //     >1 QPainter still detaches but the ROI lift goes back to legacy)
    void composeMultiTrackFrameInto(QImage &canvas,
                                    const QVector<DecodedLayer> &overlayLayers) const;
    bool harvestOverlayLayer(const PlaybackEntry &entry, int seqIdx, DecodedLayer *out);
    // Phase 1e Sprint US-3: parallelizable decode-only step. ONLY touches
    // the per-decoder TrackDecoder state — no pool / sequence / Qt object
    // access — so it is safe to invoke from a QtConcurrent worker thread.
    bool runOverlayDecodeForDecoder(TrackDecoder *d, qint64 expectedFileLocalUs,
                                    qint64 clipInUs, qint64 clipOutUs);
    // Main-thread post-processing: builds DecodedLayer fields from the
    // decoder's lastFrameRgb, falling back to the eviction grace pool when
    // the decoder produced nothing.
    bool finalizeOverlayFromDecoder(const PlaybackEntry &entry, int seqIdx,
                                    TrackDecoder *d, bool decodedOk,
                                    DecodedLayer *out) const;
    // ensureRgb=false skips av_hwframe_transfer_data + sws_scale and only
    // advances d->currentPositionUs. Used by harvestOverlayLayer's catch-up
    // loop on intermediate frames the user will never see — saves ~6-12 ms
    // per skipped frame at 1080p HEVC, which dominated the multi-track
    // tick budget before this was introduced.
    bool decodePoolFrame(TrackDecoder *d, bool ensureRgb = true);
    bool hasOverlayActive(const QVector<int> &activeIdxs) const;
    // Tear down every active V2+ decoder + every grace-pool decoder.
    // Called from the destructor and from setSequence() reconciliation
    // when the clip set genuinely changed (US-3 will refine this).
    void clearAllPoolDecoders();
    // Decrement grace TTLs and free any TrackDecoder that hits zero.
    // MUST NOT be called from inside decodeNextFrame / presentDecodedFrame
    // / handlePlaybackTick — call only from a deferred QTimer::singleShot
    // or at the start of the next tick.
    void tickEvictionGracePool();
    TrackDecoder *openTrackDecoder(const PlaybackEntry &entry);
    void closeTrackDecoder(TrackDecoder *d);
    static enum AVPixelFormat poolGetHwFormatCallback(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts);

    QLabel *m_videoDisplay;
    GLPreview *m_glPreview = nullptr;
    bool m_useGL = true;
    QPushButton *m_proxyButton = nullptr;
    QPushButton *m_stepBackButton = nullptr;
    QPushButton *m_stepFwdButton = nullptr;
    // Doubles as the pause button: clicked is wired to togglePlay() and
    // updatePlayButton() flips its glyph between ▶ and ⏸ depending on
    // m_playing.
    QPushButton *m_playButton;
    QPushButton *m_stopButton;
    QPushButton *m_maximizeButton = nullptr; // Iteration 15
    QSlider *m_seekBar;
    QLabel *m_timeLabel;
    bool m_previewMaximized = false; // Iteration 15
    QTimer *m_playbackTimer = nullptr;
    QTimer *m_seekTimer = nullptr;
    AudioMixer *m_mixer = nullptr;
    bool m_muted = false;

    AVFormatContext *m_formatCtx = nullptr;
    AVCodecContext *m_codecCtx = nullptr;
    SwsContext *m_swsCtx = nullptr;
    AVFrame *m_frame = nullptr;
    AVFrame *m_swFrame = nullptr;
    AVPacket *m_packet = nullptr;
    AVBufferRef *m_hwDeviceCtx = nullptr;
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    HdrInfo m_hdrInfo;
    int m_videoStreamIndex = -1;
    bool m_playing = false;
    int64_t m_durationUs = 0;
    int64_t m_currentPositionUs = 0;
    int64_t m_frameDurationUs = 0;
    double m_displayAspectRatio = 0.0;
    bool m_loggedCullState = false;
    QElapsedTimer m_tickWallStart;
    // Phase 1e Win #12 — Fix J: per-call play() debounce. scheduleNextFrame's
    // !advanced safety-net (VideoPlayer.cpp:2911-2923) calls pause() +
    // m_mixer->stop() the moment a tick can't decode/advance, which can fire
    // on the very first tick after play() during a cold-open. The UI sees
    // m_playing flip false→true→false and the user's single click can land
    // a re-play() callback while AudioMixer is still mid-stop/start. Empirical
    // log veditor_20260501_091710.log @ 09:20:18-19 captured 4 such play()
    // entries within 934 ms, each producing a sink Stopped→Active→Suspended→
    // Stopped cycle on the main thread (≈10 ms each), accumulating a 10.7 s
    // paintGL gap (#5600→#5700 at 09:20:14→09:20:24). Collapsing close-spaced
    // play() calls to one within a 200 ms window stops the cascade without
    // affecting deliberate user pause→play sequences (≥250 ms reaction).
    // Opt-out via VEDITOR_PLAY_DEBOUNCE_DISABLE=1 for empirical comparisons.
    QElapsedTimer m_lastPlayCallTimer;
    // Phase 1e Win #15 — Fix M: last-frame retention across same-tick clip
    // switches. advanceToEntry's loadFile call funnels into resetDecoder,
    // which historically nuked m_lastV1RawFrame and m_currentFrameImage
    // (VideoPlayer.cpp:1600 / :1643). The 60-145 ms window between the
    // clear and the next clip's first decoded frame paints a black/empty
    // GLPreview — visible as a flash at every entry boundary. When this
    // flag is set across a planned advanceToEntry/seekToTimelineUs hand-
    // off, resetDecoder skips those two clears so the previous clip's
    // last frame stays on screen until the new file's first frame
    // displaces it. Industry-standard cut behaviour (Premiere/DaVinci);
    // also gives the pending transition pipeline a frame to fade out
    // from instead of black. Opt-out via
    // VEDITOR_LAST_FRAME_RETENTION_DISABLE=1.
    bool m_retainLastFrameAcrossLoad = false;
    int m_canvasWidth = 1920;
    int m_canvasHeight = 1080;
    QSize m_projectOutputSize;
    bool m_pendingSizeRefresh = false;
    double m_playbackSpeed = 1.0;
    QImage m_currentFrameImage;
    int m_pendingSeekMs = -1;
    bool m_pendingSeekPrecise = false;
    bool m_seekInProgress = false;
    bool m_suppressUiUpdates = false; // guard against intermediate slider flashes during cross-file seeks
    // Phase 1d compositor: when true, presentDecodedFrame caches the V1
    // frame into m_lastSourceFrame but skips displayFrame so the compositor
    // step in handlePlaybackTick can blend overlays on top before pushing
    // the final image to the GLPreview.
    bool m_deferDisplayThisTick = false;
    // Tracks whether the previous tick took the compositor path. The
    // compositor forces the GL viewport to identity (1,0,0) so the active
    // entry's transform isn't applied twice on top of an already-baked
    // composite. The next non-composite tick has to undo that — push the
    // entry's own scale/dx/dy back into the GL viewport before
    // displayFrame ships the raw V1 frame, otherwise V1 snaps to identity
    // the moment the overlay ends.
    bool m_lastTickWasComposite = false;
    // After preview/seek completes the V2+ pool decoders may still hold
    // stale lastFrameRgb / currentPositionUs values. This flag asks the
    // next handlePlaybackTick to re-seed them by clearing firstFrameDecoded
    // so harvestOverlayLayer falls into its catch-up loop.
    bool m_postSeekResyncRequested = false;
    int m_lastDragMs = -1;
    int m_audioResyncTickCount = 0;
    qint64 m_audioUnmuteToken = 0;
    bool m_audioUnmuteScheduled = false;

    // Multi-clip sequence state. m_currentPositionUs remains FILE-LOCAL (so
    // the existing decoder loop is untouched); m_timelinePositionUs tracks the
    // resolved timeline position when sequence mode is active.
    QVector<PlaybackEntry> m_sequence;
    int m_activeEntry = -1;
    // Phase 1e Win #16 (Iteration 9) — boundary preroll de-dup. Set to the
    // sequence index of the entry we last asked acquireDecoderForClip to
    // warm up; cleared whenever m_activeEntry advances so the next entry's
    // preroll fires fresh. Without this, every playback tick within the
    // 1000 ms preroll window would re-call acquireDecoderForClip, which
    // re-hashes + slot-manager-refreshes 60 times a second and pollutes
    // the [pool] log. Opt-out via VEDITOR_BOUNDARY_PREROLL_DISABLE=1.
    int m_prerolledEntryIdx = -1;
    // Tracks the empty -> non-empty transition so setAudioSequence only
    // seeks the AudioMixer the first time a schedule arrives. The full
    // entry list is owned by the mixer; mirroring it here would just be
    // dead state.
    bool m_audioSequenceHadEntries = false;
    QVector<speedramp::SpeedRamp> m_speedRamps;  // parallel to m_sequence
    int64_t m_timelinePositionUs = 0;
    int64_t m_sequenceDurationUs = 0;
    QString m_loadedFilePath;
    bool m_textToolActive = false;
    // US-WIRE-3: region picker for motion tracking
    bool m_regionPickerActive = false;
    std::function<void(QRect)> m_regionPickerCallback;
    QWidget *m_regionPickerOverlay = nullptr;
    bool m_wbEyedropperActive = false;
    std::function<void(QColor)> m_wbEyedropperCallback;
    QWidget *m_wbEyedropperOverlay = nullptr;
    QVector<EnhancedTextOverlay> m_textOverlays;
    // Cached raw source of the most recent frame. Needed so
    // setHiddenTextOverlayIndex can re-compose while paused (the cached
    // m_currentFrameImage is already composited and can't be re-filtered).
    QImage m_lastSourceFrame;
    // Phase 1d compositor base: a never-composed copy of the latest V1
    // frame. m_lastSourceFrame may end up holding the post-composite image
    // through displayFrame's caching path; the compositor reads this
    // dedicated field instead so multi-track ticks never paint overlays
    // on top of a previously-composited frame.
    QImage m_lastV1RawFrame;
    // Persistent canvas-sized scratch buffer for the compositor.
    // Re-allocated only on canvas size / format change; refilled with
    // black every compositor tick. Avoids ~8MB ARGB allocations per tick
    // at 1080p which under playback cadence (60fps) translated to
    // hundreds of MB/s of allocator pressure.
    QImage m_canvasBase;
    // Overlay index to skip during compose (-1 = none), toggled by
    // GLPreview edit-started/ended signals.
    int m_hiddenTextOverlayIndex = -1;
    // Live-preview effect stack driven by the Video Effects dialog. Applied
    // on top of every composed frame while the dialog is open; empty in the
    // default/committed state.
    QVector<VideoEffect> m_previewEffects;
    bool m_previewEffectsLive = false;
    bool m_gpuEffectsEnabled = true;
    // Preview proxy divisor (1=Full, 2=1/2, 4=1/4, 8=1/8). Applied only when
    // CPU-path effects are active during playback, so heavy Sharpen/Mosaic/
    // ChromaKey stay smooth. Persisted via QSettings "proxyDivisor".
    int m_proxyDivisor = 1;
    // V3 sprint — Timeline clipSelected で更新する drag-edit target。
    // -1 = follow m_activeEntry (legacy)。 >= 0 = explicit sequence index。
    // 再生用の m_activeEntry とは独立。setSequence でリセット。
    int m_editTargetEntry = -1;

    // AR-2: ACES 色管理パイプライン (MainWindow SSOT のコピー)。既定 enabled=false。
    // displayFrame は enabled=true のときだけ最終合成結果へ適用するので、既定状態では
    // 既存のプレビュー出力とビット同一 (回帰ゼロ)。
    aces::AcesPipeline m_acesPipeline;
    bool m_lastFrameOdtApplied{false};

    // EXP-AID: 露出/フォーカス確認エイド。既定 None なので displayFrame は apply を
    // 一切呼ばず従来出力とビット同一 (性能無影響・回帰ゼロ)。None 以外のときだけ
    // displayFrame 内の 1 箇所で表示用 QImage の一時コピーへ適用する (キャッシュ /
    // 保持フレーム / 書き出しには非適用)。
    exposureaid::AidMode m_exposureAidMode = exposureaid::AidMode::None;
    safezone::Platform m_safeZonePlatform = safezone::Platform::None;  // SAFE-ZONE
    onionskin::Config m_onionSkin;  // ONION-SKIN: display-only、既定 OFF。
    int m_previewMaxLongSide = 0;  // PV-C: 0=無制限。display専用の長辺上限。
    exposureaid::AidConfig m_exposureAidConfig;

    // ---- ADAPTIVE-1: アダプティブプレビュー (品質ポリシー + 合成キャッシュ) --------
    // プレビュー再生パス専用。書き出し (RenderQueue / Exporter / renderFrameAt) は
    // 一切経由しないので、編集↔書き出しピクセル一致 (SSOT) は不変。
    //
    // m_qualityPolicy: 直近 tick の合成ウォール時間とフレーム予算を比べ、ヒステリシス
    //   付きで 1.0 → 0.5 → minScale の品質ラダーを上下する。停止時は reset() で 1.0。
    // m_frameCache: (timelineRevision, timeMs, w, h, tier, useProxy) をキーに最終
    //   合成 QImage を LRU 保持。同一プレイヘッド・同一編集状態での再描画
    //   (停止フリッカー修正で残った previewSeek 再描画 / リサイズ再描画) を
    //   再合成なしで返すための純キャッシュ。再生 tick の displayFrame 発火回数は
    //   増やさない (cache hit でも 1 tick = 最大 1 displayFrame を厳守)。
    // m_timelineRevision: 編集 / undo / クリップ変更で単調増加。setSequence で ++ し
    //   invalidateRevision を呼んで古い世代のキャッシュを破棄する。
    // m_lastTickRenderMs: 直近 tick の合成ウォール時間 (ms)。次 tick の policy 入力。
    // m_adaptiveCanvasDivisor: policy が返した scaleFactor 由来の追加キャンバス縮小
    //   係数 (1 / 2 / 4)。既存 canvasProxyDivisor() に乗算して合成解像度を下げる。
    // m_adaptivePreviewEnabled: 既定 ON。VEDITOR_ADAPTIVE_PREVIEW_DISABLE=1 で OFF
    //   (scaleFactor 強制 1.0 + キャッシュバイパス)。
    playback::PlaybackQualityPolicy m_qualityPolicy;
    playback::CompositeFrameCache   m_frameCache;
    quint64 m_timelineRevision = 1;
    double  m_lastTickRenderMs = 0.0;
    int     m_adaptiveCanvasDivisor = 1;
    bool    m_adaptivePreviewEnabled = true;
    // 直近の cache 採用 tier (qualityTier キー用)。scaleFactor を整数化したもの。
    int     m_adaptiveQualityTier = 0;
    // ADAPTIVE-1: tick 冒頭で policy に相談し m_adaptiveCanvasDivisor /
    // m_adaptiveQualityTier を更新する。停止/シーク経路では呼ばず policy.reset() を使う。
    void updateAdaptiveQuality();
    // ADAPTIVE-1: 合成済みプレビューフレームを現在の (timelineRevision, playhead,
    // size, tier) キーでキャッシュへ put する純ヘルパー。displayFrame は呼ばない。
    // m_adaptivePreviewEnabled=false 時は no-op。
    void cachePreviewComposite(const QImage &composed);

    // ---- STAGE3-GPU: マルチトラックプレビュー GPU 合成 (既定 OFF) -------------
    // 既定 (VEDITOR_GPU_COMPOSITE 未設定 or "1"以外) では m_gpuCompositeEnabled=false
    // となり、handlePlaybackTick の合成は従来 CPU 経路 (composeMultiTrackFrameInto
    // / composeMultiTrackFrame) を一切変えずに通る = 出力ビット同一。
    //
    // ON 時 (=="1") かつ 多トラック (overlay layers >= 2) かつ マット無し
    // (このプレビュー tick の DecodedLayer はマットフィールドを持たず常に matte-free)
    // かつ compositor.isAvailable() のとき、CPU と同じ layers / 同じ canvas サイズ
    // (適応縮小後) を GpuLayerInput に詰めて GpuLayerCompositor::composite() で
    // GPU 合成する。GPU が空 QImage を返したら (GL 失敗) 同 tick 内で CPU 経路へ
    // フォールバックするので、表示の displayFrame は経路に関わらず常に最大 1 回。
    //
    // UI スレッド専用 (GpuLayerCompositor は同一スレッド・非リエントラント契約)。
    // VideoPlayer は UI スレッドなので OK。書き出しパス (RenderQueue / Exporter /
    // renderFrameAt) は一切経由しない = 編集↔書き出しピクセル一致 (SSOT) は不変。
    bool                                m_gpuCompositeChecked = false;
    bool                                m_gpuCompositeEnabled = false;
    std::unique_ptr<GpuLayerCompositor> m_gpuCompositor;  // 遅延生成 (初回 GPU 合成時)
    // 初回 tick で VEDITOR_GPU_COMPOSITE / QSettings を 1 回だけ読み
    // m_gpuCompositeEnabled をキャッシュする。
    void ensureGpuCompositeFlag();
    // GPU 合成を試みる純ヘルパー。成功時に canvas-final な合成 QImage を返す。
    // 失敗 / 非対象 (flag OFF / 単一トラック / GL 不可) のときは null QImage を返し、
    // 呼び出し側は従来 CPU 経路へフォールバックする。displayFrame は呼ばない。
    QImage tryGpuComposeLayers(const QVector<DecodedLayer> &layers, QSize canvas);

    // ---- Per-clip decoder pool state (V2+ only) -----------------------------
    // Active V2+ decoders, keyed on TrackKey. V1 never lives here — it
    // stays on m_formatCtx/m_codecCtx.
    QHash<TrackKey, TrackDecoder*> m_trackDecoders;
    // Reverse lookup from DecoderSlotManager's int slotClipId back to the
    // TrackKey that originally claimed the slot. Without this, eviction
    // had to walk m_trackDecoders comparing qHash(it.key()) to
    // evictedClipId — a hash collision would surface the wrong decoder.
    // Maintained alongside m_trackDecoders insert/erase.
    QHash<int, TrackKey> m_slotIdToKey;
    // Bounded grace pool for evicted decoders. Each entry counts down
    // graceTtlTicks per playback tick; when it reaches 0 the decoder
    // is freed. Capped at 4 entries — newer evictions push older ones
    // straight to closeTrackDecoder.
    QVector<TrackDecoder*> m_evictionGracePool;
    DecoderSlotManager m_slotManager;
    // Shared HW device context used by every pool decoder. Created lazily
    // on the first openTrackDecoder() call. Kept distinct from the legacy
    // V1 m_hwDeviceCtx so V1 regression risk stays at zero.
    AVBufferRef *m_sharedPoolHwDeviceCtx = nullptr;
    AVPixelFormat m_sharedPoolHwPixFmt = AV_PIX_FMT_NONE;

    // Return a new QImage with any active text overlays drawn on top.
    // Active = startTime <= now < (endTime > 0 ? endTime : infinity),
    // using the current timeline seconds. Returns the source image
    // unmodified when the overlay list is empty.
    QImage composeFrameWithOverlays(const QImage &source) const;

    // VEDITOR_TICK_TRACE accumulators (Phase 1e Sprint US-1). Populated only
    // when tickTraceEnabled() is true; flushed and reset every 30 ticks.
    qint64 m_tickTraceWorkNs = 0;
    qint64 m_tickTraceDecodeNs = 0;
    qint64 m_tickTraceComposeNs = 0;
    qint64 m_tickTraceDriftNs = 0;
    int m_tickTraceSkipped = 0;
    int m_tickTraceCount = 0;
    void recordTickTrace(qint64 workNs);
};

// Free comparator used by the production sort (std::stable_sort in
// handlePlaybackTick) AND directly exercised by the S3-STACK predicate
// sub-assertion in src/main.cpp. Extracted from the inline lambda so a
// re-inversion of the comparator breaks the selftest loudly.
// Contract (V1-wins stacking, PlaybackTypes.h:34): V1 (sourceTrack==0) sorts
// AFTER higher tracks, so the compositor paints the highest track first
// (backmost) and V1 LAST (frontmost, ON TOP) — descending order matches
// renderFrameAt / trackmatte::composite / buildSpecialClipComposite.
// Wrapped in namespace clipstack to avoid GLOBAL-namespace ODR/symbol
// pollution; all call sites must qualify as clipstack::layerPaintOrderLess.
namespace clipstack {
bool layerPaintOrderLess(const VideoPlayer::DecodedLayer &a,
                         const VideoPlayer::DecodedLayer &b);
} // namespace clipstack
