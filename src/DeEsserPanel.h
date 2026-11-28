#pragma once

#include <climits>

#include <QHash>
#include <QWidget>

#include "AudioTrackFx.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;

// DeEsserPanel — live-apply de-esser UI hosted in a dock (same shape as
// EqualizerPanel). A target combo selects the master bus or an audio track;
// every control edit emits settingsChanged(trackId, settings) so the host can
// forward it to the AudioMixer immediately. Switching the target never emits
// and never discards the edited values of the target being left.
class DeEsserPanel : public QWidget {
    Q_OBJECT
public:
    explicit DeEsserPanel(QWidget *parent = nullptr);

    // trackIds matches itemNames 1:1 (AudioMixer::kMasterTrackId = master,
    // 0 = A1, ...). Keeps the current target when it is still present.
    void setTracks(const QStringList &itemNames, const QList<int> &trackIds);
    // Replace the stored / displayed values of trackId without emitting.
    void setSettings(int trackId, const trackfx::DeEsserSettings &settings);

    int targetCount() const;
    int currentTrackId() const;
    trackfx::DeEsserSettings currentSettings() const;

signals:
    void settingsChanged(int trackId, trackfx::DeEsserSettings settings);
    // One finished user edit (spin box editingFinished, enable checkbox) that
    // emitted settingsChanged; the host records it as one undo step. The
    // listen monitor is not undo state and never marks a gesture.
    void gestureFinished(int trackId);

private slots:
    void onTargetChanged();
    void onAnyControlChanged();
    void finishGesture();

private:
    void showSettings(const trackfx::DeEsserSettings &settings);

    QComboBox *m_target = nullptr;
    QCheckBox *m_enabled = nullptr;
    QCheckBox *m_listen = nullptr;
    QDoubleSpinBox *m_frequency = nullptr;
    QDoubleSpinBox *m_threshold = nullptr;
    QDoubleSpinBox *m_ratio = nullptr;
    QDoubleSpinBox *m_range = nullptr;

    // Edits made per target so switching never loses them.
    QHash<int, trackfx::DeEsserSettings> m_perTrackSettings;
    int m_currentTrackId = INT_MIN; // no target yet
    bool m_suppressSignals = false;
    bool m_gestureDirty = false; // edited since the last gestureFinished
};
