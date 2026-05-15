#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUuid>

namespace batchexport {

enum class TaskState { Queued, Running, Done, Failed };

struct ExportTask {
    QString   id;
    QString   projectPath;
    QString   outputPath;
    QString   preset;
    TaskState state    = TaskState::Queued;
    int       progress = 0;
    QString   message;
};

class Queue : public QObject {
    Q_OBJECT
public:
    explicit Queue(QObject *parent = nullptr);

    QString addTask(const QString &projectPath,
                    const QString &outputPath,
                    const QString &preset);

    void removeTask(const QString &id);
    void start();
    void pause();

    QList<ExportTask> tasks() const;

signals:
    void taskStateChanged(const QString &id, batchexport::TaskState state);
    void taskProgress(const QString &id, int percent);
    void queueFinished();

private:
    void processNext();

    QList<ExportTask> m_tasks;
    bool              m_running   = false;
    bool              m_paused    = false;
    QString           m_currentId;
    QTimer           *m_timer     = nullptr;
};

} // namespace batchexport
