#include "RenderInPlace.h"
#include "ClipGeometry.h"
#include "Timeline.h"
#include "RenderQueue.h"
#include "RotoApply.h"
#include "UndoManager.h"
#include "libavcore/Probe.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <algorithm>
#include <limits>

namespace renderinplace {
namespace {
// These properties act on the layer after the material effects. Never flatten
// them into an opaque codec: the shared renderer must still composite them.
void copyComposition(const ClipInfo &from, ClipInfo &to)
{
    to.videoScale = from.videoScale;
    to.videoDx = from.videoDx;
    to.videoDy = from.videoDy;
    to.rotation2DDegrees = from.rotation2DDegrees;
    to.is3DLayer = from.is3DLayer;
    to.layer3D = from.layer3D;
    to.material = from.material;
    to.motionBlurEnabled = from.motionBlurEnabled;
    to.autoOrientEnabled = from.autoOrientEnabled;
    to.autoOrientToCamera = from.autoOrientToCamera;
    to.wiggle = from.wiggle;
    to.expressions = from.expressions;
    to.text3DConfig = from.text3DConfig;
    to.brushData = from.brushData;
    to.opacity = from.opacity;
    to.visible = from.visible;
    to.blendMode = from.blendMode;
    to.layerStyle = from.layerStyle;
    to.maskSystem = from.maskSystem;
    to.maskTrackingData = from.maskTrackingData;
    to.fitContain = from.fitContain;
    to.fitCover = from.fitCover;
}

bool compositionTrack(const QString &name)
{
    return name.startsWith(QStringLiteral("motion."))
        || name == QStringLiteral("positionX") || name == QStringLiteral("positionY")
        || name == QStringLiteral("scaleX") || name == QStringLiteral("scaleY")
        || name == QStringLiteral("brush_progress");
}

QSize nativeVideoSize(const QString &path)
{
    AVFormatContext *context = nullptr;
    QSize size;
    if (avformat_open_input(&context, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return size;
    if (avformat_find_stream_info(context, nullptr) >= 0) {
        const int stream = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream >= 0) {
            const auto *parameters = context->streams[stream]->codecpar;
            if (parameters->width > 0 && parameters->height > 0)
                size = QSize((parameters->width + 1) & ~1, (parameters->height + 1) & ~1);
        }
    }
    avformat_close_input(&context);
    return size;
}

// Presentation times (seconds) of the first video stream's frames in
// [fromSec, untilSec], ascending, computed as the reverse decoder computes
// frame pts. Empty when the media cannot be read.
QVector<double> probeVideoFrameTimes(const QString &path, double fromSec, double untilSec)
{
    QVector<double> times;
    AVFormatContext *context = nullptr;
    if (avformat_open_input(&context, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return times;
    int stream = -1;
    if (avformat_find_stream_info(context, nullptr) >= 0) {
        for (unsigned i = 0; i < context->nb_streams; ++i) {
            if (context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                stream = int(i);
                break;
            }
        }
    }
    AVPacket *packet = stream >= 0 ? av_packet_alloc() : nullptr;
    if (packet) {
        const AVRational timeBase = context->streams[stream]->time_base;
        const int64_t seekTarget = static_cast<int64_t>(fromSec * AV_TIME_BASE);
        if (seekTarget > 0)
            av_seek_frame(context, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
        // Packets arrive in decode order: read one second past the window so
        // reordered frames inside it are not missed.
        while (av_read_frame(context, packet) >= 0) {
            const bool video = packet->stream_index == stream;
            const int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            av_packet_unref(packet);
            if (!video || pts == AV_NOPTS_VALUE) continue;
            const double sec = static_cast<double>(pts) * av_q2d(timeBase);
            if (sec > untilSec + 1.0) break;
            if (sec + 1e-9 >= fromSec && sec <= untilSec) times.append(sec);
        }
        av_packet_free(&packet);
    }
    avformat_close_input(&context);
    std::sort(times.begin(), times.end());
    return times;
}
}

bool renderClipInPlace(Timeline &timeline, int trackIndex, int clipIndex,
                       const Options &options, QString *outPath, QString *error)
{
    if (outPath) outPath->clear();
    if (error) error->clear();
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (trackIndex < 0 || trackIndex >= timeline.videoTracks().size())
        return fail(QStringLiteral("映像トラックが見つかりません"));
    auto *track = timeline.videoTracks()[trackIndex];
    if (track->isLocked() || clipIndex < 0 || clipIndex >= track->clipCount())
        return fail(QStringLiteral("クリップを変更できません"));
    if (!std::isfinite(options.handlesSec) || options.handlesSec < 0.0
        || options.handlesSec > 5.0 || !std::isfinite(options.fps) || options.fps <= 0.0
        || options.outputSize.width() <= 0 || options.outputSize.height() <= 0
        || (options.codec != QStringLiteral("h264") && options.codec != QStringLiteral("prores")))
        return fail(QStringLiteral("書き出し設定が不正です"));
    const ClipInfo original = track->clips()[clipIndex];
    if (!std::isfinite(original.speed) || original.speed <= 0.0
        || !std::isfinite(original.duration) || original.duration <= 0.0)
        return fail(QStringLiteral("クリップの尺または速度が不正です"));
    const double length = original.effectiveDuration();
    if (!std::isfinite(length) || length <= 0.0 || original.isSequenceReference()
        || original.isAdjustment)
        return fail(QStringLiteral("このクリップは焼き込みに対応していません"));

    // Keep the retained interval on the export frame grid, including when
    // requested handles contain a fractional frame.
    const auto snap = [&](double seconds) {
        return std::ceil(seconds * options.fps - 1e-6) / options.fps;
    };
    const double handles = snap(options.handlesSec);
    if (isOverlapTransition(original.leadIn.type) || isOverlapTransition(original.trailOut.type))
        return fail(QStringLiteral("重ね合わせトランジション付きクリップは焼き込めません。先にトランジションを解除してください"));
    // The available codecs flatten alpha to black. Inspect the entire stack,
    // including effects whose active interval/keyframes exclude the current frame.
    for (const auto &effect : original.effects) {
        if (effect.type == VideoEffectType::ChromaKey && effect.enabled)
            return fail(QStringLiteral("クロマキー付きクリップは透明部分を保持できないため焼き込めません。先にクロマキーを無効にしてください"));
    }
    if (!original.maskTrackingData.isEmpty() || !original.stabilizerKeyframes.isEmpty())
        return fail(QStringLiteral("追跡マスクまたはスタビライズ付きクリップの焼き込みには対応していません"));
    const QSize outputSize = nativeVideoSize(original.filePath);
    if (outputSize.isEmpty())
        return fail(QStringLiteral("素材の映像サイズを取得できません"));
    // Edge transitions are evaluated relative to clip boundaries by the
    // shared renderer. Extending those boundaries would move the transition
    // in the retained region. Do not silently change that picture.
    if (handles > 0.0 && (original.leadIn.type != TransitionType::None
                         || original.trailOut.type != TransitionType::None))
        return fail(QStringLiteral("トランジション付きクリップの焼き込みは、ハンドル秒を0に設定してください"));
    ClipInfo isolated = original;
    isolated.renderInPlaceOriginal.reset();
    copyComposition(ClipInfo{}, isolated);
    for (const auto &keys : original.keyframes.tracks()) {
        if (compositionTrack(keys.propertyName()))
            isolated.keyframes.removeTrack(keys.propertyName());
    }
    isolated.leadInSec = 0.0;
    isolated.linkGroup = 0;
    if (handles > 0.0) {
        // Extend source handles in timeline seconds. Keep animated parameters
        // at the same time in the retained portion of the new media.
        isolated.inPoint -= handles * original.speed;
        isolated.outPoint = (original.outPoint > 0.0 ? original.outPoint : original.duration)
            + handles * original.speed;
        for (auto &keys : isolated.keyframes.tracks()) {
            const auto points = keys.keyframes();
            for (int i = points.size() - 1; i >= 0; --i) {
                auto point = points[i];
                point.time += handles;
                keys.setKeyframePoint(i, point);
            }
        }
        for (auto &keys : isolated.keyframes.stringTracks()) {
            const auto points = keys.keyframes();
            for (int i = points.size() - 1; i >= 0; --i)
                keys.setKeyframeTime(i, points[i].time + handles);
        }
        for (auto &effect : isolated.effects) {
            if (effect.startSec >= 0.0) effect.startSec += handles;
            if (effect.endSec >= 0.0) effect.endSec += handles;
        }
        auto overlays = isolated.textManager.overlays();
        for (auto &overlay : overlays) {
            overlay.startTime += handles;
            if (overlay.endTime > 0.0) overlay.endTime += handles;
        }
        isolated.textManager.setOverlays(overlays);
        // Preserve the nonlinear source map over the retained interval.
        // Sampling at export ticks uses ClipInfo's existing mapping, never a
        // second speed-ramp integrator. Boundary handles hold when exhausted.
        // A reversed clip stays reversed with an ascending curve:
        // sourceSecondAtLocalTime folds it back to the same descending source
        // second, so the renderer keeps its reverse sampling (last frame before
        // that second, roto one source frame earlier) as on the source timeline.
        // Tail rule: the reverse decoder takes its lower bound from the lowered
        // isolated inPoint (the roto clamp of retained samples reads
        // original.inPoint, see renderInPlaceOriginal below). The decoder
        // picks frames by the stream's real timestamps (the last frame before
        // the second at or after the in point, else the first frame at or
        // after it), and roto indexes by sourceFps, which can differ from the
        // stream rate.
        // A retained sample whose source time runs backward and that lies at or
        // before the first frame at or after original.inPoint, or below
        // original.inPoint + 1/sourceFps, is therefore moved (still strictly
        // below the previous backward sample) into the stream frame interval
        // that decodes the source timeline's frame, intersected with the
        // seconds whose roto index matches the source timeline's. If no second
        // satisfies both, the picture wins and a warning is logged. Samples
        // whose source time runs forward (a reversed clip with a descending
        // curve) keep their source second: the source timeline samples them
        // forward without the reverse roto offset.
        // A reversed clip always takes this branch, also when its handles stay
        // inside the media: the lowered inPoint moves the tail otherwise.
        if (original.hasTimeRemap() || !original.speedRamp.isIdentity()
            || isolated.inPoint < 0.0 || isolated.outPoint > original.duration
            || original.reversed) {
            isolated.speedRamp = speedramp::SpeedRamp::identity();
            isolated.timeRemapCurve.keys.clear();
            // The renderer indexes roto by the curve's sourceFps once the clip
            // has a curve. A synthetic curve must carry the rate the source
            // timeline uses (stream rate, else project fps), not the 30 Hz
            // default. The picture decode does not read sourceFps.
            if (!original.hasTimeRemap() || !(original.timeRemapCurve.sourceFps > 0.0))
                isolated.timeRemapCurve.sourceFps = roto::clipEffectiveSourceFps(
                    roto::probeVideoStreamFps(original.filePath), timeline.projectFrameRate());
            const int frames = int(std::ceil((length + 2.0 * handles) * options.fps));
            const double sourceFps = isolated.timeRemapCurve.sourceFps;
            // Frame times from original.inPoint on (real pts, else the probed
            // stream-rate grid), never the sourceFps grid.
            QVector<double> frameTimes;
            if (isolated.reversed && sourceFps > 0.0) {
                const double untilSec = original.inPoint + 1.0 / sourceFps + 1.0;
                frameTimes = probeVideoFrameTimes(original.filePath, original.inPoint, untilSec);
                if (frameTimes.isEmpty()) {
                    const double streamFps = roto::clipEffectiveSourceFps(
                        roto::probeVideoStreamFps(original.filePath), timeline.projectFrameRate());
                    for (qint64 n = qint64(std::ceil(original.inPoint * streamFps - 1e-6));
                         n / streamFps <= untilSec; ++n)
                        frameTimes.append(n / streamFps);
                }
                // The renderer bounds the roto second of retained samples
                // (source second >= original.inPoint) by the in point of this
                // same-media original, as on the source timeline. Without it a
                // stream faster than sourceFps leaves no second in a tail frame
                // that indexes the source timeline's roto frame.
                isolated.renderInPlaceOriginal = std::make_shared<ClipInfo>(original);
            }
            // The renderer's reverse roto second (TimelineFrameRenderer applyRoto).
            const auto rotoIndexAt = [&](double sourceSec) {
                const double inPoint = sourceSec >= original.inPoint ? original.inPoint : isolated.inPoint;
                return roto::sourceFrameIndexFor(qMax(inPoint, sourceSec - 1.0 / sourceFps), sourceFps);
            };
            // Samples past the last real frame (bake fps above the stream
            // rate, clamped one bake frame before the media end) must still
            // decode it. The renderer reads the direction from the isolated
            // curve: a flat run decodes forward, finds no frame at or after
            // its second, and the queue refuses the null image. Forward-running
            // samples hold the last real frame. Backward-running samples of a
            // reversed clip, including head-handle samples, are bounded by the
            // media end instead of one bake frame before it (that bound can
            // fall below the last frame and flatten the head) and stay
            // strictly descending above the last frame, so the reverse decoder
            // presents the last frame (EOF predecessor).
            const QVector<double> endFrameTimes = probeVideoFrameTimes(
                original.filePath, qMax(0.0, original.duration - 1.0), original.duration + 1.0);
            const double lastFrameSec = !endFrameTimes.isEmpty()
                ? endFrameTimes.last()
                : original.duration - 1.0 / roto::clipEffectiveSourceFps(
                      roto::probeVideoStreamFps(original.filePath), timeline.projectFrameRate());
            double previousSource = std::numeric_limits<double>::infinity();
            int rotoMismatches = 0;
            int mismatchSourceRoto = -1;
            int mismatchBakedRoto = -1;
            for (int f = 0; f <= frames; ++f) {
                const double t = f / options.fps;
                const double local = t - handles;
                double source = original.sourceSecondAtLocalTime(qBound(0.0, local, length));
                if (!original.hasTimeRemap() && original.speedRamp.isIdentity()) {
                    const double sourceOut = original.outPoint > 0.0 ? original.outPoint : original.duration;
                    source = original.reversed ? sourceOut - local * original.speed
                                               : original.inPoint + local * original.speed;
                }
                const bool runsBackward = original.sourceTimeRunsBackwardAtLocalTime(qBound(0.0, local, length));
                source = original.reversed && runsBackward
                    ? qBound(0.0, source, original.duration)
                    : qBound(0.0, source, qMax(0.0, original.duration - 1.0 / options.fps));
                if (!runsBackward && source > lastFrameSec + 1e-6)
                    source = lastFrameSec;
                if (runsBackward && source > lastFrameSec + 1e-6)
                    source = qMin(source, previousSource - 1e-6);
                if (!frameTimes.isEmpty() && runsBackward && local > -1e-9 && local < length - 1e-9) {
                    // k = the decoder's frame on the source timeline: the last
                    // frame before source (-1: the first frame at or after it).
                    int k = -1;
                    while (k + 1 < frameTimes.size() && frameTimes[k + 1] + 1e-9 < source) ++k;
                    if (k < 0 || source < original.inPoint + 1.0 / sourceFps) {
                        const int pictureIndex = qMax(0, k);
                        const double pictureLo = frameTimes[pictureIndex];
                        const double pictureHi = pictureIndex + 1 < frameTimes.size()
                            ? frameTimes[pictureIndex + 1] : std::numeric_limits<double>::infinity();
                        // The decoder seeks to the nearest stream tick, which
                        // lands on the next frame from the upper half of the
                        // interval: stay in the lower half.
                        const double pictureMid = pictureLo + (pictureHi - pictureLo) / 2.0;
                        const int sourceRoto = rotoIndexAt(source);
                        const double rotoHi = (sourceRoto + 1.5) / sourceFps;
                        double adjusted = qMin(qMin(pictureMid, rotoHi), previousSource) - 1e-6;
                        if (!(adjusted > pictureLo + 1e-6)
                            || rotoIndexAt(adjusted) != sourceRoto) {
                            adjusted = qMin(pictureMid, previousSource) - 1e-6;
                            if (rotoMismatches++ == 0) {
                                mismatchSourceRoto = sourceRoto;
                                mismatchBakedRoto = rotoIndexAt(adjusted);
                            }
                        }
                        source = adjusted;
                    }
                }
                previousSource = runsBackward ? source : std::numeric_limits<double>::infinity();
                isolated.timeRemapCurve.keys.append({t, isolated.reversed ? isolated.outPoint - source
                                                                          : source - isolated.inPoint});
            }
            if (rotoMismatches > 0)
                qWarning("RenderInPlace: reversed clip '%s': %d tail frame(s) bake roto frame %d instead of %d",
                         qPrintable(original.displayName.isEmpty() ? original.filePath : original.displayName),
                         rotoMismatches, mismatchBakedRoto, mismatchSourceRoto);
        }
    }
    // A1 contains only linked audio, positioned relative to the baked video.
    // Non-overlapping linked pieces can share the same rendered audio stream.
    const auto startAt = [](const TimelineTrack *t, int index) {
        double start = 0.0;
        for (int i = 0; i <= index; ++i) {
            start += t->clips()[i].leadInSec;
            if (i < index) start += t->clips()[i].effectiveDuration();
        }
        return start;
    };
    const double videoStart = startAt(track, clipIndex);
    double prefix = handles;
    double suffix = handles;
    QVector<ClipInfo> linked;
    if (original.linkGroup != 0) {
        for (const auto *audioTrack : timeline.audioTracks()) {
            for (int i = 0; i < audioTrack->clipCount(); ++i) {
                ClipInfo audio = audioTrack->clips()[i];
                if (audio.linkGroup != original.linkGroup) continue;
                if (audioTrack->isLocked())
                    return fail(QStringLiteral("リンク音声トラックがロックされています"));
                const double relative = startAt(audioTrack, i) - videoStart;
                if (!std::isfinite(audio.effectiveDuration()) || audio.effectiveDuration() <= 0.0
                    || !std::isfinite(relative))
                    return fail(QStringLiteral("リンク音声の尺が不正です"));
                prefix = qMax(prefix, -relative);
                suffix = qMax(suffix, relative + audio.effectiveDuration() - length);
                audio.leadInSec = relative;
                audio.linkGroup = 0;
                audio.renderInPlaceOriginal.reset();
                linked.append(audio);
            }
        }
    }
    // Audio can extend the job beyond the handles. Snap before positioning
    // either track so the video source clock and replacement trim agree.
    prefix = snap(prefix);
    suffix = snap(suffix);
    std::sort(linked.begin(), linked.end(), [](const ClipInfo &a, const ClipInfo &b) {
        return a.leadInSec < b.leadInSec;
    });
    double audioEnd = 0.0;
    for (auto &audio : linked) {
        const double start = audio.leadInSec + prefix;
        if (start < audioEnd - 1e-6)
            return fail(QStringLiteral("重なったリンク音声は焼き込めません"));
        audio.leadInSec = qMax(0.0, start - audioEnd);
        audioEnd = start + audio.effectiveDuration();
    }
    // Linked audio may extend beyond either video handle. Give every export
    // frame an active V1 clip: null objects render black in the opaque output.
    // Keep the isolated clip's local clock unchanged so retained effects,
    // keyframes and source-time mapping still start at prefix in the media.
    QVector<ClipInfo> isolatedVideo;
    const auto appendBlack = [&](double duration) {
        if (duration <= 0.0) return;
        ClipInfo black{};
        black.filePath = clipgeom::nullObjectFilePath();
        black.duration = duration;
        black.outPoint = duration;
        isolatedVideo.append(black);
    };
    appendBlack(prefix - handles);
    const int prefixCount = isolatedVideo.size();
    isolatedVideo.append(isolated);
    appendBlack(suffix - handles);
    Timeline temporary;
    temporary.restoreFromProject(QVector<QVector<ClipInfo>>{isolatedVideo},
                                 QVector<QVector<ClipInfo>>{linked}, 0.0, -1.0, -1.0, 10);
    // Bake the clip's roto entry: key it to the isolated clip (after the
    // optional prefix black clip). The isolated clip maps each frame to the
    // same source second as on the source timeline (directly, or through the
    // synthetic curve whose sourceFps is the source timeline's roto rate) and
    // keeps the reversed flag, so the renderer samples the same picture frame
    // and the same roto source frame (one frame earlier where the source time
    // runs backward). The exception is the reversed tail: the decoder lower
    // bound reads the lowered isolated inPoint, so backward-running
    // samples up to the stream's first frame at or after original.inPoint (by
    // its real timestamps) or within one sourceFps frame of original.inPoint
    // are moved in the curve above to a second that decodes the same stream
    // frame and indexes the same roto frame (picture first, with a warning,
    // when no second does both).
    const QString rotoKey = trackMatteClipKey(trackIndex, clipIndex);
    const QHash<QString, RotoClipEntry> sourceRoto = timeline.rotoEntries();
    const bool bakesRoto = sourceRoto.contains(rotoKey);
    if (bakesRoto)
        temporary.setRotoEntries({{trackMatteClipKey(0, prefixCount), sourceRoto.value(rotoKey)}});
    temporary.setProjectOutputConfig(outputSize.width(), outputSize.height(), true);

    QString directory = options.outputDir;
    if (directory.isEmpty()) {
        directory = options.projectFilePath.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            : QFileInfo(options.projectFilePath).absolutePath();
        directory = QDir(directory).filePath(QStringLiteral("RenderInPlace"));
    }
    if (!QDir().mkpath(directory))
        return fail(QStringLiteral("出力先を作成できません"));
    QString name = QFileInfo(original.displayName.isEmpty() ? original.filePath : original.displayName).completeBaseName();
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), QStringLiteral("_"));
    if (name.isEmpty()) name = QStringLiteral("clip");
    name += QDateTime::currentDateTime().toString(QStringLiteral("_yyyyMMdd_hhmmss"));
    // Roto leaves alpha 0 outside the path and lower tracks show through it.
    // H.264 and ProRes 422 flatten that to black, so a roto bake always uses
    // the alpha path: ProRes 4444 (keepAlpha) in MOV.
    const QString codec = bakesRoto ? QStringLiteral("prores") : options.codec;
    // ProRes requires MOV; MP4 cannot mux ProRes in the existing encoder.
    const QString extension = codec == QStringLiteral("prores") ? QStringLiteral(".mov") : QStringLiteral(".mp4");
    QString path = QDir(directory).absoluteFilePath(name + extension);
    for (int suffix = 1; QFileInfo::exists(path); ++suffix)
        path = QDir(directory).absoluteFilePath(name + QStringLiteral("_%1").arg(suffix) + extension);
    RenderPreset preset{QStringLiteral("クリップ焼き込み"), outputSize.width(),
        outputSize.height(), codec, 100000000, extension.mid(1)};
    RenderJob job = RenderQueue::jobFromPreset(preset, path, 0,
        qRound64((length + prefix + suffix) * 1000000.0));
    job.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.timeline = &temporary;
    // RenderQueue falls back to V1 audio when projectFilePath is empty.
    // Supply a valid media input with no audio stream so its existing audio
    // probe disables passthrough. Keep it alive until the queue has finished.
    QTemporaryFile silentInput(QDir(directory).filePath(QStringLiteral(".render-in-place-XXXXXX.png")));
    if (!silentInput.open())
        return fail(QStringLiteral("一時ファイルを作成できません"));
    silentInput.close();
    QImage silentFrame(2, 2, QImage::Format_RGB32);
    silentFrame.fill(Qt::black);
    if (!silentFrame.save(silentInput.fileName(), "PNG"))
        return fail(QStringLiteral("音声なしの入力を準備できません"));
    job.projectFilePath = silentInput.fileName();
    // .veditor is not an audio-mix trigger in RenderQueue: it falls back to
    // V1 media. Prepare the normal export mix and mux it in this same job.
    QTemporaryDir mixDirectory(QDir(directory).filePath(QStringLiteral(".render-in-place-mix-XXXXXX")));
    if (!linked.isEmpty()) {
        if (!mixDirectory.isValid())
            return fail(QStringLiteral("リンク音声の一時フォルダーを作成できません"));
        // Keep the intermediate beneath outputDir when a selftest needs diagnostics.
        if (options.retainAudioMixForDiagnostics)
            mixDirectory.setAutoRemove(false);
        QString mixError;
        const QString mixPath = prepareAudioMix(&temporary,
            mixDirectory.filePath(QStringLiteral("linked-audio.m4a")),
            length + prefix + suffix, &mixError);
        if (mixPath.isEmpty() || !mixError.isEmpty())
            return fail(mixError.isEmpty() ? QStringLiteral("リンク音声を準備できません") : mixError);
        job.projectFilePath = mixPath;
    }
    job.exportConfig["fps"] = options.fps;
    job.exportConfig["proresProfile"] = bakesRoto ? 4 : 3;
    if (bakesRoto)
        job.exportConfig["keepAlpha"] = true;
    RenderQueue queue;
    QEventLoop loop;
    bool completed = false;
    bool success = false;
    QString renderError;
    QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
        [&](const QString &uuid, bool ok, const QString &message) {
            if (uuid != job.uuid) return;
            completed = true;
            success = ok;
            renderError = message;
            loop.quit();
        });
    // Nested event processing must not silently replace a clip edited by an
    // unrelated callback while encoding was in progress.
    bool changed = false;
    const auto connection = QObject::connect(timeline.undoManager(), &UndoManager::stateChanged,
        &loop, [&]() { changed = true; });
    if (options.connectProgress) options.connectProgress(queue);
    queue.addJob(job);
    queue.start();
    if (!completed) loop.exec();
    QObject::disconnect(connection);
    if (!success) {
        QFile::remove(path);
        return fail(renderError.isEmpty() ? QStringLiteral("焼き込みを中止しました") : renderError);
    }
    const auto duration = libavcore::probeDurationMicroseconds(path.toStdString());
    if (!duration || *duration <= 0 || changed) {
        QFile::remove(path);
        return fail(changed ? QStringLiteral("処理中にタイムラインが変更されました")
                            : QStringLiteral("出力メディアを確認できません"));
    }
    // Reset baked material/time properties and retain the live composition.
    ClipInfo replacement{};
    copyComposition(original, replacement);
    for (const auto &keys : original.keyframes.tracks()) {
        if (compositionTrack(keys.propertyName()))
            replacement.keyframes.addTrack(keys);
    }
    replacement.isVfxFootage = original.isVfxFootage;
    replacement.vfxIntensity = 1.0;
    // VFX black-level zero is identity; ordinary clips retain their default.
    replacement.vfxBlackLevel = original.isVfxFootage ? 0 : replacement.vfxBlackLevel;
    replacement.filePath = path;
    replacement.displayName = original.displayName;
    replacement.duration = double(*duration) / 1000000.0;
    replacement.inPoint = prefix;
    replacement.outPoint = prefix + length;
    replacement.leadInSec = original.leadInSec;
    replacement.linkGroup = original.linkGroup;
    // US-1214: the baked clip is the same clip (identity and connection).
    replacement.uid = original.uid;
    replacement.anchorUid = original.anchorUid;
    replacement.anchorOffsetSec = original.anchorOffsetSec;
    replacement.label = original.label;
    replacement.volume = original.volume;
    replacement.pan = original.pan;
    replacement.audioChannelMode = original.audioChannelMode;
    replacement.volumeEnvelope = original.volumeEnvelope;
    replacement.renderInPlaceOriginal = std::make_shared<ClipInfo>(original);
    // The baked media already carries the roto. Drop the entry so it is not
    // applied twice; replaceRenderedClip's undo state records the removal, so
    // undo restores it together with the original clip.
    const QHash<QString, RotoClipEntry> rotoBeforeReplace = timeline.rotoEntries();
    if (rotoBeforeReplace.contains(rotoKey)) {
        replacement.renderInPlaceOriginalRoto =
            std::make_shared<RotoClipEntry>(rotoBeforeReplace.value(rotoKey));
        QHash<QString, RotoClipEntry> withoutBaked = rotoBeforeReplace;
        withoutBaked.remove(rotoKey);
        timeline.setRotoEntries(withoutBaked);
    }
    if (!timeline.replaceRenderedClip(trackIndex, clipIndex, replacement,
                                     QStringLiteral("効果を焼き込んで差し替え"))) {
        if (rotoBeforeReplace.contains(rotoKey))
            timeline.setRotoEntries(rotoBeforeReplace);
        QFile::remove(path);
        return fail(QStringLiteral("クリップを差し替えられません"));
    }
    if (outPath) *outPath = path;
    return true;
}

bool decomposeRenderInPlace(Timeline &timeline, int trackIndex, int clipIndex)
{
    if (trackIndex < 0 || trackIndex >= timeline.videoTracks().size()) return false;
    const auto *track = timeline.videoTracks()[trackIndex];
    if (clipIndex < 0 || clipIndex >= track->clipCount()) return false;
    const auto original = track->clips()[clipIndex].renderInPlaceOriginal;
    if (!original) return false;
    // Restore the baked-away roto first (no undo point of its own) so the
    // single undo state saved by replaceRenderedClip covers both.
    const auto originalRoto = track->clips()[clipIndex].renderInPlaceOriginalRoto;
    const QHash<QString, RotoClipEntry> rotoBefore = timeline.rotoEntries();
    if (originalRoto) {
        QHash<QString, RotoClipEntry> restored = rotoBefore;
        restored.insert(trackMatteClipKey(trackIndex, clipIndex), *originalRoto);
        timeline.setRotoEntries(restored);
    }
    if (timeline.replaceRenderedClip(trackIndex, clipIndex, *original,
                                     QStringLiteral("元のクリップに戻す")))
        return true;
    if (originalRoto) timeline.setRotoEntries(rotoBefore);
    return false;
}
}
