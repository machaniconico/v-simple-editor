#include "TrackerPresetRegistry.h"
#include <QSettings>
#include <QJsonDocument>
#include <QDebug>

namespace tracker_preset {

Registry& Registry::instance() {
    static Registry s_instance;
    return s_instance;
}

Registry::Registry() {
    reloadFromSettings();
}

QList<TrackerPreset> Registry::allPresets() const {
    QList<TrackerPreset> all;
    for (const auto& p : builtinPresets()) all.append(p);
    all.append(m_userPresets);
    return all;
}

QList<TrackerPreset> Registry::userPresets() const {
    return m_userPresets;
}

std::optional<TrackerPreset> Registry::findByDisplayName(const QString& name) const {
    for (const auto& p : builtinPresets()) {
        if (p.displayName == name) return p;
    }
    for (const auto& p : m_userPresets) {
        if (p.displayName == name) return p;
    }
    return std::nullopt;
}

bool Registry::saveUserPreset(const TrackerPreset& p) {
    if (p.id.isEmpty()) return false;
    // built-in id 衝突チェック
    if (findBuiltin(p.id).has_value()) {
        qWarning() << "Registry::saveUserPreset: id" << p.id << "collides with built-in";
        return false;
    }
    // 既存 user preset を id で探して置換 or 追加
    bool replaced = false;
    for (auto& existing : m_userPresets) {
        if (existing.id == p.id) {
            existing = p;
            replaced = true;
            break;
        }
    }
    if (!replaced) m_userPresets.append(p);

    // QSettings 永続化
    QSettings settings("VSimpleEditor", "Preferences");
    settings.beginGroup("trackerPresets/userPresets");
    const QJsonObject obj = toJson(p);
    settings.setValue(p.id, QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    settings.endGroup();
    return true;
}

bool Registry::removeUserPreset(const QString& id) {
    if (findBuiltin(id).has_value()) return false;
    bool removed = false;
    for (int i = m_userPresets.size() - 1; i >= 0; --i) {
        if (m_userPresets[i].id == id) {
            m_userPresets.removeAt(i);
            removed = true;
        }
    }
    if (removed) {
        QSettings settings("VSimpleEditor", "Preferences");
        settings.beginGroup("trackerPresets/userPresets");
        settings.remove(id);
        settings.endGroup();
    }
    return removed;
}

void Registry::reloadFromSettings() {
    m_userPresets.clear();
    QSettings settings("VSimpleEditor", "Preferences");
    settings.beginGroup("trackerPresets/userPresets");
    const QStringList keys = settings.childKeys();
    for (const QString& key : keys) {
        const QString jsonStr = settings.value(key).toString();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "Registry: skip malformed user preset" << key << err.errorString();
            continue;
        }
        const auto parsed = fromJson(doc.object());
        if (!parsed) {
            qWarning() << "Registry: skip user preset failing validation" << key;
            continue;
        }
        m_userPresets.append(*parsed);
    }
    settings.endGroup();
}

} // namespace tracker_preset
