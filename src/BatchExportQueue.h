#pragma once
#include <QList>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUuid>

#include "RenderQueue.h"

class Timeline;

namespace batchexport {

enum class TaskState { Queued, Running, Done, Failed };
enum class TaskSource { File, CurrentProject };

struct ExportPresetDefaults {
    int width = 1920;
    int height = 1080;
    QString codec = QStringLiteral("h264");
    QString videoCodec = QStringLiteral("libx264");
    int videoBitrateKbps = 20000;
    QString audioCodec = QStringLiteral("aac");
    int audioBitrateKbps = 192;
};

// Settings supplied by the dialog. Empty codec fields and non-positive
// bitrates intentionally mean "use the selected preset default".
struct ExportSettings {
    QString preset = QStringLiteral("1080p");
    QString codec;
    QString videoCodec;
    int videoBitrateKbps = 0;
    QString audioCodec;
    int audioBitrateKbps = 0;
};

ExportPresetDefaults presetDefaults(const QString &preset);

struct ExportTask {
    QString   id;
    QString   projectPath;
    QString   outputPath;
    QString   preset;
    TaskSource source   = TaskSource::File;
    TaskState state    = TaskState::Queued;
    int       progress = 0;
    QString   message;

    // S9 (NLE-parity): additive, NON-persisted in-memory render seam, mirroring
    // RenderJob::timeline (RenderQueue.h:65). When a caller already holds a live
    // edit-graph Timeline (the BATCHEXPORT selftest, or any in-process batch
    // driver) it points this at it so the delegated RenderQueue renders THAT
    // timeline through tlrender::renderFrameAt directly. NOT owned by the task;
    // the caller guarantees the pointee outlives the queue. When null (the
    // file source row), RenderQueue loads `projectPath` via the genuine
    // ProjectFile::load path. The current-project row supplies this pointer.
    Timeline *timeline = nullptr;

    // Optional explicit render geometry / range. When left at 0 the preset
    // string (1080p / 720p / 4K) drives width/height and the whole timeline
    // is exported. The selftest sets these to bound a short, deterministic
    // export (start/end map straight onto RenderJob::startUs/endUs).
    int    width  = 0;
    int    height = 0;
    qint64 startUs = 0;
    qint64 endUs   = 0;
    double fps = 0.0;

    // Encoder settings. Empty strings and non-positive bitrates are filled
    // from the preset when the task enters the queue.
    QString codec;
    QString videoCodec;
    int     videoBitrateKbps = 0;
    QString audioCodec;
    int     audioBitrateKbps = 0;

    // The exact config handed to the delegated RenderJob, populated when the
    // task is dispatched. Keeping it on the public task snapshot makes the
    // UI-facing contract and headless E2E assertions observable.
    QJsonObject exportConfig;

    // uuid of the delegated RenderQueue job (set when the task is dispatched).
    QString renderUuid;
};

// Batch export queue. S9: no longer a fake-progress simulator — every task is
// delegated to an owned RenderQueue (the genuine S8 ffmpeg render-pipe). The
// public surface (addTask / removeTask / start / pause / resume / tasks +
// taskStateChanged / taskProgress / queueFinished) remains compatible while
// the dialog can submit either a live Timeline or a project file.
class Queue : public QObject {
    Q_OBJECT
public:
    explicit Queue(QObject *parent = nullptr);

    QString addTask(const QString &projectPath,
                    const QString &outputPath,
                    const QString &preset);

    // Overload for in-process callers (the selftest) that already hold a live
    // edit-graph Timeline + want an explicit short range. Keeps the original
    // 3-arg addTask signature intact for BatchExportDialog.
    QString addTask(const QString &projectPath,
                    const QString &outputPath,
                    const QString &preset,
                    Timeline *timeline,
                    int width,
                    int height,
                    qint64 startUs,
                    qint64 endUs,
                    double fps = 0.0);

    // Add a task prepared by the dialog's GUI-free task builder.
    QString addTask(const ExportTask &task);

    // RenderJob generation is shared by processNext() and the headless E2E
    // selftest. It does not start a process or touch the event loop.
    static RenderJob buildRenderJob(const ExportTask &task);

    void removeTask(const QString &id);
    void start();
    void pause();
    void resume();

    QList<ExportTask> tasks() const;

signals:
    void taskStateChanged(const QString &id, batchexport::TaskState state);
    void taskProgress(const QString &id, int percent);
    void queueFinished();

private:
    void processNext();
    void onJobProgress(QString uuid, int percent);
    void onJobCompleted(QString uuid, bool success, QString error);
    int  indexForUuid(const QString &uuid) const;

    QList<ExportTask> m_tasks;
    bool              m_running   = false;
    bool              m_paused    = false;
    QString           m_currentId;
    RenderQueue      *m_render    = nullptr;
};

} // namespace batchexport
