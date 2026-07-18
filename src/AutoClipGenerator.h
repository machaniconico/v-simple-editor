#pragma once

#include "AIHighlight.h"
#include "Timeline.h"

#include <QRect>
#include <QString>
#include <QVector>

namespace autoclip {

struct AutoClipConfig {
    double minDurationSec = 3.0;
    double maxDurationSec = 60.0;
    int maxClips = 10;
    double targetAspect = 0.0;
};

struct ClipPlan {
    double startSec = 0.0;
    double endSec = 0.0;
    double score = 0.0;
    QString label;
    bool hasCrop = false;
    QRect cropRect;

    double durationSec() const { return endSec - startSec; }
};

class AutoClipGenerator {
public:
    static QVector<ClipPlan> planClips(const QVector<Highlight>& highlights,
                                       double sourceDurationSec,
                                       const AutoClipConfig& config);
    static QRect computeCropRect(int srcW, int srcH, double targetAspect);
    static QVector<ClipInfo> toTimelineClips(const QVector<ClipPlan>& plans,
                                             const QString& sourceFilePath,
                                             double sourceDurationSec);
};

} // namespace autoclip
