#pragma once
#include "TrackerPreset.h"
#include <QList>
#include <QString>
#include <optional>

namespace tracker_preset {

class Registry {
public:
    static Registry& instance();

    QList<TrackerPreset> allPresets() const;        // built-in 7 + user の連結
    QList<TrackerPreset> userPresets() const;       // user のみ
    std::optional<TrackerPreset> findByDisplayName(const QString& name) const;

    bool saveUserPreset(const TrackerPreset& p);    // id 重複時は上書き、QSettings へ書く
    bool removeUserPreset(const QString& id);       // built-in id は false 返し
    void reloadFromSettings();                       // QSettings から再読込

private:
    Registry();
    ~Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    QList<TrackerPreset> m_userPresets;
};

} // namespace tracker_preset
