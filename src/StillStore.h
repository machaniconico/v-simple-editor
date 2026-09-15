#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

class QImage;

namespace stillstore {

struct Still {
    QString id;
    QString filePath;
    QDateTime timestamp;
    QString projectName;
    QString label;
};

class StillStore
{
public:
    StillStore() = default;

    // Test seam: a non-empty value is used as the gallery directory itself.
    void setBaseDirOverride(const QString &directory);
    QString baseDir() const;

    QVector<Still> list(QString *error = nullptr) const;
    bool save(const QImage &image, const QString &projectName,
              const QString &label = QString(), Still *saved = nullptr,
              QString *error = nullptr);
    bool remove(const QString &id, QString *error = nullptr);
    bool setLabel(const QString &id, const QString &label,
                  QString *error = nullptr);

private:
    bool ensureBaseDir(QString *error) const;
    bool writeIndex(const QVector<Still> &stills, QString *error) const;

    QString m_baseDirOverride;
};

} // namespace stillstore
