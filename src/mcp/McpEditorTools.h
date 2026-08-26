#pragma once

#include <QJsonObject>
#include <QString>

class MainWindow;
class Timeline;

namespace mcp {

class McpToolRegistry;

// MainWindow / Timeline を MCP ツールとして公開する。
// registry より短命であってはならない (ハンドラが this を捕捉する)。
class McpEditorTools {
public:
    McpEditorTools(MainWindow* window, McpToolRegistry* registry);

    // 読み取り専用ツールを登録する。
    void registerReadTools();
    // 変更系ツールを登録する (US-004 で実装)。
    void registerWriteTools();

private:
    Timeline* timeline() const;

    MainWindow* m_window = nullptr;
    McpToolRegistry* m_registry = nullptr;
};

} // namespace mcp
