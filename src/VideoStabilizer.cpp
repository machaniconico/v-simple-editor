#include "VideoStabilizer.h"
#include <QThread>
#include <QDir>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QFileInfo>

VideoStabilizer::VideoStabilizer(QObject *parent)
    : QObject(parent)
{
}

// --- Filter builders ---

QString VideoStabilizer::buildDetectFilter(const StabilizerConfig &config,
                                           const QString &trfPath)
{
    // vidstabdetect: shakiness, accuracy, result (output .trf file)
    return QString("vidstabdetect=shakiness=%1:accuracy=%2:result='%3'")
               .arg(config.shakiness)
               .arg(config.accuracy)
               .arg(trfPath);
}

QString VideoStabilizer::buildTransformFilter(const StabilizerConfig &config,
                                              const QString &trfPath)
{
    // vidstabtransform: smoothing, crop, zoom, interpol, input (.trf file)
    QString crop = (config.cropMode == StabCropMode::Crop) ? "black" : "keep";
    QString interpol = (config.interpolation == StabInterpolation::Bilinear)
                           ? "bilinear" : "bicubic";

    return QString("vidstabtransform=smoothing=%1:crop=%2:zoom=%3:interpol=%4:input='%5'")
               .arg(config.smoothing)
               .arg(crop)
               .arg(config.zoom, 0, 'f', 1)
               .arg(interpol)
               .arg(trfPath);
}

// --- FFmpeg binary lookup ---

QString VideoStabilizer::findFFmpegBinary()
{
    QString path = QStandardPaths::findExecutable("ffmpeg");
    if (!path.isEmpty())
        return path;

    // Common macOS / Linux locations
    QStringList searchPaths = {"/usr/local/bin", "/opt/homebrew/bin", "/usr/bin"};
    path = QStandardPaths::findExecutable("ffmpeg", searchPaths);
    return path;
}

// --- Progress parsing from FFmpeg stderr ---

void VideoStabilizer::parseProgress(const QString &output, int progressBase, int progressSpan)
{
    // Parse total duration: "Duration: HH:MM:SS.ms"
    if (m_totalDuration <= 0.0) {
        static QRegularExpression durRe(R"(Duration:\s+(\d+):(\d+):(\d+)\.(\d+))");
        auto match = durRe.match(output);
        if (match.hasMatch()) {
            m_totalDuration = match.captured(1).toDouble() * 3600.0
                            + match.captured(2).toDouble() * 60.0
                            + match.captured(3).toDouble()
                            + match.captured(4).toDouble() / 100.0;
        }
    }

    // Parse current position: "time=HH:MM:SS.ms"
    if (m_totalDuration > 0.0) {
        static QRegularExpression timeRe(R"(time=(\d+):(\d+):(\d+)\.(\d+))");
        auto match = timeRe.match(output);
        if (match.hasMatch()) {
            double currentTime = match.captured(1).toDouble() * 3600.0
                               + match.captured(2).toDouble() * 60.0
                               + match.captured(3).toDouble()
                               + match.captured(4).toDouble() / 100.0;
            double ratio = qBound(0.0, currentTime / m_totalDuration, 1.0);
            int pct = progressBase + static_cast<int>(ratio * progressSpan);
            emit progressChanged(qBound(0, pct, 100));
        }
    }
}

// --- Run FFmpeg process synchronously (called from worker thread) ---

bool VideoStabilizer::runFFmpeg(const QStringList &args, int progressBase, int progressSpan)
{
    QString ffmpegBin = findFFmpegBinary();
    if (ffmpegBin.isEmpty())
        return false;

    m_process = new QProcess();

    connect(m_process, &QProcess::readyReadStandardError, this, [this, progressBase, progressSpan]() {
        QString output = m_process->readAllStandardError();
        parseProgress(output, progressBase, progressSpan);
    });

    m_process->start(ffmpegBin, args);
    m_process->waitForStarted();

    // Poll for completion or cancellation
    while (!m_process->waitForFinished(200)) {
        if (m_cancelled) {
            m_process->kill();
            m_process->waitForFinished(3000);
            m_process->deleteLater();
            m_process = nullptr;
            return false;
        }
    }

    bool success = (m_process->exitStatus() == QProcess::NormalExit
                    && m_process->exitCode() == 0);

    m_process->deleteLater();
    m_process = nullptr;
    return success;
}

// --- Public API ---

void VideoStabilizer::stabilize(const QString &inputPath, const QString &outputPath,
                                const StabilizerConfig &config)
{
    m_cancelled = false;
    m_totalDuration = 0.0;

    QThread *thread = QThread::create([this, inputPath, outputPath, config]() {
        // Create temp .trf file in system temp dir
        QString trfPath = QDir::tempPath() + "/vstab_transforms.trf";

        emit progressChanged(0);

        // --- Pass 1: Analyze with vidstabdetect ---
        QString detectFilter = buildDetectFilter(config, trfPath);
        QStringList pass1Args = {
            "-y", "-i", inputPath,
            "-vf", detectFilter,
            "-f", "null", "-"
        };

        bool pass1Ok = runFFmpeg(pass1Args, 0, 45);  // 0-45% for pass 1

        if (!pass1Ok || m_cancelled) {
            QFile::remove(trfPath);
            emit stabilizeComplete(false,
                m_cancelled ? "Stabilization cancelled" : "Analysis pass failed");
            return;
        }

        // Verify .trf file was created
        if (!QFileInfo::exists(trfPath)) {
            emit stabilizeComplete(false, "Transform file was not generated");
            return;
        }

        emit progressChanged(50);
        m_totalDuration = 0.0;  // reset for pass 2 parsing

        // --- Pass 2: Apply with vidstabtransform ---
        QString transformFilter = buildTransformFilter(config, trfPath);
        QStringList pass2Args = {
            "-y", "-i", inputPath,
            "-vf", transformFilter,
            "-c:a", "copy",
            outputPath
        };

        bool pass2Ok = runFFmpeg(pass2Args, 50, 50);  // 50-100% for pass 2

        // Clean up .trf file
        QFile::remove(trfPath);

        if (!pass2Ok || m_cancelled) {
            QFile::remove(outputPath);
            emit stabilizeComplete(false,
                m_cancelled ? "Stabilization cancelled" : "Transform pass failed");
            return;
        }

        emit progressChanged(100);
        emit stabilizeComplete(true, "Stabilization complete");
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void VideoStabilizer::analyzeOnly(const QString &inputPath, const StabilizerConfig &config)
{
    m_cancelled = false;
    m_totalDuration = 0.0;

    QThread *thread = QThread::create([this, inputPath, config]() {
        // Create temp .trf file
        QString baseName = QFileInfo(inputPath).completeBaseName();
        QString trfPath = QDir::tempPath() + "/" + baseName + "_transforms.trf";

        emit progressChanged(0);

        QString detectFilter = buildDetectFilter(config, trfPath);
        QStringList args = {
            "-y", "-i", inputPath,
            "-vf", detectFilter,
            "-f", "null", "-"
        };

        bool ok = runFFmpeg(args, 0, 100);

        if (!ok || m_cancelled) {
            QFile::remove(trfPath);
            emit stabilizeComplete(false,
                m_cancelled ? "Analysis cancelled" : "Analysis failed");
            return;
        }

        if (!QFileInfo::exists(trfPath)) {
            emit stabilizeComplete(false, "Transform file was not generated");
            return;
        }

        emit progressChanged(100);
        emit analysisComplete(trfPath);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void VideoStabilizer::cancel()
{
    m_cancelled = true;
    if (m_process) {
        m_process->kill();
    }
}
