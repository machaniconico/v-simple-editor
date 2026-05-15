#include "VimeoOAuth.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrlQuery>
#include <QtGlobal>

namespace vimeo {
namespace oauth {

namespace {

constexpr const char* kAuthorizeUrl = "https://api.vimeo.com/oauth/authorize";
constexpr const char* kClientCredentialsUrl = "https://api.vimeo.com/oauth/authorize/client";
constexpr const char* kAccessTokenUrl = "https://api.vimeo.com/oauth/access_token";

QString configValue(const char* envName,
                    const QString& settingsKey,
                    const QString& dummyValue) {
    QString value = qEnvironmentVariable(envName).trimmed();
    if (value.isEmpty()) {
        QSettings settings;
        value = settings.value(settingsKey).toString().trimmed();
    }
    if (value.isEmpty()) {
        qWarning().noquote()
            << QStringLiteral("Vimeo OAuth missing %1 / %2, using dummy value.")
                   .arg(QString::fromLatin1(envName), settingsKey);
        value = dummyValue;
    }
    return value;
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
    config.clientId = configValue("VEDITOR_VIMEO_CLIENT_ID",
                                  QStringLiteral("vimeo_oauth/client_id"),
                                  QStringLiteral("dummy-vimeo-client-id"));
    config.clientSecret = configValue("VEDITOR_VIMEO_CLIENT_SECRET",
                                      QStringLiteral("vimeo_oauth/client_secret"),
                                      QStringLiteral("dummy-vimeo-client-secret"));
    config.scope = QStringLiteral("private public video_files");
    config.redirectUri = QStringLiteral("http://localhost:8080/vimeo/callback");

    QSettings settings;
    config.accessToken = settings.value(QStringLiteral("vimeo_oauth/access_token")).toString();
    config.refreshToken = settings.value(QStringLiteral("vimeo_oauth/refresh_token")).toString();
    return config;
}

AuthClient::AuthClient(const VimeoOAuthConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_nam(new QNetworkAccessManager(this)) {}

AuthClient::~AuthClient() = default;

QUrl AuthClient::authorizationUrl(const QString& redirectUri, const QString& state) const {
    QUrl url(QString::fromLatin1(kAuthorizeUrl));
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
    dispatchTokenRequest(QUrl(QString::fromLatin1(kClientCredentialsUrl)),
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
    dispatchTokenRequest(QUrl(QString::fromLatin1(kAccessTokenUrl)),
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
    dispatchTokenRequest(QUrl(QString::fromLatin1(kAccessTokenUrl)),
                         form.toString(QUrl::FullyEncoded).toUtf8(),
                         PendingGrant::RefreshToken);
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

    m_config.accessToken = accessToken;
    if (!refreshToken.isEmpty() || grant != PendingGrant::RefreshToken) {
        m_config.refreshToken = refreshToken;
    }
    if (!scope.isEmpty()) {
        m_config.scope = scope;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("vimeo_oauth/access_token"), m_config.accessToken);
    if (!m_config.refreshToken.isEmpty()) {
        settings.setValue(QStringLiteral("vimeo_oauth/refresh_token"), m_config.refreshToken);
    }

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
