#include "RenderInPlace.h"
#include "Timeline.h"
#include "RenderQueue.h"
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
        || name == QStringLiteral("scaleX") || name == QStringLiteral("scaleY");
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

    const double handles = options.handlesSec;
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
        if (original.hasTimeRemap() || !original.speedRamp.isIdentity()
            || isolated.inPoint < 0.0 || isolated.outPoint > original.duration) {
            isolated.reversed = false;
            isolated.speedRamp = speedramp::SpeedRamp::identity();
            isolated.timeRemapCurve.keys.clear();
            const int frames = int(std::ceil((length + 2.0 * handles) * options.fps));
            for (int f = 0; f <= frames; ++f) {
                const double t = f / options.fps;
                const double local = t - handles;
                double source = original.sourceSecondAtLocalTime(qBound(0.0, local, length));
                if (!original.hasTimeRemap() && original.speedRamp.isIdentity()) {
                    const double sourceOut = original.outPoint > 0.0 ? original.outPoint : original.duration;
                    source = original.reversed ? sourceOut - local * original.speed
                                               : original.inPoint + local * original.speed;
                }
                source = qBound(0.0, source, qMax(0.0, original.duration - 1.0 / options.fps));
                isolated.timeRemapCurve.keys.append({t, source - isolated.inPoint});
            }
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
    isolated.leadInSec = prefix - handles;
    Timeline temporary;
    temporary.restoreFromProject(QVector<QVector<ClipInfo>>{{isolated}},
                                 QVector<QVector<ClipInfo>>{linked}, 0.0, -1.0, -1.0, 10);
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
    // ProRes requires MOV; MP4 cannot mux ProRes in the existing encoder.
    const QString extension = options.codec == QStringLiteral("prores") ? QStringLiteral(".mov") : QStringLiteral(".mp4");
    QString path = QDir(directory).absoluteFilePath(name + extension);
    for (int suffix = 1; QFileInfo::exists(path); ++suffix)
        path = QDir(directory).absoluteFilePath(name + QStringLiteral("_%1").arg(suffix) + extension);
    RenderPreset preset{QStringLiteral("クリップ焼き込み"), outputSize.width(),
        outputSize.height(), options.codec, 100000000, extension.mid(1)};
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
    QTemporaryDir mixDirectory;
    if (!linked.isEmpty()) {
        if (!mixDirectory.isValid())
            return fail(QStringLiteral("リンク音声の一時フォルダーを作成できません"));
        QString mixError;
        const QString mixPath = prepareAudioMix(&temporary,
            mixDirectory.filePath(QStringLiteral("linked-audio.m4a")), &mixError);
        if (mixPath.isEmpty() || !mixError.isEmpty())
            return fail(mixError.isEmpty() ? QStringLiteral("リンク音声を準備できません") : mixError);
        job.projectFilePath = mixPath;
    }
    job.exportConfig["fps"] = options.fps;
    job.exportConfig["proresProfile"] = 3;
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
    replacement.label = original.label;
    replacement.volume = original.volume;
    replacement.pan = original.pan;
    replacement.audioChannelMode = original.audioChannelMode;
    replacement.volumeEnvelope = original.volumeEnvelope;
    replacement.renderInPlaceOriginal = std::make_shared<ClipInfo>(original);
    if (!timeline.replaceRenderedClip(trackIndex, clipIndex, replacement,
                                     QStringLiteral("効果を焼き込んで差し替え"))) {
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
    return original && timeline.replaceRenderedClip(trackIndex, clipIndex, *original,
                                                    QStringLiteral("元のクリップに戻す"));
}
}
