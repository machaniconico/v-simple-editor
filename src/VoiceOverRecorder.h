#pragma once

#include <QObject>
#include <QString>
#include <QFile>
#include <QAudioDevice>
#include <QAudioSource>
#include <QTimer>
#include <QElapsedTimer>
#include <QMutex>
#include <QVector>

namespace voiceover {

class VoiceOverRecorder : public QObject
{
    Q_OBJECT

public:
    explicit VoiceOverRecorder(QObject *parent = nullptr);
    ~VoiceOverRecorder() override;

    void startRecording(const QString &outPath, const QAudioDevice &device,
                        int sampleRate = 48000, int channels = 1);
    void stopRecording();
    bool isRecording() const { return m_recording; }

signals:
    void levelChanged(double rmsDb);
    void recordingFinished(const QString &wavPath, qint64 elapsedMs);
    void recordingFailed(const QString &errorMessage);

private:
    void writeWavHeader();
    void finalizeWavHeader();
    void computeAndEmitLevel();

    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_inputDevice = nullptr;
    QFile m_outputFile;
    qint64 m_dataSize = 0;
    int m_sampleRate = 48000;
    int m_channels = 1;
    bool m_recording = false;
    QElapsedTimer m_elapsed;
    QTimer m_levelTimer;
    QMutex m_mutex;

    // Level computation: accumulate samples between timer ticks
    QVector<qint16> m_levelBuffer;
    QMutex m_levelMutex;
};

} // namespace voiceover
