#pragma once

// MultiCamDialog
// --------------
// Premiere Pro Multicam / Resolve Multicam Sync (simplified) parity. The
// user loads 2..4 video files as "angles", drops switch markers at the
// playhead, and accepts the dialog. result() then returns a
// MultiCamProject (an EDL) ready for the caller to apply to the timeline
// in a follow-up consolidation pass.
//
// The dialog is intentionally STANDALONE: it never imports Timeline,
// AudioMixer, GLPreview, or any decoder. Thumbnails come from
// QMediaPlayer + QVideoSink (a single shared player polled per angle on
// load — the dialog stops the player as soon as a frame lands), so no
// custom FFmpeg work is required for this story.
//
// Hotkeys 1/2/3/4 trigger the corresponding "Cut to Angle N" action when
// that angle is loaded.

#include <QDialog>
#include <QVector>

#include "MultiCam.h"

class QLabel;
class QPushButton;
class QSlider;
class QListWidget;
class QGridLayout;
class QShortcut;
class QPaintEvent;
class QResizeEvent;

// Lightweight horizontal strip that draws a tick line per switch on top
// of a horizontal slider proxy. Defined in the .cpp.
class MultiCamSwitchStrip;

class MultiCamDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MultiCamDialog(QWidget *parent = nullptr);
    ~MultiCamDialog() override = default;

    // Pre-populate the dialog with an existing project (for editing). All
    // fields are deep-copied; the caller's project is unaffected.
    void setProject(const MultiCamProject &project);

    // Set the total timeline length used by the bottom edit-decision
    // strip (default = 60 s expressed in microseconds). The slider's
    // maximum is recomputed; existing switches are preserved.
    void setTimelineDurationUs(qint64 durationUs);

    // Returns the project as edited by the user. Always valid (even
    // before the dialog is accepted) so tests can poke at it.
    MultiCamProject result() const { return m_project; }

signals:
    // Emitted when the user clicks タイムラインに適用. The receiver
    // (MainWindow) replaces V1 + A1 with the EDL encoded in switches[].
    // The dialog itself stays open and never touches the timeline.
    void applyToTimeline(const MultiCamProject &project);

private slots:
    void onAddAngle();
    void onRemoveAngle();
    void onSync();
    void onPlayheadChanged(int valueMs);
    void onCutToAngle(int slotIndex);     // 0..3 — which UI slot
    void onAngleListSelected();

private:
    void buildUi();
    void wireHotkeys();
    void rebuildAngleList();
    void rebuildThumbnailGrid();
    void refreshSwitchStrip();
    void refreshAngleButtons();
    int  allocateAngleId() const;
    int  selectedAngleRow() const;        // row in m_angleList, -1 if none
    void loadThumbnailFor(int slotIndex); // best-effort QMediaPlayer probe

    static constexpr int kMaxAngles = 4;

    MultiCamProject       m_project;
    qint64                m_timelineDurationUs = 60LL * 1000000LL;
    qint64                m_playheadUs         = 0;

    // UI
    QGridLayout          *m_thumbGrid          = nullptr;
    QLabel               *m_thumbLabels[kMaxAngles] = {nullptr, nullptr,
                                                       nullptr, nullptr};
    QLabel               *m_thumbCaptions[kMaxAngles] = {nullptr, nullptr,
                                                         nullptr, nullptr};
    QListWidget          *m_angleList          = nullptr;
    QPushButton          *m_addBtn             = nullptr;
    QPushButton          *m_removeBtn          = nullptr;
    QPushButton          *m_syncBtn            = nullptr;
    QPushButton          *m_cutBtns[kMaxAngles] = {nullptr, nullptr,
                                                   nullptr, nullptr};
    QSlider              *m_playhead           = nullptr;
    MultiCamSwitchStrip  *m_strip              = nullptr;
    QPushButton          *m_applyBtn           = nullptr;
    QPushButton          *m_cancelBtn          = nullptr;

    QShortcut            *m_hotkeys[kMaxAngles] = {nullptr, nullptr,
                                                   nullptr, nullptr};
};
