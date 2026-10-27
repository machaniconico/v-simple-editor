#include "LoudnessMaster.h"
#include "RenderQueue.h"
#include "SequenceExport.h"
#include "SubtitleIO.h"
#include <algorithm>
#include <memory>
#include <QTemporaryDir>
#include <atomic>
#include "TimelineFrameRenderer.h"
#include "ProjectFile.h"
#include "Timeline.h"
#include "AudioKeyframes.h"
#include "ExportRange.h"
#include "TrackMatteKey.h"
#include "CodecDetector.h"
#include "SmartRender.h"
#include "AcesColor.h"  // AC: production export 経路への ACES 適用 (8bit のみ)
#include "TimecodeBurnIn.h"
#include "clipanim/ClipAnim.h"
#include "color/ClipOdt.h"
#include "playback/hdrexport16_flag.h"
#include "playback/hdrmatte16_flag.h"
#include "libavcore/Concat.h"
#include "libavcore/Encode.h"
#include "libavcore/Probe.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUuid>
#include <QThread>
#include <QImage>
#include <QPainter>
#include <QVariant>
#include <QtGlobal>
// US-MF-6 / US-B3-7: the 10-bit HDR (HDR10/HLG) branch falls back to ffmpeg.exe
// whenever the loaded avcodec DLL ships no 10-bit HEVC encoder. These restore
// the pre-US-MF-5 subprocess render-pipe dependencies for that fallback branch.
#include <QProcess>
#include <QProcessEnvironment>
#include <QMutexLocker>
#include <QSet>
#include <QStandardPaths>
#include <cmath>  // std::round for the HDR10 master-display luminance scaling
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

void RenderQueue::appendSubtitleTrack(QJsonObject &config,
    const QVector<SubtitleSegment> &segments, const SubtitleStyle &style,
    const QObject *rendererOwner)
{
    Q_UNUSED(rendererOwner);
    // RenderQueue historically had no subtitle burn-in. Never discover legacy
    // Exporter renderers here: they can outlive the current project's subtitles.
    // Preserve only an explicit burn-in request, refreshing it from current data.
    const bool burnInRequested = config.contains("subtitleBurnInTrack");
    // An explicit empty track also prevents startRenderPipe() from reloading subtitles
    // from an older on-disk project after the user has deleted all current cues.
    config["subtitleTrack"] = QJsonObject{};
    config.remove("subtitleBurnInTrack");
    if (segments.isEmpty()) return;
    SubtitleTrackRenderer track;
    track.setSegments(segments);
    track.setStyle(style);
    config["subtitleTrack"] = track.toJson();
    if (burnInRequested) config["subtitleBurnInTrack"] = track.toJson();
}

std::vector<libavcore::SubtitleCue> RenderQueue::subtitleCues(
    const QJsonObject &config, qint64 startUs, qint64 durationUs)
{
    const auto mode = static_cast<subtitle::ExportMode>(config.value("subtitleMode").toInt(1));
    if (!subtitle::embeds(mode) || durationUs <= 0) return {};
    SubtitleTrackRenderer track;
    track.fromJson(config.value("subtitleTrack").toObject());
    std::vector<libavcore::SubtitleCue> clips;
    std::vector<int64_t> boundaries;
    for (const auto &segment : track.segments()) {
        if (!std::isfinite(segment.startTime) || !std::isfinite(segment.endTime)
            || segment.startTime < 0 || segment.endTime > 1e9 || segment.text.isEmpty()) continue;
        const int64_t begin = std::max<int64_t>(0, std::llround(segment.startTime * 1e6) - startUs);
        const int64_t end = std::min<int64_t>(durationUs, std::llround(segment.endTime * 1e6) - startUs);
        if (end <= begin) continue;
        clips.push_back({begin, end, segment.text.toUtf8().toStdString()});
        boundaries.push_back(begin);
        boundaries.push_back(end);
    }
    // tx3g has one active sample per track. Split overlaps and join their
    // text so simultaneous captions remain visible without invalid DTS.
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    std::vector<libavcore::SubtitleCue> result;
    for (size_t i = 1; i < boundaries.size(); ++i) {
        std::string text;
        for (const auto &clip : clips) {
            if (clip.startUs <= boundaries[i-1] && clip.endUs >= boundaries[i]) {
                if (!text.empty()) text += '\n';
                text += clip.text;
            }
        }
        if (text.empty()) continue;
        if (!result.empty() && result.back().endUs == boundaries[i-1] && result.back().text == text)
            result.back().endUs = boundaries[i];
        else result.push_back({boundaries[i-1], boundaries[i], text});
    }
    return result;
}

void RenderQueue::paintSubtitles(QImage &frame, const QJsonObject &config, qint64 timeUs)
{
    const auto mode = static_cast<subtitle::ExportMode>(config.value("subtitleMode").toInt(1));
    if (!subtitle::burns(mode)) return;
    QJsonObject data = config.value("subtitleBurnInTrack").toObject();
    if (data.isEmpty() && mode == subtitle::ExportMode::Both)
        data = config.value("subtitleTrack").toObject();
    if (data.isEmpty()) return;
    SubtitleTrackRenderer renderer;
    renderer.fromJson(data);
    QPainter painter(&frame);
    renderer.paintOnto(painter, QRectF(frame.rect()), timeUs / 1e6);
}

// Shared by the render paths and the export-meta selftest.
namespace renderqueue_metadata {
bool hasMetadata(const QJsonObject &cfg)
{
    return !cfg.value("title").toString().isEmpty()
        || !cfg.value("author").toString().isEmpty()
        || !cfg.value("copyright").toString().isEmpty()
        || !cfg.value("comment").toString().isEmpty();
}

void appendSubprocessArgs(QStringList &args, const QJsonObject &cfg,
                          const QString &outputPath)
{
    if (!hasMetadata(cfg))
        return;
    for (const char *key : {"title", "author", "copyright", "comment"}) {
        const QString name = QString::fromLatin1(key);
        const QString value = cfg.value(name).toString();
        if (!value.isEmpty())
            args << QStringLiteral("-metadata") << (name + QLatin1Char('=') + value);
    }
    // Match FrameEncoder: legacy MOV/MP4 atoms discard the author tag.
    const QString suffix = QFileInfo(outputPath).suffix().toLower();
    if (suffix == QLatin1String("mp4") || suffix == QLatin1String("mov"))
        args << QStringLiteral("-movflags") << QStringLiteral("use_metadata_tags");
}
} // namespace renderqueue_metadata

namespace {
QString makeUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool odtOwnsTonemapForExport(bool clipOdtEnabled,
                              bool export16Enabled,
                              bool matte16Enabled)
{
    return clipOdtEnabled && (export16Enabled || matte16Enabled);
}

bool shouldApplyExportAces(bool acesEnabled,
                           bool isHdr10,
                           bool isHlg,
                           bool isProRes,
                           bool odtOwnsTonemap)
{
    return acesEnabled && !isHdr10 && !isHlg && !isProRes && !odtOwnsTonemap;
}

std::optional<TimecodeBurnInRenderer> timecodeBurnInForRender(
    const QJsonObject &exportConfig,
    const Timeline *timeline)
{
    QJsonObject object;
    if (exportConfig.value(QStringLiteral("timecodeBurnIn")).isObject()) {
        object = exportConfig.value(QStringLiteral("timecodeBurnIn")).toObject();
    } else if (timeline) {
        object = timeline->property("timecodeBurnIn").value<QJsonObject>();
    }

    const TimecodeBurnInSettings settings =
        TimecodeBurnInSettings::fromJson(object);
    if (!settings.enabled)
        return std::nullopt;
    return TimecodeBurnInRenderer(settings);
}

bool tracksContainSequenceReference(const QVector<QVector<ClipInfo>> &tracks)
{
    for (const QVector<ClipInfo> &track : tracks) {
        for (const ClipInfo &clip : track) {
            if (clip.isSequenceReference())
                return true;
        }
    }
    return false;
}

bool timelineHasSequenceRenderModel(const QVector<QVector<ClipInfo>> &videoTracks,
                                    const QVector<QVector<ClipInfo>> &audioTracks)
{
    return tracksContainSequenceReference(videoTracks)
        || tracksContainSequenceReference(audioTracks);
}

bool timelineHasSequenceRenderModel(const Timeline *timeline)
{
    if (!timeline)
        return false;

    return timelineHasSequenceRenderModel(timeline->allVideoTracks(),
                                          timeline->allAudioTracks());
}

bool timelineHasProjectLighting(const Timeline *timeline)
{
    if (!timeline)
        return false;
    const QVector<Light3D> lights = timeline->projectLights();
    for (const Light3D &light : lights) {
        const Light3DState state = light.state();
        if (state.enabled && (state.intensity > 0.0 || light.hasAnimation()))
            return true;
    }
    return false;
}

// US-1504: the whole-timeline pass-through predicate (SmartRender) does not know
// the wiggle, so a timeline whose video clips move over time is encoded.
bool timelineHasDynamicTransform(const Timeline *timeline)
{
    if (!timeline)
        return false;
    const ProjectTrackClips videoTracks = timeline->allVideoTracks();
    for (const QVector<ClipInfo> &clips : videoTracks) {
        for (const ClipInfo &clip : clips) {
            if (clipanim::hasAnyDynamicTransform(clip))
                return true;
        }
    }
    return false;
}

struct FpsRational {
    int num = 30;
    int den = 1;
};

FpsRational deriveFpsRational(double fps)
{
    if (!std::isfinite(fps) || fps <= 0.0)
        return {30, 1};

    static constexpr double kEpsilon = 0.01;
    static constexpr int kNtscDen = 1001;
    static constexpr int kNtscNums[] = {
        24000, 30000, 48000, 60000, 120000
    };

    for (int num : kNtscNums) {
        const double ntscFps =
            static_cast<double>(num) / static_cast<double>(kNtscDen);
        if (std::fabs(fps - ntscFps) < kEpsilon)
            return {num, kNtscDen};
    }

    const double rounded = std::round(fps);
    if (rounded > 0.0
        && rounded <= static_cast<double>(std::numeric_limits<int>::max())
        && std::fabs(fps - rounded) < kEpsilon) {
        return {static_cast<int>(rounded), 1};
    }

    const AVRational q = av_d2q(fps, 1 << 16);
    if (q.num > 0 && q.den > 0)
        return {q.num, q.den};

    return {static_cast<int>(std::round(fps)), 1};
}

constexpr double kLoudnessGainEpsilonDb = 0.0005;

double sanitizeLoudnessGainDb(double gainDb)
{
    return std::isfinite(gainDb) ? gainDb : 0.0;
}

bool hasLoudnessGain(double gainDb)
{
    return std::fabs(sanitizeLoudnessGainDb(gainDb)) > kLoudnessGainEpsilonDb;
}

double configuredLoudnessGainDb(const QJsonObject &cfg, double fallbackGainDb)
{
    if (cfg.contains(QStringLiteral("loudnessGainDb"))) {
        return sanitizeLoudnessGainDb(
            cfg.value(QStringLiteral("loudnessGainDb")).toDouble(0.0));
    }
    return sanitizeLoudnessGainDb(fallbackGainDb);
}

QString ffmpegDbLiteral(double gainDb)
{
    QString out = QString::number(sanitizeLoudnessGainDb(gainDb), 'f', 6);
    while (out.contains(QLatin1Char('.')) && out.endsWith(QLatin1Char('0')))
        out.chop(1);
    if (out.endsWith(QLatin1Char('.')))
        out.chop(1);
    if (out == QLatin1String("-0"))
        out = QStringLiteral("0");
    return out;
}

QString loudnessPostprocessTempPath(const QString &outputPath)
{
    const QFileInfo info(outputPath);
    QString suffix = info.suffix();
    if (suffix.isEmpty())
        suffix = QStringLiteral("tmp");
    const QString fileName = QStringLiteral("%1.loudness.%2.%3.%4")
        .arg(info.completeBaseName(),
             QString::number(QCoreApplication::applicationPid()),
             QString::number(QDateTime::currentMSecsSinceEpoch()),
             suffix);
    return info.dir().filePath(fileName);
}

bool runLoudnessAudioPostprocess(const QString &ffmpegBin,
                                 const QString &outputPath,
                                 double gainDb,
                                 const QString &audioCodec,
                                 int audioBitrateKbps,
                                 bool copySubtitles,
                                 QString *error)
{
    const QStringList filterArgs =
        RenderQueue::buildLoudnessAudioFilterArgs(gainDb);
    if (filterArgs.isEmpty())
        return true;

    if (ffmpegBin.isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                "loudness normalization requires ffmpeg in PATH or alongside the app");
        }
        return false;
    }

    QString codec = audioCodec.trimmed();
    if (codec.isEmpty() || codec == QLatin1String("copy"))
        codec = QStringLiteral("aac");
    if (audioBitrateKbps <= 0)
        audioBitrateKbps = 192;

    const QString tempPath = loudnessPostprocessTempPath(outputPath);
    QStringList args;
    args << QStringLiteral("-y")
         << QStringLiteral("-hide_banner")
         << QStringLiteral("-i") << outputPath
         << QStringLiteral("-map") << QStringLiteral("0:v:0")
         << QStringLiteral("-map") << QStringLiteral("0:a?")
         << QStringLiteral("-c:v") << QStringLiteral("copy")
         << QStringLiteral("-c:a") << codec
         << QStringLiteral("-b:a")
         << QStringLiteral("%1k").arg(audioBitrateKbps);
    if (copySubtitles)
        args << QStringLiteral("-map") << QStringLiteral("0:s?")
             << QStringLiteral("-c:s") << QStringLiteral("copy");
    args << filterArgs;
    args << tempPath;

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(ffmpegBin, args);
    if (!proc.waitForStarted(15000)) {
        if (error)
            *error = QStringLiteral("failed to start ffmpeg: ") + proc.errorString();
        QFile::remove(tempPath);
        return false;
    }
    proc.waitForFinished(-1);
    const QByteArray stderrBytes = proc.readAllStandardError();
    const bool ok = proc.exitStatus() == QProcess::NormalExit
        && proc.exitCode() == 0
        && QFileInfo::exists(tempPath);
    if (!ok) {
        if (error) {
            *error = QStringLiteral("ffmpeg loudness normalization failed: ")
                + QString::fromUtf8(stderrBytes);
        }
        QFile::remove(tempPath);
        return false;
    }

    const QString backupPath = tempPath + QStringLiteral(".orig");
    QFile::remove(backupPath);
    if (!QFile::rename(outputPath, backupPath)) {
        if (error)
            *error = QStringLiteral("failed to replace unnormalized export");
        QFile::remove(tempPath);
        return false;
    }
    if (!QFile::rename(tempPath, outputPath)) {
        QFile::rename(backupPath, outputPath);
        if (error)
            *error = QStringLiteral("failed to move normalized export into place");
        QFile::remove(tempPath);
        return false;
    }
    QFile::remove(backupPath);
    return true;
}
} // namespace

int runRenderQueueAcesDecisionSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[renderqueue-aces] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    {
        const bool odtOwnsTonemap = odtOwnsTonemapForExport(true, false, false);
        check(1, "ODT-only keeps 8-bit ACES tonemap enabled",
              !odtOwnsTonemap
              && shouldApplyExportAces(true, false, false, false, odtOwnsTonemap));
    }

    {
        const bool odtOwnsTonemap = odtOwnsTonemapForExport(true, true, false);
        check(2, "ODT plus EXPORT16 suppresses 8-bit ACES re-apply",
              odtOwnsTonemap
              && !shouldApplyExportAces(true, false, false, false, odtOwnsTonemap));
    }

    {
        ClipInfo clip;
        clip.duration = 1.0;
        clip.outPoint = 1.0;
        clip.sequenceRefId = QStringLiteral("precomp-1");
        clip.filePath = timeline_nesting::sequenceClipFilePath(clip.sequenceRefId);
        QVector<QVector<ClipInfo>> tracks;
        tracks.append(QVector<ClipInfo>{clip});
        check(3, "sequenceRef clips trip the smart-render nest guard",
              tracksContainSequenceReference(tracks));
    }

    {
        ClipInfo clip;
        clip.duration = 1.0;
        clip.outPoint = 1.0;
        clip.filePath =
            timeline_nesting::sequenceClipFilePath(QStringLiteral("precomp-2"));
        QVector<QVector<ClipInfo>> tracks;
        tracks.append(QVector<ClipInfo>{clip});
        check(4, "sequence URI clips trip the smart-render nest guard",
              tracksContainSequenceReference(tracks));
    }

    {
        ClipInfo clip;
        clip.duration = 1.0;
        clip.outPoint = 1.0;
        clip.filePath = QStringLiteral("plain-source.mp4");
        QVector<QVector<ClipInfo>> videoTracks;
        videoTracks.append(QVector<ClipInfo>{clip});
        QVector<QVector<ClipInfo>> audioTracks;

        TimelineSequence active;
        active.id = QStringLiteral("main");
        active.name = QStringLiteral("Main");
        active.videoTracks = videoTracks;
        active.audioTracks = audioTracks;
        const QVector<TimelineSequence> sequenceStore{active};
        const bool sequenceModelEnabled = !sequenceStore.isEmpty();

        check(5, "enabled sequence model without sequenceRef clips leaves smart-render guard open",
              sequenceModelEnabled
              && !timelineHasSequenceRenderModel(videoTracks, audioTracks));
    }

    std::printf("[renderqueue-aces] summary: gates=5 passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}

int runRenderQueueFpsRationalSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, double fps, int expNum, int expDen) {
        const FpsRational got = deriveFpsRational(fps);
        const bool ok = (got.num == expNum && got.den == expDen);
        std::printf("[renderqueue-fps-rational] %s G%d fps=%.6f got=%d/%d expected=%d/%d\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    fps,
                    got.num,
                    got.den,
                    expNum,
                    expDen);
        ok ? ++passed : ++failed;
    };

    check(1, 29.97, 30000, 1001);
    check(2, 23.976, 24000, 1001);
    check(3, 59.94, 60000, 1001);
    check(4, 30.0, 30, 1);
    check(5, 25.0, 25, 1);
    check(6, 24.0, 24, 1);

    std::printf("[renderqueue-fps-rational] summary: gates=6 passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}

RenderQueue::RenderQueue(QObject *parent)
    : QObject(parent)
{
    setObjectName(QStringLiteral("mainRenderQueue"));
}

RenderQueue::~RenderQueue()
{
    m_cancelRequested = true;
    // US-MF-6: if the HDR (HDR10/HLG) subprocess branch is mid-encode, kill
    // the ffmpeg.exe process so the worker thread's stdin write unblocks and
    // the thread can be joined below.
    QProcess *process = nullptr;
    {
        QMutexLocker locker(&m_processMutex);
        process = m_process;
        if (process) {
            // US-101: drop the finished/readyRead handlers before killing so a
            // queued completion lambda cannot run against a half-destroyed queue.
            QObject::disconnect(process, nullptr, this, nullptr);
            process->kill();
        }
    }
    if (process)
        process->waitForFinished(3000);
    if (m_renderThread) {
        m_renderThread->wait(10000);
        delete m_renderThread;
        m_renderThread = nullptr;
    }
    while (!m_jobs.isEmpty())
        discardJob(m_jobs.size() - 1);
    removeTempFiles(nullptr);
}

// AC: production export 経路の ACES パイプラインの既定値を設定する。UI スレッドから
// addJob 前に呼ばれる想定。addJob が m_acesMutex 越しにジョブへ写す (投入済みの
// ジョブには届かない) ので、この代入と写しが同一 mutex で直列化される。
void RenderQueue::setAcesPipeline(const aces::AcesPipeline &p)
{
    QMutexLocker locker(&m_acesMutex);
    m_acesPipeline = p;
}

void RenderQueue::setLoudnessGainDb(double gainDb)
{
    m_loudnessGainDb = sanitizeLoudnessGainDb(gainDb);
}

bool RenderQueue::isProjectFilePath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("veditor")
        || suffix == QLatin1String("vsep")
        || suffix == QLatin1String("json");
}

namespace {
// Paths registered through addTempFile in this process: the only ones removeTempFiles
// may delete (a stray tempFiles entry from elsewhere is ignored).
QMutex ownedTempFilesMutex;
QSet<QString> ownedTempFiles;

// The project file a job without a live Timeline loads: projectFilePath, or the recorded source
// once setIntermediateProjectFile put an audio mix there (US-1210 RenderQueueDialog file jobs).
QString projectFileToLoad(const RenderJob &job)
{
    const QString source = job.exportConfig.value(QStringLiteral("sourceProjectFilePath")).toString();
    if (!job.timeline && !RenderQueue::isProjectFilePath(job.projectFilePath)
        && RenderQueue::isProjectFilePath(source))
        return source;
    return job.projectFilePath;
}
}

void RenderQueue::setIntermediateProjectFile(RenderJob &job, const QString &path)
{
    const QString key = QStringLiteral("sourceProjectFilePath");
    if (!job.exportConfig.contains(key))
        job.exportConfig[key] = job.projectFilePath;
    job.projectFilePath = path;
}

void RenderQueue::addTempFile(QJsonObject &config, const QString &path)
{
    if (path.isEmpty())
        return;
    {
        QMutexLocker locker(&ownedTempFilesMutex);
        ownedTempFiles.insert(path);
    }
    QJsonArray files = config.value(QStringLiteral("tempFiles")).toArray();
    files.append(path);
    config[QStringLiteral("tempFiles")] = files;
}

void RenderQueue::removeTempFiles(RenderJob *job)
{
    if (job) {
        const QJsonArray files = job->exportConfig.value(QStringLiteral("tempFiles")).toArray();
        QMutexLocker locker(&ownedTempFilesMutex);
        for (const QJsonValue &file : files) {
            const QString path = file.toString();
            if (path.isEmpty() || m_pendingTempFiles.contains(path))
                continue;
            if (ownedTempFiles.contains(path))
                m_pendingTempFiles.append(path);
            else
                qWarning().noquote() << "[render-queue] ignoring unregistered temp file" << path;
        }
        job->exportConfig.remove(QStringLiteral("tempFiles"));
    }
    QStringList stillPending;
    for (const QString &path : std::as_const(m_pendingTempFiles)) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            qWarning().noquote() << "[render-queue] temp file busy, retrying at next job end" << path;
            stillPending.append(path);
        } else {
            QMutexLocker locker(&ownedTempFilesMutex);
            ownedTempFiles.remove(path);
        }
    }
    m_pendingTempFiles = stillPending;
}

void RenderQueue::discardJob(int idx)
{
    removeTempFiles(&m_jobs[idx]);
    m_jobs.removeAt(idx);
    if (m_currentJobIndex > idx)
        m_currentJobIndex--;
    else if (m_currentJobIndex == idx)
        m_currentJobIndex = -1;
}

namespace {
std::atomic<int> autoLoudnessMeasurements{0};
}

void RenderQueue::resetAutoLoudnessMeasurementCountForTest()
{
    autoLoudnessMeasurements.store(0);
}

int RenderQueue::autoLoudnessMeasurementCountForTest()
{
    return autoLoudnessMeasurements.load();
}

QString RenderQueue::measureAutoLoudness(QJsonObject &config, const QString &mixPath)
{
    if (!config.value("autoLoudness").toBool(false)) return {};
    double measured = std::numeric_limits<double>::quiet_NaN();
    if (!mixPath.isEmpty()) {
        ++autoLoudnessMeasurements;
        measured = loudness::measureIntegratedLufs(mixPath);
    }
    const double target = config.value("targetLufs").toDouble(-14.0);
    const bool skip = std::isnan(measured) || !std::isfinite(measured)
        || measured <= -70.0 || !std::isfinite(target);
    const double gain = skip ? 0.0 : loudness::computeGainDb(measured, target);
    config["loudnessGainDb"] = gain;
    config.remove("measuredLufs");
    if (std::isfinite(measured)) config["measuredLufs"] = measured;
    const QString message = skip
        ? QStringLiteral("自動ラウドネス: 無音・音声なし・測定失敗のためスキップ（適用ゲイン 0 dB）")
        : QStringLiteral("自動ラウドネス: 測定 %1 LUFS / 目標 %2 LUFS / 適用ゲイン %3 dB")
            .arg(measured, 0, 'f', 2).arg(target, 0, 'f', 1).arg(gain, 0, 'f', 2);
    config["loudnessAutoStatus"] = message;
    if (skip) qWarning().noquote() << message;
    else qInfo().noquote() << message;
    return message;
}

QStringList RenderQueue::buildLoudnessAudioFilterArgs(double gainDb)
{
    if (!hasLoudnessGain(gainDb))
        return {};

    return {
        QStringLiteral("-af"),
        QStringLiteral("volume=%1dB,alimiter=limit=0.98")
            .arg(ffmpegDbLiteral(gainDb))
    };
}

namespace {
std::atomic<int> s_blackCompositeCount{0};
}
void RenderQueue::resetBlackCompositeCountForTest() { s_blackCompositeCount.store(0); }
int RenderQueue::blackCompositeCountForTest() { return s_blackCompositeCount.load(); }

QString RenderQueue::alphaExportError(const QJsonObject &config)
{
    const auto alpha = config.value("keepAlpha");
    if (!alpha.isUndefined() && !alpha.isBool())
        return QStringLiteral("keepAlpha は真偽値で指定してください");
    if (!alpha.toBool()) return {};
    if (config.value("imageSequence").toBool()) return {};
    const QString codec = config.value("videoCodec").toString();
    const auto profile = config.value("proresProfile");
    if (libavcore::alphaExportPixelFormat(codec.toStdString(), profile.toInt(-1)) == AV_PIX_FMT_NONE
        || config.value("audioOnly").toBool())
        return QStringLiteral("このコーデックはアルファを保持できません");
    return {};
}

int RenderQueue::addJob(const QString &name, const QString &projectFilePath,
                        const QString &outputPath, const QJsonObject &exportConfig)
{
    const QString alphaError = alphaExportError(exportConfig);
    if (!alphaError.isEmpty()) {
        emit jobFailed(-1, alphaError);
        return -1;
    }
    RenderJob job;
    job.id = m_nextId++;
    job.uuid = makeUuid();
    job.name = name;
    job.projectFilePath = projectFilePath;
    job.outputPath = outputPath;
    job.exportConfig = exportConfig;
    job.loudnessGainDb = configuredLoudnessGainDb(
        job.exportConfig, m_loudnessGainDb);
    if (hasLoudnessGain(job.loudnessGainDb))
        job.exportConfig[QStringLiteral("loudnessGainDb")] = job.loudnessGainDb;
    {
        QMutexLocker locker(&m_acesMutex);
        job.acesPipeline = m_acesPipeline;
    }
    job.status = RenderJobStatus::Pending;
    job.progress = 0;
    job.progressPercent = 0;

    // Mirror legacy exportConfig into flat fields so spec API can read them.
    job.width = exportConfig.value("width").toInt(1920);
    job.height = exportConfig.value("height").toInt(1080);
    job.codec = exportConfig.value("videoCodec").toString("h264");
    // exportConfig stored bitrate in kbps; flat field uses bps.
    job.bitrateBps = exportConfig.value("videoBitrate").toInt(10000) * 1000;
    job.preset = exportConfig.value("preset").toString();

    m_jobs.append(job);
    emit jobsChanged();
    return job.id;
}

void RenderQueue::addJob(const RenderJob &job)
{
    RenderJob copy = job;
    if (copy.uuid.isEmpty())
        copy.uuid = makeUuid();
    if (copy.id == 0)
        copy.id = m_nextId++;
    // The job keeps the pipeline current at submission; setAcesPipeline only sets
    // the default for later jobs. A caller-set pipeline is kept as is.
    if (!copy.acesPipeline) {
        QMutexLocker locker(&m_acesMutex);
        copy.acesPipeline = m_acesPipeline;
    }
    if (copy.status == RenderJobStatus::Pending && copy.progress == 0)
        copy.progress = copy.progressPercent;
    if (copy.errorMessage.isEmpty())
        copy.errorMessage = copy.error;

    // Mirror flat fields into exportConfig so the render-pipe encoder picks
    // them up without a separate code path.
    QJsonObject cfg = copy.exportConfig;
    const bool hadLoudnessGain =
        cfg.contains(QStringLiteral("loudnessGainDb"));
    copy.loudnessGainDb = configuredLoudnessGainDb(
        cfg,
        hasLoudnessGain(copy.loudnessGainDb)
            ? copy.loudnessGainDb
            : m_loudnessGainDb);
    cfg["width"] = copy.width;
    cfg["height"] = copy.height;
    cfg["videoCodec"] = mapCodecToEncoderName(copy.codec);
    cfg["videoBitrate"] = copy.bitrateBps / 1000;  // legacy config stores kbps
    if (!copy.preset.isEmpty())
        cfg["preset"] = copy.preset;
    if (hadLoudnessGain || hasLoudnessGain(copy.loudnessGainDb))
        cfg[QStringLiteral("loudnessGainDb")] = copy.loudnessGainDb;
    copy.exportConfig = cfg;
    // Keep the two rejection causes on separate channels: piggybacking the loudness
    // failure on alphaExportError() hid the alpha message whenever both applied.
    const QString alphaError = alphaExportError(cfg);
    const QString loudnessError = cfg.value("loudnessAutoError").toString();
    QString rejection = alphaError;
    if (!loudnessError.isEmpty())
        rejection = rejection.isEmpty() ? loudnessError
                                        : rejection + QLatin1Char('\n') + loudnessError;
    if (!rejection.isEmpty()) {
        removeTempFiles(&copy);
        emit jobFailed(copy.id, rejection);
        emit jobCompletedUuid(copy.uuid, false, rejection);
        return;
    }

    m_jobs.append(copy);
    emit jobsChanged();
}

void RenderQueue::removeJob(int id)
{
    int idx = findJobIndex(id);
    if (idx < 0)
        return;

    // Don't remove the currently rendering job
    if (m_jobs[idx].status == RenderJobStatus::Rendering)
        return;

    discardJob(idx);
    emit jobsChanged();
}

void RenderQueue::removeJob(const QString &uuid)
{
    int idx = findJobIndexByUuid(uuid);
    if (idx < 0)
        return;
    if (m_jobs[idx].status == RenderJobStatus::Rendering)
        return;

    discardJob(idx);
    emit jobsChanged();
}

void RenderQueue::cancelJob(const QString &uuid)
{
    const int idx = findJobIndexByUuid(uuid);
    if (idx < 0)
        return;
    if (m_jobs[idx].status == RenderJobStatus::Pending)
        removeJob(uuid);
    else if (m_jobs[idx].status == RenderJobStatus::Rendering && idx == m_currentJobIndex)
        cancelCurrent();
}

void RenderQueue::clear()
{
    clearAll();
}

void RenderQueue::start()
{
    startQueue();
}

void RenderQueue::stop()
{
    m_running = false;
    m_paused = false;
    cancelCurrent();
}

void RenderQueue::clearCompleted()
{
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        if (m_jobs[i].status == RenderJobStatus::Completed ||
            m_jobs[i].status == RenderJobStatus::Failed ||
            m_jobs[i].status == RenderJobStatus::Cancelled) {
            // A job cancelled mid-render is Cancelled before its worker ends.
            discardJob(i);
        }
    }
    emit jobsChanged();
}

void RenderQueue::clearAll()
{
    const bool hadRunningJob = m_running;
    m_running = false;
    m_paused = false;
    if (hadRunningJob)
        cancelCurrent();

    while (!m_jobs.isEmpty())
        discardJob(m_jobs.size() - 1);
    m_currentJobIndex = -1;
    emit jobsChanged();
}

void RenderQueue::startQueue()
{
    if (m_running)
        return;

    m_running = true;
    m_paused = false;
    startNextJob();
}

void RenderQueue::pauseQueue()
{
    m_paused = true;
    // Current job keeps running, but no new jobs will start
}

void RenderQueue::resumeQueue()
{
    if (!m_paused)
        return;

    m_paused = false;

    // If no job is currently rendering, start the next one
    if (m_currentJobIndex < 0 || m_jobs[m_currentJobIndex].status != RenderJobStatus::Rendering)
        startNextJob();
}

void RenderQueue::cancelCurrent()
{
    // US-101: guard the upper bound too, so a stale index cannot index m_jobs
    // out of range after the queue was cleared.
    if (m_currentJobIndex < 0 || m_currentJobIndex >= m_jobs.size())
        return;

    // Signal the S8 render-pipe worker (if running) to stop encoding frames;
    // it polls m_cancelRequested between frames exactly like
    // VideoStabilizer::m_cancelled.
    m_cancelRequested = true;

    // US-MF-6: if the 10-bit HDR subprocess branch is mid-encode, kill the
    // ffmpeg.exe process so a worker blocked inside a stdin write/flush
    // unblocks immediately rather than waiting for the next frame boundary.
    // Access m_process only under the mutex (it may be null for the in-process
    // render path, and a worker thread may reassign it).
    {
        QMutexLocker locker(&m_processMutex);
        if (m_process)
            m_process->kill();
    }

    // US-101: only flip a job to Cancelled when it is genuinely mid-render, and
    // notify listeners. No `!m_process` early-return: HEAD cancels the
    // in-process render-pipe worker via m_cancelRequested even when no ffmpeg
    // subprocess exists.
    RenderJob &j = m_jobs[m_currentJobIndex];
    if (j.status == RenderJobStatus::Rendering) {
        j.status = RenderJobStatus::Cancelled;
        j.endTime = QDateTime::currentDateTime();
        j.errorMessage = "Cancelled by user";
        j.error = j.errorMessage;
        emit jobsChanged();
    }

    // Don't auto-advance; let the render-pipe completion handler deal with it
}

const RenderJob *RenderQueue::currentJob() const
{
    if (m_currentJobIndex >= 0 && m_currentJobIndex < m_jobs.size() &&
        m_jobs[m_currentJobIndex].status == RenderJobStatus::Rendering) {
        return &m_jobs[m_currentJobIndex];
    }
    return nullptr;
}

int RenderQueue::pendingCount() const
{
    int count = 0;
    for (const auto &job : m_jobs) {
        if (job.status == RenderJobStatus::Pending)
            count++;
    }
    return count;
}

int RenderQueue::completedCount() const
{
    int count = 0;
    for (const auto &job : m_jobs) {
        if (job.status == RenderJobStatus::Completed)
            count++;
    }
    return count;
}

void RenderQueue::moveJobUp(int id)
{
    int idx = findJobIndex(id);
    if (idx <= 0)
        return;

    // Only reorder pending jobs
    if (m_jobs[idx].status != RenderJobStatus::Pending)
        return;

    m_jobs.swapItemsAt(idx, idx - 1);
    emit jobsChanged();
}

void RenderQueue::moveJobDown(int id)
{
    int idx = findJobIndex(id);
    if (idx < 0 || idx >= m_jobs.size() - 1)
        return;

    if (m_jobs[idx].status != RenderJobStatus::Pending)
        return;

    m_jobs.swapItemsAt(idx, idx + 1);
    emit jobsChanged();
}

void RenderQueue::moveJobUpUuid(const QString &uuid)
{
    int idx = findJobIndexByUuid(uuid);
    if (idx <= 0)
        return;
    if (m_jobs[idx].status != RenderJobStatus::Pending)
        return;
    m_jobs.swapItemsAt(idx, idx - 1);
    emit jobsChanged();
}

void RenderQueue::moveJobDownUuid(const QString &uuid)
{
    int idx = findJobIndexByUuid(uuid);
    if (idx < 0 || idx >= m_jobs.size() - 1)
        return;
    if (m_jobs[idx].status != RenderJobStatus::Pending)
        return;
    m_jobs.swapItemsAt(idx, idx + 1);
    emit jobsChanged();
}

int RenderQueue::totalEstimatedTime() const
{
    int totalSeconds = 0;
    for (const auto &job : m_jobs) {
        if (job.status != RenderJobStatus::Pending)
            continue;

        // Rough estimate: file size in MB * 2 seconds per MB
        QFileInfo info(job.projectFilePath);
        if (info.exists()) {
            int sizeMB = static_cast<int>(info.size() / (1024 * 1024));
            totalSeconds += qMax(sizeMB * 2, 10);  // minimum 10 seconds per job
        } else {
            totalSeconds += 30;  // default estimate
        }
    }
    return totalSeconds;
}

bool RenderQueue::saveQueue(const QString &filePath) const
{
    QJsonArray jobsArray;
    for (const auto &job : m_jobs) {
        QJsonObject obj;
        obj["id"] = job.id;                 // legacy int id
        obj["uuid"] = job.uuid;             // spec QString uuid
        obj["name"] = job.name;
        // An audio intermediate is process-local: persist the project path it replaced.
        // Without a recorded source (callers outside setIntermediateProjectFile) keep the
        // previous behavior of saving the path as is, and say so.
        const QString sourceKey = QStringLiteral("sourceProjectFilePath");
        if (job.exportConfig.contains(sourceKey)) {
            obj["projectFilePath"] = job.exportConfig.value(sourceKey).toString();
        } else {
            if (job.exportConfig.value(QStringLiteral("tempFiles")).toArray()
                    .contains(job.projectFilePath))
                qWarning().noquote() << "RenderQueue::saveQueue: job" << job.uuid
                                     << "uses a temporary input with no recorded source:"
                                     << job.projectFilePath;
            obj["projectFilePath"] = job.projectFilePath;
        }
        obj["outputPath"] = job.outputPath;
        obj["preset"] = job.preset;
        obj["width"] = job.width;
        obj["height"] = job.height;
        obj["codec"] = job.codec;
        obj["bitrateBps"] = job.bitrateBps;
        obj["startUs"] = static_cast<qint64>(job.startUs);
        obj["endUs"] = static_cast<qint64>(job.endUs);
        obj["passes"] = job.passes;
        QJsonObject exportConfig = job.exportConfig;
        if (!exportConfig.value("audioOnly").toBool(false))
            exportConfig.remove("audioOnly");
        if (exportConfig.value("rateControl").toString("bitrate") == "bitrate")
            exportConfig.remove("rateControl");
        if (exportConfig.value("crf").toInt(-1) == -1)
            exportConfig.remove("crf");
        exportConfig.remove(QStringLiteral("tempFiles")); // process-local, never persisted
        exportConfig.remove(sourceKey); // folded into projectFilePath above
        obj["exportConfig"] = exportConfig;
        obj["loudnessGainDb"] = job.loudnessGainDb;
        obj["status"] = static_cast<int>(job.status);
        obj["statusString"] = job.statusString();
        obj["progressPercent"] = job.progressPercent;
        obj["progress"] = job.progress;
        obj["error"] = job.error;
        obj["errorMessage"] = job.errorMessage;

        if (job.startTime.isValid())
            obj["startTime"] = job.startTime.toString(Qt::ISODate);
        if (job.endTime.isValid())
            obj["endTime"] = job.endTime.toString(Qt::ISODate);

        jobsArray.append(obj);
    }

    QJsonObject root;
    root["version"] = 2;
    root["nextId"] = m_nextId;
    root["jobs"] = jobsArray;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    file.write(QJsonDocument(root).toJson());
    return true;
}

bool RenderQueue::loadQueue(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    QJsonObject root = doc.object();
    m_nextId = root["nextId"].toInt(1);

    while (!m_jobs.isEmpty())
        discardJob(m_jobs.size() - 1);
    QJsonArray jobsArray = root["jobs"].toArray();
    for (const QJsonValue &val : jobsArray) {
        QJsonObject obj = val.toObject();
        RenderJob job;
        job.id = obj["id"].toInt();
        job.uuid = obj.value("uuid").toString(makeUuid());
        job.name = obj["name"].toString();
        job.projectFilePath = obj["projectFilePath"].toString();
        job.outputPath = obj["outputPath"].toString();
        job.preset = obj["preset"].toString();
        job.width = obj["width"].toInt(1920);
        job.height = obj["height"].toInt(1080);
        job.codec = obj["codec"].toString("h264");
        job.bitrateBps = obj["bitrateBps"].toInt(50000000);
        job.startUs = static_cast<qint64>(obj["startUs"].toDouble(0));
        job.endUs = static_cast<qint64>(obj["endUs"].toDouble(0));
        job.passes = obj["passes"].toInt(1);
        job.exportConfig = obj["exportConfig"].toObject();
        // Trust boundary: temp files belong to the process that created them.
        job.exportConfig.remove(QStringLiteral("tempFiles"));
        job.loudnessGainDb = configuredLoudnessGainDb(
            job.exportConfig,
            obj.value(QStringLiteral("loudnessGainDb")).toDouble(0.0));
        job.status = static_cast<RenderJobStatus>(obj["status"].toInt());
        job.progressPercent = obj["progressPercent"].toInt(obj["progress"].toInt());
        job.progress = job.progressPercent;
        job.error = obj["error"].toString(obj["errorMessage"].toString());
        job.errorMessage = job.error;

        if (obj.contains("startTime"))
            job.startTime = QDateTime::fromString(obj["startTime"].toString(), Qt::ISODate);
        if (obj.contains("endTime"))
            job.endTime = QDateTime::fromString(obj["endTime"].toString(), Qt::ISODate);
        {
            QMutexLocker locker(&m_acesMutex);
            job.acesPipeline = m_acesPipeline;
        }

        m_jobs.append(job);
    }

    emit jobsChanged();
    return true;
}

void RenderQueue::startNextJob()
{
    if (!m_running) {
        m_currentJobIndex = -1;
        return;
    }

    if (m_paused) {
        m_running = false;
        return;
    }

    // Find next pending job
    m_currentJobIndex = -1;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].status == RenderJobStatus::Pending) {
            m_currentJobIndex = i;
            break;
        }
    }

    if (m_currentJobIndex < 0) {
        m_running = false;
        emit queueFinished();
        emit allCompleted();
        return;
    }

    RenderJob &job = m_jobs[m_currentJobIndex];
    job.status = RenderJobStatus::Rendering;
    job.progress = 0;
    job.progressPercent = 0;
    job.startTime = QDateTime::currentDateTime();
    m_cancelRequested = false;

    emit jobStarted(job.id);
    emit jobsChanged();

    // S8: the export is no longer a source-file passthrough transcode. Drive
    // the SSOT render-pipe — every output frame comes from
    // tlrender::renderFrameAt so the entire edit graph (grade / FX / masks /
    // tracking / text / multi-track) reaches the exported file, with the
    // original audio muxed in. The public RenderJob/signal/queue-advance
    // contract is unchanged: jobProgress per frame, jobCompleted / jobFailed
    // on finish, cancellation honoured, and the next pending job is started
    // from finishCurrentJob().
    // US-1505: the job starts with the live Timeline's expression audio-level
    // snapshot current (UI thread); its render thread then reads the latest
    // published one per frame. A file-backed job has its own Timeline (level 0).
    if (job.timeline)
        job.timeline->ensureExpressionAudioEnvCurrent();
    startRenderPipe(m_currentJobIndex);
}

// S8/US-MF-5/US-MF-6/US-B3-7: 3-way export dispatcher.
// Frames always come from tlrender::renderFrameAt (the SSOT edit graph). The
// SINK depends on the job:
//   • 10-bit HDR (HDR10 / HLG) AND the loaded avcodec DLL exposes a 10-bit
//     HEVC encoder (libx265 / hevc_nvenc / hevc_qsv / hevc_amf — probed at
//     runtime via libavcore::tenBitHevcEncoderAvailable) → in-process
//     libavcore::FrameEncoder path, with request.videoCodecName overridden to
//     libavcore::firstTenBitHevcEncoder() and isHdr10/isHlg + hdrMaster*
//     populated. The encoder writes a genuine 10-bit HEVC yuv420p10le stream
//     with the BT.2020/PQ|HLG colour signalling FrameEncoder already wires up
//     (the same code path that handles main10 / HDR10 side-data was always
//     present — US-B3-7 just makes it reachable when the DLL supports it).
//   • 10-bit HDR but the DLL has no 10-bit HEVC encoder
//     (the bundled avcodec-62.dll: libx264/libx265 absent, h264_mf/hevc_mf
//     8-bit only) → startRenderPipeSubprocess: an ffmpeg.exe QProcess encode
//     (libx265 yuv420p10le + BT.2020/PQ|HLG metadata) as a runtime fallback,
//     identical to the pre-US-B3-7 behaviour.
//   • everything else (8-bit H.264 / H.265 / ProRes / AV1) → the in-process
//     libavcore::FrameEncoder path below, unchanged from US-MF-5.
//
// The 8-bit routing is unchanged: only HDR jobs consult
// tenBitHevcEncoderAvailable; an 8-bit job never visits the subprocess
// fallback. Drop-in replacing the bundled DLL with a libx265-enabled build
// flips HDR jobs to the in-process path automatically — no recompile needed.
void RenderQueue::startSequenceExport(int jobIndex)
{
    const RenderJob job = m_jobs[jobIndex];
    const auto &cfg = job.exportConfig;
    const auto settings = sequenceexport::Settings::fromJson(cfg, job.outputPath);
    QString error = alphaExportError(cfg);
    if (error.isEmpty()) error = sequenceexport::validate(settings);
    if (!error.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, error]() { finishCurrentJob(false, error); }, Qt::QueuedConnection);
        return;
    }
    if (m_renderThread) {
        m_renderThread->wait();
        delete m_renderThread;
        m_renderThread = nullptr;
    }
    Timeline *owned = nullptr;
    Timeline *timeline = resolveTimeline(job, &owned);
    const double fps = cfg.value("fps").toDouble(30.0);
    const QSize size(cfg.value("width").toInt(job.width), cfg.value("height").toInt(job.height));
    if (!timeline || !std::isfinite(fps) || fps <= 0 || size.isEmpty()) {
        delete owned;
        QMetaObject::invokeMethod(this, [this]() {
            finishCurrentJob(false, QStringLiteral("連番のタイムラインまたは出力設定が無効です"));
        }, Qt::QueuedConnection);
        return;
    }
    const qint64 total = static_cast<qint64>(timeline->totalDuration() * fps + 0.5);
    qint64 startUs = qMax(qint64(0), job.startUs);
    qint64 frames = qMax(qint64(0), total - static_cast<qint64>(startUs * fps / 1000000.0));
    if (cfg.value("exportMarkedRangeOnly").toBool()) {
        const auto range = computeExportRange(timeline->markedIn(), timeline->markedOut(),
            fps, total, true, timeline->hasMarkedRange());
        frames = range.frameCount();
        startUs = range.usedMarkedRange ? static_cast<qint64>(timeline->markedIn() * 1000000.0)
                                      : static_cast<qint64>(range.startFrame * 1000000.0 / fps);
    } else if (job.endUs > startUs) {
        frames = qMin(frames, static_cast<qint64>((job.endUs - startUs) * fps / 1000000.0 + 0.5));
    }
    const auto timecode = timecodeBurnInForRender(cfg, timeline);
    m_renderThread = QThread::create([this, job, settings, timeline, owned, size, fps, startUs, frames, timecode]() {
        const QString error = sequenceexport::write(timeline, settings, size, fps, startUs, frames,
            [this]() { return m_cancelRequested.load(); },
            [this, job](int percent) {
                emit jobProgress(job.id, qMin(percent, 99));
                emit jobProgressUuid(job.uuid, qMin(percent, 99));
            },
            [timeline, timecode, fps, size](QImage &frame, qint64 usec) {
                if (!timecode.has_value()) return;
                QPainter painter(&frame);
                const double sec = usec / 1000000.0;
                timecode->paintOnto(painter, QRectF(QPointF(0, 0), QSizeF(size)), sec, fps,
                                   timecodeBurnInClipNameAt(timeline, sec));
            });
        QMetaObject::invokeMethod(this, [this, owned, error]() {
            delete owned;
            finishCurrentJob(error.isEmpty(), error);
        }, Qt::QueuedConnection);
    });
    m_renderThread->start();
}

void RenderQueue::startRenderPipe(int jobIndex)
{
    auto &subtitleJob = m_jobs[jobIndex];
    const auto subtitleMode = static_cast<subtitle::ExportMode>(subtitleJob.exportConfig.value("subtitleMode").toInt(1));
    if (subtitle::embeds(subtitleMode)
        && (!subtitle::supportsMovText(QFileInfo(subtitleJob.outputPath).suffix())
            || subtitleJob.exportConfig.value("imageSequence").toBool(false))) {
        QMetaObject::invokeMethod(this, [this]() {
            finishCurrentJob(false, QStringLiteral("mov_text 字幕は MP4 / MOV 動画のみ対応しています"));
        }, Qt::QueuedConnection);
        return;
    }

    if (m_jobs[jobIndex].exportConfig.value("imageSequence").toBool()
        && sequenceexport::enabledForTest()) {
        startSequenceExport(jobIndex);
        return;
    }
    if (subtitle::embeds(subtitleMode)
        && !subtitleJob.exportConfig.contains("subtitleTrack")
        && isProjectFilePath(projectFileToLoad(subtitleJob))) {
        ProjectData data;
        if (ProjectFile::load(projectFileToLoad(subtitleJob), data)) {
            QVector<SubtitleSegment> segments;
            for (const auto &entry : data.subtitleSegments) {
                SubtitleSegment segment;
                segment.startTime = entry.start;
                segment.endTime = entry.end;
                segment.text = entry.text;
                segments.append(segment);
            }
            SubtitleStyle style;
            if (!data.subtitleStyle.isEmpty()) {
                style.font.setFamily(data.subtitleStyle.value("fontName").toString());
                style.font.setPointSize(data.subtitleStyle.value("fontSize").toInt(28));
                style.font.setBold(data.subtitleStyle.value("fontBold").toBool(true));
                style.color = QColor(data.subtitleStyle.value("color").toString("#ffffffff"));
                style.outlineColor = QColor(data.subtitleStyle.value("outlineColor").toString("#ff000000"));
                style.outlineWidth = data.subtitleStyle.value("outlineWidth").toDouble(2.0);
                style.verticalPos = data.subtitleStyle.value("verticalPos").toDouble(0.85);
            }
            appendSubtitleTrack(subtitleJob.exportConfig, segments, style, nullptr);
        }
    }
    const RenderJob jobCopy = m_jobs[jobIndex];
    const QString alphaError = alphaExportError(jobCopy.exportConfig);
    if (!alphaError.isEmpty()) {
        finishCurrentJob(false, alphaError);
        return;
    }

    // US-MF-6 / US-B3-7 dispatch: decide HDR10/HLG from the SAME job-config
    // values the in-process branch uses for request.isHdr10 / request.isHlg,
    // so the two branches agree on what "10-bit HDR" means. ProRes is never
    // HDR here (mirrors the in-process `!isProRes` guard below). When the
    // loaded avcodec DLL exposes a 10-bit HEVC encoder we keep the in-process
    // path; otherwise we fall through to startRenderPipeSubprocess.
    {
        const QJsonObject &cfg = jobCopy.exportConfig;
        QString dispatchCodec = cfg.value("videoCodec").toString();
        if (dispatchCodec.isEmpty()) {
            dispatchCodec = mapCodecToEncoderName(jobCopy.codec.isEmpty()
                ? QStringLiteral("h264") : jobCopy.codec);
        }
        const bool dispatchIsProRes =
            (dispatchCodec == QLatin1String("prores_ks")
             || dispatchCodec == QLatin1String("prores"));
        const QString dispatchHdrMode =
            cfg.value("hdrMode").toString().toLower();
        const bool dispatchIsHdr10 = !dispatchIsProRes
            && (dispatchHdrMode == QLatin1String("hdr10")
                || cfg.value("hdr10").toBool());
        const bool dispatchIsHlg =
            !dispatchIsProRes && dispatchHdrMode == QLatin1String("hlg");
        if (dispatchIsHdr10 || dispatchIsHlg) {
            // US-B3-7: route HDR jobs to subprocess fallback only when the
            // currently loaded avcodec DLL has no 10-bit HEVC encoder. If a
            // 10-bit HEVC encoder is present, fall through to the in-process
            // FrameEncoder path (videoCodecName + isHdr10/isHlg are set
            // below) which can emit a genuine 10-bit HDR stream directly.
            if (!libavcore::tenBitHevcEncoderAvailable()) {
                startRenderPipeSubprocess(jobIndex);
                return;
            }
        }
    }

    if (m_renderThread) {
        m_renderThread->wait(5000);
        delete m_renderThread;
        m_renderThread = nullptr;
    }

    // ── Resolve everything that touches the GUI/Timeline on THIS thread ─────
    // Timeline is a QWidget; constructing / restoring it must happen on the
    // GUI thread (the parity selftest + MainWindow both create it there). The
    // worker thread below only calls tlrender::renderFrameAt + FrameEncoder.
    // renderFrameAt is pure libav decode + CPU QPainter/QImage compositing
    // with NO QWidget construction — including its S6 text-overlay stage,
    // which bakes through the FREE function textbake::bakeOverlays (NOT a
    // VideoPlayer). Constructing a QWidget off the GUI thread is Qt undefined
    // behaviour, so this property is load-bearing for any timeline that
    // carries a text overlay. So we resolve the Timeline, geometry, frame
    // count and audio input path here and hand plain values to the worker.
    Timeline *owned = nullptr;
    Timeline *tl = resolveTimeline(jobCopy, &owned);
    if (!tl) {
        delete owned;
        QMetaObject::invokeMethod(this, [this, jobCopy]() {
            finishCurrentJob(false, QStringLiteral(
                "could not obtain a Timeline for job (project load failed and "
                "no in-memory timeline supplied): ") + jobCopy.projectFilePath);
        }, Qt::QueuedConnection);
        return;
    }

    // The file-backed carrier selftest observes the Timeline actually used by
    // the queue. Keep this opt-in so ordinary exports do no extra copying.
    if (property("captureResolvedRotoClipIdsForTest").toBool()) {
        QJsonObject ids;
        const auto entries = tl->rotoEntries();
        for (auto it = entries.cbegin(); it != entries.cend(); ++it)
            ids.insert(it.key(), it.value().clipId);
        setProperty("resolvedRotoClipIdsForTest", QVariant::fromValue(ids));
    }

    const QJsonObject &cfg = jobCopy.exportConfig;
    const std::optional<TimecodeBurnInRenderer> timecodeRenderer =
        timecodeBurnInForRender(cfg, tl);
    const double loudnessGainDb = configuredLoudnessGainDb(
        cfg, jobCopy.loudnessGainDb);
    const bool applyLoudnessFilter = hasLoudnessGain(loudnessGainDb);
    int outW = cfg.value("width").toInt(jobCopy.width > 0 ? jobCopy.width : 1920);
    int outH = cfg.value("height").toInt(jobCopy.height > 0 ? jobCopy.height : 1080);
    if (outW <= 0) outW = 1920;
    if (outH <= 0) outH = 1080;
    // The H.264/H.265 4:2:0 path requires even dimensions; keep the legacy
    // render-pipe rounding behavior before handing geometry to libavcore.
    outW &= ~1;
    outH &= ~1;
    double fps = cfg.value("fps").toDouble(0.0);
    if (fps <= 0.0)
        fps = 30.0;

    // Iterate from 0 to the timeline duration at the project fps.
    // usec-per-frame = 1e6 / fps (the renderFrameAt time unit).
    const double durationSec = tl->totalDuration();
    const qint64 timelineTotalFrames =
        static_cast<qint64>(durationSec * fps + 0.5);
    const double usecPerFrame = 1'000'000.0 / fps;
    qint64 totalFrames = timelineTotalFrames;
    qint64 startUsec = jobCopy.startUs > 0 ? jobCopy.startUs : 0;
    bool markedRangeApplied = false;
    const bool exportMarkedRangeOnly =
        cfg.value("exportMarkedRangeOnly").toBool(false);
    if (exportMarkedRangeOnly) {
        const ExportFrameRange range = computeExportRange(
            tl->markedIn(),
            tl->markedOut(),
            fps,
            timelineTotalFrames,
            true,
            tl->hasMarkedRange());
        totalFrames = range.frameCount();
        markedRangeApplied = range.usedMarkedRange;
        startUsec = markedRangeApplied
            ? static_cast<qint64>(tl->markedIn() * 1'000'000.0)
            : static_cast<qint64>(
                static_cast<double>(range.startFrame) * usecPerFrame);
    } else if (jobCopy.endUs > 0 && jobCopy.endUs > jobCopy.startUs) {
        const double rangeSec =
            static_cast<double>(jobCopy.endUs - jobCopy.startUs) / 1'000'000.0;
        totalFrames = static_cast<qint64>(rangeSec * fps + 0.5);
    }
    if (totalFrames <= 0)
        totalFrames = 1;
    const bool trimAudioToJobStart = startUsec > 0;

    // Audio mux input: the original source file's audio stream. FrameEncoder
    // handles this via audioSourcePath, preserving the render-pipe's optional
    // second input in-process.
    QString audioInputPath = jobCopy.projectFilePath;
    {
        const bool projIsMedia = !audioInputPath.isEmpty()
            && QFile::exists(audioInputPath)
            && !RenderQueue::isProjectFilePath(audioInputPath);
        if (!projIsMedia) {
            const QVector<ClipInfo> &v1 = tl->videoClips();
            if (!v1.isEmpty() && QFile::exists(v1.first().filePath))
                audioInputPath = v1.first().filePath;
            else
                audioInputPath.clear();
        }
    }

    libavcore::EncodeRequest request;
    request.subtitles = subtitleCues(cfg, startUsec, static_cast<qint64>(totalFrames * usecPerFrame));
    request.width = outW;
    request.height = outH;
    const FpsRational fpsRational = deriveFpsRational(fps);
    request.fps = static_cast<int>(std::round(fps));
    if (request.fps <= 0)
        request.fps = 30;
    request.fpsNum = fpsRational.num;
    request.fpsDen = fpsRational.den;
    request.outputPath = jobCopy.outputPath.toUtf8().toStdString();

    QString videoCodec = cfg.value("videoCodec").toString();
    if (videoCodec.isEmpty()) {
        videoCodec = mapCodecToEncoderName(jobCopy.codec.isEmpty()
            ? QStringLiteral("h264") : jobCopy.codec);
    }

    if (cfg.value("keepAlpha").toBool(false) && videoCodec == QLatin1String("prores"))
        videoCodec = QStringLiteral("prores_ks");

    const bool isProRes = (videoCodec == QLatin1String("prores_ks")
                           || videoCodec == QLatin1String("prores"));
    const QString hdrMode = cfg.value("hdrMode").toString().toLower();
    const bool isHdr10 = !isProRes
        && (hdrMode == QLatin1String("hdr10") || cfg.value("hdr10").toBool());
    const bool isHlg = !isProRes && hdrMode == QLatin1String("hlg");
    const int proresProfile = isProRes
        ? cfg.value("proresProfile").toInt(1) : -1;

    // AC: ACES は 8bit RGBA 出力。10bit/HDR (HDR10/HLG/ProRes) 経路では適用
    // しない。ジョブが addJob 時に写した値を使い、適用可否を 1 つの bool に畳む。
    // フレームループ内ではこの不変値だけ参照するので競合しない。
    // enabled=false のときは applyAces=false となり一切呼ばれない。
    aces::AcesPipeline acesPipe;
    {
        QMutexLocker locker(&m_acesMutex);
        acesPipe = jobCopy.acesPipeline.value_or(m_acesPipeline);
    }
    // When VEDITOR_HDR_ODT is ON with a 16-bit renderFrameAt path, that ODT
    // owns tonemap; otherwise the 8-bit ACES export pass must still run.
    const bool odtOwnsTonemap = odtOwnsTonemapForExport(
        clipodt::enabledFromEnv(),
        hdrexport16::enabledFromEnv(),
        hdrmatte16::enabledFromEnv());
    const bool applyAces = shouldApplyExportAces(
        acesPipe.enabled, isHdr10, isHlg, isProRes, odtOwnsTonemap);

    // HDR10/HLG followed the old render-pipe branch by forcing x265 unless the
    // caller explicitly selected an HEVC hardware encoder. US-B3-7: when this
    // branch executes we already know (per the 3-way dispatcher above) that
    // the loaded avcodec DLL exposes a 10-bit HEVC encoder; override
    // videoCodec to firstTenBitHevcEncoder() so the in-process encoder uses
    // whatever the DLL actually has (libx265 / hevc_nvenc / hevc_qsv /
    // hevc_amf) instead of an arbitrary preference. If the caller already
    // picked an HEVC encoder we leave it alone so an explicit selection is
    // respected.
    if ((isHdr10 || isHlg)
        && videoCodec != QLatin1String("libx265")
        && videoCodec != QLatin1String("hevc_nvenc")
        && videoCodec != QLatin1String("hevc_qsv")
        && videoCodec != QLatin1String("hevc_amf")) {
        const auto probed = libavcore::firstTenBitHevcEncoder();
        if (probed.has_value() && !probed->empty())
            videoCodec = QString::fromStdString(*probed);
        else
            videoCodec = QStringLiteral("libx265");
    }

    const bool blockSmartRenderForSequences =
        timelineHasSequenceRenderModel(tl);
    const bool blockSmartRenderForLighting = timelineHasProjectLighting(tl);
    const bool blockSmartRenderForDynamicTransform =
        timelineHasDynamicTransform(tl);
    const bool hasExportMetadata = renderqueue_metadata::hasMetadata(cfg);
    if (smartrender::enabledFromEnv()
        && !cfg.value("keepAlpha").toBool(false)
        && !applyLoudnessFilter
        && !timecodeRenderer.has_value()
        && !blockSmartRenderForSequences
        && !hasExportMetadata
        && request.subtitles.empty()
        && !cfg.contains("subtitleBurnInTrack")
        && cfg.value("subtitleMode").toInt(1) != 3
        && !blockSmartRenderForLighting
        && !blockSmartRenderForDynamicTransform) {
        if (m_cancelRequested) {
            delete owned;
            finishCurrentJob(false, QStringLiteral("cancelled"));
            return;
        }

        const smartrender::PassThroughEligibility passThrough =
            smartrender::timelinePassThrough(tl,
                                             jobCopy.outputPath,
                                             videoCodec,
                                             outW,
                                             outH,
                                             fps,
                                             jobCopy.startUs,
                                             jobCopy.endUs,
                                             exportMarkedRangeOnly,
                                             clipodt::enabledFromEnv(),
                                             hdrexport16::enabledFromEnv(),
                                             hdrmatte16::enabledFromEnv(),
                                             applyAces,
                                             isHdr10,
                                             isHlg);
        if (passThrough.eligible) {
            const QString srcPath = tl->videoTracks().first()->clips().first().filePath;
            const std::string srcPathUtf8 = srcPath.toUtf8().toStdString();
            const std::string outputPathUtf8 =
                jobCopy.outputPath.toUtf8().toStdString();
            const std::optional<std::string> copyError =
                libavcore::concatCopy({srcPathUtf8}, outputPathUtf8);
            if (!copyError.has_value()) {
                if (m_cancelRequested) {
                    QFile::remove(jobCopy.outputPath);
                    delete owned;
                    finishCurrentJob(false, QStringLiteral("cancelled"));
                    return;
                }
                qInfo().noquote()
                    << "[smart-render] stream-copied whole timeline"
                    << "source=" << srcPath
                    << "output=" << jobCopy.outputPath;
                delete owned;
                finishCurrentJob(true, QString());
                return;
            }

            const QString reason = QString::fromStdString(*copyError);
            qWarning().noquote()
                << QStringLiteral(
                       "[smart-render] stream copy failed (%1), falling back to encode")
                       .arg(reason);
            QFile::remove(jobCopy.outputPath);
        } else {
            qInfo().noquote()
                << "[smart-render] whole timeline not stream-copy eligible:"
                << passThrough.reason;
        }
    } else if (smartrender::enabledFromEnv() && hasExportMetadata) {
        qInfo().noquote()
            << "[smart-render] whole timeline not stream-copy eligible:"
            << "export metadata requires encoding";
    } else if (smartrender::enabledFromEnv()
               && !applyLoudnessFilter
               && (blockSmartRenderForSequences || blockSmartRenderForLighting)) {
        qInfo().noquote()
            << "[smart-render] whole timeline not stream-copy eligible:"
            << (blockSmartRenderForSequences
                    ? (blockSmartRenderForLighting
                           ? "timeline contains nested sequence state and project lighting"
                           : "timeline contains nested sequence state")
                    : "timeline contains project lighting");
    } else if (smartrender::enabledFromEnv()
               && !applyLoudnessFilter
               && blockSmartRenderForDynamicTransform) {
        qInfo().noquote()
            << "[smart-render] whole timeline not stream-copy eligible:"
            << "timeline has a dynamic transform (wiggle)";
    }

    request.rateControl = cfg.value("rateControl").toString() == "crf"
        ? libavcore::EncodeRequest::RateControl::Crf
        : libavcore::EncodeRequest::RateControl::Bitrate;
    request.crf = cfg.value("crf").toInt(-1);
    request.videoBitrateBits = jobCopy.bitrateBps;
    if (request.videoBitrateBits <= 0) {
        int videoBitrateKbps = cfg.value("videoBitrate").toInt(0);
        if (videoBitrateKbps <= 0)
            videoBitrateKbps = 10000;
        request.videoBitrateBits =
            static_cast<int64_t>(videoBitrateKbps) * 1000;
    }
    request.videoCodecName = videoCodec.toUtf8().toStdString();
    request.isHdr10 = isHdr10;
    request.isHlg = isHlg;
    request.proresProfile = proresProfile;
    request.keepAlpha = cfg.value("keepAlpha").toBool(false);
    request.title = cfg.value("title").toString().toUtf8().toStdString();
    request.author = cfg.value("author").toString().toUtf8().toStdString();
    request.copyright = cfg.value("copyright").toString().toUtf8().toStdString();
    request.comment = cfg.value("comment").toString().toUtf8().toStdString();

    request.hdrMasterMaxNits =
        cfg.value("hdrMasterMaxLum").toDouble(1000.0);
    request.hdrMasterMinNits =
        cfg.value("hdrMasterMinLum").toDouble(0.0001);
    request.hdrMaxCll = cfg.value("hdrMaxCll").toInt(1000);
    request.hdrMaxFall = cfg.value("hdrMaxFall").toInt(400);

    const bool haveAudio = !audioInputPath.isEmpty()
        && QFile::exists(audioInputPath);
    if (haveAudio) {
        request.audioSourcePath = audioInputPath.toUtf8().toStdString();
        if (trimAudioToJobStart)
            request.audioStartUs = startUsec;
    }
    const QString loudnessAudioCodec = cfg.value("audioCodec").toString();
    const int loudnessAudioBitrateKbps = cfg.value("audioBitrate").toInt(192);

    request.encoderAvailableHook = [](const std::string &name) {
        return CodecDetector::isEncoderAvailable(QString::fromStdString(name));
    };

    m_renderThread = QThread::create(
        [this, jobCopy, tl, owned, request, outW, outH,
         totalFrames, startUsec, usecPerFrame, applyAces, acesPipe,
         haveAudio, loudnessGainDb, loudnessAudioCodec,
         loudnessAudioBitrateKbps, timecodeRenderer, fps]() {
        QString failMsg;
        // Delete the heap Timeline (only set when loaded from a project
        // file). It is a QWidget created on the GUI thread; QObject deletion
        // is safe from another thread only if it has no thread affinity ops
        // pending — Timeline here is render-only (never shown, no event loop
        // posted to it), so deleteLater on the owning thread is the safe
        // disposal. We schedule that back on the queue thread at the end.
        const bool ok = [&]() -> bool {
            auto encoderOwner = std::make_unique<libavcore::FrameEncoder>();
            libavcore::FrameEncoder &encoder = *encoderOwner;
            if (auto err = encoder.open(request)) {
                failMsg = QStringLiteral("failed to open encoder: ")
                    + QString::fromStdString(*err);
                return false;
            }

            const QSize outSize(outW, outH);
            const int rgbRowBytes = outW * 3;
            QByteArray frameData;
            frameData.resize(static_cast<qsizetype>(rgbRowBytes)
                             * static_cast<qsizetype>(outH));

            int lastPct = -1;
            bool encodeOk = true;
            bool cancelled = false;
            qint64 pts = 0;
            for (qint64 f = 0; f < totalFrames; ++f) {
                if (m_cancelRequested) {
                    cancelled = true;
                    failMsg = QStringLiteral("cancelled");
                    break;
                }

                const qint64 usec =
                    startUsec + static_cast<qint64>(f * usecPerFrame);
                QImage frame = tlrender::renderFrameAt(tl, usec, outSize, usecPerFrame);
                if (frame.isNull()) {
                    failMsg = QStringLiteral(
                        "renderFrameAt returned a null image at frame ")
                        + QString::number(f);
                    encodeOk = false;
                    break;
                }
                if (frame.format() != QImage::Format_RGBA8888)
                    frame = frame.convertToFormat(QImage::Format_RGBA8888);
                if (frame.width() != outW || frame.height() != outH)
                    frame = frame.scaled(outSize, Qt::IgnoreAspectRatio,
                                          Qt::SmoothTransformation)
                                .convertToFormat(QImage::Format_RGBA8888);

                // AC: ACES カラーマネジメントを production export に適用する。
                // frame は Format_RGBA8888。applyPipelineToImage は RGBA8888 を
                // 返すので後段の RGB888 flatten 契約を壊さない。applyAces は
                // (enabled && !HDR && !ProRes) のときだけ真 → 8bit 経路限定。
                // enabled=false 時は呼ばれずビット同一 (回帰ゼロ)。
                if (applyAces)
                    frame = aces::applyPipelineToImage(frame, acesPipe);

                paintSubtitles(frame, jobCopy.exportConfig, usec);

                if (!request.keepAlpha) {
                ++s_blackCompositeCount;
                // A video file is OPAQUE — it has no alpha channel. The SSOT
                // frame can be partially transparent (a per-clip mask with no
                // lower track leaves alpha=0 regions). The genuine preview
                // displays that composite over a BLACK canvas
                // (VideoPlayer ARGB32_Premultiplied base, VideoPlayer.cpp:1939
                // fills black) and the genuine Exporter encodes through an
                // opaque Format_RGB888 (Exporter.cpp:482/499, AV_PIX_FMT_RGB24)
                // which carries no alpha. So the correct deliverable is the
                // SSOT flattened onto opaque black. Compose onto a black
                // Format_RGB888 and feed libavcore rgb24 (3 bytes/px, no
                // alpha) — identical pixel semantics to the Exporter's RGB24
                // path.
                QImage rgb(outW, outH, QImage::Format_RGB888);
                rgb.fill(Qt::black);
                {
                    QPainter pp(&rgb);
                    pp.setCompositionMode(QPainter::CompositionMode_SourceOver);
                    pp.drawImage(0, 0, frame);
                    if (timecodeRenderer.has_value()) {
                        const double timelineSec =
                            static_cast<double>(usec) / 1'000'000.0;
                        timecodeRenderer->paintOnto(
                            pp, QRectF(0, 0, outW, outH), timelineSec, fps,
                            timecodeBurnInClipNameAt(tl, timelineSec));
                    }
                }

                // FrameEncoder receives packed RGB24. Qt pads Format_RGB888
                // scanlines to a 4-byte boundary, so compact into a contiguous
                // W*H*3 buffer before pushFrameRgb24(width*3 stride).
                char *dst = frameData.data();
                for (int y = 0; y < outH; ++y) {
                    std::memcpy(dst + static_cast<qsizetype>(y) * rgbRowBytes,
                                rgb.constScanLine(y),
                                static_cast<std::size_t>(rgbRowBytes));
                }

                if (!encoder.pushFrameRgb24(
                        reinterpret_cast<const uint8_t *>(frameData.constData()),
                        rgbRowBytes,
                        pts++)) {
                    failMsg = QStringLiteral("encoder frame push failed");
                    encodeOk = false;
                    break;
                }

                } else {
                    frame = frame.convertToFormat(QImage::Format_RGBA8888);
                    if (timecodeRenderer.has_value()) {
                        QPainter pp(&frame);
                        const double timelineSec = static_cast<double>(usec) / 1'000'000.0;
                        timecodeRenderer->paintOnto(
                            pp, QRectF(0, 0, outW, outH), timelineSec, fps,
                            timecodeBurnInClipNameAt(tl, timelineSec));
                    }
                    if (!encoder.pushFrameRgba32(frame.constBits(), frame.bytesPerLine(), pts++)) {
                        failMsg = QStringLiteral("アルファ付きフレームの書き出しに失敗しました");
                        encodeOk = false;
                        break;
                    }
                }

                const int pct = qBound(0, static_cast<int>(
                    (static_cast<double>(f + 1)
                        / static_cast<double>(totalFrames)) * 100.0), 99);
                if (pct != lastPct) {
                    lastPct = pct;
                    emit jobProgress(jobCopy.id, pct);
                    emit jobProgressUuid(jobCopy.uuid, pct);
                }
            }

            if (auto err = encoder.finalize()) {
                if (failMsg.isEmpty()) {
                    failMsg = QStringLiteral("encoder finalize failed: ")
                        + QString::fromStdString(*err);
                }
                encodeOk = false;
            }

            if (encodeOk && haveAudio && hasLoudnessGain(loudnessGainDb)) {
                // finalize() writes the trailer but keeps the output handle
                // open. Close it before ffmpeg reads/replaces the file: Windows
                // otherwise rejects the rename of the unnormalized export.
                encoderOwner.reset();
                QString loudnessError;
                if (!runLoudnessAudioPostprocess(
                        findFFmpegBinary(),
                        jobCopy.outputPath,
                        loudnessGainDb,
                        loudnessAudioCodec,
                        loudnessAudioBitrateKbps,
                        !request.subtitles.empty(),
                        &loudnessError)) {
                    failMsg = loudnessError;
                    encodeOk = false;
                }
            }

            if (cancelled || m_cancelRequested)
                return false;
            return encodeOk;
        }();

        // Dispose the heap Timeline (render-only QWidget, never shown). Hand
        // the result back to the queue thread; delete owned there so the
        // QWidget is destroyed on the GUI/queue thread that created it.
        QMetaObject::invokeMethod(this, [this, ok, failMsg, owned]() {
            delete owned;
            finishCurrentJob(ok, failMsg);
        }, Qt::QueuedConnection);
    });
    m_renderThread->start();
}

// US-MF-6 / US-B3-7: 10-bit HDR (HDR10 / HLG) subprocess fallback branch.
// Reached only when startRenderPipe() determined the loaded avcodec DLL has
// no 10-bit HEVC encoder (libavcore::tenBitHevcEncoderAvailable() == false).
// When the bundled avcodec DLL is in use this is the active HDR path: it
// ships neither libx264/libx265 nor a 10-bit MediaFoundation encoder
// (h264_mf/hevc_mf are 8-bit only), so the in-process libavcore::FrameEncoder
// cannot emit a genuine HDR10/HLG stream from the stock build.
//
// This restores the pre-US-MF-5 render-pipe for HDR fallback ONLY: a worker
// QThread renders every output frame via tlrender::renderFrameAt (the SSOT
// edit graph — grade / FX / masks / LUT all applied) and streams flattened
// rgb24 to ffmpeg.exe over stdin; ffmpeg encodes libx265 main10 yuv420p10le
// with BT.2020 + SMPTE-2084 (HDR10) / ARIB-STD-B67 (HLG) signalling, the
// HDR10 master-display / MaxCLL / MaxFALL x265-params, and muxes the source
// audio. The public RenderJob / signal / queue-advance contract is identical
// to the in-process branch: jobProgress per frame, cancellation honoured via
// m_cancelRequested, finishCurrentJob() on completion. The argv mirrors the
// genuine Exporter HDR setup (Exporter.cpp HDR10/HLG x265 path) byte-for-byte
// so the artifact matches the parity selftest's byte-identical Path A
// round-trip. The master-display string itself is sourced from
// libavcore::hdr10MasterDisplayString (the SSOT shared with FrameEncoder /
// Exporter) so the in-process and subprocess paths agree on the SMPTE-2086
// primaries + scaled luminance terms.
void RenderQueue::startRenderPipeSubprocess(int jobIndex)
{
    const RenderJob jobCopy = m_jobs[jobIndex];
    if (jobCopy.exportConfig.value("keepAlpha").toBool()) {
        finishCurrentJob(false, QStringLiteral("HDR 代替経路ではアルファを保持できません"));
        return;
    }

    // AC: ACES は 8bit RGBA 出力。この経路は定義上 10bit HDR (HDR10/HLG) 専用
    // (startRenderPipe が 10bit HEVC encoder 不在のときだけ委譲する) なので
    // ACES は適用しない。スナップショットだけ取り、apply は意図的にスキップする
    // (SSOT メンバへのアクセスを mutex で直列化する一貫性のため取得はする)。
    {
        QMutexLocker locker(&m_acesMutex);
        (void)m_acesPipeline;  // 10bit/HDR 経路: apply スキップ (8bit のみ対象)
    }

    // Reap any prior subprocess / worker thread before starting a new job.
    QProcess *staleProc = nullptr;
    {
        QMutexLocker locker(&m_processMutex);
        staleProc = m_process;
        m_process = nullptr;
    }
    if (staleProc)
        delete staleProc;
    if (m_renderThread) {
        m_renderThread->wait(5000);
        delete m_renderThread;
        m_renderThread = nullptr;
    }

    // ── Resolve everything that touches the GUI/Timeline on THIS thread ─────
    // Identical rationale to the in-process branch: Timeline is a QWidget;
    // restoring it must happen on the GUI thread. The worker thread below
    // only calls tlrender::renderFrameAt (pure libav decode + CPU QPainter
    // compositing, no QWidget construction) + QProcess stdin I/O.
    const QString ffmpegBin = findFFmpegBinary();
    if (ffmpegBin.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            // US-B3-7: surface the genuine requirement so users can fix
            // their environment instead of seeing a silent failure. The
            // subprocess path is only reached when the loaded avcodec DLL
            // has no 10-bit HEVC encoder (libavcore::tenBitHevcEncoderAvailable
            // returned false), and libx265-enabled ffmpeg.exe is what
            // produces the genuine HDR10/HLG stream the in-process path
            // cannot.
            finishCurrentJob(false, QStringLiteral(
                "10-bit HDR export requires ffmpeg.exe with libx265 in PATH "
                "or alongside the app — the bundled avcodec DLL has no "
                "10-bit HEVC encoder and ffmpeg.exe could not be located"));
        }, Qt::QueuedConnection);
        return;
    }

    Timeline *owned = nullptr;
    Timeline *tl = resolveTimeline(jobCopy, &owned);
    if (!tl) {
        delete owned;
        QMetaObject::invokeMethod(this, [this, jobCopy]() {
            finishCurrentJob(false, QStringLiteral(
                "could not obtain a Timeline for job (project load failed and "
                "no in-memory timeline supplied): ") + jobCopy.projectFilePath);
        }, Qt::QueuedConnection);
        return;
    }

    const QJsonObject &cfg = jobCopy.exportConfig;
    const std::optional<TimecodeBurnInRenderer> timecodeRenderer =
        timecodeBurnInForRender(cfg, tl);
    const double loudnessGainDb = configuredLoudnessGainDb(
        cfg, jobCopy.loudnessGainDb);
    const QStringList loudnessAudioFilterArgs =
        buildLoudnessAudioFilterArgs(loudnessGainDb);
    int outW = cfg.value("width").toInt(jobCopy.width > 0 ? jobCopy.width : 1920);
    int outH = cfg.value("height").toInt(jobCopy.height > 0 ? jobCopy.height : 1080);
    if (outW <= 0) outW = 1920;
    if (outH <= 0) outH = 1080;
    // libx265 yuv420p10le requires even dimensions; round down to even.
    outW &= ~1;
    outH &= ~1;
    double fps = cfg.value("fps").toDouble(0.0);
    if (fps <= 0.0)
        fps = 30.0;

    const double durationSec = tl->totalDuration();
    const qint64 timelineTotalFrames =
        static_cast<qint64>(durationSec * fps + 0.5);
    const double usecPerFrame = 1'000'000.0 / fps;
    qint64 totalFrames = timelineTotalFrames;
    qint64 startUsec = jobCopy.startUs > 0 ? jobCopy.startUs : 0;
    bool markedRangeApplied = false;
    const bool exportMarkedRangeOnly =
        cfg.value("exportMarkedRangeOnly").toBool(false);
    if (exportMarkedRangeOnly) {
        const ExportFrameRange range = computeExportRange(
            tl->markedIn(),
            tl->markedOut(),
            fps,
            timelineTotalFrames,
            true,
            tl->hasMarkedRange());
        totalFrames = range.frameCount();
        markedRangeApplied = range.usedMarkedRange;
        startUsec = markedRangeApplied
            ? static_cast<qint64>(tl->markedIn() * 1'000'000.0)
            : static_cast<qint64>(
                static_cast<double>(range.startFrame) * usecPerFrame);
    } else if (jobCopy.endUs > 0 && jobCopy.endUs > jobCopy.startUs) {
        const double rangeSec =
            static_cast<double>(jobCopy.endUs - jobCopy.startUs) / 1'000'000.0;
        totalFrames = static_cast<qint64>(rangeSec * fps + 0.5);
    }
    if (totalFrames <= 0)
        totalFrames = 1;
    const bool trimAudioToJobStart = startUsec > 0;

    // Audio mux input — identical resolution to the in-process branch's
    // audioSourcePath: the job's source media, falling back to the V1 clip's
    // file when projectFilePath is a .veditor / empty.
    QString audioInputPath = jobCopy.projectFilePath;
    {
        const bool projIsMedia = !audioInputPath.isEmpty()
            && QFile::exists(audioInputPath)
            && !RenderQueue::isProjectFilePath(audioInputPath);
        if (!projIsMedia) {
            const QVector<ClipInfo> &v1 = tl->videoClips();
            if (!v1.isEmpty() && QFile::exists(v1.first().filePath))
                audioInputPath = v1.first().filePath;
            else
                audioInputPath.clear();
        }
    }

    // ── Build the ffmpeg argv: rawvideo rgb24 stdin → libx265 HDR ───────────
    QString videoCodec = cfg.value("videoCodec").toString();
    if (videoCodec.isEmpty()) {
        videoCodec = mapCodecToEncoderName(jobCopy.codec.isEmpty()
            ? QStringLiteral("h264") : jobCopy.codec);
    }
    const QString hdrMode = cfg.value("hdrMode").toString().toLower();
    const bool isHdr10 =
        (hdrMode == QLatin1String("hdr10") || cfg.value("hdr10").toBool());
    const bool isHlg = hdrMode == QLatin1String("hlg");

    // HDR10/HLG are 10-bit; force libx265 unless the caller explicitly chose
    // an HEVC hardware encoder (mirrors the in-process branch's override).
    if (videoCodec != QLatin1String("libx265")
        && videoCodec != QLatin1String("hevc_nvenc")
        && videoCodec != QLatin1String("hevc_qsv")
        && videoCodec != QLatin1String("hevc_amf")) {
        videoCodec = QStringLiteral("libx265");
    }

    QStringList args;
    args << QStringLiteral("-y")
         << QStringLiteral("-hide_banner")
         // Input 0: the rendered frame stream on stdin. The SSOT RGBA is
         // flattened onto opaque black and fed as rgb24 (3 bytes/px, NO
         // alpha) — same pixel semantics as the genuine Exporter's
         // AV_PIX_FMT_RGB24 path.
         << QStringLiteral("-f") << QStringLiteral("rawvideo")
         << QStringLiteral("-pix_fmt") << QStringLiteral("rgb24")
         << QStringLiteral("-s:v")
         << QStringLiteral("%1x%2").arg(outW).arg(outH)
         << QStringLiteral("-r") << QString::number(fps, 'f', 6)
         << QStringLiteral("-i") << QStringLiteral("-");

    // Input 1: the original media — used ONLY for its audio stream.
    const bool haveAudio = !audioInputPath.isEmpty()
        && QFile::exists(audioInputPath);
    if (haveAudio) {
        if (trimAudioToJobStart) {
            args << QStringLiteral("-ss")
                 << QString::number(
                        static_cast<double>(startUsec) / 1'000'000.0,
                        'f',
                        6);
        }
        args << QStringLiteral("-i") << audioInputPath;
    }

    std::shared_ptr<QTemporaryDir> subtitleTemp;
    const auto cues = subtitleCues(cfg, startUsec, static_cast<qint64>(totalFrames * usecPerFrame));
    if (!cues.empty()) {
        subtitleTemp = std::make_shared<QTemporaryDir>();
        QList<caption::Clip> captions;
        for (const auto &cue : cues) {
            caption::Clip clip;
            clip.startMs = cue.startUs / 1000;
            clip.endMs = cue.endUs / 1000;
            clip.text = QString::fromUtf8(cue.text.data(), static_cast<qsizetype>(cue.text.size()));
            captions.append(clip);
        }
        const QString path = subtitleTemp->filePath("captions.srt");
        if (!subtitleTemp->isValid() || !subtitle::exportSrt(path, captions)) {
            delete owned;
            QMetaObject::invokeMethod(this, [this]() {
                finishCurrentJob(false, QStringLiteral("字幕の一時ファイルを作成できません"));
            }, Qt::QueuedConnection);
            return;
        }
        args << "-i" << path;
    }
    args << QStringLiteral("-map") << QStringLiteral("0:v:0");
    if (haveAudio)
        args << QStringLiteral("-map") << QStringLiteral("1:a?");
    if (subtitleTemp)
        args << "-map" << (haveAudio ? "2:s:0" : "1:s:0") << "-c:s" << "mov_text";

    // Video codec + rate control.
    args << QStringLiteral("-c:v") << videoCodec;
    if (cfg.value("rateControl").toString() == "crf") {
        libavcore::EncodeRequest qualityRequest;
        qualityRequest.rateControl = libavcore::EncodeRequest::RateControl::Crf;
        qualityRequest.crf = cfg.value("crf").toInt(-1);
        for (const auto& option : libavcore::codecOptionsFor(qualityRequest, videoCodec))
            args << (QStringLiteral("-") + option.first) << option.second;
    } else {
        int videoBitrateKbps = cfg.value("videoBitrate").toInt(0);
        if (videoBitrateKbps <= 0)
            videoBitrateKbps = jobCopy.bitrateBps > 0
                ? static_cast<int>(jobCopy.bitrateBps / 1000) : 10000;
        args << QStringLiteral("-b:v")
             << QStringLiteral("%1k").arg(videoBitrateKbps);
    }

    // 10-bit pixel format + BT.2020 colour signalling (Exporter HDR path:
    // HDR10 = PQ/SMPTE-2084, HLG = ARIB-STD-B67, BT.2020 primaries +
    // non-constant luminance, limited (tv) range).
    args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p10le")
         << QStringLiteral("-color_primaries") << QStringLiteral("bt2020")
         << QStringLiteral("-colorspace") << QStringLiteral("bt2020nc")
         << QStringLiteral("-color_range") << QStringLiteral("tv")
         << QStringLiteral("-color_trc")
         << (isHdr10 ? QStringLiteral("smpte2084")
                     : QStringLiteral("arib-std-b67"));

    if (videoCodec == QLatin1String("libx265")) {
        args << QStringLiteral("-preset") << QStringLiteral("medium");
    }

    // libx265 10-bit HDR profile + x265-params — byte-for-byte the genuine
    // Exporter's HDR10/HLG x265 setup. HDR10 master-display / MaxCLL / MaxFALL
    // come from exportConfig with the standard 1000-nit P3-D65-in-2020
    // mastering defaults. US-B3-7: the master-display chromaticity + scaled
    // luminance terms are produced by libavcore::hdr10MasterDisplayString —
    // the SSOT shared with libavcore::FrameEncoder / Exporter — so the
    // in-process and subprocess HDR paths agree on the SMPTE-2086 primaries.
    if (videoCodec == QLatin1String("libx265")) {
        args << QStringLiteral("-profile:v") << QStringLiteral("main10");
        if (isHdr10) {
            const double maxLum =
                cfg.value("hdrMasterMaxLum").toDouble(1000.0);
            const double minLum =
                cfg.value("hdrMasterMinLum").toDouble(0.0001);
            const int maxCll  = cfg.value("hdrMaxCll").toInt(1000);
            const int maxFall = cfg.value("hdrMaxFall").toInt(400);
            const QString masterDisplay = QString::fromStdString(
                libavcore::hdr10MasterDisplayString(maxLum, minLum));
            const QString x265p =
                QStringLiteral(
                    "hdr10=1:repeat-headers=1:colorprim=bt2020:"
                    "transfer=smpte2084:colormatrix=bt2020nc:range=limited:"
                    "master-display=%1:max-cll=%2,%3")
                    .arg(masterDisplay)
                    .arg(maxCll)
                    .arg(maxFall);
            args << QStringLiteral("-x265-params") << x265p;
        } else {  // HLG
            args << QStringLiteral("-x265-params")
                 << QStringLiteral("repeat-headers=1:colorprim=bt2020:"
                                   "transfer=arib-std-b67:"
                                   "colormatrix=bt2020nc");
        }
    }

    if (haveAudio) {
        QString audioCodec = cfg.value("audioCodec").toString();
        if (audioCodec.isEmpty())
            audioCodec = QStringLiteral("aac");
        args << QStringLiteral("-c:a") << audioCodec;
        const int audioBitrate = cfg.value("audioBitrate").toInt(192);
        args << QStringLiteral("-b:a")
             << QStringLiteral("%1k").arg(audioBitrate);
        args << loudnessAudioFilterArgs;
        // Stop at the shorter of rendered video / source audio so a longer
        // audio track doesn't pad black frames past the timeline.
        args << QStringLiteral("-shortest");
    }

    renderqueue_metadata::appendSubprocessArgs(args, cfg, jobCopy.outputPath);
    args << QStringLiteral("-progress") << QStringLiteral("pipe:2");
    args << jobCopy.outputPath;

    m_renderThread = QThread::create(
        [this, jobCopy, tl, owned, ffmpegBin, args, outW, outH,
         totalFrames, startUsec, usecPerFrame, timecodeRenderer, fps, subtitleTemp]() {
        QString failMsg;
        const bool ok = [&]() -> bool {
            QProcess *proc = new QProcess();
            proc->setProcessChannelMode(QProcess::SeparateChannels);
            {
                QMutexLocker locker(&m_processMutex);
                m_process = proc;
            }
            auto clearProcess = [&]() {
                QMutexLocker locker(&m_processMutex);
                if (m_process == proc)
                    m_process = nullptr;
            };
            proc->start(ffmpegBin, args);
            if (!proc->waitForStarted(15000)) {
                failMsg = QStringLiteral("failed to start ffmpeg: ")
                    + proc->errorString();
                clearProcess();
                proc->deleteLater();
                return false;
            }

            const QSize outSize(outW, outH);
            const int rgbRowBytes = outW * 3;
            int lastPct = -1;
            for (qint64 f = 0; f < totalFrames; ++f) {
                if (m_cancelRequested) {
                    proc->kill();
                    proc->waitForFinished(3000);
                    clearProcess();
                    proc->deleteLater();
                    failMsg = QStringLiteral("cancelled");
                    return false;
                }

                const qint64 usec =
                    startUsec + static_cast<qint64>(f * usecPerFrame);
                QImage frame = tlrender::renderFrameAt(tl, usec, outSize, usecPerFrame);
                if (frame.isNull()) {
                    proc->kill();
                    proc->waitForFinished(3000);
                    clearProcess();
                    proc->deleteLater();
                    failMsg = QStringLiteral(
                        "renderFrameAt returned a null image at frame ")
                        + QString::number(f);
                    return false;
                }
                if (frame.format() != QImage::Format_RGBA8888)
                    frame = frame.convertToFormat(QImage::Format_RGBA8888);
                if (frame.width() != outW || frame.height() != outH)
                    frame = frame.scaled(outSize, Qt::IgnoreAspectRatio,
                                          Qt::SmoothTransformation)
                                .convertToFormat(QImage::Format_RGBA8888);

                // A video file is OPAQUE — flatten the (possibly partially
                // transparent) SSOT composite onto opaque black and feed
                // rgb24, identical to the in-process branch.
                paintSubtitles(frame, jobCopy.exportConfig, usec);
                QImage rgb(outW, outH, QImage::Format_RGB888);
                rgb.fill(Qt::black);
                {
                    QPainter pp(&rgb);
                    pp.setCompositionMode(QPainter::CompositionMode_SourceOver);
                    pp.drawImage(0, 0, frame);
                    if (timecodeRenderer.has_value()) {
                        const double timelineSec =
                            static_cast<double>(usec) / 1'000'000.0;
                        timecodeRenderer->paintOnto(
                            pp, QRectF(0, 0, outW, outH), timelineSec, fps,
                            timecodeBurnInClipNameAt(tl, timelineSec));
                    }
                }

                // ffmpeg -s WxH -pix_fmt rgb24 expects exactly W*H*3 bytes
                // per frame with NO row padding; Qt pads Format_RGB888
                // scanlines to a 4-byte boundary, so write row by row.
                for (int y = 0; y < outH; ++y) {
                    const char *row =
                        reinterpret_cast<const char *>(rgb.constScanLine(y));
                    qint64 rem = rgbRowBytes;
                    while (rem > 0) {
                        const qint64 wrote = proc->write(row, rem);
                        if (wrote < 0) {
                            proc->kill();
                            proc->waitForFinished(3000);
                            clearProcess();
                            proc->deleteLater();
                            failMsg =
                                QStringLiteral("ffmpeg stdin write failed");
                            return false;
                        }
                        row += wrote;
                        rem -= wrote;
                        if (rem > 0 && !proc->waitForBytesWritten(15000)) {
                            proc->kill();
                            proc->waitForFinished(3000);
                            clearProcess();
                            proc->deleteLater();
                            failMsg = QStringLiteral("ffmpeg stdin stalled");
                            return false;
                        }
                    }
                }

                proc->readAllStandardError();

                const int pct = qBound(0, static_cast<int>(
                    (static_cast<double>(f + 1)
                        / static_cast<double>(totalFrames)) * 100.0), 99);
                if (pct != lastPct) {
                    lastPct = pct;
                    emit jobProgress(jobCopy.id, pct);
                    emit jobProgressUuid(jobCopy.uuid, pct);
                }
            }

            proc->closeWriteChannel();
            while (!proc->waitForFinished(100)) {
                proc->readAllStandardError();
            }
            proc->readAllStandardError();
            const bool encOk = proc->exitStatus() == QProcess::NormalExit
                && proc->exitCode() == 0;
            if (!encOk)
                failMsg = QStringLiteral("ffmpeg exited with code ")
                    + QString::number(proc->exitCode());
            clearProcess();
            proc->deleteLater();
            return encOk;
        }();

        QMetaObject::invokeMethod(this, [this, ok, failMsg, owned]() {
            delete owned;
            finishCurrentJob(ok, failMsg);
        }, Qt::QueuedConnection);
    });
    m_renderThread->start();
}

// US-MF-6 / US-B3-7: locate the ffmpeg.exe binary for the HDR subprocess
// fallback branch. PATH first (the WinGet / gyan build installs ffmpeg.exe
// onto the user PATH), then a list of common install dirs that cover the
// realistic ways a Windows user gets ffmpeg.exe onto disk:
//   - the application's own directory (bundled side-by-side with the .exe)
//   - %LOCALAPPDATA%\Microsoft\WinGet\Links (WinGet shim, often not in PATH
//     when launched from a desktop shortcut)
//   - C:\ffmpeg\bin (the manual gyan.dev install path the project README
//     suggests)
// macOS / Linux paths are kept so headless / Unix builds keep working.
QString RenderQueue::findFFmpegBinary()
{
    QString path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!path.isEmpty())
        return path;
    QStringList searchPaths;
    // Application directory — ffmpeg.exe shipped alongside the binary.
    if (QCoreApplication::instance()) {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty())
            searchPaths << appDir;
    }
    // WinGet shim directory under %LOCALAPPDATA%.
    const QString localAppData =
        QProcessEnvironment::systemEnvironment().value(
            QStringLiteral("LOCALAPPDATA"));
    if (!localAppData.isEmpty()) {
        searchPaths << QDir::cleanPath(
            localAppData + QStringLiteral("/Microsoft/WinGet/Links"));
    }
    // Common manual install paths.
    searchPaths << QStringLiteral("C:/ffmpeg/bin")
                << QStringLiteral("/usr/local/bin")
                << QStringLiteral("/opt/homebrew/bin")
                << QStringLiteral("/usr/bin");
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"),
                                          searchPaths);
}

// Finalise the current job and advance the queue. Always runs on the
// RenderQueue's (main/queue) thread — preserves the exact public contract the
// worker completion handler has.
void RenderQueue::finishCurrentJob(bool success, const QString &errorMsg)
{
    if (m_renderThread) {
        m_renderThread->wait(5000);
        m_renderThread->deleteLater();
        m_renderThread = nullptr;
    }

    if (m_currentJobIndex < 0 || m_currentJobIndex >= m_jobs.size()) {
        removeTempFiles(nullptr); // worker has stopped: retry paths it held open
        return;
    }

    RenderJob &job = m_jobs[m_currentJobIndex];
    job.endTime = QDateTime::currentDateTime();
    // Single job-end cleanup for every jobCompletedUuid below (cancel / failure /
    // success), done before the emits because a slot may enqueue and reallocate m_jobs.
    removeTempFiles(&job);

    if (job.status == RenderJobStatus::Cancelled || m_cancelRequested) {
        job.status = RenderJobStatus::Cancelled;
        if (job.errorMessage.isEmpty()) {
            job.errorMessage = QStringLiteral("Cancelled by user");
            job.error = job.errorMessage;
        }
        emit jobCompletedUuid(job.uuid, false, job.error);
    } else if (!success) {
        job.status = RenderJobStatus::Failed;
        job.errorMessage = errorMsg.isEmpty()
            ? QStringLiteral("Render pipe failed") : errorMsg;
        job.error = job.errorMessage;
        emit jobFailed(job.id, job.errorMessage);
        emit jobCompletedUuid(job.uuid, false, job.errorMessage);
    } else {
        job.status = RenderJobStatus::Completed;
        job.progress = 100;
        job.progressPercent = 100;
        // Capture before emitting: a slot connected to the signals below could
        // enqueue/remove jobs and reallocate m_jobs, leaving `job` dangling.
        const QString dvXml = job.dolbyVisionXml;
        const QString dvOutputPath = job.outputPath;
        emit jobProgress(job.id, 100);
        emit jobProgressUuid(job.uuid, 100);
        emit jobCompleted(job.id);
        emit jobCompletedUuid(job.uuid, true, QString());
        if (!dvXml.isEmpty()) {
            // ".dv.xml" (not bare ".xml") avoids clobbering an unrelated sidecar
            // such as a hand-authored EDL/settings XML sharing the output stem.
            const QString xmlPath = QFileInfo(dvOutputPath).absolutePath() + "/"
                + QFileInfo(dvOutputPath).completeBaseName() + ".dv.xml";
            QFile f(xmlPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write(dvXml.toUtf8());
                f.close();
            } else {
                qWarning() << "[DV XML] Failed to write sidecar:" << xmlPath;
            }
        }
    }

    emit jobsChanged();

    // Emit overall queue progress (unchanged from the legacy handler).
    int total = m_jobs.size();
    int done = completedCount();
    for (const auto &j : m_jobs) {
        if (j.status == RenderJobStatus::Failed
            || j.status == RenderJobStatus::Cancelled)
            done++;
    }
    if (total > 0)
        emit queueProgress(done * 100 / total);

    m_cancelRequested = false;

    // Advance to the next pending job.
    startNextJob();
}

// Resolve the Timeline a job renders. Priority:
//   1. job.timeline (the additive in-memory seam) — used as-is, not owned.
//      RM-1.5: track-matte wiring for THIS branch is NOT populated here —
//      the live Timeline's trackMatteEntries() is the matte SSOT (US-1016),
//      so the render reads it directly and nothing is synced before submitting.
//   2. ProjectFile::load(job.projectFilePath) into a fresh heap Timeline via
//      the genuine Timeline::restoreFromProject path (the same call
//      MainWindow::applyLoadedProjectData uses, MainWindow.cpp:3982). The
//      returned Timeline is heap-owned; *ownedOut carries it so the caller
//      deletes it after the render. Only THIS branch populates the matte
//      carrier from persisted ProjectData (see the block below).
Timeline *RenderQueue::resolveTimeline(const RenderJob &job,
                                       Timeline **ownedOut)
{
    if (ownedOut)
        *ownedOut = nullptr;

    if (job.timeline)
        return job.timeline;

    const QString projectPath = projectFileToLoad(job);
    if (projectPath.isEmpty()
        || !QFile::exists(projectPath))
        return nullptr;

    ProjectData data;
    if (!ProjectFile::load(projectPath, data))
        return nullptr;

    auto *tl = new Timeline();
    // US-1504: the project canvas the dynamic overlay (wiggle) normalizes with,
    // and (US-1520) the fps its expressions read, the same values MainWindow
    // gives the live Timeline.
    if (data.config.width > 0 && data.config.height > 0)
        tl->setProjectOutputConfig(data.config.width, data.config.height,
                                   data.config.explicitOutputResolution, data.config.fps);
    tl->restoreGeneratedCaptionOverlays(data.generatedCaptionOverlays);
    tl->restoreFromProject(data.videoTracks, data.audioTracks,
                           data.playheadPos, data.markIn, data.markOut,
                           data.zoomLevel);
    tl->setProperty(
        "timecodeBurnIn",
        QVariant::fromValue(data.loudnessSettings.value(
            QStringLiteral("_timecodeBurnIn")).toObject()));
    QVector<Light3D> projectLights;
    projectLights.reserve(data.projectLights.size());
    for (const QJsonValue &value : data.projectLights)
        projectLights.append(Light3D::fromJson(value.toObject()));
    Camera3D projectCamera;
    if (!data.projectCamera.isEmpty())
        projectCamera.fromJson(data.projectCamera);
    tl->setProjectLights(projectLights);
    tl->setProjectLightViewPosition(projectCamera.camera().position);
    tl->setProjectCamera(projectCamera);
    // US-1709: the expression audioLevel() envelope, published once from the
    // loaded audio tracks (the owned Timeline has no rebuild hook; the snapshot
    // stays as published for the whole export).
    tl->setExpressionAudioEnv(tl->buildExpressionAudioEnv(audiokf::sharedEnvelopeCache()));

    // RM-1.5: this branch is reached ONLY for a parentless project-FILE
    // load (job.timeline == nullptr). The job.timeline != nullptr paths
    // (File→Export MainWindow.cpp, mobile export, the live-Timeline batch
    // overload) early-return above at `if (job.timeline)` and render the
    // live Timeline's own matte entries (the matte SSOT since US-1016, so
    // there is nothing to sync before addJob). This block
    // does NOT cover those paths. Here we populate the freshly-rebuilt
    // Timeline's intrinsic matte carrier straight from the persisted
    // ProjectData so a file-path export applies the EXACT same track
    // matte the GUI preview showed. ProjectData's
    // QVector<TrackMatteClipEntry> is keyed by entry.clipId, written by
    // MainWindow::brushClipId == trackMatteClipKey (src/TrackMatteKey.h);
    // we re-canonicalise each parseable "track:clip" id through the
    // shared key so a hand-edited / legacy project can't desync the
    // consumer (tlrender::renderClipId uses the same formula).
    QHash<QString, TimelineTrackMatteEntry> matteEntries;
    matteEntries.reserve(data.trackMatteClipEntries.size());
    const auto canonicalKey = [](const QString &raw) { return canonicalClipKey(raw); };
    for (const TrackMatteClipEntry &entry : data.trackMatteClipEntries) {
        if (entry.clipId.isEmpty())
            continue;
        TimelineTrackMatteEntry e;
        e.matteType = entry.matteType;
        e.matteSourceClipId = canonicalKey(entry.matteSourceClipId);
        matteEntries.insert(canonicalKey(entry.clipId), e);
    }
    tl->setTrackMatteEntries(matteEntries);
    QHash<QString, RotoClipEntry> rotoEntries;
    rotoEntries.reserve(data.rotoClipEntries.size());
    for (const RotoClipEntry &entry : data.rotoClipEntries) {
        if (!entry.clipId.isEmpty())
            rotoEntries.insert(canonicalKey(entry.clipId), entry);
    }
    tl->setRotoEntries(rotoEntries);
    QHash<QString, QString> parentEntries;
    parentEntries.reserve(data.clipParentEntries.size());
    for (const ClipParentEntry &entry : data.clipParentEntries) {
        const QString child = canonicalKey(entry.clipId);
        const QString parent = canonicalKey(entry.parentClipId);
        if (!child.isEmpty() && !parent.isEmpty() && child != parent)
            parentEntries.insert(child, parent);
    }
    tl->setClipParentEntries(parentEntries);
    // The saved time-remap / freeze-frame curves live only in the sidecar
    // section, so the file-path export has to put them back on the clips.
    ProjectFile::applyTimeRemapEntries(tl, data.timeRemapClipEntries);

    if (ownedOut)
        *ownedOut = tl;
    return tl;
}

int RenderQueue::findJobIndex(int id) const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].id == id)
            return i;
    }
    return -1;
}

int RenderQueue::findJobIndexByUuid(const QString &uuid) const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].uuid == uuid)
            return i;
    }
    return -1;
}

QString RenderQueue::mapCodecToEncoderName(const QString &codec)
{
    // The flat-field codec uses Premiere/Resolve naming; map it to the
    // libavcore encoder name the render-pipe expects.
    if (codec == "h264") return "libx264";
    if (codec == "hevc" || codec == "h265") return "libx265";
    if (codec == "av1") return "libsvtav1";
    if (codec == "prores") return "prores_ks";
    return codec;  // already-prefixed names pass through (libx264, libvpx-vp9...)
}

QString RenderQueue::defaultContainerFor(const QString &codec)
{
    if (codec == "prores") return "mov";
    if (codec == "av1")    return "mp4";
    return "mp4";
}

QVector<RenderPreset> RenderQueue::availablePresets()
{
    // Built-in catalogue — Premiere Media Encoder / Resolve Deliver parity.
    // Bitrates expressed in bps to match the spec.
    return {
        { "1080p H.264 / 50 Mbps",            1920, 1080, "h264",   50000000,  "mp4" },
        { "1080p H.265 / 30 Mbps",            1920, 1080, "hevc",   30000000,  "mp4" },
        { "4K H.264 / 100 Mbps",              3840, 2160, "h264",  100000000,  "mp4" },
        { "4K H.265 / 60 Mbps",               3840, 2160, "hevc",   60000000,  "mp4" },
        { "720p H.264 / 8 Mbps (web)",        1280,  720, "h264",    8000000,  "mp4" },
        { "ProRes 422 LT",                    1920, 1080, "prores", 100000000,  "mov" },
        { "AV1 4K / 30 Mbps",                 3840, 2160, "av1",    30000000,  "mp4" },
        { "Twitter 720p H.264 / 6 Mbps",      1280,  720, "h264",    6000000,  "mp4" },
        { "YouTube 4K HDR (HEVC) / 80 Mbps",  3840, 2160, "hevc",   80000000,  "mp4" },
        { "Vimeo 1080p H.264 / 20 Mbps",      1920, 1080, "h264",   20000000,  "mp4" },
    };
}

RenderJob RenderQueue::jobFromPreset(const RenderPreset &preset,
                                     const QString &outputPath,
                                     qint64 startUs,
                                     qint64 endUs)
{
    RenderJob j;
    j.uuid = makeUuid();
    j.outputPath = outputPath;
    j.preset = preset.name;
    j.width = preset.width;
    j.height = preset.height;
    j.codec = preset.codec;
    j.bitrateBps = preset.bitrateBps;
    j.startUs = startUs;
    j.endUs = endUs;
    j.passes = 1;
    j.status = RenderJobStatus::Pending;
    j.name = preset.name;
    return j;
}
