#include "MultiCamSync.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace multicam {

MultiCamSync::MultiCamSync(QObject *parent)
    : QObject(parent)
{
}

void MultiCamSync::setSources(const QVector<CamSource> &sources)
{
    m_sources = sources;
}

QVector<CamSource> MultiCamSync::sources() const
{
    return m_sources;
}

// ---------------------------------------------------------------------------
// estimateOffsetMs — classic normalized cross-correlation.
//
// For every candidate lag in [-(N-1), +(N-1)] (capped to the smaller of the
// two envelope sizes) we compute the dot-product of the overlapping region,
// normalized by the L2 norms of the overlapping slices. The lag yielding the
// maximum normalized correlation is returned, converted to milliseconds via
// envHopMs (the time spacing between consecutive envelope samples).
//
// A positive returned value means otherEnv is delayed relative to refEnv
// (i.e. otherEnv must be shifted left / earlier by that amount to align).
// ---------------------------------------------------------------------------
double MultiCamSync::estimateOffsetMs(const QVector<float> &refEnv,
                                      const QVector<float> &otherEnv,
                                      double envHopMs)
{
    if (refEnv.isEmpty() || otherEnv.isEmpty())
        return 0.0;

    const int nRef   = refEnv.size();
    const int nOther = otherEnv.size();

    // Cap the search window to a reasonable range.
    const int maxLag = std::min(nRef, nOther) - 1;
    if (maxLag < 0)
        return 0.0;

    double bestCorr = -std::numeric_limits<double>::infinity();
    int    bestLag  = 0;

    // `lag` = how many samples otherEnv is DELAYED relative to refEnv
    // (positive => otherEnv is later). If other[i] == ref[i - lag], then
    // ref[k] aligns with other[k + lag], which the dot-product below
    // detects, so the returned lag matches the true offset sign.
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        // refEnv[refStart + k]  <->  otherEnv[refStart + k + lag]
        const int refStart   = std::max(0, -lag);
        const int overlap    =
            std::min(nRef - refStart, nOther - (refStart + lag));
        if (overlap <= 0)
            continue;

        double dot     = 0.0;
        double normRef = 0.0;
        double normOth = 0.0;
        for (int k = 0; k < overlap; ++k) {
            const double a = static_cast<double>(refEnv[refStart + k]);
            const double b =
                static_cast<double>(otherEnv[refStart + k + lag]);
            dot     += a * b;
            normRef += a * a;
            normOth += b * b;
        }

        if (normRef <= 0.0 || normOth <= 0.0)
            continue;

        const double corr = dot / (std::sqrt(normRef) * std::sqrt(normOth));
        if (corr > bestCorr) {
            bestCorr = corr;
            bestLag  = lag;
        }
    }

    return static_cast<double>(bestLag) * envHopMs;
}

QVector<qint64> MultiCamSync::computeAngleOffsetsUs(
    const QVector<QVector<float>> &angleEnvelopes,
    double envHopMs)
{
    QVector<qint64> offsetsUs(angleEnvelopes.size(), 0);
    if (angleEnvelopes.size() < 2 || envHopMs <= 0.0)
        return offsetsUs;

    const QVector<float> &refEnv = angleEnvelopes.first();
    if (refEnv.isEmpty())
        return offsetsUs;

    for (int i = 1; i < angleEnvelopes.size(); ++i) {
        const QVector<float> &otherEnv = angleEnvelopes[i];
        if (otherEnv.isEmpty()) {
            offsetsUs[i] = 0;
            continue;
        }

        const double offsetMs = estimateOffsetMs(refEnv, otherEnv, envHopMs);
        offsetsUs[i] = static_cast<qint64>(std::llround(offsetMs * 1000.0));
    }

    return offsetsUs;
}

// ---------------------------------------------------------------------------
// syncByAudio — build a coarse amplitude envelope per source via
// WaveformGenerator and cross-correlate each against source[0].
// ---------------------------------------------------------------------------
void MultiCamSync::syncByAudio()
{
    if (m_sources.isEmpty()) {
        emit syncProgress(100);
        emit syncFinished();
        return;
    }

    // Coarse envelope resolution: 50 peaks/sec => 20 ms hop.
    const int    peaksPerSecond = 50;
    const double envHopMs       = 1000.0 / static_cast<double>(peaksPerSecond);

    // Reference is source[0] — its offset is always 0.
    m_sources[0].offsetMs = 0.0;
    const WaveformData refWf =
        WaveformGenerator::generate(m_sources[0].filePath, peaksPerSecond);
    const QVector<float> refEnv = refWf.peaks;

    const int total = m_sources.size();
    emit syncProgress(total > 0 ? static_cast<int>(100.0 / total) : 100);

    for (int i = 1; i < total; ++i) {
        const WaveformData otherWf =
            WaveformGenerator::generate(m_sources[i].filePath, peaksPerSecond);

        if (refEnv.isEmpty() || otherWf.peaks.isEmpty()) {
            m_sources[i].offsetMs = 0.0;
        } else {
            m_sources[i].offsetMs =
                estimateOffsetMs(refEnv, otherWf.peaks, envHopMs);
        }

        emit syncProgress(
            static_cast<int>(100.0 * static_cast<double>(i + 1) / total));
    }

    emit syncProgress(100);
    emit syncFinished();
}

void MultiCamSync::addAngleCut(double timeMs, int camIndex)
{
    AngleCut cut;
    cut.timeMs   = timeMs;
    cut.camIndex = camIndex;
    m_cuts.append(cut);
}

QString MultiCamSync::exportSwitchedEdl() const
{
    QVector<AngleCut> sorted = m_cuts;
    std::sort(sorted.begin(), sorted.end(),
              [](const AngleCut &a, const AngleCut &b) {
                  return a.timeMs < b.timeMs;
              });

    QString edl;
    for (int i = 0; i < sorted.size(); ++i) {
        const AngleCut &c = sorted[i];

        QString filePath;
        if (c.camIndex >= 0 && c.camIndex < m_sources.size())
            filePath = m_sources[c.camIndex].filePath;

        edl += QStringLiteral("%1  CAM%2  %3ms  %4\n")
                   .arg(i + 1)
                   .arg(c.camIndex)
                   .arg(c.timeMs)
                   .arg(filePath);
    }
    return edl;
}

} // namespace multicam
