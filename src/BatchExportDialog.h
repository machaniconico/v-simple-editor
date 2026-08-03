#pragma once
#include <QDialog>
#include "BatchExportQueue.h"

class QTableWidget;
class QPushButton;
class QLabel;
class Timeline;

class BatchExportDialog : public QDialog {
    Q_OBJECT
public:
    struct CurrentProjectContext {
        QString projectPath;
        Timeline *timeline = nullptr;
        int width = 0;
        int height = 0;
        double fps = 0.0;
        qint64 startUs = 0;
        qint64 endUs = 0;
    };

    explicit BatchExportDialog(QWidget *parent = nullptr);

    void setCurrentProjectContext(const CurrentProjectContext &context);

    // These builders are the same task-generation functions used by the
    // button handlers. They do not show UI or start rendering, so the E2E
    // selftest can exercise the real dialog path headlessly.
    static batchexport::ExportTask makeCurrentProjectTask(
        const CurrentProjectContext &context,
        const QString &outputPath,
        const batchexport::ExportSettings &settings);
    static batchexport::ExportTask makeFileProjectTask(
        const QString &projectPath,
        const QString &outputPath,
        const batchexport::ExportSettings &settings);

private slots:
    void onAddClicked();
    void onAddCurrentProjectClicked();
    void onRemoveClicked();
    void onStartClicked();
    void onPauseClicked();
    void onTaskStateChanged(const QString &id, batchexport::TaskState state);
    void onTaskProgress(const QString &id, int percent);

private:
    bool chooseExportSettings(batchexport::ExportSettings *settings);
    void appendTaskRow(const batchexport::ExportTask &task);
    batchexport::Queue *m_queue      = nullptr;
    QTableWidget       *m_table      = nullptr;
    QLabel             *m_emptyLabel = nullptr;
    QPushButton        *m_addBtn     = nullptr;
    QPushButton        *m_addCurrentBtn = nullptr;
    QPushButton        *m_removeBtn  = nullptr;
    QPushButton        *m_startBtn   = nullptr;
    QPushButton        *m_pauseBtn   = nullptr;
    CurrentProjectContext m_currentProject;

    int rowForId(const QString &id) const;
};
