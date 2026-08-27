#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMetaObject>
#include <QString>

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

private:
    Timeline* timeline() const;
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
    // MCP の export_video ジョブは McpEditorTools の存続中、最終スナップショットを
    // 削除しない。通常は MainWindow と同じくプロセス終了まで jobId を照会できる。
    QHash<QString, ExportJobObservation> m_exportJobObservations;
};

} // namespace mcp
