#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMetaObject>
#include <QString>

#include "McpToolRegistry.h"

class MainWindow;
class Timeline;
class RenderQueue;

namespace mcp {

class McpToolRegistry;

// MainWindow / Timeline を MCP ツールとして公開する。
// registry より短命であってはならない (ハンドラが this を捕捉する)。
class McpEditorTools {
public:
    McpEditorTools(MainWindow* window, McpToolRegistry* registry);
    ~McpEditorTools();

    // 読み取り専用ツールを登録する。
    void registerReadTools();
    // 変更系ツールを登録する。
    void registerWriteTools();

    // 変更系ツールの排他。ネストしたイベントループ (run_command が開いたダイアログ等) の
    // 最中に届いた別の変更ツール呼び出しを isError で拒否する。同一 GUI スレッド上の
    // 再帰を防ぐフラグであり、スレッド排他ではない。
    bool beginExclusiveWrite(const QString& toolName, QString* err);
    void endExclusiveWrite();
    QString activeWriteTool() const { return m_activeWriteTool; }

private:
    Timeline* timeline() const;
    ToolHandler guardedWrite(const QString& toolName, ToolHandler inner);
    void syncSelectionAfterEdit();
    RenderQueue* ensureRenderQueue(QString* err);
    void observeRenderQueue(RenderQueue* queue);
    QJsonObject exportStatus(const QString& jobId, QString* err);

    struct ExportJobObservation {
        QString status;
        int progress = 0;
        QString error;
    };

    MainWindow* m_window = nullptr;
    McpToolRegistry* m_registry = nullptr;
    RenderQueue* m_observedRenderQueue = nullptr;
    QMetaObject::Connection m_exportProgressConnection;
    QMetaObject::Connection m_exportCompletedConnection;
    QString m_activeWriteTool;
    // MCP の export_video ジョブは McpEditorTools の存続中、最終スナップショットを
    // 削除しない。通常は MainWindow と同じくプロセス終了まで jobId を照会できる。
    QHash<QString, ExportJobObservation> m_exportJobObservations;
};

} // namespace mcp
