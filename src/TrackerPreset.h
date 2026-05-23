#pragma once
#include <QString>
#include <QJsonObject>
#include <array>
#include <optional>
#include "MotionTracker.h"

namespace tracker_preset {

struct TrackerPreset {
    QString id;
    QString displayName;
    QString regionShape        = "rect";
    QString matchMetric        = "NCC";   // NCC / SSD / ZNCC
    int     searchRadius       = 24;
    bool    kalmanEnabled      = true;
    double  kalmanProcessNoise      = 0.1;
    double  kalmanMeasurementNoise  = 1.0;
    double  occlusionGate      = 0.5;
    bool    subPixelEnabled    = true;
    double  minConfidence      = 0.7;
};

const std::array<TrackerPreset, 7>& builtinPresets();
std::optional<TrackerPreset> findBuiltin(const QString& id);

QJsonObject toJson(const TrackerPreset& p);
std::optional<TrackerPreset> fromJson(const QJsonObject& j);

void applyToMotionTracker(MotionTracker* tracker, const TrackerPreset& p);

} // namespace tracker_preset
