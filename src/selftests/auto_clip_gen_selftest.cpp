#include <QDebug>
#include <QString>
#include <QVector>
#include <QRect>

#include "../AutoClipGenerator.h"
#include "../AIHighlight.h"
#include "../Timeline.h"

namespace {

bool nearlyEqual(double lhs, double rhs)
{
    const double diff = lhs > rhs ? lhs - rhs : rhs - lhs;
    return diff < 0.001;
}

Highlight makeHighlight(double start, double end, double score, const QString& description = QString())
{
    Highlight highlight;
    highlight.startTime = start;
    highlight.endTime = end;
    highlight.score = score;
    highlight.description = description;
    return highlight;
}

bool plansWithinSourceAndSorted(const QVector<autoclip::ClipPlan>& plans, double sourceDurationSec)
{
    double previousStart = -1.0;
    for (const autoclip::ClipPlan& plan : plans) {
        if (plan.startSec < 0.0
            || plan.endSec > sourceDurationSec
            || plan.endSec <= plan.startSec
            || plan.startSec < previousStart) {
            return false;
        }
        previousStart = plan.startSec;
    }
    return true;
}

} // namespace

int runAutoClipGenSelftest()
{
    qInfo().noquote() << "[auto-clip-gen] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[auto-clip-gen] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[auto-clip-gen] FAIL" << name << ":" << msg; };

    using autoclip::AutoClipConfig;
    using autoclip::AutoClipGenerator;
    using autoclip::ClipPlan;

    // G1: planClips ranges
    AutoClipConfig rangeConfig;
    rangeConfig.minDurationSec = 1.0;
    rangeConfig.maxDurationSec = 12.0;
    rangeConfig.maxClips = 3;
    const double sourceDurationSec = 40.0;
    const QVector<Highlight> rangeHighlights = {
        makeHighlight(22.0, 26.0, 0.52, QStringLiteral("third")),
        makeHighlight(-3.0, 2.0, 0.84, QStringLiteral("clamped start")),
        makeHighlight(10.0, 12.5, 0.91, QStringLiteral("best")),
        makeHighlight(37.0, 45.0, 0.61, QStringLiteral("clamped end"))
    };
    const QVector<ClipPlan> rangePlans = AutoClipGenerator::planClips(
        rangeHighlights,
        sourceDurationSec,
        rangeConfig);
    const bool rangeOk = !rangePlans.isEmpty()
        && rangePlans.size() <= rangeConfig.maxClips
        && plansWithinSourceAndSorted(rangePlans, sourceDurationSec);
    if (rangeOk) {
        pass("G1 planClips keeps ranges inside source sorted and maxClips bounded");
    } else {
        fail("G1 planClips ranges", QStringLiteral("count=%1 max=%2 source=%3")
                .arg(rangePlans.size())
                .arg(rangeConfig.maxClips)
                .arg(sourceDurationSec));
    }

    // G2: clamp+merge+duration
    AutoClipConfig mergeConfig;
    mergeConfig.minDurationSec = 0.0;
    mergeConfig.maxDurationSec = 20.0;
    mergeConfig.maxClips = 10;
    const QVector<ClipPlan> mergedPlans = AutoClipGenerator::planClips(
        {
            makeHighlight(10.0, 12.0, 0.4, QStringLiteral("overlap a")),
            makeHighlight(11.0, 13.0, 0.8, QStringLiteral("overlap b"))
        },
        60.0,
        mergeConfig);
    const bool mergeOk = mergedPlans.size() == 1
        && nearlyEqual(mergedPlans.at(0).startSec, 10.0)
        && nearlyEqual(mergedPlans.at(0).endSec, 13.0)
        && nearlyEqual(mergedPlans.at(0).score, 0.8);

    AutoClipConfig durationConfig;
    durationConfig.minDurationSec = 3.0;
    durationConfig.maxDurationSec = 5.0;
    durationConfig.maxClips = 10;
    const QVector<ClipPlan> minPlans = AutoClipGenerator::planClips(
        { makeHighlight(20.0, 20.5, 0.5, QStringLiteral("short")) },
        60.0,
        durationConfig);
    const bool minOk = minPlans.size() == 1
        && nearlyEqual(minPlans.at(0).startSec, 20.0)
        && nearlyEqual(minPlans.at(0).endSec, 23.0)
        && nearlyEqual(minPlans.at(0).durationSec(), durationConfig.minDurationSec);

    const QVector<ClipPlan> maxPlans = AutoClipGenerator::planClips(
        { makeHighlight(30.0, 42.0, 0.7, QStringLiteral("long")) },
        60.0,
        durationConfig);
    const bool maxOk = maxPlans.size() == 1
        && nearlyEqual(maxPlans.at(0).startSec, 30.0)
        && nearlyEqual(maxPlans.at(0).endSec, 35.0)
        && nearlyEqual(maxPlans.at(0).durationSec(), durationConfig.maxDurationSec);

    if (mergeOk && minOk && maxOk) {
        pass("G2 planClips merges overlap extends min and truncates max");
    } else {
        fail("G2 clamp merge duration", QStringLiteral("merge=%1 min=%2 max=%3")
                .arg(mergeOk)
                .arg(minOk)
                .arg(maxOk));
    }

    // G3: computeCropRect 9:16
    const QRect crop = AutoClipGenerator::computeCropRect(1920, 1080, 9.0 / 16.0);
    const QRect emptyCrop = AutoClipGenerator::computeCropRect(1920, 1080, 0.0);
    const double cropAspect = crop.height() > 0
        ? static_cast<double>(crop.width()) / static_cast<double>(crop.height())
        : 0.0;
    const bool cropOk = crop.height() == 1080
        && crop.width() == 608
        && crop.x() == 656
        && crop.y() == 0
        && nearlyEqual(cropAspect, 9.0 / 16.0)
        && emptyCrop.isNull();
    if (cropOk) {
        pass("G3 computeCropRect returns centered 9:16 crop and empty invalid aspect");
    } else {
        fail("G3 computeCropRect 9:16", QStringLiteral("rect=(%1,%2 %3x%4) aspect=%5 empty=%6")
                .arg(crop.x())
                .arg(crop.y())
                .arg(crop.width())
                .arg(crop.height())
                .arg(cropAspect)
                .arg(emptyCrop.isNull()));
    }

    // G4: toTimelineClips
    const QString sourceFilePath = QStringLiteral("/tmp/source.mp4");
    const QVector<ClipPlan> timelinePlans = {
        {0.0, 4.0, 0.9, QStringLiteral("first"), false, QRect()},
        {0.0, 11.25, 0.7, QStringLiteral("second"), false, QRect()}
    };
    const QVector<ClipInfo> timelineClips = AutoClipGenerator::toTimelineClips(
        timelinePlans,
        sourceFilePath,
        120.0);
    bool timelineOk = timelineClips.size() == timelinePlans.size();
    for (int i = 0; timelineOk && i < timelineClips.size(); ++i) {
        const ClipInfo& clip = timelineClips.at(i);
        const ClipPlan& plan = timelinePlans.at(i);
        timelineOk = clip.filePath == sourceFilePath
            && nearlyEqual(clip.inPoint, plan.startSec)
            && nearlyEqual(clip.outPoint, plan.endSec)
            && nearlyEqual(clip.effectiveDuration(), clip.outPoint);
    }
    if (timelineOk) {
        pass("G4 toTimelineClips preserves count file path inPoint outPoint and effectiveDuration");
    } else {
        fail("G4 toTimelineClips", QStringLiteral("clipCount=%1 planCount=%2")
                .arg(timelineClips.size())
                .arg(timelinePlans.size()));
    }

    qInfo().noquote().nospace() << "[auto-clip-gen] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
