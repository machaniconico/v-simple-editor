#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;
class QTcpSocket;

// ---------------------------------------------------------------------------
// namespace youtube::oauth — Sprint 17 US-YT-1
// YouTube Data API v3 用 OAuth 2.0 PKCE (RFC 7636) 認証層。
// 外部 OAuth ライブラリは使わず QtNetwork + QCryptographicHash で実装。
//
// 典型フロー:
//   AuthClient client(YoutubeOAuthConfig::defaultConfig());
//   QObject::connect(&client, &AuthClient::tokensReceived, ...);
//   QObject::connect(&client, &AuthClient::authError,      ...);
//   const QString url = client.launchAuthFlow();
//   QDesktopServices::openUrl(QUrl(url));
//   // ... callback で codeReceived → exchangeCodeForTokens が自動的に呼ばれる
//
// セキュリティ注意:
//   - clientSecret はデスクトップアプリ用 OAuth client (PKCE) で空でも可。
//   - clientId / clientSecret の取得経路: env (VEDITOR_YOUTUBE_CLIENT_ID/_SECRET)
//     → QSettings (youtube_oauth/client_id|_secret) → 空文字列 (creds::CredentialStore)
//   - refresh_token は QSettings に base64 で軽 obfuscate して保存する。
//     プラットフォーム secure storage は別 story (US-YT-2 以降) で扱う。
// ---------------------------------------------------------------------------

namespace youtube {
namespace oauth {

// OAuth クライアント設定。Google Cloud Console で発行した値を入れる。
struct YoutubeOAuthConfig {
    QString clientId;        // OAuth 2.0 Client ID
    QString clientSecret;    // PKCE のみなら空でも可 (Web client では必須)
    QString redirectUri;     // 既定: http://localhost:8080/callback
    QString scope;           // 既定: youtube.upload

    // mock server / staging 経由のテスト用に endpoint base を override する。
    // 空ならハードコードされた https://accounts.google.com と https://oauth2.googleapis.com を使う。
    // 設定例: "http://localhost:8081" → authorize は <base>/o/oauth2/v2/auth に、
    //   token は <base>/token に向く。
    QString baseUrl;

    // 既定値 (clientId / clientSecret は空、redirectUri/scope のみ埋まる)
    static YoutubeOAuthConfig defaultConfig();
};

// access / refresh トークンと有効期限。
struct Token {
    QString accessToken;
    QString refreshToken;
    QDateTime expiresAt;     // 絶対時刻 (UTC 推奨)
    QString tokenType;       // 通常 "Bearer"
    QString scope;

    bool isValid() const { return !accessToken.isEmpty(); }
    bool isExpired(int leewaySec = 60) const;
};

class AuthClient : public QObject {
    Q_OBJECT
public:
    explicit AuthClient(const YoutubeOAuthConfig& config, QObject* parent = nullptr);
    ~AuthClient() override;

    const YoutubeOAuthConfig& config() const { return m_config; }
    const Token& currentToken() const { return m_token; }

    // PKCE flow Step1: code_verifier / code_challenge を生成し、
    // localhost:8080 (使用中なら fallback random port) で QTcpServer を listen 開始する。
    // 返り値は呼び出し側が QDesktopServices::openUrl で開くべき
    // Google authorize URL (https://accounts.google.com/o/oauth2/v2/auth?...).
    // 既に flow が動いていればそれを停止して再起動する。
    QString launchAuthFlow();

    // PKCE flow Step2: callback で受け取った code を token endpoint と交換する。
    // 通常 codeReceived シグナル経由で内部から自動的に呼ばれるが、テスト用に public。
    void exchangeCodeForTokens(const QString& code);

    // expiresAt - now < leeway なら refresh_token で更新を試みる。
    // refresh_token が無ければ何もしない (false 返す)。非同期完了は tokensReceived。
    bool refreshIfExpired(int leewaySec = 60);

    // QSettings 'youtube_oauth/refresh_token' に base64 encode 保存 / 読み出し。
    // 軽 obfuscate のみ (本番は KeyChain 等を別 story で予定)。
    void saveToken() const;
    bool loadToken();

    // テスト/再接続用: 動作中の callback server を即停止する。
    void cancelAuthFlow();

    // 現在採用された callback port (random fallback 時の確認用)。0 = 未起動。
    quint16 callbackPort() const { return m_callbackPort; }

    // PKCE 内部値 accessor (テスト/デバッグ用途)
    QString codeVerifier() const { return m_codeVerifier; }
    QString codeChallenge() const { return m_codeChallenge; }

signals:
    // callback で code を受け取った瞬間 (exchange 前)。
    void codeReceived(const QString& code);
    // token endpoint との交換 (or refresh) 成功時。
    void tokensReceived(const youtube::oauth::Token& token);
    // authorize / token endpoint / callback parse などで失敗した時。
    // 例外は投げず、必ず本シグナルでエラー通知する。
    void authError(const QString& reason);

private slots:
    void onIncomingConnection();
    void onClientReadyRead();
    void onTokenReplyFinished();

private:
    // PKCE 補助
    static QString generateCodeVerifier();
    static QString deriveCodeChallenge(const QString& verifier);
    static QString base64UrlEncode(const QByteArray& bytes);

    // callback server 管理
    bool startCallbackServer();
    void stopCallbackServer();
    void handleCallbackRequest(QTcpSocket* socket, const QByteArray& requestLine);
    void writeCallbackResponse(QTcpSocket* socket, const QString& body, int statusCode = 200);

    // token endpoint POST
    void postToTokenEndpoint(const QByteArray& formBody, bool isRefresh);
    void parseTokenJson(const QByteArray& json, bool isRefresh);

    YoutubeOAuthConfig m_config;
    Token m_token;

    QString m_codeVerifier;
    QString m_codeChallenge;

    QPointer<QTcpServer> m_callbackServer;
    quint16 m_callbackPort = 0;

    QPointer<QNetworkAccessManager> m_nam;
    QPointer<QNetworkReply> m_pendingReply;
    bool m_pendingIsRefresh = false;
};

} // namespace oauth
} // namespace youtube
