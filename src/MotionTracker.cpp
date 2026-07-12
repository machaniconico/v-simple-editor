#include "MotionTracker.h"
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cmath>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace {
QMutex g_runningTrackersMutex;
QSet<const MotionTracker *> g_runningTrackers;

bool markTrackerRunning(const MotionTracker *tracker)
{
    QMutexLocker locker(&g_runningTrackersMutex);
    if (g_runningTrackers.contains(tracker))
        return false;
    g_runningTrackers.insert(tracker);
    return true;
}

void markTrackerStopped(const MotionTracker *tracker)
{
    QMutexLocker locker(&g_runningTrackersMutex);
    g_runningTrackers.remove(tracker);
}
}

// ---------------------------------------------------------------------------
// TrackingResult
// ---------------------------------------------------------------------------

QRect TrackingResult::positionAtTime(double timeSec) const
{
    if (regions.isEmpty() || fps <= 0.0)
        return {};

    double frameIdx = timeSec * fps - startFrame;
    if (frameIdx <= 0.0)
        return regions.first().rect;
    if (frameIdx >= regions.size() - 1)
        return regions.last().rect;

    int lo = static_cast<int>(std::floor(frameIdx));
    int hi = lo + 1;
    double t = frameIdx - lo;

    const QRect &a = regions[lo].rect;
    const QRect &b = regions[hi].rect;

    // Linear interpolation
    int x = static_cast<int>(a.x() + (b.x() - a.x()) * t);
    int y = static_cast<int>(a.y() + (b.y() - a.y()) * t);
    int w = static_cast<int>(a.width() + (b.width() - a.width()) * t);
    int h = static_cast<int>(a.height() + (b.height() - a.height()) * t);

    return QRect(x, y, w, h);
}

// ---------------------------------------------------------------------------
// MotionTracker
// ---------------------------------------------------------------------------

MotionTracker::MotionTracker(QObject *parent)
    : QObject(parent) {}

void MotionTracker::setSearchMargin(int margin)
{
    m_searchMargin = qMax(10, margin);
}

void MotionTracker::setMinConfidence(double conf)
{
    m_minConfidence = qBound(0.0, conf, 1.0);
}

void MotionTracker::setKalmanEnabled(bool enabled)
{
    m_kalmanEnabled = enabled;
}

// ---------------------------------------------------------------------------
// US-MT-2: Constant-velocity Kalman smoothing of the sub-pixel center.
//
// State: x = [px, py, vx, vy]^T in source-frame pixel coords.
// Transition F (dt = 1 frame):
//     [1 0 1 0]
//     [0 1 0 1]
//     [0 0 1 0]
//     [0 0 0 1]
// Measurement H = [1 0 0 0; 0 1 0 0]  (position only).
// Process noise Q = diag(0.5, 0.5, 0.05, 0.05).
// Measurement noise R baseline = diag(1.0, 1.0); inflated 4x when
// matchScore < 0.7 * peakScoreSeenSoFar so a low-confidence frame cannot
// yank the smoothed track.
//
// All matrix math is hand-rolled — fixed 4x4 / 2x2 / 4x2 dimensions, no
// external dependency.
// ---------------------------------------------------------------------------

void MotionTracker::resetKalmanState()
{
    m_kalmanInitialized = false;
    for (int i = 0; i < 4; ++i) {
        m_kalmanState[i] = 0.0;
        for (int j = 0; j < 4; ++j) {
            m_kalmanCov[i][j] = 0.0;
        }
    }
    m_peakScoreSeenSoFar = 0.0;
}

QPointF MotionTracker::kalmanSmooth(const QPointF &measurement, double matchScore)
{
    // First-frame init: seed state from measurement, large initial uncertainty
    // so the first 2-3 frames near-fully accept measurements.
    if (!m_kalmanInitialized) {
        m_kalmanState[0] = measurement.x();
        m_kalmanState[1] = measurement.y();
        m_kalmanState[2] = 0.0;
        m_kalmanState[3] = 0.0;
        // P0 = diag(10, 10, 100, 100)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                m_kalmanCov[i][j] = 0.0;
        m_kalmanCov[0][0] = 10.0;
        m_kalmanCov[1][1] = 10.0;
        m_kalmanCov[2][2] = 100.0;
        m_kalmanCov[3][3] = 100.0;
        m_kalmanInitialized = true;
        m_peakScoreSeenSoFar = qMax(m_peakScoreSeenSoFar, matchScore);
        return measurement;
    }

    // --- Predict --------------------------------------------------------
    // x_pred = F * x_prev   (dt = 1)
    double xPred[4] = {
        m_kalmanState[0] + m_kalmanState[2],
        m_kalmanState[1] + m_kalmanState[3],
        m_kalmanState[2],
        m_kalmanState[3]
    };

    // P_pred = F P F^T + Q
    // F P first: row 0 = P[0] + P[2], row 1 = P[1] + P[3], rows 2,3 = P[2], P[3]
    double FP[4][4];
    for (int j = 0; j < 4; ++j) {
        FP[0][j] = m_kalmanCov[0][j] + m_kalmanCov[2][j];
        FP[1][j] = m_kalmanCov[1][j] + m_kalmanCov[3][j];
        FP[2][j] = m_kalmanCov[2][j];
        FP[3][j] = m_kalmanCov[3][j];
    }
    // (FP) F^T: col 0 = FP[*][0] + FP[*][2], col 1 = FP[*][1] + FP[*][3],
    //          col 2 = FP[*][2],             col 3 = FP[*][3]
    double Pp[4][4];
    for (int i = 0; i < 4; ++i) {
        Pp[i][0] = FP[i][0] + FP[i][2];
        Pp[i][1] = FP[i][1] + FP[i][3];
        Pp[i][2] = FP[i][2];
        Pp[i][3] = FP[i][3];
    }
    // + Q = diag(0.5, 0.5, 0.05, 0.05)
    Pp[0][0] += 0.5;
    Pp[1][1] += 0.5;
    Pp[2][2] += 0.05;
    Pp[3][3] += 0.05;

    // --- Adaptive R -----------------------------------------------------
    // Update peak BEFORE computing R so the very first low-conf frame after
    // a high-peak frame is treated as suspect. Then check ratio against the
    // peak prior to this frame's measurement (using the running max captured
    // here).
    const double peakBefore = m_peakScoreSeenSoFar;
    m_peakScoreSeenSoFar = qMax(m_peakScoreSeenSoFar, matchScore);

    double rDiag = 1.0;
    if (peakBefore > 0.0 && matchScore < 0.7 * peakBefore) {
        rDiag = 4.0;  // inflate R 4x for low-confidence frame
    }
    // US-MT-3: consume one-shot R multiplier (set to 2.0 at re-acquire).
    // Default 1.0 = identity with US-MT-2.
    rDiag *= m_oneShotRMultiplier;
    m_oneShotRMultiplier = 1.0;

    // --- Update ---------------------------------------------------------
    // S = H P_pred H^T + R   (2x2). With H = [I_2 | 0_2], H P H^T is the
    // top-left 2x2 of P_pred.
    double S00 = Pp[0][0] + rDiag;
    double S01 = Pp[0][1];
    double S10 = Pp[1][0];
    double S11 = Pp[1][1] + rDiag;

    double detS = S00 * S11 - S01 * S10;

    // Singular-matrix guard: skip update, keep predict-only state.
    if (std::abs(detS) < 1e-9) {
        for (int i = 0; i < 4; ++i) {
            m_kalmanState[i] = xPred[i];
            for (int j = 0; j < 4; ++j)
                m_kalmanCov[i][j] = Pp[i][j];
        }
        return QPointF(m_kalmanState[0], m_kalmanState[1]);
    }

    // S^-1 (2x2 inverse)
    double invDetS = 1.0 / detS;
    double Si00 =  S11 * invDetS;
    double Si01 = -S01 * invDetS;
    double Si10 = -S10 * invDetS;
    double Si11 =  S00 * invDetS;

    // K = P_pred H^T S^-1.  P_pred H^T is the first two columns of P_pred (4x2).
    // K is 4x2 with K[i][0] = Pp[i][0] * Si00 + Pp[i][1] * Si10
    //               K[i][1] = Pp[i][0] * Si01 + Pp[i][1] * Si11
    double K[4][2];
    for (int i = 0; i < 4; ++i) {
        K[i][0] = Pp[i][0] * Si00 + Pp[i][1] * Si10;
        K[i][1] = Pp[i][0] * Si01 + Pp[i][1] * Si11;
    }

    // Innovation y = z - H x_pred
    double y0 = measurement.x() - xPred[0];
    double y1 = measurement.y() - xPred[1];

    // x_post = x_pred + K y
    for (int i = 0; i < 4; ++i)
        m_kalmanState[i] = xPred[i] + K[i][0] * y0 + K[i][1] * y1;

    // P_post = (I - K H) P_pred. With H = [I_2 | 0_2], K H is 4x4 with
    // first two columns = K and last two columns = 0. So
    // (I - KH)[i][j] = (i == j ? 1 : 0) - (j < 2 ? K[i][j] : 0).
    // P_post[i][j] = sum_k (I-KH)[i][k] * Pp[k][j]
    //              = Pp[i][j] - K[i][0] * Pp[0][j] - K[i][1] * Pp[1][j]
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            m_kalmanCov[i][j] = Pp[i][j] - K[i][0] * Pp[0][j] - K[i][1] * Pp[1][j];

    return QPointF(m_kalmanState[0], m_kalmanState[1]);
}

// ---------------------------------------------------------------------------
// US-MT-3: predict-only state advance for occluded frames.
//
// Identical predict step to kalmanSmooth() (F, Q match exactly), but with no
// measurement consumption — the posterior is just the prediction. Used while
// the tracker is occluded so the smoothed track keeps coasting at the
// previously-estimated velocity instead of being yanked by garbage matches
// against the wrong subject.
// ---------------------------------------------------------------------------

QPointF MotionTracker::kalmanPredictOnly()
{
    if (!m_kalmanInitialized) {
        return QPointF(m_kalmanState[0], m_kalmanState[1]);
    }

    // x_pred = F * x_prev   (dt = 1)
    double xPred[4] = {
        m_kalmanState[0] + m_kalmanState[2],
        m_kalmanState[1] + m_kalmanState[3],
        m_kalmanState[2],
        m_kalmanState[3]
    };

    // P_pred = F P F^T + Q  (matches kalmanSmooth Q = diag(0.5,0.5,0.05,0.05))
    double FP[4][4];
    for (int j = 0; j < 4; ++j) {
        FP[0][j] = m_kalmanCov[0][j] + m_kalmanCov[2][j];
        FP[1][j] = m_kalmanCov[1][j] + m_kalmanCov[3][j];
        FP[2][j] = m_kalmanCov[2][j];
        FP[3][j] = m_kalmanCov[3][j];
    }
    double Pp[4][4];
    for (int i = 0; i < 4; ++i) {
        Pp[i][0] = FP[i][0] + FP[i][2];
        Pp[i][1] = FP[i][1] + FP[i][3];
        Pp[i][2] = FP[i][2];
        Pp[i][3] = FP[i][3];
    }
    Pp[0][0] += 0.5;
    Pp[1][1] += 0.5;
    Pp[2][2] += 0.05;
    Pp[3][3] += 0.05;

    // Commit predicted state — no update step.
    for (int i = 0; i < 4; ++i) {
        m_kalmanState[i] = xPred[i];
        for (int j = 0; j < 4; ++j)
            m_kalmanCov[i][j] = Pp[i][j];
    }

    return QPointF(m_kalmanState[0], m_kalmanState[1]);
}

// ---------------------------------------------------------------------------
// US-MT-3: 16-frame match-score median ring helpers.
// ---------------------------------------------------------------------------

void MotionTracker::pushRecentScore(double score)
{
    m_recentScores.append(score);
    if (m_recentScores.size() > 16)
        m_recentScores.removeFirst();
}

double MotionTracker::medianOfRecent() const
{
    if (m_recentScores.isEmpty())
        return 0.0;
    QVector<double> sorted = m_recentScores;
    std::sort(sorted.begin(), sorted.end());
    int n = sorted.size();
    if (n % 2 == 1)
        return sorted[n / 2];
    return 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
}

// ---------------------------------------------------------------------------
// Public: startTracking
// ---------------------------------------------------------------------------

void MotionTracker::startTracking(const QString &filePath, const QRect &initialRect)
{
    if (!markTrackerRunning(this))
        return;

    m_result = TrackingResult{};
    m_cancelRequested = false;
    // US-MT-2: clear Kalman filter for a fresh tracking run so the next
    // measurement seeds the state from scratch (P0 = diag(10,10,100,100)).
    resetKalmanState();

    // US-MT-3 / US-FIX-1: reset occlusion FSM state so a previous run's
    // occlusion history can't leak into this one. Centralized in
    // resetOcclusionState() so startTracking and cleanup share exactly the
    // same reset semantics.
    resetOcclusionState();

    auto *thread = QThread::create([this, filePath, initialRect]() {
        decodeAndTrack(filePath, initialRect);
        TrackingResult result = m_result;
        markTrackerStopped(this);
        emit trackingComplete(result);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MotionTracker::cancel()
{
    m_cancelRequested = true;
}

void MotionTracker::resetOcclusionState()
{
    m_occluded = false;
    m_occludedSince = -1;
    m_consecLowScoreFrames = 0;
    m_consecOccludedFrames = 0;
    m_postReacquireRamp = 0;
    m_baseSearchMargin = m_searchMargin;
    m_currentSearchMargin = m_searchMargin;
    m_lastConfidentTemplate = QImage();
    m_lastConfidentTemplateFrame = -1;
    m_recentScores.clear();
    m_trackingLostEmitted = false;
    m_oneShotRMultiplier = 1.0;
}

// ---------------------------------------------------------------------------
// Public: trackFrame — single-frame template matching
// ---------------------------------------------------------------------------

TrackingRegion MotionTracker::trackFrame(const QImage &currentFrame,
                                         const QImage &templateImage,
                                         const QRect &searchArea)
{
    TrackingRegion best;
    best.confidence = -1.0;

    QImage grayFrame = toGrayscale(currentFrame);
    QImage grayTempl = toGrayscale(templateImage);

    int tw = grayTempl.width();
    int th = grayTempl.height();

    // Clamp search area to frame bounds
    int sx = qMax(0, searchArea.x());
    int sy = qMax(0, searchArea.y());
    int ex = qMin(grayFrame.width() - tw, searchArea.x() + searchArea.width() - tw);
    int ey = qMin(grayFrame.height() - th, searchArea.y() + searchArea.height() - th);

    if (ex < sx || ey < sy)
        return best;

    // Slide template over search area, compute NCC at each position.
    int bestX = sx, bestY = sy;
    for (int y = sy; y <= ey; ++y) {
        for (int x = sx; x <= ex; ++x) {
            double score = computeNCC(grayFrame, grayTempl, x, y);
            if (score > best.confidence) {
                best.confidence = score;
                best.rect = QRect(x, y, tw, th);
                bestX = x;
                bestY = y;
            }
        }
    }

    // Sub-pixel refinement (US-MT-1): parabolic interpolation on the 3x3 NCC
    // neighbourhood. NCC is higher-is-better, so we fit a peak (not a valley).
    // Only refine when all 4 cardinal neighbours are inside the search window;
    // otherwise leave subPixelCenter default-constructed so callers fall back
    // to the integer center.
    if (best.confidence >= 0.0 &&
        bestX > sx && bestX < ex && bestY > sy && bestY < ey)
    {
        const double sCenter = best.confidence;
        const double sLeft   = computeNCC(grayFrame, grayTempl, bestX - 1, bestY);
        const double sRight  = computeNCC(grayFrame, grayTempl, bestX + 1, bestY);
        const double sUp     = computeNCC(grayFrame, grayTempl, bestX,     bestY - 1);
        const double sDown   = computeNCC(grayFrame, grayTempl, bestX,     bestY + 1);

        const double denomX = sLeft - 2.0 * sCenter + sRight;
        const double denomY = sUp   - 2.0 * sCenter + sDown;

        // Numerical safety: skip refinement when the peak is degenerate.
        double dx = (std::abs(denomX) < 1e-6) ? 0.0
                  : 0.5 * (sLeft - sRight) / denomX;
        double dy = (std::abs(denomY) < 1e-6) ? 0.0
                  : 0.5 * (sUp   - sDown)  / denomY;

        // Clamp to [-1.0, +1.0] to suppress runaway from noisy scores.
        dx = qBound(-1.0, dx, 1.0);
        dy = qBound(-1.0, dy, 1.0);

        const double cx = bestX + tw / 2.0 + dx;
        const double cy = bestY + th / 2.0 + dy;
        best.subPixelCenter = QPointF(cx, cy);
    }

    return best;
}

// ---------------------------------------------------------------------------
// Public: applyToOverlay
// ---------------------------------------------------------------------------

QRectF MotionTracker::applyToOverlay(const TrackingResult &trackingData,
                                     const QRectF &overlayRect,
                                     double currentTime,
                                     int videoWidth, int videoHeight)
{
    if (trackingData.isEmpty() || videoWidth <= 0 || videoHeight <= 0)
        return overlayRect;

    QRect tracked = trackingData.positionAtTime(currentTime);
    if (tracked.isNull())
        return overlayRect;

    // Convert tracked pixel position to normalized 0.0-1.0 coordinates
    double nx = static_cast<double>(tracked.x()) / videoWidth;
    double ny = static_cast<double>(tracked.y()) / videoHeight;

    // Offset overlay so its center follows the tracked center
    double cx = nx + static_cast<double>(tracked.width()) / (2.0 * videoWidth);
    double cy = ny + static_cast<double>(tracked.height()) / (2.0 * videoHeight);

    return QRectF(cx - overlayRect.width() / 2.0,
                  cy - overlayRect.height() / 2.0,
                  overlayRect.width(),
                  overlayRect.height());
}

// ---------------------------------------------------------------------------
// JSON export / import
// ---------------------------------------------------------------------------

bool MotionTracker::exportTrackingData(const TrackingResult &data, const QString &filePath)
{
    QJsonObject root;
    root["startFrame"] = data.startFrame;
    root["endFrame"] = data.endFrame;
    root["fps"] = data.fps;

    QJsonArray arr;
    for (const auto &r : data.regions) {
        QJsonObject obj;
        obj["x"] = r.rect.x();
        obj["y"] = r.rect.y();
        obj["w"] = r.rect.width();
        obj["h"] = r.rect.height();
        obj["confidence"] = r.confidence;
        obj["frame"] = r.frameNumber;
        // US-MT-1: persist sub-pixel center losslessly when it has been
        // refined. A null subPixelCenter (default-constructed QPointF) means
        // refinement was skipped; we omit the keys in that case so importers
        // reconstruct the same null state.
        if (!r.subPixelCenter.isNull()) {
            obj["subCx"] = r.subPixelCenter.x();
            obj["subCy"] = r.subPixelCenter.y();
        }
        arr.append(obj);
    }
    root["regions"] = arr;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

TrackingResult MotionTracker::importTrackingData(const QString &filePath)
{
    TrackingResult result;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return result;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return result;

    QJsonObject root = doc.object();
    result.startFrame = root["startFrame"].toInt();
    result.endFrame = root["endFrame"].toInt();
    result.fps = root["fps"].toDouble();

    QJsonArray arr = root["regions"].toArray();
    for (const auto &v : arr) {
        QJsonObject obj = v.toObject();
        TrackingRegion r;
        r.rect = QRect(obj["x"].toInt(), obj["y"].toInt(),
                        obj["w"].toInt(), obj["h"].toInt());
        r.confidence = obj["confidence"].toDouble();
        r.frameNumber = obj["frame"].toInt();
        // US-MT-1: restore sub-pixel center if it was persisted. Absence of
        // both keys means refinement was skipped — leave subPixelCenter
        // default-constructed so callers fall back to rect.center().
        if (obj.contains("subCx") && obj.contains("subCy")) {
            r.subPixelCenter = QPointF(obj["subCx"].toDouble(),
                                       obj["subCy"].toDouble());
        }
        result.regions.append(r);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Private: decodeAndTrack — FFmpeg frame extraction + tracking loop
// ---------------------------------------------------------------------------

bool MotionTracker::decodeAndTrack(const QString &filePath, const QRect &initialRect)
{
    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Find video stream
    int videoIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    auto *codecpar = fmtCtx->streams[videoIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmtCtx); return false; }

    AVCodecContext *decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decCtx, codecpar);
    if (avcodec_open2(decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    int frameW = decCtx->width;
    int frameH = decCtx->height;

    // SwsContext to convert decoded frames to RGB32 QImage
    SwsContext *swsCtx = sws_getContext(
        frameW, frameH, decCtx->pix_fmt,
        frameW, frameH, AV_PIX_FMT_RGB32,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVStream *stream = fmtCtx->streams[videoIdx];
    double fps = av_q2d(stream->avg_frame_rate);
    if (fps <= 0.0) fps = 25.0;

    // Estimate total frames for progress
    int64_t totalFrames = stream->nb_frames;
    if (totalFrames <= 0 && stream->duration > 0)
        totalFrames = static_cast<int64_t>(stream->duration * av_q2d(stream->time_base) * fps);
    if (totalFrames <= 0) totalFrames = 1;

    m_result.fps = fps;
    m_result.startFrame = 0;
    m_result.srcWidth = frameW;
    m_result.srcHeight = frameH;

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    QImage templateImage;   // extracted from first frame
    QRect lastRect = initialRect;
    int frameCount = 0;
    bool firstFrame = true;

    auto processDecodedFrame = [&]() -> bool {
        if (m_cancelRequested)
            return false;

        // Convert to QImage
        QImage qimg(frameW, frameH, QImage::Format_RGB32);
        uint8_t *dest[1] = { qimg.bits() };
        int destLinesize[1] = { static_cast<int>(qimg.bytesPerLine()) };
        sws_scale(swsCtx, frame->data, frame->linesize, 0,
                  frameH, dest, destLinesize);

        if (firstFrame) {
            // Extract template from initial rectangle
            QRect clamped = initialRect.intersected(qimg.rect());
            if (clamped.isEmpty()) {
                // Bad initial rect — abort
                return false;
            }
            templateImage = qimg.copy(clamped);
            lastRect = clamped;

            // US-MT-3: seed last-confident template cache with the
            // user-picked region. Confidence is by definition 1.0 on
            // the seed frame, so this also pre-loads peakScore.
            m_lastConfidentTemplate = templateImage;
            m_lastConfidentTemplateFrame = 0;

            TrackingRegion region;
            region.rect = clamped;
            region.confidence = 1.0;
            region.frameNumber = 0;
            // US-MT-2: seed Kalman with the user-picked center so the
            // first measurement on frame 1 has a valid prior state.
            if (m_kalmanEnabled) {
                QPointF seedCenter(clamped.x() + clamped.width() / 2.0,
                                   clamped.y() + clamped.height() / 2.0);
                QPointF smoothed = kalmanSmooth(seedCenter, 1.0);
                region.subPixelCenter = smoothed;
            }
            m_result.regions.append(region);

            firstFrame = false;
        } else {
            // US-MT-3: stop appending regions for the rest of the
            // timeline once trackingLost has fired. Still drain
            // decoder + advance frame counter so cleanup is clean.
            if (m_trackingLostEmitted) {
                frameCount++;
                int pct = static_cast<int>(100.0 * frameCount / totalFrames);
                emit progressChanged(qMin(pct, 100));
                return true;
            }

            // US-MT-3: build search area using the live (possibly
            // occlusion-expanded) margin. m_currentSearchMargin ==
            // m_baseSearchMargin in the no-occlusion path so behaviour
            // is bit-identical to US-MT-2 when occlusion never fires.
            const int searchMargin = m_currentSearchMargin;
            QRect search(
                lastRect.x() - searchMargin,
                lastRect.y() - searchMargin,
                lastRect.width() + 2 * searchMargin,
                lastRect.height() + 2 * searchMargin);

            // US-MT-3: while occluded, match against the last-confident
            // template (cached when score > 0.85 * peakScore). This
            // prevents the live template from drifting onto the
            // occluder. When NOT occluded, use the live template
            // (identity with US-MT-2).
            const QImage &activeTemplate =
                (m_occluded && !m_lastConfidentTemplate.isNull())
                    ? m_lastConfidentTemplate
                    : templateImage;

            TrackingRegion region = trackFrame(qimg, activeTemplate, search);
            region.frameNumber = frameCount;

            if (region.confidence >= m_minConfidence) {
                lastRect = region.rect;
            } else {
                // Low confidence — keep last known position
                region.rect = lastRect;
            }

            // ----- US-MT-3: occlusion FSM ----------------------------
            // Snapshot peak/median BEFORE this frame so threshold
            // checks use the prior running max. We do NOT update
            // m_peakScoreSeenSoFar here — that update is owned by
            // kalmanSmooth (US-MT-2 invariant). Only the median
            // ring is touched here. When kalmanSmooth is bypassed
            // (occluded path uses predict-only), the peak simply
            // does not grow with the occluder's garbage matches —
            // which is exactly the desired semantic.
            const double matchScore = region.confidence;
            const double peakBefore = m_peakScoreSeenSoFar;
            const double medianBefore = medianOfRecent();
            pushRecentScore(matchScore);

            // Cache last-confident template ONLY when this match is
            // close enough to the running peak. Crop from the match
            // rectangle (clamped to frame bounds for safety). Skip
            // when occluded — we don't want the cache to contaminate
            // with the occluder.
            if (!m_occluded && peakBefore > 0.0 &&
                matchScore > 0.85 * peakBefore)
            {
                QRect cacheRect = region.rect.intersected(qimg.rect());
                if (!cacheRect.isEmpty()) {
                    m_lastConfidentTemplate = qimg.copy(cacheRect);
                    m_lastConfidentTemplateFrame = frameCount;
                }
            }

            if (!m_occluded) {
                // --- Occlusion onset detection -----------------------
                // Rule A: matchScore < 0.3 * peakScore for 1 frame.
                // Rule B: matchScore < 0.5 * median for 2 consecutive frames.
                bool ruleA = (peakBefore > 0.0 &&
                              matchScore < 0.3 * peakBefore);
                bool ruleB = false;
                if (medianBefore > 0.0 && matchScore < 0.5 * medianBefore) {
                    m_consecLowScoreFrames++;
                    if (m_consecLowScoreFrames >= 2)
                        ruleB = true;
                } else {
                    m_consecLowScoreFrames = 0;
                }

                if (ruleA || ruleB) {
                    // Occlusion onset.
                    m_occluded = true;
                    m_occludedSince = frameCount;
                    m_consecOccludedFrames = 1;
                    m_consecLowScoreFrames = 0;
                    // Expand search radius to min(96, srcDim/2). srcDim
                    // = min(frameW, frameH) is the conservative bound.
                    const int srcDim = qMin(frameW, frameH);
                    m_currentSearchMargin = qMin(96, srcDim / 2);
                    if (m_currentSearchMargin < m_baseSearchMargin)
                        m_currentSearchMargin = m_baseSearchMargin;
                }
            } else {
                // --- Occluded: count frames + check re-acquire -------
                m_consecOccludedFrames++;

                // Hard-fail at 60 consecutive occluded frames.
                if (m_consecOccludedFrames >= 60 && !m_trackingLostEmitted) {
                    m_trackingLostEmitted = true;
                    emit trackingLost(frameCount);
                    // Don't append this frame's region — we're done.
                    frameCount++;
                    int pct = static_cast<int>(100.0 * frameCount / totalFrames);
                    emit progressChanged(qMin(pct, 100));
                    return true;
                }

                // Re-acquire when score recovers to > 0.7 * peak.
                if (peakBefore > 0.0 && matchScore > 0.7 * peakBefore) {
                    m_occluded = false;
                    m_occludedSince = -1;
                    m_consecOccludedFrames = 0;
                    // One-shot R reset: R = baseline * 2.0 for this
                    // single frame so the smoothed track interpolates
                    // gradually toward the recovered measurement
                    // instead of snapping to it. The next call to
                    // kalmanSmooth (below, since !m_occluded now)
                    // multiplies its rDiag by m_oneShotRMultiplier
                    // and resets it back to 1.0. Adaptive R inside
                    // kalmanSmooth picks baseline (1.0) for this
                    // frame because matchScore > 0.7 * peakBefore,
                    // so final R = 1.0 * 2.0 = 2.0 as specified.
                    m_oneShotRMultiplier = 2.0;
                    m_postReacquireRamp = 8;
                }
            }

            // --- Search-margin contraction ramp (post re-acquire) ---
            if (m_postReacquireRamp > 0 && !m_occluded) {
                // Linear contract from current → base over 8 frames.
                // ramp counts down: 8,7,6,5,4,3,2,1, then back to base.
                int rampLeft = m_postReacquireRamp;
                const int srcDim = qMin(frameW, frameH);
                const int expanded = qMax(m_baseSearchMargin,
                                          qMin(96, srcDim / 2));
                const int delta = expanded - m_baseSearchMargin;
                // After this frame's lerp, decrement so next frame
                // shrinks further. At rampLeft==1 we set to base
                // exactly.
                m_currentSearchMargin = m_baseSearchMargin
                    + (delta * (rampLeft - 1)) / 8;
                m_postReacquireRamp--;
                if (m_postReacquireRamp == 0)
                    m_currentSearchMargin = m_baseSearchMargin;
            }
            // --- end FSM --------------------------------------------

            // US-MT-2 + US-MT-3: smooth the sub-pixel center.
            // While occluded, advance state via predict-only so the
            // Kalman track coasts at the previously-estimated velocity
            // instead of being yanked by the (possibly garbage) match.
            // When NOT occluded, behaviour is identical to US-MT-2.
            if (m_kalmanEnabled) {
                if (m_occluded) {
                    QPointF predicted = kalmanPredictOnly();
                    region.subPixelCenter = predicted;
                } else {
                    QPointF measurement = region.subPixelCenter.isNull()
                        ? QPointF(region.rect.x() + region.rect.width() / 2.0,
                                  region.rect.y() + region.rect.height() / 2.0)
                        : region.subPixelCenter;
                    QPointF smoothed = kalmanSmooth(measurement, region.confidence);
                    region.subPixelCenter = smoothed;
                }
            }

            m_result.regions.append(region);
        }

        frameCount++;

        // Report progress
        int pct = static_cast<int>(100.0 * frameCount / totalFrames);
        emit progressChanged(qMin(pct, 100));
        return true;
    };

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (m_cancelRequested) {
            av_packet_unref(packet);
            goto cleanup;
        }
        if (packet->stream_index != videoIdx) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (avcodec_receive_frame(decCtx, frame) == 0) {
                if (!processDecodedFrame())
                    goto cleanup;
            }
        }
        av_packet_unref(packet);
    }

    if (!m_cancelRequested && avcodec_send_packet(decCtx, nullptr) == 0) {
        while (avcodec_receive_frame(decCtx, frame) == 0) {
            if (!processDecodedFrame())
                goto cleanup;
        }
    }

cleanup:
    m_result.endFrame = frameCount > 0 ? frameCount - 1 : 0;
    resetOcclusionState();

    av_frame_free(&frame);
    av_packet_free(&packet);
    if (swsCtx) sws_freeContext(swsCtx);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    return !m_result.isEmpty();
}

// ---------------------------------------------------------------------------
// Private: computeNCC — normalized cross-correlation
// ---------------------------------------------------------------------------

double MotionTracker::computeNCC(const QImage &frame, const QImage &templ,
                                 int offsetX, int offsetY)
{
    int tw = templ.width();
    int th = templ.height();

    // Verify bounds
    if (offsetX < 0 || offsetY < 0 ||
        offsetX + tw > frame.width() || offsetY + th > frame.height())
        return -1.0;

    // Compute means
    double sumF = 0.0, sumT = 0.0;
    int count = tw * th;

    for (int y = 0; y < th; ++y) {
        const uchar *fRow = frame.constScanLine(offsetY + y);
        const uchar *tRow = templ.constScanLine(y);
        for (int x = 0; x < tw; ++x) {
            sumF += fRow[offsetX + x];
            sumT += tRow[x];
        }
    }

    double meanF = sumF / count;
    double meanT = sumT / count;

    // Compute NCC
    double num = 0.0, denF = 0.0, denT = 0.0;

    for (int y = 0; y < th; ++y) {
        const uchar *fRow = frame.constScanLine(offsetY + y);
        const uchar *tRow = templ.constScanLine(y);
        for (int x = 0; x < tw; ++x) {
            double f = fRow[offsetX + x] - meanF;
            double t = tRow[x] - meanT;
            num += f * t;
            denF += f * f;
            denT += t * t;
        }
    }

    double den = std::sqrt(denF * denT);
    if (den < 1e-10) return 0.0;

    return num / den;
}

// ---------------------------------------------------------------------------
// Private: toGrayscale
// ---------------------------------------------------------------------------

QImage MotionTracker::toGrayscale(const QImage &image)
{
    if (image.format() == QImage::Format_Grayscale8)
        return image;

    return image.convertToFormat(QImage::Format_Grayscale8);
}
