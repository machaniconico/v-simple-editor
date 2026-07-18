#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

namespace creds {

class CredentialAuditLog {
public:
    enum class EventType {
        AccessTokenLoaded,
        RefreshFired,
        RefreshSucceeded,
        RefreshFailed,
        CredentialRotated,
        CredentialExpired,
        AccessTokenSet
    };

    struct Entry {
        QDateTime timestamp;
        EventType type = EventType::AccessTokenLoaded;
        QString platform;
        QString settingsKey;
        QString message;
    };

    // event を audit log に append する。thread-safe (QMutex protected)。format: JSONL (1 行 = 1 JSON)。
    // platform: "YouTube" / "Vimeo" 等
    // settingsKey: 例 "youtube_oauth/access_token" — 出力時には先頭 8 文字 + "***" に mask される
    // message: 任意の補助情報 (空可)
    static void logEvent(EventType type,
                         const QString& platform,
                         const QString& settingsKey,
                         const QString& message = QString());

    // QStandardPaths::AppConfigLocation 配下の audit.log path
    static QString logFilePath();

    // audit.log が 1MB 超なら audit.log.1 に rename、新 audit.log を作成。return 1 if rotated else 0
    static int rotateIfNeeded();

    // event type → text label ("REFRESH_FIRED" 等)。selftest と内部用
    static QString eventTypeLabel(EventType type);

    // label → event type. unknown は AccessTokenLoaded fallback
    static EventType eventTypeFromLabel(const QString& label);

    // settingsKey の mask ("youtube_oauth/access_token" → "youtube_***")
    static QString maskSettingsKey(const QString& settingsKey);

    // audit.log を JSONL parse して Entry 配列を return。parse 不能行は skip。thread-safe。
    static QList<Entry> readEntries();

    // 現在 UTC - days より古い entry を audit.log から削除し、purged 件数を return。
    // atomic: audit.log.tmp に書いて rename で置換。parse 不能行は残す。thread-safe。
    static int purgeOlderThanDays(int days);
};

} // creds
