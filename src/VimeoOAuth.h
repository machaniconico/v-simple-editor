#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace vimeo {
namespace oauth {

struct VimeoOAuthConfig {
    QString clientId;
    QString clientSecret;
    QString scope;
    // mock server / staging 経由のテスト用に endpoint base を override する。
    // 空ならハードコードされた https://api.vimeo.com を使う。
    QString baseUrl;
    QString accessToken;
    QString refreshToken;
    QString redirectUri;
    QDateTime expiresAt;  // Phase 5F: access_token expiry UTC、無効なら expiry 未知

    static VimeoOAuthConfig defaultConfig();
};

class AuthClient : public QObject {
    Q_OBJECT
public:
    explicit AuthClient(const VimeoOAuthConfig& config = VimeoOAuthConfig::defaultConfig(),
                        QObject* parent = nullptr);
    ~AuthClient() override;

    const VimeoOAuthConfig& config() const { return m_config; }
    QString accessToken() const { return m_config.accessToken; }
    QString refreshToken() const { return m_config.refreshToken; }
    bool hasAccessToken() const { return !m_config.accessToken.isEmpty(); }

    QUrl authorizationUrl(const QString& redirectUri,
                          const QString& state = QString()) const;

    void requestAccessToken();
    void requestClientCredentialsToken();
    void exchangeAuthorizationCode(const QString& authorizationCode,
                                   const QString& redirectUri = QString());
    void refreshAccessToken();
    // expiresAt - now < leeway なら refresh_token で更新を試みる。
    // refreshToken 未設定なら何もしない (false 返す)。非同期完了は tokensUpdated signal。
    bool refreshIfExpired(int leewaySec = 60);

    void setAccessToken(const QString& accessToken);
    void setRefreshToken(const QString& refreshToken);

signals:
    void tokenReceived(const QString& accessToken);
    void tokensUpdated(const QString& accessToken, const QString& refreshToken);
    void authError(const QString& reason);

private slots:
    void onReplyFinished();

private:
    enum class PendingGrant {
        None,
        ClientCredentials,
        AuthorizationCode,
        RefreshToken
    };

    void dispatchTokenRequest(const QUrl& url,
                              const QByteArray& formBody,
                              PendingGrant grant);
    void applyTokenPayload(const QByteArray& payload, PendingGrant grant);
    static QByteArray basicAuthorizationHeader(const QString& clientId,
                                               const QString& clientSecret);

    VimeoOAuthConfig m_config;
    QPointer<QNetworkAccessManager> m_nam;
    QPointer<QNetworkReply> m_pendingReply;
    PendingGrant m_pendingGrant = PendingGrant::None;
};

} // namespace oauth
} // namespace vimeo
