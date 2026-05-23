#include "TrackerPreset.h"
#include "MotionTracker.h"
#include <QJsonValue>
#include <QDebug>

namespace tracker_preset {

const std::array<TrackerPreset, 7>& builtinPresets()
{
    static const std::array<TrackerPreset, 7> kPresets = {{
        // 1. slow-pan-static-bg
        {
            "slow-pan-static-bg",
            "Slow Pan, Static Background",
            "rect", "NCC",
            24, true, 0.08, 1.0, 0.5, true, 0.75
        },
        // 2. fast-action-handheld
        {
            "fast-action-handheld",
            "Fast Action, Handheld Camera",
            "rect", "ZNCC",
            48, true, 0.15, 0.8, 0.3, true, 0.6
        },
        // 3. logo-corner-static
        {
            "logo-corner-static",
            "Logo Corner, Locked-Off Camera",
            "rect", "SSD",
            12, false, 0.1, 1.0, 0.8, true, 0.85
        },
        // 4. occlusion-prone-walk
        {
            "occlusion-prone-walk",
            "Occlusion-Prone Walk",
            "rect", "ZNCC",
            40, true, 0.10, 2.0, 0.25, true, 0.55
        },
        // 5. tight-product-shot
        {
            "tight-product-shot",
            "Tight Product Shot",
            "rect", "NCC",
            8, true, 0.03, 1.2, 0.6, true, 0.80
        },
        // 6. scene-change-resilient
        {
            "scene-change-resilient",
            "Scene Change Resilient",
            "rect", "ZNCC",
            60, true, 0.20, 2.5, 0.15, false, 0.50
        },
        // 7. manual-keyframe-only
        {
            "manual-keyframe-only",
            "Manual Keyframes Only",
            "rect", "NCC",
            0, false, 0.1, 1.0, 1.0, false, 1.0
        },
    }};
    return kPresets;
}

std::optional<TrackerPreset> findBuiltin(const QString& id)
{
    for (const auto& p : builtinPresets()) {
        if (p.id == id)
            return p;
    }
    return std::nullopt;
}

QJsonObject toJson(const TrackerPreset& p)
{
    QJsonObject obj;
    obj["id"]                    = p.id;
    obj["displayName"]           = p.displayName;
    obj["regionShape"]           = p.regionShape;
    obj["matchMetric"]           = p.matchMetric;
    obj["searchRadius"]          = p.searchRadius;
    obj["kalmanEnabled"]         = p.kalmanEnabled;
    obj["kalmanProcessNoise"]    = p.kalmanProcessNoise;
    obj["kalmanMeasurementNoise"]= p.kalmanMeasurementNoise;
    obj["occlusionGate"]         = p.occlusionGate;
    obj["subPixelEnabled"]       = p.subPixelEnabled;
    obj["minConfidence"]         = p.minConfidence;
    return obj;
}

std::optional<TrackerPreset> fromJson(const QJsonObject& j)
{
    // Required string fields
    if (!j.contains("id") || !j.contains("displayName"))
        return std::nullopt;

    TrackerPreset p;
    p.id          = j["id"].toString();
    p.displayName = j["displayName"].toString();
    if (p.id.isEmpty() || p.displayName.isEmpty())
        return std::nullopt;

    p.regionShape = j.value("regionShape").toString("rect");

    // matchMetric validation
    const QString metric = j.value("matchMetric").toString("NCC");
    if (metric != "NCC" && metric != "SSD" && metric != "ZNCC")
        return std::nullopt;
    p.matchMetric = metric;

    // searchRadius >= 0
    const int sr = j.value("searchRadius").toInt(-1);
    if (sr < 0)
        return std::nullopt;
    p.searchRadius = sr;

    p.kalmanEnabled = j.value("kalmanEnabled").toBool(true);

    // kalmanProcessNoise in [0.0, 10.0]
    const double kpn = j.value("kalmanProcessNoise").toDouble(-1.0);
    if (kpn < 0.0 || kpn > 10.0)
        return std::nullopt;
    p.kalmanProcessNoise = kpn;

    // kalmanMeasurementNoise in [0.0, 10.0]
    const double kmn = j.value("kalmanMeasurementNoise").toDouble(-1.0);
    if (kmn < 0.0 || kmn > 10.0)
        return std::nullopt;
    p.kalmanMeasurementNoise = kmn;

    // occlusionGate in [0.0, 1.0]
    const double og = j.value("occlusionGate").toDouble(-1.0);
    if (og < 0.0 || og > 1.0)
        return std::nullopt;
    p.occlusionGate = og;

    p.subPixelEnabled = j.value("subPixelEnabled").toBool(true);

    // minConfidence in [0.0, 1.0]
    const double mc = j.value("minConfidence").toDouble(-1.0);
    if (mc < 0.0 || mc > 1.0)
        return std::nullopt;
    p.minConfidence = mc;

    return p;
}

namespace {
MotionTracker::MatchMetric stringToMatchMetric(const QString& s)
{
    const QString u = s.trimmed().toUpper();
    if (u == "NCC")  return MotionTracker::MatchMetric::NCC;
    if (u == "SSD")  return MotionTracker::MatchMetric::SSD;
    if (u == "ZNCC") return MotionTracker::MatchMetric::ZNCC;
    qWarning() << "tracker_preset: unknown matchMetric" << s << "— falling back to NCC";
    return MotionTracker::MatchMetric::NCC;
}
} // anonymous namespace

void applyToMotionTracker(MotionTracker* tracker, const TrackerPreset& p)
{
    if (!tracker) return;
    tracker->setSearchRadius(p.searchRadius);
    tracker->setMatchMetric(stringToMatchMetric(p.matchMetric));
    tracker->setKalmanEnabled(p.kalmanEnabled);
    tracker->setKalmanProcessNoise(p.kalmanProcessNoise);
    tracker->setKalmanMeasurementNoise(p.kalmanMeasurementNoise);
    tracker->setOcclusionThreshold(p.occlusionGate);
    tracker->setSubPixelPrecision(p.subPixelEnabled);
    tracker->setMinConfidence(p.minConfidence);
}

} // namespace tracker_preset
