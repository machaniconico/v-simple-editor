#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QRect>
#include <QElapsedTimer>
#include <QTimer>

// --- Recording configuration ---

struct RecordingConfig {
    int fps = 30;                   // frames per second
    int quality = 23;               // CRF value (0-51, lower = better quality)
    bool captureAudio = true;       // capture system audio
    bool captureMouse = true;       // show mouse cursor in recording
    QRect region;                   // empty = fullscreen, otherwise capture region
    QString outputPath;             // output file path
    QString format = "mp4";         // output format (mp4, mkv, etc.)
};

// --- Screen/display info for selection ---

struct ScreenInfo {
    int index = 0;
    QString name;
    QRect geometry;
};

// --- Screen Recorder (screen capture via FFmpeg) ---

class ScreenRecorder : public QObject
{
    Q_OBJECT

public:
    explicit ScreenRecorder(QObject *parent = nullptr);
    ~ScreenRecorder();

    // Start screen capture with the given configuration
    void startRecording(const RecordingConfig &config);

    // Stop and finalize the recording
    void stopRecording();

    // Pause / resume (tracks pause time for duration display)
    void pauseRecording();
    void resumeRecording();

    // State queries
    bool isRecording() const { return m_recording; }
    bool isPaused() const { return m_paused; }

    // Elapsed recording time in seconds (excludes paused time)
    double recordingDuration() const;

    // List available screens/displays
    static QVector<ScreenInfo> availableScreens();

signals:
    void recordingStarted();
    void recordingStopped(const QString &outputPath);
    void recordingError(const QString &message);
    void durationChanged(double seconds);

private:
    // Build FFmpeg arguments for the current platform and config
    QStringList buildFFmpegArgs(const RecordingConfig &config) const;

    // Find FFmpeg binary
    static QString findFFmpegBinary();

    // Platform-specific input format and device
    static QString platformInputFormat();
    static QString platformInputDevice(int screenIndex, bool captureAudio);

    QProcess *m_process = nullptr;
    QElapsedTimer m_elapsed;
    QTimer *m_durationTimer = nullptr;

    RecordingConfig m_config;
    bool m_recording = false;
    bool m_paused = false;
    qint64 m_pausedElapsed = 0;     // accumulated time in ms before pauses
    qint64 m_pauseStartTime = 0;    // timestamp when pause began
};
