#include "VimeoOAuth.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrlQuery>
#include <QtGlobal>

#include "CredentialAuditLog.h"
#include "CredentialStore.h"

namespace vimeo {
namespace oauth {

namespace {

QString resolvedBase(const QString& configBase) {
    return configBase.isEmpty() ? QStringLiteral("https://api.vimeo.com") : configBase;
}

QString authorizeUrl(const QString& configBase) {
    return resolvedBase(configBase) + QStringLiteral("/oauth/authorize");
}

QString clientCredentialsUrl(const QString& configBase) {
    return resolvedBase(configBase) + QStringLiteral("/oauth/authorize/client");
}

QString accessTokenUrl(const QString& configBase) {
    return resolvedBase(configBase) + QStringLiteral("/oauth/access_token");
}

QString jsonErrorString(const QByteArray& payload) {
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        return QString::fromUtf8(payload).trimmed();
    }

    const QJsonObject obj = doc.object();
    const QString developerMessage = obj.value(QStringLiteral("developer_message")).toString();
    if (!developerMessage.isEmpty()) {
        return developerMessage;
    }

    const QString error = obj.value(QStringLiteral("error")).toString();
    const QString description = obj.value(QStringLiteral("error_description")).toString();
    if (!error.isEmpty() && !description.isEmpty()) {
        return QStringLiteral("%1: %2").arg(error, description);
    }
    if (!error.isEmpty()) {
        return error;
    }
    if (!description.isEmpty()) {
        return description;
    }

    return QString::fromUtf8(payload).trimmed();
}

} // namespace

VimeoOAuthConfig VimeoOAuthConfig::defaultConfig() {
    VimeoOAuthConfig config;
    config.clientId = creds::CredentialStore::get(
        "VEDITOR_VIMEO_CLIENT_ID",
        QStringLiteral("vimeo_oauth/client_id"),
        QString(),
        true);
    config.clientSecret = creds::CredentialStore::get(
        "VEDITOR_VIMEO_CLIENT_SECRET",
        QStringLiteral("vimeo_oauth/client_secret"),
        QString(),
        true);
    config.scope = QStringLiteral("private public video_files");
    config.baseUrl = QString();
    config.redirectUri = QStringLiteral("http://localhost:8080/vimeo/callback");

    QSettings settings;
    config.accessToken = settings.value(QStringLiteral("vimeo_oauth/access_token")).toString();
    config.refreshToken = settings.value(QStringLiteral("vimeo_oauth/refresh_token")).toString();
    config.expiresAt = creds::CredentialStore::getExpiry(QStringLiteral("vimeo_oauth/access_token"));
    creds::CredentialAuditLog::logEvent(
        creds::CredentialAuditLog::EventType::AccessTokenLoaded,
        QStringLiteral("Vimeo"),
        QStringLiteral("vimeo_oauth/access_token"),
        QStringLiteral("config loaded from QSettings"));
    const int purged = creds::CredentialAuditLog::purgeOlderThanDays(30);
    if (purged > 0) {
        qDebug() << "[Vimeo OAuth] audit log purged" << purged << "old entries (>30 days)";
    }
    return config;
}

AuthClient::AuthClient(const VimeoOAuthConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_nam(new QNetworkAccessManager(this)) {
    // Phase 5G: auto-refresh on construction if loaded config is expired
    if (refreshIfExpired()) {
        qDebug() << "[Vimeo OAuth] auto-refresh fired on ctor";
        creds::CredentialAuditLog::logEvent(
            creds::CredentialAuditLog::EventType::RefreshFired,
            QStringLiteral("Vimeo"),
            QStringLiteral("vimeo_oauth/access_token"),
            QStringLiteral("auto-refresh on ctor"));
    }
}

AuthClient::~AuthClient() = default;

QUrl AuthClient::authorizationUrl(const QString& redirectUri, const QString& state) const {
    QUrl url(authorizeUrl(m_config.baseUrl));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), m_config.clientId);
    query.addQueryItem(QStringLiteral("redirect_uri"),
                       redirectUri.isEmpty() ? m_config.redirectUri : redirectUri);
    query.addQueryItem(QStringLiteral("scope"), m_config.scope);
    if (!state.isEmpty()) {
        query.addQueryItem(QStringLiteral("state"), state);
    }
    url.setQuery(query);
    return url;
}

void AuthClient::requestAccessToken() {
    qWarning() << "Vimeo OAuth grant not specified; falling back to client_credentials.";
    requestClientCredentialsToken();
}

void AuthClient::requestClientCredentialsToken() {
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("client_credentials"));
    if (!m_config.scope.isEmpty()) {
        form.addQueryItem(QStringLiteral("scope"), m_config.scope);
    }
    dispatchTokenRequest(QUrl(clientCredentialsUrl(m_config.baseUrl)),
                         form.toString(QUrl::FullyEncoded).toUtf8(),
                         PendingGrant::ClientCredentials);
}

void AuthClient::exchangeAuthorizationCode(const QString& authorizationCode,
                                           const QString& redirectUri) {
    if (authorizationCode.trimmed().isEmpty()) {
        emit authError(QStringLiteral("authorization code is empty"));
        return;
    }

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    form.addQueryItem(QStringLiteral("code"), authorizationCode.trimmed());
    form.addQueryItem(QStringLiteral("redirect_uri"),
                      redirectUri.isEmpty() ? m_config.redirectUri : redirectUri);
    dispatchTokenRequest(QUrl(accessTokenUrl(m_config.baseUrl)),
                         form.toString(QUrl::FullyEncoded).toUtf8(),
                         PendingGrant::AuthorizationCode);
}

void AuthClient::refreshAccessToken() {
    if (m_config.refreshToken.trimmed().isEmpty()) {
        emit authError(QStringLiteral("refresh token is empty"));
        return;
    }

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    form.addQueryItem(QStringLiteral("refresh_token"), m_config.refreshToken.trimmed());
    dispatchTokenRequest(QUrl(accessTokenUrl(m_config.baseUrl)),
                         form.toString(QUrl::FullyEncoded).toUtf8(),
                         PendingGrant::RefreshToken);
}

bool AuthClient::refreshIfExpired(int leewaySec) {
    if (m_config.refreshToken.trimmed().isEmpty()) {
        return false;  // refresh_token 無し → refresh 不可
    }
    // expiresAt が有効 かつ now + leeway < expiresAt → まだ十分有効
    if (m_config.expiresAt.isValid()) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (now.addSecs(leewaySec) < m_config.expiresAt) {
            return false;  // expiry まで余裕あり、refresh 不要
        }
    }
    // expired or expiry 未知 → refresh fire (既存 method を呼ぶ)
    refreshAccessToken();
    return true;
}

void AuthClient::setAccessToken(const QString& accessToken) {
    m_config.accessToken = accessToken.trimmed();
}

void AuthClient::setRefreshToken(const QString& refreshToken) {
    m_config.refreshToken = refreshToken.trimmed();
}

void AuthClient::dispatchTokenRequest(const QUrl& url,
                                      const QByteArray& formBody,
                                      PendingGrant grant) {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    if (m_pendingReply) {
        m_pendingReply->deleteLater();
        m_pendingReply.clear();
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Accept", "application/vnd.vimeo.*+json;version=3.4");
    request.setRawHeader("Authorization",
                         basicAuthorizationHeader(m_config.clientId, m_config.clientSecret));

    m_pendingGrant = grant;
    m_pendingReply = m_nam->post(request, formBody);
    if (!m_pendingReply) {
        m_pendingGrant = PendingGrant::None;
        emit authError(QStringLiteral("failed to dispatch Vimeo OAuth request"));
        return;
    }
    connect(m_pendingReply, &QNetworkReply::finished,
            this, &AuthClient::onReplyFinished);
}

void AuthClient::onReplyFinished() {
    if (!m_pendingReply) {
        return;
    }

    QNetworkReply* reply = m_pendingReply;
    const PendingGrant grant = m_pendingGrant;
    m_pendingReply.clear();
    m_pendingGrant = PendingGrant::None;

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkMessage = reply->errorString();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError || status >= 400) {
        creds::CredentialAuditLog::logEvent(
            creds::CredentialAuditLog::EventType::RefreshFailed,
            QStringLiteral("Vimeo"),
            QStringLiteral("vimeo_oauth/access_token"),
            networkMessage);
        QString detail = jsonErrorString(payload);
        if (detail.isEmpty()) {
            detail = networkMessage;
        }
        emit authError(QStringLiteral("Vimeo OAuth request failed (HTTP %1): %2")
                           .arg(status)
                           .arg(detail));
        return;
    }

    applyTokenPayload(payload, grant);
}

void AuthClient::applyTokenPayload(const QByteArray& payload, PendingGrant grant) {
    const QString previousAccess = m_config.accessToken;
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        emit authError(QStringLiteral("Vimeo OAuth response was not JSON"));
        return;
    }

    const QJsonObject obj = doc.object();
    const QString accessToken = obj.value(QStringLiteral("access_token")).toString().trimmed();
    if (accessToken.isEmpty()) {
        emit authError(QStringLiteral("Vimeo OAuth response missing access_token"));
        return;
    }

    const QString refreshToken = obj.value(QStringLiteral("refresh_token")).toString().trimmed();
    const QString scope = obj.value(QStringLiteral("scope")).toString().trimmed();
    const int expiresIn = obj.value(QStringLiteral("expires_in")).toInt(0);

    m_config.accessToken = accessToken;
    if (!refreshToken.isEmpty() || grant != PendingGrant::RefreshToken) {
        m_config.refreshToken = refreshToken;
    }
    if (!scope.isEmpty()) {
        m_config.scope = scope;
    }
    if (expiresIn > 0) {
        m_config.expiresAt = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
    } else {
        m_config.expiresAt = QDateTime();
    }
    const bool isRefresh = (grant == PendingGrant::RefreshToken);
    if (isRefresh && previousAccess != m_config.accessToken) {
        creds::CredentialAuditLog::logEvent(
            creds::CredentialAuditLog::EventType::CredentialRotated,
            QStringLiteral("Vimeo"),
            QStringLiteral("vimeo_oauth/access_token"),
            QStringLiteral("access_token rotated via refresh"));
    }
    creds::CredentialAuditLog::logEvent(
        isRefresh ? creds::CredentialAuditLog::EventType::RefreshSucceeded
                  : creds::CredentialAuditLog::EventType::AccessTokenSet,
        QStringLiteral("Vimeo"),
        QStringLiteral("vimeo_oauth/access_token"),
        QStringLiteral("expiresAt=%1").arg(m_config.expiresAt.toString(Qt::ISODate)));

    QSettings settings;
    settings.setValue(QStringLiteral("vimeo_oauth/access_token"), m_config.accessToken);
    if (!m_config.refreshToken.isEmpty()) {
        settings.setValue(QStringLiteral("vimeo_oauth/refresh_token"), m_config.refreshToken);
    }
    creds::CredentialStore::setExpiry(QStringLiteral("vimeo_oauth/access_token"), m_config.expiresAt);

    emit tokenReceived(m_config.accessToken);
    emit tokensUpdated(m_config.accessToken, m_config.refreshToken);
}

QByteArray AuthClient::basicAuthorizationHeader(const QString& clientId,
                                                const QString& clientSecret) {
    const QByteArray credentials =
        (clientId + QStringLiteral(":") + clientSecret).toUtf8().toBase64();
    return QByteArray("Basic ") + credentials;
}

} // namespace oauth
} // namespace vimeo
