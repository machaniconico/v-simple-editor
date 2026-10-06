#include "../AudioMixer.h"
#include "../BatchExportDialog.h"
#include "../BatchExportQueue.h"
#include "../RenderInPlace.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QProcess>
#include <QStandardPaths>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include "SelftestGate.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    selftest::GateTally tally;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        const std::string why = (QString::fromLatin1(name)
            + (detail.isEmpty() ? QString() : QStringLiteral(": ") + detail)).toStdString();
        tally.gate(gate, ok, why.c_str());
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
                // Queue an AAC source at -30 LUFS first (made by the queue itself). Since
                // US-802 the plain task (live timeline, loudness OFF) muxes the timeline's
                // own AAC mix instead of projectPath; either way its output carries
                // measurable audio the plain preset must leave far from -16.
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
        const QString silentTemp =
            silentTask.exportConfig.value("tempFiles").toArray().at(0).toString();
        const QString noAudioTemp =
            noAudioTask.exportConfig.value("tempFiles").toArray().at(0).toString();
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

    // G12: a user preset sharing its name with an ExportDialog built-in (first entry) and
    // auto loudness ON is offered by presetChoices() AND resolves into the job. Every offered
    // user-preset name with loudness ON resolves the same way (enumeration == resolution).
    {
        QTemporaryDir presetDir;
        const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
        const QByteArray oldEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
        qputenv("VEDITOR_EXPORT_PRESET_DIR", presetDir.path().toUtf8());
        const auto dialogPresets = ExportDialog::presets();
        const QString shadowName = dialogPresets.isEmpty() ? QString() : dialogPresets.first().name;
        ExportUserPreset shadowUser, plainUser, paddedUser;
        shadowUser.name = shadowName;
        paddedUser.name = QStringLiteral("  g12-padded ");
        paddedUser.config.autoLoudness = true;
        paddedUser.config.targetLufs = -16.0;
        plainUser.name = QStringLiteral("g12-plain");
        shadowUser.config.autoLoudness = plainUser.config.autoLoudness = true;
        shadowUser.config.targetLufs = plainUser.config.targetLufs = -16.0;
        const bool saved = presetDir.isValid() && !shadowName.isEmpty()
            && ExportUserPresets::save(shadowUser) && ExportUserPresets::save(plainUser)
            && ExportUserPresets::save(paddedUser);
        const QStringList choices = batchexport::presetChoices();
        const bool offered = choices.contains(shadowName) && choices.contains(paddedUser.name);
        bool allResolve = true;
        QStringList perName;
        for (const QString &name : choices) {
            const bool builtin = name == QStringLiteral("1080p") || name == QStringLiteral("720p")
                || name == QStringLiteral("4K");
            batchexport::ExportTask t;
            t.projectPath = QStringLiteral("g12.veditor");
            t.outputPath = QStringLiteral("g12.mp4");
            t.preset = name;
            t.source = batchexport::TaskSource::File;
            // Re-resolution path (loudnessResolved == false) and the addTask path.
            const RenderJob reJob = batchexport::Queue::buildRenderJob(t);
            batchexport::Queue q;
            q.addTask(t);
            const bool hasTask = q.tasks().size() == 1;
            const bool qTaskOn = hasTask && q.tasks().first().autoLoudness;
            const double qTaskLufs = hasTask ? q.tasks().first().targetLufs : 0.0;
            const QJsonObject qCfg = hasTask
                ? batchexport::Queue::buildRenderJob(q.tasks().first()).exportConfig : QJsonObject();
            const bool qJobOn = qCfg.value("autoLoudness").toBool(false);
            const double qJobLufs = qCfg.value("targetLufs").toDouble(0.0);
            const bool reOn = reJob.exportConfig.value("autoLoudness").toBool(false);
            const double reLufs = reJob.exportConfig.value("targetLufs").toDouble(0.0);
            const bool queued = qTaskOn && qJobOn && std::fabs(qJobLufs + 16.0) <= 1e-9
                && std::fabs(qTaskLufs + 16.0) <= 1e-9;
            const bool reResolved = reOn && std::fabs(reLufs + 16.0) <= 1e-9;
            perName << QStringLiteral("%1:builtin=%2 queuedJob(auto=%3,lufs=%4) reresolvedJob(auto=%5,lufs=%6)")
                .arg(name).arg(builtin).arg(qJobOn).arg(qJobLufs).arg(reOn).arg(reLufs);
            if (builtin) allResolve = allResolve && !qJobOn && !reOn && !qTaskOn;
            else allResolve = allResolve && queued && reResolved;
        }
        qInfo().noquote() << QStringLiteral(
            "[batchexport-e2e] G12 values: saved=%1 shadowName=%2 offered=%3 allResolve=%4 "
            "perName=%5")
            .arg(saved).arg(shadowName).arg(offered).arg(allResolve).arg(perName.join('|'));
        check(12, "user preset named like a dialog built-in resolves loudness",
              saved && offered && allResolve, QString());
        if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
        else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    }

    // G13-G17 (US-802): a live-timeline task with loudness OFF muxes the timeline's AAC
    // mix (two audio tracks, A1 gain -6 dB, fades) instead of the V1 clip's source audio;
    // loudness ON keeps prepareJob's mix; temporaries listed in exportConfig["tempFiles"]
    // are gone after success and failure; a task without a live timeline is unchanged.
    {
        QTemporaryDir presetDir;
        QTemporaryDir work;
        const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
        const QByteArray oldEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
        qputenv("VEDITOR_EXPORT_PRESET_DIR", presetDir.path().toUtf8());
        ExportUserPreset loud;
        loud.name = QStringLiteral("g14-loud");
        loud.config.autoLoudness = true;
        loud.config.targetLufs = -16.0;
        const bool saved = presetDir.isValid() && work.isValid()
            && ExportUserPresets::save(loud);

        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        auto writeTone = [](const QString &path, double hz, double amp, QString *err) {
            std::vector<float> samples(48000 * 4 * 2);
            for (int i = 0; i < 48000 * 4; ++i)
                samples[2 * i] = samples[2 * i + 1] = static_cast<float>(
                    amp * std::sin(2.0 * 3.141592653589793 * hz * i / 48000.0));
            return libavcore::writeWavFloatInterleaved(path, samples, 48000, 2, err);
        };
        // Decoded mono PCM length (seconds) and RMS (dBFS) of a file's first audio stream.
        auto decode = [&](const QString &media, const QString &tag, double *sec, double *rmsDb) {
            *sec = *rmsDb = std::numeric_limits<double>::quiet_NaN();
            const QString wavPath = work.filePath(tag + QStringLiteral("-decoded.wav"));
            std::vector<double> pcm;
            int rate = 0;
            QString err;
            if (!libavcore::extractAudioToWav(media, wavPath, 48000, &err)
                || !libavcore::readPcm16WavToMono(wavPath, pcm, rate, &err)
                || rate <= 0 || pcm.empty()) {
                std::fprintf(stderr, "[batchexport-e2e] decode %s failed: %s\n",
                             qPrintable(media), qPrintable(err));
                return;
            }
            double sum = 0.0;
            for (double v : pcm) sum += v * v;
            *sec = static_cast<double>(pcm.size()) / rate;
            *rmsDb = 20.0 * std::log10(std::max(1e-12, std::sqrt(sum / pcm.size())));
        };

        const QString video = work.filePath("g13-video.mp4");
        const QString v1Tone = work.filePath("g13-v1tone.wav");
        const QString v1Path = work.filePath("g13-v1.mp4");
        const QString a1Path = work.filePath("g13-a1.wav");
        const QString a2Path = work.filePath("g13-a2.wav");
        QString werr;
        bool fixture = false;
        if (saved && !ffmpeg.isEmpty()
            && writeG8Video(video) && writeTone(v1Tone, 440.0, 0.05, &werr)
            && writeTone(a1Path, 1000.0, 0.4, &werr) && writeTone(a2Path, 300.0, 0.3, &werr)) {
            QProcess mux;
            mux.start(ffmpeg, {QStringLiteral("-y"), QStringLiteral("-i"), video,
                               QStringLiteral("-i"), v1Tone, QStringLiteral("-map"),
                               QStringLiteral("0:v"), QStringLiteral("-map"), QStringLiteral("1:a"),
                               QStringLiteral("-c:v"), QStringLiteral("copy"),
                               QStringLiteral("-c:a"), QStringLiteral("aac"),
                               QStringLiteral("-shortest"), v1Path});
            fixture = mux.waitForFinished(60000) && mux.exitStatus() == QProcess::NormalExit
                && mux.exitCode() == 0 && QFileInfo::exists(v1Path);
            if (!fixture)
                werr = QString::fromUtf8(mux.readAllStandardError()).right(400);
        }
        double v1Sec = 0.0, v1RmsDb = 0.0;
        if (fixture)
            decode(v1Path, QStringLiteral("g13-v1"), &v1Sec, &v1RmsDb);

        AudioMixer mixer;
        Timeline tl;
        ClipInfo picture;
        picture.filePath = v1Path;
        picture.duration = picture.outPoint = 4.0;
        ClipInfo a1;
        a1.filePath = a1Path;
        a1.duration = a1.outPoint = 4.0;
        a1.trailOut.type = TransitionType::FadeOut;
        a1.trailOut.duration = 0.5;
        ClipInfo a2;
        a2.filePath = a2Path;
        a2.duration = a2.outPoint = 4.0;
        a2.leadIn.type = TransitionType::FadeIn;
        a2.leadIn.duration = 1.0;
        tl.restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
            QVector<QVector<ClipInfo>>{{a1}, {a2}}, 0, -1, -1, 10);
        tl.setAudioMixer(&mixer);
        mixer.setTrackGain(0, 0.5); // A1 -6 dB
        const double timelineSec = tl.totalDuration();
        const double fps = 10.0;

        auto makeTask = [&](const QString &preset, const QString &out, Timeline *timeline) {
            batchexport::ExportTask t;
            t.projectPath = QStringLiteral("g13.veditor");
            t.outputPath = out;
            t.preset = preset;
            t.timeline = timeline;
            t.source = timeline ? batchexport::TaskSource::CurrentProject
                                : batchexport::TaskSource::File;
            t.width = t.height = 64;
            t.fps = fps;
            t.codec = QStringLiteral("mpeg4");
            t.videoCodec = QStringLiteral("mpeg4");
            t.videoBitrateKbps = 200;
            t.audioCodec = QStringLiteral("aac");
            t.audioBitrateKbps = 192;
            return t;
        };
        struct Dispatch {
            bool seen = false;
            int mixCalls = -1;
            QString path;
            bool pathExisted = false;
            QStringList temps;
            bool tempsExisted = false;   // every non-WAV temp existed at dispatch (and there was one)
            int wavTemps = 0;            // loudness measurement WAVs registered in tempFiles
            bool wavsGone = true;        // ... and already removed by prepareJob
            double mixSec = std::numeric_limits<double>::quiet_NaN();
            double mixRmsDb = std::numeric_limits<double>::quiet_NaN();
            QJsonObject cfg;
            long long bitRate = -1;
        };
        const QString offOut = work.filePath("g13-off.mp4");
        const QString onOut = work.filePath("g14-on.mp4");
        const QString failOut = work.filePath("no-such-dir/g16-fail.mp4");
        batchexport::ExportTask nullTask = makeTask(QStringLiteral("720p"),
            work.filePath("g17-null.mp4"), nullptr);
        nullTask.projectPath = work.filePath("g17-other-project.veditor");
        // G22: noise audio (a tone would not make the AAC encoder spend its bitrate) on its own
        // timeline, queued twice: 128 kbps and the 192 default.
        const QString noisePath = work.filePath("g22-noise.wav");
        bool noiseOk = false;
        if (fixture) {
            std::vector<float> noise(48000 * 4 * 2);
            unsigned noiseState = 12345u;
            for (float &v : noise) {
                noiseState = noiseState * 1664525u + 1013904223u;
                v = (static_cast<float>(noiseState >> 8) / 8388608.0f - 1.0f) * 0.3f;
            }
            noiseOk = libavcore::writeWavFloatInterleaved(noisePath, noise, 48000, 2, &werr);
        }
        Timeline tl22;
        {
            ClipInfo noiseClip;
            noiseClip.filePath = noisePath;
            noiseClip.duration = noiseClip.outPoint = 4.0;
            tl22.restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
                QVector<QVector<ClipInfo>>{{noiseClip}}, 0, -1, -1, 10);
        }
        const auto probeBitRate = [](const QString &path) -> long long {
            AVFormatContext *format = nullptr;
            if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0)
                return -1;
            long long rate = -1;
            if (avformat_find_stream_info(format, nullptr) >= 0) {
                for (unsigned i = 0; i < format->nb_streams; ++i)
                    if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                        rate = format->streams[i]->codecpar->bit_rate;
            }
            avformat_close_input(&format);
            return rate;
        };
        batchexport::Queue q;
        QString offId, onId, failId, nullId, lowId, defId;
        if (fixture) {
            offId = q.addTask(makeTask(QStringLiteral("720p"), offOut, &tl));
            onId = q.addTask(makeTask(QStringLiteral("g14-loud"), onOut, &tl));
            failId = q.addTask(makeTask(QStringLiteral("720p"), failOut, &tl));
            if (noiseOk) {
                batchexport::ExportTask low = makeTask(QStringLiteral("720p"),
                    work.filePath("g22-128.mp4"), &tl22);
                low.audioBitrateKbps = 128;
                lowId = q.addTask(low);
                defId = q.addTask(makeTask(QStringLiteral("720p"),
                    work.filePath("g22-192.mp4"), &tl22));
            }
        }
        nullId = q.addTask(nullTask);
        QHash<QString, Dispatch> dispatched;
        int lastCalls = renderinplace::prepareAudioMixCallCountForTest();
        // processNext emits Running after building the job and before RenderQueue
        // takes it, so the dispatch-time intermediates can be inspected here.
        QObject::connect(&q, &batchexport::Queue::taskStateChanged, &q,
                         [&](const QString &id, batchexport::TaskState state) {
            if (state != batchexport::TaskState::Running)
                return;
            const int calls = renderinplace::prepareAudioMixCallCountForTest();
            Dispatch d;
            d.seen = true;
            d.mixCalls = calls - lastCalls;
            lastCalls = calls;
            for (const auto &t : q.tasks()) {
                if (t.id != id) continue;
                d.path = t.dispatchedProjectFilePath;
                d.cfg = t.exportConfig;
            }
            d.pathExisted = QFileInfo::exists(d.path);
            for (const QJsonValue &v : d.cfg.value("tempFiles").toArray())
                d.temps << v.toString();
            int otherTemps = 0;
            d.tempsExisted = true;
            for (const QString &temp : std::as_const(d.temps)) {
                if (temp.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)) {
                    ++d.wavTemps;
                    d.wavsGone = d.wavsGone && !QFileInfo::exists(temp);
                } else {
                    ++otherTemps;
                    d.tempsExisted = d.tempsExisted && QFileInfo::exists(temp);
                }
            }
            d.tempsExisted = d.tempsExisted && otherTemps > 0;
            if ((id == lowId || id == defId) && d.pathExisted)
                d.bitRate = probeBitRate(d.path);
            if (id == offId && d.pathExisted)
                decode(d.path, QStringLiteral("g13-mix"), &d.mixSec, &d.mixRmsDb);
            dispatched.insert(id, d);
        });
        QSignalSpy done(&q, &batchexport::Queue::queueFinished);
        q.start();
        const bool finished = done.wait(240000) || !done.isEmpty();
        QHash<QString, batchexport::ExportTask> finalTasks;
        for (const auto &t : q.tasks())
            finalTasks.insert(t.id, t);
        auto anyExists = [](const QStringList &paths) {
            for (const QString &p : paths)
                if (QFileInfo::exists(p)) return true;
            return false;
        };
        const Dispatch off = dispatched.value(offId);
        const Dispatch on = dispatched.value(onId);
        const Dispatch failD = dispatched.value(failId);
        const Dispatch nullD = dispatched.value(nullId);
        const batchexport::TaskState offState = finalTasks.value(offId).state;
        const batchexport::TaskState onState = finalTasks.value(onId).state;
        const batchexport::TaskState failState = finalTasks.value(failId).state;

        if (ffmpeg.isEmpty()) {
            for (int g = 13; g <= 16; ++g)
                tally.skip(g, "ffmpeg.exe not found on PATH (timeline audio mix needs it)");
        } else {
            double outSec = std::numeric_limits<double>::quiet_NaN();
            double outRmsDb = std::numeric_limits<double>::quiet_NaN();
            if (offState == batchexport::TaskState::Done)
                decode(offOut, QStringLiteral("g13-out"), &outSec, &outRmsDb);
            const bool g13 = fixture && finished && off.seen && off.mixCalls == 1
                && QFileInfo(off.path).suffix() == QStringLiteral("m4a")
                && off.pathExisted && off.path != v1Path
                && std::fabs(off.mixSec - timelineSec) <= 1.0 / fps
                && std::fabs(off.mixRmsDb - v1RmsDb) >= 1.0
                && offState == batchexport::TaskState::Done
                && std::fabs(outRmsDb - off.mixRmsDb) <= 1.0;
            std::fprintf(stderr,
                "[batchexport-e2e] G13 values: fixture=%d finished=%d seen=%d mixCalls=%d "
                "path=%s existed=%d v1=%s timelineSec=%.4f mixSec=%.4f v1Sec=%.4f "
                "mixRmsDb=%.3f v1RmsDb=%.3f offState=%d outSec=%.4f outRmsDb=%.3f err=%s\n",
                fixture, finished, off.seen, off.mixCalls, qPrintable(off.path),
                off.pathExisted, qPrintable(v1Path), timelineSec, off.mixSec, v1Sec,
                off.mixRmsDb, v1RmsDb, static_cast<int>(offState), outSec, outRmsDb,
                qPrintable(werr));
            check(13, "loudness-OFF live-timeline task muxes the timeline AAC mix", g13, werr);

            const bool g14 = fixture && on.seen && on.mixCalls == 0
                && !on.path.isEmpty() && on.temps.contains(on.path) && on.pathExisted
                && QFileInfo(on.path).suffix().compare(QStringLiteral("wav"),
                                                       Qt::CaseInsensitive) != 0
                && on.cfg.contains("loudnessGainDb");
            std::fprintf(stderr,
                "[batchexport-e2e] G14 values: seen=%d mixCalls=%d path=%s existed=%d "
                "temps=%s loudnessGainDb=%s onState=%d\n",
                on.seen, on.mixCalls, qPrintable(on.path), on.pathExisted,
                qPrintable(on.temps.join('|')),
                on.cfg.contains("loudnessGainDb")
                    ? qPrintable(QString::number(on.cfg.value("loudnessGainDb").toDouble()))
                    : "absent",
                static_cast<int>(onState));
            check(14, "loudness-ON task keeps prepareJob's AAC mix", g14);

            const bool g15 = fixture && finished
                && offState == batchexport::TaskState::Done
                && onState == batchexport::TaskState::Done
                && off.tempsExisted && on.tempsExisted
                && on.wavTemps == 1 && on.wavsGone   // loudness measurement WAV: registered + removed
                && !anyExists(off.temps) && !anyExists(on.temps);
            std::fprintf(stderr,
                "[batchexport-e2e] G15 values: fixture=%d finished=%d offState=%d onState=%d offTemps=%s "
                "existedAtDispatch=%d existNow=%d onTemps=%s existedAtDispatch=%d existNow=%d "
                "offWav=%d/%d onWav=%d/%d\n",
                fixture, finished, static_cast<int>(offState), static_cast<int>(onState),
                qPrintable(off.temps.join('|')), off.tempsExisted, anyExists(off.temps),
                qPrintable(on.temps.join('|')), on.tempsExisted, anyExists(on.temps),
                off.wavTemps, off.wavsGone, on.wavTemps, on.wavsGone);
            check(15, "tempFiles removed after successful tasks", g15);

            const bool g16 = fixture && finished
                && failState == batchexport::TaskState::Failed
                && failD.tempsExisted && !anyExists(failD.temps);
            std::fprintf(stderr,
                "[batchexport-e2e] G16 values: failState=%d message=%s temps=%s "
                "existedAtDispatch=%d existNow=%d\n",
                static_cast<int>(failState), qPrintable(finalTasks.value(failId).message),
                qPrintable(failD.temps.join('|')), failD.tempsExisted, anyExists(failD.temps));
            check(16, "tempFiles removed after a failed task", g16);
        }

        if (ffmpeg.isEmpty()) {
            tally.skip(22, "ffmpeg.exe not found on PATH (timeline audio mix needs it)");
        } else {
            const Dispatch low = dispatched.value(lowId);
            const Dispatch def = dispatched.value(defId);
            std::fprintf(stderr,
                "[batchexport-e2e] G22 values: noiseOk=%d lowSeen=%d defSeen=%d lowMixCalls=%d "
                "defMixCalls=%d lowRate=%lld defRate=%lld lowPath=%s\n",
                noiseOk, low.seen, def.seen, low.mixCalls, def.mixCalls, low.bitRate,
                def.bitRate, qPrintable(low.path));
            check(22, "batch task audioBitrateKbps=128 reaches the AAC mix intermediate",
                fixture && noiseOk && finished && low.seen && def.seen && low.mixCalls == 1
                && def.mixCalls == 1 && low.bitRate > 115000 && low.bitRate < 141000
                && def.bitRate > 170000 && low.bitRate < def.bitRate);
        }

        const bool g17 = finished && nullD.seen && nullD.mixCalls == 0
            && nullD.path == nullTask.projectPath && nullD.temps.isEmpty();
        std::fprintf(stderr,
            "[batchexport-e2e] G17 values: finished=%d seen=%d mixCalls=%d path=%s "
            "projectPath=%s temps=%s\n",
            finished, nullD.seen, nullD.mixCalls, qPrintable(nullD.path),
            qPrintable(nullTask.projectPath), qPrintable(nullD.temps.join('|')));
        check(17, "task without a live timeline keeps projectPath audio", g17);
        if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
        else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    }

    // G18-G21 (US-802 r1/r2): RenderQueue consumes each tempFiles path exactly once.
    // G18: clearAll() during a render while the temp file is held open (Windows refuses
    // the delete) still leaves nothing behind once the worker's completion runs.
    // G19: a path reused after its job completed is not deleted again by removeJob /
    // clearAll of that finished job.
    {
        QTemporaryDir work;
        const QString video = work.filePath("g18-video.mp4");
        const bool fixture = work.isValid() && writeG8Video(video);
        Timeline tl;
        ClipInfo picture;
        picture.filePath = video;
        picture.duration = picture.outPoint = 4.0;
        tl.restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
            QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
        auto makeJob = [&](const QString &out, const QString &temp) {
            batchexport::ExportTask t;
            t.projectPath = QStringLiteral("g18.veditor");
            t.outputPath = out;
            t.preset = QStringLiteral("720p");
            t.timeline = &tl;
            t.width = t.height = 64;
            t.fps = 10.0;
            t.codec = QStringLiteral("mpeg4");
            t.videoCodec = QStringLiteral("mpeg4");
            t.videoBitrateKbps = 200;
            t.audioCodec = QStringLiteral("aac");
            t.audioBitrateKbps = 192;
            RenderJob job = batchexport::Queue::buildRenderJob(t);
            RenderQueue::addTempFile(job.exportConfig, temp);
            return job;
        };
        auto writeFile = [](const QString &path, const QByteArray &bytes) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        auto readFile = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };

        // G18
        const QString busyTemp = work.filePath("g18-busy.m4a");
        bool startedRendering = false, existsAfterClear = false, goneAtEnd = false;
        bool lockedOpen = false;
        if (fixture && writeFile(busyTemp, QByteArrayLiteral("g18"))) {
            RenderQueue rq;
            rq.addJob(makeJob(work.filePath("g18-out.mp4"), busyTemp));
            rq.start();
            const RenderJob *current = rq.currentJob();
            startedRendering = current && current->status == RenderJobStatus::Rendering;
            QFile holder(busyTemp);
            lockedOpen = holder.open(QIODevice::ReadOnly);
            rq.clearAll(); // no event processing in between: the job is still in flight
            existsAfterClear = QFileInfo::exists(busyTemp);
            holder.close();
            goneAtEnd = QTest::qWaitFor([&]() { return !QFileInfo::exists(busyTemp); }, 30000);
        }
        std::fprintf(stderr,
            "[batchexport-e2e] G18 values: fixture=%d startedRendering=%d lockedOpen=%d "
            "existsAfterClearAll=%d goneAfterWorkerEnd=%d temp=%s\n",
            fixture, startedRendering, lockedOpen, existsAfterClear, goneAtEnd,
            qPrintable(busyTemp));
        if (fixture && lockedOpen && !existsAfterClear)
            tally.skip(18, "open file did not block deletion here; busy-delete path not exercised");
        else
            check(18, "temp file busy at clearAll is removed after the worker ends",
                  fixture && startedRendering && lockedOpen && existsAfterClear && goneAtEnd);

        // G19
        const QString tempA = work.filePath("g19-a.m4a");
        const QString tempB = work.filePath("g19-b.m4a");
        bool bothDone = false, consumed = false, keptAfterRemove = false, keptAfterClear = false;
        QString uuidA;
        if (fixture && writeFile(tempA, "a") && writeFile(tempB, "b")) {
            RenderQueue rq;
            RenderJob jobA = makeJob(work.filePath("g19-a.mp4"), tempA);
            RenderJob jobB = makeJob(work.filePath("g19-b.mp4"), tempB);
            uuidA = jobA.uuid;
            int completedOk = 0;
            QObject::connect(&rq, &RenderQueue::jobCompletedUuid, &rq,
                             [&](QString, bool success, QString) { completedOk += success ? 1 : 0; });
            QSignalSpy finishedSpy(&rq, &RenderQueue::queueFinished);
            rq.addJob(jobA);
            rq.addJob(jobB);
            rq.start();
            bothDone = (finishedSpy.wait(120000) || !finishedSpy.isEmpty()) && completedOk == 2;
            consumed = !QFileInfo::exists(tempA) && !QFileInfo::exists(tempB);
            // Unrelated files now occupy the consumed paths.
            writeFile(tempA, QByteArrayLiteral("unrelated-a"));
            writeFile(tempB, QByteArrayLiteral("unrelated-b"));
            rq.removeJob(uuidA);
            keptAfterRemove = readFile(tempA) == QByteArrayLiteral("unrelated-a");
            rq.clearAll();
            keptAfterClear = readFile(tempB) == QByteArrayLiteral("unrelated-b")
                && keptAfterRemove && QFileInfo::exists(tempA);
        }
        std::fprintf(stderr,
            "[batchexport-e2e] G19 values: fixture=%d bothDone=%d consumedAtJobEnd=%d "
            "keptAfterRemoveJob=%d keptAfterClearAll=%d tempA=%s tempB=%s\n",
            fixture, bothDone, consumed, keptAfterRemove, keptAfterClear,
            qPrintable(tempA), qPrintable(tempB));
        check(19, "a consumed temp path is never deleted twice",
              fixture && bothDone && consumed && keptAfterRemove && keptAfterClear);

        // G20: cancelCurrent() marks the in-flight job Cancelled at once; clearCompleted()
        // then discards it while the temp file is still open. The path must stay pending
        // and be removed when the worker's completion runs.
        const QString cancelTemp = work.filePath("g20-cancel.m4a");
        bool g20Started = false, g20Locked = false, g20Cancelled = false;
        bool g20Discarded = false, g20ExistsAfterClear = false, g20Gone = false;
        if (fixture && writeFile(cancelTemp, QByteArrayLiteral("g20"))) {
            RenderQueue rq;
            rq.addJob(makeJob(work.filePath("g20-out.mp4"), cancelTemp));
            rq.start();
            const RenderJob *current = rq.currentJob();
            g20Started = current && current->status == RenderJobStatus::Rendering;
            QFile holder(cancelTemp);
            g20Locked = holder.open(QIODevice::ReadOnly);
            rq.cancelCurrent();
            g20Cancelled = !rq.jobs().isEmpty()
                && rq.jobs().first().status == RenderJobStatus::Cancelled;
            rq.clearCompleted(); // still no event processing: the worker is in flight
            g20Discarded = rq.jobs().isEmpty();
            g20ExistsAfterClear = QFileInfo::exists(cancelTemp);
            holder.close();
            g20Gone = QTest::qWaitFor([&]() { return !QFileInfo::exists(cancelTemp); }, 30000);
        }
        std::fprintf(stderr,
            "[batchexport-e2e] G20 values: fixture=%d startedRendering=%d lockedOpen=%d "
            "cancelledBeforeWorkerEnd=%d discardedByClearCompleted=%d existsAfterClearCompleted=%d "
            "goneAfterWorkerEnd=%d temp=%s\n",
            fixture, g20Started, g20Locked, g20Cancelled, g20Discarded, g20ExistsAfterClear,
            g20Gone, qPrintable(cancelTemp));
        if (fixture && g20Locked && !g20ExistsAfterClear)
            tally.skip(20, "open file did not block deletion here; busy-delete path not exercised");
        else
            check(20, "cancel + clearCompleted leaves no temp file after the worker ends",
                  fixture && g20Started && g20Locked && g20Cancelled && g20Discarded
                      && g20ExistsAfterClear && g20Gone);

        // G21: tempFiles is a trust boundary. saveQueue does not persist it; a loaded
        // queue JSON naming a user file never gets that file deleted (also not on
        // destruction), while loadQueue still cleans the jobs it replaces. A path that
        // was never registered through addTempFile is ignored even if a job carries it.
        const QString userFile = work.filePath("g21-user-media.mp4");
        const QString strayFile = work.filePath("g21-unregistered.m4a");
        const QString replacedTemp = work.filePath("g21-replaced.m4a");
        const QString savedJson = work.filePath("g21-saved.json");
        const QString loadJson = work.filePath("g21-load.json");
        bool savedClean = false, replacedGone = false, loadedStripped = false;
        bool strayKept = false, userKept = false, wrote = false;
        if (fixture) {
            wrote = writeFile(userFile, QByteArrayLiteral("user-media"))
                && writeFile(strayFile, QByteArrayLiteral("stray"))
                && writeFile(replacedTemp, QByteArrayLiteral("replaced"));
            QJsonObject loadedJob;
            loadedJob["id"] = 1;
            loadedJob["uuid"] = QStringLiteral("g21-loaded");
            loadedJob["name"] = QStringLiteral("g21");
            loadedJob["projectFilePath"] = QStringLiteral("g21.veditor");
            loadedJob["outputPath"] = work.filePath("g21-out.mp4");
            QJsonObject loadedCfg;
            loadedCfg["tempFiles"] = QJsonArray{userFile};
            loadedJob["exportConfig"] = loadedCfg;
            loadedJob["status"] = 0;
            QJsonObject root;
            root["nextId"] = 2;
            root["jobs"] = QJsonArray{loadedJob};
            wrote = wrote && writeFile(loadJson, QJsonDocument(root).toJson());
            {
                RenderQueue rq;
                rq.addJob(makeJob(work.filePath("g21-pending.mp4"), replacedTemp));
                QJsonObject strayCfg;
                strayCfg["tempFiles"] = QJsonArray{strayFile};
                const int strayId = rq.addJob(QStringLiteral("g21-stray"),
                    QStringLiteral("g21.veditor"), work.filePath("g21-stray.mp4"), strayCfg);
                rq.removeJob(strayId);
                strayKept = readFile(strayFile) == QByteArrayLiteral("stray");
                savedClean = rq.saveQueue(savedJson)
                    && !readFile(savedJson).contains("tempFiles");
                const bool loaded = rq.loadQueue(loadJson);
                replacedGone = loaded && !QFileInfo::exists(replacedTemp);
                loadedStripped = loaded && rq.jobs().size() == 1
                    && !rq.jobs().first().exportConfig.contains("tempFiles");
            } // destructor discards the loaded job
            userKept = readFile(userFile) == QByteArrayLiteral("user-media");
        }
        std::fprintf(stderr,
            "[batchexport-e2e] G21 values: fixture=%d wrote=%d savedWithoutTempFiles=%d "
            "unregisteredKept=%d replacedJobTempGone=%d loadedStripped=%d "
            "userFileKeptAfterDestroy=%d user=%s\n",
            fixture, wrote, savedClean, strayKept, replacedGone, loadedStripped, userKept,
            qPrintable(userFile));
        check(21, "tempFiles never persisted, loaded or deleted without ownership",
              fixture && wrote && savedClean && strayKept && replacedGone
                  && loadedStripped && userKept);

        // G23: a job whose projectFilePath was swapped for the batch-OFF audio mix
        // (temp file) is saved with the project path it replaced, and the temp path
        // appears nowhere in the saved JSON. Detects: temp path persisted (old form).
        const QString g23Mix = work.filePath("g23-batch_mix.m4a");
        const QString g23Json = work.filePath("g23-saved.json");
        QString g23Saved;
        bool g23Wrote = false;
        if (fixture) {
            RenderQueue rq;
            RenderJob job = makeJob(work.filePath("g23-out.mp4"), QString());
            RenderQueue::addTempFile(job.exportConfig, g23Mix);
            RenderQueue::setIntermediateProjectFile(job, g23Mix);
            rq.addJob(job);
            g23Wrote = rq.saveQueue(g23Json);
            const QJsonArray jobs = QJsonDocument::fromJson(readFile(g23Json))
                                        .object().value("jobs").toArray();
            if (!jobs.isEmpty())
                g23Saved = jobs.first().toObject().value("projectFilePath").toString();
        }
        const bool g23NoTemp = !readFile(g23Json).contains("g23-batch_mix");
        // Legacy caller: temp input set directly, no recorded source. The path is kept
        // as is (previous behavior) rather than silently saved empty.
        const QString g23LegacyJson = work.filePath("g23-legacy.json");
        QString g23LegacySaved;
        if (fixture) {
            RenderQueue rq;
            RenderJob job = makeJob(work.filePath("g23-legacy-out.mp4"), QString());
            RenderQueue::addTempFile(job.exportConfig, g23Mix);
            job.projectFilePath = g23Mix;
            rq.addJob(job);
            rq.saveQueue(g23LegacyJson);
            const QJsonArray jobs = QJsonDocument::fromJson(readFile(g23LegacyJson))
                                        .object().value("jobs").toArray();
            if (!jobs.isEmpty())
                g23LegacySaved = jobs.first().toObject().value("projectFilePath").toString();
        }
        const bool g23LegacyKept = g23LegacySaved == g23Mix;
        std::fprintf(stderr,
            "[batchexport-e2e] G23 values: fixture=%d wrote=%d savedProjectFilePath=%s "
            "isOriginal=%d tempAbsentFromJson=%d legacySaved=%s legacyKept=%d\n",
            fixture, g23Wrote, qPrintable(g23Saved),
            g23Saved == QStringLiteral("g18.veditor"), g23NoTemp,
            qPrintable(g23LegacySaved), g23LegacyKept);
        check(23, "saveQueue persists the replaced project path, never the audio temp path",
              fixture && g23Wrote && g23Saved == QStringLiteral("g18.veditor") && g23NoTemp
                  && g23LegacyKept);
    }

    return tally.finish("batchexport-e2e");
}
