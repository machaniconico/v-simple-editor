#include "McpProtocol.h"

#include "McpToolRegistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

namespace mcp {

namespace {

const QJsonValue nullId(QJsonValue::Null);

QByteArray responseForRequest(bool notification, const QByteArray& response)
{
    return notification ? QByteArray() : response;
}

QJsonObject toolCallResult(const QString& text, bool isError)
{
    QJsonObject contentItem;
    contentItem.insert(QStringLiteral("type"), QStringLiteral("text"));
    contentItem.insert(QStringLiteral("text"), text);

    QJsonArray content;
    content.append(contentItem);

    QJsonObject result;
    result.insert(QStringLiteral("content"), content);
    result.insert(QStringLiteral("isError"), isError);
    return result;
}

} // namespace

McpProtocol::McpProtocol(McpToolRegistry* registry, const ServerInfo& info)
    : m_registry(registry)
    , m_info(info)
{
}

QByteArray McpProtocol::handleMessage(const QByteArray& body) const
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return makeErrorResponse(nullId, -32700, QStringLiteral("parse error"));

    if (document.isArray())
        return makeErrorResponse(nullId, -32600,
                                 QStringLiteral("batch requests are not supported"));

    if (!document.isObject())
        return makeErrorResponse(nullId, -32600, QStringLiteral("invalid request"));

    const QJsonObject request = document.object();
    const bool notification = !request.contains(QStringLiteral("id"));
    const QJsonValue id = notification ? nullId : request.value(QStringLiteral("id"));
    const QJsonValue methodValue = request.value(QStringLiteral("method"));
    if (!methodValue.isString())
        return responseForRequest(notification,
                                  makeErrorResponse(id, -32600,
                                                    QStringLiteral("invalid request")));

    const QString method = methodValue.toString();
    if (method == QStringLiteral("initialize")) {
        m_initialized = true;

        QJsonObject toolsCapability;
        toolsCapability.insert(QStringLiteral("listChanged"), false);
        QJsonObject capabilities;
        capabilities.insert(QStringLiteral("tools"), toolsCapability);

        QJsonObject serverInfo;
        serverInfo.insert(QStringLiteral("name"), m_info.name);
        serverInfo.insert(QStringLiteral("version"), m_info.version);

        QJsonObject result;
        result.insert(QStringLiteral("protocolVersion"),
                      QString::fromLatin1(protocolVersion()));
        result.insert(QStringLiteral("capabilities"), capabilities);
        result.insert(QStringLiteral("serverInfo"), serverInfo);
        return responseForRequest(notification, makeResultResponse(id, result));
    }

    if (method == QStringLiteral("notifications/initialized"))
        return QByteArray();

    if (method == QStringLiteral("ping"))
        return responseForRequest(notification,
                                  makeResultResponse(id, QJsonObject()));

    if (!m_registry) {
        // コンストラクタの契約違反。クラッシュさせずに internal error を返す。
        return responseForRequest(notification,
                                  makeErrorResponse(id, -32603,
                                                    QStringLiteral("internal error: no tool registry")));
    }

    if (method == QStringLiteral("tools/list")) {
        QJsonObject result;
        result.insert(QStringLiteral("tools"), m_registry->listToolsJson());
        return responseForRequest(notification, makeResultResponse(id, result));
    }

    if (method == QStringLiteral("tools/call")) {
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();
        const QJsonValue nameValue = params.value(QStringLiteral("name"));
        if (!nameValue.isString()) {
            return responseForRequest(
                notification,
                makeErrorResponse(id, -32602,
                                  QStringLiteral("invalid params: name is required")));
        }

        QJsonObject arguments;
        if (params.contains(QStringLiteral("arguments"))) {
            const QJsonValue argumentsValue = params.value(QStringLiteral("arguments"));
            if (!argumentsValue.isObject()) {
                return responseForRequest(
                    notification,
                    makeErrorResponse(id, -32602,
                                      QStringLiteral("invalid params: arguments must be an object")));
            }
            arguments = argumentsValue.toObject();
        }

        bool found = false;
        QString error;
        const QJsonObject toolResult = m_registry->callTool(
            nameValue.toString(), arguments, &found, &error);
        if (!found) {
            return responseForRequest(
                notification,
                makeErrorResponse(id, -32602,
                                  QStringLiteral("unknown tool: %1").arg(nameValue.toString())));
        }

        if (!error.isEmpty()) {
            return responseForRequest(notification,
                                      makeResultResponse(id, toolCallResult(error, true)));
        }

        const QString serializedResult = QString::fromUtf8(
            QJsonDocument(toolResult).toJson(QJsonDocument::Compact));
        QJsonObject result = toolCallResult(serializedResult, false);
        result.insert(QStringLiteral("structuredContent"), toolResult);
        return responseForRequest(notification, makeResultResponse(id, result));
    }

    return responseForRequest(
        notification,
        makeErrorResponse(id, -32601,
                          QStringLiteral("method not found: %1").arg(method)));
}

QByteArray McpProtocol::makeErrorResponse(const QJsonValue& id, int code,
                                          const QString& message)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);

    QJsonObject response;
    response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("error"), error);
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

QByteArray McpProtocol::makeResultResponse(const QJsonValue& id, const QJsonObject& result)
{
    QJsonObject response;
    response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("result"), result);
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

const char* McpProtocol::protocolVersion()
{
    // Streamable HTTP (POST 1 発で JSON レスポンス) を前提にしているので、
    // それが仕様に入った 2025-03-26 以降の版を名乗る。2024-11-05 を名乗ると
    // HTTP+SSE の旧トランスポートを期待するクライアントと噛み合わない。
    return "2025-06-18";
}

} // namespace mcp
