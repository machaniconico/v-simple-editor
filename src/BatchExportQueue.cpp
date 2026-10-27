#include "BatchExportQueue.h"

#include "ExportUserPresets.h"
#include "RenderInPlace.h"
#include "Timeline.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>

namespace batchexport {

namespace {
void fillPresetDefaults(ExportTask &task)
{
    if (task.preset.trimmed().isEmpty())
        task.preset = QStringLiteral("1080p");

    const ExportPresetDefaults defaults = presetDefaults(task.preset);
    if (task.codec.isEmpty())
        task.codec = defaults.codec;
    if (task.videoCodec.isEmpty())
        task.videoCodec = defaults.videoCodec;
    if (task.videoBitrateKbps <= 0)
        task.videoBitrateKbps = defaults.videoBitrateKbps;
    if (task.audioCodec.isEmpty())
        task.audioCodec = defaults.audioCodec;
    if (task.audioBitrateKbps <= 0)
        task.audioBitrateKbps = defaults.audioBitrateKbps;
}
} // namespace

ExportPresetDefaults presetDefaults(const QString &preset)
{
    const QString p = preset.trimmed().toLower();
    if (p == QStringLiteral("4k") || p == QStringLiteral("2160p")) {
        return { 3840, 2160, QStringLiteral("h264"),
                 QStringLiteral("libx264"), 80000,
                 QStringLiteral("aac"), 192 };
    }
    if (p == QStringLiteral("720p")) {
        return { 1280, 720, QStringLiteral("h264"),
                 QStringLiteral("libx264"), 8000,
                 QStringLiteral("aac"), 192 };
    }
    // "1080p" and anything unrecognised.
    return { 1920, 1080, QStringLiteral("h264"),
             QStringLiteral("libx264"), 20000,
             QStringLiteral("aac"), 192 };
}

// Batch built-ins always resolve as built-ins (case-insensitive); a same-named user
// preset is neither offered nor consulted for loudness. Single source for both sites.
static bool samePresetName(const QString &a, const QString &b)
{
    return a.trimmed().compare(b.trimmed(), Qt::CaseSensitive) == 0;
}

static bool isBatchBuiltinName(const QString &name)
{
    const QString n = name.trimmed();
    return n.compare(QStringLiteral("1080p"), Qt::CaseInsensitive) == 0
        || n.compare(QStringLiteral("720p"), Qt::CaseInsensitive) == 0
        || n.compare(QStringLiteral("4K"), Qt::CaseInsensitive) == 0;
}

// Saved names are stored untrimmed, but the batch dialog trims the selected text. Names are
// therefore compared trimmed everywhere (choices de-duplication and loudness lookup).
QStringList presetChoices()
{
    QStringList names{QStringLiteral("1080p"), QStringLiteral("720p"), QStringLiteral("4K")};
    for (const auto &p : ExportUserPresets::load()) {
        if (p.name.trimmed().isEmpty() || isBatchBuiltinName(p.name)) continue;
        const bool dup = std::any_of(names.begin(), names.end(), [&](const QString &n) {
            return samePresetName(n, p.name); });
        if (!dup) names << p.name;
    }
    return names;
}

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

// Resolve the task's saved user preset into the auto-loudness fields. Built-in
// preset names never resolve to a user preset. Returns whether the name resolved (built-in or
// a saved user preset); only a resolved name counts as captured (loudnessResolved).
static bool captureLoudness(ExportTask &task)
{
    task.loudnessResolved = false;
    task.autoLoudness = false;
    if (isBatchBuiltinName(task.preset)) {
        task.loudnessResolved = true;
        return true;
    }
    // Same rule as presetChoices(): look the saved user preset up by name directly, so a user
    // preset sharing a name with an ExportDialog built-in still resolves.
    ExportConfig presetConfig;
    bool found = false;
    for (const auto &p : ExportUserPresets::load()) {
        if (!samePresetName(p.name, task.preset)) continue;
        presetConfig = p.config;
        found = true;
        break;
    }
    if (!found) return false;
    task.loudnessResolved = true;
    if (!presetConfig.autoLoudness) return true;
    QJsonObject loudness;
    presetConfig.appendLoudnessSettings(loudness);
    task.autoLoudness = true;
    task.targetLufs = loudness.value("targetLufs").toDouble(-14.0);
    return true;
}

QString Queue::addTask(const QString &projectPath,
                       const QString &outputPath,
                       const QString &preset)
{
    return addTask(projectPath, outputPath, preset,
                   nullptr, 0, 0, 0, 0, 0.0);
}

QString Queue::addTask(const QString &projectPath,
                       const QString &outputPath,
                       const QString &preset,
                       Timeline *timeline,
                       int width,
                       int height,
                       qint64 startUs,
                       qint64 endUs,
                       double fps)
{
    ExportTask t;
    t.projectPath = projectPath;
    t.outputPath  = outputPath;
    t.preset      = preset;
    t.source      = timeline ? TaskSource::CurrentProject : TaskSource::File;
    t.timeline    = timeline;
    t.width       = width;
    t.height      = height;
    t.startUs     = startUs;
    t.endUs       = endUs;
    t.fps         = fps;
    return addTask(t);
}

QString Queue::addTask(const ExportTask &input)
{
    ExportTask t = input;
    t.id       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t.state    = TaskState::Queued;
    t.progress = 0;
    t.message.clear();
    t.renderUuid.clear();
    t.exportConfig = QJsonObject();
    t.dispatchedProjectFilePath.clear();
    fillPresetDefaults(t);
    captureLoudness(t);
    m_tasks.append(t);
    return t.id;
}

RenderJob Queue::buildRenderJob(const ExportTask &input)
{
    ExportTask task = input;
    fillPresetDefaults(task);
    const ExportPresetDefaults defaults = presetDefaults(task.preset);
    const int outW = task.width > 0 ? task.width : defaults.width;
    const int outH = task.height > 0 ? task.height : defaults.height;

    RenderJob job;
    job.uuid            = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.name            = task.outputPath;
    job.projectFilePath = task.projectPath;
    job.outputPath      = task.outputPath;
    job.preset          = task.preset;
    job.width           = outW;
    job.height          = outH;
    job.codec           = task.codec;
    job.bitrateBps      = static_cast<qint64>(task.videoBitrateKbps) * 1000;
    job.startUs         = task.startUs;
    job.endUs           = task.endUs;
    // The additive in-memory edit-graph seam (LUT / mask / tracking are not
    // serialized to .veditor) is handed straight to RenderQueue.
    job.timeline        = task.timeline;

    QJsonObject cfg;
    cfg["width"]        = outW;
    cfg["height"]       = outH;
    if (task.fps > 0.0)
        cfg["fps"]       = task.fps;
    cfg["videoCodec"]   = task.videoCodec;
    cfg["videoBitrate"] = task.videoBitrateKbps;
    cfg["audioCodec"]   = task.audioCodec;
    cfg["audioBitrate"] = task.audioBitrateKbps;
    // Intentional exception to rule 17b (exportcfg::toJobJson): a saved user preset
    // with auto loudness adds ONLY these two keys. Mapping the whole ExportConfig
    // would also pull HDR / subtitle / metadata / sequence settings into queued
    // jobs, a behaviour change beyond US-711.
    if (!task.loudnessResolved)
        captureLoudness(task);
    if (task.autoLoudness) {
        cfg["autoLoudness"] = true;
        cfg["targetLufs"]   = task.targetLufs;
    }
    job.exportConfig     = cfg;
    return job;
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
    for (int taskIndex = 0; taskIndex < m_tasks.size(); ++taskIndex) {
        ExportTask &task = m_tasks[taskIndex];
        if (task.state == TaskState::Queued) {
            // A name that could not be resolved when queued (or a task restored without a
            // capture) is looked up again now; if it is still unknown the task fails
            // instead of silently exporting with the 1080p defaults.
            if (!task.loudnessResolved && !captureLoudness(task)) {
                task.state   = TaskState::Failed;
                task.message = QStringLiteral("プリセット '%1' が見つかりません").arg(task.preset);
                emit taskStateChanged(task.id, TaskState::Failed);
                continue;
            }
            m_currentId   = task.id;
            task.state    = TaskState::Running;
            task.progress = 0;

            RenderJob job = buildRenderJob(task);
            if (job.exportConfig.value(QStringLiteral("autoLoudness")).toBool(false)) {
                // Same measure -> gain -> AAC swap as the normal export path. Without a
                // live timeline there is nothing to measure: reject rather than 0 dB.
                if (!job.timeline) {
                    job.exportConfig[QStringLiteral("loudnessAutoError")] = QStringLiteral(
                        "自動ラウドネス: 現在のプロジェクト以外のタスクでは測定できません");
                } else {
                    loudnessauto::prepareJob(job);
                    // measureAutoLoudness only records "skipped, 0 dB" for silent /
                    // audio-less / unmeasurable audio; never export that unnormalized.
                    const double measured = job.exportConfig.value(QStringLiteral("measuredLufs"))
                        .toDouble(std::numeric_limits<double>::quiet_NaN());
                    if (!std::isfinite(measured) || measured <= -70.0)
                        job.exportConfig[QStringLiteral("loudnessAutoError")] = QStringLiteral(
                            "自動ラウドネス: 無音・音声なし・測定失敗のため正規化できません");
                }
            } else if (job.timeline && job.timeline->totalDuration() > 0.0
                       && (!job.timeline->computeAudioPlaybackSequence().isEmpty()
                           || (job.timeline->videoClips().isEmpty()
                               && !loudnessauto::timelineAudioMatchesPassthrough(job.timeline)))) {
                // Empty V1 with video on an upper track: also a silent / mixed track, as
                // the normal export (the V1 source-audio fallback has no clip to read).
                // Same AAC timeline-mix intermediate as the normal export: the queue
                // muxes audio by stream copy, so this must not be a .wav. Built from
                // the timeline as it is now (task dispatch), like the video frames.
                const QString mixPath = QDir::tempPath()
                    + QStringLiteral("/v-simple-editor-audio-mix/batch_mix_%1.m4a").arg(job.uuid);
                QDir().mkpath(QFileInfo(mixPath).absolutePath());
                RenderQueue::addTempFile(job.exportConfig, mixPath);
                QString mixError;
                const QString mixed = renderinplace::prepareAudioMix(
                    job.timeline, mixPath, job.timeline->totalDuration(), &mixError,
                    task.audioBitrateKbps);
                if (mixed == mixPath && QFileInfo::exists(mixPath)) {
                    RenderQueue::setIntermediateProjectFile(job, mixPath);
                } else if (job.timeline->videoClips().isEmpty()) {
                    // Empty V1: there is no source audio to fall back to (the render would
                    // be video-only), so reject like the other entry points.
                    job.exportConfig[QStringLiteral("loudnessAutoError")] = QStringLiteral(
                        "タイムライン音声ミックスの作成に失敗しました: %1").arg(mixError);
                    qWarning().noquote() << job.exportConfig.value(
                        QStringLiteral("loudnessAutoError")).toString();
                } else {
                    // Fall back to the previous audio source, but keep the reason.
                    job.exportConfig[QStringLiteral("audioMixWarning")] = QStringLiteral(
                        "タイムライン音声ミックスの作成に失敗したため元ファイル音声で書き出します: %1")
                        .arg(mixError);
                    qWarning().noquote() << job.exportConfig.value(
                        QStringLiteral("audioMixWarning")).toString();
                }
            }
            // A rejected job (loudnessAutoError) is never rendered; RenderQueue::addJob
            // deletes its exportConfig["tempFiles"] when it emits the failure.
            task.exportConfig = job.exportConfig;
            task.dispatchedProjectFilePath = job.projectFilePath;

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
        const QString audioMixWarning =
            task.exportConfig.value(QStringLiteral("audioMixWarning")).toString();
        task.message  = audioMixWarning.isEmpty()
            ? QStringLiteral("Export complete")
            : QStringLiteral("Export complete (%1)").arg(audioMixWarning);
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
