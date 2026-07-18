#include "BatchExportQueue.h"

#include <QJsonObject>

namespace batchexport {

namespace {
// Map the dialog's preset string (1080p / 720p / 4K) onto render geometry +
// bitrate. Unknown strings fall back to 1080p — the same forgiving behaviour
// the dialog's fixed preset list implies.
struct PresetGeom {
    int width;
    int height;
    int bitrateKbps;
};
PresetGeom geomForPreset(const QString &preset)
{
    const QString p = preset.trimmed().toLower();
    if (p == QStringLiteral("4k") || p == QStringLiteral("2160p"))
        return { 3840, 2160, 80000 };
    if (p == QStringLiteral("720p"))
        return { 1280, 720, 8000 };
    // "1080p" and anything unrecognised.
    return { 1920, 1080, 20000 };
}
} // namespace

Queue::Queue(QObject *parent)
    : QObject(parent)
{
    // S9: own the genuine render pipeline (the S8 ffmpeg render-pipe). Every
    // batch task is delegated here — there is no fake-progress simulation any
    // more. We drive ONE job at a time so batch-level pause/resume stays under
    // BatchExportQueue's control (RenderQueue has its own QProcess; we never
    // need to externally pump it).
    m_render = new RenderQueue(this);

    connect(m_render, &RenderQueue::jobProgressUuid,
            this, &Queue::onJobProgress);
    connect(m_render, &RenderQueue::jobCompletedUuid,
            this, &Queue::onJobCompleted);
}

QString Queue::addTask(const QString &projectPath,
                       const QString &outputPath,
                       const QString &preset)
{
    return addTask(projectPath, outputPath, preset,
                   nullptr, 0, 0, 0, 0);
}

QString Queue::addTask(const QString &projectPath,
                       const QString &outputPath,
                       const QString &preset,
                       Timeline *timeline,
                       int width,
                       int height,
                       qint64 startUs,
                       qint64 endUs)
{
    ExportTask t;
    t.id          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t.projectPath = projectPath;
    t.outputPath  = outputPath;
    t.preset      = preset;
    t.state       = TaskState::Queued;
    t.progress    = 0;
    t.timeline    = timeline;
    t.width       = width;
    t.height      = height;
    t.startUs     = startUs;
    t.endUs       = endUs;
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
    // Batch-level pause: the queue must NOT advance to the next task while
    // paused. This preserves the exact intent of the old timer-lambda's
    // `if (m_paused) return;` guard (commit 5f184fd) — now controlling the
    // real RenderQueue rather than a fake counter. The in-flight ffmpeg of
    // the CURRENT job is allowed to finish (acceptable batch-pause semantics:
    // RenderQueue runs one QProcess at a time and killing it mid-encode would
    // corrupt the partial output); the key guarantee is that onJobCompleted
    // will NOT dispatch the next pending task while m_paused is set.
    m_paused = true;
}

void Queue::resume()
{
    if (!m_paused)
        return;
    m_paused = false;
    // If nothing is currently rendering, kick the next pending task. (If a
    // job is still in flight, its onJobCompleted will advance the queue now
    // that the pause guard is clear.)
    if (m_running && m_currentId.isEmpty())
        processNext();
}

QList<ExportTask> Queue::tasks() const
{
    return m_tasks;
}

int Queue::indexForUuid(const QString &uuid) const
{
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].renderUuid == uuid)
            return i;
    }
    return -1;
}

void Queue::processNext()
{
    // Honour the batch-level pause guard (intent preserved from the old
    // `if (m_paused) return;`): never dispatch a new job while paused.
    if (m_paused)
        return;

    // Find first Queued task and delegate it to the real RenderQueue.
    for (auto &task : m_tasks) {
        if (task.state == TaskState::Queued) {
            m_currentId   = task.id;
            task.state    = TaskState::Running;
            task.progress = 0;

            const PresetGeom g = geomForPreset(task.preset);
            const int outW = task.width  > 0 ? task.width  : g.width;
            const int outH = task.height > 0 ? task.height : g.height;

            RenderJob job;
            job.uuid            = QUuid::createUuid()
                                      .toString(QUuid::WithoutBraces);
            job.name            = task.outputPath;
            job.projectFilePath = task.projectPath;
            job.outputPath      = task.outputPath;
            job.preset          = task.preset;
            job.width           = outW;
            job.height          = outH;
            job.codec           = QStringLiteral("h264");
            job.bitrateBps      = g.bitrateKbps * 1000;
            job.startUs         = task.startUs;
            job.endUs           = task.endUs;
            // The additive in-memory edit-graph seam (LUT / mask / tracking
            // are NOT serialized to .veditor) — handed straight to the real
            // RenderQueue exactly as PARITY S8 does (main.cpp:4526).
            job.timeline        = task.timeline;

            QJsonObject cfg;
            cfg["width"]        = outW;
            cfg["height"]       = outH;
            cfg["fps"]          = 30.0;
            cfg["videoCodec"]   = QStringLiteral("libx264");
            cfg["videoBitrate"] = g.bitrateKbps;     // kbps
            cfg["audioCodec"]   = QStringLiteral("aac");
            cfg["audioBitrate"] = 192;
            job.exportConfig    = cfg;

            task.renderUuid = job.uuid;

            emit taskStateChanged(task.id, TaskState::Running);

            m_render->addJob(job);
            m_render->start();
            return;
        }
    }

    // No more queued tasks.
    m_currentId.clear();
    m_running = false;
    emit queueFinished();
}

void Queue::onJobProgress(QString uuid, int percent)
{
    const int idx = indexForUuid(uuid);
    if (idx < 0)
        return;
    ExportTask &task = m_tasks[idx];
    if (task.state != TaskState::Running)
        return;
    task.progress = percent;
    emit taskProgress(task.id, percent);
}

void Queue::onJobCompleted(QString uuid, bool success, QString error)
{
    const int idx = indexForUuid(uuid);
    if (idx < 0)
        return;
    ExportTask &task = m_tasks[idx];

    if (success) {
        task.progress = 100;
        task.state    = TaskState::Done;
        task.message  = QStringLiteral("Export complete");
        emit taskProgress(task.id, 100);
        emit taskStateChanged(task.id, TaskState::Done);
    } else {
        task.state   = TaskState::Failed;
        task.message = error.isEmpty()
            ? QStringLiteral("Export failed") : error;
        emit taskStateChanged(task.id, TaskState::Failed);
    }

    m_currentId.clear();

    // Advance to the next task — but ONLY if not paused. This is the real
    // teeth of pause(): a paused queue stops here and does not dispatch the
    // next RenderJob (the old `if (!m_paused) processNext();` semantics,
    // preserved through the new RenderQueue-backed path). resume() will
    // re-enter processNext() when the user unpauses.
    if (!m_paused)
        processNext();
}

} // namespace batchexport
