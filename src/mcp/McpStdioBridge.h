#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QString>

// Codex CLI などの stdio 専用 MCP ホストと、常駐エディタの MCP HTTP
// サーバの間を中継する GUI 非依存ブリッジ。
class McpStdioBridge
{
public:
    // QCoreApplication を生成し、stdio ループを実行する。
    static int run(quint16 port, const QString& token);

    // JSON を検証し、JSON-RPC の 1 行フレーミング用に compact 化する。
    // 不正な JSON の場合は空 QByteArray を返す。
    static QByteArray compactJson(const QByteArray& json);

    // JSON-RPC リクエスト行から id を取り出す。不正な行、非オブジェクト、
    // id 欠落時は JSON の null 値を返す。hadId は id キーの存在を返す。
    static QJsonValue extractRequestId(const QByteArray& line,
                                       bool *hadId = nullptr);

    // VEDITOR_MCP_TIMEOUT_MS (1000ms 以上) があればその値、なければ既定値。
    static int requestTimeoutMs();

    // stdio transport 層の internal error を compact な JSON で生成する。
    static QByteArray makeTransportError(const QJsonValue& id,
                                         const QString& message);
};
