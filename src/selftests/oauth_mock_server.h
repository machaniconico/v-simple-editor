#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>
#include <QByteArray>

class QTcpServer;
class QTcpSocket;

namespace selftests {

// localhost mock HTTP server for OAuth + Upload pipeline testing.
// 起動 → server.baseUrl() で http://127.0.0.1:<port> 取得 →
// YoutubeOAuthConfig/VimeoOAuthConfig.baseUrl と Client::setApiBaseUrl に代入。
class OAuthMockServer : public QObject {
    Q_OBJECT
public:
    explicit OAuthMockServer(QObject* parent = nullptr);
    ~OAuthMockServer() override;

    // ポート確保。port=0 で OS が割り当て。失敗時 false。
    bool start(quint16 preferredPort = 8081);
    void stop();

    // 起動後の baseUrl (http://127.0.0.1:<port>)。未起動なら空。
    QString baseUrl() const;
    quint16 boundPort() const { return m_port; }

    // 状態 inspector (selftest 用)
    int tokenRequestCount() const { return m_tokenRequestCount; }
    int initiateRequestCount() const { return m_initiateRequestCount; }
    int chunkPutCount() const { return m_chunkPutCount; }
    int tusPatchCount() const { return m_tusPatchCount; }
    int instagramContainerCount() const { return m_instagramContainerCount; }
    int instagramStatusCount() const { return m_instagramStatusCount; }
    int instagramPublishCount() const { return m_instagramPublishCount; }
    int xInitCount() const { return m_xInitCount; }
    int xAppendCount() const { return m_xAppendCount; }
    int xFinalizeCount() const { return m_xFinalizeCount; }
    int xStatusCount() const { return m_xStatusCount; }
    int xTweetCount() const { return m_xTweetCount; }
    qint64 lastReceivedBytes() const { return m_lastReceivedBytes; }
    const QList<QByteArray>& tokenRequests() const { return m_tokenRequests; }

    // session 経由で受信した動画 byte 合計 (sessionUri をキーに集計)
    qint64 receivedBytesForSession(const QString& sessionUri) const;

private slots:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();

private:
    struct Request {
        QByteArray method;
        QByteArray path;
        QHash<QByteArray, QByteArray> headers;
        QByteArray body;
        QByteArray buffer;
        bool headersParsed = false;
        int contentLength = -1;
    };

    void handleRequest(QTcpSocket* socket, const Request& req);
    void writeResponse(QTcpSocket* socket, int status,
                       const QByteArray& body,
                       const QHash<QByteArray, QByteArray>& extraHeaders = {});

    // route handlers
    void routeToken(QTcpSocket* socket, const Request& req);             // POST /token, /oauth/access_token
    void routeAuthorize(QTcpSocket* socket, const Request& req);         // GET /o/oauth2/v2/auth, /oauth/authorize
    void routeYoutubeInitiate(QTcpSocket* socket, const Request& req);   // POST /upload/youtube/v3/videos...
    void routeYoutubeChunk(QTcpSocket* socket, const Request& req);      // PUT /mock-session/yt/<id>
    void routeVimeoCreate(QTcpSocket* socket, const Request& req);       // POST /me/videos
    void routeVimeoTus(QTcpSocket* socket, const Request& req);          // PATCH /mock-session/vimeo/<id>
    // Instagram Graph API mock routes
    void routeInstagramContainer(QTcpSocket* socket, const Request& req);   // POST /v19.0/<igUserId>/media
    void routeInstagramStatus(QTcpSocket* socket, const Request& req);      // GET /v19.0/<creationId>?fields=status_code
    void routeInstagramPublish(QTcpSocket* socket, const Request& req);     // POST /v19.0/<igUserId>/media_publish
    // X Media Upload v1.1 mock routes
    void routeXInit(QTcpSocket* socket, const Request& req);                // POST /1.1/media/upload.json?command=INIT
    void routeXAppend(QTcpSocket* socket, const Request& req);              // POST /1.1/media/upload.json
    void routeXFinalize(QTcpSocket* socket, const Request& req);            // POST /1.1/media/upload.json?command=FINALIZE
    void routeXStatus(QTcpSocket* socket, const Request& req);              // GET /1.1/media/upload.json?command=STATUS&media_id=xxx
    // X Tweet API v2 mock route
    void routeXCreateTweet(QTcpSocket* socket, const Request& req);         // POST /2/tweets

    QTcpServer* m_server = nullptr;
    quint16 m_port = 0;
    QHash<QTcpSocket*, Request> m_pending;
    QHash<QString, qint64> m_sessionBytes;  // sessionUri → total received bytes
    QList<QByteArray> m_tokenRequests;      // raw /token request line + body
    int m_tokenRequestCount = 0;
    int m_initiateRequestCount = 0;
    int m_chunkPutCount = 0;
    int m_tusPatchCount = 0;
    qint64 m_lastReceivedBytes = 0;
    int m_instagramContainerCount = 0;
    int m_instagramStatusCount = 0;
    int m_instagramPublishCount = 0;
    int m_xInitCount = 0;
    int m_xAppendCount = 0;
    int m_xFinalizeCount = 0;
    int m_xStatusCount = 0;
    int m_xTweetCount = 0;
    int m_sessionCounter = 0;  // sessionUri ID 採番
};

} // namespace selftests
