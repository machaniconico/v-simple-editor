#include "SpeedRamp.h"
#include <QThread>
#include <QDir>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>

SpeedRamp::SpeedRamp(QObject *parent)
    : QObject(parent)
{
}

// --- Point management ---

void SpeedRamp::addPoint(double time, double speed, SpeedEasing easing)
{
    // Clamp speed to valid range
    speed = qBound(0.1, speed, 10.0);

    // Replace existing point at same time (within 1ms tolerance)
    for (int i = 0; i < m_config.points.size(); ++i) {
        if (std::abs(m_config.points[i].time - time) < 0.001) {
            m_config.points[i].speed = speed;
            m_config.points[i].easing = easing;
            return;
        }
    }

    SpeedPoint pt;
    pt.time = time;
    pt.speed = speed;
    pt.easing = easing;
    m_config.points.append(pt);

    // Keep sorted by time
    std::sort(m_config.points.begin(), m_config.points.end(),
        [](const SpeedPoint &a, const SpeedPoint &b) { return a.time < b.time; });
}

void SpeedRamp::removePoint(int index)
{
    if (index >= 0 && index < m_config.points.size())
        m_config.points.removeAt(index);
}

void SpeedRamp::clearPoints()
{
    m_config.points.clear();
}

// --- Easing functions ---

double SpeedRamp::easeIn(double t)
{
    return t * t;
}

double SpeedRamp::easeOut(double t)
{
    return t * (2.0 - t);
}

double SpeedRamp::easeInOut(double t)
{
    return t < 0.5 ? 2.0 * t * t : -1.0 + (4.0 - 2.0 * t) * t;
}

double SpeedRamp::applyEasing(double t, SpeedEasing easing)
{
    switch (easing) {
    case SpeedEasing::Linear:    return t;
    case SpeedEasing::EaseIn:    return easeIn(t);
    case SpeedEasing::EaseOut:   return easeOut(t);
    case SpeedEasing::EaseInOut: return easeInOut(t);
    }
    return t;
}

// --- Speed interpolation ---

double SpeedRamp::speedAtTime(const QVector<SpeedPoint> &points, double time)
{
    if (points.isEmpty()) return 1.0;
    if (points.size() == 1) return points[0].speed;

    // Before first point
    if (time <= points.first().time) return points.first().speed;
    // After last point
    if (time >= points.last().time) return points.last().speed;

    // Find surrounding points and interpolate
    for (int i = 0; i < points.size() - 1; ++i) {
        const auto &a = points[i];
        const auto &b = points[i + 1];
        if (time >= a.time && time <= b.time) {
            double t = (time - a.time) / (b.time - a.time);
            double easedT = applyEasing(t, a.easing);
            return a.speed + (b.speed - a.speed) * easedT;
        }
    }
    return 1.0;
}

// --- Visualization curve generation ---

QVector<QPair<double, double>> SpeedRamp::generateSpeedCurve(
    const SpeedRampConfig &config, double duration, int sampleCount)
{
    QVector<QPair<double, double>> curve;
    curve.reserve(sampleCount);

    if (sampleCount <= 1 || duration <= 0.0) {
        double spd = config.points.isEmpty() ? 1.0 : speedAtTime(config.points, 0.0);
        curve.append({0.0, spd});
        return curve;
    }

    double step = duration / (sampleCount - 1);
    for (int i = 0; i < sampleCount; ++i) {
        double t = i * step;
        double spd = speedAtTime(config.points, t);
        curve.append({t, spd});
    }
    return curve;
}

// --- New duration calculation ---

double SpeedRamp::calculateNewDuration(const SpeedRampConfig &config,
                                       double originalDuration)
{
    if (config.points.isEmpty() || originalDuration <= 0.0)
        return originalDuration;

    // Numerically integrate 1/speed over the original duration
    // Using Simpson's rule with fine sampling
    const int N = 1000;  // number of intervals (must be even)
    double h = originalDuration / N;
    double sum = 0.0;

    for (int i = 0; i <= N; ++i) {
        double t = i * h;
        double spd = speedAtTime(config.points, t);
        if (spd < 0.1) spd = 0.1;  // safety clamp

        double weight = 1.0;
        if (i == 0 || i == N)
            weight = 1.0;
        else if (i % 2 == 1)
            weight = 4.0;
        else
            weight = 2.0;

        sum += weight / spd;
    }

    return (h / 3.0) * sum;
}

// --- FFmpeg helpers ---

QString SpeedRamp::buildAtempoChain(double speed)
{
    // atempo filter accepts 0.5 to 100.0
    // For speeds outside this range, chain multiple atempo filters
    QStringList filters;

    if (speed < 0.5) {
        // Chain atempo=0.5 until remainder is >= 0.5
        double remaining = speed;
        while (remaining < 0.5) {
            filters.append("atempo=0.5");
            remaining /= 0.5;  // each 0.5 doubles the remaining factor
        }
        filters.append(QString("atempo=%1").arg(remaining, 0, 'f', 6));
    } else if (speed > 100.0) {
        // Chain atempo=100.0 until remainder is <= 100.0
        double remaining = speed;
        while (remaining > 100.0) {
            filters.append("atempo=100.0");
            remaining /= 100.0;
        }
        filters.append(QString("atempo=%1").arg(remaining, 0, 'f', 6));
    } else {
        filters.append(QString("atempo=%1").arg(speed, 0, 'f', 6));
    }

    return filters.join(",");
}

QString SpeedRamp::buildAudioFilter(double speed, bool preservePitch)
{
    if (preservePitch) {
        return buildAtempoChain(speed);
    }
    // Without pitch preservation: change sample rate
    // asetrate changes playback speed by altering sample rate
    return QString("asetrate=44100*%1,aresample=44100").arg(speed, 0, 'f', 6);
}

QString SpeedRamp::findFFmpegBinary()
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

void SpeedRamp::parseProgress(const QString &output, int progressBase, int progressSpan)
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

// --- Run FFmpeg process ---

bool SpeedRamp::runFFmpeg(const QStringList &args, int progressBase, int progressSpan)
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

// --- Public API: apply speed ramp ---

void SpeedRamp::applySpeedRamp(const QString &inputPath, const QString &outputPath,
                               const SpeedRampConfig &config)
{
    m_cancelled = false;
    m_totalDuration = 0.0;

    QThread *thread = QThread::create([this, inputPath, outputPath, config]() {
        emit progressChanged(0);

        if (config.points.isEmpty()) {
            emit rampComplete(false, "No speed points defined");
            return;
        }

        // Determine if all points share the same speed (constant speed shortcut)
        bool constantSpeed = true;
        double firstSpeed = config.points.first().speed;
        for (const auto &pt : config.points) {
            if (std::abs(pt.speed - firstSpeed) > 0.001) {
                constantSpeed = false;
                break;
            }
        }

        if (constantSpeed) {
            // --- Simple constant speed: single FFmpeg pass ---
            double speed = qBound(0.1, firstSpeed, 10.0);
            QString videoFilter = QString("setpts=PTS/%1").arg(speed, 0, 'f', 6);
            QString audioFilter = buildAudioFilter(speed, config.preservePitch);

            QStringList args = {
                "-y", "-i", inputPath,
                "-vf", videoFilter,
                "-af", audioFilter,
                outputPath
            };

            bool ok = runFFmpeg(args, 0, 100);

            if (!ok || m_cancelled) {
                QFile::remove(outputPath);
                emit rampComplete(false,
                    m_cancelled ? "Speed ramp cancelled" : "FFmpeg processing failed");
                return;
            }

            emit progressChanged(100);
            emit rampComplete(true, "Speed ramp applied successfully");
            return;
        }

        // --- Variable speed: split into segments, process each, concatenate ---
        QTemporaryDir tempDir;
        if (!tempDir.isValid()) {
            emit rampComplete(false, "Failed to create temporary directory");
            return;
        }

        // Build segment list from speed points
        // Each segment spans from points[i].time to points[i+1].time
        // with an average speed for that segment
        QVector<SpeedPoint> sortedPoints = config.points;
        std::sort(sortedPoints.begin(), sortedPoints.end(),
            [](const SpeedPoint &a, const SpeedPoint &b) { return a.time < b.time; });

        struct Segment {
            double startTime;
            double endTime;
            double avgSpeed;
        };
        QVector<Segment> segments;

        for (int i = 0; i < sortedPoints.size() - 1; ++i) {
            double start = sortedPoints[i].time;
            double end = sortedPoints[i + 1].time;
            if (end - start < 0.01) continue;  // skip negligible segments

            // Compute average speed over this segment by sampling
            const int samples = 50;
            double speedSum = 0.0;
            for (int s = 0; s <= samples; ++s) {
                double t = start + (end - start) * s / samples;
                speedSum += speedAtTime(sortedPoints, t);
            }
            double avgSpeed = qBound(0.1, speedSum / (samples + 1), 10.0);

            segments.append({start, end, avgSpeed});
        }

        if (segments.isEmpty()) {
            emit rampComplete(false, "No valid segments to process");
            return;
        }

        // Process each segment
        int totalSegments = segments.size();
        QStringList segmentFiles;
        QString concatListPath = tempDir.path() + "/concat_list.txt";

        for (int i = 0; i < totalSegments; ++i) {
            if (m_cancelled) {
                emit rampComplete(false, "Speed ramp cancelled");
                return;
            }

            const auto &seg = segments[i];
            QString segFile = tempDir.path() + QString("/seg_%1.mp4").arg(i, 4, 10, QChar('0'));
            double duration = seg.endTime - seg.startTime;

            QString videoFilter = QString("setpts=PTS/%1").arg(seg.avgSpeed, 0, 'f', 6);
            QString audioFilter = buildAudioFilter(seg.avgSpeed, config.preservePitch);

            QStringList args = {
                "-y",
                "-ss", QString::number(seg.startTime, 'f', 6),
                "-t", QString::number(duration, 'f', 6),
                "-i", inputPath,
                "-vf", videoFilter,
                "-af", audioFilter,
                segFile
            };

            int progressBase = static_cast<int>(80.0 * i / totalSegments);
            int progressSpan = static_cast<int>(80.0 / totalSegments);
            m_totalDuration = 0.0;

            bool ok = runFFmpeg(args, progressBase, progressSpan);
            if (!ok || m_cancelled) {
                emit rampComplete(false,
                    m_cancelled ? "Speed ramp cancelled"
                                : QString("Failed to process segment %1").arg(i + 1));
                return;
            }

            segmentFiles.append(segFile);
        }

        // Write concat demuxer list file
        {
            QFile listFile(concatListPath);
            if (!listFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                emit rampComplete(false, "Failed to create concat list file");
                return;
            }
            QTextStream out(&listFile);
            for (const auto &f : segmentFiles) {
                out << "file '" << f << "'\n";
            }
        }

        emit progressChanged(85);

        // Concatenate segments
        m_totalDuration = 0.0;
        QStringList concatArgs = {
            "-y",
            "-f", "concat",
            "-safe", "0",
            "-i", concatListPath,
            "-c", "copy",
            outputPath
        };

        bool concatOk = runFFmpeg(concatArgs, 85, 15);

        if (!concatOk || m_cancelled) {
            QFile::remove(outputPath);
            emit rampComplete(false,
                m_cancelled ? "Speed ramp cancelled" : "Concatenation failed");
            return;
        }

        emit progressChanged(100);
        emit rampComplete(true, "Variable speed ramp applied successfully");
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void SpeedRamp::cancel()
{
    m_cancelled = true;
    if (m_process) {
        m_process->kill();
    }
}
