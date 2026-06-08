#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QFileInfo>
#include <QVector>
#include <QHash>
#include <QMenu>
#include <QElapsedTimer>
#include "VideoEffect.h"
#include "Keyframe.h"
#include "WaveformGenerator.h"
#include "TextManager.h"
#include "PlaybackTypes.h"
#include "Overlay.h"
#include "TrackMatteKey.h"
#include "SnapEngine.h"
#include "MarkerData.h"
#include "SpeedRampData.h"
#include "color/ClipColor.h"
#include "AdjustmentLayer.h"
#include "MotionStabilizer.h"
#include "Camera3D.h"
#include "MotionSectionWidget.h"
#include "MaskSystem.h"      // S7: per-clip mask container (additive seam)
#include "MotionTracker.h"   // S7: per-clip tracker data animating the mask

// Where Timeline::addClip drops a freshly-imported clip. Persisted via
// QSettings('VSimpleEditor','Preferences')/importPlacement; the MainWindow
// preference menu toggles between the two values.
enum class ImportPlacement {
    ParallelTrack = 0,      // Premiere-style: first empty V/A track (V2, V3, ...)
    AppendToFirstTrack = 1  // Append to the end of V1/A1 as a continuous sequence
};

// Persisted via QSettings('VSimpleEditor','Preferences')/autoProxyMode.
// Decides whether Timeline::addClip auto-enqueues proxy generation for
// heavy sources (AV1 or QHD+). VEDITOR_AUTO_PROXY_DISABLE=1 still acts as
// a global kill switch regardless of the value chosen here.
enum class AutoProxyMode {
    Disabled = 0,         // Never auto-generate; user runs ツール → プロキシ管理 manually
    MultiTrackOnly = 1,   // Default: only when the clip lands on V2 or later
    Always = 2            // Generate on every heavy import regardless of track
};

class AudioMixer;
class UndoManager;
class Timeline;
class PlayheadOverlay;
class TextStripWidget;
struct TimelineState;
class MarkerManager;
class MarkerLane;  // 16px-tall lane painting Marker triangles above the time ruler
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QTimer;

// TR-3: トリム種別。実体は src/TrimOps.h。TrimOps.h は ClipInfo のために
// この Timeline.h を #include するので、ここで逆 include すると循環参照に
// なる。enum class は前方宣言できないため namespace + enum 種別だけを
// 前方宣言し、シグネチャに使う (Timeline.cpp 側で TrimOps.h を実 include)。
namespace trimops { enum class TrimType; }

struct ClipInfo {
    QString filePath;
    QString displayName;
    double duration;
    double inPoint = 0.0;
    double outPoint = 0.0;
    double leadInSec = 0.0; // leading gap before the clip on the timeline, grows on left-trim to keep the right edge fixed
    double speed = 1.0;   // 0.25x - 4.0x
    double volume = 1.0;  // 0.0 - 2.0 (0=mute, 1=normal, 2=boost)
    // Pro-NLE "rubber band" volume automation. Each point is (clip-local
    // seconds, gain). Empty vector = static `volume` for the whole clip.
    // Sorted by .time; AudioMixer evaluates via linear interpolation.
    QVector<AudioGainPoint> volumeEnvelope;
    // Non-zero → clip is linked with every other clip that shares the same
    // linkGroup. Linked clips are selected together, dragged together, and
    // deleted together so V/A stays in AV sync. 0 = unlinked / standalone.
    int linkGroup = 0;

    // US-T35 per-clip OBS-style video source transform. scale=1.0, dx=dy=0
    // is identity (no transform). dx/dy are offsets in fractions of the
    // letterbox width/height, scale is uniform.
    double videoScale = 1.0;
    double videoDx = 0.0;
    double videoDy = 0.0;
    double rotation2DDegrees = 0.0;
    bool is3DLayer = false;
    Layer3DTransform layer3D;

    // Future multi-track compositing groundwork. 1.0 = opaque (current
    // V1-wins behaviour). <1.0 values are placeholders until the layered
    // compositor lands in a follow-up iteration.
    double opacity = 1.0;

    // SNS vertical fit: when true and the project output aspect differs from
    // this clip's native frame, export/previews may pre-contain the frame into
    // a transparent same-aspect intermediate canvas before clipgeom placement.
    bool fitContain = false;
    bool fitCover = false;

    // HDR Stage1: per-clip input color metadata. Default SDR is deliberately
    // inert and is omitted from project JSON to preserve old files byte-for-byte.
    clipcolor::ColorMeta colorMeta;

    // Phase 3: Color correction, effects, keyframes
    ColorCorrection colorCorrection;
    QVector<VideoEffect> effects;
    KeyframeManager keyframes;

    // S4 (NLE-parity SSOT): per-clip 3D-LUT reference. The GPU preview's LUT
    // (GLPreview::setLut, src/GLPreview.cpp:3229) is a single global texture,
    // but renderFrameAt must grade every clip independently so an export
    // pixel-matches the preview when a graded clip is on screen. There was no
    // per-clip LUT storage anywhere (verified: zero ClipInfo / Timeline /
    // ProjectFile references), so this is the minimal purely-additive seam:
    // both members default to "no LUT" (empty path) and are read ONLY by the
    // SSOT renderer + the S4 selftest — no existing code path changes.
    QString lutFilePath;          // empty == no LUT on this clip
    double  lutIntensity = 1.0;   // 0.0 = original, 1.0 = full LUT (matches
                                  // GLPreview uLutIntensity / LutData::intensity)

    // True when this clip carries a 3D LUT to apply. Mirrors the
    // ColorCorrection::isDefault() gating idiom Exporter uses
    // (src/Exporter.cpp:473-474) so the SSOT renderer can cheaply skip
    // un-graded clips and stay byte-identical to S2/S3.
    bool hasLut() const { return !lutFilePath.isEmpty(); }

    // S7 (NLE-parity SSOT): per-clip compositing mask + the motion-tracker
    // data that animates it. This is the SAME minimal purely-additive seam
    // pattern as the S4 lutFilePath member above: there was NO per-clip mask
    // storage anywhere (verified — zero ClipInfo / Timeline / ProjectFile
    // references to a per-clip MaskSystem), and the GPU preview's US-EF-2
    // "Power Window" is a single GLOBAL grade-localisation uniform set
    // (GLPreview::setMask, src/GLPreview.cpp:3427), not a per-clip alpha
    // matte. But renderFrameAt must apply each clip's genuine AE/Premiere-
    // style compositing mask (MaskSystem::applyMask — multiplies the layer's
    // alpha) independently so an export pixel-matches a masked clip. Both
    // members default to "no mask" (empty MaskSystem / empty TrackingResult)
    // and are read ONLY by the SSOT renderer + the S7 selftest. ProjectFile
    // clipToJson/clipFromJson never touch them (exactly like lutFilePath),
    // so on-disk project serialisation is byte-identical.
    MaskSystem    maskSystem;        // empty masks() == no mask on this clip
    TrackingResult maskTrackingData; // empty == static mask (no animation)

    // True when this clip carries at least one mask. Mirrors the hasLut()
    // gating idiom so the SSOT renderer can cheaply skip un-masked clips
    // and stay byte-identical to S2..S6.
    bool hasMask() const { return !maskSystem.masks().isEmpty(); }

    // Phase 5: Waveform
    WaveformData waveform;

    // Edge-attached transitions. leadIn renders at the start of this clip
    // (e.g., FadeIn from black, or CrossDissolve from the previous clip);
    // trailOut renders at the end (FadeOut to black, or CrossDissolve to
    // the next clip). type=None means no transition on that edge.
    Transition leadIn;
    Transition trailOut;

    // Phase 6: Enhanced text overlays
    TextManager textManager;

    // Per-clip variable-speed curve. Default = identity (single keyframe at
    // 0us, speed 1.0). When non-identity, consumers (VideoPlayer / AudioMixer)
    // must walk the curve via timelineToSourceUs to map timeline ticks to
    // source PTS. Consumer wiring is deferred — see SpeedRampData.h header
    // comment for the integration sites.
    speedramp::SpeedRamp speedRamp = speedramp::SpeedRamp::identity();
    bool atempoEnabled = false;

    // US-INT-4: per-frame stabilization transform keyframes baked by the
    // 編集 > スタビライズ slot. Empty = identity (no stabilization). GLPreview
    // looks up the active source-time via std::lower_bound and applies the
    // INVERSE 2D affine, composed with the user 3D-rotate matrix.
    QVector<StabilizerKeyframe> stabilizerKeyframes;

    double effectiveDuration() const {
        double out = (outPoint > 0.0) ? outPoint : duration;
        return (out - inPoint) / speed;
    }
};

enum class DragMode {
    None,
    TrimLeft,
    TrimRight,
    MoveClip,
    TransitionLeadInResize,
    TransitionTrailOutResize
};

// TM-8: intrinsic track-matte wiring carried BY the Timeline so the SSOT
// renderFrameAt (src/TimelineFrameRenderer.cpp) no longer reaches into a
// live MainWindow off the worker thread for matte data. This mirrors the
// 2 fields the consumer actually reads (matteType + matteSourceClipId);
// it is deliberately NOT ProjectFile.h's TrackMatteClipEntry because
// ProjectFile.h #includes Timeline.h (a cycle would form). TrackMatteType
// is already visible here via the existing MaskSystem.h include above.
struct TimelineTrackMatteEntry {
    TrackMatteType matteType = TrackMatteType::None;
    QString matteSourceClipId;   // "trackIdx:clipIdx" of the matte source clip
};

class TimelineTrack : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineTrack(QWidget *parent = nullptr);

    void addClip(const ClipInfo &clip);
    void insertClip(int index, const ClipInfo &clip);
    void removeClip(int index);
    void moveClip(int fromIndex, int toIndex);
    void splitClipAt(int index, double localSeconds);
    // TR-3: 純粋エンジン trimops::applyTrim を clip[clipIndex] に適用する。
    // 成功時は split/insert と同じ後処理 (updateMinimumWidth/update/emit
    // modified) を行い true を返す。失敗 (不正 index / 境界違反) 時は
    // m_clips 不変で false を返し、errorOut に日本語の理由を書き込む。
    bool applyTrim(int clipIndex, trimops::TrimType type, double deltaSec,
                   QString *errorOut = nullptr);
    int clipCount() const { return m_clips.size(); }
    const QVector<ClipInfo> &clips() const { return m_clips; }
    void setClips(const QVector<ClipInfo> &clips);

    void setSelectedClip(int index);
    void toggleClipSelection(int index);
    void clearClipSelection();
    void moveSelectedClipsGroup(int targetIndex);
    int selectedClip() const { return m_selectedClips.isEmpty() ? -1 : m_selectedClips.last(); }
    const QList<int> &selectedClips() const { return m_selectedClips; }
    bool isClipSelected(int index) const { return m_selectedClips.contains(index); }

    int clipAtX(int x) const;
    double xToSeconds(int x) const;
    int secondsToX(double seconds) const;
    int clipStartX(int index) const;

    // Remove clip[index] and push its freed time (leadInSec + effectiveDur)
    // into clip[index+1]'s leadInSec so downstream clips keep their absolute
    // timeline positions. Used by cross-track drag.
    void removeClipPreservingDownstream(int index);
    // Insert clip at (index), with a specific leadInSec, then subtract the
    // inserted clip's footprint from clip[index+1]'s leadInSec so downstream
    // clips don't slide right. Caller must have verified the plan fits.
    void insertClipPreservingDownstream(int index, const ClipInfo &clip, double leadInSec);

    // SM-3: ソースモニター3点編集の実行口。純粋エンジン threepoint:: の計算結果を
    // 既存の split/remove/insert プリミティブで適用する。
    // Insert = 既存クリップを右へ押し出して timelineStartSec に割り込む (ripple)。
    void insertClip3Point(double timelineStartSec, const ClipInfo &clip);
    // Overwrite = [T, T+L) を上書きし、跨ぐクリップを分割・収まるクリップを削除して
    // 新クリップを T 開始に配置する。
    void overwriteClip3Point(double timelineStartSec, const ClipInfo &clip);

    // TB-3: タイムラインの時間範囲 [startSec, endSec) をリップル削除する。
    // 範囲境界で splitClipAt して跨ぐクリップを分割し、範囲内に完全に収まる
    // クリップを削除した後、後続クリップ全体を削除長ぶん左へ詰める (ripple)。
    // 文字起こし駆動編集 (textedit::deletionRanges) の各削除区間を適用する口。
    // 空 / 範囲外 / startSec>=endSec は安全に no-op。既存の点/挿入/上書き挙動は
    // 壊さず、insertClip3Point/overwriteClip3Point と同じプリミティブを再利用する。
    // 戻り値はトラック内容または leadInSec が実際に変わったか。
    bool rippleDeleteTimeRange(double startSec, double endSec);

    struct DropPlan {
        bool valid = false;
        int insertIdx = -1;
        double newLeadIn = 0.0;
        double nextLeadInDelta = 0.0;
    };
    DropPlan planDrop(double dropTime, double clipDuration) const;

    void setSnapEnabled(bool enabled) { m_snapEnabled = enabled; }
    bool snapEnabled() const { return m_snapEnabled; }
    void setPixelsPerSecond(double pps);
    double pixelsPerSecond() const { return m_pixelsPerSecond; }
    void setRowHeight(int h);
    int rowHeight() const { return m_rowHeight; }
    void setMuted(bool muted) { m_muted = muted; update(); }
    bool isMuted() const { return m_muted; }
    void setSolo(bool solo) { m_solo = solo; update(); }
    bool isSolo() const { return m_solo; }
    void setHidden(bool hidden) { m_hidden = hidden; update(); }
    bool isHidden() const { return m_hidden; }

    static constexpr int SNAP_THRESHOLD = 8;

    // Edit lock. When locked, mousePressEvent early-returns before entering
    // any drag/trim/split mode and cross-track drops into this track are
    // rejected. The lock is a pure editing guard — playback is unaffected.
    void setLocked(bool locked) { m_locked = locked; update(); }
    bool isLocked() const { return m_locked; }

    // Marks this widget as an audio row so the volume-envelope overlay
    // (rubber band + draggable points) is only drawn / hit-tested on
    // audio tracks. Set by Timeline at track-creation time.
    void setIsAudioTrack(bool isAudio) { m_isAudioTrack = isAudio; update(); }
    bool isAudioTrack() const { return m_isAudioTrack; }

    // Global toggle for the per-clip volume envelope edit mode. When ON,
    // audio rows draw the envelope and accept Alt+click / drag / Alt+right
    // to add / move / remove points. The flag is process-wide so a single
    // menu action in MainWindow flips every audio track at once.
    static void setEnvelopeEditMode(bool on);
    static bool envelopeEditMode();

    // Quietly mutate a clip's leadInSec (and optionally its next clip's
    // leadInSec) during a drag coordinated at the Timeline level. Does NOT
    // emit modified(); the drag owner batches a single save-undo on release.
    void applyDragMove(int clipIdx, double leadIn, double nextLeadIn);

    void setTimeline(Timeline *t) { m_timeline = t; }

signals:
    void clipClicked(int index);
    void selectionChanged(int primaryIndex, bool additive);
    void emptyAreaClicked();
    void clipMoved(int fromIndex, int toIndex);
    void modified();
    // Fired only at the end of a discrete user interaction (drag, trim,
    // transition handle resize, envelope point edit). Timeline listens and
    // calls saveUndoState(description) so Ctrl+Z reverts the WHOLE
    // interaction, not the previous explicit-action snapshot. Distinct
    // from `modified()` (which is a fan-out for any pixel change and
    // would push hundreds of undo entries during a single drag).
    void interactionCompleted(QString description);
    void seekRequested(double seconds);
    void rowHeightChanged(int newHeight);
    void clipContextMenuRequested(int clipIndex, const QPoint &globalPos);
    void linkedDragStarted(int clipIndex);
    void linkedDragDelta(int clipIndex, double deltaSec);
    void linkedDragCancelled();
    void crossTrackDropped(int clipIndex, int linkGroup, double dropTime);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void updateMinimumWidth();
    int snapToEdge(int x) const;
    bool tryHitTrimEdge(QMouseEvent *ev, int clipIndex, const QRectF &clipRect);
    bool tryHitTransitionDragHandle(QMouseEvent *ev, int clipIndex, const QRectF &clipRect);
    bool tryHitEnvelopeKeyframe(QMouseEvent *ev, int clipIndex);
    bool tryHitTransitionBadge(QMouseEvent *ev, int clipIndex);
    void handleBodyClick(QMouseEvent *ev, int clipIndex);

    QVector<ClipInfo> m_clips;
    QList<int> m_selectedClips;
    DragMode m_dragMode = DragMode::None;
    int m_dragClipIndex = -1;
    int m_dragStartX = 0;
    double m_dragOriginalValue = 0.0;
    double m_dragOriginalLeadIn = 0.0;
    // Captured at the start of a transition badge handle drag so we can
    // compute new duration = original + (delta px / pps) without drift.
    double m_dragOriginalTransitionDuration = 0.0;
    // leadInSec of the clip RIGHT AFTER the dragged one, captured at drag
    // start. Used to compensate its gap during a MoveClip drag so downstream
    // clips don't slide when the user repositions a single clip. -1 = no next.
    double m_dragOriginalLeadInNext = -1.0;
    // Screen-X of the dragged clip's left edge at drag start, used for snap.
    int m_dragOriginalClipStartX = 0;
    bool m_snapEnabled = true;
    int m_dropTargetIndex = -1;

    double m_pixelsPerSecond = 10.0;
    bool m_muted = false;
    bool m_locked = false;
    bool m_solo = false;
    bool m_hidden = false;
    bool m_isAudioTrack = false;
    // Volume-envelope drag state. -1 = no drag in progress.
    int m_envelopeDragClipIdx = -1;
    int m_envelopeDragPointIdx = -1;
    int m_rowHeight = 50; // adjustable per-track via setRowHeight
    bool m_resizingHeight = false;
    int m_resizeStartY = 0;
    int m_resizeStartHeight = 0;
    // Cross-track drop preview state. -1 = no indicator. When >=0 this is
    // the widget-local x where the dropped clip's left edge would land.
    int m_dropIndicatorX = -1;
    bool m_dropIndicatorValid = false;
    static constexpr int TRIM_HANDLE_WIDTH = 6;
    static constexpr int RESIZE_HANDLE_HEIGHT = 6;
    static constexpr int CROSS_TRACK_DRAG_THRESHOLD = 10;
    Timeline *m_timeline = nullptr;
};

class Timeline : public QWidget
{
    Q_OBJECT

public:
    explicit Timeline(QWidget *parent = nullptr);

    void addClip(const QString &filePath);
    void splitAtPlayhead();
    void deleteSelectedClip();
    void rippleDeleteSelectedClip();
    bool hasSelection() const;

    // Copy / Paste
    void copySelectedClip();
    void pasteClip();
    bool hasClipboard() const { return m_clipboard.has_value(); }

    // Undo / Redo
    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;

    // Snap
    void setSnapEnabled(bool enabled);
    bool snapEnabled() const;
    qint64 findSnapTarget(qint64 candidateUs, int *outScreenX) const;
    void triggerSnapLine(int screenX);
    int snapLineX() const;
    int snapLineAlpha() const;

    // SnapEngine
    SnapEngine &snapEngine() { return m_snapEngine; }
    const SnapEngine &snapEngine() const { return m_snapEngine; }

    // Track access for SnapEngine collection
    const QVector<TimelineTrack *> &videoTracks() const { return m_videoTracks; }
    const QVector<TimelineTrack *> &audioTracks() const { return m_audioTracks; }

    // Marker integration
    void setMarkerManager(MarkerManager *mm) { m_markerManager = mm; }
    MarkerManager *markerManager() const { return m_markerManager; }

    // --- Timeline markers (Premiere Pro / DaVinci Resolve parity) ---
    // Independent of the legacy MarkerManager/SnapEngine pipeline. Owns a
    // monotonic id counter so ids survive save/load and can be referenced
    // from a future markers-panel UI without churn. Spec acceptance #1-4.
    int addMarker(qint64 timelineUs, const QString &label,
                  QColor color = QColor(QStringLiteral("#ff5050")));

    // PV-B: クリップ操作の公開エントリ(クリップ右クリックメニューと
    // プレビュー右クリックメニューで共有する SSOT)。
    void applySnsFitToClip(TimelineTrack *track, int clipIndex,
                           bool contain, bool cover, const QString &undoLabel);
    void applySilenceCutToClip(TimelineTrack *track, int clipIndex);
    void applyBeatMarkersToClip(TimelineTrack *track, int clipIndex);
    // 再生ヘッド直下の V1 クリップを解決(見つかれば true)。
    bool clipUnderPlayhead(TimelineTrack *&outTrack, int &outClipIndex) const;
    bool removeMarker(int id);
    bool updateMarker(int id, const Marker &updated);
    // MK-1: turn a point marker into a span (duration) marker, or back to a
    // point (durationUs == 0). Premiere "duration marker" parity. Finds the
    // marker by id, writes durationUs, repaints the lane and emits
    // markersChanged(). No-op if no marker with that id exists.
    void setMarkerDuration(int markerId, qint64 durationUs);
    Marker markerById(int id) const;
    const QVector<Marker> &markers() const { return m_markersData; }
    QVector<Marker> markersInRange(qint64 startUs, qint64 endUs) const;
    int nextMarkerAfter(qint64 timelineUs) const;   // returns id, -1 if none
    int prevMarkerBefore(qint64 timelineUs) const;  // returns id, -1 if none
    // Replace the entire marker list (used on project load). Resets the
    // monotonic id counter to max(existing ids)+1 so new markers don't
    // collide with serialized ids.
    void setMarkers(const QVector<Marker> &markers);

    // --- Adjustment layers (Premiere/Photoshop parity) ---
    // Stored unsorted; ordering is dictated by the layer's `trackIndex`
    // and stack position which composeAdjustmentLayersAt() resolves at
    // render time. Mutations emit adjustmentLayersChanged() so future
    // panel UI re-syncs without manual hooks.
    int addAdjustmentLayer(const AdjustmentLayer &layer);   // returns id
    bool removeAdjustmentLayer(int id);
    bool updateAdjustmentLayer(int id, const AdjustmentLayer &layer);
    const QVector<AdjustmentLayer> &adjustmentLayers() const { return m_adjustmentLayers; }
    AdjustmentLayer adjustmentLayerById(int id) const;
    // Replace the entire adjustment layer list (used on project load).
    // Resets the monotonic id counter to max(existing ids)+1.
    void setAdjustmentLayers(const QVector<AdjustmentLayer> &layers);

    // AudioMixer integration for undo/restore of track gains
    void setAudioMixer(AudioMixer *mixer) { m_audioMixer = mixer; }
    AudioMixer *audioMixer() const { return m_audioMixer; }

    // Zoom
    void zoomIn();
    void zoomOut();
    void setZoomLevel(double pixelsPerSecond);

    // I/O markers
    void markIn();
    void markOut();
    double markedIn() const { return m_markIn; }
    double markedOut() const { return m_markOut; }
    bool hasMarkedRange() const { return m_markIn >= 0 && m_markOut > m_markIn; }

    // Multi-track
    void addVideoTrack();
    void addAudioTrack();
    // Force every audio row to repaint. Used after global UI state changes
    // (e.g. the volume-envelope edit-mode toggle) so the overlay flips
    // visibility on every track at once.
    void repaintAudioTracks();
    // Pro-NLE-style auto-ducking. Treats every clip on `voiceTrackIdx` as
    // a "voice" range and writes a per-clip volumeEnvelope on every OTHER
    // audio track that overlaps the range so the BGM dips by `duckGain`
    // (linear) for the duration of each voice clip, ramping in/out over
    // attack/release seconds. Silently no-ops if voiceTrackIdx is out of
    // range or that track has no clips.
    void applyDuckingFromTrack(int voiceTrackIdx,
                               double duckGain = 0.25,
                               double attackSec = 0.20,
                               double releaseSec = 0.40);
    int videoTrackCount() const { return m_videoTracks.size(); }
    int audioTrackCount() const { return m_audioTracks.size(); }

    // Track row height (applied to all tracks AND their header widgets)
    void setTrackHeight(int h);
    int trackHeight() const { return m_trackHeight; }
    void increaseTrackHeight();
    void decreaseTrackHeight();

    // Clip speed & volume
    void setClipSpeed(double speed);
    void setClipVolume(double volume);

    // Per-clip speed-ramp (variable speed curve). Operates on V1 by clip
    // index. Returns the identity ramp for invalid indices so callers can
    // safely query without bounds-checking. setSpeedRamp re-emits the
    // playback sequences so VideoPlayer / AudioMixer pull the new ramp on
    // their next pass.
    const speedramp::SpeedRamp &speedRamp(int clipIndex) const;
    void setSpeedRamp(int clipIndex, const speedramp::SpeedRamp &ramp);

    // Phase 3: Color correction, effects, keyframes
    void setClipColorCorrection(const ColorCorrection &cc);
    // Attach a transition to the currently selected clip. FadeIn writes to
    // the clip's leadIn slot (start-of-clip); every other type writes to
    // trailOut (end-of-clip / boundary to next clip).
    void applyTransitionToSelected(const Transition &t);
    // Reset both leadIn and trailOut on the selected clip back to None.
    void clearTransitionsOnSelected();
    void setClipEffects(const QVector<VideoEffect> &effects);
    void setClipKeyframes(const KeyframeManager &km);
    ColorCorrection clipColorCorrection() const;
    QVector<VideoEffect> clipEffects() const;
    KeyframeManager clipKeyframes() const;
    double selectedClipDuration() const;
    // Index of the selected clip on V1 (delegates to m_videoTrack); -1 if none.
    int selectedVideoClipIndex() const;

    // SM-3: アクティブ動画トラック (先頭 V1) へ委譲する 3点編集ラッパー。
    // ソースモニターから組み立てた ClipInfo を timelineStartSec に Insert / Overwrite で
    // 配置し、saveUndoState で 1 操作 = 1 Undo にまとめる。
    void insertClip3PointActive(double timelineStartSec, const ClipInfo &clip);
    void overwriteClip3PointActive(double timelineStartSec, const ClipInfo &clip);

    // TB-3: アクティブ動画トラック (m_activeVideoTrackIndex、無ければ先頭 V1) の
    // タイムライン時間範囲 [startSec, endSec) をリップル削除する薄いラッパー。
    // 文字起こし駆動編集ダイアログが複数の削除区間を適用する際は、インデックス/
    // 時刻ズレを避けるため呼び出し側が降順 (後ろの区間から) で呼ぶ前提でよいが、
    // 単一区間の正しさはここで保証する。1 操作 = 1 Undo (saveUndoState)。
    void rippleDeleteTimeRangeActive(double startSec, double endSec);

    // TR-3: アクティブ動画トラックの現在選択中クリップへ trimops の
    // トリムを適用する薄いラッパー。Roll は選択クリップとその次クリップの編集点
    // として扱う。成功時は 1 操作 = 1 Undo (saveUndoState) でまとめ true を返す。
    // 選択無し / 動画トラック無し / 境界違反なら false + errorOut に日本語理由。
    bool applyTrimActive(trimops::TrimType type, double deltaSec,
                         QString *errorOut = nullptr);

    // Audio
    void addAudioFile(const QString &filePath);
    void insertAudioClipAtPlayhead(const QString &wavPath, int trackIdx = 2);
    void toggleMuteTrack(int audioTrackIndex);
    void toggleSoloTrack(int audioTrackIndex);
    void normalizeAudioClipPeak(int trackIdx, int clipIdx);

    void setPlayheadPosition(double seconds);
    // Chase mode — re-centres the viewport on the playhead when it leaves
    // the central 50% comfort band. Called automatically from
    // setPlayheadPosition while the user isn't dragging the playhead bar
    // (drag has its own pinning behaviour in resolvePlayheadDragX).
    void ensurePlayheadVisible();
    double playheadPosition() const { return m_playheadPos; }
    double totalDuration() const;
    const QVector<ClipInfo> &videoClips() const {
        static const QVector<ClipInfo> kEmpty;
        if (m_videoTracks.isEmpty() || !m_videoTracks.first())
            return kEmpty;
        return m_videoTracks.first()->clips();
    }
    // US-INT-4: V1-only stabilizer keyframe accessors. Returns empty for
    // out-of-range indices. setClipStabilizerKeyframes overwrites the
    // existing vector (the slot calls clear-then-write semantics).
    const QVector<StabilizerKeyframe> &clipStabilizerKeyframesAt(int clipIndex) const {
        static const QVector<StabilizerKeyframe> kEmpty;
        const auto &clips = videoClips();
        if (clipIndex < 0 || clipIndex >= clips.size())
            return kEmpty;
        return clips[clipIndex].stabilizerKeyframes;
    }
    void setClipStabilizerKeyframes(int clipIndex, const QVector<StabilizerKeyframe> &kfs);
    // Add a text overlay to V1's first clip, writing back via setClips so
    // the mutation actually persists. Used by MainWindow's Adobe-style text
    // tool. Returns true if an overlay was added.
    bool addTextOverlayToFirstVideoClip(const EnhancedTextOverlay &overlay);
    // Update the text of an existing overlay on V1's first clip. Used by
    // click-to-edit when the user edits an existing overlay in place.
    bool updateTextOverlayText(int overlayIndex, const QString &newText);
    // Rewrite an existing overlay's rect (center-based normalized x/y,
    // normalized width/height). Returns false if the index is invalid.
    bool updateTextOverlayRect(int overlayIndex, double x, double y, double width, double height);
    // Apply a fully-mutated overlay (e.g. from TrackerLink) back into the
    // clip's TextManager without requiring a const_cast at the call site.
    bool applyTrackingToOverlay(int overlayIndex, const EnhancedTextOverlay &updated);
    // US-T35 update a V-track clip's per-clip video source transform.
    // trackIdx 0 = V1, 1 = V2, ... ; clipIdx is the clip position within
    // that track. Triggers sequenceChanged so VideoPlayer re-pulls the
    // new transform on the next advanceToEntry.
    bool setClipVideoTransform(int trackIdx, int clipIdx,
                               double scale, double dx, double dy);
    void setClipMotion(int trackIdx, int clipIdx,
                       const effectctrl::MotionState &motion);
    // Update an existing overlay's start/end time. Called from the timeline
    // text strip when the user drags an overlay's edge handle.
    bool updateTextOverlayTime(int overlayIndex, double startTime, double endTime);

    UndoManager *undoManager() const { return m_undoManager; }

    // Timeline 側のプロジェクト出力ジオメトリ複製を最新に保つ。currentState() が
    // undo スナップショットへ捕捉できるよう、MainWindow がプロジェクト設定変更時
    // (applyProjectConfig / プロジェクト読込)に呼ぶ。
    void setProjectOutputConfig(int width, int height, bool explicitOutput);

    // Multi-clip playback: flatten all video tracks into a sorted, gap-aware
    // schedule with topmost-track-wins resolution (Premiere V1/V2 semantics).
    QVector<PlaybackEntry> computePlaybackSequence() const;
    QVector<PlaybackEntry> computeAudioPlaybackSequence() const;

    // Re-emit sequenceChanged / audioSequenceChanged with the current clip
    // graph. Called when an external source (proxy generation, proxy mode
    // toggle) changes the path resolution rules without altering the clip
    // structure itself, so VideoPlayer's setSequence wiring picks up the
    // new resolved paths.
    void refreshPlaybackSequence();

    // Project save/load support
    QVector<QVector<ClipInfo>> allVideoTracks() const;
    QVector<QVector<ClipInfo>> allAudioTracks() const;
    void restoreFromProject(const QVector<QVector<ClipInfo>> &videoTracks,
                            const QVector<QVector<ClipInfo>> &audioTracks,
                            double playhead, double markIn, double markOut, int zoom);

    // TM-8: track-matte wiring SSOT. Producers (MainWindow on every
    // m_trackMatteClipEntries mutation; RenderQueue::resolveTimeline after
    // rebuilding a parentless Timeline from a loaded project) push the
    // QHash here keyed by "trackIdx:clipIdx" (== MainWindow::brushClipId ==
    // tlrender::renderClipId). renderFrameAt reads ONLY this — never a live
    // MainWindow off the worker thread (kills the C2 data race + C1
    // edit≠export divergence on the queue/file export path).
    void setTrackMatteEntries(const QHash<QString, TimelineTrackMatteEntry> &entries) {
        m_trackMatteEntries = entries;
    }
    // RM-3: return BY VALUE (a cheap Qt copy-on-write snapshot), NOT a
    // const reference. The RenderQueue worker thread reads this while the
    // GUI thread may call setTrackMatteEntries(); handing back a reference
    // let a reader observe a half-reassigned QHash (data race). A value
    // return atomically detaches a stable snapshot for the reader.
    QHash<QString, TimelineTrackMatteEntry> trackMatteEntries() const {
        return m_trackMatteEntries;
    }
signals:
    void clipSelected(int index);
    // V3 sprint — track-aware overload. emitted alongside the int-only
    // signal so MainWindow can drop its playhead heuristic and resolve
    // the (sourceTrack, sourceClipIndex) directly.
    void clipSelectedOnTrack(int trackIdx, int clipIdx);
    void scrubPositionChanged(double seconds);
    void positionChanged(double seconds);
    void sequenceChanged(const QVector<PlaybackEntry> &entries);
    void audioSequenceChanged(const QVector<PlaybackEntry> &entries);
    // restoreState が、プロジェクト出力ジオメトリを持つ undo/redo スナップショットを
    // 復元したときに発火。MainWindow が canvas + 出力サイズを再適用し、SNS プリセット
    // のリサイズを undo した後にプレビューが縦伸びしないようにする。
    void projectOutputConfigRestored(int width, int height, bool explicitOutput);
    // Per-track audio state updates that don't fit cleanly into the
    // PlaybackEntry struct. AudioMixer applies them via setTrackSolo on
    // receipt; mute and per-clip volume already ride on PlaybackEntry.
    void trackSoloChanged(int trackIdx, bool solo);
    // Fired when the user drags a text overlay's edge handle on the
    // timeline text strip so MainWindow can resync the right-panel
    // 開始時間 / 表示時間 spinboxes and re-push the preview overlays.
    void textOverlayTimeChanged(int overlayIndex, double startTime, double endTime);
    // Right-click → "カスタム..." / "ビデオエフェクト..." / "色補正..." in the
    // clip context menu. Timeline doesn't own the dialogs themselves, so it
    // signals up to MainWindow which holds the existing entry points
    // (addTransition / videoEffects / colorCorrection).
    void transitionDialogRequested();
    void videoEffectsDialogRequested();
    void colorCorrectionRequested();
    // Emitted from applyTransitionToSelected when the requested duration
    // could not be honored against the available source handles. Carries
    // the asked vs effective duration in seconds so MainWindow can show
    // a status message ("クロスディゾルブを 1.0s → 0.4s に短縮").
    void transitionShortened(QString transitionTypeName,
                             double askedSec, double effectiveSec);
    void statusMessageRequested(const QString &message, int timeoutMs);
    // Spec-conformant marker signals. markersChanged fires on every
    // addMarker/removeMarker/updateMarker/setMarkers mutation; markerClicked
    // fires from MarkerLane when the user clicks the body of a marker
    // triangle (future story will route this to an edit dialog).
    void markersChanged();
    void markerClicked(int id);
    // Fires on every addAdjustmentLayer / removeAdjustmentLayer /
    // updateAdjustmentLayer / setAdjustmentLayers mutation. Future
    // GLPreview composite + panel UI subscribe here.
    void adjustmentLayersChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onTrackClipClicked(int index);
    void onTrackModified();
    void onPlayheadAutoScrollTick();

private:
    void setupUI();
    void saveUndoState(const QString &description);
public:
    void restoreState(const TimelineState &state);
    TimelineState currentState() const;
private:
    void syncPlayheadOverlay();
    void updateInfoLabel();
    void ensureSequenceFitsViewport();
    QWidget *createTrackHeader(TimelineTrack *track, const QString &name, bool isAudioRow);
    void notifyMutationsChanged();
    void wireTrackSelection(TimelineTrack *track);
    void clearAllSelections();
    void captureZoomAnchor();
    void clearZoomAnchor();
    // Coalesces sequenceChanged + audioSequenceChanged emissions across
    // rapid mutations (drag-scrub, batch import, linked-drag stream). Each
    // call restarts a 50 ms single-shot timer; on timeout both signals fire
    // once with the latest computed sequences. Without this, every mouse-
    // move during a clip drag emitted sequenceChanged synchronously, and
    // the downstream AudioMixer::seekTo + QAudioSink stop/restart per
    // emission accumulated multi-second main-thread blocking.
    void scheduleEmitSequenceChanged();
    void emitSequenceChangedNow();

    QVector<TimelineTrack*> m_videoTracks;
    QVector<TimelineTrack*> m_audioTracks;
    TimelineTrack *m_videoTrack = nullptr; // alias for m_videoTracks[0]
    TimelineTrack *m_audioTrack = nullptr; // alias for m_audioTracks[0]
    int m_activeVideoTrackIndex = -1; // last video row that originated selection
    // TM-8: track-matte wiring carried by the Timeline (see
    // setTrackMatteEntries). Keyed by "trackIdx:clipIdx".
    QHash<QString, TimelineTrackMatteEntry> m_trackMatteEntries;
    QScrollArea *m_scrollArea;
    QWidget *m_tracksWidget;
    QVBoxLayout *m_tracksLayout;
    QWidget *m_headerColumn = nullptr;
    QVBoxLayout *m_headerLayout = nullptr;
    static constexpr int kHeaderColumnWidth = 130;
    PlayheadOverlay *m_playheadOverlay = nullptr;
    class TimeRuler *m_timeRuler = nullptr;
    TextStripWidget *m_textStrip = nullptr;
    QWidget *m_textStripHeader = nullptr;
    QWidget *m_magnetArea = nullptr;
    QWidget *m_vaSeparator = nullptr;
    QWidget *m_vaSeparatorHeader = nullptr;
    int m_textStripCustomHeight = 0;  // 0 = follows m_trackHeight
    void refreshTextStrip();
    QLabel *m_infoLabel;
    double m_playheadPos = 0.0;
    // プロジェクト出力ジオメトリの複製 (SSOT は MainWindow::m_projectConfig)。
    // currentState() で undo スナップショットへ捕捉する。
    // 既定は ProjectConfig の既定サイズ(1920x1080, 非明示出力)に合わせる。これにより
    // undo スナップショットが projectWidth=-1 を持たない不変条件を保証し、SNS プリセット
    // 適用前にサイズ同期(applyProjectConfig)を通らなかった経路でも、undo で元の
    // プロジェクトサイズへ正しく戻れる(restoreState の projectWidth>0 ガード)。
    // explicit=false のため addClip の auto-contain 判定(m_projectExplicitOutput ゲート)
    // は不変。
    int m_projectWidth = 1920;
    int m_projectHeight = 1080;
    bool m_projectExplicitOutput = false;
    double m_markIn = -1.0;
    double m_markOut = -1.0;
    double m_zoomLevel = 10.0; // pixels per second (double so we can go sub-1 for long clips)
    int m_trackHeight = 50; // default row height for new and existing tracks
    // Viewport-X of the playhead captured at the start of a zoom drag. While
    // set (>= 0), setZoomLevel pins the playhead to this viewport column so
    // the user zooms into the frame they were looking at, not the left edge.
    int m_zoomAnchorViewportX = -1;

    // Playhead drag auto-scroll: while the user drags the playhead bar past
    // the central 70% of the visible viewport (i.e., into the outer 15% on
    // either side), the bar visually pins at the boundary and the timeline
    // auto-scrolls under it instead of letting the bar travel offscreen.
    QTimer *m_playheadAutoScrollTimer = nullptr;
    int m_playheadDragViewportX = 0;
    bool m_playheadDragging = false;
    bool m_playheadDragMoved = false;

    UndoManager *m_undoManager;
    std::optional<ClipInfo> m_clipboard;
    // Monotonic counter for generating new linkGroup IDs. Zero is reserved
    // for "unlinked" so the next usable id is 1.
    int m_nextLinkGroup = 1;
    // Re-entrancy guard so propagating a selection to linked clips doesn't
    // bounce back through the selectionChanged signals and recurse forever.
    bool m_inLinkedSelectionSync = false;

    // Snapshot of every linked partner's leadInSec state at the start of a
    // MoveClip drag. Timeline applies the globally-clamped drag delta to
    // every partner (including the source clip) each frame so the V/A pair
    // stays locked together, without letting one partner drift past its
    // neighbor while another clamps short.
    struct LinkedDragPartner {
        TimelineTrack *track = nullptr;
        int clipIdx = -1;
        double origLeadIn = 0.0;
        double origLeadInNext = -1.0; // -1 = no next clip
    };
    QVector<LinkedDragPartner> m_linkedDragPartners;

    // Single-shot debouncer for sequenceChanged + audioSequenceChanged.
    // Owned by Qt object tree; created in the ctor.
    QTimer *m_emitSequenceTimer = nullptr;

    int allocateLinkGroup() { return m_nextLinkGroup++; }
    void onTrackSelectionChanged(int primaryIndex, bool additive);
    void showClipContextMenu(TimelineTrack *track, int clipIndex, const QPoint &globalPos);
    void unlinkClipGroup(int linkGroup);
    void relinkClipAt(TimelineTrack *track, int clipIndex);
    void cutSelectedClip();
    void captureLinkedDragPartners(TimelineTrack *source, int clipIdx);
    void applyLinkedDragDelta(double rawDeltaSec);
    void revertLinkedDragPartners();
    void clearLinkedDragState();
    void handleCrossTrackLinkedDrop(TimelineTrack *destTrack, int linkGroup, double dropTime);

    // Snap line visual feedback
    int m_snapLineX = -1;
    QElapsedTimer m_snapLineFadeTimer;
    static constexpr int kSnapLineFadeMs = 200;

    // SnapEngine — collects targets once per drag start, binary-search per tick
    SnapEngine m_snapEngine;

    // MarkerManager pointer — set by MainWindow for snap integration
    MarkerManager *m_markerManager = nullptr;

    // AudioMixer pointer — set by MainWindow for undo/restore of track gains
    AudioMixer *m_audioMixer = nullptr;

    // --- Timeline markers (Premiere Pro / DaVinci Resolve parity) ---
    // Markers are kept sorted by timelineUs so binary navigation in
    // nextMarkerAfter / prevMarkerBefore stays O(log N). The id counter is
    // monotonic and resets on setMarkers() to max(loaded ids)+1.
    QVector<Marker> m_markersData;
    int m_nextMarkerId = 1;
    MarkerLane *m_markerLane = nullptr;

    // --- Adjustment layers (Premiere/Photoshop parity) ---
    // Storage is order-of-insertion; trackIndex + stack position are
    // resolved by composeAdjustmentLayersAt() at render time. Id counter
    // resets on setAdjustmentLayers() to max(loaded ids)+1.
    QVector<AdjustmentLayer> m_adjustmentLayers;
    int m_nextAdjustmentLayerId = 1;
};

class PlayheadOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit PlayheadOverlay(QWidget *parent = nullptr);
    void setPlayheadX(int x);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

signals:
    void playheadMoved(int x);
    void playheadReleased(int x);

private:
    int m_playheadX = 0;
    bool m_dragging = false;
};

class TimeRuler : public QWidget
{
    Q_OBJECT

public:
    explicit TimeRuler(QWidget *parent = nullptr);
    void setPixelsPerSecond(double pps);
    double pixelsPerSecond() const { return m_pixelsPerSecond; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

signals:
    void zoomChanged(double newPixelsPerSecond);
    void zoomDragStarted();
    void zoomDragEnded();

private:
    double m_pixelsPerSecond = 10.0;
    bool m_dragging = false;
    int m_dragStartX = 0;
    double m_dragStartPps = 10.0;
};

// 16px-tall strip painted above the time ruler. Draws each Timeline marker
// as an 8x8 downward-pointing triangle at its time-mapped X. Hover shows a
// QToolTip with `label\nH:MM:SS.mmm`; left-click on a marker emits
// Timeline::markerClicked(id). The lane mirrors the timeline content
// width + scroll offset so marker positions track ruler tick positions
// exactly. No per-marker QObject overhead — every marker is a value in
// QVector<Marker>, so ~100 markers stays a single paint call.
class MarkerLane : public QWidget
{
    Q_OBJECT

public:
    explicit MarkerLane(Timeline *timeline, QWidget *parent = nullptr);

public slots:
    void setPixelsPerSecond(double pps);
    // Content x-offset to subtract when drawing. Mirrors the horizontal
    // scrollbar value of the timeline scroll area so markers align with
    // clip x-coordinates while the user pans.
    void setScrollOffset(int offsetX);

signals:
    void markerClicked(int id);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;  // for QHelpEvent / QToolTip

private:
    int markerScreenX(qint64 timelineUs) const;
    int hitTestMarker(const QPoint &pos) const;  // returns marker id, -1 on miss

    Timeline *m_timeline = nullptr;
    double m_pixelsPerSecond = 10.0;
    int m_scrollOffset = 0;
    static constexpr int kLaneHeight = 16;
    static constexpr int kTriangleSize = 8;
    static constexpr int kHitPad = 3;  // padding around triangle for hit-test
};
