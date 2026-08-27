#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

namespace mcp {

// ツールハンドラ。成功なら結果 JSON を返す。失敗なら *err にメッセージを入れて
// 空オブジェクトを返す (呼び出し側が MCP の isError:true result に変換する)。
using ToolHandler = std::function<QJsonObject(const QJsonObject& args, QString* err)>;

// text 以外の MCP コンテンツを返すツール用ハンドラ。payload は
// structuredContent に入り、content には MCP の content 配列へ追加する項目を入れる。
// 既存ツールは ToolHandler のまま登録できるため、従来の応答形状は変わらない。
using ToolContentHandler = std::function<QJsonObject(const QJsonObject& args,
                                                     QString* err,
                                                     QJsonArray* content)>;

struct ToolDescriptor {
    QString name;
    QString description;
    QJsonObject inputSchema;   // JSON Schema (type:"object", properties, required)
    ToolHandler handler;
    ToolContentHandler contentHandler;
    QJsonObject outputSchema;
};

class McpToolRegistry {
public:
    // 同名を登録した場合は後勝ちで置き換える。
    void registerTool(const ToolDescriptor& tool);
    void clear();
    bool contains(const QString& name) const;
    QVector<QString> toolNames() const;

    // tools/list の "tools" 配列 (name/description/inputSchema/outputSchema、
    // handler は出さない)。
    QJsonArray listToolsJson() const;

    // ツールを実行する。name が未登録なら *found=false。
    // ハンドラが例外を投げた場合も捕捉して *err に載せる (プロセスを落とさない)。
    QJsonObject callTool(const QString& name, const QJsonObject& args,
                         bool* found, QString* err,
                         QJsonArray* content = nullptr) const;

private:
    QVector<ToolDescriptor> m_tools;
};

} // namespace mcp
