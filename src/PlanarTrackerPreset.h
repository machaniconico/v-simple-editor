#pragma once
#include <QString>
#include <QJsonObject>
#include <array>
#include <optional>
#include "PlanarTracker.h"

namespace planar_tracker_preset {

struct PlanarTrackerPreset {
    QString id;
    QString displayName;
    QString description;
    double  searchRadiusPx    = 16.0;
    double  patchSizePx       = 32.0;
    double  dampingFactor     = 0.3;
    int     maxFramesPerCall  = 0;
};

const std::array<PlanarTrackerPreset, 5>& builtinPresets();
std::optional<PlanarTrackerPreset> findBuiltin(const QString& id);
QJsonObject toJson(const PlanarTrackerPreset& p);
std::optional<PlanarTrackerPreset> fromJson(const QJsonObject& j);

// MotionTrackerDialog 経由ではなく、planar::Tracker インスタンスへ直接 preset を
// 適用する helper。nullptr safe — tracker==nullptr で何もせず return。
void applyToPlanarTracker(planar::Tracker* tracker, const PlanarTrackerPreset& p);

} // namespace planar_tracker_preset
