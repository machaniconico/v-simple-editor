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
#include <QUuid>
#include <cmath>

namespace renderinplace {
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
    // Edge transitions are evaluated relative to clip boundaries by the
    // shared renderer. Extending those boundaries would move the transition
    // in the retained region. Do not silently change that picture.
    if (handles > 0.0 && (original.leadIn.type != TransitionType::None
                         || original.trailOut.type != TransitionType::None))
        return fail(QStringLiteral("トランジション付きクリップの焼き込みは、ハンドル秒を0に設定してください"));
    ClipInfo isolated = original;
    isolated.renderInPlaceOriginal.reset();
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
    Timeline temporary;
    temporary.restoreFromProject(QVector<QVector<ClipInfo>>{{isolated}},
                                 QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 10);
    temporary.setProjectOutputConfig(options.outputSize.width(), options.outputSize.height(), true);

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
    RenderPreset preset{QStringLiteral("クリップ焼き込み"), options.outputSize.width(),
        options.outputSize.height(), options.codec, 100000000, extension.mid(1)};
    RenderJob job = RenderQueue::jobFromPreset(preset, path, 0,
        qRound64((length + 2.0 * handles) * 1000000.0));
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
    // A fresh ClipInfo resets every baked visual/temporal field, including
    // new effects added later. Preserve only editing/audio metadata.
    ClipInfo replacement{};
    replacement.filePath = path;
    replacement.displayName = original.displayName;
    replacement.duration = double(*duration) / 1000000.0;
    replacement.inPoint = handles;
    replacement.outPoint = handles + length;
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
