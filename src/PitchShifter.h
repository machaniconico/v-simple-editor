#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class Timeline;

namespace pitchshift {

enum class VoicePreset {
    Natural = 0,
    Chipmunk,      // +6 semitones
    DeepVoice,     // -5 semitones
    Robot,         // ring modulation + slight metallic tone
    TelephoneRadio,// 300-3400Hz bandpass + mild drive
    Helium,        // +9 semitones
    Demon          // -8 semitones
};

struct PitchShiftParams {
    bool enabled = false;
    double semitones = 0.0;        // [-12.0 .. +12.0]
    VoicePreset preset = VoicePreset::Natural;
    double wetMix = 1.0;           // [0.0 .. 1.0]
    double robotFreqHz = 85.0;     // carrier Hz for Robot preset
};

QString presetDisplayName(VoicePreset preset);
PitchShiftParams paramsForPreset(VoicePreset preset);

// Fast-path atomic store persisted per clip UID (zero per-frame disk I/O when unused).
bool hasAnyActiveConfig();
PitchShiftParams configForClip(const QString &clipUid);
void setConfigForClip(const QString &clipUid, const PitchShiftParams &params);
void clearConfigForClip(const QString &clipUid);
void clearAllConfigsForTest();

// Pure C++ duration-preserving pitch shifter (Hann-windowed OLA time-stretch + linear resample)
// + voice changer post-processing (Robot ring-mod, Telephone/Radio bandpass).
// Output sample count ALWAYS matches input sample count!
QVector<float> processMonoBuffer(const QVector<float> &input,
                                 int sampleRate,
                                 const PitchShiftParams &params);

// Estimate dominant frequency in Hz via DFT bin scan (used by selftest & diagnostics).
double estimateDominantFrequencyHz(const QVector<float> &mono, int sampleRate);

// Read 16-bit PCM WAV, apply pitch shift / voice changer, write 16-bit PCM WAV.
bool processWavFile(const QString &inputWavPath,
                    const QString &outputWavPath,
                    const PitchShiftParams &params,
                    QString *errorOut = nullptr);

// Bounce pitch-shifted WAV for an audio clip and replace clip.filePath in 1 Undo step.
bool applyPitchShiftToAudioClip(Timeline *timeline,
                                int audioTrackIdx,
                                int clipIdx,
                                const PitchShiftParams &params,
                                const QString &outputWavPath,
                                QString *errorOut = nullptr);

class PitchShiftDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PitchShiftDialog(const QString &clipUid,
                              const QString &clipLabel,
                              QWidget *parent = nullptr);

    PitchShiftParams currentParams() const;

private:
    void onPresetChanged(int idx);
    void applyFromParams(const PitchShiftParams &p);
    void onAcceptSave();

    QString m_clipUid;
    QCheckBox *m_enableCheck = nullptr;
    QComboBox *m_presetCombo = nullptr;
    QDoubleSpinBox *m_semitonesSpin = nullptr;
    QDoubleSpinBox *m_wetMixSpin = nullptr;
    QDoubleSpinBox *m_robotFreqSpin = nullptr;
};

} // namespace pitchshift
