#pragma once

#include <QObject>
#include <QRect>
#include <QRectF>
#include <QPointF>
#include <QImage>
#include <QVector>
#include <QString>
#include <atomic>

// --- Tracking region at a single frame ---

struct TrackingRegion {
    QRect rect;             // bounding box in pixel coordinates (integer)
    double confidence = 0.0; // NCC score 0.0-1.0
    int frameNumber = 0;
    // Sub-pixel refined center (US-MT-1). Default-constructed QPointF() means
    // refinement was skipped — callers should fall back to rect.center().
    // When valid, this is the parabolic-interpolated peak of the NCC score
    // surface around the integer-best match (rect.center() + dx,dy in [-1,+1]).
    QPointF subPixelCenter;
};

// --- Full tracking result over time ---

struct TrackingResult {
    QVector<TrackingRegion> regions;
    int startFrame = 0;
    int endFrame = 0;
    double fps = 0.0;
    int srcWidth = 0;
    int srcHeight = 0;

    bool isEmpty() const { return regions.isEmpty(); }

    // Get interpolated position at a given time (seconds)
    QRect positionAtTime(double timeSec) const;
};

// --- Motion tracker using template matching (NCC on QImage) ---

class MotionTracker : public QObject
{
    Q_OBJECT

public:
    explicit MotionTracker(QObject *parent = nullptr);

    // Configuration
    void setSearchMargin(int margin);   // pixels to expand search from last position
    int searchMargin() const { return m_searchMargin; }

    void setMinConfidence(double conf); // minimum NCC to accept a match
    double minConfidence() const { return m_minConfidence; }

    // Start tracking: extract frames via FFmpeg and track the object in initialRect
    void startTracking(const QString &filePath, const QRect &initialRect);

    // Cancel an in-progress tracking operation
    void cancel();

    // Track object in a single frame given a template image
    TrackingRegion trackFrame(const QImage &currentFrame, const QImage &templateImage,
                              const QRect &searchArea);

    // Retrieve all tracked positions
    TrackingResult getTrackingData() const { return m_result; }

    // Calculate overlay position based on tracking data at a given time
    static QRectF applyToOverlay(const TrackingResult &trackingData,
                                 const QRectF &overlayRect,
                                 double currentTime,
                                 int videoWidth, int videoHeight);

    // JSON export / import
    static bool exportTrackingData(const TrackingResult &data, const QString &filePath);
    static TrackingResult importTrackingData(const QString &filePath);

    // US-MT-2: enable/disable constant-velocity Kalman smoothing of the
    // sub-pixel center. Default true. When disabled, subPixelCenter retains
    // the raw US-MT-1 parabolic-interpolated peak (parity with US-MT-1).
    void setKalmanEnabled(bool enabled);
    bool kalmanEnabled() const { return m_kalmanEnabled; }

signals:
    void progressChanged(int percent);
    void trackingComplete(const TrackingResult &result);
    // US-MT-3: emitted exactly once per tracking run when occlusion FSM has
    // counted 60 consecutive occluded frames without re-acquire. After this
    // signal fires, decodeAndTrack stops appending regions to m_result for
    // the rest of the timeline.
    void trackingLost(int atFrame);

private:
    // Decode video frames via FFmpeg and run tracking loop
    bool decodeAndTrack(const QString &filePath, const QRect &initialRect);

    // Normalized cross-correlation on grayscale data
    static double computeNCC(const QImage &frame, const QImage &templ,
                             int offsetX, int offsetY);

    // Convert QImage region to grayscale
    static QImage toGrayscale(const QImage &image);

    // US-MT-2: reset Kalman state to "uninitialized" so the next call to
    // kalmanSmooth() seeds from the first measurement.
    void resetKalmanState();

    // US-MT-2: per-frame constant-velocity Kalman update. Takes the raw
    // measurement (typically subPixelCenter) and the current match score.
    // Returns the smoothed posterior (x, y) in source-frame pixel coords.
    // First call after resetKalmanState() seeds the state from the
    // measurement and returns it unchanged.
    QPointF kalmanSmooth(const QPointF &measurement, double matchScore);

    // US-MT-3: predict-only state advance for occluded frames. Runs
    // x_pred = F x and P_pred = F P F^T + Q (same F and Q as kalmanSmooth)
    // and writes them back into m_kalmanState / m_kalmanCov, but does NOT
    // consume any measurement (no innovation, no Kalman gain, no posterior
    // update). Returns the predicted (x, y) in source-frame pixel coords.
    // No-op when the filter is uninitialized.
    QPointF kalmanPredictOnly();

    // US-MT-3: occlusion FSM helper — append the current match score to the
    // 16-frame median ring buffer. Updates running peak as a side-effect.
    void pushRecentScore(double score);

    // US-MT-3: occlusion FSM helper — return the median of the 16-frame
    // ring buffer (or whatever is populated when fewer than 16 samples).
    double medianOfRecent() const;

    // US-FIX-1: centralized occlusion-state reset. Called from startTracking,
    // cancel, and the cleanup: label after a tracking run terminates so a
    // second startTracking() on the same instance starts identically to a
    // fresh instance.
    void resetOcclusionState();

    TrackingResult m_result;
    int m_searchMargin = 50;       // pixel margin around last known position
    double m_minConfidence = 0.5;  // minimum NCC score to accept
    std::atomic<bool> m_cancelRequested{false};

    // US-MT-2: constant-velocity Kalman state [x, y, vx, vy] in source-frame
    // pixel coordinates. m_kalmanInitialized tracks whether the next call
    // should seed (true) or run predict+update (false).
    bool m_kalmanEnabled = true;
    bool m_kalmanInitialized = false;
    double m_kalmanState[4] = {0.0, 0.0, 0.0, 0.0};
    double m_kalmanCov[4][4] = {{0.0}};
    double m_peakScoreSeenSoFar = 0.0;

    // US-MT-3: occlusion FSM. None of these fire when the score never drops
    // below the onset thresholds — output is then bit-identical to US-MT-2.
    //
    // m_occluded            — true between onset and re-acquire; cleared at
    //                         the end of the contract ramp.
    // m_occludedSince       — frame index where the current occlusion
    //                         started; -1 when not occluded.
    // m_consecLowScoreFrames — counter for the "2 consecutive frames below
    //                         0.5*median" half of the onset rule.
    // m_consecOccludedFrames — counter for the hard-fail rule (60 frames).
    // m_postReacquireRamp   — counts down 8→0 after re-acquire, contracts
    //                         search radius linearly back to base.
    // m_baseSearchMargin    — captured from m_searchMargin at startTracking
    //                         so we can restore exactly after recovery.
    // m_currentSearchMargin — what we actually use this frame; modified
    //                         during occlusion / ramp.
    // m_lastConfidentTemplate — cached when matchScore > 0.85 * peakScore.
    //                         Used to re-template-match while occluded
    //                         instead of drifting with the live template.
    // m_lastConfidentTemplateFrame — frame index for diagnostics.
    // m_recentScores        — ring buffer (≤ 16 entries) of recent
    //                         matchScore values; sorted-copy → median.
    // m_trackingLostEmitted — flips true when trackingLost() fires; we
    //                         must emit at most once and stop appending
    //                         regions thereafter.
    bool m_occluded = false;
    int m_occludedSince = -1;
    int m_consecLowScoreFrames = 0;
    int m_consecOccludedFrames = 0;
    int m_postReacquireRamp = 0;
    int m_baseSearchMargin = 50;
    int m_currentSearchMargin = 50;
    QImage m_lastConfidentTemplate;
    int m_lastConfidentTemplateFrame = -1;
    QVector<double> m_recentScores;
    bool m_trackingLostEmitted = false;
    // US-MT-3: one-shot multiplier consumed by the next kalmanSmooth call.
    // Set to 2.0 at re-acquire so R = baseline * 2.0 for that single frame
    // (softens the post-occlusion snap toward the recovered measurement),
    // then kalmanSmooth resets it back to 1.0. Default 1.0 = identity with
    // US-MT-2 in the no-occlusion path.
    double m_oneShotRMultiplier = 1.0;
};
