#pragma once

#include <climits>
#include <array>

#include <QHash>
#include <QWidget>

#include "AudioTrackFx.h"

class AudioMixer;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

// MultibandCompressorPanel — Wave 34 live-apply 4-band LR4 multiband compressor
// UI hosted in a dock (same signal/reseed/undo contract as DeEsserPanel).
// Target combo selects the master bus or an audio track; edits emit
// settingsChanged(trackId, settings) immediately and gestureFinished(trackId)
// once per completed user gesture so MainWindow records 1 Undo step.
class MultibandCompressorPanel : public QWidget {
    Q_OBJECT
public:
    explicit MultibandCompressorPanel(QWidget *parent = nullptr);

    void setMixer(AudioMixer *mixer);
    void setTracks(const QStringList &itemNames, const QList<int> &trackIds);
    void setSettings(int trackId, const trackfx::MultibandCompSettings &settings);

    int targetCount() const;
    int currentTrackId() const;
    trackfx::MultibandCompSettings currentSettings() const;
    int changedEmitCountForTest() const { return m_emitCount; }

signals:
    void settingsChanged(int trackId, trackfx::MultibandCompSettings settings);
    void gestureFinished(int trackId);

private slots:
    void onTargetChanged();
    void onPresetActivated(int index);
    void onAnyControlChanged();
    void onSoloToggled();
    void finishGesture();
    void onPollMeters();

private:
    struct BandControls {
        QDoubleSpinBox *threshold = nullptr;
        QDoubleSpinBox *ratio = nullptr;
        QDoubleSpinBox *attack = nullptr;
        QDoubleSpinBox *release = nullptr;
        QDoubleSpinBox *knee = nullptr;
        QDoubleSpinBox *makeup = nullptr;
        QPushButton *bypass = nullptr;
        QPushButton *solo = nullptr;
        QProgressBar *grMeter = nullptr;
        QLabel *grLabel = nullptr;
    };

    void showSettings(const trackfx::MultibandCompSettings &settings);

    AudioMixer *m_mixer = nullptr;
    QComboBox *m_target = nullptr;
    QCheckBox *m_enabled = nullptr;
    QComboBox *m_preset = nullptr;
    QDoubleSpinBox *m_outputGain = nullptr;
    QDoubleSpinBox *m_wetMix = nullptr;
    std::array<QDoubleSpinBox *, trackfx::MultibandCompSettings::kCrossoverCount> m_crossovers{};
    std::array<BandControls, trackfx::MultibandCompSettings::kBandCount> m_bands{};
    QTimer *m_pollTimer = nullptr;

    QHash<int, trackfx::MultibandCompSettings> m_perTrackSettings;
    int m_currentTrackId = INT_MIN;
    bool m_suppressSignals = false;
    bool m_gestureDirty = false;
    int m_emitCount = 0;
};
