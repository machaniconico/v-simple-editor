#include "VoiceOverDialog.h"
#include "VoiceOverRecorder.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QMediaDevices>
#include <QDateTime>
#include <QFont>
#include <QDir>

namespace voiceover {

VoiceOverDialog::VoiceOverDialog(const QString &defaultWavPath, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Voice-over Record"));
    setModal(true);
    setMinimumWidth(420);
    setMinimumHeight(340);

    m_recorder = new voiceover::VoiceOverRecorder(this);

    setupUI();
    populateDevices();

    m_pathEdit->setText(defaultWavPath);

    connect(m_recorder, &voiceover::VoiceOverRecorder::levelChanged,
            this, &VoiceOverDialog::onLevelChanged);
    connect(m_recorder, &voiceover::VoiceOverRecorder::recordingFinished,
            this, &VoiceOverDialog::onRecordingFinished);
    connect(m_recorder, &voiceover::VoiceOverRecorder::recordingFailed,
            this, &VoiceOverDialog::onRecordingFailed);

    setPhase(Phase::Idle);
}

VoiceOverDialog::~VoiceOverDialog() = default;

void VoiceOverDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);

    // Device selection
    auto *deviceGroup = new QGroupBox(tr("Audio Input Device"));
    auto *deviceLayout = new QHBoxLayout(deviceGroup);
    m_deviceCombo = new QComboBox();
    deviceLayout->addWidget(m_deviceCombo);
    mainLayout->addWidget(deviceGroup);

    // Countdown label
    m_countdownLabel = new QLabel();
    m_countdownLabel->setAlignment(Qt::AlignCenter);
    QFont countdownFont = m_countdownLabel->font();
    countdownFont.setPointSize(48);
    countdownFont.setBold(true);
    m_countdownLabel->setFont(countdownFont);
    m_countdownLabel->setMinimumHeight(70);
    mainLayout->addWidget(m_countdownLabel);

    // Recording status row: indicator + elapsed
    auto *statusRow = new QHBoxLayout();
    m_recordIndicator = new QLabel();
    m_recordIndicator->setFixedSize(20, 20);
    m_recordIndicator->setStyleSheet(
        "QLabel { background-color: #555555; border-radius: 10px; }");
    statusRow->addWidget(m_recordIndicator);

    m_elapsedLabel = new QLabel("00:00");
    m_elapsedLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont elapsedFont = m_elapsedLabel->font();
    elapsedFont.setPointSize(14);
    m_elapsedLabel->setFont(elapsedFont);
    statusRow->addWidget(m_elapsedLabel);
    statusRow->addStretch();
    mainLayout->addLayout(statusRow);

    // Level meter
    m_levelBar = new QProgressBar();
    m_levelBar->setRange(0, 100);
    m_levelBar->setValue(0);
    m_levelBar->setTextVisible(false);
    m_levelBar->setFixedHeight(12);
    mainLayout->addWidget(m_levelBar);

    // Save path
    auto *pathRow = new QHBoxLayout();
    pathRow->addWidget(new QLabel(tr("Save to:")));
    m_pathEdit = new QLineEdit();
    pathRow->addWidget(m_pathEdit);
    m_browseButton = new QPushButton(tr("Browse..."));
    connect(m_browseButton, &QPushButton::clicked, this, &VoiceOverDialog::onBrowsePath);
    pathRow->addWidget(m_browseButton);
    mainLayout->addLayout(pathRow);

    // Insert at playhead checkbox
    m_insertCheck = new QCheckBox(tr("Insert at playhead on stop"));
    m_insertCheck->setChecked(true);
    mainLayout->addWidget(m_insertCheck);

    // Buttons
    auto *buttonRow = new QHBoxLayout();
    m_recordButton = new QPushButton(tr("Record"));
    m_recordButton->setMinimumWidth(100);
    connect(m_recordButton, &QPushButton::clicked, this, &VoiceOverDialog::startRecording);
    buttonRow->addWidget(m_recordButton);

    m_stopButton = new QPushButton(tr("Stop"));
    m_stopButton->setMinimumWidth(100);
    m_stopButton->setEnabled(false);
    connect(m_stopButton, &QPushButton::clicked, this, &VoiceOverDialog::stopRecording);
    buttonRow->addWidget(m_stopButton);

    auto *cancelButton = new QPushButton(tr("Cancel"));
    cancelButton->setMinimumWidth(100);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonRow->addWidget(cancelButton);

    mainLayout->addLayout(buttonRow);

    // Timers
    m_countdownTimer = new QTimer(this);
    m_countdownTimer->setInterval(1000);
    m_countdownTimer->setSingleShot(false);
    connect(m_countdownTimer, &QTimer::timeout, this, &VoiceOverDialog::onCountdownTick);

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(500);
    m_blinkTimer->setSingleShot(false);
    connect(m_blinkTimer, &QTimer::timeout, this, &VoiceOverDialog::onRecordIndicatorTick);

    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(200);
    m_elapsedTimer->setSingleShot(false);
    connect(m_elapsedTimer, &QTimer::timeout, this, &VoiceOverDialog::onElapsedTick);
}

void VoiceOverDialog::populateDevices()
{
    m_deviceCombo->clear();
    const auto devices = QMediaDevices::audioInputs();
    if (devices.isEmpty()) {
        m_deviceCombo->addItem(tr("No audio input devices found"));
        return;
    }
    for (const auto &dev : devices) {
        m_deviceCombo->addItem(dev.description());
    }
}

QAudioDevice VoiceOverDialog::selectedDevice() const
{
    const auto devices = QMediaDevices::audioInputs();
    int idx = m_deviceCombo->currentIndex();
    if (idx >= 0 && idx < devices.size())
        return devices.at(idx);
    return QMediaDevices::defaultAudioInput();
}

void VoiceOverDialog::setPhase(Phase phase)
{
    m_phase = phase;

    switch (phase) {
    case Phase::Idle:
        m_countdownLabel->clear();
        m_recordIndicator->setStyleSheet(
            "QLabel { background-color: #555555; border-radius: 10px; }");
        m_elapsedLabel->setText("00:00");
        m_levelBar->setValue(0);
        m_recordButton->setEnabled(true);
        m_stopButton->setEnabled(false);
        m_deviceCombo->setEnabled(true);
        m_blinkTimer->stop();
        m_elapsedTimer->stop();
        m_countdownTimer->stop();
        break;

    case Phase::Countdown:
        m_countdownValue = 3;
        m_countdownLabel->setText("3");
        m_recordButton->setEnabled(false);
        m_stopButton->setEnabled(false);
        m_deviceCombo->setEnabled(false);
        m_countdownTimer->start();
        m_blinkTimer->stop();
        m_elapsedTimer->stop();
        break;

    case Phase::Recording:
        m_countdownLabel->clear();
        m_recordButton->setEnabled(false);
        m_stopButton->setEnabled(true);
        m_deviceCombo->setEnabled(false);
        m_countdownTimer->stop();
        m_blinkTimer->start();
        m_elapsedTimer->start();
        m_elapsedMs = 0;
        m_elapsedLabel->setText("00:00");
        break;

    case Phase::Done:
        m_countdownLabel->clear();
        m_recordIndicator->setStyleSheet(
            "QLabel { background-color: #555555; border-radius: 10px; }");
        m_recordButton->setEnabled(false);
        m_stopButton->setEnabled(false);
        m_blinkTimer->stop();
        m_elapsedTimer->stop();
        break;
    }
}

void VoiceOverDialog::startRecording()
{
    const auto devices = QMediaDevices::audioInputs();
    if (devices.isEmpty()) {
        QMessageBox::warning(this, tr("Voice-over Record"),
                             tr("No audio input devices available."));
        return;
    }

    QString path = m_pathEdit->text().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Voice-over Record"),
                             tr("Please specify an output file path."));
        return;
    }

    // Ensure .wav extension
    if (!path.endsWith(".wav", Qt::CaseInsensitive))
        path += ".wav";
    m_pathEdit->setText(path);

    // Countdown first; actual recording starts in onCountdownTick
    setPhase(Phase::Countdown);
}

void VoiceOverDialog::stopRecording()
{
    if (m_recorder->isRecording()) {
        m_recorder->stopRecording();
    }
}

void VoiceOverDialog::onBrowsePath()
{
    QString dir = QDir::homePath();
    QString current = m_pathEdit->text();
    if (!current.isEmpty())
        dir = QFileInfo(current).absolutePath();

    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Voice-over"), dir,
        tr("WAV Files (*.wav)"));
    if (!path.isEmpty()) {
        m_pathEdit->setText(path);
    }
}

void VoiceOverDialog::onCountdownTick()
{
    m_countdownValue--;
    if (m_countdownValue > 0) {
        m_countdownLabel->setText(QString::number(m_countdownValue));
    } else {
        m_countdownTimer->stop();
        m_countdownLabel->clear();
        // Start actual recording
        const auto devices = QMediaDevices::audioInputs();
        int idx = m_deviceCombo->currentIndex();
        QAudioDevice device = (idx >= 0 && idx < devices.size())
                                  ? devices.at(idx)
                                  : QMediaDevices::defaultAudioInput();
        QString path = m_pathEdit->text().trimmed();
        m_recorder->startRecording(path, device, 48000, 1);
        setPhase(Phase::Recording);
        emit recordingStarted();
    }
}

void VoiceOverDialog::onRecordIndicatorTick()
{
    m_indicatorOn = !m_indicatorOn;
    if (m_indicatorOn) {
        m_recordIndicator->setStyleSheet(
            "QLabel { background-color: #ff2020; border-radius: 10px; }");
    } else {
        m_recordIndicator->setStyleSheet(
            "QLabel { background-color: #555555; border-radius: 10px; }");
    }
}

void VoiceOverDialog::onLevelChanged(double rmsDb)
{
    // Map -40dB..0dB to 0..100
    int value = qRound((rmsDb + 40.0) / 40.0 * 100.0);
    value = qMax(0, qMin(100, value));
    m_levelBar->setValue(value);

    // Color: green -> yellow -> red
    if (value < 60) {
        m_levelBar->setStyleSheet(
            "QProgressBar::chunk { background-color: #4CAF50; }");
    } else if (value < 85) {
        m_levelBar->setStyleSheet(
            "QProgressBar::chunk { background-color: #FFC107; }");
    } else {
        m_levelBar->setStyleSheet(
            "QProgressBar::chunk { background-color: #F44336; }");
    }
}

void VoiceOverDialog::onRecordingFinished(const QString &wavPath, qint64 elapsedMs)
{
    m_elapsedMs = elapsedMs;
    setPhase(Phase::Done);
    // Store result for retrieval after accept()
    m_resultWavPath = wavPath;
    m_resultElapsedMs = elapsedMs;
    accept();
}

void VoiceOverDialog::onRecordingFailed(const QString &errorMessage)
{
    setPhase(Phase::Idle);
    QMessageBox::warning(this, tr("Recording Error"), errorMessage);
}

void VoiceOverDialog::onElapsedTick()
{
    m_elapsedMs += 200;
    m_elapsedLabel->setText(formatElapsed(m_elapsedMs));
}

QString VoiceOverDialog::formatElapsed(qint64 ms) const
{
    qint64 totalSec = ms / 1000;
    qint64 minutes = totalSec / 60;
    qint64 seconds = totalSec % 60;
    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

void VoiceOverDialog::done(int result)
{
    if (result == Accepted && !m_resultWavPath.isEmpty()) {
        emit recordingStopped(m_resultWavPath, m_resultElapsedMs);
    }
    QDialog::done(result);
}

} // namespace voiceover
