#pragma once

#include <QDialog>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QProgressBar>
#include <QTimer>
#include <QString>
#include <QAudioDevice>

namespace voiceover {
class VoiceOverRecorder;

class VoiceOverDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Phase { Idle, Countdown, Recording, Done };

    explicit VoiceOverDialog(const QString &defaultWavPath, QWidget *parent = nullptr);
    ~VoiceOverDialog() override;

    QString outputWavPath() const { return m_pathEdit->text(); }
    bool insertAtPlayhead() const { return m_insertCheck->isChecked(); }
    QAudioDevice selectedDevice() const;
    Phase currentPhase() const { return m_phase; }

public slots:
    void startRecording();
    void stopRecording();

signals:
    void recordingStarted();
    void recordingStopped(const QString &wavPath, qint64 elapsedMs);

private slots:
    void onBrowsePath();
    void onCountdownTick();
    void onRecordIndicatorTick();
    void onLevelChanged(double rmsDb);
    void onRecordingFinished(const QString &wavPath, qint64 elapsedMs);
    void onRecordingFailed(const QString &errorMessage);
    void onElapsedTick();

private:
    void setupUI();
    void populateDevices();
    void setPhase(Phase phase);
    QString formatElapsed(qint64 ms) const;

    Phase m_phase = Phase::Idle;

    // UI widgets
    QComboBox *m_deviceCombo = nullptr;
    QLabel *m_countdownLabel = nullptr;
    QLabel *m_recordIndicator = nullptr;
    QLabel *m_elapsedLabel = nullptr;
    QProgressBar *m_levelBar = nullptr;
    QPushButton *m_recordButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QCheckBox *m_insertCheck = nullptr;

    // Timers
    QTimer *m_countdownTimer = nullptr;
    QTimer *m_blinkTimer = nullptr;
    QTimer *m_elapsedTimer = nullptr;

    // Recorder
    voiceover::VoiceOverRecorder *m_recorder = nullptr;

    // State
    int m_countdownValue = 3;
    qint64 m_elapsedMs = 0;
    bool m_indicatorOn = false;

    // Recording result (set before accept())
    QString m_resultWavPath;
    qint64 m_resultElapsedMs = 0;

protected:
    void done(int result) override;
};

} // namespace voiceover
