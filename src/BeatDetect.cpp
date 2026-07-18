#include "BeatDetect.h"

#include <algorithm>
#include <cmath>

namespace beatdetect {
namespace {

// フレームごとのエネルギー E[k] = sum(x^2)。非重複窓。
QVector<double> frameEnergies(const QVector<float>& samples, int hop)
{
    QVector<double> energies;
    const int n = samples.size();
    energies.reserve(n / hop + 1);
    for (int start = 0; start < n; start += hop) {
        const int end = std::min(n, start + hop);
        double sum = 0.0;
        for (int i = start; i < end; ++i) {
            const double x = samples[i];
            sum += x * x;
        }
        energies.push_back(sum);
    }
    return energies;
}

} // namespace

Result detectBeats(const QVector<float>& samples, int sampleRate, const Config& cfg)
{
    Result result;
    if (samples.isEmpty() || sampleRate <= 0)
        return result;

    const double windowSec = cfg.windowSec > 0.0 ? cfg.windowSec : 0.01;
    const int hop = std::max(1, static_cast<int>(std::round(sampleRate * windowSec)));

    const QVector<double> energy = frameEnergies(samples, hop);
    const int frames = energy.size();
    if (frames < 2)
        return result;

    // オンセット強度: 半波整流した正のエネルギー増分。
    QVector<double> flux(frames, 0.0);
    for (int k = 1; k < frames; ++k)
        flux[k] = std::max(0.0, energy[k] - energy[k - 1]);

    // 適応しきい値: ±0.1s 相当の窓での移動平均 * sensitivity。
    const int meanRadius =
        std::max(1, static_cast<int>(std::round(0.1 / windowSec)));

    // 最大 BPM に対応する最小間隔(秒)→ デバウンス用フレーム数。
    const double maxBpm = cfg.maxBpm > 0.0 ? cfg.maxBpm : 200.0;
    const double minIntervalSec = 60.0 / maxBpm;

    QVector<double> beats;
    double lastBeatSec = -1.0e9;
    for (int k = 1; k < frames - 1; ++k) {
        const int lo = std::max(0, k - meanRadius);
        const int hi = std::min(frames - 1, k + meanRadius);
        double sum = 0.0;
        for (int j = lo; j <= hi; ++j)
            sum += flux[j];
        const double mean = sum / static_cast<double>(hi - lo + 1);
        const double threshold = mean * cfg.sensitivity;

        // しきい値超え かつ 局所最大。
        if (flux[k] <= threshold)
            continue;
        if (!(flux[k] >= flux[k - 1] && flux[k] > flux[k + 1]))
            continue;

        const double tSec =
            (static_cast<double>(k) * hop + hop / 2.0) / sampleRate;
        if (tSec - lastBeatSec < minIntervalSec)
            continue; // デバウンス
        beats.push_back(tSec);
        lastBeatSec = tSec;
    }

    result.beatTimesSec = beats;

    // BPM 推定: 隣接間隔の中央値 → 60/median を [minBpm,maxBpm] に折り込む。
    if (beats.size() >= 2) {
        QVector<double> intervals;
        intervals.reserve(beats.size() - 1);
        for (int i = 1; i < beats.size(); ++i)
            intervals.push_back(beats[i] - beats[i - 1]);
        std::sort(intervals.begin(), intervals.end());
        const int m = intervals.size();
        const double medianInterval =
            (m % 2 == 1) ? intervals[m / 2]
                         : 0.5 * (intervals[m / 2 - 1] + intervals[m / 2]);
        if (medianInterval > 0.0) {
            double bpm = 60.0 / medianInterval;
            const double minBpm = cfg.minBpm > 0.0 ? cfg.minBpm : 60.0;
            // テンポ・オクターブ折り込み。
            int guard = 0;
            while (bpm < minBpm && bpm > 0.0 && guard++ < 8)
                bpm *= 2.0;
            guard = 0;
            while (bpm > maxBpm && guard++ < 8)
                bpm *= 0.5;
            if (bpm < minBpm) bpm = minBpm;
            if (bpm > maxBpm) bpm = maxBpm;
            result.bpm = bpm;
        }
    }

    return result;
}

} // namespace beatdetect
