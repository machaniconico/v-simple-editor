#include "VideoStabilizer.h"
#include "WarpDistortion.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QtGlobal>
#include <QDir>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kHomographySingularThreshold = 1e-12;
constexpr planartrack::Homography kIdentityHomography = {
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0
};

bool isPlanarFrameIndexValid(const planartrack::PlanarTrack *track, int frameIndex)
{
    return track != nullptr
        && frameIndex >= 0
        && frameIndex < static_cast<int>(track->frames.size());
}

}

VideoStabilizer::VideoStabilizer(QObject *parent)
    : QObject(parent)
{
    refreshAnchorHomography();
}

void VideoStabilizer::setModel(Model model)
{
    m_model = model;
}

void VideoStabilizer::setPlanarTrack(const planartrack::PlanarTrack *track)
{
    if (m_planarTrack == track)
        return;

    m_planarTrack = track;
    refreshAnchorHomography();
}

void VideoStabilizer::setPlanarStabilizeAnchorFrame(int frameIndex)
{
    const int clampedFrame = std::max(0, frameIndex);
    if (m_settings.planarStabilizeAnchorFrame == clampedFrame)
        return;

    m_settings.planarStabilizeAnchorFrame = clampedFrame;
    refreshAnchorHomography();
}

void VideoStabilizer::setOutputCropPercent(double percent)
{
    m_settings.outputCropPercent = qBound(0.0, percent, 25.0);
}

QImage VideoStabilizer::stabilizeFrame(const QImage &source, int frameIndex) const
{
    if (source.isNull())
        return QImage();

    if (m_model != Model::PlanarInversion || m_planarTrack == nullptr)
        return source;

    return applyPlanarInversion(source, frameIndex);
}

VideoStabilizer::VideoStreamInfo VideoStabilizer::probeVideoStream(const QString &inputPath)
{
    VideoStreamInfo info;
    const QString ffmpegBin = findFFmpegBinary();
    if (ffmpegBin.isEmpty())
        return info;

    QProcess probe;
    probe.start(ffmpegBin, QStringList{QStringLiteral("-hide_banner"),
                                       QStringLiteral("-i"),
                                       inputPath});
    if (!probe.waitForStarted(3000))
        return info;

    probe.closeWriteChannel();
    probe.waitForFinished(5000);

    const QString stderrOutput = QString::fromLocal8Bit(probe.readAllStandardError());
    const QStringList lines = stderrOutput.split(QLatin1Char('\n'));
    static const QRegularExpression sizeRe(R"((\d+)x(\d+))");
    static const QRegularExpression fpsRe(R"((\d+(?:\.\d+)?)\s+fps)");

    for (const QString &line : lines) {
        if (!line.contains(QStringLiteral("Video:")))
            continue;

        const QRegularExpressionMatch sizeMatch = sizeRe.match(line);
        if (sizeMatch.hasMatch()) {
            info.width = sizeMatch.captured(1).toInt();
            info.height = sizeMatch.captured(2).toInt();
        }

        const QRegularExpressionMatch fpsMatch = fpsRe.match(line);
        if (fpsMatch.hasMatch())
            info.fps = fpsMatch.captured(1).toDouble();

        if (info.width > 0 && info.height > 0 && info.fps > 0.0)
            break;
    }

    return info;
}

planartrack::Homography VideoStabilizer::invertHomographyWithFallback(
    const planartrack::Homography &H,
    bool &usedIdentityFallback)
{
    const double a = H[0];
    const double b = H[1];
    const double c = H[2];
    const double d = H[3];
    const double e = H[4];
    const double f = H[5];
    const double g = H[6];
    const double h = H[7];
    const double i = H[8];

    const double c00 = e * i - f * h;
    const double c01 = -(d * i - f * g);
    const double c02 = d * h - e * g;
    const double c10 = -(b * i - c * h);
    const double c11 = a * i - c * g;
    const double c12 = -(a * h - b * g);
    const double c20 = b * f - c * e;
    const double c21 = -(a * f - c * d);
    const double c22 = a * e - b * d;

    const double det = a * c00 + b * c01 + c * c02;
    if (std::abs(det) < kHomographySingularThreshold) {
        usedIdentityFallback = true;
        return kIdentityHomography;
    }

    usedIdentityFallback = false;
    const double invDet = 1.0 / det;
    return planartrack::Homography{
        c00 * invDet, c10 * invDet, c20 * invDet,
        c01 * invDet, c11 * invDet, c21 * invDet,
        c02 * invDet, c12 * invDet, c22 * invDet
    };
}

planartrack::Homography VideoStabilizer::multiplyHomographies(
    const planartrack::Homography &lhs,
    const planartrack::Homography &rhs)
{
    planartrack::Homography out = {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double value = 0.0;
            for (int k = 0; k < 3; ++k)
                value += lhs[row * 3 + k] * rhs[k * 3 + col];
            out[row * 3 + col] = value;
        }
    }
    return out;
}

bool VideoStabilizer::isIdentityHomography(const planartrack::Homography &H)
{
    return planartrack::isIdentityHomography(H, kHomographySingularThreshold);
}

void VideoStabilizer::refreshAnchorHomography()
{
    m_anchorHomography = kIdentityHomography;
    if (!isPlanarFrameIndexValid(m_planarTrack, m_settings.planarStabilizeAnchorFrame))
        return;

    m_anchorHomography = m_planarTrack->frames[static_cast<std::size_t>(
        m_settings.planarStabilizeAnchorFrame)].H;
}

QImage VideoStabilizer::applyPlanarInversion(const QImage &source, int frameIndex) const
{
    if (!isPlanarFrameIndexValid(m_planarTrack, frameIndex))
        return source;

    const planartrack::Homography &frameHomography =
        m_planarTrack->frames[static_cast<std::size_t>(frameIndex)].H;

    bool usedIdentityFallback = false;
    const planartrack::Homography inverted =
        invertHomographyWithFallback(frameHomography, usedIdentityFallback);
    Q_UNUSED(usedIdentityFallback);

    const planartrack::Homography applied =
        multiplyHomographies(m_anchorHomography, inverted);

    QImage result = source;
    if (!isIdentityHomography(applied)) {
        applyHomography(source, applied, result);
    }

    applyTransparentCrop(result, m_settings.outputCropPercent);
    return result;
}

void VideoStabilizer::applyTransparentCrop(QImage &image, double cropPercent)
{
    const double clampedPercent = qBound(0.0, cropPercent, 25.0);
    if (image.isNull() || clampedPercent <= 0.0)
        return;

    const int inset = static_cast<int>(std::floor(
        (clampedPercent / 100.0) * static_cast<double>(std::min(image.width(), image.height()))));
    if (inset <= 0)
        return;

    const QRect innerRect(inset, inset,
                          std::max(0, image.width() - inset * 2),
                          std::max(0, image.height() - inset * 2));

    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    if (inset > 0) {
        painter.fillRect(QRect(0, 0, image.width(), std::min(inset, image.height())), Qt::transparent);
        painter.fillRect(QRect(0, std::max(0, image.height() - inset), image.width(), inset), Qt::transparent);
        painter.fillRect(QRect(0, innerRect.top(), std::min(inset, image.width()), std::max(0, innerRect.height())),
                         Qt::transparent);
        painter.fillRect(QRect(std::max(0, image.width() - inset), innerRect.top(), inset,
                               std::max(0, innerRect.height())),
                         Qt::transparent);
    }
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

bool VideoStabilizer::stabilizePlanarInversion(const QString &inputPath, const QString &outputPath)
{
    const QString ffmpegBin = findFFmpegBinary();
    if (ffmpegBin.isEmpty())
        return false;

    const VideoStreamInfo info = probeVideoStream(inputPath);
    if (info.width <= 0 || info.height <= 0)
        return false;

    const double fps = info.fps > 0.0 ? info.fps : 30.0;
    const qsizetype frameBytes = static_cast<qsizetype>(info.width)
        * static_cast<qsizetype>(info.height) * 4;
    if (frameBytes <= 0)
        return false;

    QProcess decoder;
    decoder.setProcessChannelMode(QProcess::SeparateChannels);
    decoder.start(ffmpegBin, QStringList{
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-i"), inputPath,
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-vsync"), QStringLiteral("0"),
        QStringLiteral("-f"), QStringLiteral("rawvideo"),
        QStringLiteral("-pix_fmt"), QStringLiteral("rgba"),
        QStringLiteral("-")
    });
    if (!decoder.waitForStarted(5000))
        return false;

    m_process = new QProcess();
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        const QString output = QString::fromLocal8Bit(m_process->readAllStandardError());
        parseProgress(output, 0, 100);
    });

    m_process->start(ffmpegBin, QStringList{
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-f"), QStringLiteral("rawvideo"),
        QStringLiteral("-pix_fmt"), QStringLiteral("rgba"),
        QStringLiteral("-s:v"), QStringLiteral("%1x%2").arg(info.width).arg(info.height),
        QStringLiteral("-r"), QString::number(fps, 'f', 6),
        QStringLiteral("-i"), QStringLiteral("-"),
        QStringLiteral("-i"), inputPath,
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("1:a?"),
        QStringLiteral("-c:a"), QStringLiteral("copy"),
        outputPath
    });
    if (!m_process->waitForStarted(5000)) {
        decoder.kill();
        decoder.waitForFinished(3000);
        m_process->deleteLater();
        m_process = nullptr;
        return false;
    }

    emit progressChanged(0);
    QByteArray buffer;
    buffer.reserve(frameBytes * 2);
    int frameIndex = 0;
    const int totalFrames = m_planarTrack != nullptr
        ? static_cast<int>(m_planarTrack->frames.size())
        : 0;

    auto cleanupProcesses = [this, &decoder]() {
        if (m_process) {
            m_process->deleteLater();
            m_process = nullptr;
        }
        decoder.closeReadChannel(QProcess::StandardOutput);
    };

    while (true) {
        if (m_cancelled) {
            decoder.kill();
            decoder.waitForFinished(3000);
            if (m_process) {
                m_process->kill();
                m_process->waitForFinished(3000);
            }
            cleanupProcesses();
            return false;
        }

        buffer.append(decoder.readAllStandardOutput());
        while (buffer.size() < frameBytes && decoder.state() != QProcess::NotRunning) {
            if (!decoder.waitForReadyRead(200))
                break;
            buffer.append(decoder.readAllStandardOutput());
        }
        buffer.append(decoder.readAllStandardOutput());

        if (buffer.size() < frameBytes) {
            if (decoder.state() == QProcess::NotRunning)
                break;
            continue;
        }

        QByteArray frameData = buffer.left(frameBytes);
        buffer.remove(0, frameBytes);

        const QImage rawFrame(reinterpret_cast<const uchar *>(frameData.constData()),
                              info.width, info.height, QImage::Format_RGBA8888);
        const QImage stabilized = stabilizeFrame(rawFrame.copy(), frameIndex)
                                      .convertToFormat(QImage::Format_RGBA8888);
        const char *framePtr = reinterpret_cast<const char *>(stabilized.constBits());
        qsizetype remaining = static_cast<qsizetype>(stabilized.bytesPerLine())
            * static_cast<qsizetype>(stabilized.height());
        while (remaining > 0) {
            const qint64 written = m_process->write(framePtr, remaining);
            if (written <= 0) {
                decoder.kill();
                decoder.waitForFinished(3000);
                m_process->kill();
                m_process->waitForFinished(3000);
                cleanupProcesses();
                return false;
            }
            framePtr += written;
            remaining -= written;
            if (remaining > 0 && !m_process->waitForBytesWritten(5000)) {
                decoder.kill();
                decoder.waitForFinished(3000);
                m_process->kill();
                m_process->waitForFinished(3000);
                cleanupProcesses();
                return false;
            }
        }

        ++frameIndex;
        if (totalFrames > 0) {
            const int pct = qBound(0, static_cast<int>(
                (static_cast<double>(frameIndex) / static_cast<double>(totalFrames)) * 100.0), 100);
            emit progressChanged(pct);
        }
    }

    decoder.waitForFinished(5000);
    if (m_process) {
        m_process->closeWriteChannel();
        m_process->waitForFinished(-1);
    }

    const bool decodeOk = decoder.exitStatus() == QProcess::NormalExit && decoder.exitCode() == 0;
    const bool encodeOk = m_process
        && m_process->exitStatus() == QProcess::NormalExit
        && m_process->exitCode() == 0;

    cleanupProcesses();
    if (decodeOk && encodeOk)
        emit progressChanged(100);
    return decodeOk && encodeOk;
}

// --- Public API ---

void VideoStabilizer::stabilize(const QString &inputPath, const QString &outputPath,
                                const StabilizerConfig &config)
{
    m_cancelled = false;
    m_totalDuration = 0.0;

    QThread *thread = QThread::create([this, inputPath, outputPath, config]() {
        if (m_model == Model::PlanarInversion && m_planarTrack != nullptr) {
            const bool ok = stabilizePlanarInversion(inputPath, outputPath);
            if (!ok || m_cancelled) {
                QFile::remove(outputPath);
                emit stabilizeComplete(false,
                    m_cancelled ? "Stabilization cancelled" : "Planar inversion stabilization failed");
                return;
            }

            emit stabilizeComplete(true, "Stabilization complete");
            return;
        }

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
