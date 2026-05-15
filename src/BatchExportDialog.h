#pragma once
#include <QDialog>
#include "BatchExportQueue.h"

class QTableWidget;
class QPushButton;

class BatchExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit BatchExportDialog(QWidget *parent = nullptr);

private slots:
    void onAddClicked();
    void onRemoveClicked();
    void onStartClicked();
    void onPauseClicked();
    void onTaskStateChanged(const QString &id, batchexport::TaskState state);
    void onTaskProgress(const QString &id, int percent);

private:
    batchexport::Queue *m_queue      = nullptr;
    QTableWidget       *m_table      = nullptr;
    QPushButton        *m_addBtn     = nullptr;
    QPushButton        *m_removeBtn  = nullptr;
    QPushButton        *m_startBtn   = nullptr;
    QPushButton        *m_pauseBtn   = nullptr;

    int rowForId(const QString &id) const;
};
