#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QComboBox;
class QDoubleSpinBox;
class QRadioButton;
class Timeline;
struct ClipInfo;

namespace loudnesspreset {

enum class NormalizeScope {
    SingleClip = 0,          // Clicked audio clip only
    AllClipsIndividual = 1,  // Every audio clip normalized individually to target LUFS
    AllClipsUniform = 2      // Uniform gain across all audio clips preserving relative balance
};

struct PlatformPreset {
    QString id;
    QString displayName;
    double targetLufs = -14.0;
    double maxTruePeakDbtp = -1.0;
};

struct ClipMeasurement {
    bool valid = false;
    double integratedLufs = -70.0;
    double truePeakDbtp = -70.0;
    double durationSec = 0.0;
};

struct GainDecision {
    bool valid = false;
    double gainLinear = 1.0;
    double gainDb = 0.0;
    bool peakClamped = false;
    bool maxGainClamped = false;
};

struct NormalizeResult {
    bool ok = false;
    int clipsUpdated = 0;
    int clipsSkipped = 0;
    double measuredLufs = -70.0;
    double appliedGainDb = 0.0;
    bool peakClamped = false;
    QString summary;
};

// Presets shared by ExportDialog (indices 0..3 match -14 / -23 / -16 / -24, index 4 = custom).
// Keeping index 0 = -14, index 1 = -23, index 2 = -16 preserves loudness-auto G6 compatibility.
QVector<PlatformPreset> exportDialogPresets();
int exportPresetIndexForLufs(double targetLufs);

// Presets displayed in the timeline audio clip right-click menu & NormalizeOptionsDialog.
QVector<PlatformPreset> timelinePresets();

// Decode [clip.inPoint, clip.outPoint] via FFmpeg and measure BS.1770 integrated LUFS & true peak.
// When scaleFactor != 1.0, input PCM is scaled by scaleFactor before measurement.
ClipMeasurement measureClipLoudness(const ClipInfo &clip, double scaleFactor = 1.0);

// Pure gain computation for a single clip (raw unity-gain measurement -> final linear clip.volume).
GainDecision computeTargetClipGain(double rawIntegratedLufs,
                                   double rawTruePeakDbtp,
                                   double targetLufs,
                                   double maxTruePeakDbtp = -1.0);

// Pure uniform gain-ratio computation across multiple clips already measured at their current volumes.
GainDecision computeUniformGainRatio(const QVector<ClipMeasurement> &scaledMeasurements,
                                     double targetLufs,
                                     double maxTruePeakDbtp = -1.0);

// Apply loudness normalization to Timeline with a single undo step.
NormalizeResult applyLoudnessNormalization(Timeline *timeline,
                                           NormalizeScope scope,
                                           int trackIdx,
                                           int clipIdx,
                                           double targetLufs,
                                           double maxTruePeakDbtp,
                                           const QString &presetLabel);

class NormalizeOptionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit NormalizeOptionsDialog(NormalizeScope initialScope = NormalizeScope::SingleClip,
                                    double initialLufs = -14.0,
                                    double initialPeakDbtp = -1.0,
                                    QWidget *parent = nullptr);

    NormalizeScope selectedScope() const;
    double targetLufs() const;
    double maxTruePeakDbtp() const;
    QString selectedPresetLabel() const;

private:
    QRadioButton *m_scopeSingle = nullptr;
    QRadioButton *m_scopeAllIndividual = nullptr;
    QRadioButton *m_scopeAllUniform = nullptr;
    QComboBox *m_presetCombo = nullptr;
    QDoubleSpinBox *m_lufsSpin = nullptr;
    QDoubleSpinBox *m_peakSpin = nullptr;
};

} // namespace loudnesspreset
