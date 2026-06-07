#include "SilenceCut.h"

#include <algorithm>
#include <cmath>

namespace silencecut {
namespace {

double totalDurationSec(const QVector<float>& samples, int sampleRate)
{
    return samples.size() / static_cast<double>(sampleRate);
}

QVector<Segment> confirmedSilenceSegments(const QVector<float>& samples, int sampleRate, const Config& cfg)
{
    QVector<Segment> silences;
    if (samples.isEmpty() || sampleRate <= 0)
        return silences;

    const double windowSec = cfg.windowSec > 0.0 ? cfg.windowSec : 0.02;
    const int winN = std::max(1, static_cast<int>(std::round(sampleRate * windowSec)));
    const int sampleCount = samples.size();
    QVector<bool> silentWindows;
    silentWindows.reserve((sampleCount + winN - 1) / winN);

    for (int start = 0; start < sampleCount; start += winN) {
        const int end = std::min(sampleCount, start + winN);
        double sumSquares = 0.0;
        for (int i = start; i < end; ++i) {
            const double x = samples[i];
            sumSquares += x * x;
        }
        const double meanSquares = sumSquares / static_cast<double>(end - start);
        const double rms = std::sqrt(meanSquares);
        const double db = 20.0 * std::log10(std::max(rms, 1.0e-9));
        silentWindows.push_back(db < cfg.thresholdDb);
    }

    for (int i = 0; i < silentWindows.size();) {
        if (!silentWindows[i]) {
            ++i;
            continue;
        }

        const int runStartWin = i;
        while (i < silentWindows.size() && silentWindows[i])
            ++i;
        const int runEndWin = i;

        const int startSample = runStartWin * winN;
        const int endSample = std::min(sampleCount, runEndWin * winN);
        const double startSec = startSample / static_cast<double>(sampleRate);
        const double endSec = endSample / static_cast<double>(sampleRate);
        if (endSec - startSec >= cfg.minSilenceSec)
            silences.push_back({ startSec, endSec });
    }

    return silences;
}

QVector<Segment> complementSegments(const QVector<Segment>& segments, double total)
{
    QVector<Segment> gaps;
    double cursor = 0.0;
    for (const Segment& segment : segments) {
        const double start = std::max(0.0, std::min(segment.startSec, total));
        const double end = std::max(start, std::min(segment.endSec, total));
        if (start > cursor)
            gaps.push_back({ cursor, start });
        cursor = std::max(cursor, end);
    }
    if (cursor < total)
        gaps.push_back({ cursor, total });
    return gaps;
}

QVector<Segment> paddedMergedKeeps(const QVector<Segment>& rawKeeps, double total, const Config& cfg)
{
    QVector<Segment> keeps;
    const double paddingSec = std::max(0.0, cfg.paddingSec);
    for (const Segment& keep : rawKeeps) {
        const double start = std::max(0.0, keep.startSec - paddingSec);
        const double end = std::min(total, keep.endSec + paddingSec);
        if (end <= start)
            continue;

        if (!keeps.isEmpty() && start <= keeps.last().endSec) {
            keeps.last().endSec = std::max(keeps.last().endSec, end);
        } else {
            keeps.push_back({ start, end });
        }
    }

    QVector<Segment> filtered;
    filtered.reserve(keeps.size());
    for (const Segment& keep : keeps) {
        if (keep.durationSec() >= cfg.minKeepSec)
            filtered.push_back(keep);
    }
    return filtered;
}

QVector<Segment> finalKeepSegments(const QVector<float>& samples, int sampleRate, const Config& cfg)
{
    if (samples.isEmpty() || sampleRate <= 0)
        return {};

    const double total = totalDurationSec(samples, sampleRate);
    const QVector<Segment> silences = confirmedSilenceSegments(samples, sampleRate, cfg);
    const QVector<Segment> rawKeeps = complementSegments(silences, total);
    return paddedMergedKeeps(rawKeeps, total, cfg);
}

} // namespace

QVector<ClipInfo> planKeepClips(const ClipInfo& src,
                                const QVector<Segment>& keepsActiveSec)
{
    if (keepsActiveSec.isEmpty())
        return {};

    const double srcOut = (src.outPoint > 0.0) ? src.outPoint : src.duration;
    QVector<ClipInfo> result;
    result.reserve(keepsActiveSec.size());

    for (const Segment& k : keepsActiveSec) {
        double absIn  = src.inPoint + k.startSec;
        double absOut = src.inPoint + k.endSec;
        absIn  = std::max(absIn,  src.inPoint);
        absOut = std::min(absOut, srcOut);
        if (absOut <= absIn)
            continue;
        ClipInfo c = src;
        c.inPoint  = absIn;
        c.outPoint = absOut;
        result.append(c);
    }
    return result;
}

QVector<Segment> detectKeepSegments(const QVector<float>& samples, int sampleRate, const Config& cfg)
{
    return finalKeepSegments(samples, sampleRate, cfg);
}

QVector<Segment> detectSilenceSegments(const QVector<float>& samples, int sampleRate, const Config& cfg)
{
    if (samples.isEmpty() || sampleRate <= 0)
        return {};

    const QVector<Segment> keeps = finalKeepSegments(samples, sampleRate, cfg);
    return complementSegments(keeps, totalDurationSec(samples, sampleRate));
}

} // namespace silencecut
