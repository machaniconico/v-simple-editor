#include "MotionStabilizer.h"

#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

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

    const int scaledRows = sws_scale(ctx, frame->data, frame->linesize, 0, frame->height,
                                     tmpData, tmpStride);
    if (scaledRows != image.height()) {
        av_freep(&tmpData[0]);
        return false;
    }

    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(image.scanLine(y), tmpData[0] + y * tmpStride[0],
                    static_cast<std::size_t>(rowBytes));
    }
    av_freep(&tmpData[0]);
    return true;
}

}

// ---------------------------------------------------------------------------
// MotionStabilizer (US-MS-1)
//
// Standalone analyser — see MotionStabilizer.h for the algorithm summary.
// NCC math intentionally mirrors MotionTracker::computeNCC so the two share
// behaviour (same mean-subtracted Pearson correlation, same denom guard),
// but the kernel is full-frame here (no template), so the inputs are two
// pre-aligned downscaled grayscale frames and the search slides one image
// over the other.
// ---------------------------------------------------------------------------

MotionStabilizer::MotionStabilizer(QObject *parent)
    : QObject(parent) {}

void MotionStabilizer::setSmoothness(double radius)
{
    m_smoothness = qBound(0.0, radius, 1.0);
}

void MotionStabilizer::setBordersAllowed(double frac)
{
    // Allow 0 (no clamp possible — output forced to zero) up to 0.5
    // (50% — anything more would crop more than half the frame).
    m_bordersAllowed = qBound(0.0, frac, 0.5);
}

void MotionStabilizer::reset()
{
    m_prevFrame = QImage();
    m_prevFrameWidth = 0;
    m_prevFrameHeight = 0;
    m_frames.clear();
    m_rawX.clear();
    m_rawY.clear();
    m_lastFrameIdx = -1;
}

// ---------------------------------------------------------------------------
// Public: processFrame
// ---------------------------------------------------------------------------

void MotionStabilizer::processFrame(int frameIndex, const QImage &frame)
{
    if (frame.isNull())
        return;

    QImage gray = toGrayDownscaled(frame);
    if (gray.isNull())
        return;

    StabilizationFrame entry;
    entry.frameIndex = frameIndex;

    if (m_prevFrame.isNull()) {
        // First frame — trajectory starts at the origin.
        entry.tx = 0.0;
        entry.ty = 0.0;
        m_rawX.append(0.0);
        m_rawY.append(0.0);
    } else {
        double dxDown = 0.0;
        double dyDown = 0.0;
        // Score is captured for completeness; we accept whatever the peak
        // is (caller can post-filter on confidence later if needed).
        (void)estimateTranslation(m_prevFrame, gray, dxDown, dyDown);

        // Map per-frame delta from downscaled coords back to source coords.
        // Use the SOURCE width/height of the previous frame as the reference
        // (per-frame deltas are inherently w.r.t. the prev->curr motion).
        const double sxRatio = (m_prevFrameWidth > 0)
            ? static_cast<double>(m_prevFrameWidth) / static_cast<double>(kMatchWidth)
            : 1.0;
        const double syRatio = (m_prevFrameHeight > 0)
            ? static_cast<double>(m_prevFrameHeight) / static_cast<double>(kMatchHeight)
            : 1.0;
        const double dxSrc = dxDown * sxRatio;
        const double dySrc = dyDown * syRatio;

        const double prevX = m_rawX.isEmpty() ? 0.0 : m_rawX.last();
        const double prevY = m_rawY.isEmpty() ? 0.0 : m_rawY.last();
        const double cumX = prevX + dxSrc;
        const double cumY = prevY + dySrc;
        m_rawX.append(cumX);
        m_rawY.append(cumY);

        entry.tx = cumX;
        entry.ty = cumY;
    }

    // smoothTx/smoothTy stay 0 until finalize() runs.
    entry.smoothTx = 0.0;
    entry.smoothTy = 0.0;
    m_frames.append(entry);

    m_prevFrame = gray;
    m_prevFrameWidth = frame.width();
    m_prevFrameHeight = frame.height();
    m_lastFrameIdx = frameIndex;

    emit progressUpdated(m_frames.size(), m_frames.size());
}

// ---------------------------------------------------------------------------
// Public: finalize
//
// Build the smoothed trajectory and write per-frame inverse counter-
// translations. Clamped to ± borders * frame dimension so a runaway
// smoothing residual cannot demand a crop the renderer cannot satisfy.
// Idempotent — recomputes from the current m_rawX/m_rawY each call.
// ---------------------------------------------------------------------------

void MotionStabilizer::finalize()
{
    const int n = m_frames.size();
    if (n == 0) {
        emit finished();
        return;
    }

    // sigma = N * smoothness * 0.1, clamped >= 0.5 (a sigma below half a
    // sample is effectively no smoothing — but we still want the path to
    // execute the conv loop deterministically). When smoothness == 0 we
    // bypass smoothing entirely so the inverse is exactly zero.
    const bool noSmoothing = (m_smoothness <= 0.0);
    const double sigma = noSmoothing
        ? 0.0
        : qMax(0.5, static_cast<double>(n) * m_smoothness * 0.1);

    QVector<double> smoothX = m_rawX;
    QVector<double> smoothY = m_rawY;
    if (!noSmoothing) {
        gaussianSmooth1D(smoothX, sigma);
        gaussianSmooth1D(smoothY, sigma);
    } else {
        // No smoothing — smooth curve == raw curve, so inverse == 0.
        // Keep smoothX/Y as the raw copy; the residual will be 0.
    }

    // Per-frame inverse = raw - smooth (counter-act jitter while preserving
    // the smoothed camera path). Clamp to ± borders * frame dim using the
    // last known source frame size as a reasonable proxy (all frames in a
    // single stabilization pass are expected to share a resolution).
    const double clampX = m_bordersAllowed *
        static_cast<double>(qMax(1, m_prevFrameWidth));
    const double clampY = m_bordersAllowed *
        static_cast<double>(qMax(1, m_prevFrameHeight));

    for (int i = 0; i < n; ++i) {
        double tx = m_rawX[i] - smoothX[i];
        double ty = m_rawY[i] - smoothY[i];
        tx = qBound(-clampX, tx, clampX);
        ty = qBound(-clampY, ty, clampY);
        m_frames[i].smoothTx = tx;
        m_frames[i].smoothTy = ty;
    }

    emit finished();
}

// ---------------------------------------------------------------------------
// Private: estimateTranslation — integer ±kSearchRadius NCC slide + 3x3
// parabolic refinement.
//
// Both inputs are expected to be the same dimensions (kMatchWidth x
// kMatchHeight Grayscale8). For each candidate (dx, dy) in
// [-kSearchRadius, kSearchRadius]^2 we compute Pearson correlation between
// prev and curr-shifted-by-(dx, dy), restricted to the overlap region.
//
// Return value is the peak NCC score; outTx/outTy are filled with the
// (sub-pixel-refined) per-frame delta in downscaled pixels. On total
// failure (all scores invalid) we fall through with outTx=outTy=0 and
// return -1.0.
// ---------------------------------------------------------------------------

double MotionStabilizer::estimateTranslation(const QImage &prev, const QImage &curr,
                                             double &outTx, double &outTy)
{
    outTx = 0.0;
    outTy = 0.0;

    if (prev.isNull() || curr.isNull())
        return -1.0;
    if (prev.width() != curr.width() || prev.height() != curr.height())
        return -1.0;
    if (prev.format() != QImage::Format_Grayscale8 ||
        curr.format() != QImage::Format_Grayscale8)
        return -1.0;

    int bestDx = 0;
    int bestDy = 0;
    double bestScore = -2.0;

    // Slide curr over prev in [-R, R].
    for (int dy = -kSearchRadius; dy <= kSearchRadius; ++dy) {
        for (int dx = -kSearchRadius; dx <= kSearchRadius; ++dx) {
            double s = computeShiftedNCC(prev, curr, dx, dy);
            if (s > bestScore) {
                bestScore = s;
                bestDx = dx;
                bestDy = dy;
            }
        }
    }

    if (bestScore < -1.0) {
        // No valid candidate at all (degenerate inputs) — leave outputs at 0.
        return -1.0;
    }

    outTx = static_cast<double>(bestDx);
    outTy = static_cast<double>(bestDy);

    // Sub-pixel refinement: 3x3 parabolic fit on the score surface around
    // the integer best — only when all 4 cardinal neighbours are inside
    // the search window. Mirrors MotionTracker US-MT-1.
    if (bestDx > -kSearchRadius && bestDx < kSearchRadius &&
        bestDy > -kSearchRadius && bestDy < kSearchRadius)
    {
        const double sCenter = bestScore;
        const double sLeft  = computeShiftedNCC(prev, curr, bestDx - 1, bestDy);
        const double sRight = computeShiftedNCC(prev, curr, bestDx + 1, bestDy);
        const double sUp    = computeShiftedNCC(prev, curr, bestDx,     bestDy - 1);
        const double sDown  = computeShiftedNCC(prev, curr, bestDx,     bestDy + 1);

        const double denomX = sLeft - 2.0 * sCenter + sRight;
        const double denomY = sUp   - 2.0 * sCenter + sDown;

        double subDx = (std::abs(denomX) < 1e-6) ? 0.0
                     : 0.5 * (sLeft - sRight) / denomX;
        double subDy = (std::abs(denomY) < 1e-6) ? 0.0
                     : 0.5 * (sUp   - sDown)  / denomY;

        subDx = qBound(-1.0, subDx, 1.0);
        subDy = qBound(-1.0, subDy, 1.0);

        outTx = static_cast<double>(bestDx) + subDx;
        outTy = static_cast<double>(bestDy) + subDy;
    }

    return bestScore;
}

// ---------------------------------------------------------------------------
// Private: computeShiftedNCC
//
// NCC between prev and curr, with curr translated by (offsetX, offsetY).
// Only the overlap region contributes — pixels outside the overlap on
// either side are excluded (this is the standard 'phase correlation lite'
// approach for full-frame motion estimation, equivalent to running NCC on
// the common rect).
// ---------------------------------------------------------------------------

double MotionStabilizer::computeShiftedNCC(const QImage &prev, const QImage &curr,
                                           int offsetX, int offsetY)
{
    const int W = prev.width();
    const int H = prev.height();

    // Overlap rect in prev coords:
    //   prev x in [max(0, offsetX), min(W, W + offsetX))
    //   prev y in [max(0, offsetY), min(H, H + offsetY))
    const int x0 = qMax(0, offsetX);
    const int y0 = qMax(0, offsetY);
    const int x1 = qMin(W, W + offsetX);
    const int y1 = qMin(H, H + offsetY);

    const int ow = x1 - x0;
    const int oh = y1 - y0;
    if (ow <= 0 || oh <= 0)
        return -1.0;

    const int count = ow * oh;
    if (count <= 1)
        return -1.0;

    // Compute means over the overlap region.
    double sumP = 0.0;
    double sumC = 0.0;
    for (int y = 0; y < oh; ++y) {
        const uchar *pRow = prev.constScanLine(y0 + y);
        const uchar *cRow = curr.constScanLine(y0 + y - offsetY);
        for (int x = 0; x < ow; ++x) {
            sumP += pRow[x0 + x];
            sumC += cRow[x0 + x - offsetX];
        }
    }
    const double meanP = sumP / count;
    const double meanC = sumC / count;

    // Compute Pearson correlation.
    double num = 0.0;
    double denP = 0.0;
    double denC = 0.0;
    for (int y = 0; y < oh; ++y) {
        const uchar *pRow = prev.constScanLine(y0 + y);
        const uchar *cRow = curr.constScanLine(y0 + y - offsetY);
        for (int x = 0; x < ow; ++x) {
            const double p = pRow[x0 + x] - meanP;
            const double c = cRow[x0 + x - offsetX] - meanC;
            num += p * c;
            denP += p * p;
            denC += c * c;
        }
    }

    const double den = std::sqrt(denP * denC);
    if (den < 1e-10)
        return 0.0;
    return num / den;
}

// ---------------------------------------------------------------------------
// Private: toGrayDownscaled
// ---------------------------------------------------------------------------

QImage MotionStabilizer::toGrayDownscaled(const QImage &src)
{
    if (src.isNull())
        return QImage();

    // Two-step: scale first, then convert format. Scaling on RGB then
    // converting to Grayscale8 produces marginally better luma than
    // converting first (Qt's convertToFormat to Grayscale8 uses ITU-R BT.601
    // weights, applied during the conversion either way). The cost
    // difference is negligible for 320x180 output.
    QImage scaled = src.scaled(kMatchWidth, kMatchHeight,
                               Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
    if (scaled.format() != QImage::Format_Grayscale8) {
        scaled = scaled.convertToFormat(QImage::Format_Grayscale8);
    }
    return scaled;
}

// ---------------------------------------------------------------------------
// Private: gaussianSmooth1D
//
// Direct convolution with a truncated Gaussian kernel. Edge handling is
// mirror reflection (sample[k] = xs[abs(k) % N] effectively, with bounce
// at both ends) — this avoids the smoothed curve at the boundaries from
// being pulled toward zero, which would re-introduce a jitter spike at
// frame 0 / N-1.
// ---------------------------------------------------------------------------

void MotionStabilizer::gaussianSmooth1D(QVector<double> &xs, double sigma)
{
    const int n = xs.size();
    if (n == 0 || sigma <= 0.0)
        return;

    // Truncate at ±3 sigma. Cap at n-1 so the kernel never exceeds the
    // signal length (otherwise mirror padding wraps multiple times).
    int halfWidth = static_cast<int>(std::ceil(3.0 * sigma));
    if (halfWidth < 1) halfWidth = 1;
    if (halfWidth > n - 1) halfWidth = n - 1;
    if (halfWidth < 1) {
        // n == 1 case — nothing to smooth.
        return;
    }

    QVector<double> kernel = buildGaussianKernel(sigma, halfWidth);

    QVector<double> out(n, 0.0);
    for (int i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int k = -halfWidth; k <= halfWidth; ++k) {
            int idx = i + k;
            // Mirror padding: reflect at boundaries. For idx < 0, idx = -idx;
            // for idx >= n, idx = 2*(n-1) - idx. Then re-clamp because
            // multiple reflections aren't handled (kernel halfWidth <= n-1
            // guarantees at most one reflection step).
            if (idx < 0)        idx = -idx;
            else if (idx >= n)  idx = 2 * (n - 1) - idx;
            if (idx < 0)        idx = 0;
            else if (idx >= n)  idx = n - 1;
            acc += xs[idx] * kernel[k + halfWidth];
        }
        out[i] = acc;
    }

    xs = out;
}

QVector<double> MotionStabilizer::buildGaussianKernel(double sigma, int halfWidth)
{
    const int len = 2 * halfWidth + 1;
    QVector<double> k(len, 0.0);
    const double twoSig2 = 2.0 * sigma * sigma;
    double sum = 0.0;
    for (int i = -halfWidth; i <= halfWidth; ++i) {
        const double v = std::exp(-static_cast<double>(i * i) / twoSig2);
        k[i + halfWidth] = v;
        sum += v;
    }
    if (sum > 0.0) {
        for (int i = 0; i < len; ++i)
            k[i] /= sum;
    }
    return k;
}

// ---------------------------------------------------------------------------
// US-INT-4: synchronous one-shot analyser. Decodes every video frame from
// `filePath` via libavformat, feeds processFrame()/finalize(), and packs
// the per-frame inverse counter-translations as StabilizerKeyframe (timeUs
// from frame PTS in microseconds). Translation-only model — theta=0,
// scale=1 since MotionStabilizer doesn't estimate rotation/zoom.
// ---------------------------------------------------------------------------

QVector<StabilizerKeyframe> MotionStabilizer::analyzeFile(const QString &filePath)
{
    reset();

    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return {};
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return {};
    }

    int vIdx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vIdx = static_cast<int>(i);
            break;
        }
    }
    if (vIdx < 0) {
        avformat_close_input(&fmt);
        return {};
    }

    AVStream *vs = fmt->streams[vIdx];
    const AVCodec *dec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!dec) {
        avformat_close_input(&fmt);
        return {};
    }
    AVCodecContext *cc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cc, vs->codecpar);
    if (avcodec_open2(cc, dec, nullptr) < 0) {
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return {};
    }

    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    SwsContext *sws = nullptr;
    int swsW = 0, swsH = 0;

    int frameIndex = 0;
    QVector<qint64> frameTimesUs;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vIdx) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(cc, pkt) >= 0) {
            while (avcodec_receive_frame(cc, frame) >= 0) {
                if (frame->width <= 0 || frame->height <= 0) {
                    av_frame_unref(frame);
                    continue;
                }
                if (!sws || swsW != frame->width || swsH != frame->height) {
                    if (sws) sws_freeContext(sws);
                    sws = sws_getContext(frame->width, frame->height,
                                         static_cast<AVPixelFormat>(frame->format),
                                         frame->width, frame->height,
                                         AV_PIX_FMT_RGB24,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
                    swsW = frame->width;
                    swsH = frame->height;
                }
                if (sws) {
                    QImage img(frame->width, frame->height, QImage::Format_RGB888);
                    if (scaleFrameToQImagePadded(sws, frame, AV_PIX_FMT_RGB24, img)) {
                        qint64 ptsUs = 0;
                        if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                            const AVRational tb = vs->time_base;
                            ptsUs = av_rescale_q(frame->best_effort_timestamp,
                                                 tb,
                                                 AVRational{1, 1000000});
                        } else {
                            ptsUs = frameIndex * 1000000LL / 30;
                        }
                        frameTimesUs.append(ptsUs);
                        processFrame(frameIndex, img);
                        ++frameIndex;
                    }
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(cc, nullptr);
    while (avcodec_receive_frame(cc, frame) >= 0) {
        if (frame->width > 0 && frame->height > 0) {
            if (!sws || swsW != frame->width || swsH != frame->height) {
                if (sws) sws_freeContext(sws);
                sws = sws_getContext(frame->width, frame->height,
                                     static_cast<AVPixelFormat>(frame->format),
                                     frame->width, frame->height,
                                     AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
                swsW = frame->width;
                swsH = frame->height;
            }
            if (sws) {
                QImage img(frame->width, frame->height, QImage::Format_RGB888);
                if (scaleFrameToQImagePadded(sws, frame, AV_PIX_FMT_RGB24, img)) {
                    qint64 ptsUs = 0;
                    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        ptsUs = av_rescale_q(frame->best_effort_timestamp,
                                             vs->time_base,
                                             AVRational{1, 1000000});
                    } else {
                        ptsUs = frameIndex * 1000000LL / 30;
                    }
                    frameTimesUs.append(ptsUs);
                    processFrame(frameIndex, img);
                    ++frameIndex;
                }
            }
        }
        av_frame_unref(frame);
    }

    if (sws) sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);

    finalize();

    QVector<StabilizerKeyframe> out;
    out.reserve(m_frames.size());
    for (int i = 0; i < m_frames.size(); ++i) {
        StabilizerKeyframe kf;
        kf.timeUs = (i < frameTimesUs.size()) ? frameTimesUs[i] : (i * 1000000LL / 30);
        kf.dx = m_frames[i].smoothTx;
        kf.dy = m_frames[i].smoothTy;
        kf.theta = 0.0;
        kf.scale = 1.0;
        out.append(kf);
    }
    return out;
}
