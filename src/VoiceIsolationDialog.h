#pragma once

#include <QByteArray>
#include <QDialog>
#include <QMediaPlayer>
#include <QString>
#include <QTemporaryFile>
#include <QVector>

#include "VoiceIsolation.h"

class QAudioOutput;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;

class VoiceIsolationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VoiceIsolationDialog(QWidget *parent = nullptr);

    void setAudioSource(const QString &label,
                        const QVector<float> &samples,
                        int sampleRate);
    void setInitialRange(double startSeconds, double endSeconds);
    QVector<float> processedSamples() const { return m_processedSamples; }
    int sampleRate() const { return m_sampleRate; }
    double rangeStartSeconds() const;
    double rangeEndSeconds() const;

signals:
    void applied();

private slots:
    void onAnalyzeClicked();
    void onPreviewClicked();
    void onApplyClicked();
    void onRangeChanged();
    void onPlayerStateChanged(QMediaPlayer::PlaybackState state);

private:
    voiceiso::VoiceIsolationParams paramsFromUi() const;
    bool processCurrentRange();
    bool writePreviewFile(const QVector<float> &samples, QString *error);
    void setBusy(bool busy);
    void invalidateAnalysis();
    void updateActionState();

    QLabel *m_sourceLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_ratioLabel = nullptr;
    QDoubleSpinBox *m_rangeStartSpin = nullptr;
    QDoubleSpinBox *m_rangeEndSpin = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QSlider *m_strengthSlider = nullptr;
    QDoubleSpinBox *m_strengthSpin = nullptr;
    QDoubleSpinBox *m_voiceGainSpin = nullptr;
    QDoubleSpinBox *m_backgroundGainSpin = nullptr;
    QDoubleSpinBox *m_lowHzSpin = nullptr;
    QDoubleSpinBox *m_highHzSpin = nullptr;
    QDoubleSpinBox *m_noiseLearnSpin = nullptr;
    QCheckBox *m_adaptiveNoiseCheck = nullptr;
    QSlider *m_harmonicSlider = nullptr;
    QDoubleSpinBox *m_harmonicSpin = nullptr;
    QSlider *m_smoothingSlider = nullptr;
    QDoubleSpinBox *m_smoothingSpin = nullptr;
    QPushButton *m_analyzeButton = nullptr;
    QPushButton *m_previewButton = nullptr;
    QPushButton *m_applyButton = nullptr;

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QTemporaryFile m_previewFile;

    QVector<float> m_samples;
    QVector<float> m_processedSamples;
    int m_sampleRate = 0;
    bool m_hasAnalysis = false;
    bool m_busy = false;
};
