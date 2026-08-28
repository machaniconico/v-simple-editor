#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace mcp {

class McpToolRegistry;

struct ServerInfo {
    QString name = QStringLiteral("v-simple-editor");
    QString version = QStringLiteral("0.1.0");
};

class McpProtocol {
public:
    // registry は呼び出し側が所有する。null 不可。
    McpProtocol(McpToolRegistry* registry, const ServerInfo& info);

    // JSON-RPC メッセージ 1 件分のボディを処理し、返すべきレスポンスボディを返す。
    // 通知 (id を持たないメッセージ) の場合は空 QByteArray を返す
    // = トランスポート側は 202 Accepted / 何も書かない、で応じる。
    // tools/call の content は従来の text に加えて MCP の image なども保持する。
    // ツールの payload は従来どおり structuredContent に格納する。
    QByteArray handleMessage(const QByteArray& body) const;

    bool initialized() const { return m_initialized; }

    static QByteArray makeErrorResponse(const QJsonValue& id, int code, const QString& message);
    static QByteArray makeResultResponse(const QJsonValue& id, const QJsonObject& result);

    // MCP の最新 protocolVersion。initialize で対応版を要求された場合は
    // その版を返し、欠落または非対応の場合はこの最新値を返す。
    static const char* protocolVersion();
    static QStringList supportedProtocolVersions();
    static bool isSupportedProtocolVersion(const QString& version);

private:
    McpToolRegistry* m_registry = nullptr;
    ServerInfo m_info;
    mutable bool m_initialized = false;
};

} // namespace mcp
