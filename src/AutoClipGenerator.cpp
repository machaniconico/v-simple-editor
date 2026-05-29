#include "AutoClipGenerator.h"

#include <algorithm>
#include <cmath>

namespace autoclip {

namespace {

double clampDouble(double value, double minValue, double maxValue)
{
    return std::min(std::max(value, minValue), maxValue);
}

void enforceDurationBounds(ClipPlan& plan,
                           double sourceDurationSec,
                           double minDurationSec,
                           double maxDurationSec)
{
    const double sourceDuration = std::max(0.0, sourceDurationSec);

    if (minDurationSec > 0.0 && plan.endSec - plan.startSec < minDurationSec) {
        plan.endSec = plan.startSec + minDurationSec;
        if (plan.endSec > sourceDuration) {
            plan.endSec = sourceDuration;
            plan.startSec = std::max(0.0, plan.endSec - minDurationSec);
        }
    }

    if (maxDurationSec > 0.0 && plan.endSec - plan.startSec > maxDurationSec) {
        plan.endSec = std::min(sourceDuration, plan.startSec + maxDurationSec);
    }
}

} // namespace

QVector<ClipPlan> AutoClipGenerator::planClips(const QVector<Highlight>& highlights,
                                               double sourceDurationSec,
                                               const AutoClipConfig& config)
{
    const double sourceDuration = std::max(0.0, sourceDurationSec);
    if (sourceDuration <= 0.0 || config.maxClips <= 0) {
        return {};
    }

    double minDuration = std::max(0.0, config.minDurationSec);
    const double maxDuration = config.maxDurationSec > 0.0
        ? std::min(config.maxDurationSec, sourceDuration)
        : sourceDuration;
    if (maxDuration < minDuration) {
        minDuration = maxDuration;
    }

    QVector<ClipPlan> plans;
    plans.reserve(highlights.size());

    for (const Highlight& highlight : highlights) {
        ClipPlan plan;
        plan.startSec = clampDouble(highlight.startTime, 0.0, sourceDuration);
        plan.endSec = clampDouble(highlight.endTime, 0.0, sourceDuration);
        if (plan.endSec <= plan.startSec) {
            continue;
        }

        enforceDurationBounds(plan, sourceDuration, minDuration, maxDuration);
        if (plan.endSec <= plan.startSec) {
            continue;
        }

        plan.score = highlight.score;
        plan.label = highlight.description;
        plan.hasCrop = config.targetAspect > 0.0;
        plans.append(plan);
    }

    std::stable_sort(plans.begin(), plans.end(), [](const ClipPlan& a, const ClipPlan& b) {
        return a.startSec < b.startSec;
    });

    QVector<ClipPlan> merged;
    merged.reserve(plans.size());
    for (const ClipPlan& plan : plans) {
        if (merged.isEmpty() || plan.startSec > merged.last().endSec) {
            merged.append(plan);
            continue;
        }

        ClipPlan& previous = merged.last();
        previous.endSec = std::max(previous.endSec, plan.endSec);
        previous.score = std::max(previous.score, plan.score);
        previous.hasCrop = previous.hasCrop || plan.hasCrop;
        enforceDurationBounds(previous, sourceDuration, minDuration, maxDuration);
    }

    if (merged.size() > config.maxClips) {
        std::sort(merged.begin(), merged.end(), [](const ClipPlan& a, const ClipPlan& b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            if (a.startSec != b.startSec) {
                return a.startSec < b.startSec;
            }
            if (a.endSec != b.endSec) {
                return a.endSec < b.endSec;
            }
            return a.label < b.label;
        });
        merged.resize(config.maxClips);
        std::sort(merged.begin(), merged.end(), [](const ClipPlan& a, const ClipPlan& b) {
            if (a.startSec != b.startSec) {
                return a.startSec < b.startSec;
            }
            if (a.endSec != b.endSec) {
                return a.endSec < b.endSec;
            }
            if (a.score != b.score) {
                return a.score > b.score;
            }
            return a.label < b.label;
        });
    }

    return merged;
}

QRect AutoClipGenerator::computeCropRect(int srcW, int srcH, double targetAspect)
{
    if (srcW <= 0 || srcH <= 0 || targetAspect <= 0.0) {
        return QRect();
    }

    const double srcAspect = static_cast<double>(srcW) / static_cast<double>(srcH);
    if (srcAspect > targetAspect) {
        const int cropW = std::min(srcW, std::max(1, static_cast<int>(std::round(srcH * targetAspect))));
        const int cropX = (srcW - cropW) / 2;
        return QRect(cropX, 0, cropW, srcH);
    }

    const int cropH = std::min(srcH, std::max(1, static_cast<int>(std::round(srcW / targetAspect))));
    const int cropY = (srcH - cropH) / 2;
    return QRect(0, cropY, srcW, cropH);
}

QVector<ClipInfo> AutoClipGenerator::toTimelineClips(const QVector<ClipPlan>& plans,
                                                     const QString& sourceFilePath,
                                                     double sourceDurationSec)
{
    QVector<ClipInfo> clips;
    clips.reserve(plans.size());

    for (int i = 0; i < plans.size(); ++i) {
        const ClipPlan& plan = plans.at(i);
        ClipInfo clip;
        clip.filePath = sourceFilePath;
        clip.displayName = plan.label.isEmpty()
            ? QStringLiteral("Clip %1").arg(i + 1)
            : plan.label;
        clip.duration = sourceDurationSec;
        clip.inPoint = plan.startSec;
        clip.outPoint = plan.endSec;
        clips.append(clip);
    }

    return clips;
}

} // namespace autoclip
