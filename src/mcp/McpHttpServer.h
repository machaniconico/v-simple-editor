#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include "McpProtocol.h"

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace mcp {

class McpToolRegistry;

// エディタ内蔵の MCP サーバ。127.0.0.1 にのみバインドし、Bearer トークンで認証する。
// Qt の GUI スレッドのイベントループ上で動くので、ツールハンドラは GUI オブジェクトに
// 直接触れてよい (マーシャリング不要)。その代わりハンドラは短時間で返す契約。
class McpHttpServer : public QObject {
    Q_OBJECT
public:
    explicit McpHttpServer(McpToolRegistry* registry, QObject* parent = nullptr);
    ~McpHttpServer() override;

    // preferredPort から最大 20 ポート試して空きを探す。成功で true。
    // トークンは start() のたびに再生成する。
    bool start(quint16 preferredPort = 8765);
    void stop();

    // 保存済みトークンを破棄して次回 start() で作り直させる。
    // VEDITOR_MCP_TOKEN が設定されている間は環境変数が優先されるため何も起きない。
    void regenerateToken();

    bool isRunning() const;
    quint16 port() const { return m_port; }
    QString token() const { return m_token; }
    static int maxConnections();
    // http://127.0.0.1:<port>/mcp
    QString endpointUrl() const;

signals:
    void started(quint16 port);
    void stopped();
    // ログ用。tools/call の name と成否。UI のステータス表示に使う。
    void toolCalled(const QString& toolName, bool ok);

private slots:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void onIdleTimeout();

private:
    struct Request {
        QByteArray method;
        QByteArray path;
        QHash<QByteArray, QByteArray> headers;
        QByteArray body;
        QByteArray buffer;
        bool headersParsed = false;
        int contentLength = -1;
        qsizetype headerScanPosition = 0;
        qint64 lastReceivedMs = 0;
    };

    void handleRequest(QTcpSocket* socket, const Request& request);
    void writeResponse(QTcpSocket* socket, int status, const QByteArray& body,
                       const QHash<QByteArray, QByteArray>& extraHeaders = {},
                       bool closeConnection = false);
    bool authorized(const Request& request) const;
    static bool constantTimeEquals(const QByteArray& lhs, const QByteArray& rhs);
    static QByteArray generateToken();
    static QString resolveToken();

    QTcpServer* m_server = nullptr;
    QTimer* m_idleTimer = nullptr;
    McpToolRegistry* m_registry = nullptr;
    McpProtocol m_protocol;
    quint16 m_port = 0;
    QString m_token;
    QHash<QTcpSocket*, Request> m_pending;
    QSet<QTcpSocket*> m_processingSockets;
};

} // namespace mcp
