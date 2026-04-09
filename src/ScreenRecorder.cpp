#include "ScreenRecorder.h"
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>

ScreenRecorder::ScreenRecorder(QObject *parent)
    : QObject(parent)
{
    m_durationTimer = new QTimer(this);
    m_durationTimer->setInterval(250);  // emit duration 4x per second
    connect(m_durationTimer, &QTimer::timeout, this, [this]() {
        emit durationChanged(recordingDuration());
    });
}

ScreenRecorder::~ScreenRecorder()
{
    if (m_recording) {
        stopRecording();
    }
}

// --- FFmpeg binary lookup ---

QString ScreenRecorder::findFFmpegBinary()
{
    QString path = QStandardPaths::findExecutable("ffmpeg");
    if (!path.isEmpty())
        return path;

    // Common macOS / Linux locations
    QStringList searchPaths = {"/usr/local/bin", "/opt/homebrew/bin", "/usr/bin"};
    path = QStandardPaths::findExecutable("ffmpeg", searchPaths);
    return path;
}

// --- Platform helpers ---

QString ScreenRecorder::platformInputFormat()
{
#if defined(Q_OS_MACOS)
    return "avfoundation";
#elif defined(Q_OS_WIN)
    return "gdigrab";
#else
    return "x11grab";
#endif
}

QString ScreenRecorder::platformInputDevice(int screenIndex, bool captureAudio)
{
#if defined(Q_OS_MACOS)
    // avfoundation: "screen_index:audio_index" or "screen_index:none"
    QString audioDevice = captureAudio ? "0" : "none";
    return QString("%1:%2").arg(screenIndex).arg(audioDevice);
#elif defined(Q_OS_WIN)
    Q_UNUSED(screenIndex)
    Q_UNUSED(captureAudio)
    return "desktop";
#else
    Q_UNUSED(captureAudio)
    // x11grab: ":display.screen+x,y"
    return QString(":0.%1").arg(screenIndex);
#endif
}

// --- Build FFmpeg arguments ---

QStringList ScreenRecorder::buildFFmpegArgs(const RecordingConfig &config) const
{
    QStringList args;
    args << "-y";  // overwrite output

    // --- Input format ---
    args << "-f" << platformInputFormat();

    // --- Frame rate ---
    args << "-framerate" << QString::number(config.fps);

    // --- Mouse cursor ---
#if defined(Q_OS_MACOS)
    args << "-capture_cursor" << (config.captureMouse ? "1" : "0");
#elif defined(Q_OS_WIN)
    args << "-draw_mouse" << (config.captureMouse ? "1" : "0");
#else
    args << "-draw_mouse" << (config.captureMouse ? "1" : "0");
#endif

    // --- Region capture (Windows/Linux: video_size + offset) ---
#if !defined(Q_OS_MACOS)
    if (!config.region.isEmpty()) {
        args << "-video_size"
             << QString("%1x%2").arg(config.region.width()).arg(config.region.height());
        args << "-offset_x" << QString::number(config.region.x());
        args << "-offset_y" << QString::number(config.region.y());
    }
#endif

    // --- Input device ---
    // Determine screen index (default 0)
    int screenIndex = 0;
    auto screens = QGuiApplication::screens();
    if (!config.region.isEmpty()) {
        // Find which screen contains the region center
        QPoint center = config.region.center();
        for (int i = 0; i < screens.size(); ++i) {
            if (screens[i]->geometry().contains(center)) {
                screenIndex = i;
                break;
            }
        }
    }

    args << "-i" << platformInputDevice(screenIndex, config.captureAudio);

    // --- Audio input (Windows/Linux) ---
#if defined(Q_OS_WIN)
    if (config.captureAudio) {
        args << "-f" << "dshow"
             << "-i" << "audio=virtual-audio-capturer";
    }
#elif !defined(Q_OS_MACOS)
    if (config.captureAudio) {
        args << "-f" << "pulse"
             << "-i" << "default";
    }
#endif

    // --- Encoding ---
    args << "-c:v" << "libx264";
    args << "-crf" << QString::number(config.quality);
    args << "-preset" << "ultrafast";
    args << "-pix_fmt" << "yuv420p";

    // --- Region crop filter on macOS (avfoundation captures full screen) ---
#if defined(Q_OS_MACOS)
    if (!config.region.isEmpty()) {
        QString cropFilter = QString("crop=%1:%2:%3:%4")
            .arg(config.region.width())
            .arg(config.region.height())
            .arg(config.region.x())
            .arg(config.region.y());
        args << "-vf" << cropFilter;
    }
#endif

    // --- Audio codec ---
    if (config.captureAudio) {
        args << "-c:a" << "aac" << "-b:a" << "128k";
    }

    // --- Output ---
    args << config.outputPath;

    return args;
}

// --- Public API ---

void ScreenRecorder::startRecording(const RecordingConfig &config)
{
    if (m_recording) {
        emit recordingError("Recording is already in progress");
        return;
    }

    // Validate output path
    if (config.outputPath.isEmpty()) {
        emit recordingError("Output path is not set");
        return;
    }

    // Ensure output directory exists
    QDir outputDir = QFileInfo(config.outputPath).absoluteDir();
    if (!outputDir.exists()) {
        if (!outputDir.mkpath(".")) {
            emit recordingError("Cannot create output directory: " + outputDir.path());
            return;
        }
    }

    QString ffmpegBin = findFFmpegBinary();
    if (ffmpegBin.isEmpty()) {
        emit recordingError("FFmpeg not found. Please install FFmpeg.");
        return;
    }

    m_config = config;

    // Build arguments
    QStringList args = buildFFmpegArgs(config);

    // Create and start process
    m_process = new QProcess(this);

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        Q_UNUSED(error)
        if (m_recording) {
            m_recording = false;
            m_paused = false;
            m_durationTimer->stop();
            emit recordingError("FFmpeg process error: " + m_process->errorString());
        }
    });

    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        Q_UNUSED(exitCode)
        Q_UNUSED(exitStatus)
        if (m_recording) {
            m_recording = false;
            m_paused = false;
            m_durationTimer->stop();
        }
        QString outputPath = m_config.outputPath;
        m_process->deleteLater();
        m_process = nullptr;
        emit recordingStopped(outputPath);
    });

    m_process->start(ffmpegBin, args);

    if (!m_process->waitForStarted(5000)) {
        QString errorMsg = m_process->errorString();
        m_process->deleteLater();
        m_process = nullptr;
        emit recordingError("Failed to start FFmpeg: " + errorMsg);
        return;
    }

    // Recording started successfully
    m_recording = true;
    m_paused = false;
    m_pausedElapsed = 0;
    m_pauseStartTime = 0;
    m_elapsed.start();
    m_durationTimer->start();

    emit recordingStarted();
}

void ScreenRecorder::stopRecording()
{
    if (!m_recording || !m_process)
        return;

    m_durationTimer->stop();
    m_recording = false;
    m_paused = false;

    // Send "q" to FFmpeg stdin for graceful shutdown
    m_process->write("q");
    m_process->closeWriteChannel();

    // Wait for process to finish gracefully
    if (!m_process->waitForFinished(5000)) {
        // Force kill if it doesn't stop
        m_process->kill();
        m_process->waitForFinished(3000);
    }
}

void ScreenRecorder::pauseRecording()
{
    if (!m_recording || m_paused)
        return;

    m_paused = true;
    m_pauseStartTime = m_elapsed.elapsed();
    m_durationTimer->stop();
}

void ScreenRecorder::resumeRecording()
{
    if (!m_recording || !m_paused)
        return;

    // Accumulate the paused duration
    qint64 pauseDuration = m_elapsed.elapsed() - m_pauseStartTime;
    m_pausedElapsed += pauseDuration;

    m_paused = false;
    m_durationTimer->start();
}

double ScreenRecorder::recordingDuration() const
{
    if (!m_recording)
        return 0.0;

    qint64 totalElapsed = m_elapsed.elapsed();

    if (m_paused) {
        // Currently paused: subtract accumulated pauses and current pause
        qint64 currentPause = totalElapsed - m_pauseStartTime;
        return static_cast<double>(totalElapsed - m_pausedElapsed - currentPause) / 1000.0;
    }

    // Running: subtract only accumulated pauses
    return static_cast<double>(totalElapsed - m_pausedElapsed) / 1000.0;
}

// --- Available screens ---

QVector<ScreenInfo> ScreenRecorder::availableScreens()
{
    QVector<ScreenInfo> result;

    QList<QScreen *> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        ScreenInfo info;
        info.index = i;
        info.name = screens[i]->name();
        info.geometry = screens[i]->geometry();
        result.append(info);
    }

    return result;
}
