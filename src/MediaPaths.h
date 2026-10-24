#pragma once

#include <QHash>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

struct ProjectData;

namespace mediapaths {

struct PathSlot {
    QString *field = nullptr;
    QString category;

    PathSlot() = default;
    PathSlot(QString *directField, QString slotCategory);
    PathSlot(QString value, QString slotCategory,
             std::function<void(const QString &)> writer);

    QString value() const;
    bool setValue(const QString &newValue);

private:
    QSharedPointer<QString> m_ownedField;
    std::function<void(const QString &)> m_writer;
};

QVector<PathSlot> enumeratePathSlots(ProjectData &data);
QStringList missingFiles(const ProjectData &data);
// True for a file path that is absent or not a regular file; null-object and
// nested-sequence references are internal and never missing.
bool isMissingFilesystemPath(const QString &path);
int replacePaths(ProjectData &data,
                 const QHash<QString, QString> &oldToNew);

} // namespace mediapaths
