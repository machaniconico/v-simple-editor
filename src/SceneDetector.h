#pragma once

#include <QObject>
#include <QVector>
#include <QImage>

// Streaming scene-cut detector. Caller feeds decoded frames in order via
// processFrame(); the detector emits sceneCutDetected() whenever a cut is
// identified using a chi-squared grayscale histogram distance with a
// minimum-scene-length gate to suppress micro-cuts (e.g. flashes).
//
// Standalone module: no decoder/timeline dependencies. Wiring into the
// decoder loop and the "Detect Scene Cuts" menu action is a follow-up story.
class SceneDetector : public QObject
{
    Q_OBJECT

public:
    explicit SceneDetector(QObject *parent = nullptr);

    // Configuration
    void setThreshold(double diffThreshold);   // 0.0..1.0 chi-squared cutoff, default 0.35
    void setMinSceneFrames(int n);              // minimum frames between cuts, default 24 (~1s @ 24fps)
    void setAdaptiveBaseline(bool enabled);     // NIT: switch from static to running mean+stdev

    // Clear all accumulated state (histograms, cut list, stats).
    void reset();

    // Process one frame in temporal order. Emits sceneCutDetected() if this
    // frame begins a new scene relative to the previous one.
    // frameRate is used to compute the timestamp; pass the source clip's fps.
    void processFrame(int frameIndex, const QImage &frame, double frameRate);

    // Read accumulated cut data
    QVector<int> sceneCutFrames() const;
    QVector<qint64> sceneCutTimestampsUs() const;

    // Stats
    double lastDiff() const;
    int totalCuts() const;

signals:
    void sceneCutDetected(int frameIndex, qint64 timeUs);

private:
    // 256-bin grayscale histogram normalized so bins sum to 1.0.
    QVector<double> computeGrayHistogram(const QImage &frame) const;

    // Chi-squared distance: sum_i (a[i] - b[i])^2 / max(a[i] + b[i], epsilon).
    double histogramDistance(const QVector<double> &a, const QVector<double> &b) const;

    QVector<double> m_prevHist;
    int m_lastCutFrame = -10000;
    int m_minSceneFrames = 24;
    double m_threshold = 0.35;
    double m_lastDiff = 0.0;
    QVector<int> m_cutFrames;
    QVector<qint64> m_cutTimesUs;

    // Adaptive baseline (NIT): running mean + variance over the last
    // kBaselineWindow distance samples; cut when d > mean + 3*stdev.
    bool m_adaptiveBaseline = false;
    QVector<double> m_recentDiffs;
    static constexpr int kBaselineWindow = 32;
};
