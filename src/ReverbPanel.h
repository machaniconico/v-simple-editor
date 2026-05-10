#pragma once

#include <QWidget>

#include "AudioMixer.h"

class QComboBox;
class QDial;
class QCheckBox;
class QPushButton;
class QLabel;

// ReverbPanel — per-track reverb UI for AudioMixer (Audition /
// Fairlight Multitap parity, simplified Schroeder topology). Mirrors
// CompressorPanel's signal contract: owner connects reverbChanged() to
// AudioMixer::setReverbForTrack and the panel emits whenever any
// control changes.
//
// Layout: track combobox + Enable checkbox + 5 dials (Mix / Decay /
// Pre-delay / Damping / Width) + 5 preset buttons (Hall / Room /
// Plate / Cathedral / Off).
class ReverbPanel : public QWidget {
    Q_OBJECT
public:
    explicit ReverbPanel(QWidget *parent = nullptr);

    // Provide the live mixer pointer so the panel can pull current
    // settings on track change. The panel does not take ownership.
    void setMixer(AudioMixer *mixer);

    // Refresh the combobox label list. Pass {1,2,3,...} for active audio
    // track ids. The master row (id 0) is always inserted first.
    void setTrackList(const QList<int> &trackIds);

    // Programmatic load — set dials + enable from settings without
    // triggering reverbChanged.
    void loadSettings(int trackId, const AudioMixer::ReverbSettings &r);

    int currentTrackId() const;

signals:
    void reverbChanged(int trackId, AudioMixer::ReverbSettings r);

private slots:
    void onAnyControlChanged();
    void onTrackChanged(int idx);
    void onPresetClicked();

private:
    void buildUi();
    AudioMixer::ReverbSettings collectSettings() const;
    void applySettingsToControls(const AudioMixer::ReverbSettings &r);
    void emitChange();

    AudioMixer *m_mixer = nullptr;
    bool m_suppressEmit = false;

    QComboBox *m_trackCombo = nullptr;
    QCheckBox *m_enable = nullptr;

    QDial *m_mix = nullptr;
    QDial *m_decay = nullptr;
    QDial *m_preDelay = nullptr;
    QDial *m_damping = nullptr;
    QDial *m_width = nullptr;

    QLabel *m_mixLabel = nullptr;
    QLabel *m_decayLabel = nullptr;
    QLabel *m_preDelayLabel = nullptr;
    QLabel *m_dampingLabel = nullptr;
    QLabel *m_widthLabel = nullptr;

    QPushButton *m_presetHall = nullptr;
    QPushButton *m_presetRoom = nullptr;
    QPushButton *m_presetPlate = nullptr;
    QPushButton *m_presetCathedral = nullptr;
    QPushButton *m_presetOff = nullptr;
};
