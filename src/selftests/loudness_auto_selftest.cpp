#include "../ExportDialog.h"
#include "../ExportUserPresets.h"
#include "../LoudnessMaster.h"
#include "../RenderQueue.h"
#include "../Timeline.h"
#include "../libavcore/AudioExtract.h"
#include "SelftestGate.h"
#include <QComboBox>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
bool writeVideo(const QString &path, QString &error)
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
    if (auto failure = encoder.open(request)) {
        error = QString::fromStdString(*failure);
        return false;
    }
    QImage frame(64, 64, QImage::Format_RGB888);
    frame.fill(Qt::blue);
    for (int i = 0; i < 40; ++i)
        if (!encoder.pushFrame(frame, i)) {
            error = QStringLiteral("fixture frame push failed at %1").arg(i);
            return false;
        }
    if (auto failure = encoder.finalize()) {
        error = QString::fromStdString(*failure);
        return false;
    }
    return true;
}

bool render(RenderJob job, QString &error)
{
    error.clear();
    RenderQueue queue;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool ok = false;
    QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
        [&](const QString &, bool success, const QString &message) {
            ok = success;
            error = message;
            loop.quit();
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("render timed out after 60000 ms");
        loop.quit();
    });
    queue.addJob(job);
    if (queue.jobs().isEmpty()) {
        if (error.isEmpty()) error = QStringLiteral("queue rejected job");
        return false;
    }
    timeout.start(60000);
    QTimer::singleShot(0, &queue, &RenderQueue::start);
    loop.exec();
    queue.stop();
    return ok;
}
}

int runLoudnessAutoSelftest()
{
    selftest::GateTally tally;
    const auto gate = [&](int n, bool ok) { tally.gate(n, ok); };
    QTemporaryDir temporary;
    ExportConfig settings;
    ExportDialog dialog(ProjectConfig{});
    const auto *toggle = dialog.findChild<QCheckBox *>("autoLoudness");
    const auto *target = dialog.findChild<QDoubleSpinBox *>("targetLufs");
    RenderJob off;
    settings.appendLoudnessSettings(off.exportConfig);
    off.exportConfig["loudnessGainDb"] = 2.25;
    RenderQueue::resetAutoLoudnessMeasurementCountForTest();
    const auto before = off.exportConfig;
    const bool offNoWork = loudnessauto::prepareJob(off).isEmpty()
        && RenderQueue::measureAutoLoudness(off.exportConfig, "missing.wav").isEmpty();
    gate(1, toggle && !toggle->isChecked() && target && target->value() == -14.0
        && !settings.autoLoudness && settings.targetLufs == -14.0 && offNoWork
        && RenderQueue::autoLoudnessMeasurementCountForTest() == 0
        && off.exportConfig == before
        && RenderQueue::buildLoudnessAudioFilterArgs(off.exportConfig["loudnessGainDb"].toDouble())
            == QStringList({"-af", "volume=2.25dB,alimiter=limit=0.98"}));

    const QString source = temporary.filePath("tone.wav");
    const QString video = temporary.filePath("video.mp4");
    std::vector<float> samples(48000 * 4 * 2);
    for (int i = 0; i < 48000 * 4; ++i) {
        const float value = static_cast<float>(0.08 * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 48000.0));
        samples[2 * i] = samples[2 * i + 1] = value;
    }
    QString error;
    const bool fixture = temporary.isValid()
        && libavcore::writeWavFloatInterleaved(source, samples, 48000, 2, &error)
        && writeVideo(video, error);
    const QString fixtureError = error;
    Timeline timeline;
    ClipInfo picture;
    picture.filePath = video;
    picture.duration = picture.outPoint = 4.0;
    ClipInfo audio;
    audio.filePath = source;
    audio.duration = audio.outPoint = 4.0;
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
        QVector<QVector<ClipInfo>>{{audio}}, 0, -1, -1, 10);
    settings.autoLoudness = true;
    RenderJob normal;
    normal.timeline = &timeline;
    normal.width = normal.height = 64;
    normal.codec = "mpeg4";
    normal.bitrateBps = 200000;
    normal.outputPath = temporary.filePath("normal.mp4");
    normal.exportConfig["fps"] = 10;
    normal.exportConfig["audioCodec"] = "aac";
    settings.appendLoudnessSettings(normal.exportConfig);
    const QString status = fixture ? loudnessauto::prepareJob(normal) : QString();
    QString renderError;
    const bool rendered = fixture && !status.isEmpty() && render(normal, renderError);
    const double result = rendered ? loudness::measureIntegratedLufs(normal.outputPath)
                                  : std::numeric_limits<double>::quiet_NaN();
    const bool resultFinite = std::isfinite(result);
    const bool targetMet = resultFinite && std::abs(result + 14.0) <= 1.0;
    std::fprintf(stderr, "[loudness-auto] G2 fixture=%d statusPresent=%d rendered=%d "
        "result=%g finite=%d targetMet=%d measured=%g gain=%g error=%s fixtureError=%s status=%s\n",
        fixture, !status.isEmpty(), rendered, result, resultFinite, targetMet,
        normal.exportConfig["measuredLufs"].toDouble(std::numeric_limits<double>::quiet_NaN()),
        normal.loudnessGainDb, qPrintable(renderError), qPrintable(fixtureError), qPrintable(status));
    gate(2, rendered && resultFinite && targetMet);

    std::fill(samples.begin(), samples.end(), 0.0f);
    const QString silence = temporary.filePath("silence.wav");
    bool skipped = libavcore::writeWavFloatInterleaved(silence, samples, 48000, 2, &error);
    // Silent audio, a genuine video-only file, and an undecodable file.
    const QString broken = temporary.filePath("broken.wav");
    QFile file(broken);
    if (file.open(QIODevice::WriteOnly)) { file.write("not an audio file"); file.close(); }
    for (const QString &path : QStringList{silence, video, broken}) {
        QJsonObject config;
        settings.appendLoudnessSettings(config);
        config["loudnessGainDb"] = 12.0;
        const QString message = RenderQueue::measureAutoLoudness(config, path);
        skipped = skipped && config["loudnessGainDb"].toDouble(-1.0) == 0.0
            && message.contains(QStringLiteral("スキップ"));
    }
    gate(3, fixture && skipped);

    RenderJob batch = normal;
    batch.outputPath = temporary.filePath("broadcast.mp4");
    settings.targetLufs = -23.0;
    settings.appendLoudnessSettings(batch.exportConfig);
    const QString batchStatus = fixture ? loudnessauto::prepareJob(batch) : QString();
    QString batchError;
    const bool batchRendered = fixture && !batchStatus.isEmpty() && render(batch, batchError);
    const double broadcast = batchRendered ? loudness::measureIntegratedLufs(batch.outputPath)
                                          : std::numeric_limits<double>::quiet_NaN();
    const bool broadcastFinite = std::isfinite(broadcast);
    const bool broadcastTargetMet = broadcastFinite && std::abs(broadcast + 23.0) <= 1.0;
    const double gainDelta = batch.loudnessGainDb - normal.loudnessGainDb;
    const bool gainDeltaMet = std::abs(gainDelta + 9.0) < 0.01;
    std::fprintf(stderr, "[loudness-auto] G4 fixture=%d statusPresent=%d batchRendered=%d "
        "broadcast=%g finite=%d targetMet=%d gainDeltaMet=%d gainDelta=%g "
        "normalGain=%g batchGain=%g measured=%g error=%s fixtureError=%s status=%s\n",
        fixture, !batchStatus.isEmpty(), batchRendered, broadcast, broadcastFinite,
        broadcastTargetMet, gainDeltaMet, gainDelta, normal.loudnessGainDb, batch.loudnessGainDb,
        batch.exportConfig["measuredLufs"].toDouble(std::numeric_limits<double>::quiet_NaN()),
        qPrintable(batchError), qPrintable(fixtureError), qPrintable(batchStatus));
    gate(4, batchRendered && broadcastFinite && broadcastTargetMet && gainDeltaMet);

    RenderQueue queue;
    queue.addJob(normal);
    queue.addJob(batch);
    const auto jobs = queue.jobs();
    ExportConfig normalConfig = settings;
    normalConfig.targetLufs = -14.0;
    ExportConfig batchConfig = settings;
    batchConfig.targetLufs = -23.0;
    const QJsonObject normalMapped = exportcfg::toJobJson(normalConfig);
    const QJsonObject batchMapped = exportcfg::toJobJson(batchConfig);
    const bool queued = jobs.size() == 2 && jobs[0].exportConfig["autoLoudness"].toBool()
        && jobs[1].exportConfig["autoLoudness"].toBool()
        && jobs[0].exportConfig["targetLufs"].toDouble() == -14.0
        && jobs[1].exportConfig["targetLufs"].toDouble() == -23.0;
    const bool mapped = normalMapped.value("autoLoudness").toBool()
        && batchMapped.value("autoLoudness").toBool()
        && normalMapped.value("targetLufs").toDouble() == -14.0
        && batchMapped.value("targetLufs").toDouble() == -23.0;
    // The removed text check counted prepareJob calls inside two private UI
    // handlers; calling those handlers would open dialogs. G2/G4 instead run
    // prepareJob on both target profiles and render their audio. The
    // handlers still call prepareJob themselves (outside the shared job
    // builder that export-cfg G3 covers), so a dropped call in one handler
    // is not detected by any gate.
    const double normalMeasured = normal.exportConfig.value("measuredLufs").toDouble(
        std::numeric_limits<double>::quiet_NaN());
    const double batchMeasured = batch.exportConfig.value("measuredLufs").toDouble(
        std::numeric_limits<double>::quiet_NaN());
    const bool normalGainSaved = normal.exportConfig.value("loudnessGainDb").toDouble()
        == normal.loudnessGainDb;
    const bool batchGainSaved = batch.exportConfig.value("loudnessGainDb").toDouble()
        == batch.loudnessGainDb;
    const bool bothPrepared = !status.isEmpty() && !batchStatus.isEmpty()
        && std::isfinite(normalMeasured) && std::isfinite(batchMeasured)
        && normalGainSaved && batchGainSaved;
    // addJob mirrors flat video fields into exportConfig, so compare the
    // measured audio fields that prepareJob supplied on each submitted job.
    const auto keptAudioState = [](const RenderJob &submitted, const RenderJob &prepared) {
        for (const char *key : {"autoLoudness", "targetLufs", "measuredLufs", "loudnessGainDb"})
            if (submitted.exportConfig.value(key) != prepared.exportConfig.value(key)) return false;
        return true;
    };
    const bool queuedPrepared = jobs.size() == 2
        && keptAudioState(jobs[0], normal) && keptAudioState(jobs[1], batch);
    std::fprintf(stderr, "[loudness-auto] G5 queued=%d mapped=%d bothPrepared=%d "
        "queuedPrepared=%d normalStatus=%d batchStatus=%d "
        "normalTarget=%g batchTarget=%g normalMeasured=%g batchMeasured=%g "
        "normalGain=%g batchGain=%g normalGainSaved=%d batchGainSaved=%d\n",
        queued, mapped, bothPrepared, queuedPrepared,
        !status.isEmpty(), !batchStatus.isEmpty(),
        normalMapped.value("targetLufs").toDouble(),
        batchMapped.value("targetLufs").toDouble(),
        normalMeasured, batchMeasured, normal.loudnessGainDb, batch.loudnessGainDb,
        normalGainSaved, batchGainSaved);
    gate(5, queued && mapped && bothPrepared && queuedPrepared);
    if (normal.projectFilePath != source) QFile::remove(normal.projectFilePath);
    if (batch.projectFilePath != source) QFile::remove(batch.projectFilePath);

    // G6: selecting a saved preset whose target is -14 / -23 / -16 LUFS puts the
    // loudness preset combo on 配信 / 放送 / 自由入力 (typing in the spin box
    // intentionally means 自由入力).
    {
        QTemporaryDir presetDir;
        const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
        const QByteArray oldEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
        bool saved = presetDir.isValid();
        int idx[3] = {-1, -1, -1};
        bool controls = false;
        if (saved) {
            qputenv("VEDITOR_EXPORT_PRESET_DIR", presetDir.path().toUtf8());
            const double targets[3] = {-14.0, -23.0, -16.0};
            for (int i = 0; i < 3; ++i) {
                ExportUserPreset preset;
                preset.name = QStringLiteral("g6-%1").arg(i);
                preset.config.autoLoudness = true;
                preset.config.targetLufs = targets[i];
                saved = saved && ExportUserPresets::save(preset);
            }
            ExportDialog g6Dialog(ProjectConfig{});
            auto *presetCombo = g6Dialog.findChild<QComboBox *>("preset");
            auto *loudnessCombo = g6Dialog.findChild<QComboBox *>("loudnessPreset");
            controls = presetCombo && loudnessCombo;
            for (int i = 0; controls && i < 3; ++i) {
                const int userIndex = presetCombo->findData(QStringLiteral("g6-%1").arg(i));
                if (userIndex < 0) { controls = false; break; }
                presetCombo->setCurrentIndex(userIndex);
                idx[i] = loudnessCombo->currentIndex();
            }
            if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
            else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
        }
        std::fprintf(stderr, "[loudness-auto] G6 saved=%d controls=%d idx14=%d idx23=%d idx16=%d\n",
            saved, controls, idx[0], idx[1], idx[2]);
        gate(6, saved && controls && idx[0] == 0 && idx[1] == 1 && idx[2] == 2);
    }
    return tally.finish("loudness-auto");
}
