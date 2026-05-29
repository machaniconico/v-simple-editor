#include "CredentialAuditLog.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>

namespace creds {

namespace {

constexpr qint64 kMaxLogBytes = 1024 * 1024;

QMutex& auditLogMutex()
{
    static QMutex mutex;
    return mutex;
}

int rotateIfNeededUnlocked(const QString& path, bool force = false)
{
    const QFileInfo fileInfo(path);
    if (!fileInfo.exists() || (!force && fileInfo.size() <= kMaxLogBytes)) {
        return 0;
    }

    const QString rotatedPath = path + QStringLiteral(".1");
    QFile::remove(rotatedPath);
    if (!QFile::rename(path, rotatedPath)) {
        return 0;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return 0;
    }
    file.close();
    return 1;
}

} // namespace

QString CredentialAuditLog::eventTypeLabel(EventType type)
{
    switch (type) {
    case EventType::AccessTokenLoaded:
        return QStringLiteral("ACCESS_TOKEN_LOADED");
    case EventType::RefreshFired:
        return QStringLiteral("REFRESH_FIRED");
    case EventType::RefreshSucceeded:
        return QStringLiteral("REFRESH_SUCCEEDED");
    case EventType::RefreshFailed:
        return QStringLiteral("REFRESH_FAILED");
    case EventType::CredentialRotated:
        return QStringLiteral("CREDENTIAL_ROTATED");
    case EventType::CredentialExpired:
        return QStringLiteral("CREDENTIAL_EXPIRED");
    case EventType::AccessTokenSet:
        return QStringLiteral("ACCESS_TOKEN_SET");
    }

    return QStringLiteral("UNKNOWN");
}

CredentialAuditLog::EventType CredentialAuditLog::eventTypeFromLabel(const QString& label)
{
    if (label == QStringLiteral("ACCESS_TOKEN_LOADED")) return EventType::AccessTokenLoaded;
    if (label == QStringLiteral("REFRESH_FIRED")) return EventType::RefreshFired;
    if (label == QStringLiteral("REFRESH_SUCCEEDED")) return EventType::RefreshSucceeded;
    if (label == QStringLiteral("REFRESH_FAILED")) return EventType::RefreshFailed;
    if (label == QStringLiteral("CREDENTIAL_ROTATED")) return EventType::CredentialRotated;
    if (label == QStringLiteral("CREDENTIAL_EXPIRED")) return EventType::CredentialExpired;
    if (label == QStringLiteral("ACCESS_TOKEN_SET")) return EventType::AccessTokenSet;
    return EventType::AccessTokenLoaded;
}

QString CredentialAuditLog::maskSettingsKey(const QString& settingsKey)
{
    if (settingsKey.size() <= 8) {
        return settingsKey + QStringLiteral("***");
    }

    return settingsKey.left(8) + QStringLiteral("***");
}

QString CredentialAuditLog::logFilePath()
{
    const QString basePath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (basePath.isEmpty()) {
        return QStringLiteral("audit.log");
    }

    QDir().mkpath(basePath);
    return basePath + QStringLiteral("/audit.log");
}

int CredentialAuditLog::rotateIfNeeded()
{
    QMutexLocker locker(&auditLogMutex());
    return rotateIfNeededUnlocked(logFilePath());
}

void CredentialAuditLog::logEvent(EventType type,
                                  const QString& platform,
                                  const QString& settingsKey,
                                  const QString& message)
{
    QMutexLocker locker(&auditLogMutex());

    QJsonObject o;
    o.insert(QStringLiteral("ts"),
             QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ")));
    o.insert(QStringLiteral("type"), eventTypeLabel(type));
    o.insert(QStringLiteral("platform"), platform);
    o.insert(QStringLiteral("key"), maskSettingsKey(settingsKey));
    o.insert(QStringLiteral("message"), message);

    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    const QString path = logFilePath();
    const QFileInfo fileInfo(path);
    if (fileInfo.exists() && fileInfo.size() + line.size() > kMaxLogBytes) {
        rotateIfNeededUnlocked(path, true);
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return;
    }
    file.write(line);
    file.close();
}

QList<CredentialAuditLog::Entry> CredentialAuditLog::readEntries()
{
    QMutexLocker locker(&auditLogMutex());
    QList<Entry> result;
    QFile file(logFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }

        const QJsonObject o = doc.object();
        Entry e;
        const QString ts = o.value(QStringLiteral("ts")).toString();
        e.timestamp = QDateTime::fromString(ts, Qt::ISODate);
        if (e.timestamp.isValid()) {
            e.timestamp.setTimeSpec(Qt::UTC);
        }
        e.type = eventTypeFromLabel(o.value(QStringLiteral("type")).toString());
        e.platform = o.value(QStringLiteral("platform")).toString();
        e.settingsKey = o.value(QStringLiteral("key")).toString();
        e.message = o.value(QStringLiteral("message")).toString();
        result.append(e);
    }

    file.close();
    return result;
}

int CredentialAuditLog::purgeOlderThanDays(int days)
{
    QMutexLocker locker(&auditLogMutex());
    const QString path = logFilePath();
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        return 0;
    }

    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-static_cast<qint64>(days));
    QByteArray kept;
    int purged = 0;
    while (!in.atEnd()) {
        const QByteArray line = in.readLine();
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            kept += line.endsWith('\n') ? line : (line + '\n');
            continue;
        }

        const QJsonObject o = doc.object();
        const QString ts = o.value(QStringLiteral("ts")).toString();
        QDateTime entryTs = QDateTime::fromString(ts, Qt::ISODate);
        if (entryTs.isValid()) {
            entryTs.setTimeSpec(Qt::UTC);
        }
        if (entryTs.isValid() && entryTs < cutoff) {
            ++purged;
            continue;
        }

        kept += line.endsWith('\n') ? line : (line + '\n');
    }
    in.close();

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        return 0;
    }
    out.write(kept);
    if (!out.commit()) {
        return 0;
    }
    return purged;
}

} // creds
