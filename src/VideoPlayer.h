#pragma once

#include <QWidget>
#include <QLabel>
#include <QImage>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "AudioMixer.h"
#include <QVector>
#include <QHash>
#include <QRectF>
#include "VideoEffect.h"
#include "PlaybackTypes.h"
#include "TextManager.h"
#include "DecoderSlotManager.h"

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
    // Audio-side schedule routed to AudioMixer. The mixer owns its own
    // FFmpeg decoder pool + ring buffers and mixes every active entry into
    // a single QAudioSink output, so unlinked J-cut/L-cut clips and stacked
    // A2/A3/... tracks all sound simultaneously.
    void setAudioSequence(const QVector<PlaybackEntry> &entries);
    AudioMixer *audioMixer() { return m_mixer; }
    void setCanvasSize(int width, int height);
    void setColorCorrection(const ColorCorrection &cc);
    // Transient effect stack applied on top of every composed frame (live dialog preview).
    // Empty vector disables the path. Does not mutate timeline state.
    // live=true keeps CPU effects active during playback (committed state).
    // live=false (default) is pause-only — used during dialog editing so
    // heavy per-frame work doesn't stutter slider drags.
    void setPreviewEffects(const QVector<VideoEffect> &effects, bool live = false);
    bool isGLAccelerated() const { return m_useGL; }
    void setGLAcceleration(bool enabled);

    // Read-only accessor used by MainWindow when retargeting the sequence
    // (proxy generation finished, proxy mode toggled). Wrapping the swap in
    // pause/play stops the mixer and the frame timer from
    // running through the loadFile reset, which previously surfaced as a
    // visible rewind + speed-up when the path resolution changed underneath
    // an active playback.
    bool isPlaying() const { return m_playing; }

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
    GLPreview *glPreview() const { return m_glPreview; }

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

signals:
    void positionChanged(double positionSeconds);
    void durationChanged(double durationSeconds);
    void stateChanged(bool playing);
    void playbackSpeedChanged(double speed);
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
    bool seekInternal(int64_t positionUs, bool displayFrame, bool precise);
    bool decodeNextFrame(bool displayFrame);
    bool presentDecodedFrame(AVFrame *frame, bool displayFrame);
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
        // > 0 means this decoder has been moved into the eviction grace
        // pool. Decremented per playback tick; reaches 0 → safe to free
        // (deferred outside decode loop).
        int graceTtlTicks = 0;
    };

    TrackDecoder *acquireDecoderForClip(const PlaybackEntry &entry);
    void releaseDecoderForClip(const TrackKey &key);

    // ---- Phase 1d software compositor ---------------------------------------
    // One overlay layer harvested from a V2+ TrackDecoder for the current
    // tick. composeMultiTrackFrame paints these on top of the V1 frame in
    // m_sequence order (V1 base + V2/V3/... above), with isFresh=false
    // entries falling back to the previous decoded frame from the eviction
    // grace pool when a re-seek hasn't caught up yet.
    struct DecodedLayer {
        QImage rgb;
        double opacity = 1.0;
        double videoScale = 1.0;
        double videoDx = 0.0;
        double videoDy = 0.0;
        int sourceTrack = 0;
        int sequenceIdx = -1;
        bool isFresh = true;
    };

    QImage composeMultiTrackFrame(const QImage &v1Frame,
                                  const QVector<DecodedLayer> &overlayLayers) const;
    bool harvestOverlayLayer(const PlaybackEntry &entry, int seqIdx, DecodedLayer *out);
    bool decodePoolFrame(TrackDecoder *d);
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
    QSlider *m_seekBar;
    QLabel *m_timeLabel;
    QTimer *m_playbackTimer = nullptr;
    QTimer *m_seekTimer = nullptr;
    AudioMixer *m_mixer = nullptr;

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
    int m_canvasWidth = 1920;
    int m_canvasHeight = 1080;
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
    // Tracks the empty -> non-empty transition so setAudioSequence only
    // seeks the AudioMixer the first time a schedule arrives. The full
    // entry list is owned by the mixer; mirroring it here would just be
    // dead state.
    bool m_audioSequenceHadEntries = false;
    int64_t m_timelinePositionUs = 0;
    int64_t m_sequenceDurationUs = 0;
    QString m_loadedFilePath;
    bool m_textToolActive = false;
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
    // Preview proxy divisor (1=Full, 2=1/2, 4=1/4, 8=1/8). Applied only when
    // CPU-path effects are active during playback, so heavy Sharpen/Mosaic/
    // ChromaKey stay smooth. Persisted via QSettings "proxyDivisor".
    int m_proxyDivisor = 1;

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
};
