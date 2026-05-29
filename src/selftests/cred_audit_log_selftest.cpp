#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QFutureSynchronizer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QtConcurrent>
#include <QDebug>

#include "CredentialAuditLog.h"

int runCredAuditLogSelftest()
{
    qInfo().noquote() << "[cred-audit-log] selftest start";
    int passed = 0;
    int failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[cred-audit-log] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[cred-audit-log] FAIL" << name << ":" << msg;
    };

    using AuditLog = creds::CredentialAuditLog;
    using ET = AuditLog::EventType;

    const QString path = AuditLog::logFilePath();
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".1"));

    if (!path.isEmpty()) {
        pass("G1 logFilePath() returns non-empty path");
    } else {
        fail("G1 logFilePath() returns non-empty path", QStringLiteral("empty"));
    }

    AuditLog::logEvent(ET::AccessTokenLoaded,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("initial test"));
    QFileInfo fi(path);
    if (fi.exists() && fi.size() > 0) {
        pass("G2 logEvent creates audit.log with size > 0");
    } else {
        fail("G2 logEvent creates audit.log with size > 0",
             QStringLiteral("exists=%1 size=%2").arg(fi.exists()).arg(fi.size()));
    }

    AuditLog::logEvent(ET::RefreshFired,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("test"));
    AuditLog::logEvent(ET::RefreshSucceeded,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("test"));
    AuditLog::logEvent(ET::RefreshFailed,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("test"));
    AuditLog::logEvent(ET::CredentialRotated,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("test"));
    AuditLog::logEvent(ET::CredentialExpired,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("test"));
    AuditLog::logEvent(ET::AccessTokenSet,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("test"));

    QFile f(path);
    f.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    const QStringList expectedLabels = {
        QStringLiteral("ACCESS_TOKEN_LOADED"),
        QStringLiteral("REFRESH_FIRED"),
        QStringLiteral("REFRESH_SUCCEEDED"),
        QStringLiteral("REFRESH_FAILED"),
        QStringLiteral("CREDENTIAL_ROTATED"),
        QStringLiteral("CREDENTIAL_EXPIRED"),
        QStringLiteral("ACCESS_TOKEN_SET")
    };
    bool allLabelsFound = true;
    QString missingLabel;
    for (const QString& label : expectedLabels) {
        if (!content.contains(QStringLiteral("\"type\":\"") + label + QStringLiteral("\""))) {
            allLabelsFound = false;
            missingLabel = label;
            break;
        }
    }
    if (allLabelsFound) {
        pass("G3 all 7 EventType labels present in audit.log");
    } else {
        fail("G3 all 7 EventType labels present", QStringLiteral("missing: %1").arg(missingLabel));
    }

    if (content.contains(QStringLiteral("\"key\":\"youtube_***\""))
        && !content.contains(QStringLiteral("youtube_oauth/access_token"))) {
        pass("G4 settingsKey masked correctly");
    } else {
        fail("G4 settingsKey masked correctly", QStringLiteral("raw key leaked or mask missing"));
    }

    const QRegularExpression isoRe(
        QStringLiteral("\"ts\":\"\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z\""));
    bool tsOk = false;
    for (const QString& line : content.split('\n', Qt::SkipEmptyParts)) {
        if (isoRe.match(line).hasMatch()) {
            tsOk = true;
            break;
        }
    }
    if (tsOk) {
        pass("G5 ISO8601 UTC timestamp format present");
    } else {
        fail("G5 ISO8601 UTC timestamp format", QStringLiteral("no matching line"));
    }

    {
        QFile bloat(path);
        bloat.open(QIODevice::WriteOnly | QIODevice::Truncate);
        const QByteArray chunk(1024, 'x');
        for (int i = 0; i < 1100; ++i) {
            bloat.write(chunk);
        }
        bloat.close();
    }
    const int rotResult = AuditLog::rotateIfNeeded();
    const bool rotatedExists = QFileInfo::exists(path + QStringLiteral(".1"));
    if (rotResult == 1 && rotatedExists) {
        pass("G6 rotateIfNeeded() rotates 1MB+ file (returns 1, .1 exists)");
    } else {
        fail("G6 rotateIfNeeded() rotates 1MB+ file",
             QStringLiteral("rotResult=%1 rotatedExists=%2").arg(rotResult).arg(rotatedExists));
    }

    const qint64 sizeAfterRotate = QFileInfo(path).size();
    AuditLog::logEvent(ET::AccessTokenLoaded,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("post-rotation"));
    const qint64 sizeAfterAppend = QFileInfo(path).size();
    if (sizeAfterRotate == 0 && sizeAfterAppend > 0) {
        pass("G7 post-rotation audit.log empty then appendable");
    } else {
        fail("G7 post-rotation audit.log empty then appendable",
             QStringLiteral("sizeAfterRotate=%1 sizeAfterAppend=%2")
                 .arg(sizeAfterRotate)
                 .arg(sizeAfterAppend));
    }

    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".1"));
    {
        QFutureSynchronizer<void> sync;
        for (int i = 0; i < 10; ++i) {
            sync.addFuture(QtConcurrent::run([i]() {
                AuditLog::logEvent(ET::AccessTokenLoaded,
                                   QStringLiteral("YouTube"),
                                   QStringLiteral("youtube_oauth/access_token"),
                                   QStringLiteral("thread-%1").arg(i));
            }));
        }
        sync.waitForFinished();
    }
    QFile f2(path);
    f2.open(QIODevice::ReadOnly | QIODevice::Text);
    const QStringList lines = QString::fromUtf8(f2.readAll()).split('\n', Qt::SkipEmptyParts);
    f2.close();
    if (lines.size() == 10) {
        pass("G8 thread-safe - 10 parallel logEvent -> 10 lines");
    } else {
        fail("G8 thread-safe - 10 parallel logEvent -> 10 lines",
             QStringLiteral("got %1 lines").arg(lines.size()));
    }

    // G9: readEntries round-trip
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".1"));
    AuditLog::logEvent(ET::RefreshSucceeded,
                       QStringLiteral("Vimeo"),
                       QStringLiteral("vimeo_oauth/access_token"),
                       QStringLiteral("g9-test"));
    AuditLog::logEvent(ET::RefreshFailed,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("g9-test-err"));
    const QList<AuditLog::Entry> entries = AuditLog::readEntries();
    bool g9Ok = entries.size() == 2
             && entries[0].type == ET::RefreshSucceeded
             && entries[0].platform == QStringLiteral("Vimeo")
             && entries[0].message == QStringLiteral("g9-test")
             && entries[1].type == ET::RefreshFailed
             && entries[1].platform == QStringLiteral("YouTube")
             && entries[1].message == QStringLiteral("g9-test-err");
    if (g9Ok) {
        pass("G9 readEntries() round-trip - 2 entries with correct type/platform/message");
    } else {
        fail("G9 readEntries() round-trip",
             QStringLiteral("got %1 entries; entry[0].type=%2 platform=%3")
                 .arg(entries.size())
                 .arg(entries.size() > 0 ? AuditLog::eventTypeLabel(entries[0].type)
                                         : QStringLiteral("<none>"))
                 .arg(entries.size() > 0 ? entries[0].platform : QStringLiteral("<none>")));
    }

    // G10: purgeOlderThanDays - days=-1 で全 purge、days=365 で 0 purge
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".1"));
    AuditLog::logEvent(ET::AccessTokenLoaded,
                       QStringLiteral("YouTube"),
                       QStringLiteral("youtube_oauth/access_token"),
                       QStringLiteral("g10-test-a"));
    AuditLog::logEvent(ET::AccessTokenSet,
                       QStringLiteral("Vimeo"),
                       QStringLiteral("vimeo_oauth/access_token"),
                       QStringLiteral("g10-test-b"));
    const int purged365 = AuditLog::purgeOlderThanDays(365);
    const int sizeAfter365 = AuditLog::readEntries().size();
    const int purgedMinus1 = AuditLog::purgeOlderThanDays(-1);
    const int sizeAfterMinus1 = AuditLog::readEntries().size();
    if (purged365 == 0 && sizeAfter365 == 2 && purgedMinus1 == 2 && sizeAfterMinus1 == 0) {
        pass("G10 purgeOlderThanDays - days=365 keeps all, days=-1 purges all");
    } else {
        fail("G10 purgeOlderThanDays",
             QStringLiteral("purged365=%1 sizeAfter365=%2 purgedMinus1=%3 sizeAfterMinus1=%4")
                 .arg(purged365).arg(sizeAfter365).arg(purgedMinus1).arg(sizeAfterMinus1));
    }

    qInfo().noquote().nospace()
        << "[cred-audit-log] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
