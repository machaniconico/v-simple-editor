#include "VideoStabilizer.h"
#include "WarpDistortion.h"
#include "libavcore/VideoFilterGraph.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QThread>
#include <QtGlobal>
#include <QDir>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr double kHomographySingularThreshold = 1e-12;
constexpr planartrack::Homography kIdentityHomography = {
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0
};
thread_local std::optional<std::string> s_lastDeshakeError;

bool isPlanarFrameIndexValid(const planartrack::PlanarTrack *track, int frameIndex)
{
    return track != nullptr
        && frameIndex >= 0
        && frameIndex < static_cast<int>(track->frames.size());
}

bool scaleFrameToQImagePadded(SwsContext *ctx,
                              const AVFrame *frame,
                              AVPixelFormat dstPixFmt,
                              QImage &image)
{
    if (!ctx || !frame || image.isNull())
        return false;

    const int rowBytes = av_image_get_linesize(dstPixFmt, image.width(), 0);
    if (rowBytes <= 0 || rowBytes > image.bytesPerLine())
        return false;

    uint8_t *tmpData[4] = { nullptr, nullptr, nullptr, nullptr };
    int tmpStride[4] = { 0, 0, 0, 0 };
    if (av_image_alloc(tmpData, tmpStride, image.width(), image.height(),
                       dstPixFmt, 64) < 0)
        return false;

    sws_scale(ctx, frame->data, frame->linesize, 0, frame->height,
              tmpData, tmpStride);
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(image.scanLine(y), tmpData[0] + y * tmpStride[0],
                    static_cast<std::size_t>(rowBytes));
    }
    av_freep(&tmpData[0]);
    return true;
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
    if (source.isNull()) {
        qWarning() << "VideoStabilizer::stabilizeFrame: null source image at frame" << frameIndex;
        return QImage();
    }

    if (m_model != Model::PlanarInversion || m_planarTrack == nullptr) {
        qDebug() << "VideoStabilizer::stabilizeFrame: no planar track, returning source unchanged at frame" << frameIndex;
        return source;
    }

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

// --- Filter builder ---

// Build deshake filter string for the in-process libavfilter path (US-B3).
//
// deshake is a single-pass filter (no .trf file). Compared to the vidstab
// two-pass approach it has fewer tuning knobs, so some StabilizerConfig
// fields cannot be expressed and are intentionally ignored:
//   - smoothing:     deshake has no explicit smoothing-window parameter
//                    (it implicitly smooths over the entire clip); ignored.
//   - zoom:          deshake has no zoom parameter; ignored.
//   - interpolation: deshake uses a fixed internal interpolation; ignored.
//
// Mapping rationale (approved trade-offs, values clamped to libavfilter limits):
//   shakiness (1-10) → rx / ry (search-range, 0-64):
//     Larger shakiness means larger expected motion, so we widen the block-
//     search range proportionally: rx = ry = clamp(shakiness * 6, 8, 64).
//     Factor 6 gives range [6,60], then clamped to [8,64].
//
//   accuracy (1-15) → search + blocksize:
//     Higher accuracy requests exhaustive search (search=0) and a finer
//     block grid (smaller blocksize). Below accuracy 8 we use the "less"
//     search method (search=1) for speed.
//     blocksize = clamp(64 - accuracy * 3, 8, 128)
//     accuracy=1  → blocksize=61, search=1  (fast, coarse)
//     accuracy=8  → blocksize=40, search=0  (accurate)
//     accuracy=15 → blocksize=19, search=0  (most accurate)
//
//   cropMode → edge (border-fill mode):
//     Keep  → edge=1 (fill with original pixels, no black border)
//     Crop  → edge=0 (fill with blank/black — caller is expected to crop)

QString VideoStabilizer::buildDeshakeFilter(const StabilizerConfig &config)
{
    // rx / ry: search range derived from shakiness.
    // libavfilter deshake REQUIRES rx and ry to be a multiple of 16
    // (range [0, 64]); a non-multiple value triggers
    // "rx must be a multiple of 16" / "Not yet implemented" errors at graph
    // build time. We round shakiness*6 to the nearest multiple of 16 and
    // clamp into [16, 64] so the search range scales monotonically with
    // shakiness while satisfying the filter's quantization constraint.
    const int rxryRounded = ((config.shakiness * 6 + 8) / 16) * 16;
    const int rxry = qBound(16, rxryRounded, 64);

    // search: 0=exhaustive (accurate), 1=less (fast)
    const int search = (config.accuracy >= 8) ? 0 : 1;

    // blocksize: finer blocks for higher accuracy. deshake accepts [4, 128];
    // we round to the nearest power-of-2-ish even value within range to avoid
    // edge-case rejections in motion estimation.
    const int blocksizeRaw = qBound(8, 64 - config.accuracy * 3, 128);
    const int blocksize = (blocksizeRaw / 2) * 2;

    // edge: border-fill mode
    // 0=blank, 1=original, 2=clamp, 3=mirror
    const int edge = (config.cropMode == StabCropMode::Crop) ? 0 : 1;

    return QString("deshake=rx=%1:ry=%2:edge=%3:blocksize=%4:search=%5")
               .arg(rxry)
               .arg(rxry)
               .arg(edge)
               .arg(blocksize)
               .arg(search);
}

// --- Translation path: deshake single-pass via libavcore::VideoFilterGraph ---
//
// PRD-B3 US-B3-3: the previous Translation path ran vidstabdetect followed by
// vidstabtransform as two ffmpeg.exe subprocess passes. Those filters require
// libvidstab, but the bundled avfilter-11.dll is built with libvidstab disabled
// and autodetect off — so vidstab is absent from the in-process libav. To fully
// eliminate QProcess(ffmpeg.exe) from VideoStabilizer, we switched to the
// single-pass deshake filter, which is included in the standard libavfilter
// build. deshake performs whole-clip smoothing implicitly and is qualitatively
// inferior to vidstab for large camera shake (no separate detection / smoothing
// budget, no zoom, fixed interpolation) — this is an approved trade-off
// documented at the top of buildDeshakeFilter().
bool VideoStabilizer::stabilizeDeshake(const QString &inputPath,
                                       const QString &outputPath,
                                       const StabilizerConfig &config)
{
    libavcore::VideoFilterRequest request;
    request.inputPath = inputPath.toStdString();
    request.outputPath = outputPath.toStdString();
    request.filterDescription = buildDeshakeFilter(config).toStdString();
    // videoCodecName="libx264" lets VideoFilterGraph's encoder fallback chain
    // resolve to h264_mf on Windows (PRD-B-MF). copyAudio=true passes the
    // source audio stream through unchanged (equivalent to the old vidstab
    // pass-2 audio-copy behaviour, now handled in-process).
    request.videoCodecName = "libx264";
    request.copyAudio = true;

    emit progressChanged(0);

    s_lastDeshakeError.reset();

    libavcore::VideoFilterGraph graph;
    auto err = graph.run(
        request,
        [this](int pct) {
            emit progressChanged(qBound(0, pct, 100));
        },
        [this]() {
            return m_cancelled;
        });

    if (err.has_value() || m_cancelled) {
        if (err.has_value())
            s_lastDeshakeError = *err;
        QFile::remove(outputPath);
        return false;
    }

    emit progressChanged(100);
    return true;
}

bool VideoStabilizer::stabilizePlanarInversion(const QString &inputPath, const QString &outputPath)
{
    // PRD-B2 US-B2-5: this PlanarInversion path is fully in-process. Decoding
    // uses libavcore::MediaDecoder and encoding uses libavcore::FrameEncoder
    // (no ffmpeg.exe subprocess). The Translation path now runs deshake
    // single-pass through libavcore::VideoFilterGraph (PRD-B3 US-B3-3).
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

    // --- Encoder: in-process FrameEncoder (PRD-B2 US-B2-5).
    // videoCodecName="libx264" lets FrameEncoder's fallback chain resolve to
    // h264_mf on Windows; audioSourcePath=inputPath copies the source audio
    // stream unchanged (in-process equivalent of the old subprocess audio
    // passthrough).
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
        if (!scaleFrameToQImagePadded(toRgbaCtx, frame, AV_PIX_FMT_RGBA, rawFrame)) {
            encodeFailed = true;
            break;
        }

        // Per-frame homography processing.
        // h264 output carries no alpha plane: the transparent crop frame
        // (applyTransparentCrop) and any area left uncovered by a non-identity
        // homography are flattened to black here. This is parity with the
        // behaviour before PRD-B2 (h264 mux, no alpha), not a regression.
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
    // with a truncated output file. We guard against this with a
    // frameIndex-based sanity check: zero frames decoded from a video stream,
    // or a large shortfall vs. the expected frame count, signals a mid-stream
    // abort and causes the output to be discarded.
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

        // Translation path: in-process deshake single-pass via
        // libavcore::VideoFilterGraph (PRD-B3 US-B3-3). No QProcess/ffmpeg.exe.
        const bool ok = stabilizeDeshake(inputPath, outputPath, config);
        if (!ok || m_cancelled) {
            QFile::remove(outputPath);
            const auto err = s_lastDeshakeError;
            if (m_cancelled) {
                emit stabilizeComplete(false, tr("スタビライズを中断しました"));
            } else if (err.has_value()) {
                emit stabilizeComplete(false,
                    tr("スタビライズに失敗しました: %1").arg(QString::fromStdString(*err)));
            } else {
                emit stabilizeComplete(false, tr("スタビライズに失敗しました: %1").arg(tr("不明なエラー")));
            }
            return;
        }

        emit stabilizeComplete(true, tr("スタビライズが完了しました"));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void VideoStabilizer::cancel()
{
    // VideoFilterGraph polls the cancelCheck callback we passed in
    // stabilizeDeshake(); stabilizePlanarInversion() polls m_cancelled at the
    // top of its decode loop. Both paths read this flag from the worker
    // thread, so a plain assign is sufficient.
    m_cancelled = true;
}
