#pragma once

#include <QWidget>
#include <QLabel>
#include <QImage>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVector>
#include <QRectF>
#include "VideoEffect.h"
#include "PlaybackTypes.h"
#include "TextManager.h"

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
    // Audio-side schedule (A1 only for MVP). Drives m_audioPlayer independently
    // from the video schedule — this is what makes unlinked J-cut / L-cut
    // clips actually emit audio at their post-drag timing.
    void setAudioSequence(const QVector<PlaybackEntry> &entries);
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
    // pause/play stops the audio side-player and the frame timer from
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
    bool audioSequenceActive() const { return !m_audioSequence.isEmpty(); }
    int findActiveEntryAt(int64_t timelineUs) const;
    int findActiveAudioEntryAt(int64_t timelineUs) const;
    void applyAudioEntryAtTimeline(int64_t timelineUs, bool forceSourceReload, bool forceReposition);
    int64_t entryLocalPositionUs(int entryIdx, int64_t timelineUs) const;
    int64_t fileLocalToTimelineUs(int entryIdx, int64_t fileLocalUs) const;
    bool seekToTimelineUs(int64_t timelineUs, bool precise);
    bool advanceToEntry(int newEntryIdx);
    void applySequenceSliderRange();
    int sliderTimelinePosition(int64_t timelineUs) const;
    void applyActiveEntryAudioMute();
    // A/V drift correction: when the audio side-player is reading the same
    // file as the active video entry, treat the audio clock as master and
    // skip-decode extra video frames if the video PTS has fallen behind.
    // No-op for J-cut/L-cut (audio file != video file) and for reverse
    // playback. Returns the number of extra frames actually skip-decoded.
    int correctVideoDriftAgainstAudioClock();

    QLabel *m_videoDisplay;
    GLPreview *m_glPreview = nullptr;
    bool m_useGL = true;
    QPushButton *m_proxyButton = nullptr;
    QPushButton *m_stepBackButton = nullptr;
    QPushButton *m_stepFwdButton = nullptr;
    QPushButton *m_playButton;
    QPushButton *m_pauseButton;
    QPushButton *m_stopButton;
    QSlider *m_seekBar;
    QLabel *m_timeLabel;
    QTimer *m_playbackTimer = nullptr;
    QTimer *m_seekTimer = nullptr;
    QMediaPlayer *m_audioPlayer = nullptr;
    QAudioOutput *m_audioOut = nullptr;

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
    int m_lastDragMs = -1;
    int m_audioResyncTickCount = 0;
    qint64 m_audioUnmuteToken = 0;
    bool m_audioUnmuteScheduled = false;

    // Multi-clip sequence state. m_currentPositionUs remains FILE-LOCAL (so
    // the existing decoder loop is untouched); m_timelinePositionUs tracks the
    // resolved timeline position when sequence mode is active.
    QVector<PlaybackEntry> m_sequence;
    int m_activeEntry = -1;
    QVector<PlaybackEntry> m_audioSequence;
    int m_activeAudioEntry = -1;
    QString m_audioLoadedFilePath;  // current m_audioPlayer source, tracked separately from m_loadedFilePath
    // Qt MediaFoundation setSource() is asynchronous on Windows — setPosition
    // and play() calls issued before LoadedMedia fires are silently dropped,
    // leaving the audio side-player muted after a file switch. We stash the
    // desired position and play-intent here and re-apply them when the
    // QMediaPlayer::mediaStatusChanged signal reports LoadedMedia.
    qint64 m_pendingAudioPositionMs = -1;
    bool m_pendingAudioPlay = false;
    int64_t m_timelinePositionUs = 0;
    int64_t m_sequenceDurationUs = 0;
    QString m_loadedFilePath;
    bool m_textToolActive = false;
    QVector<EnhancedTextOverlay> m_textOverlays;
    // Cached raw source of the most recent frame. Needed so
    // setHiddenTextOverlayIndex can re-compose while paused (the cached
    // m_currentFrameImage is already composited and can't be re-filtered).
    QImage m_lastSourceFrame;
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

    // Return a new QImage with any active text overlays drawn on top.
    // Active = startTime <= now < (endTime > 0 ? endTime : infinity),
    // using the current timeline seconds. Returns the source image
    // unmodified when the overlay list is empty.
    QImage composeFrameWithOverlays(const QImage &source) const;
};
