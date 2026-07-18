#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QSettings>
#include <QTimer>
#include <QByteArray>
#include <QDebug>

#include "CredentialStore.h"
#include "VimeoOAuth.h"
#include "YoutubeOAuth.h"
#include "selftests/oauth_mock_server.h"

namespace {

constexpr auto kOrgName = "VSimpleEditorSelftests";
constexpr auto kAppName = "oauth-refresh-e2e";
constexpr auto kYoutubeRefreshTokenKey = "youtube_oauth/refresh_token";
constexpr auto kYoutubeAccessTokenKey = "youtube_oauth/access_token";
constexpr auto kVimeoAccessTokenKey = "vimeo_oauth/access_token";
constexpr auto kVimeoRefreshTokenKey = "vimeo_oauth/refresh_token";

class SettingsContextGuard {
public:
    SettingsContextGuard()
        : m_orgName(QCoreApplication::organizationName())
        , m_appName(QCoreApplication::applicationName()) {
        QCoreApplication::setOrganizationName(QString::fromLatin1(kOrgName));
        QCoreApplication::setApplicationName(QString::fromLatin1(kAppName));
    }

    ~SettingsContextGuard() {
        QCoreApplication::setOrganizationName(m_orgName);
        QCoreApplication::setApplicationName(m_appName);
    }

private:
    QString m_orgName;
    QString m_appName;
};

void clearYoutubeState() {
    QSettings settings;
    settings.remove(QString::fromLatin1(kYoutubeRefreshTokenKey));
    creds::CredentialStore::clearExpiry(QString::fromLatin1(kYoutubeAccessTokenKey));
    settings.sync();
}

void clearVimeoState() {
    QSettings settings;
    settings.remove(QString::fromLatin1(kVimeoAccessTokenKey));
    settings.remove(QString::fromLatin1(kVimeoRefreshTokenKey));
    creds::CredentialStore::clearExpiry(QString::fromLatin1(kVimeoAccessTokenKey));
    settings.sync();
}

void clearOAuthState() {
    clearYoutubeState();
    clearVimeoState();
}

void seedYoutubeState(const QString& refreshToken, const QDateTime& expiresAt) {
    clearYoutubeState();

    QSettings settings;
    if (!refreshToken.isEmpty()) {
        const QByteArray encoded = refreshToken.toUtf8().toBase64();
        settings.setValue(QString::fromLatin1(kYoutubeRefreshTokenKey),
                          QString::fromLatin1(encoded));
    }
    creds::CredentialStore::setExpiry(QString::fromLatin1(kYoutubeAccessTokenKey), expiresAt);
    settings.sync();
}

bool waitForAsync(bool* done, QEventLoop* loop, int timeoutMs = 5000) {
    if (*done) {
        return true;
    }
    QTimer::singleShot(timeoutMs, loop, &QEventLoop::quit);
    loop->exec();
    return *done;
}

bool tokenRequestContains(const QList<QByteArray>& requests,
                          int startIndex,
                          const QByteArray& needle) {
    for (int i = startIndex; i < requests.size(); ++i) {
        if (requests.at(i).contains(needle)) {
            return true;
        }
    }
    return false;
}

QString describeBool(bool value) {
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

} // namespace

int runOAuthRefreshE2eSelftest() {
    qInfo().noquote() << "[oauth-refresh-e2e] selftest start";

    int passed = 0;
    int failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[oauth-refresh-e2e] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[oauth-refresh-e2e] FAIL" << name << ":" << msg;
    };

    SettingsContextGuard contextGuard;
    clearOAuthState();

    selftests::OAuthMockServer server;
    if (!server.start(8081) && !server.start(0)) {
        fail("server-startup", QStringLiteral("OAuthMockServer::start() failed"));
        clearOAuthState();
        qInfo().noquote().nospace()
            << "[oauth-refresh-e2e] selftest end, passed=" << passed
            << " failed=" << failed;
        return 1;
    }

    const QString baseUrl = server.baseUrl();
    if (baseUrl.isEmpty()) {
        fail("server-startup", QStringLiteral("OAuthMockServer::baseUrl() is empty"));
        server.stop();
        clearOAuthState();
        qInfo().noquote().nospace()
            << "[oauth-refresh-e2e] selftest end, passed=" << passed
            << " failed=" << failed;
        return 1;
    }

    youtube::oauth::YoutubeOAuthConfig ytBase = youtube::oauth::YoutubeOAuthConfig::defaultConfig();
    ytBase.clientId = QStringLiteral("test-yt-client");
    ytBase.clientSecret.clear();
    ytBase.baseUrl = baseUrl;

    // ===== YT 5 gates =====
    clearYoutubeState();
    {
        youtube::oauth::AuthClient yt(ytBase);
        const bool started = yt.refreshIfExpired();
        if (!started) {
            pass("G1 YT empty refreshToken -> false");
        } else {
            fail("G1 YT empty refreshToken -> false",
                 QStringLiteral("refreshIfExpired returned true"));
        }
    }

    seedYoutubeState(QStringLiteral("yt-future-refresh"),
                     QDateTime::currentDateTimeUtc().addSecs(3600));
    {
        youtube::oauth::AuthClient yt(ytBase);
        const int requestCountBefore = server.tokenRequestCount();
        const bool loaded = yt.loadToken();
        const bool started = loaded ? yt.refreshIfExpired() : false;
        const bool ok = loaded
            && !started
            && server.tokenRequestCount() == requestCountBefore;
        if (ok) {
            pass("G2 YT future expiresAt -> false");
        } else {
            fail("G2 YT future expiresAt -> false",
                 QStringLiteral("loaded=%1 started=%2 requestsBefore=%3 requestsAfter=%4")
                     .arg(describeBool(loaded))
                     .arg(describeBool(started))
                     .arg(requestCountBefore)
                     .arg(server.tokenRequestCount()));
        }
    }

    seedYoutubeState(QStringLiteral("yt-expired-refresh"),
                     QDateTime::currentDateTimeUtc().addSecs(-3600));
    {
        youtube::oauth::AuthClient yt(ytBase);
        QEventLoop loop;
        bool done = false;
        QString errorReason;
        youtube::oauth::Token refreshedToken;
        QObject::connect(&yt, &youtube::oauth::AuthClient::tokensReceived,
                         [&](const youtube::oauth::Token& token) {
                             refreshedToken = token;
                             done = true;
                             loop.quit();
                         });
        QObject::connect(&yt, &youtube::oauth::AuthClient::authError,
                         [&](const QString& reason) {
                             errorReason = reason;
                             done = true;
                             loop.quit();
                         });

        const int requestCountBefore = server.tokenRequestCount();
        const int requestLogBefore = server.tokenRequests().size();
        const bool loaded = yt.loadToken();
        const bool completed = waitForAsync(&done, &loop);
        const bool ok = loaded
            && completed
            && errorReason.isEmpty()
            && !refreshedToken.accessToken.isEmpty()
            && server.tokenRequestCount() > requestCountBefore;
        if (ok) {
            pass("G3 YT expired loadToken auto-refresh");
        } else {
            fail("G3 YT expired loadToken auto-refresh",
                 QStringLiteral("loaded=%1 completed=%2 error=%3 accessToken=%4 requestDelta=%5")
                     .arg(describeBool(loaded))
                     .arg(describeBool(completed))
                     .arg(errorReason.isEmpty() ? QStringLiteral("<none>") : errorReason)
                     .arg(refreshedToken.accessToken.isEmpty() ? QStringLiteral("<empty>")
                                                               : refreshedToken.accessToken)
                     .arg(server.tokenRequestCount() - requestCountBefore));
        }

        const bool sawRefreshGrant =
            tokenRequestContains(server.tokenRequests(),
                                 requestLogBefore,
                                 QByteArray("grant_type=refresh_token"));
        if (sawRefreshGrant) {
            pass("G4 YT /token POST contains grant_type=refresh_token");
        } else {
            fail("G4 YT /token POST contains grant_type=refresh_token",
                 QStringLiteral("requestLogDelta=%1")
                     .arg(server.tokenRequests().size() - requestLogBefore));
        }

        const QDateTime persistedExpiry =
            creds::CredentialStore::getExpiry(QString::fromLatin1(kYoutubeAccessTokenKey));
        const bool persistedFuture =
            !refreshedToken.accessToken.isEmpty()
            && persistedExpiry.isValid()
            && persistedExpiry > QDateTime::currentDateTimeUtc();
        if (persistedFuture) {
            pass("G5 YT access_token refreshed + expiresAt persisted");
        } else {
            fail("G5 YT access_token refreshed + expiresAt persisted",
                 QStringLiteral("accessToken=%1 persistedExpiry=%2")
                     .arg(refreshedToken.accessToken.isEmpty() ? QStringLiteral("<empty>")
                                                               : refreshedToken.accessToken)
                     .arg(persistedExpiry.toString(Qt::ISODate)));
        }
    }
    clearYoutubeState();

    vimeo::oauth::VimeoOAuthConfig vimeoBase = vimeo::oauth::VimeoOAuthConfig::defaultConfig();
    vimeoBase.clientId = QStringLiteral("test-vimeo-client");
    vimeoBase.clientSecret = QStringLiteral("test-vimeo-secret");
    vimeoBase.baseUrl = baseUrl;
    vimeoBase.accessToken.clear();
    vimeoBase.refreshToken.clear();
    vimeoBase.expiresAt = QDateTime();

    // ===== Vimeo 5 gates =====
    clearVimeoState();
    {
        vimeo::oauth::AuthClient vimeo(vimeoBase);
        const int requestCountBefore = server.tokenRequestCount();
        const bool started = vimeo.refreshIfExpired();
        const bool ok = !started && server.tokenRequestCount() == requestCountBefore;
        if (ok) {
            pass("G6 Vimeo empty refreshToken -> false");
        } else {
            fail("G6 Vimeo empty refreshToken -> false",
                 QStringLiteral("started=%1 requestsBefore=%2 requestsAfter=%3")
                     .arg(describeBool(started))
                     .arg(requestCountBefore)
                     .arg(server.tokenRequestCount()));
        }
    }

    {
        vimeo::oauth::VimeoOAuthConfig cfg = vimeoBase;
        cfg.refreshToken = QStringLiteral("vimeo-future-refresh");
        cfg.expiresAt = QDateTime::currentDateTimeUtc().addSecs(3600);

        const int requestCountBefore = server.tokenRequestCount();
        vimeo::oauth::AuthClient vimeo(cfg);
        const bool started = vimeo.refreshIfExpired();
        const bool ok = !started && server.tokenRequestCount() == requestCountBefore;
        if (ok) {
            pass("G7 Vimeo future expiresAt -> false");
        } else {
            fail("G7 Vimeo future expiresAt -> false",
                 QStringLiteral("started=%1 requestsBefore=%2 requestsAfter=%3")
                     .arg(describeBool(started))
                     .arg(requestCountBefore)
                     .arg(server.tokenRequestCount()));
        }
    }

    clearVimeoState();
    {
        vimeo::oauth::VimeoOAuthConfig cfg = vimeoBase;
        cfg.refreshToken = QStringLiteral("vimeo-expired-refresh");
        cfg.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-3600);

        const int requestCountBefore = server.tokenRequestCount();
        const int requestLogBefore = server.tokenRequests().size();
        vimeo::oauth::AuthClient vimeo(cfg);

        QEventLoop loop;
        bool done = false;
        QString accessToken;
        QString refreshToken;
        QString errorReason;
        QObject::connect(&vimeo, &vimeo::oauth::AuthClient::tokenReceived,
                         [&](const QString& token) {
                             accessToken = token;
                             done = true;
                             loop.quit();
                         });
        QObject::connect(&vimeo, &vimeo::oauth::AuthClient::tokensUpdated,
                         [&](const QString& token, const QString& newRefreshToken) {
                             accessToken = token;
                             refreshToken = newRefreshToken;
                             done = true;
                             loop.quit();
                         });
        QObject::connect(&vimeo, &vimeo::oauth::AuthClient::authError,
                         [&](const QString& reason) {
                             errorReason = reason;
                             done = true;
                             loop.quit();
                         });

        const bool completed = waitForAsync(&done, &loop);
        const bool ok = completed
            && errorReason.isEmpty()
            && !accessToken.isEmpty()
            && server.tokenRequestCount() > requestCountBefore;
        if (ok) {
            pass("G8 Vimeo expired ctor auto-refresh");
        } else {
            fail("G8 Vimeo expired ctor auto-refresh",
                 QStringLiteral("completed=%1 error=%2 accessToken=%3 requestDelta=%4")
                     .arg(describeBool(completed))
                     .arg(errorReason.isEmpty() ? QStringLiteral("<none>") : errorReason)
                     .arg(accessToken.isEmpty() ? QStringLiteral("<empty>") : accessToken)
                     .arg(server.tokenRequestCount() - requestCountBefore));
        }

        const bool sawRefreshGrant =
            tokenRequestContains(server.tokenRequests(),
                                 requestLogBefore,
                                 QByteArray("grant_type=refresh_token"));
        if (sawRefreshGrant) {
            pass("G9 Vimeo /token POST contains grant_type=refresh_token");
        } else {
            fail("G9 Vimeo /token POST contains grant_type=refresh_token",
                 QStringLiteral("requestLogDelta=%1")
                     .arg(server.tokenRequests().size() - requestLogBefore));
        }

        QSettings settings;
        const QString persistedAccessToken =
            settings.value(QString::fromLatin1(kVimeoAccessTokenKey)).toString();
        const QDateTime persistedExpiry =
            creds::CredentialStore::getExpiry(QString::fromLatin1(kVimeoAccessTokenKey));
        const bool persistedOk =
            !accessToken.isEmpty()
            && !persistedAccessToken.isEmpty()
            && persistedAccessToken == accessToken
            && persistedExpiry.isValid()
            && persistedExpiry > QDateTime::currentDateTimeUtc();
        if (persistedOk) {
            pass("G10 Vimeo access_token persisted + expiresAt future");
        } else {
            fail("G10 Vimeo access_token persisted + expiresAt future",
                 QStringLiteral("accessToken=%1 persistedAccessToken=%2 refreshToken=%3 persistedExpiry=%4")
                     .arg(accessToken.isEmpty() ? QStringLiteral("<empty>") : accessToken)
                     .arg(persistedAccessToken.isEmpty() ? QStringLiteral("<empty>")
                                                         : persistedAccessToken)
                     .arg(refreshToken.isEmpty() ? QStringLiteral("<empty>") : refreshToken)
                     .arg(persistedExpiry.toString(Qt::ISODate)));
        }
    }

    server.stop();
    clearOAuthState();

    qInfo().noquote().nospace()
        << "[oauth-refresh-e2e] selftest end, passed=" << passed
        << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
