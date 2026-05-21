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

extern "C" {
#include <libswscale/swscale.h>
}

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
    // PRD-B2 US-B2-5 documented limitation: the vidstab two-pass path
    // (buildDetectFilter/buildTransformFilter via runFFmpeg) intentionally
    // stays on QProcess(ffmpeg.exe). vidstabdetect/vidstabtransform require
    // libvidstab, but the bundled avfilter-11.dll is built with libvidstab
    // disabled and autodetect off, so both filters are absent from the
    // in-process libav. This mirrors PRD-B-MF keeping HDR10 export on an
    // ffmpeg.exe subprocess. Only the PlanarInversion path
    // (stabilizePlanarInversion) was moved in-process to libavcore.
    //
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
    // PRD-B2 US-B2-5: this PlanarInversion path is fully in-process. Decoding
    // uses libavcore::MediaDecoder and encoding uses libavcore::FrameEncoder
    // (no ffmpeg.exe subprocess). The vidstab two-pass path below stays on
    // QProcess(ffmpeg.exe) — see buildDetectFilter() for that documented limit.
    const std::string inputUtf8 = inputPath.toStdString();
    const std::string outputUtf8 = outputPath.toStdString();

    // --- Decoder: replaces the "source -> rawvideo rgba pipe" ffmpeg process ---
    libavcore::MediaDecoder decoder;
    if (decoder.open(inputUtf8, /*wantAudio=*/false).has_value())
        return false;
    if (!decoder.hasVideo())
        return false;

    const libavcore::VideoStreamProps vprops = decoder.videoProps();
    if (vprops.width <= 0 || vprops.height <= 0)
        return false;

    const double fpsValue = vprops.frameRate.den > 0 && vprops.frameRate.num > 0
        ? av_q2d(vprops.frameRate)
        : 30.0;
    const int fpsRounded = std::max(1, static_cast<int>(std::lround(fpsValue)));

    // --- Encoder: replaces the "rawvideo rgba pipe -> mux -c:a copy" process.
    // videoCodecName="libx264" lets FrameEncoder's fallback chain resolve to
    // h264_mf on Windows; audioSourcePath=inputPath reproduces the prior
    // "-map 1:a? -c:a copy" audio passthrough behaviour.
    libavcore::EncodeRequest request;
    request.width = vprops.width;
    request.height = vprops.height;
    request.fps = fpsRounded;
    request.outputPath = outputUtf8;
    request.audioSourcePath = inputUtf8;
    request.videoCodecName = "libx264";

    libavcore::FrameEncoder encoder;
    if (encoder.open(request).has_value())
        return false;

    emit progressChanged(0);

    int frameIndex = 0;
    const int totalFrames = m_planarTrack != nullptr
        ? static_cast<int>(m_planarTrack->frames.size())
        : 0;

    // Reusable swscale context for AVFrame (decoder pix_fmt) -> RGBA8888.
    SwsContext *toRgbaCtx = nullptr;
    int swsSrcW = 0;
    int swsSrcH = 0;
    int swsSrcFmt = AV_PIX_FMT_NONE;

    bool encodeFailed = false;

    while (true) {
        if (m_cancelled) {
            if (toRgbaCtx)
                sws_freeContext(toRgbaCtx);
            return false;
        }

        AVFrame *frame = decoder.nextVideoFrame();
        if (frame == nullptr)
            break;  // EOF or decode error

        // (Re)build the swscale context if frame geometry/format changed.
        if (toRgbaCtx == nullptr
            || frame->width != swsSrcW
            || frame->height != swsSrcH
            || frame->format != swsSrcFmt) {
            if (toRgbaCtx)
                sws_freeContext(toRgbaCtx);
            toRgbaCtx = sws_getContext(
                frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format),
                vprops.width, vprops.height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (toRgbaCtx == nullptr) {
                encodeFailed = true;
                break;
            }
            swsSrcW = frame->width;
            swsSrcH = frame->height;
            swsSrcFmt = frame->format;
        }

        QImage rawFrame(vprops.width, vprops.height, QImage::Format_RGBA8888);
        uint8_t *rgbaDest[1] = { rawFrame.bits() };
        int rgbaLinesize[1] = { static_cast<int>(rawFrame.bytesPerLine()) };
        sws_scale(toRgbaCtx, frame->data, frame->linesize, 0, frame->height,
                  rgbaDest, rgbaLinesize);

        // Per-frame homography processing — identical to the subprocess path.
        // h264 output carries no alpha plane: the transparent crop frame
        // (applyTransparentCrop) and any area left uncovered by a non-identity
        // homography are flattened to black here. This matches the pre-PRD-B2
        // subprocess path, whose encoder command had no -c:v and therefore
        // muxed h264 into the .mp4/.mov — so this is parity, not a regression.
        // The explicit RGB888 conversion makes that flattening visible at the
        // call site rather than relying on pushFrame()'s hidden internal one.
        const QImage stabilized = stabilizeFrame(rawFrame, frameIndex)
                                      .convertToFormat(QImage::Format_RGB888);

        if (!encoder.pushFrame(stabilized, frameIndex)) {
            encodeFailed = true;
            break;
        }

        ++frameIndex;
        if (totalFrames > 0) {
            const int pct = qBound(0, static_cast<int>(
                (static_cast<double>(frameIndex) / static_cast<double>(totalFrames)) * 100.0), 100);
            emit progressChanged(pct);
        }
    }

    if (toRgbaCtx)
        sws_freeContext(toRgbaCtx);

    if (encodeFailed) {
        encoder.finalize();
        QFile::remove(outputPath);
        return false;
    }

    // Decode-failure detection. libavcore::MediaDecoder::nextVideoFrame()
    // returns nullptr for BOTH a clean EOF and a fatal mid-stream decode
    // error, and videoEnded() does not distinguish them — so a corrupt input
    // would otherwise break the loop, finalize() cleanly and report success
    // with a truncated output file. The old subprocess path guarded against
    // this via the decoder QProcess exit status (decodeOk && encodeOk). We
    // restore that guarantee here with a frameIndex-based sanity check, with
    // no change to the Decode API.
    bool decodeFailed = false;
    if (frameIndex == 0) {
        // Not a single frame decoded from a stream that reported video.
        decodeFailed = true;
    } else if (totalFrames > 0) {
        // m_planarTrack->frames.size() is the authoritative frame count used
        // as totalFrames for progress. Minor drift (±a few frames) between
        // the track and the actual decode is tolerated; a large shortfall
        // means the decoder aborted mid-stream.
        const int shortfallTolerance =
            std::max(8, static_cast<int>(std::lround(totalFrames * 0.02)));
        if (frameIndex < totalFrames - shortfallTolerance)
            decodeFailed = true;
    }

    if (decodeFailed) {
        encoder.finalize();
        QFile::remove(outputPath);
        return false;
    }

    const bool encodeOk = !encoder.finalize().has_value();
    if (encodeOk)
        emit progressChanged(100);
    else
        QFile::remove(outputPath);
    return encodeOk;
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
