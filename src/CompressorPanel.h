#pragma once

#include <QWidget>

#include "AudioMixer.h"

class QComboBox;
class QDial;
class QCheckBox;
class QPushButton;
class QLabel;
class QProgressBar;
class QTimer;

// CompressorPanel — per-track feed-forward compressor / limiter UI for
// AudioMixer. Mirrors EqualizerPanel's signal contract: owner connects
// compressorChanged() to AudioMixer::setCompressorForTrack and the panel
// emits whenever any control changes.
//
// Layout: track combobox + 6 dials (Threshold / Ratio / Attack / Release
// / Knee / Make-up) + Enable checkbox + Limiter Mode push-button + GR
// meter. The GR meter polls AudioMixer::currentGainReductionDb at 30 Hz.
class CompressorPanel : public QWidget {
    Q_OBJECT
public:
    explicit CompressorPanel(QWidget *parent = nullptr);

    // Provide the live mixer pointer so the GR meter can poll it. The
    // panel does not take ownership.
    void setMixer(AudioMixer *mixer);

    // Refresh the combobox label list. Pass {1,2,3,...} for active audio
    // track ids. The master row (id 0) is always inserted first.
    void setTrackList(const QList<int> &trackIds);

    // Programmatic load — set dials + enable from settings without
    // triggering compressorChanged.
    void loadSettings(int trackId, const AudioMixer::CompressorSettings &c);

    int currentTrackId() const;

signals:
    void compressorChanged(int trackId, AudioMixer::CompressorSettings c);

private slots:
    void onAnyControlChanged();
    void onTrackChanged(int idx);
    void onLimiterModeClicked();
    void onPollMeter();

private:
    void buildUi();
    AudioMixer::CompressorSettings collectSettings() const;
    void applySettingsToControls(const AudioMixer::CompressorSettings &c);
    void emitChange();

    AudioMixer *m_mixer = nullptr;
    bool m_suppressEmit = false;

    QComboBox *m_trackCombo = nullptr;
    QDial *m_threshold = nullptr;
    QDial *m_ratio = nullptr;
    QDial *m_attack = nullptr;
    QDial *m_release = nullptr;
    QDial *m_knee = nullptr;
    QDial *m_makeup = nullptr;
    QLabel *m_thresholdLabel = nullptr;
    QLabel *m_ratioLabel = nullptr;
    QLabel *m_attackLabel = nullptr;
    QLabel *m_releaseLabel = nullptr;
    QLabel *m_kneeLabel = nullptr;
    QLabel *m_makeupLabel = nullptr;
    QCheckBox *m_enable = nullptr;
    QPushButton *m_limiterMode = nullptr;
    QProgressBar *m_grMeter = nullptr;
    QLabel *m_grValue = nullptr;
    QTimer *m_pollTimer = nullptr;
};
