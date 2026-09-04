#include "MusicRemix.h"

#include <algorithm>
#include <cmath>
#include <QtGlobal>

namespace remix {

namespace {

constexpr double kEpsilon = 1.0e-9;

struct Interval {
    double start = 0.0;
    double end = 0.0;

    double duration() const { return end - start; }
};

QVector<double> normalizedBoundaries(const QVector<double> &beatTimes,
                                     double clipDuration)
{
    QVector<double> beats;
    beats.reserve(beatTimes.size());
    for (double beat : beatTimes) {
        if (!std::isfinite(beat))
            continue;
        if (beat < -kEpsilon || beat > clipDuration + kEpsilon)
            continue;
        beats.append(qBound(0.0, beat, clipDuration));
    }
    std::sort(beats.begin(), beats.end());

    QVector<double> boundaries;
    boundaries.reserve(beats.size() + 2);
    boundaries.append(0.0);
    for (double beat : beats) {
        if (qAbs(beat - boundaries.last()) > kEpsilon)
            boundaries.append(beat);
    }
    if (qAbs(boundaries.last() - clipDuration) > kEpsilon)
        boundaries.append(clipDuration);
    else
        boundaries.last() = clipDuration;
    return boundaries;
}

QVector<Interval> makeIntervals(const QVector<double> &boundaries)
{
    QVector<Interval> intervals;
    intervals.reserve(boundaries.size() > 1 ? boundaries.size() - 1 : 0);
    for (int i = 0; i + 1 < boundaries.size(); ++i) {
        if (boundaries[i + 1] - boundaries[i] > kEpsilon)
            intervals.append({boundaries[i], boundaries[i + 1]});
    }
    return intervals;
}

} // namespace

Plan planRemix(const QVector<double> &beatTimes,
               double clipDuration,
               double targetDuration,
               const Config &config)
{
    Plan plan;
    plan.crossfadeSec = config.crossfadeSec;

    if (beatTimes.size() < 2) {
        plan.error = QStringLiteral("ビート境界が 2 個未満です");
        return plan;
    }
    if (!std::isfinite(clipDuration) || clipDuration <= 0.0) {
        plan.error = QStringLiteral("クリップ尺が不正です");
        return plan;
    }
    if (!std::isfinite(targetDuration) || targetDuration <= 0.0) {
        plan.error = QStringLiteral("目標尺は 0 より大きい有限値で指定してください");
        return plan;
    }
    if (!std::isfinite(plan.crossfadeSec) || plan.crossfadeSec < 0.0) {
        plan.error = QStringLiteral("クロスフェード時間が不正です");
        return plan;
    }

    const QVector<double> boundaries = normalizedBoundaries(beatTimes, clipDuration);
    const QVector<Interval> intervals = makeIntervals(boundaries);
    if (intervals.isEmpty()) {
        plan.error = QStringLiteral("クリップのビート区間を作成できません");
        return plan;
    }

    QVector<Interval> selectedIntervals;
    QVector<Interval> repeatedIntervals;
    QVector<bool> removed(intervals.size(), false);

    if (targetDuration < clipDuration - kEpsilon) {
        double remainingToRemove = clipDuration - targetDuration;
        // Greedily choose the still-present interior beat interval which
        // leaves the smallest absolute target error. Ties keep source order.
        while (remainingToRemove > kEpsilon && intervals.size() > 2) {
            int bestIndex = -1;
            double bestError = remainingToRemove;
            for (int i = 1; i + 1 < intervals.size(); ++i) {
                if (removed[i])
                    continue;
                const double candidateError =
                    qAbs(remainingToRemove - intervals[i].duration());
                if (candidateError + kEpsilon < bestError) {
                    bestError = candidateError;
                    bestIndex = i;
                }
            }
            if (bestIndex < 0)
                break;
            removed[bestIndex] = true;
            remainingToRemove -= intervals[bestIndex].duration();
        }
    } else if (targetDuration > clipDuration + kEpsilon
               && intervals.size() > 2) {
        double remainingToAdd = targetDuration - clipDuration;
        // Repeating one interior beat interval at a time makes the result
        // deterministic and keeps every inserted boundary musical.
        while (remainingToAdd > kEpsilon) {
            int bestIndex = -1;
            double bestError = remainingToAdd;
            for (int i = 1; i + 1 < intervals.size(); ++i) {
                const double candidateError =
                    qAbs(remainingToAdd - intervals[i].duration());
                if (candidateError + kEpsilon < bestError) {
                    bestError = candidateError;
                    bestIndex = i;
                }
            }
            if (bestIndex < 0)
                break;
            repeatedIntervals.append(intervals[bestIndex]);
            remainingToAdd -= intervals[bestIndex].duration();
        }
    }

    if (intervals.size() == 1) {
        selectedIntervals.append(intervals.first());
    } else {
        selectedIntervals.append(intervals.first());
        for (int i = 1; i + 1 < intervals.size(); ++i) {
            if (!removed[i])
                selectedIntervals.append(intervals[i]);
        }
        selectedIntervals += repeatedIntervals;
        selectedIntervals.append(intervals.last());
    }

    for (const Interval &interval : selectedIntervals)
        plan.segments.append({interval.start, interval.end});
    for (const Segment &segment : plan.segments)
        plan.resultDuration += segment.srcEnd - segment.srcStart;

    if (plan.segments.isEmpty() || plan.resultDuration <= 0.0) {
        plan.segments.clear();
        plan.resultDuration = 0.0;
        plan.error = QStringLiteral("リミックス区間を作成できません");
        return plan;
    }

    double totalIntervalDuration = 0.0;
    for (const Interval &interval : intervals)
        totalIntervalDuration += interval.duration();
    const double averageIntervalDuration =
        totalIntervalDuration / intervals.size();
    if (qAbs(plan.resultDuration - targetDuration)
        > averageIntervalDuration + kEpsilon) {
        const double reachableDuration = plan.resultDuration;
        plan.segments.clear();
        plan.resultDuration = 0.0;
        plan.error = QStringLiteral(
                         "目標尺 %1 秒はビート境界で実現できません (到達可能: %2 秒)")
                         .arg(targetDuration, 0, 'f', 3)
                         .arg(reachableDuration, 0, 'f', 3);
        return plan;
    }
    plan.valid = true;
    return plan;
}

} // namespace remix
