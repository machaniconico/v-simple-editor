#pragma once

#include <QDateTime>
#include <QString>

namespace creds {

// Credential resolution:
//   1) env var (case-sensitive name)
//   2) CredentialVault for sensitive keys when supported
//   3) QSettings (default scope)
//   4) optional default value
//
// All values are checked after trimming. get() may return an empty string
// when defaultValue is empty.
class CredentialStore {
public:
    static QString get(const char* envName,
                       const QString& settingsKey,
                       const QString& defaultValue = QString(),
                       bool emitWarning = false);

    static void set(const QString& settingsKey, const QString& value);
    static void clear(const QString& settingsKey);
    static bool has(const char* envName, const QString& settingsKey);
    static QString maskedDisplay(const QString& value);
    static int migrateSensitiveKeysToVault();

    // ----- TTL helpers (Phase 5F) -----
    static void setExpiry(const QString& settingsKey, const QDateTime& expiresAtUtc);
    static QDateTime getExpiry(const QString& settingsKey);
    static void clearExpiry(const QString& settingsKey);
    static bool isExpired(const QString& settingsKey);
};

} // namespace creds
