#include "../BatchExportDialog.h"
#include "../BatchExportQueue.h"
#include "../ExportUserPresets.h"
#include "../LoudnessMaster.h"
#include "../Timeline.h"
#include "../libavcore/AudioExtract.h"
#include "../libavcore/Encode.h"

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTest>
#include <QHash>
#include <QMetaObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

bool nearlyEqual(double lhs, double rhs)
{
    return std::fabs(lhs - rhs) <= 1.0e-9;
}

bool containsState(const QVector<batchexport::TaskState> &states,
                   batchexport::TaskState wanted)
{
    return std::find(states.cbegin(), states.cend(), wanted) != states.cend();
}

batchexport::TaskState taskState(const batchexport::Queue &queue,
                                 const QString &id)
{
    for (const batchexport::ExportTask &task : queue.tasks()) {
        if (task.id == id)
            return task.state;
    }
    return batchexport::TaskState::Failed;
}

bool writeG8Video(const QString &path)
{
    libavcore::EncodeRequest request;
    request.outputPath = path.toStdString();
    request.width = 64;
    request.height = 64;
    request.fps = request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    request.videoBitrateBits = 200000;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(64, 64, QImage::Format_RGB888);
    frame.fill(Qt::blue);
    for (int i = 0; i < 40; ++i)
        if (!encoder.pushFrame(frame, i)) return false;
    return !encoder.finalize();
}

bool runQueueContractGate(QString *detail)
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        if (detail)
            *detail = QStringLiteral("temporary directory unavailable");
        return false;
    }

    batchexport::ExportSettings settings;
    settings.preset = QStringLiteral("1080p");

    const batchexport::ExportTask firstTask =
        BatchExportDialog::makeFileProjectTask(
            tempDir.filePath(QStringLiteral("missing-first.veditor")),
            tempDir.filePath(QStringLiteral("first.mp4")), settings);
    const batchexport::ExportTask secondTask =
        BatchExportDialog::makeFileProjectTask(
            tempDir.filePath(QStringLiteral("missing-second.veditor")),
            tempDir.filePath(QStringLiteral("second.mp4")), settings);

    batchexport::Queue queue;
    const QString firstId = queue.addTask(firstTask);
    const QString secondId = queue.addTask(secondTask);
    if (firstId.isEmpty() || secondId.isEmpty()) {
        if (detail)
            *detail = QStringLiteral("queue did not register two tasks");
        return false;
    }

    QHash<QString, QVector<batchexport::TaskState>> states;
    QSet<QString> progressIds;
    int queueFinishedCount = 0;
    QObject::connect(&queue, &batchexport::Queue::taskStateChanged,
                     &queue, [&](const QString &id,
                                 batchexport::TaskState state) {
        states[id].append(state);
    });
    QObject::connect(&queue, &batchexport::Queue::taskProgress,
                     &queue, [&](const QString &id, int) {
        progressIds.insert(id);
    });
    QObject::connect(&queue, &batchexport::Queue::queueFinished,
                     &queue, [&]() { ++queueFinishedCount; });

    QEventLoop firstLoop;
    QTimer firstTimeout;
    firstTimeout.setSingleShot(true);
    bool firstTerminal = false;
    QObject::connect(&queue, &batchexport::Queue::taskStateChanged,
                     &firstLoop, [&](const QString &id,
                                     batchexport::TaskState state) {
        if (id == firstId
            && (state == batchexport::TaskState::Done
                || state == batchexport::TaskState::Failed)) {
            firstTerminal = true;
            firstLoop.quit();
        }
    });
    QObject::connect(&firstTimeout, &QTimer::timeout,
                     &firstLoop, &QEventLoop::quit);

    queue.start();
    queue.pause();
    if (!firstTerminal) {
        firstTimeout.start(10000);
        firstLoop.exec();
    }
    firstTimeout.stop();

    const bool signalContract =
        queue.metaObject()->indexOfSignal("taskProgress(QString,int)") >= 0
        && queue.metaObject()->indexOfSignal(
               "taskStateChanged(QString,batchexport::TaskState)") >= 0
        && queue.metaObject()->indexOfSignal("queueFinished()") >= 0;
    const bool firstEmittedExpectedStates =
        containsState(states.value(firstId), batchexport::TaskState::Running)
        && containsState(states.value(firstId), batchexport::TaskState::Failed);
    const bool pauseHeld =
        firstTerminal
        && taskState(queue, firstId) == batchexport::TaskState::Failed
        && taskState(queue, secondId) == batchexport::TaskState::Queued
        && queueFinishedCount == 0;

    QEventLoop secondLoop;
    QTimer secondTimeout;
    secondTimeout.setSingleShot(true);
    bool finishedAfterResume = false;
    QObject::connect(&queue, &batchexport::Queue::queueFinished,
                     &secondLoop, [&]() {
        finishedAfterResume = true;
        secondLoop.quit();
    });
    QObject::connect(&secondTimeout, &QTimer::timeout,
                     &secondLoop, &QEventLoop::quit);
    queue.resume();
    if (!finishedAfterResume) {
        secondTimeout.start(10000);
        secondLoop.exec();
    }
    secondTimeout.stop();

    const bool resumedAndFinished =
        finishedAfterResume
        && queueFinishedCount == 1
        && taskState(queue, secondId) == batchexport::TaskState::Failed
        && containsState(states.value(secondId), batchexport::TaskState::Running)
        && containsState(states.value(secondId), batchexport::TaskState::Failed);

    // Invalid project files fail before a render frame exists, so no progress
    // value is expected here. The real success-path taskProgress=100 contract
    // remains covered by the existing batchexport selftest when test_assets is
    // present; this gate verifies the signal exists and pause/advance ordering.
    Q_UNUSED(progressIds);
    if (!(signalContract && firstEmittedExpectedStates
          && pauseHeld && resumedAndFinished)) {
        if (detail) {
            *detail = QStringLiteral(
                "signals=%1 firstStates=%2 pauseHeld=%3 resumed=%4")
                .arg(signalContract)
                .arg(firstEmittedExpectedStates)
                .arg(pauseHeld)
                .arg(resumedAndFinished);
        }
        return false;
    }
    return true;
}

} // namespace

int runBatchExportE2ESelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral(
                "[batchexport-e2e] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral(
                "[batchexport-e2e] FAIL G%1 %2%3")
                .arg(gate)
                .arg(QString::fromLatin1(name))
                .arg(detail.isEmpty() ? QString()
                                      : QStringLiteral(": ") + detail);
        }
    };

    if (!QApplication::instance()) {
        qCritical().noquote() << QStringLiteral(
            "[batchexport-e2e] FAIL G1 QApplication is unavailable");
        return 1;
    }

    // The selftest creates a real Timeline model but never shows a window or
    // invokes a dialog. This makes the pointer transport check genuine while
    // keeping the E2E path GUI-free.
    Timeline timeline;
    BatchExportDialog::CurrentProjectContext context;
    context.projectPath = QStringLiteral("current-project.veditor");
    context.timeline = &timeline;
    context.width = 640;
    context.height = 360;
    context.fps = 24.0;
    context.startUs = 1'000'000;
    context.endUs = 3'000'000;

    batchexport::ExportSettings currentSettings;
    currentSettings.preset = QStringLiteral("1080p");
    currentSettings.codec = QStringLiteral("h264");
    currentSettings.videoCodec = QStringLiteral("libx264");
    currentSettings.videoBitrateKbps = 12345;
    currentSettings.audioCodec = QStringLiteral("aac");
    currentSettings.audioBitrateKbps = 160;
    const batchexport::ExportTask currentTask =
        BatchExportDialog::makeCurrentProjectTask(
            context, QStringLiteral("current.mp4"), currentSettings);
    check(1, "current-project task carries live Timeline",
          currentTask.timeline == &timeline
              && currentTask.source == batchexport::TaskSource::CurrentProject);

    const RenderJob currentJob =
        batchexport::Queue::buildRenderJob(currentTask);
    const double currentFps = qvariant_cast<double>(
        currentJob.exportConfig.value("fps").toVariant());
    check(2, "explicit fps reaches RenderJob exportConfig",
          currentJob.timeline == &timeline
              && nearlyEqual(currentFps, 24.0)
              && currentJob.exportConfig.value("videoBitrate").toInt() == 12345
              && currentJob.exportConfig.value("audioBitrate").toInt() == 160);

    batchexport::ExportSettings unspecifiedSettings;
    unspecifiedSettings.preset = QStringLiteral("1080p");
    const batchexport::ExportTask unspecifiedTask =
        BatchExportDialog::makeFileProjectTask(
            QStringLiteral("file-project.veditor"),
            QStringLiteral("file.mp4"), unspecifiedSettings);
    const RenderJob unspecifiedJob =
        batchexport::Queue::buildRenderJob(unspecifiedTask);
    check(3, "unspecified fps leaves exportConfig key absent",
          unspecifiedJob.exportConfig.contains("fps") == false);

    batchexport::ExportTask explicitSizeTask = currentTask;
    explicitSizeTask.width = 640;
    explicitSizeTask.height = 360;
    explicitSizeTask.preset = QStringLiteral("1080p");
    const RenderJob explicitSizeJob =
        batchexport::Queue::buildRenderJob(explicitSizeTask);

    batchexport::ExportSettings presetSettings;
    presetSettings.preset = QStringLiteral("720p");
    const batchexport::ExportTask presetSizeTask =
        BatchExportDialog::makeFileProjectTask(
            QStringLiteral("preset-project.veditor"),
            QStringLiteral("preset.mp4"), presetSettings);
    const RenderJob presetSizeJob =
        batchexport::Queue::buildRenderJob(presetSizeTask);
    check(4, "explicit geometry wins and preset geometry is fallback",
          explicitSizeJob.exportConfig.value("width").toInt() == 640
              && explicitSizeJob.exportConfig.value("height").toInt() == 360
              && presetSizeJob.exportConfig.value("width").toInt() == 1280
              && presetSizeJob.exportConfig.value("height").toInt() == 720);

    const batchexport::ExportTask fileTask =
        BatchExportDialog::makeFileProjectTask(
            QStringLiteral("saved-project.veditor"),
            QStringLiteral("saved.mp4"), currentSettings);
    const bool fileRouteIsExplicit =
        fileTask.timeline == nullptr
        && fileTask.source == batchexport::TaskSource::File;
    QString detail;
    check(5, "file route retains its source distinction",
          fileRouteIsExplicit, QStringLiteral("source=%1 timeline=%2")
              .arg(static_cast<int>(fileTask.source))
              .arg(fileTask.timeline != nullptr));

    detail.clear();
    check(6, "queue signals and paused advance contract",
          runQueueContractGate(&detail), detail);

    // G7: a saved user preset with auto loudness reaches the job; a preset
    // without loudness leaves the legacy 7 keys unchanged and adds no loudness key.
    {
        QTemporaryDir presetDir;
        const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
        const QByteArray oldEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
        qputenv("VEDITOR_EXPORT_PRESET_DIR", presetDir.path().toUtf8());
        ExportUserPreset loud;
        loud.name = QStringLiteral("g7-loud");
        loud.config.autoLoudness = true;
        loud.config.targetLufs = -16.0;
        ExportUserPreset plain;
        plain.name = QStringLiteral("g7-plain");
        QString saveError;
        const bool saved = presetDir.isValid()
            && ExportUserPresets::save(loud, &saveError)
            && ExportUserPresets::save(plain, &saveError);

        auto legacyKeysMatch = [](const RenderJob &a, const RenderJob &b) {
            for (const char *k : {"width", "height", "fps", "videoCodec", "videoBitrate",
                                  "audioCodec", "audioBitrate"})
                if (a.exportConfig.value(k) != b.exportConfig.value(k)) return false;
            return a.exportConfig.contains("fps") == b.exportConfig.contains("fps");
        };
        batchexport::ExportTask base = BatchExportDialog::makeFileProjectTask(
            QStringLiteral("g7.veditor"), QStringLiteral("g7.mp4"), currentSettings);
        batchexport::ExportTask loudTask = base;
        loudTask.preset = QStringLiteral("g7-loud");
        batchexport::ExportTask plainTask = base;
        plainTask.preset = QStringLiteral("g7-plain");
        const RenderJob baseJob = batchexport::Queue::buildRenderJob(base);
        const RenderJob loudJob = batchexport::Queue::buildRenderJob(loudTask);
        const RenderJob plainJob = batchexport::Queue::buildRenderJob(plainTask);
        const bool loudOn = loudJob.exportConfig.value("autoLoudness").toBool(false);
        const double loudLufs = loudJob.exportConfig.value("targetLufs").toDouble(0.0);
        const bool loudLegacy = legacyKeysMatch(loudJob, baseJob);
        const bool plainNoKey = !plainJob.exportConfig.contains("autoLoudness")
            && !plainJob.exportConfig.contains("targetLufs");
        const bool plainLegacy = legacyKeysMatch(plainJob, baseJob);
        const bool baseNoKey = !baseJob.exportConfig.contains("autoLoudness");
        // Absolute control: the 1080p built-in definition (1920x1080) plus the
        // explicit encoder settings the base task was built with; fps unset => no key.
        auto legacyAbsolute = [&](const char *label, const RenderJob &j) {
            const QJsonObject &c = j.exportConfig;
            const bool ok = c.value("width").toInt() == 1920
                && c.value("height").toInt() == 1080
                && !c.contains("fps")
                && c.value("videoCodec").toString() == QStringLiteral("libx264")
                && c.value("videoBitrate").toInt() == 12345
                && c.value("audioCodec").toString() == QStringLiteral("aac")
                && c.value("audioBitrate").toInt() == 160;
            qInfo().noquote() << QStringLiteral(
                "[batchexport-e2e] G7 %1 7keys: width=%2 height=%3 fps=%4 videoCodec=%5 "
                "videoBitrate=%6 audioCodec=%7 audioBitrate=%8 matchesConstants=%9")
                .arg(QLatin1String(label)).arg(c.value("width").toInt())
                .arg(c.value("height").toInt())
                .arg(c.contains("fps") ? QString::number(c.value("fps").toDouble())
                                       : QStringLiteral("<absent>"))
                .arg(c.value("videoCodec").toString()).arg(c.value("videoBitrate").toInt())
                .arg(c.value("audioCodec").toString()).arg(c.value("audioBitrate").toInt())
                .arg(ok);
            return ok;
        };
        const bool absBase = legacyAbsolute("base", baseJob);
        const bool absLoud = legacyAbsolute("loud", loudJob);
        const bool absPlain = legacyAbsolute("plain", plainJob);
        qInfo().noquote() << QStringLiteral(
            "[batchexport-e2e] G7 values: saved=%1 loudOn=%2 loudLufs=%3 loudLegacy=%4 "
            "plainNoKey=%5 plainLegacy=%6 baseNoKey=%7 w=%8 h=%9")
            .arg(saved).arg(loudOn).arg(loudLufs).arg(loudLegacy).arg(plainNoKey)
            .arg(plainLegacy).arg(baseNoKey)
            .arg(loudJob.exportConfig.value("width").toInt())
            .arg(loudJob.exportConfig.value("height").toInt());
        check(7, "user preset auto loudness reaches the batch job",
              saved && loudOn && nearlyEqual(loudLufs, -16.0) && loudLegacy
                  && plainNoKey && plainLegacy && baseNoKey && absBase && absLoud && absPlain,
              saveError);
        if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
        else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    }

    // G8: the queue itself measures and normalizes (loudness preset), leaves a plain
    // preset untouched, and rejects an unmeasurable (no live timeline) loudness task.
    {
        QTemporaryDir presetDir;
        QTemporaryDir work;
        const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
        const QByteArray oldEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
        qputenv("VEDITOR_EXPORT_PRESET_DIR", presetDir.path().toUtf8());
        ExportUserPreset loud;
        loud.name = QStringLiteral("g8-loud");
        loud.config.autoLoudness = true;
        loud.config.targetLufs = -16.0;
        ExportUserPreset plain;
        plain.name = QStringLiteral("g8-plain");
        ExportUserPreset quiet;
        quiet.name = QStringLiteral("g8-quiet");
        quiet.config.autoLoudness = true;
        quiet.config.targetLufs = -30.0;
        const bool saved = presetDir.isValid() && work.isValid()
            && ExportUserPresets::save(loud) && ExportUserPresets::save(plain)
            && ExportUserPresets::save(quiet);

        const QString wav = work.filePath("tone.wav");
        const QString video = work.filePath("video.mp4");
        std::vector<float> samples(48000 * 4 * 2);
        for (int i = 0; i < 48000 * 4; ++i) {
            const float v = static_cast<float>(
                0.08 * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 48000.0));
            samples[2 * i] = samples[2 * i + 1] = v;
        }
        QString werr;
        const bool fixture = saved
            && libavcore::writeWavFloatInterleaved(wav, samples, 48000, 2, &werr)
            && writeG8Video(video);
        Timeline g8Timeline;
        ClipInfo picture;
        picture.filePath = video;
        picture.duration = picture.outPoint = 4.0;
        ClipInfo audio;
        audio.filePath = wav;
        audio.duration = audio.outPoint = 4.0;
        g8Timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
            QVector<QVector<ClipInfo>>{{audio}}, 0, -1, -1, 10);

        auto makeTask = [&](const QString &preset, const QString &out, Timeline *tl) {
            batchexport::ExportTask t;
            t.projectPath = QStringLiteral("g8.veditor");
            t.outputPath = out;
            t.preset = preset;
            t.timeline = tl;
            t.source = tl ? batchexport::TaskSource::CurrentProject
                          : batchexport::TaskSource::File;
            t.width = t.height = 64;
            t.fps = 10.0;
            t.codec = QStringLiteral("mpeg4");
            t.videoCodec = QStringLiteral("mpeg4");
            t.videoBitrateKbps = 200;
            t.audioCodec = QStringLiteral("aac");
            t.audioBitrateKbps = 192;
            return t;
        };
        const QString loudOut = work.filePath("loud.mp4");
        const QString plainOut = work.filePath("plain.mp4");
        batchexport::Queue g8Queue;
        const QString quietOut = work.filePath("quiet.mp4");
        QString loudId, plainId, rejectId, quietId;
        bool finished = false;
        if (fixture) {
            loudId = g8Queue.addTask(makeTask(QStringLiteral("g8-loud"), loudOut, &g8Timeline));
            {
                // Queue an AAC source at -30 LUFS first (made by the queue itself); the plain
                // task muxes it by stream copy, so its output carries measurable audio
                // the plain preset must leave far from -16.
                quietId = g8Queue.addTask(makeTask(QStringLiteral("g8-quiet"), quietOut, &g8Timeline));
                batchexport::ExportTask plainSource =
                    makeTask(QStringLiteral("g8-plain"), plainOut, &g8Timeline);
                plainSource.projectPath = quietOut;
                plainId = g8Queue.addTask(plainSource);
            }
            rejectId = g8Queue.addTask(makeTask(QStringLiteral("g8-loud"),
                work.filePath("reject.mp4"), nullptr));
            QSignalSpy done(&g8Queue, &batchexport::Queue::queueFinished);
            g8Queue.start();
            finished = done.wait(180000) || !done.isEmpty();
        }
        batchexport::ExportTask loudTask, plainTask, rejectTask, quietTask;
        for (const auto &t : g8Queue.tasks()) {
            if (t.id == loudId) loudTask = t;
            else if (t.id == plainId) plainTask = t;
            else if (t.id == quietId) quietTask = t;
            else if (t.id == rejectId) rejectTask = t;
        }
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double loudLufs = loudTask.state == batchexport::TaskState::Done
            ? loudness::measureIntegratedLufs(loudOut) : nan;
        const double plainLufs = plainTask.state == batchexport::TaskState::Done
            ? loudness::measureIntegratedLufs(plainOut) : nan;
        const bool loudDone = loudTask.state == batchexport::TaskState::Done;
        const bool plainDone = plainTask.state == batchexport::TaskState::Done;
        const bool loudHit = std::isfinite(loudLufs) && std::fabs(loudLufs + 16.0) <= 1.0;
        // The plain output must HAVE measurable audio and it must stay far from -16.
        const bool quietDone = quietTask.state == batchexport::TaskState::Done;
        const bool plainUntouched = plainDone && quietDone
            && std::isfinite(plainLufs) && std::fabs(plainLufs + 16.0) > 3.0
            && !plainTask.exportConfig.contains("autoLoudness")
            && !plainTask.exportConfig.contains("loudnessGainDb");
        const bool rejected = rejectTask.state == batchexport::TaskState::Failed
            && !rejectTask.message.isEmpty();
        qInfo().noquote() << QStringLiteral(
            "[batchexport-e2e] G8 values: fixture=%1 finished=%2 loudDone=%3 loudLufs=%4 "
            "loudGain=%5 loudHit=%6 plainDone=%7 plainLufs=%8 plainUntouched=%9 "
            "quietDone=%12 rejectState=%10 rejectMessage=%11")
            .arg(fixture).arg(finished).arg(loudDone).arg(loudLufs)
            .arg(loudTask.exportConfig.value("loudnessGainDb").toDouble(nan))
            .arg(loudHit).arg(plainDone).arg(plainLufs).arg(plainUntouched)
            .arg(static_cast<int>(rejectTask.state)).arg(rejectTask.message).arg(quietDone);
        check(8, "queue normalizes loudness, leaves plain presets, rejects unmeasurable",
              fixture && finished && loudDone && loudHit && plainUntouched && rejected,
              werr);
        if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
        else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    }

    // G9 / G10: an unmeasurable loudness task (silent audio / no audio) is failed with a
    // reason instead of exporting unnormalized; a preset deleted after queueing still
    // normalizes because the loudness settings were captured when the task was queued.
    {
        QTemporaryDir presetDir;
        QTemporaryDir work;
        const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
        const QByteArray oldEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
        qputenv("VEDITOR_EXPORT_PRESET_DIR", presetDir.path().toUtf8());
        ExportUserPreset loud;
        loud.name = QStringLiteral("g9-loud");
        loud.config.autoLoudness = true;
        loud.config.targetLufs = -16.0;
        ExportUserPreset gone = loud;
        gone.name = QStringLiteral("g10-gone");
        const bool saved = presetDir.isValid() && work.isValid()
            && ExportUserPresets::save(loud) && ExportUserPresets::save(gone);

        const QString tone = work.filePath("tone.wav");
        const QString silence = work.filePath("silence.wav");
        const QString video = work.filePath("video.mp4");
        std::vector<float> toneSamples(48000 * 4 * 2);
        for (int i = 0; i < 48000 * 4; ++i)
            toneSamples[2 * i] = toneSamples[2 * i + 1] = static_cast<float>(
                0.08 * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 48000.0));
        const std::vector<float> silentSamples(48000 * 4 * 2, 0.0f);
        QString werr;
        const bool fixture = saved
            && libavcore::writeWavFloatInterleaved(tone, toneSamples, 48000, 2, &werr)
            && libavcore::writeWavFloatInterleaved(silence, silentSamples, 48000, 2, &werr)
            && writeG8Video(video);
        ClipInfo picture;
        picture.filePath = video;
        picture.duration = picture.outPoint = 4.0;
        auto audioClip = [](const QString &path) {
            ClipInfo c;
            c.filePath = path;
            c.duration = c.outPoint = 4.0;
            return c;
        };
        Timeline toneTimeline, silentTimeline, noAudioTimeline;
        toneTimeline.restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
            QVector<QVector<ClipInfo>>{{audioClip(tone)}}, 0, -1, -1, 10);
        silentTimeline.restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
            QVector<QVector<ClipInfo>>{{audioClip(silence)}}, 0, -1, -1, 10);
        const QVector<QVector<ClipInfo>> noAudioTracks;
        noAudioTimeline.restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
            noAudioTracks, 0, -1, -1, 10);

        auto makeTask = [&](const QString &preset, const QString &out, Timeline *tl) {
            batchexport::ExportTask t;
            t.projectPath = QStringLiteral("g9.veditor");
            t.outputPath = out;
            t.preset = preset;
            t.timeline = tl;
            t.source = batchexport::TaskSource::CurrentProject;
            t.width = t.height = 64;
            t.fps = 10.0;
            t.codec = QStringLiteral("mpeg4");
            t.videoCodec = QStringLiteral("mpeg4");
            t.videoBitrateKbps = 200;
            t.audioCodec = QStringLiteral("aac");
            t.audioBitrateKbps = 192;
            return t;
        };
        const QString silentOut = work.filePath("silent.mp4");
        const QString noAudioOut = work.filePath("noaudio.mp4");
        const QString goneOut = work.filePath("gone.mp4");
        batchexport::Queue q;
        QString silentId, noAudioId, goneId;
        bool finished = false;
        bool removed = false;
        if (fixture) {
            silentId = q.addTask(makeTask(QStringLiteral("g9-loud"), silentOut, &silentTimeline));
            noAudioId = q.addTask(makeTask(QStringLiteral("g9-loud"), noAudioOut, &noAudioTimeline));
            goneId = q.addTask(makeTask(QStringLiteral("g10-gone"), goneOut, &toneTimeline));
            // Delete the preset AFTER queueing, BEFORE dispatch.
            ExportConfig probe;
            removed = ExportUserPresets::remove(QStringLiteral("g10-gone"))
                && !ExportUserPresets::resolve(QStringLiteral("g10-gone"), &probe);
            QSignalSpy done(&q, &batchexport::Queue::queueFinished);
            q.start();
            finished = done.wait(180000) || !done.isEmpty();
        }
        batchexport::ExportTask silentTask, noAudioTask, goneTask;
        for (const auto &t : q.tasks()) {
            if (t.id == silentId) silentTask = t;
            else if (t.id == noAudioId) noAudioTask = t;
            else if (t.id == goneId) goneTask = t;
        }
        const bool silentFailed = silentTask.state == batchexport::TaskState::Failed
            && silentTask.message.contains(QStringLiteral("ラウドネス"));
        const bool noAudioFailed = noAudioTask.state == batchexport::TaskState::Failed
            && noAudioTask.message.contains(QStringLiteral("ラウドネス"));
        const bool noOutputs = !QFileInfo::exists(silentOut) && !QFileInfo::exists(noAudioOut);
        const QString silentTemp = silentTask.exportConfig.value("loudnessTempFile").toString();
        const QString noAudioTemp = noAudioTask.exportConfig.value("loudnessTempFile").toString();
        // The silent task reached the AAC step (a temp file was created, then deleted);
        // the audio-less task never created one.
        const bool tempsGone = !silentTemp.isEmpty() && !QFileInfo::exists(silentTemp)
            && (noAudioTemp.isEmpty() || !QFileInfo::exists(noAudioTemp));
        qInfo().noquote() << QStringLiteral(
            "[batchexport-e2e] G9 temps: silentTemp=%1 exists=%2 noAudioTemp=%3 exists=%4")
            .arg(silentTemp).arg(QFileInfo::exists(silentTemp))
            .arg(noAudioTemp).arg(!noAudioTemp.isEmpty() && QFileInfo::exists(noAudioTemp));
        qInfo().noquote() << QStringLiteral(
            "[batchexport-e2e] G9 values: fixture=%1 finished=%2 silentState=%3 silentMessage=%4 "
            "noAudioState=%5 noAudioMessage=%6 noOutputs=%7")
            .arg(fixture).arg(finished).arg(static_cast<int>(silentTask.state))
            .arg(silentTask.message).arg(static_cast<int>(noAudioTask.state))
            .arg(noAudioTask.message).arg(noOutputs);
        check(9, "queue fails unmeasurable loudness tasks with a reason",
              fixture && finished && silentFailed && noAudioFailed && noOutputs && tempsGone, werr);

        const bool goneDone = goneTask.state == batchexport::TaskState::Done;
        const double goneLufs = goneDone ? loudness::measureIntegratedLufs(goneOut)
            : std::numeric_limits<double>::quiet_NaN();
        const bool goneHit = std::isfinite(goneLufs) && std::fabs(goneLufs + 16.0) <= 1.0;
        const bool goneKeys = goneTask.exportConfig.value("autoLoudness").toBool(false)
            && nearlyEqual(goneTask.exportConfig.value("targetLufs").toDouble(0.0), -16.0);
        qInfo().noquote() << QStringLiteral(
            "[batchexport-e2e] G10 values: fixture=%1 removed=%2 finished=%3 goneState=%4 "
            "goneLufs=%5 goneHit=%6 goneKeys=%7 message=%8")
            .arg(fixture).arg(removed).arg(finished).arg(static_cast<int>(goneTask.state))
            .arg(goneLufs).arg(goneHit).arg(goneKeys).arg(goneTask.message);
        check(10, "preset deleted after queueing still normalizes",
              fixture && removed && finished && goneDone && goneHit && goneKeys, werr);
        if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
        else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    }

    // G11: the batch settings dialog offers the three built-ins (fixed order) and then
    // the saved user preset names.
    {
        QTemporaryDir presetDir;
        const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
        const QByteArray oldEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
        qputenv("VEDITOR_EXPORT_PRESET_DIR", presetDir.path().toUtf8());
        ExportUserPreset u1, u2;
        u1.name = QStringLiteral("g11-a");
        u2.name = QStringLiteral("g11-b");
        ExportUserPreset shadow, shadowCase;
        shadow.name = QStringLiteral("1080p");
        shadowCase.name = QStringLiteral("1080P");
        shadow.config.autoLoudness = shadowCase.config.autoLoudness = true;
        shadow.config.targetLufs = shadowCase.config.targetLufs = -16.0;
        const bool saved = presetDir.isValid()
            && ExportUserPresets::save(u1) && ExportUserPresets::save(u2)
            && ExportUserPresets::save(shadow) && ExportUserPresets::save(shadowCase);
        batchexport::ExportTask builtinTask = BatchExportDialog::makeFileProjectTask(
            QStringLiteral("g11.veditor"), QStringLiteral("g11.mp4"), currentSettings);
        const RenderJob builtinJob = batchexport::Queue::buildRenderJob(builtinTask);
        batchexport::Queue g11Queue;
        const QString g11Id = g11Queue.addTask(builtinTask);
        const bool queuedNoLoudness = g11Queue.tasks().size() == 1
            && !g11Queue.tasks().first().autoLoudness;
        const bool jobNoKeys = !builtinJob.exportConfig.contains("autoLoudness")
            && !builtinJob.exportConfig.contains("targetLufs");
        const QStringList choices = batchexport::presetChoices();
        const bool builtins = choices.size() >= 3
            && choices.mid(0, 3) == QStringList{QStringLiteral("1080p"),
                                                QStringLiteral("720p"), QStringLiteral("4K")};
        const bool once = std::count_if(choices.begin(), choices.end(), [](const QString &c) {
            return c.compare(QStringLiteral("1080p"), Qt::CaseInsensitive) == 0; }) == 1;
        const bool users = choices.indexOf(QStringLiteral("g11-a")) >= 3
            && choices.indexOf(QStringLiteral("g11-b")) >= 3;
        qInfo().noquote() << QStringLiteral(
            "[batchexport-e2e] G11 values: saved=%1 choices=%2 builtins=%3 users=%4 once1080p=%5 "
            "jobHasAutoLoudness=%6 jobHasTargetLufs=%7 queuedAutoLoudness=%8 id=%9")
            .arg(saved).arg(choices.join('|')).arg(builtins).arg(users).arg(once)
            .arg(builtinJob.exportConfig.contains("autoLoudness"))
            .arg(builtinJob.exportConfig.contains("targetLufs"))
            .arg(!queuedNoLoudness).arg(g11Id);
        check(11, "preset choices: built-ins first, then saved user presets",
              saved && builtins && users && once && jobNoKeys && queuedNoLoudness,
              QString());
        if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
        else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    }

    qInfo().noquote() << QStringLiteral(
        "[batchexport-e2e] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
