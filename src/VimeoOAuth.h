#pragma once

#include <QByteArray>
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
    QString accessToken;
    QString refreshToken;
    QString redirectUri;

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
