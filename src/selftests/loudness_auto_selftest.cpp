#include "../AudioExportMix.h"
#include "../ExportDialog.h"
#include "../ExportUserPresets.h"
#include "../LoudnessMaster.h"
#include "../MainWindow.h"
#include "../MobileExportDialog.h"
#include "../RenderInPlace.h"
#include "../RenderQueue.h"
#include "../Timeline.h"
#include "../libavcore/AudioExtract.h"
#include "SelftestGate.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTest>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {
bool writeVideo(const QString &path, QString &error, const QString &audioSourcePath = QString())
{
    libavcore::EncodeRequest request;
    request.outputPath = path.toStdString();
    request.audioSourcePath = audioSourcePath.toStdString();
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

// G7-G12 environment rule: only a missing ffmpeg.exe is an environment SKIP. A failed fixture
// (libavcore writes it) is a production failure and FAILs, whether ffmpeg exists or not.
enum class FixtureVerdict { Run, Skip, Fail };
FixtureVerdict fixtureVerdict(bool fixtureOk, bool ffmpegFound)
{
    if (!fixtureOk) return FixtureVerdict::Fail;
    return ffmpegFound ? FixtureVerdict::Run : FixtureVerdict::Skip;
}

// addJob on a fresh queue: jobsChanged (connected before the call) reads jobs() synchronously.
struct Submission {
    int failedCount = 0;
    QString error;
    bool sawJob = false;
    qsizetype jobsAfter = -1;
};
Submission submit(const RenderJob &job)
{
    Submission result;
    RenderQueue queue;
    QObject::connect(&queue, &RenderQueue::jobsChanged, &queue, [&]() {
        result.sawJob = result.sawJob || !queue.jobs().isEmpty();
    });
    QObject::connect(&queue, &RenderQueue::jobFailed, &queue, [&](int, const QString &error) {
        ++result.failedCount;
        result.error = error;
    });
    queue.addJob(job);
    result.jobsAfter = queue.jobs().size();
    queue.clear(); // an accepted job is discarded through removeTempFiles, never started
    return result;
}

// Peak of the first audio stream, decoded by libavcore (-1 when it cannot be read).
double audioPeak(const QString &media, const QString &wavPath)
{
    std::vector<double> mono;
    int rate = 0;
    if (!libavcore::extractAudioToWav(media, wavPath, 48000)
        || !libavcore::readPcm16WavToMono(wavPath, mono, rate) || mono.empty())
        return -1.0;
    double peak = 0.0;
    for (double value : mono) peak = std::max(peak, std::abs(value));
    return peak;
}

// The GUI entries' preparation (loudnessauto::prepareJobAsync), waited for on this thread.
RenderJob prepareAsync(RenderJob job)
{
    auto result = std::make_shared<std::optional<RenderJob>>();
    loudnessauto::prepareJobAsync(job, [result](RenderJob prepared, QString) {
        *result = std::move(prepared);
    }, [] { return false; });
    if (!QTest::qWaitFor([result] { return result->has_value(); }, 120000))
        std::fprintf(stderr, "[loudness-auto] prepareJobAsync did not finish within 120000 ms\n");
    return result->has_value() ? **result : job;
}

QStringList tempFilesOf(const RenderJob &job)
{
    QStringList paths;
    for (const QJsonValue &value : job.exportConfig.value("tempFiles").toArray())
        paths << value.toString();
    return paths;
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
    // handlers. G2/G4 run prepareJob on both target profiles and render their
    // audio; the normal / mobile entries run prepareJobAsync after
    // MainWindow::buildExportRenderJob (G14 / G15).
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

    // G7: when exportVideo's audio step (MainWindow::finalizeExportJobAudio) fails after
    // buildExportRenderJob -> prepareJobAsync already created intermediates, it must return
    // false and leave none behind. Detects removing that cleanup call (the temp file
    // would survive). The failure is forced via m_exportAudioMixErrorForTest; ffmpeg.exe
    // unavailable -> SKIP, a failed fixture -> FAIL (fixtureVerdict, G12).
    {
        MainWindow window;
        window.m_timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
            QVector<QVector<ClipInfo>>{{audio}}, 0, -1, -1, 10);
        ExportConfig g7Config = settings;
        g7Config.outputPath = temporary.filePath("g7.mp4");
        g7Config.autoLoudness = true;
        RenderJob g7 = prepareAsync(window.buildExportRenderJob(g7Config, MainWindow::ExportEntry::Normal));
        QStringList created;
        for (const QJsonValue &value : g7.exportConfig.value("tempFiles").toArray())
            created << value.toString();
        // The measurement WAV is registered but already removed by prepareJob; the AAC mux
        // file is the intermediate that must exist here.
        QStringList wavPaths;
        QStringList otherPaths;
        for (const QString &path : created)
            (path.endsWith(QStringLiteral(".m4a.wav")) ? wavPaths : otherPaths) << path;
        bool otherExists = !otherPaths.isEmpty();
        for (const QString &path : otherPaths) otherExists = otherExists && QFile::exists(path);
        const bool wavRegistered = wavPaths.size() == 1;
        const bool wavGone = wavRegistered && !QFile::exists(wavPaths.first());
        const bool createdExists = otherExists && wavRegistered && wavGone;
        const FixtureVerdict g7Verdict = fixtureVerdict(fixture,
            !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty());
        if (g7Verdict != FixtureVerdict::Run || !createdExists) {
            std::fprintf(stderr, "[loudness-auto] G7 fixture=%d created=%lld otherCount=%lld "
                "otherExists=%d wavRegistered=%d wavGone=%d createdExists=%d verdict=%d fixtureError=%s\n",
                fixture, static_cast<long long>(created.size()),
                static_cast<long long>(otherPaths.size()), otherExists, wavRegistered, wavGone,
                createdExists, static_cast<int>(g7Verdict), qPrintable(fixtureError));
            if (g7Verdict == FixtureVerdict::Skip) {
                tally.skip(7, "ffmpeg.exe unavailable");
            } else {
                tally.gate(7, false, g7Verdict == FixtureVerdict::Fail
                    ? "audio fixture generation (libavcore) failed"
                    : "prepareJob temp registration invalid: WAV not registered/removed or mux missing");
                RenderQueue q;
                q.removeTempFiles(&g7);
            }
        } else {
            window.m_exportAudioMixErrorForTest = QStringLiteral("forced mix failure");
            QString mixError;
            const bool finalized = window.finalizeExportJobAudio(g7, g7Config, &mixError);
            bool leftover = false;
            for (const QString &path : created) leftover = leftover || QFile::exists(path);
            std::fprintf(stderr, "[loudness-auto] G7 created=%lld otherCount=%lld otherExists=%d "
                "wavRegistered=%d wavGone=%d createdExists=%d finalized=%d "
                "error=%s leftover=%d tempFilesKey=%d\n",
                static_cast<long long>(created.size()), static_cast<long long>(otherPaths.size()),
                otherExists, wavRegistered, wavGone, createdExists, finalized,
                qPrintable(mixError), leftover, g7.exportConfig.contains("tempFiles"));
            tally.gate(7, !finalized && mixError == QStringLiteral("forced mix failure")
                && !leftover && !g7.exportConfig.contains("tempFiles"));
        }
    }
    // G8: prepareJob registers both intermediates, the PCM measurement WAV (<mix>.m4a.wav,
    // which it also removes itself) and the AAC mux file, in exportConfig["tempFiles"].
    // Detects dropping the WAV registration: removeTempFiles could then not retry a
    // failed remove. ffmpeg.exe unavailable -> SKIP, a failed fixture -> FAIL.
    {
        RenderJob g8 = normal;
        g8.exportConfig.remove("tempFiles");
        g8.exportConfig.remove("sourceProjectFilePath");
        g8.projectFilePath.clear();
        g8.outputPath = temporary.filePath("g8.mp4");
        settings.targetLufs = -14.0;
        settings.appendLoudnessSettings(g8.exportConfig);
        const bool g8Ffmpeg = !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();
        const QString g8Status = fixture ? loudnessauto::prepareJob(g8) : QString();
        QString wavPath, muxPath;
        for (const QJsonValue &value : g8.exportConfig.value("tempFiles").toArray()) {
            const QString path = value.toString();
            if (path.endsWith(QStringLiteral(".m4a.wav"))) wavPath = path;
            else if (path.endsWith(QStringLiteral(".m4a"))) muxPath = path;
        }
        const FixtureVerdict g8Verdict = fixtureVerdict(fixture, g8Ffmpeg);
        if (g8Verdict == FixtureVerdict::Skip) {
            tally.skip(8, "ffmpeg.exe unavailable");
        } else if (g8Verdict == FixtureVerdict::Fail) {
            std::fprintf(stderr, "[loudness-auto] G8 fixtureError=%s\n", qPrintable(fixtureError));
            tally.gate(8, false, "audio fixture generation (libavcore) failed");
        } else if (muxPath.isEmpty() || !QFile::exists(muxPath)) {
            std::fprintf(stderr, "[loudness-auto] G8 status=[%s] muxPath=[%s]\n",
                qPrintable(g8Status), qPrintable(muxPath));
            tally.gate(8, false, "prepareJob created no mux file although ffmpeg and the fixture exist");
        } else {
            const bool wavRegistered = !wavPath.isEmpty();
            const bool wavGone = wavRegistered && !QFile::exists(wavPath);
            const bool muxIsProject = g8.projectFilePath == muxPath;
            std::fprintf(stderr, "[loudness-auto] G8 wavPath=[%s] wavRegistered=%d wavExists=%d "
                "muxPath=[%s] muxExists=%d muxIsProject=%d\n", qPrintable(wavPath), wavRegistered,
                wavRegistered && QFile::exists(wavPath), qPrintable(muxPath), QFile::exists(muxPath),
                muxIsProject);
            tally.gate(8, wavRegistered && wavGone && muxIsProject);
            RenderQueue g8Queue;
            g8Queue.removeTempFiles(&g8);
        }
    }

    // G9-G11 share the timelines: tone = V1 video + audio clip (G7), avOnly = V1 video with an
    // audible audio stream and no audio clip (empty audio playback sequence).
    const bool ffmpegFound = !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();
    const QString avSource = temporary.filePath("av-tone.wav");
    const QString avVideo = temporary.filePath("av.mkv");
    QByteArray pcm(48000 * 4 * 2 * 2, Qt::Uninitialized);
    for (int i = 0; i < 48000 * 4 * 2; ++i) {
        const qint16 value = static_cast<qint16>(8000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * (i / 2) / 48000.0));
        pcm[2 * i] = static_cast<char>(value & 0xff);
        pcm[2 * i + 1] = static_cast<char>((value >> 8) & 0xff);
    }
    QString avError;
    const bool avFixture = temporary.isValid()
        && libavcore::writePcm16AsWav(avSource, pcm, 48000, 2, &avError)
        && writeVideo(avVideo, avError, avSource);
    ClipInfo avPicture = picture;
    avPicture.filePath = avVideo;
    const auto useTimeline = [&](MainWindow &window, bool withAudioClip) {
        window.m_timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{withAudioClip ? picture : avPicture}},
            withAudioClip ? QVector<QVector<ClipInfo>>{{audio}} : QVector<QVector<ClipInfo>>(1), 0, -1, -1, 10);
    };
    ExportConfig autoConfig = settings;
    autoConfig.autoLoudness = true;
    int outputSerial = 0;
    const auto buildJob = [&](MainWindow &window) {
        autoConfig.outputPath = temporary.filePath(QStringLiteral("g9-%1.mp4").arg(++outputSerial));
        return prepareAsync(window.buildExportRenderJob(autoConfig, MainWindow::ExportEntry::Normal));
    };

    // G9: ON + empty audio playback sequence (GUI normal entry): prepareJob puts a silent .m4a in
    // tempFiles and projectFilePath, so RenderQueue no longer muxes the V1 source audio. Detects
    // dropping the silent track (HEAD: projectFilePath stays the project / V1 audio).
    {
        const FixtureVerdict verdict = fixtureVerdict(avFixture, ffmpegFound);
        if (verdict == FixtureVerdict::Skip) {
            tally.skip(9, "ffmpeg.exe unavailable");
        } else if (verdict == FixtureVerdict::Fail) {
            std::fprintf(stderr, "[loudness-auto] G9 avFixtureError=%s\n", qPrintable(avError));
            tally.gate(9, false, "audio+video fixture generation (libavcore) failed");
        } else {
            MainWindow window;
            useTimeline(window, false);
            const qsizetype sequenceSize = window.m_timeline->computeAudioPlaybackSequence().size();
            const int mixBefore = window.exportAudioMixCallCountForTest;
            const int silentBefore = loudnessauto::silentMixCallCountForTest();
            RenderJob g9 = buildJob(window);
            const int mixDelta = window.exportAudioMixCallCountForTest - mixBefore;
            const int silentDelta = loudnessauto::silentMixCallCountForTest() - silentBefore;
            const QStringList temps = tempFilesOf(g9);
            const bool silentIsProject = g9.projectFilePath.endsWith(QStringLiteral(".m4a"))
                && temps.contains(g9.projectFilePath) && QFile::exists(g9.projectFilePath);
            const double silentPeak = silentIsProject
                ? audioPeak(g9.projectFilePath, temporary.filePath("g9-silent.wav")) : -1.0;
            const double sourcePeak = audioPeak(avVideo, temporary.filePath("g9-source.wav"));
            const QString autoError = g9.exportConfig.value("loudnessAutoError").toString();
            std::fprintf(stderr, "[loudness-auto] G9 sequenceSize=%lld projectFilePath=[%s] tempFiles=%lld "
                "silentIsProject=%d silentDelta=%d mixDelta=%d silentPeak=%g sourceV1Peak=%g autoError=[%s]\n",
                static_cast<long long>(sequenceSize), qPrintable(g9.projectFilePath),
                static_cast<long long>(temps.size()), silentIsProject, silentDelta, mixDelta, silentPeak,
                sourcePeak, qPrintable(autoError));
            tally.gate(9, sequenceSize == 0 && silentIsProject && silentDelta == 1 && mixDelta == 0
                && silentPeak == 0.0 && sourcePeak > 0.0 && autoError.isEmpty(),
                "ON + empty audio sequence did not put a silent track on the job (V1 source audio would be muxed)");
            RenderQueue cleanup;
            cleanup.removeTempFiles(&g9);
        }
    }

    // G10: a PCM mix failure rejects the job through RenderQueue::addJob (jobFailed, nothing
    // queued) instead of exporting unnormalized audio. Control: hook None, same input -> accepted.
    {
        const FixtureVerdict verdict = fixtureVerdict(fixture, ffmpegFound);
        if (verdict == FixtureVerdict::Skip) {
            tally.skip(10, "ffmpeg.exe unavailable");
        } else if (verdict == FixtureVerdict::Fail) {
            tally.gate(10, false, "audio fixture generation (libavcore) failed");
        } else {
            MainWindow window;
            useTimeline(window, true);
            loudnessauto::setForcedFailureForTest(loudnessauto::ForcedFailure::PcmMix);
            const RenderJob failing = buildJob(window);
            loudnessauto::setForcedFailureForTest(loudnessauto::ForcedFailure::None);
            const QString autoError = failing.exportConfig.value("loudnessAutoError").toString();
            const Submission rejected = submit(failing);
            const RenderJob control = buildJob(window);
            const QString controlError = control.exportConfig.value("loudnessAutoError").toString();
            const Submission accepted = submit(control);
            std::fprintf(stderr, "[loudness-auto] G10 error=[%s] failedCount=%d rejection=[%s] sawJob=%d "
                "jobsAfter=%lld | control error=[%s] failedCount=%d sawJob=%d jobsAfter=%lld\n",
                qPrintable(autoError), rejected.failedCount, qPrintable(rejected.error), rejected.sawJob,
                static_cast<long long>(rejected.jobsAfter), qPrintable(controlError), accepted.failedCount,
                accepted.sawJob, static_cast<long long>(accepted.jobsAfter));
            tally.gate(10, !autoError.isEmpty() && rejected.failedCount == 1 && rejected.error.contains(autoError)
                && !rejected.sawJob && rejected.jobsAfter == 0
                && controlError.isEmpty() && accepted.failedCount == 0 && accepted.sawJob && accepted.jobsAfter == 1,
                "a PCM mix failure did not reject the job (or the unforced control was rejected)");
        }
    }

    // G11: AAC conversion failure (tone timeline) and silent mix failure (G9 input) take the same
    // rejection path, and the measurement WAV / AAC / silent m4a they registered are gone after the
    // rejection's removeTempFiles. The forced failures run after the file was written.
    {
        const FixtureVerdict verdict = fixtureVerdict(fixture && avFixture, ffmpegFound);
        if (verdict == FixtureVerdict::Skip) {
            tally.skip(11, "ffmpeg.exe unavailable");
        } else if (verdict == FixtureVerdict::Fail) {
            tally.gate(11, false, "audio fixture generation (libavcore) failed");
        } else {
            bool ok = true;
            const std::pair<loudnessauto::ForcedFailure, bool> cases[] = {
                {loudnessauto::ForcedFailure::AacConvert, true}, {loudnessauto::ForcedFailure::SilentMix, false}};
            for (const auto &[failure, withAudioClip] : cases) {
                MainWindow window;
                useTimeline(window, withAudioClip);
                loudnessauto::setForcedFailureForTest(failure);
                const RenderJob failing = buildJob(window);
                loudnessauto::setForcedFailureForTest(loudnessauto::ForcedFailure::None);
                const QString autoError = failing.exportConfig.value("loudnessAutoError").toString();
                const QStringList temps = tempFilesOf(failing);
                QStringList written;
                for (const QString &path : temps)
                    if (path.endsWith(QStringLiteral(".m4a")) && QFile::exists(path)) written << path;
                const Submission rejected = submit(failing);
                QStringList leftover;
                for (const QString &path : temps)
                    if (QFile::exists(path)) leftover << path;
                const bool wavRegistered = !withAudioClip
                    || std::any_of(temps.cbegin(), temps.cend(), [](const QString &path) {
                           return path.endsWith(QStringLiteral(".m4a.wav")); });
                std::fprintf(stderr, "[loudness-auto] G11 case=%d error=[%s] failedCount=%d rejection=[%s] "
                    "sawJob=%d jobsAfter=%lld tempFiles=[%s] writtenM4a=%lld wavRegistered=%d leftover=[%s]\n",
                    static_cast<int>(failure), qPrintable(autoError), rejected.failedCount,
                    qPrintable(rejected.error), rejected.sawJob, static_cast<long long>(rejected.jobsAfter),
                    qPrintable(temps.join(QLatin1Char(','))), static_cast<long long>(written.size()),
                    wavRegistered, qPrintable(leftover.join(QLatin1Char(','))));
                ok = ok && !autoError.isEmpty() && rejected.failedCount == 1 && rejected.error.contains(autoError)
                    && !rejected.sawJob && rejected.jobsAfter == 0
                    && written.size() == 1 && wavRegistered && leftover.isEmpty();
            }
            tally.gate(11, ok, "an AAC / silent mix failure was not rejected or left a registered temp file");
        }
    }

    // G12: the G7-G11 environment rule. A fixture that libavcore fails to write (output directory
    // missing) takes the FAIL branch, not SKIP; only a missing ffmpeg.exe SKIPs. Detects folding a
    // fixture failure back into the environment SKIP.
    {
        QString g12Error;
        const bool g12Fixture = writeVideo(temporary.filePath("missing-dir/g12.mp4"), g12Error);
        const FixtureVerdict failed = fixtureVerdict(g12Fixture, true);
        const FixtureVerdict failedNoFfmpeg = fixtureVerdict(g12Fixture, false);
        const FixtureVerdict control = fixtureVerdict(true, true);
        const FixtureVerdict noFfmpeg = fixtureVerdict(true, false);
        std::fprintf(stderr, "[loudness-auto] G12 fixture=%d error=[%s] verdict=%d verdictNoFfmpeg=%d "
            "control=%d noFfmpeg=%d (0=Run 1=Skip 2=Fail)\n", g12Fixture, qPrintable(g12Error),
            static_cast<int>(failed), static_cast<int>(failedNoFfmpeg), static_cast<int>(control),
            static_cast<int>(noFfmpeg));
        tally.gate(12, !g12Fixture && !g12Error.isEmpty() && failed == FixtureVerdict::Fail
            && failedNoFfmpeg == FixtureVerdict::Fail && control == FixtureVerdict::Run
            && noFfmpeg == FixtureVerdict::Skip,
            "a failed fixture generation was classified as SKIP (or the environment rule changed)");
    }

    // G13: the export mix plan is a value snapshot. Per case (plain single-pass mix / track-FX
    // path), buildMixPlan runs once on the GUI thread, then the Timeline and the mixer are deleted
    // and the global channel-mode / reversed bindings cleared, and executeMixPlan runs under
    // QtConcurrent::run. Its WAV must equal the synchronous wrapper's WAV (renderinplace::
    // prepareAudioMix) byte for byte. Control: the plan a hidden read of the cleared bindings would
    // execute (all Stereo, none reversed) must differ, so the comparison cannot pass vacuously.
    {
        const QString lrSource = temporary.filePath("g13-lr.wav");
        std::vector<float> lr(48000 * 4 * 2);
        for (int i = 0; i < 48000 * 4; ++i) {
            lr[2 * i] = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 48000.0));
            lr[2 * i + 1] = static_cast<float>(0.1 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0));
        }
        QString lrError;
        const bool lrFixture = temporary.isValid()
            && libavcore::writeWavFloatInterleaved(lrSource, lr, 48000, 2, &lrError);
        const FixtureVerdict verdict = fixtureVerdict(fixture && lrFixture, ffmpegFound);
        if (verdict == FixtureVerdict::Skip) {
            tally.skip(13, "ffmpeg.exe unavailable");
        } else if (verdict == FixtureVerdict::Fail) {
            std::fprintf(stderr, "[loudness-auto] G13 fixtureError=%s lrError=%s\n",
                qPrintable(fixtureError), qPrintable(lrError));
            tally.gate(13, false, "audio fixture generation (libavcore) failed");
        } else {
            ClipInfo first;
            first.filePath = lrSource;
            first.duration = 4.0;
            first.outPoint = 1.5;
            first.volume = 0.8;
            first.audioChannelMode = AudioChannelMode::FillLeft;
            ClipInfo second;
            second.filePath = lrSource;
            second.duration = 4.0;
            second.inPoint = 1.0;
            second.outPoint = 2.5;
            second.reversed = true;
            second.pan = 0.4;
            const double durationSec = 3.0;
            const auto readAll = [](const QString &path) {
                QFile in(path);
                return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
            };
            const auto runAsync = [](const audioexport::MixPlan &plan, QThread **workerThread) {
                QFuture<std::pair<QString, QString>> future = QtConcurrent::run([plan, workerThread]() {
                    *workerThread = QThread::currentThread();
                    QString error;
                    const QString written = audioexport::executeMixPlan(plan, [] { return false; }, &error);
                    return std::make_pair(written, error);
                });
                // waitForFinished() may run a not-yet-started task on the waiting (GUI) thread;
                // polling isFinished() never steals it.
                const bool finished = QTest::qWaitFor([&future]() { return future.isFinished(); }, 120000);
                std::pair<QString, QString> result = future.result(); // the task writes *workerThread
                if (!finished) result.second = QStringLiteral("executeMixPlan ran past 120000 ms");
                return result;
            };
            bool ok = true;
            for (const bool trackFx : {false, true}) {
                const char *name = trackFx ? "trackfx" : "plain";
                auto *mixer = new AudioMixer;
                auto *planTimeline = new Timeline;
                planTimeline->setAudioMixer(mixer);
                // A clip pan != 0 takes the FX path since US-1114: only the trackfx case pans.
                ClipInfo panned = second;
                panned.pan = trackFx ? 0.4 : 0.0;
                planTimeline->restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
                    QVector<QVector<ClipInfo>>{{first, panned}}, 0, -1, -1, 10);
                if (trackFx) mixer->setTrackGain(0, 0.5);
                const QString syncPath = temporary.filePath(QStringLiteral("g13-%1-sync.wav").arg(name));
                const QString asyncPath = temporary.filePath(QStringLiteral("g13-%1-async.wav").arg(name));
                const QString controlPath = temporary.filePath(QStringLiteral("g13-%1-control.wav").arg(name));
                QString syncError;
                const bool syncOk = renderinplace::prepareAudioMix(planTimeline, syncPath, durationSec, &syncError)
                    == syncPath;
                const int guiBefore = audioexport::buildMixPlanGuiThreadCallsForTest();
                const int otherBefore = audioexport::buildMixPlanOtherThreadCallsForTest();
                const audioexport::MixPlan plan = audioexport::buildMixPlan(planTimeline, mixer,
                    {asyncPath, durationSec});
                delete planTimeline;
                delete mixer;
                setAudioChannelModePlaybackBindings({});
                setAudioReversedPlaybackBindings({});
                audioexport::MixPlan control = plan;
                control.outputPath = controlPath;
                control.channelModes.fill(AudioChannelMode::Stereo);
                control.reversed.fill(false);
                QThread *asyncThread = nullptr;
                QThread *controlThread = nullptr;
                const auto [asyncWritten, asyncError] = runAsync(plan, &asyncThread);
                const auto [controlWritten, controlError] = runAsync(control, &controlThread);
                const int guiDelta = audioexport::buildMixPlanGuiThreadCallsForTest() - guiBefore;
                const int otherDelta = audioexport::buildMixPlanOtherThreadCallsForTest() - otherBefore;
                const QByteArray syncBytes = readAll(syncPath);
                const QByteArray asyncBytes = readAll(asyncPath);
                const QByteArray controlBytes = readAll(controlPath);
                const bool offGuiThread = asyncThread && asyncThread != QThread::currentThread()
                    && controlThread && controlThread != QThread::currentThread();
                const bool planShape = plan.entries.size() == 2 && plan.channelModes.size() == 2
                    && plan.channelModes.contains(AudioChannelMode::FillLeft) && plan.reversed.contains(true)
                    && plan.useTrackFx == trackFx && !plan.passthrough;
                // >= 2.5 s of 48 kHz stereo pcm_s16le: the mix is not an empty / truncated file.
                const bool sized = syncBytes.size() >= 48000 * 2 * 2 * 5 / 2;
                const bool identical = syncOk && asyncWritten == asyncPath && asyncError.isEmpty() && sized
                    && asyncBytes == syncBytes;
                const bool controlDiffers = controlWritten == controlPath && controlBytes.size() >= 44
                    && controlBytes != syncBytes;
                std::fprintf(stderr, "[loudness-auto] G13 case=%s syncOk=%d syncBytes=%lld asyncBytes=%lld "
                    "identical=%d controlBytes=%lld controlDiffers=%d guiDelta=%d otherDelta=%d offGuiThread=%d "
                    "entries=%lld modes=[%d,%d] reversed=[%d,%d] useTrackFx=%d syncError=[%s] asyncError=[%s] "
                    "controlError=[%s]\n",
                    name, syncOk, static_cast<long long>(syncBytes.size()),
                    static_cast<long long>(asyncBytes.size()), identical,
                    static_cast<long long>(controlBytes.size()), controlDiffers, guiDelta, otherDelta,
                    offGuiThread, static_cast<long long>(plan.entries.size()),
                    static_cast<int>(plan.channelModes.value(0, AudioChannelMode::Stereo)),
                    static_cast<int>(plan.channelModes.value(1, AudioChannelMode::Stereo)),
                    plan.reversed.value(0, false), plan.reversed.value(1, false), plan.useTrackFx,
                    qPrintable(syncError), qPrintable(asyncError), qPrintable(controlError));
                ok = ok && planShape && identical && controlDiffers && guiDelta == 1 && otherDelta == 0
                    && offGuiThread;
            }
            tally.gate(13, ok, "executeMixPlan off the GUI thread after the Timeline was deleted did not "
                "reproduce the synchronous mix byte for byte (or the control / counters / plan shape failed)");
        }
    }

    // G14: the GUI normal and mobile entries prepare auto loudness off the GUI thread behind the
    // indeterminate "loudnessPrepareProgress" dialog. Right after the entry returns, the dialog
    // is shown and no job is queued yet (a synchronous prepare would already have queued it);
    // then done adds the prepared job: prepareJobCallCountForTest +1 and one jobsChanged with a
    // new uuid per job. Mobile: two exportRequested emissions give +2 and two queued jobs.
    // The two emissions model two separate dialog operations (one job per click), not a preset loop.
    // Detects the entries going back to a synchronous prepare (US-1105-mutation.txt).
    // G15: Cancel during the setPrepareDelayForTest delay queues no job, leaves nothing in the
    // export audio-mix temp directory and reports the cancel. Detects ignoring Cancel.
    {
        const FixtureVerdict verdict = fixtureVerdict(fixture, ffmpegFound);
        if (verdict != FixtureVerdict::Run) {
            for (const int n : {14, 15}) {
                if (verdict == FixtureVerdict::Skip) tally.skip(n, "ffmpeg.exe unavailable");
                else tally.gate(n, false, "audio fixture generation (libavcore) failed");
            }
        } else {
            // A failed export opens a modal QMessageBox from the entries' callbacks: close it and
            // fail the gate instead of hanging.
            QStringList modalTexts;
            QTimer modalGuard;
            QObject::connect(&modalGuard, &QTimer::timeout, &modalGuard, [&modalTexts]() {
                for (QWidget *widget : QApplication::topLevelWidgets()) {
                    auto *box = qobject_cast<QMessageBox *>(widget);
                    if (!box || !box->isVisible()) continue;
                    modalTexts << box->text();
                    box->done(0);
                }
            });
            modalGuard.start(200);
            ExportConfig asyncConfig = settings;
            asyncConfig.autoLoudness = true;
            asyncConfig.targetLufs = -14.0;
            asyncConfig.width = asyncConfig.height = 64;
            asyncConfig.fps = 10;
            asyncConfig.videoCodec = QStringLiteral("mpeg4");
            asyncConfig.videoBitrate = 200;
            const auto outputFor = [&](const char *name) {
                ExportConfig config = asyncConfig;
                config.outputPath = temporary.filePath(QString::fromLatin1(name));
                return config;
            };
            const QString dialogName = QStringLiteral("loudnessPrepareProgress");
            {
                MainWindow window;
                window.hide();
                useTimeline(window, true);
                RenderQueue *queue = window.ensureRenderQueue();
                QStringList addedUuids;
                int preparedAdded = 0;
                int addEvents = 0;
                QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                    bool fresh = false;
                    for (const RenderJob &job : queue->jobs()) {
                        if (addedUuids.contains(job.uuid)) continue;
                        addedUuids << job.uuid;
                        fresh = true;
                        if (job.exportConfig.contains("loudnessAutoStatus")
                            && !job.exportConfig.contains("loudnessAutoError")
                            && job.projectFilePath.endsWith(QStringLiteral(".m4a")))
                            ++preparedAdded;
                    }
                    if (fresh) ++addEvents;
                });
                const auto entryCase = [&](const char *name, int expected, const std::function<void()> &enter) {
                    const int countBefore = loudnessauto::prepareJobCallCountForTest();
                    const qsizetype uuidsBefore = addedUuids.size();
                    const int eventsBefore = addEvents;
                    const int preparedBefore = preparedAdded;
                    enter();
                    const int countAtReturn = loudnessauto::prepareJobCallCountForTest() - countBefore;
                    const QList<QProgressDialog *> dialogs = window.findChildren<QProgressDialog *>(dialogName);
                    bool shown = dialogs.size() == expected;
                    for (QProgressDialog *dialog : dialogs)
                        shown = shown && dialog->isVisible() && dialog->minimum() == 0 && dialog->maximum() == 0
                            && dialog->labelText() == QStringLiteral("音声を準備しています…");
                    const qsizetype queuedAtReturn = addedUuids.size() - uuidsBefore;
                    const bool settled = QTest::qWaitFor([&]() {
                        return addedUuids.size() - uuidsBefore >= expected
                            && window.findChildren<QProgressDialog *>(dialogName).isEmpty();
                    }, 120000);
                    const qsizetype added = addedUuids.size() - uuidsBefore;
                    const int events = addEvents - eventsBefore;
                    const int prepared = preparedAdded - preparedBefore;
                    std::fprintf(stderr, "[loudness-auto] G14 entry=%s countAtReturn=%d dialogs=%lld shown=%d "
                        "queuedAtReturn=%lld settled=%d added=%lld jobsChangedAdds=%d preparedAdded=%d "
                        "expected=%d\n", name, countAtReturn, static_cast<long long>(dialogs.size()), shown,
                        static_cast<long long>(queuedAtReturn), settled, static_cast<long long>(added), events,
                        prepared, expected);
                    return countAtReturn == expected && shown && queuedAtReturn == 0 && settled
                        && added == expected && events == expected && prepared == expected;
                };
                const bool normalOk = entryCase("normal", 1, [&]() {
                    window.submitNormalExport(outputFor("g14-normal.mp4"));
                });
                window.onMobileExport();
                auto *mobile = window.findChild<MobileExportDialog *>(QStringLiteral("mobileExportDialog"));
                if (mobile) mobile->hide();
                const bool mobileOk = mobile && entryCase("mobile", 2, [&]() {
                    emit mobile->exportRequested(outputFor("g14-mobile-a.mp4"));
                    emit mobile->exportRequested(outputFor("g14-mobile-b.mp4"));
                });
                const bool rendered = QTest::qWaitFor([&]() {
                    if (queue->isRunning()) return false;
                    for (const RenderJob &job : queue->jobs())
                        if (job.status == RenderJobStatus::Pending || job.status == RenderJobStatus::Rendering)
                            return false;
                    return true;
                }, 300000);
                int completed = 0;
                for (const RenderJob &job : queue->jobs())
                    if (job.status == RenderJobStatus::Completed) ++completed;
                std::fprintf(stderr, "[loudness-auto] G14 mobileDialog=%d normalOk=%d mobileOk=%d rendered=%d "
                    "completed=%d modal=[%s]\n", mobile != nullptr, normalOk, mobileOk, rendered, completed,
                    qPrintable(modalTexts.join(QLatin1Char('|'))));
                tally.gate(14, normalOk && mobileOk && rendered && completed == 3 && modalTexts.isEmpty(),
                    "an entry prepared synchronously (no dialog / job queued at return) or did not queue the "
                    "prepared job");
            }
            {
                MainWindow window;
                window.hide();
                useTimeline(window, true);
                RenderQueue *queue = window.ensureRenderQueue();
                int jobEvents = 0;
                QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                    if (!queue->jobs().isEmpty()) ++jobEvents;
                });
                const QDir mixDir(QDir::tempPath() + QStringLiteral("/v-simple-editor-audio-mix"));
                const QStringList filesBefore = mixDir.entryList(QDir::Files);
                audioexport::setPrepareDelayForTest(1500);
                const int countBefore = loudnessauto::prepareJobCallCountForTest();
                window.submitNormalExport(outputFor("g15.mp4"));
                QPointer<QProgressDialog> dialog = window.findChild<QProgressDialog *>(dialogName);
                const bool found = !dialog.isNull();
                if (dialog) emit dialog->canceled();
                const bool settled = QTest::qWaitFor([&dialog]() { return dialog.isNull(); }, 120000);
                // Boundary: done hides the dialog, calls deleteLater and then returns (cancel) or calls
                // next, which runs submitNormalExport -> queueNormalExport -> addJob synchronously in that
                // same call. Once dialog is null, any addJob (wanted or not) has already happened.
                audioexport::setPrepareDelayForTest(0);
                const int countDelta = loudnessauto::prepareJobCallCountForTest() - countBefore;
                QStringList leftover;
                for (const QString &file : mixDir.entryList(QDir::Files))
                    if (!filesBefore.contains(file)) leftover << file;
                const QString message = window.statusBar()->currentMessage();
                std::fprintf(stderr, "[loudness-auto] G15 found=%d settled=%d countDelta=%d jobEvents=%d "
                    "jobs=%lld leftover=[%s] status=[%s] modal=[%s]\n", found, settled, countDelta, jobEvents,
                    static_cast<long long>(queue->jobs().size()), qPrintable(leftover.join(QLatin1Char(','))),
                    qPrintable(message), qPrintable(modalTexts.join(QLatin1Char('|'))));
                tally.gate(15, found && settled && countDelta == 1 && jobEvents == 0 && queue->jobs().isEmpty()
                    && leftover.isEmpty() && message == QStringLiteral("音声の準備をキャンセルしました")
                    && modalTexts.isEmpty(),
                    "Cancel during the preparation delay still queued the job or left a temp file");
            }
        }
    }
    return tally.finish("loudness-auto");
}
