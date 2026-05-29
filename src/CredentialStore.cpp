#include "CredentialStore.h"
#include "CredentialVault.h"

#include <QDateTime>
#include <QSettings>
#include <QStringList>
#include <QtGlobal>

namespace {

constexpr const char* kVaultTargetPrefix = "v-simple-editor/";

const QStringList& sensitiveKeyList()
{
    static const QStringList list = {
        QStringLiteral("youtube_oauth/client_id"),
        QStringLiteral("youtube_oauth/client_secret"),
        QStringLiteral("youtube_oauth/refresh_token"),
        QStringLiteral("vimeo_oauth/client_id"),
        QStringLiteral("vimeo_oauth/client_secret"),
        QStringLiteral("vimeo_oauth/access_token"),
        QStringLiteral("vimeo_oauth/refresh_token"),
        QStringLiteral("instagram/access_token"),
        QStringLiteral("instagram/ig_user_id"),
        QStringLiteral("x_video/bearer_token"),
        QStringLiteral("twitch/stream_key"),
    };
    return list;
}

bool isSensitiveKey(const QString& settingsKey)
{
    return sensitiveKeyList().contains(settingsKey);
}

QString vaultTargetFor(const QString& settingsKey)
{
    return QString::fromLatin1(kVaultTargetPrefix) + settingsKey;
}

QString expiryMetaKey(const QString& settingsKey)
{
    return settingsKey + QStringLiteral("__expires_at");
}

} // namespace

QString creds::CredentialStore::get(const char* envName,
                                    const QString& settingsKey,
                                    const QString& defaultValue,
                                    bool emitWarning)
{
    QString value;
    if (envName != nullptr && *envName != '\0') {
        value = qEnvironmentVariable(envName).trimmed();
    }

    if (value.isEmpty() && isSensitiveKey(settingsKey) && creds::CredentialVault::isSupported()) {
        value = creds::CredentialVault::retrieve(vaultTargetFor(settingsKey)).trimmed();
    }

    if (value.isEmpty()) {
        QSettings settings;
        value = settings.value(settingsKey).toString().trimmed();
    }

    if (value.isEmpty()) {
        if (emitWarning) {
            const QString envLabel =
                (envName != nullptr && *envName != '\0') ? QString::fromLatin1(envName)
                                                         : QStringLiteral("(env skipped)");
            qWarning().noquote()
                << QStringLiteral("Credential missing %1 / %2, using default value.")
                       .arg(envLabel, settingsKey);
        }
        return defaultValue;
    }

    return value;
}

void creds::CredentialStore::set(const QString& settingsKey, const QString& value)
{
    if (value.isEmpty()) {
        if (isSensitiveKey(settingsKey) && creds::CredentialVault::isSupported()) {
            creds::CredentialVault::erase(vaultTargetFor(settingsKey));
        }

        QSettings settings;
        settings.remove(settingsKey);
        return;
    }

    if (isSensitiveKey(settingsKey) && creds::CredentialVault::isSupported()) {
        if (creds::CredentialVault::store(vaultTargetFor(settingsKey), value)) {
            QSettings settings;
            settings.remove(settingsKey);
            return;
        }
    }

    QSettings settings;
    settings.setValue(settingsKey, value);
}

void creds::CredentialStore::clear(const QString& settingsKey)
{
    if (isSensitiveKey(settingsKey) && creds::CredentialVault::isSupported()) {
        creds::CredentialVault::erase(vaultTargetFor(settingsKey));
    }

    QSettings settings;
    settings.remove(settingsKey);
}

bool creds::CredentialStore::has(const char* envName, const QString& settingsKey)
{
    if (envName != nullptr && *envName != '\0' && !qEnvironmentVariable(envName).trimmed().isEmpty()) {
        return true;
    }

    if (isSensitiveKey(settingsKey) && creds::CredentialVault::isSupported()
        && creds::CredentialVault::exists(vaultTargetFor(settingsKey))) {
        return true;
    }

    QSettings settings;
    return !settings.value(settingsKey).toString().trimmed().isEmpty();
}

QString creds::CredentialStore::maskedDisplay(const QString& value)
{
    if (value.isEmpty()) {
        return QStringLiteral("(unset)");
    }
    if (value.size() < 8) {
        return QStringLiteral("***");
    }
    return value.left(4) + QStringLiteral("...") + value.right(4);
}

int creds::CredentialStore::migrateSensitiveKeysToVault()
{
    if (!creds::CredentialVault::isSupported()) {
        return 0;
    }

    QSettings settings;
    int migrated = 0;
    for (const QString& key : sensitiveKeyList()) {
        const QString value = settings.value(key).toString().trimmed();
        if (value.isEmpty()) {
            continue;
        }

        if (creds::CredentialVault::store(vaultTargetFor(key), value)) {
            settings.remove(key);
            ++migrated;
        }
    }

    return migrated;
}

void creds::CredentialStore::setExpiry(const QString& settingsKey, const QDateTime& expiresAtUtc)
{
    QSettings settings;
    if (!expiresAtUtc.isValid()) {
        settings.remove(expiryMetaKey(settingsKey));
        return;
    }

    settings.setValue(expiryMetaKey(settingsKey),
                      QVariant::fromValue<qint64>(expiresAtUtc.toSecsSinceEpoch()));
}

QDateTime creds::CredentialStore::getExpiry(const QString& settingsKey)
{
    QSettings settings;
    const QVariant value = settings.value(expiryMetaKey(settingsKey));
    if (!value.isValid() || value.isNull()) {
        return QDateTime();
    }

    bool ok = false;
    const qint64 secs = value.toLongLong(&ok);
    if (!ok || secs <= 0) {
        return QDateTime();
    }

    return QDateTime::fromSecsSinceEpoch(secs, Qt::UTC);
}

void creds::CredentialStore::clearExpiry(const QString& settingsKey)
{
    QSettings settings;
    settings.remove(expiryMetaKey(settingsKey));
}

bool creds::CredentialStore::isExpired(const QString& settingsKey)
{
    const QDateTime expiry = getExpiry(settingsKey);
    if (!expiry.isValid()) {
        return false;
    }

    return expiry < QDateTime::currentDateTimeUtc();
}
