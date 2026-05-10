#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QVector>
#include <QtGlobal>

// Per-frame transform keyframe baked onto a ClipInfo by US-INT-4.
// Identity: dx=dy=theta=0, scale=1. GLPreview applies the INVERSE 2D affine
// (-dx, -dy, -theta, 1/scale around the frame centre) per source frame.
struct StabilizerKeyframe {
    qint64 timeUs = 0;
    double dx = 0.0;
    double dy = 0.0;
    double theta = 0.0;
    double scale = 1.0;
};

// ---------------------------------------------------------------------------
// MotionStabilizer (US-MS-1)
//
// Standalone module — given a sequence of decoded video frames fed via
// processFrame(), estimates per-frame global translation (downscaled NCC
// match against the previous frame), accumulates a cumulative trajectory,
// and on finalize() applies 1-D Gaussian smoothing across the trajectory to
// produce per-frame counter-translations.
//
// This is a pure analyser — no decoder/compose integration. A future story
// will wire results() into the VideoPlayer compose path so that, per frame
// i, we render at offset (-stab.smoothTx, -stab.smoothTy) to undo jitter.
//
// Algorithm (per processFrame):
//   1. toGrayscale() + downscale to kMatchWidth x kMatchHeight (320x180)
//   2. estimateTranslation(): integer ±kSearchRadius px (±32) NCC sliding
//      template match, then 3x3 parabolic refinement on the score peak
//      (mirrors MotionTracker US-MT-1).
//   3. Cumulative rawX/rawY appended.
// finalize():
//   4. 1-D Gaussian smoothing (truncated kernel ±3*sigma) over rawX/rawY
//      with sigma = totalFrames * smoothness * 0.1 (clamped >= 0.5).
//   5. smoothTx = rawX[i] - smoothX[i], smoothTy = rawY[i] - smoothY[i]
//      (the inverse the camera should apply to follow the smoothed curve).
//   6. Clamp smoothTx/smoothTy to ± bordersAllowed * frameWidth/Height.
// ---------------------------------------------------------------------------

class MotionStabilizer : public QObject
{
    Q_OBJECT
public:
    struct StabilizationFrame {
        int frameIndex = 0;
        // Raw measured per-frame translation (pixels, in source-frame coords).
        // tx/ty are the *cumulative* trajectory — i.e. integral of per-frame
        // delta — so finalize() can smooth them directly.
        double tx = 0.0;
        double ty = 0.0;
        // Post-smoothing inverse to apply during compose: rendering at
        // offset (-smoothTx, -smoothTy) cancels the jitter component while
        // preserving the smooth camera curve.
        double smoothTx = 0.0;
        double smoothTy = 0.0;
    };

    explicit MotionStabilizer(QObject *parent = nullptr);

    // Configuration ------------------------------------------------------
    // smoothness in [0, 1]. 0 = no smoothing (smoothTx/smoothTy = 0 — no
    // stabilisation). 1 = heavy smoothing (sigma = N * 0.1). Default 0.5.
    void setSmoothness(double radius);
    double smoothness() const { return m_smoothness; }

    // Max fraction of frame width/height the per-frame counter-translation
    // is allowed to consume. Default 0.1 = 10%. Larger values cancel more
    // jitter at the cost of needing more border crop downstream.
    void setBordersAllowed(double frac);
    double bordersAllowed() const { return m_bordersAllowed; }

    // Reset all state — m_prevFrame, frames, raw trajectory, last index.
    void reset();

    // Main API: feed frames sequentially. The first frame seeds the
    // trajectory at (0, 0); subsequent frames produce a (dx, dy) which is
    // accumulated. Frame indices SHOULD be monotonically increasing — if a
    // gap is detected we still accept it but treat it as a one-frame jump.
    void processFrame(int frameIndex, const QImage &frame);

    // After all frames are processed, compute the smoothed trajectory and
    // the per-frame inverse counter-translations. Idempotent — safe to call
    // multiple times (re-runs the smoothing pass).
    void finalize();

    // Read results (after finalize()).
    QVector<StabilizationFrame> results() const { return m_frames; }

    // US-INT-4: synchronous one-shot analyser. Opens `filePath` via
    // libavformat, decodes every video frame to RGB888, feeds processFrame()
    // + finalize(), and returns the per-frame inverse counter-translations
    // packed as StabilizerKeyframe (timeUs from the frame's PTS, dx/dy from
    // smoothTx/smoothTy; theta=0, scale=1.0 — translation-only model).
    // Resets internal state on entry. Returns empty vector on open failure.
    QVector<StabilizerKeyframe> analyzeFile(const QString &filePath);

signals:
    void progressUpdated(int currentFrame, int totalFrames);
    void finished();

private:
    // Estimate per-frame translation between two same-size grayscale images
    // via integer ±kSearchRadius sliding NCC match + parabolic refinement.
    // Returns the peak NCC score; outTx/outTy are the recovered per-frame
    // delta in DOWNSCALED-image pixels (caller scales back to source coords).
    static double estimateTranslation(const QImage &prev, const QImage &curr,
                                      double &outTx, double &outTy);

    // Normalised cross-correlation between two equally-sized grayscale
    // patches, with `curr` shifted by (offsetX, offsetY) relative to `prev`.
    // Returns NCC in [-1, 1]; -1.0 sentinel if the shifted region falls
    // outside the image bounds.
    static double computeShiftedNCC(const QImage &prev, const QImage &curr,
                                    int offsetX, int offsetY);

    // QImage -> Grayscale8 + downscale to (kMatchWidth, kMatchHeight) using
    // Qt's smooth transformation (good enough — this is for matching, not
    // display).
    static QImage toGrayDownscaled(const QImage &src);

    // 1-D Gaussian convolution over a single trajectory axis. Kernel
    // half-width = ceil(3 * sigma) clamped to xs.size()-1. Edges use mirror
    // padding (sample reflection) so the smoothed value at index 0 / N-1
    // does not collapse toward zero.
    static void gaussianSmooth1D(QVector<double> &xs, double sigma);

    // Build a normalised truncated-Gaussian 1-D kernel with stdev `sigma`
    // and half-width `halfWidth`. Total length = 2*halfWidth + 1.
    static QVector<double> buildGaussianKernel(double sigma, int halfWidth);

    // Constants ----------------------------------------------------------
    static constexpr int kMatchWidth = 320;
    static constexpr int kMatchHeight = 180;
    // ±32 px search radius in DOWNSCALED coords, per the US spec.
    static constexpr int kSearchRadius = 32;

    // State --------------------------------------------------------------
    QImage m_prevFrame;            // previous downscaled grayscale frame
    int m_prevFrameWidth = 0;      // SOURCE-frame width that produced m_prevFrame
    int m_prevFrameHeight = 0;     // SOURCE-frame height that produced m_prevFrame
    QVector<StabilizationFrame> m_frames;
    QVector<double> m_rawX;        // cumulative raw trajectory, source-pixel coords
    QVector<double> m_rawY;
    double m_smoothness = 0.5;
    double m_bordersAllowed = 0.1;
    int m_lastFrameIdx = -1;
};
