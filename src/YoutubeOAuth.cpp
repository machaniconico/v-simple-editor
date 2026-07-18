#include "YoutubeOAuth.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QCryptographicHash>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QRandomGenerator64>
#include <QHostAddress>
#include <QByteArray>
#include <QtGlobal>

#include "CredentialAuditLog.h"
#include "CredentialStore.h"

namespace youtube {
namespace oauth {

// ─────────────────────────────────────────────
// YoutubeOAuthConfig
// ─────────────────────────────────────────────
YoutubeOAuthConfig YoutubeOAuthConfig::defaultConfig() {
    YoutubeOAuthConfig c;
    c.clientId     = creds::CredentialStore::get(
        "VEDITOR_YOUTUBE_CLIENT_ID",
        QStringLiteral("youtube_oauth/client_id"));
    c.clientSecret = creds::CredentialStore::get(
        "VEDITOR_YOUTUBE_CLIENT_SECRET",
        QStringLiteral("youtube_oauth/client_secret"));
    c.redirectUri  = QStringLiteral("http://localhost:8080/callback");
    c.scope        = QStringLiteral("https://www.googleapis.com/auth/youtube.upload");
    c.baseUrl      = QString();
    return c;
}

// ─────────────────────────────────────────────
// Token
// ─────────────────────────────────────────────
bool Token::isExpired(int leewaySec) const {
    if (!expiresAt.isValid()) return true;
    return QDateTime::currentDateTimeUtc().addSecs(leewaySec) >= expiresAt;
}

// ─────────────────────────────────────────────
// AuthClient
// ─────────────────────────────────────────────
AuthClient::AuthClient(const YoutubeOAuthConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
{
    m_nam = new QNetworkAccessManager(this);
}

AuthClient::~AuthClient() {
    stopCallbackServer();
}

// ── PKCE helpers ────────────────────────────────────────────────────────────
QString AuthClient::base64UrlEncode(const QByteArray& bytes) {
    QByteArray b64 = bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QString::fromLatin1(b64);
}

QString AuthClient::generateCodeVerifier() {
    // RFC 7636: 43〜128 文字の URL-safe (unreserved char) 文字列。
    // 32 byte の random を base64url すると 43 char になる (>= 43 を満たす)。
    QByteArray raw(32, '\0');
    auto* rng = QRandomGenerator64::system();
    for (int i = 0; i < raw.size(); i += 8) {
        const quint64 v = rng->generate64();
        for (int b = 0; b < 8 && (i + b) < raw.size(); ++b) {
            raw[i + b] = static_cast<char>((v >> (b * 8)) & 0xFF);
        }
    }
    return base64UrlEncode(raw);
}

QString AuthClient::deriveCodeChallenge(const QString& verifier) {
    const QByteArray hash = QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256);
    return base64UrlEncode(hash);
}

// ── callback server ─────────────────────────────────────────────────────────
bool AuthClient::startCallbackServer() {
    stopCallbackServer();
    m_callbackServer = new QTcpServer(this);
    connect(m_callbackServer.data(), &QTcpServer::newConnection,
            this, &AuthClient::onIncomingConnection);

    // 既定 port を試して busy なら random fallback (0 = OS 任せ)。
    if (!m_callbackServer->listen(QHostAddress::LocalHost, 8080)) {
        if (!m_callbackServer->listen(QHostAddress::LocalHost, 0)) {
            const QString err = m_callbackServer->errorString();
            stopCallbackServer();
            emit authError(QStringLiteral("Failed to start callback server: %1").arg(err));
            return false;
        }
    }
    m_callbackPort = m_callbackServer->serverPort();
    return true;
}

void AuthClient::stopCallbackServer() {
    if (m_callbackServer) {
        m_callbackServer->close();
        m_callbackServer->deleteLater();
        m_callbackServer = nullptr;
    }
    m_callbackPort = 0;
}

void AuthClient::cancelAuthFlow() {
    stopCallbackServer();
}

void AuthClient::onIncomingConnection() {
    if (!m_callbackServer) return;
    while (QTcpSocket* sock = m_callbackServer->nextPendingConnection()) {
        connect(sock, &QTcpSocket::readyRead, this, &AuthClient::onClientReadyRead);
        connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
    }
}

void AuthClient::onClientReadyRead() {
    auto* sock = qobject_cast<QTcpSocket*>(sender());
    if (!sock) return;
    // HTTP/1.1 GET の最初の 1 行だけパースすれば充分 (code は URL クエリ)。
    if (!sock->canReadLine()) return;
    const QByteArray requestLine = sock->readLine().trimmed();
    // 残りの header を読み捨て (簡易)。
    while (sock->canReadLine()) {
        const QByteArray l = sock->readLine();
        if (l.trimmed().isEmpty()) break;
    }
    handleCallbackRequest(sock, requestLine);
}

void AuthClient::handleCallbackRequest(QTcpSocket* socket, const QByteArray& requestLine) {
    // requestLine 例: "GET /callback?code=4/abc&scope=... HTTP/1.1"
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 2 || parts[0].toUpper() != "GET") {
        writeCallbackResponse(socket, QStringLiteral("Bad Request"), 400);
        emit authError(QStringLiteral("Malformed callback HTTP request"));
        return;
    }
    const QByteArray pathAndQuery = parts[1];
    // QUrl で path?query を解析するため先頭にダミー scheme を付ける。
    const QUrl u(QStringLiteral("http://localhost") + QString::fromLatin1(pathAndQuery));
    const QUrlQuery q(u);
    const QString errorParam = q.queryItemValue(QStringLiteral("error"));
    const QString code = q.queryItemValue(QStringLiteral("code"));

    if (!errorParam.isEmpty()) {
        writeCallbackResponse(socket,
                              QStringLiteral("Authorization denied: %1").arg(errorParam),
                              400);
        emit authError(QStringLiteral("Authorization denied: %1").arg(errorParam));
        stopCallbackServer();
        return;
    }
    if (code.isEmpty()) {
        writeCallbackResponse(socket, QStringLiteral("Missing code parameter"), 400);
        emit authError(QStringLiteral("Callback missing 'code' parameter"));
        stopCallbackServer();
        return;
    }

    writeCallbackResponse(socket,
                          QStringLiteral("Authorization complete, you may close this tab."),
                          200);

    // 1 度だけ受ければよい。
    stopCallbackServer();

    emit codeReceived(code);
    // 自動で token endpoint と交換。
    exchangeCodeForTokens(code);
}

void AuthClient::writeCallbackResponse(QTcpSocket* socket, const QString& body, int statusCode) {
    if (!socket) return;
    const QByteArray bodyBytes = body.toUtf8();
    const QString reason = (statusCode == 200) ? QStringLiteral("OK")
                         : (statusCode == 400) ? QStringLiteral("Bad Request")
                                               : QStringLiteral("Error");
    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + reason.toUtf8() + "\r\n";
    response += "Content-Type: text/plain; charset=utf-8\r\n";
    response += "Content-Length: " + QByteArray::number(bodyBytes.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += bodyBytes;
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

// ── public flow ─────────────────────────────────────────────────────────────
QString AuthClient::launchAuthFlow() {
    if (!startCallbackServer()) {
        return QString();
    }

    m_codeVerifier  = generateCodeVerifier();
    m_codeChallenge = deriveCodeChallenge(m_codeVerifier);

    // redirectUri は config 値だが、random fallback 時は port を差し替える必要がある。
    QUrl redirect(m_config.redirectUri);
    if (m_callbackPort != 0 && redirect.port() != m_callbackPort) {
        redirect.setPort(m_callbackPort);
    }

    const QString authBase = m_config.baseUrl.isEmpty()
        ? QStringLiteral("https://accounts.google.com")
        : m_config.baseUrl;
    QUrl authUrl(authBase + QStringLiteral("/o/oauth2/v2/auth"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("client_id"),             m_config.clientId);
    q.addQueryItem(QStringLiteral("redirect_uri"),          redirect.toString());
    q.addQueryItem(QStringLiteral("response_type"),         QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("scope"),                 m_config.scope);
    q.addQueryItem(QStringLiteral("code_challenge"),        m_codeChallenge);
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("access_type"),           QStringLiteral("offline"));
    q.addQueryItem(QStringLiteral("prompt"),                QStringLiteral("consent"));
    authUrl.setQuery(q);
    return authUrl.toString();
}

void AuthClient::exchangeCodeForTokens(const QString& code) {
    if (code.isEmpty()) {
        emit authError(QStringLiteral("Empty authorization code"));
        return;
    }
    QUrl redirect(m_config.redirectUri);
    if (m_callbackPort != 0 && redirect.port() != m_callbackPort) {
        redirect.setPort(m_callbackPort);
    }

    QUrlQuery body;
    body.addQueryItem(QStringLiteral("grant_type"),    QStringLiteral("authorization_code"));
    body.addQueryItem(QStringLiteral("code"),          code);
    body.addQueryItem(QStringLiteral("code_verifier"), m_codeVerifier);
    body.addQueryItem(QStringLiteral("redirect_uri"),  redirect.toString());
    body.addQueryItem(QStringLiteral("client_id"),     m_config.clientId);
    if (!m_config.clientSecret.isEmpty()) {
        body.addQueryItem(QStringLiteral("client_secret"), m_config.clientSecret);
    }
    postToTokenEndpoint(body.toString(QUrl::FullyEncoded).toUtf8(), /*isRefresh=*/false);
}

bool AuthClient::refreshIfExpired(int leewaySec) {
    if (m_token.refreshToken.isEmpty()) {
        return false;
    }
    if (!m_token.isExpired(leewaySec)) {
        return false;
    }
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("grant_type"),    QStringLiteral("refresh_token"));
    body.addQueryItem(QStringLiteral("refresh_token"), m_token.refreshToken);
    body.addQueryItem(QStringLiteral("client_id"),     m_config.clientId);
    if (!m_config.clientSecret.isEmpty()) {
        body.addQueryItem(QStringLiteral("client_secret"), m_config.clientSecret);
    }
    postToTokenEndpoint(body.toString(QUrl::FullyEncoded).toUtf8(), /*isRefresh=*/true);
    return true;
}

// ── token endpoint plumbing ─────────────────────────────────────────────────
void AuthClient::postToTokenEndpoint(const QByteArray& formBody, bool isRefresh) {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    const QString tokenBase = m_config.baseUrl.isEmpty()
        ? QStringLiteral("https://oauth2.googleapis.com")
        : m_config.baseUrl;
    QNetworkRequest req(QUrl(tokenBase + QStringLiteral("/token")));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    req.setRawHeader("Accept", "application/json");

    m_pendingIsRefresh = isRefresh;
    m_pendingReply = m_nam->post(req, formBody);
    connect(m_pendingReply.data(), &QNetworkReply::finished,
            this, &AuthClient::onTokenReplyFinished);
}

void AuthClient::onTokenReplyFinished() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        emit authError(QStringLiteral("Token reply has no sender"));
        return;
    }
    m_pendingReply.clear();
    const QNetworkReply::NetworkError err = reply->error();
    const QString errStr = reply->errorString();
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    if (err != QNetworkReply::NoError) {
        creds::CredentialAuditLog::logEvent(
            creds::CredentialAuditLog::EventType::RefreshFailed,
            QStringLiteral("YouTube"),
            QStringLiteral("youtube_oauth/access_token"),
            errStr);
        emit authError(QStringLiteral("Token endpoint error: %1 (%2)")
                       .arg(errStr, QString::fromUtf8(body)));
        return;
    }
    parseTokenJson(body, m_pendingIsRefresh);
}

void AuthClient::parseTokenJson(const QByteArray& json, bool isRefresh) {
    const QString previousAccess = m_token.accessToken;
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        emit authError(QStringLiteral("Token JSON parse error: %1").arg(perr.errorString()));
        return;
    }
    const QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("error"))) {
        const QString e  = obj.value(QStringLiteral("error")).toString();
        const QString ed = obj.value(QStringLiteral("error_description")).toString();
        emit authError(QStringLiteral("OAuth error: %1 (%2)").arg(e, ed));
        return;
    }

    Token t = m_token;  // 既存値を保持しつつ上書き (refresh で refresh_token が省略される場合あり)
    t.accessToken = obj.value(QStringLiteral("access_token")).toString();
    const QString rt = obj.value(QStringLiteral("refresh_token")).toString();
    if (!rt.isEmpty()) {
        t.refreshToken = rt;
    }
    t.tokenType = obj.value(QStringLiteral("token_type")).toString();
    t.scope     = obj.value(QStringLiteral("scope")).toString();
    const int expiresIn = obj.value(QStringLiteral("expires_in")).toInt(3600);
    t.expiresAt = QDateTime::currentDateTimeUtc().addSecs(expiresIn);

    if (t.accessToken.isEmpty()) {
        emit authError(QStringLiteral("Token JSON missing access_token"));
        return;
    }

    m_token = t;
    saveToken();  // Phase 5F: refresh_token + expiresAt を persist
    if (isRefresh && previousAccess != m_token.accessToken) {
        creds::CredentialAuditLog::logEvent(
            creds::CredentialAuditLog::EventType::CredentialRotated,
            QStringLiteral("YouTube"),
            QStringLiteral("youtube_oauth/access_token"),
            QStringLiteral("access_token rotated via refresh"));
    }
    creds::CredentialAuditLog::logEvent(
        isRefresh ? creds::CredentialAuditLog::EventType::RefreshSucceeded
                  : creds::CredentialAuditLog::EventType::AccessTokenSet,
        QStringLiteral("YouTube"),
        QStringLiteral("youtube_oauth/access_token"),
        QStringLiteral("expiresAt=%1").arg(m_token.expiresAt.toString(Qt::ISODate)));
    emit tokensReceived(m_token);
}

// ── persistence ─────────────────────────────────────────────────────────────
void AuthClient::saveToken() const {
    QSettings s;
    if (m_token.refreshToken.isEmpty()) {
        s.remove(QStringLiteral("youtube_oauth/refresh_token"));
    } else {
        const QByteArray b64 = m_token.refreshToken.toUtf8().toBase64();
        s.setValue(QStringLiteral("youtube_oauth/refresh_token"), QString::fromLatin1(b64));
    }
    // Phase 5F: access_token expiry を persist (access_token 本体は揮発)
    creds::CredentialStore::setExpiry(QStringLiteral("youtube_oauth/access_token"), m_token.expiresAt);
}

bool AuthClient::loadToken() {
    QSettings s;
    const QString stored = s.value(QStringLiteral("youtube_oauth/refresh_token")).toString();
    if (stored.isEmpty()) return false;
    const QByteArray decoded = QByteArray::fromBase64(stored.toLatin1());
    if (decoded.isEmpty()) return false;
    m_token.refreshToken = QString::fromUtf8(decoded);
    m_token.accessToken.clear();
    // Phase 5F: 永続化された expiry を読み戻す
    m_token.expiresAt = creds::CredentialStore::getExpiry(QStringLiteral("youtube_oauth/access_token"));
    // Phase 5G: auto-refresh if expired
    const bool fired = refreshIfExpired();
    if (fired) {
        qDebug() << "[YT OAuth] auto-refresh fired on loadToken";
        creds::CredentialAuditLog::logEvent(
            creds::CredentialAuditLog::EventType::RefreshFired,
            QStringLiteral("YouTube"),
            QStringLiteral("youtube_oauth/access_token"),
            QStringLiteral("auto-refresh on loadToken"));
    }
    creds::CredentialAuditLog::logEvent(
        creds::CredentialAuditLog::EventType::AccessTokenLoaded,
        QStringLiteral("YouTube"),
        QStringLiteral("youtube_oauth/access_token"),
        QStringLiteral("refresh_token restored from QSettings"));
    const int purged = creds::CredentialAuditLog::purgeOlderThanDays(30);
    if (purged > 0) {
        qDebug() << "[YT OAuth] audit log purged" << purged << "old entries (>30 days)";
    }
    return true;
}

} // namespace oauth
} // namespace youtube
