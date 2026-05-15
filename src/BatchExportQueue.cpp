#include "BatchExportQueue.h"

namespace batchexport {

Queue::Queue(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(200);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (m_currentId.isEmpty())
            return;

        // Find the running task
        for (auto &task : m_tasks) {
            if (task.id == m_currentId && task.state == TaskState::Running) {
                task.progress += 20;
                if (task.progress > 100)
                    task.progress = 100;
                emit taskProgress(task.id, task.progress);
                if (task.progress >= 100) {
                    m_timer->stop();
                    task.state   = TaskState::Done;
                    task.message = QStringLiteral("Export complete");
                    emit taskStateChanged(task.id, TaskState::Done);
                    m_currentId.clear();
                    if (!m_paused)
                        processNext();
                }
                break;
            }
        }
    });
}

QString Queue::addTask(const QString &projectPath,
                       const QString &outputPath,
                       const QString &preset)
{
    ExportTask t;
    t.id          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t.projectPath = projectPath;
    t.outputPath  = outputPath;
    t.preset      = preset;
    t.state       = TaskState::Queued;
    t.progress    = 0;
    m_tasks.append(t);
    return t.id;
}

void Queue::removeTask(const QString &id)
{
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id == id) {
            if (m_tasks[i].state == TaskState::Running)
                return; // cannot remove a running task
            m_tasks.removeAt(i);
            return;
        }
    }
}

void Queue::start()
{
    m_running = true;
    m_paused  = false;
    if (m_currentId.isEmpty())
        processNext();
}

void Queue::pause()
{
    m_paused = true;
}

QList<ExportTask> Queue::tasks() const
{
    return m_tasks;
}

void Queue::processNext()
{
    // Find first Queued task
    for (auto &task : m_tasks) {
        if (task.state == TaskState::Queued) {
            m_currentId  = task.id;
            task.state   = TaskState::Running;
            task.progress = 0;
            emit taskStateChanged(task.id, TaskState::Running);
            m_timer->start();
            return;
        }
    }
    // No more queued tasks
    m_currentId.clear();
    m_running = false;
    emit queueFinished();
}

} // namespace batchexport
