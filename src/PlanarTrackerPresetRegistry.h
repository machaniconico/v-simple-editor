#pragma once
#include "PlanarTrackerPreset.h"
#include <QList>
#include <QString>
#include <optional>

namespace planar_tracker_preset {

class Registry {
public:
    static Registry& instance();

    QList<PlanarTrackerPreset> allPresets() const;       // built-in 5 + user
    QList<PlanarTrackerPreset> userPresets() const;
    std::optional<PlanarTrackerPreset> findByDisplayName(const QString& name) const;

    bool saveUserPreset(const PlanarTrackerPreset& p);   // built-in id 衝突は false
    bool removeUserPreset(const QString& id);            // built-in は false
    void reloadFromSettings();

private:
    Registry();
    ~Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    QList<PlanarTrackerPreset> m_userPresets;
};

} // namespace planar_tracker_preset
