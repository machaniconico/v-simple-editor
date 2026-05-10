#include "SceneDetector.h"

#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kHistEpsilon = 1e-9;
constexpr int kHistogramBins = 256;
// Adaptive sensitivity: cut when d > mean + kAdaptiveSigma * stdev.
constexpr double kAdaptiveSigma = 3.0;
}

SceneDetector::SceneDetector(QObject *parent)
    : QObject(parent)
{
}

void SceneDetector::setThreshold(double diffThreshold)
{
    m_threshold = std::clamp(diffThreshold, 0.0, 1.0);
}

void SceneDetector::setMinSceneFrames(int n)
{
    m_minSceneFrames = std::max(1, n);
}

void SceneDetector::setAdaptiveBaseline(bool enabled)
{
    m_adaptiveBaseline = enabled;
    if (!enabled)
        m_recentDiffs.clear();
}

void SceneDetector::reset()
{
    m_prevHist.clear();
    m_lastCutFrame = -10000;
    m_lastDiff = 0.0;
    m_cutFrames.clear();
    m_cutTimesUs.clear();
    m_recentDiffs.clear();
}

void SceneDetector::processFrame(int frameIndex, const QImage &frame, double frameRate)
{
    if (frame.isNull())
        return;

    const QVector<double> currHist = computeGrayHistogram(frame);

    // First frame: seed history; nothing to compare against yet.
    if (m_prevHist.isEmpty()) {
        m_prevHist = currHist;
        m_lastDiff = 0.0;
        return;
    }

    const double d = histogramDistance(currHist, m_prevHist);
    m_lastDiff = d;

    // Determine the cut threshold. Static by default; adaptive (NIT) tracks a
    // running mean+stdev over the last kBaselineWindow samples.
    double effectiveThreshold = m_threshold;
    if (m_adaptiveBaseline && m_recentDiffs.size() >= 4) {
        double sum = 0.0;
        for (double v : m_recentDiffs)
            sum += v;
        const double mean = sum / m_recentDiffs.size();
        double sqSum = 0.0;
        for (double v : m_recentDiffs) {
            const double delta = v - mean;
            sqSum += delta * delta;
        }
        const double variance = sqSum / m_recentDiffs.size();
        const double stdev = std::sqrt(variance);
        effectiveThreshold = std::max(m_threshold, mean + kAdaptiveSigma * stdev);
    }

    const bool gateOpen = (frameIndex - m_lastCutFrame) >= m_minSceneFrames;
    if (d > effectiveThreshold && gateOpen) {
        const double safeFps = frameRate > 0.0 ? frameRate : 30.0;
        const qint64 timeUs = static_cast<qint64>(qRound64(frameIndex * 1e6 / safeFps));
        m_cutFrames.append(frameIndex);
        m_cutTimesUs.append(timeUs);
        m_lastCutFrame = frameIndex;
        emit sceneCutDetected(frameIndex, timeUs);
    }

    // Maintain adaptive baseline window.
    if (m_adaptiveBaseline) {
        m_recentDiffs.append(d);
        if (m_recentDiffs.size() > kBaselineWindow)
            m_recentDiffs.remove(0, m_recentDiffs.size() - kBaselineWindow);
    }

    // Always advance the histogram baseline regardless of cut decision.
    m_prevHist = currHist;
}

QVector<int> SceneDetector::sceneCutFrames() const
{
    return m_cutFrames;
}

QVector<qint64> SceneDetector::sceneCutTimestampsUs() const
{
    return m_cutTimesUs;
}

double SceneDetector::lastDiff() const
{
    return m_lastDiff;
}

int SceneDetector::totalCuts() const
{
    return m_cutFrames.size();
}

QVector<double> SceneDetector::computeGrayHistogram(const QImage &frame) const
{
    QVector<double> hist(kHistogramBins, 0.0);
    if (frame.isNull())
        return hist;

    // Convert once to a known grayscale-friendly format. QImage::Format_Grayscale8
    // gives one byte per pixel and avoids per-pixel format branching.
    QImage gray = (frame.format() == QImage::Format_Grayscale8)
                      ? frame
                      : frame.convertToFormat(QImage::Format_Grayscale8);

    const int w = gray.width();
    const int h = gray.height();
    if (w <= 0 || h <= 0)
        return hist;

    quint64 total = 0;
    for (int y = 0; y < h; ++y) {
        const uchar *row = gray.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            ++hist[row[x]];
            ++total;
        }
    }

    if (total > 0) {
        const double inv = 1.0 / static_cast<double>(total);
        for (int i = 0; i < kHistogramBins; ++i)
            hist[i] *= inv;
    }
    return hist;
}

double SceneDetector::histogramDistance(const QVector<double> &a, const QVector<double> &b) const
{
    if (a.size() != b.size() || a.isEmpty())
        return 0.0;

    // Chi-squared: sum_i (a - b)^2 / max(a + b, epsilon). Two normalized
    // histograms sum to 1 each, so the maximum theoretical value is 2.0; we
    // halve it to land in [0, 1] which matches the documented threshold range.
    double sum = 0.0;
    for (int i = 0; i < a.size(); ++i) {
        const double diff = a[i] - b[i];
        const double denom = std::max(a[i] + b[i], kHistEpsilon);
        sum += (diff * diff) / denom;
    }
    return 0.5 * sum;
}
