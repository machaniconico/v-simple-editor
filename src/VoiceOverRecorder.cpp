#include "VoiceOverRecorder.h"
#include <QAudioFormat>
#include <QIODevice>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDebug>
#include <QtMath>
#include <cstdint>

namespace voiceover {

VoiceOverRecorder::VoiceOverRecorder(QObject *parent)
    : QObject(parent)
{
    m_levelTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_levelTimer, &QTimer::timeout, this, &VoiceOverRecorder::computeAndEmitLevel);
}

VoiceOverRecorder::~VoiceOverRecorder()
{
    if (m_recording)
        stopRecording();
}

void VoiceOverRecorder::startRecording(const QString &outPath, const QAudioDevice &device,
                                        int sampleRate, int channels)
{
    QMutexLocker locker(&m_mutex);

    if (m_recording) {
        emit recordingFailed(QStringLiteral("Already recording."));
        return;
    }

    m_sampleRate = sampleRate;
    m_channels = channels;
    m_dataSize = 0;
    m_levelBuffer.clear();

    // Ensure parent directory exists
    QFileInfo fi(outPath);
    QDir dir(fi.absolutePath());
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            emit recordingFailed(QStringLiteral("Cannot create output directory: ") + fi.absolutePath());
            return;
        }
    }

    m_outputFile.setFileName(outPath);
    if (!m_outputFile.open(QIODevice::WriteOnly)) {
        emit recordingFailed(QStringLiteral("Cannot write to file: ") + outPath);
        return;
    }

    // Write placeholder WAV header (44 bytes), will be finalized on stop
    writeWavHeader();

    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Int16);

    if (!device.isFormatSupported(format)) {
        m_outputFile.close();
        emit recordingFailed(QStringLiteral("Audio format not supported by device."));
        return;
    }

    m_audioSource = new QAudioSource(device, format, this);

    connect(m_audioSource, &QAudioSource::stateChanged, this,
            [this](QAudio::State state) {
                if (state == QAudio::StoppedState && m_recording) {
                    if (m_audioSource && m_audioSource->error() != QAudio::NoError) {
                        QString err;
                        switch (m_audioSource->error()) {
                        case QAudio::OpenError:
                            err = QStringLiteral("Audio device open error.");
                            break;
                        case QAudio::IOError:
                            err = QStringLiteral("Audio I/O error.");
                            break;
                        case QAudio::UnderrunError:
                            err = QStringLiteral("Audio underrun error.");
                            break;
                        case QAudio::FatalError:
                            err = QStringLiteral("Fatal audio error.");
                            break;
                        default:
                            err = QStringLiteral("Audio error occurred.");
                            break;
                        }
                        {
                            QMutexLocker locker(&m_mutex);
                            if (!m_recording)
                                return;
                            m_recording = false;
                        }

                        m_levelTimer.stop();
                        if (m_audioSource) {
                            m_audioSource->stop();
                            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 100);
                            delete m_audioSource;
                            m_audioSource = nullptr;
                            m_inputDevice = nullptr;
                        }

                        finalizeWavHeader();
                        m_outputFile.close();
                        emit recordingFailed(err);
                    }
                }
            });

    m_inputDevice = m_audioSource->start();
    if (!m_inputDevice) {
        delete m_audioSource;
        m_audioSource = nullptr;
        m_inputDevice = nullptr;
        m_outputFile.close();
        emit recordingFailed(QStringLiteral("Failed to start audio capture."));
        return;
    }

    // Connect readyRead to capture PCM data and accumulate level samples
    connect(m_inputDevice, &QIODevice::readyRead, this, [this]() {
        QMutexLocker lock(&m_mutex);
        if (!m_recording || !m_inputDevice) return;
        QByteArray data = m_inputDevice->readAll();
        if (data.isEmpty()) return;

        qint64 written = m_outputFile.write(data);
        if (written < 0) {
            qWarning() << "VoiceOverRecorder: write failed, stopping recording";
            QMetaObject::invokeMethod(this, [this]() {
                if (m_recording) stopRecording();
            }, Qt::QueuedConnection);
            return;
        }
        m_dataSize += written;

        // Accumulate samples for level computation
        const auto *samples = reinterpret_cast<const qint16 *>(data.constData());
        int numSamples = data.size() / static_cast<int>(sizeof(qint16));
        {
            QMutexLocker levelLock(&m_levelMutex);
            m_levelBuffer.reserve(m_levelBuffer.size() + numSamples);
            for (int i = 0; i < numSamples; ++i) {
                m_levelBuffer.append(samples[i]);
            }
        }
    });

    m_recording = true;
    m_elapsed.start();
    m_levelTimer.start(50);
}

void VoiceOverRecorder::stopRecording()
{
    {
        QMutexLocker locker(&m_mutex);
        if (!m_recording) return;
        m_recording = false;
    }

    m_levelTimer.stop();

    qint64 elapsedMs = m_elapsed.elapsed();

    if (m_audioSource) {
        m_audioSource->stop();
        // Process any queued readyRead signals before deleting
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 100);
        delete m_audioSource;
        m_audioSource = nullptr;
        m_inputDevice = nullptr;
    }

    // Finalize WAV header with actual data size
    finalizeWavHeader();
    m_outputFile.close();

    QString wavPath = m_outputFile.fileName();
    emit recordingFinished(wavPath, elapsedMs);
}

void VoiceOverRecorder::writeWavHeader()
{
    // RIFF header
    m_outputFile.write("RIFF", 4);
    quint32 fileSize = 0; // placeholder
    m_outputFile.write(reinterpret_cast<const char*>(&fileSize), 4);
    m_outputFile.write("WAVE", 4);

    // fmt chunk
    m_outputFile.write("fmt ", 4);
    quint32 fmtSize = 16;
    m_outputFile.write(reinterpret_cast<const char*>(&fmtSize), 4);
    quint16 audioFormat = 1; // PCM
    m_outputFile.write(reinterpret_cast<const char*>(&audioFormat), 2);
    quint16 numChannels = static_cast<quint16>(m_channels);
    m_outputFile.write(reinterpret_cast<const char*>(&numChannels), 2);
    quint32 sampleRate = static_cast<quint32>(m_sampleRate);
    m_outputFile.write(reinterpret_cast<const char*>(&sampleRate), 4);
    quint16 bitsPerSample = 16;
    quint32 byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    m_outputFile.write(reinterpret_cast<const char*>(&byteRate), 4);
    quint16 blockAlign = numChannels * (bitsPerSample / 8);
    m_outputFile.write(reinterpret_cast<const char*>(&blockAlign), 2);
    m_outputFile.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

    // data chunk header
    m_outputFile.write("data", 4);
    quint32 dataSize = 0; // placeholder
    m_outputFile.write(reinterpret_cast<const char*>(&dataSize), 4);
}

void VoiceOverRecorder::finalizeWavHeader()
{
    if (m_dataSize == 0) return;

    quint32 dataSize = static_cast<quint32>(m_dataSize);
    quint32 fileSize = dataSize + 36; // 44 - 8 (RIFF chunk size excludes first 8 bytes)

    m_outputFile.seek(4);
    m_outputFile.write(reinterpret_cast<const char*>(&fileSize), 4);
    m_outputFile.seek(40);
    m_outputFile.write(reinterpret_cast<const char*>(&dataSize), 4);
    m_outputFile.seek(m_outputFile.size());
}

void VoiceOverRecorder::computeAndEmitLevel()
{
    QMutexLocker lock(&m_levelMutex);

    if (m_levelBuffer.isEmpty()) {
        emit levelChanged(-60.0);
        return;
    }

    // Compute RMS
    double sumSq = 0.0;
    for (qint16 s : m_levelBuffer) {
        double normalized = static_cast<double>(s) / 32768.0;
        sumSq += normalized * normalized;
    }
    double rms = qSqrt(sumSq / m_levelBuffer.size());
    m_levelBuffer.clear();

    double rmsDb = (rms > 0.0) ? (20.0 * qLn(rms) / qLn(10.0)) : -60.0;
    // Clamp to [-60, 0]
    rmsDb = qMax(-60.0, qMin(0.0, rmsDb));

    emit levelChanged(rmsDb);
}

} // namespace voiceover
