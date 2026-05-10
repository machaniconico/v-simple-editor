#pragma once

#include <QWidget>

#include "AudioMixer.h"

class QComboBox;
class QDial;
class QCheckBox;
class QLabel;
class QTimer;

// NoiseReductionPanel — per-track noise reduction UI for AudioMixer
// (Audition Voice Isolation / Resolve Fairlight noise gate parity,
// simplified expander). Mirrors ReverbPanel's signal contract: the owner
// connects noiseReductionChanged() to AudioMixer::setNoiseReductionForTrack
// and the panel emits whenever any control changes.
//
// Layout: track combobox + Enable checkbox + 5 dials (Threshold /
// Reduction / Attack / Release / Manual Floor) + Auto-floor checkbox +
// real-time floor estimate readout.
class NoiseReductionPanel : public QWidget {
    Q_OBJECT
public:
    explicit NoiseReductionPanel(QWidget *parent = nullptr);

    // Provide the live mixer pointer so the panel can pull current
    // settings on track change AND poll the auto-floor estimate readout.
    // The panel does not take ownership.
    void setMixer(AudioMixer *mixer);

    // Refresh the combobox label list. Pass {1,2,3,...} for active audio
    // track ids. The master row (id 0) is always inserted first.
    void setTrackList(const QList<int> &trackIds);

    // Programmatic load — set dials + checkbox state from settings without
    // triggering noiseReductionChanged.
    void loadSettings(int trackId,
                      const AudioMixer::NoiseReductionSettings &nr);

    int currentTrackId() const;

signals:
    void noiseReductionChanged(int trackId,
                               AudioMixer::NoiseReductionSettings nr);

private slots:
    void onAnyControlChanged();
    void onTrackChanged(int idx);
    void onAutoFloorToggled(bool checked);
    void onPollFloorReadout();

private:
    void buildUi();
    AudioMixer::NoiseReductionSettings collectSettings() const;
    void applySettingsToControls(
        const AudioMixer::NoiseReductionSettings &nr);
    void updateFloorReadoutEnabled();
    void emitChange();

    AudioMixer *m_mixer = nullptr;
    bool m_suppressEmit = false;

    QComboBox *m_trackCombo = nullptr;
    QCheckBox *m_enable = nullptr;
    QCheckBox *m_autoFloor = nullptr;

    QDial *m_threshold = nullptr;
    QDial *m_reduction = nullptr;
    QDial *m_attack = nullptr;
    QDial *m_release = nullptr;
    QDial *m_manualFloor = nullptr;

    QLabel *m_thresholdLabel = nullptr;
    QLabel *m_reductionLabel = nullptr;
    QLabel *m_attackLabel = nullptr;
    QLabel *m_releaseLabel = nullptr;
    QLabel *m_manualFloorLabel = nullptr;
    QLabel *m_floorReadout = nullptr;

    QTimer *m_pollTimer = nullptr;
};
