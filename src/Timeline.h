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
#include <QMenu>
#include "VideoEffect.h"
#include "Keyframe.h"
#include "WaveformGenerator.h"
#include "TextManager.h"

class UndoManager;
struct TimelineState;

struct ClipInfo {
    QString filePath;
    QString displayName;
    double duration;
    double inPoint = 0.0;
    double outPoint = 0.0;
    double speed = 1.0;   // 0.25x - 4.0x
    double volume = 1.0;  // 0.0 - 2.0 (0=mute, 1=normal, 2=boost)

    // Phase 3: Color correction, effects, keyframes
    ColorCorrection colorCorrection;
    QVector<VideoEffect> effects;
    KeyframeManager keyframes;

    // Phase 5: Waveform
    WaveformData waveform;

    // Phase 6: Enhanced text overlays
    TextManager textManager;

    double effectiveDuration() const {
        double out = (outPoint > 0.0) ? outPoint : duration;
        return (out - inPoint) / speed;
    }
};

enum class DragMode {
    None,
    TrimLeft,
    TrimRight,
    MoveClip
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
    int clipCount() const { return m_clips.size(); }
    const QVector<ClipInfo> &clips() const { return m_clips; }
    void setClips(const QVector<ClipInfo> &clips);

    void setSelectedClip(int index);
    int selectedClip() const { return m_selectedClip; }

    int clipAtX(int x) const;
    double xToSeconds(int x) const;
    int secondsToX(double seconds) const;
    int clipStartX(int index) const;

    void setSnapEnabled(bool enabled) { m_snapEnabled = enabled; }
    bool snapEnabled() const { return m_snapEnabled; }
    void setPixelsPerSecond(int pps);
    int pixelsPerSecond() const { return m_pixelsPerSecond; }
    void setMuted(bool muted) { m_muted = muted; update(); }
    bool isMuted() const { return m_muted; }
    void setSolo(bool solo) { m_solo = solo; update(); }
    bool isSolo() const { return m_solo; }

signals:
    void clipClicked(int index);
    void selectionChanged(int index);
    void clipMoved(int fromIndex, int toIndex);
    void modified();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void updateMinimumWidth();
    int snapToEdge(int x) const;

    QVector<ClipInfo> m_clips;
    int m_selectedClip = -1;
    DragMode m_dragMode = DragMode::None;
    int m_dragClipIndex = -1;
    int m_dragStartX = 0;
    double m_dragOriginalValue = 0.0;
    bool m_snapEnabled = true;
    int m_dropTargetIndex = -1;

    int m_pixelsPerSecond = 10;
    bool m_muted = false;
    bool m_solo = false;
    static constexpr int CLIP_HEIGHT = 50;
    static constexpr int TRIM_HANDLE_WIDTH = 6;
    static constexpr int SNAP_THRESHOLD = 8;
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

    // Zoom
    void zoomIn();
    void zoomOut();
    void setZoomLevel(int pixelsPerSecond);

    // I/O markers
    void markIn();
    void markOut();
    double markedIn() const { return m_markIn; }
    double markedOut() const { return m_markOut; }
    bool hasMarkedRange() const { return m_markIn >= 0 && m_markOut > m_markIn; }

    // Multi-track
    void addVideoTrack();
    void addAudioTrack();
    int videoTrackCount() const { return m_videoTracks.size(); }
    int audioTrackCount() const { return m_audioTracks.size(); }

    // Clip speed & volume
    void setClipSpeed(double speed);
    void setClipVolume(double volume);

    // Phase 3: Color correction, effects, keyframes
    void setClipColorCorrection(const ColorCorrection &cc);
    void setClipEffects(const QVector<VideoEffect> &effects);
    void setClipKeyframes(const KeyframeManager &km);
    ColorCorrection clipColorCorrection() const;
    QVector<VideoEffect> clipEffects() const;
    KeyframeManager clipKeyframes() const;
    double selectedClipDuration() const;

    // Audio
    void addAudioFile(const QString &filePath);
    void toggleMuteTrack(int audioTrackIndex);
    void toggleSoloTrack(int audioTrackIndex);

    void setPlayheadPosition(double seconds);
    double playheadPosition() const { return m_playheadPos; }
    double totalDuration() const;
    const QVector<ClipInfo> &videoClips() const { return m_videoTracks[0]->clips(); }

    UndoManager *undoManager() const { return m_undoManager; }

    // Project save/load support
    QVector<QVector<ClipInfo>> allVideoTracks() const;
    QVector<QVector<ClipInfo>> allAudioTracks() const;
    void restoreFromProject(const QVector<QVector<ClipInfo>> &videoTracks,
                            const QVector<QVector<ClipInfo>> &audioTracks,
                            double playhead, double markIn, double markOut, int zoom);

signals:
    void clipSelected(int index);
    void positionChanged(double seconds);

private slots:
    void onTrackClipClicked(int index);
    void onTrackModified();

private:
    void setupUI();
    void saveUndoState(const QString &description);
    void restoreState(const TimelineState &state);
    TimelineState currentState() const;
    void updateInfoLabel();

    QVector<TimelineTrack*> m_videoTracks;
    QVector<TimelineTrack*> m_audioTracks;
    TimelineTrack *m_videoTrack; // alias for m_videoTracks[0]
    TimelineTrack *m_audioTrack; // alias for m_audioTracks[0]
    QScrollArea *m_scrollArea;
    QWidget *m_tracksWidget;
    QVBoxLayout *m_tracksLayout;
    QLabel *m_infoLabel;
    double m_playheadPos = 0.0;
    double m_markIn = -1.0;
    double m_markOut = -1.0;
    int m_zoomLevel = 10; // pixels per second

    UndoManager *m_undoManager;
    std::optional<ClipInfo> m_clipboard;
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

private:
    int m_playheadX = 0;
    bool m_dragging = false;
};
