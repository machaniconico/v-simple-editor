#include "../RenderInPlace.h"
#include "../RenderQueue.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../ProjectFile.h"
#include "../UndoManager.h"
#include "../libavcore/Probe.h"
#include <QFileInfo>
#include <QDir>
#include <QEventLoop>
#include <QSet>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <cmath>
#include <limits>

namespace {
QJsonObject clipJson(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{{clip}};
    return QJsonDocument::fromJson(ProjectFile::toJsonString(data).toUtf8()).object()
        .value("videoTracks").toArray().at(0).toArray().at(0).toObject();
}
// Independent encode/decode control: export the untouched, one-clip timeline
// directly. This deliberately does not call renderClipInPlace or construct its
// isolated/replacement clips. It measures the actual queue codec + RGB/YUV
// round trip, including the encoder fallback available on the acceptance host.
bool renderControl(Timeline &timeline, const renderinplace::Options &options,
                   QString *path, QString *error)
{
    *path = QDir(options.outputDir).filePath(QStringLiteral("control.mp4"));
    const QString silentPath = QDir(options.outputDir).filePath(QStringLiteral("silent.png"));
    QImage silent(2, 2, QImage::Format_RGB32);
    silent.fill(Qt::black);
    if (!silent.save(silentPath)) {
        *error = QStringLiteral("control: cannot save silent input");
        return false;
    }
    RenderPreset preset{QStringLiteral("対照書き出し"), options.outputSize.width(),
        options.outputSize.height(), options.codec, 100000000, QStringLiteral("mp4")};
    RenderJob job = RenderQueue::jobFromPreset(preset, *path, 0,
        qRound64(timeline.totalDuration() * 1000000.0));
    job.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.timeline = &timeline;
    job.projectFilePath = silentPath;
    job.exportConfig["fps"] = options.fps;
    RenderQueue queue;
    QEventLoop loop;
    bool done = false, success = false;
    QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
        [&](const QString &uuid, bool ok, const QString &message) {
            if (uuid != job.uuid) return;
            done = true;
            success = ok;
            *error = message;
            loop.quit();
        });
    queue.addJob(job);
    queue.start();
    if (!done) loop.exec();
    return success;
}

bool jsonEqual(const char *label, const QJsonObject &expected, const QJsonObject &actual)
{
    QSet<QString> keys;
    for (const QString &key : expected.keys()) keys.insert(key);
    for (const QString &key : actual.keys()) keys.insert(key);
    for (const QString &key : keys) {
        if (expected.value(key) == actual.value(key)) continue;
        const auto describe = [](const QJsonValue &value) {
            return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
        };
        std::fprintf(stderr, "%s diff key=%s expected=%s actual=%s\n", label,
            qPrintable(key), describe(expected.value(key)).constData(),
            describe(actual.value(key)).constData());
    }
    return expected == actual;
}

void saveFrameDifference(const QString &directory, int index,
                         const QImage &before, const QImage &after)
{
    const QString prefix = QDir(directory).filePath(QStringLiteral("G1_%1_").arg(index));
    const bool beforeSaved = before.save(prefix + QStringLiteral("before.png"));
    const bool afterSaved = after.save(prefix + QStringLiteral("after.png"));
    if (before.isNull() || after.isNull() || before.size() != after.size()) {
        std::fprintf(stderr, "G1 frame %d invalid dimensions: %dx%d / %dx%d\n",
            index, before.width(), before.height(), after.width(), after.height());
        return;
    }
    const QImage a = before.convertToFormat(QImage::Format_RGB32);
    const QImage b = after.convertToFormat(QImage::Format_RGB32);
    QImage difference(a.size(), QImage::Format_RGB32);
    double signedSum[3] = {};
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb left = a.pixel(x, y), right = b.pixel(x, y);
            const int r = qRed(right) - qRed(left);
            const int g = qGreen(right) - qGreen(left);
            const int bl = qBlue(right) - qBlue(left);
            signedSum[0] += r; signedSum[1] += g; signedSum[2] += bl;
            difference.setPixel(x, y, qRgb(std::abs(r), std::abs(g), std::abs(bl)));
        }
    }
    const double pixels = double(a.width()) * a.height();
    const bool diffSaved = difference.save(prefix + QStringLiteral("diff.png"));
    std::fprintf(stderr, "G1 frame %d mean RGB bias=(%.6f, %.6f, %.6f), PNG saved=%d/%d/%d\n",
        index, signedSum[0] / pixels, signedSum[1] / pixels, signedSum[2] / pixels,
        int(beforeSaved), int(afterSaved), int(diffSaved));
}

bool videoOnly(const QString &path)
{
    AVFormatContext *context = nullptr;
    if (avformat_open_input(&context, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    bool video = false, audio = false;
    const bool probed = avformat_find_stream_info(context, nullptr) >= 0;
    if (probed) {
        for (unsigned i = 0; i < context->nb_streams; ++i) {
            video |= context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
            audio |= context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
        }
    }
    avformat_close_input(&context);
    return probed && video && !audio;
}
double mse(const QImage &left, const QImage &right)
{
    if (left.isNull() || right.isNull() || left.size() != right.size())
        return std::numeric_limits<double>::infinity();
    const auto a = left.convertToFormat(QImage::Format_RGB32);
    const auto b = right.convertToFormat(QImage::Format_RGB32);
    double sum = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const double r = qRed(pa[x]) - qRed(pb[x]);
            const double g = qGreen(pa[x]) - qGreen(pb[x]);
            const double bl = qBlue(pa[x]) - qBlue(pb[x]);
            sum += r*r + g*g + bl*bl;
        }
    }
    return sum / (a.width() * a.height() * 3.0);
}
}

int runRenderInPlaceSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    QTemporaryDir output;
    ClipInfo original{};
    original.filePath = QFileInfo(QStringLiteral("test_assets/e2e_clip.mp4")).absoluteFilePath();
    original.displayName = QStringLiteral("焼き込みテスト");
    const auto sourceDuration = libavcore::probeDurationMicroseconds(original.filePath.toStdString());
    original.duration = sourceDuration ? double(*sourceDuration) / 1000000.0 : 0.0;
    if (!output.isValid() || !QFileInfo::exists(original.filePath)
        || !sourceDuration || original.duration < 3.0) {
        std::fprintf(stderr, "render-in-place prerequisite FAIL: outputValid=%d, "
            "fixture=%s, exists=%d, duration=%.6f (required >= 3.0 seconds); "
            "run from the repository root\n", int(output.isValid()),
            qPrintable(original.filePath), int(QFileInfo::exists(original.filePath)), original.duration);
        for (int number = 1; number <= 8; ++number) gate(number, false);
        std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
        return failed;
    }
    original.inPoint = 1.0;
    original.outPoint = 2.0;
    original.effects.append(VideoEffect::createBrightnessContrast(12.0, 0.0));
    Timeline timeline;
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{original}},
                                QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
    timeline.undoManager()->clear();
    timeline.saveUndoState(QStringLiteral("テスト初期状態"));
    renderinplace::Options options;
    options.outputDir = output.path();
    options.outputSize = QSize(640, 360);
    options.fps = 30;
    const QJsonObject before = clipJson(original);
    QVector<QImage> frames;
    for (qint64 tick : {100000LL, 500000LL, 900000LL})
        frames.append(tlrender::renderFrameAt(&timeline, tick, options.outputSize));
    QString path, error, controlPath;
    const bool controlRendered = renderControl(timeline, options, &controlPath, &error);
    if (!controlRendered) std::fprintf(stderr, "G1 control export failed: %s\n", qPrintable(error));
    ClipInfo controlClip{};
    controlClip.filePath = controlPath;
    controlClip.duration = original.effectiveDuration();
    Timeline controlTimeline;
    controlTimeline.restoreFromProject(QVector<QVector<ClipInfo>>{{controlClip}},
        QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
    const quint64 serial = timeline.undoManager()->saveSerial();
    const bool baked = renderinplace::renderClipInPlace(timeline, 0, 0, options, &path, &error);
    if (!baked) std::fprintf(stderr, "G1 bake failed: %s\n", qPrintable(error));
    bool pictureMatches = baked && controlRendered;
    QVector<QImage> afterFrames;
    int index = 0;
    // A YUV-to-YUV transcode does not measure the queue's RGB round trip.
    // The queue consumes RGB
    // after effects and resamples chroma with SWS_BILINEAR on both legs.
    // Use the measured control floor for EACH frame, with a 0.25 MSE margin;
    // also compare to the decoded control directly to catch time/colour shifts.
    constexpr double margin = 0.25;
    for (qint64 tick : {100000LL, 500000LL, 900000LL}) {
        const QImage after = tlrender::renderFrameAt(&timeline, tick, options.outputSize);
        const QImage control = tlrender::renderFrameAt(&controlTimeline, tick, options.outputSize);
        afterFrames.append(after);
        const double actualMse = mse(frames[index], after);
        const double controlMse = mse(frames[index], control);
        const double residual = mse(control, after);
        const bool ok = std::isfinite(actualMse) && std::isfinite(controlMse)
            && actualMse <= controlMse + margin && residual <= margin;
        std::fprintf(stderr, "G1 frame %d tick=%lld MSE=%.6f control=%.6f "
            "residual=%.6f margin=%.2f %s\n", index, static_cast<long long>(tick),
            actualMse, controlMse, residual, margin, ok ? "OK" : "FAIL");
        pictureMatches = pictureMatches && ok;
        ++index;
    }
    if (!pictureMatches) {
        output.setAutoRemove(false);
        std::fprintf(stderr, "G1 diagnostics retained in %s\n", qPrintable(output.path()));
        index = 0;
        for (qint64 tick : {100000LL, 500000LL, 900000LL}) {
            saveFrameDifference(output.path(), index, frames[index], afterFrames[index]);
            const QImage control = tlrender::renderFrameAt(&controlTimeline, tick, options.outputSize);
            control.save(output.filePath(QStringLiteral("G1_%1_control.png").arg(index)));
            const double earlier = mse(frames[index], tlrender::renderFrameAt(
                &timeline, tick - 33333, options.outputSize));
            const double later = mse(frames[index], tlrender::renderFrameAt(
                &timeline, tick + 33333, options.outputSize));
            std::fprintf(stderr, "G1 frame %d adjacent-frame MSE: earlier=%.6f later=%.6f\n",
                index, earlier, later);
            ++index;
        }
    }
    gate(1, pictureMatches);
    const bool oneUndo = baked && timeline.undoManager()->saveSerial() == serial + 1;
    if (baked) timeline.undo();
    gate(4, oneUndo && clipJson(timeline.videoTracks()[0]->clips()[0]) == before);

    options.handlesSec = 1.0;
    const bool withHandles = baked && renderinplace::renderClipInPlace(timeline, 0, 0, options, &path, &error);
    if (baked && !withHandles) std::fprintf(stderr, "G2 bake failed: %s\n", qPrintable(error));
    const ClipInfo replaced = timeline.videoTracks()[0]->clips()[0];
    const auto mediaDuration = withHandles ? libavcore::probeDurationMicroseconds(path.toStdString())
                                           : std::optional<int64_t>{};
    gate(2, withHandles && videoOnly(path) && mediaDuration
        && std::abs(double(*mediaDuration) / 1000000.0 - (original.effectiveDuration() + 2.0)) <= 1.0 / options.fps
        && replaced.inPoint == 1.0 && replaced.effects.isEmpty() && replaced.speed == 1.0);
    const auto restoreSerial = timeline.undoManager()->saveSerial();
    const bool restored = withHandles && renderinplace::decomposeRenderInPlace(timeline, 0, 0);
    gate(3, restored && clipJson(timeline.videoTracks()[0]->clips()[0]) == before
        && timeline.undoManager()->saveSerial() == restoreSerial + 1);

    ClipInfo nested = replaced;
    nested.renderInPlaceOriginal = std::make_shared<ClipInfo>(original);
    nested.renderInPlaceOriginal->renderInPlaceOriginal = std::make_shared<ClipInfo>(original);
    ProjectData saved, loaded;
    saved.videoTracks = QVector<QVector<ClipInfo>>{{nested, original}};
    const QString projectPath = output.filePath(QStringLiteral("roundtrip.veditor"));
    const QJsonObject serialized = clipJson(nested);
    const bool savedOk = ProjectFile::save(projectPath, saved);
    const bool loadedOk = savedOk && ProjectFile::load(projectPath, loaded);
    const bool roundTrip = savedOk && loadedOk;
    std::fprintf(stderr, "G5 file save=%d load=%d tracks=%lld firstTrackClips=%lld\n",
        int(savedOk), int(loadedOk), static_cast<long long>(loaded.videoTracks.size()),
        loaded.videoTracks.isEmpty() ? -1LL : static_cast<long long>(loaded.videoTracks[0].size()));
    const bool shape = roundTrip && loaded.videoTracks.size() == 1 && loaded.videoTracks[0].size() == 2;
    // Also exercise the reader against nested input that was not emitted by
    // our writer (which already strips the second level).
    QJsonObject injected = serialized;
    QJsonObject child = injected.value("renderInPlaceOriginal").toObject();
    child["renderInPlaceOriginal"] = before;
    injected["renderInPlaceOriginal"] = child;
    QJsonObject root = QJsonDocument::fromJson(ProjectFile::toJsonString(saved).toUtf8()).object();
    QJsonArray injectedClips;
    injectedClips.append(injected);
    QJsonArray injectedTracks;
    injectedTracks.append(QJsonValue(injectedClips));
    root["videoTracks"] = injectedTracks;
    ProjectData external;
    const bool readNested = ProjectFile::fromJsonString(
        QString::fromUtf8(QJsonDocument(root).toJson()), external)
        && external.videoTracks.size() == 1 && external.videoTracks[0].size() == 1
        && external.videoTracks[0][0].renderInPlaceOriginal
        && !external.videoTracks[0][0].renderInPlaceOriginal->renderInPlaceOriginal;
    std::fprintf(stderr, "G5 injected reader tracks=%lld firstTrackClips=%lld\n",
        static_cast<long long>(external.videoTracks.size()),
        external.videoTracks.isEmpty() ? -1LL : static_cast<long long>(external.videoTracks[0].size()));
    bool persistenceMatches = true;
    const auto condition = [&](const char *name, bool ok) {
        std::fprintf(stderr, "G5 %s: %s\n", name, ok ? "OK" : "FAIL");
        persistenceMatches = persistenceMatches && ok;
    };
    condition("shape", shape);
    condition("readNested", readNested);
    const auto loadedOriginal = shape ? loaded.videoTracks[0][0].renderInPlaceOriginal
                                      : std::shared_ptr<ClipInfo>{};
    condition("loaded original non-null", bool(loadedOriginal));
    condition("second level null", loadedOriginal && !loadedOriginal->renderInPlaceOriginal);
    condition("clipJson equality", loadedOriginal
        && jsonEqual("G5 saved original", before, clipJson(*loadedOriginal)));
    condition("adjacent original null", shape && !loaded.videoTracks[0][1].renderInPlaceOriginal);
    condition("default-omit ordinary", !clipJson(original).contains("renderInPlaceOriginal")
        && shape && !clipJson(loaded.videoTracks[0][1]).contains("renderInPlaceOriginal"));
    condition("default-omit nested", serialized.value("renderInPlaceOriginal").isObject()
        && !serialized.value("renderInPlaceOriginal").toObject().contains("renderInPlaceOriginal"));

    // Independent JSON idempotence, without an UndoManager copy or a baked
    // clip: clipToJson(clipFromJson(clipToJson(original))) == clipToJson(original).
    ProjectData plain, plainLoaded;
    plain.videoTracks = QVector<QVector<ClipInfo>>{{original}};
    const bool plainRead = ProjectFile::fromJsonString(ProjectFile::toJsonString(plain), plainLoaded)
        && plainLoaded.videoTracks.size() == 1 && plainLoaded.videoTracks[0].size() == 1;
    condition("independent JSON idempotence", plainRead
        && jsonEqual("G5 plain original", before, clipJson(plainLoaded.videoTracks[0][0])));
    if (!persistenceMatches) {
        output.setAutoRemove(false);
        std::fprintf(stderr, "G5 roundtrip project retained in %s\n", qPrintable(projectPath));
    }
    gate(5, persistenceMatches);
    // Reject either side of an overlap before even creating the output directory
    // or connecting queue progress. Include handles=0 (the reported regression).
    bool overlapRejected = true;
    for (int target : {0, 1}) {
        for (double handles : {0.0, 1.0}) {
            ClipInfo first = original, second = original;
            first.trailOut.type = second.leadIn.type = TransitionType::CrossDissolve;
            first.trailOut.duration = second.leadIn.duration = 0.5;
            Timeline overlap;
            overlap.restoreFromProject(QVector<QVector<ClipInfo>>{{first, second}},
                QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
            overlap.undoManager()->clear();
            overlap.saveUndoState(QStringLiteral("重ね合わせ初期状態"));
            const quint64 initialSerial = overlap.undoManager()->saveSerial();
            auto rejectOptions = options;
            rejectOptions.handlesSec = handles;
            rejectOptions.outputDir = output.filePath(QStringLiteral("rejected_%1_%2").arg(target).arg(handles));
            bool queueStarted = false;
            rejectOptions.connectProgress = [&](RenderQueue &) { queueStarted = true; };
            QString rejectedPath, rejectedError;
            const bool accepted = renderinplace::renderClipInPlace(
                overlap, 0, target, rejectOptions, &rejectedPath, &rejectedError);
            overlapRejected = overlapRejected && !accepted && !rejectedError.isEmpty()
                && rejectedPath.isEmpty() && !queueStarted
                && !QFileInfo::exists(rejectOptions.outputDir)
                && overlap.undoManager()->saveSerial() == initialSerial
                && clipJson(overlap.videoTracks()[0]->clips()[0]) == clipJson(first)
                && clipJson(overlap.videoTracks()[0]->clips()[1]) == clipJson(second);
        }
    }
    gate(6, overlapRejected);

    // Independent material control from G1, composited over the same untouched
    // lower track. This measures queue loss without encoding the background.
    // Also exercise a portrait canvas: a project-sized bake would add black bars.
    bool compositionMatches = controlRendered;
    for (bool animated : {false, true}) {
        ClipInfo lower = original;
        lower.effects.clear();
        ClipInfo upper = original;
        upper.videoScale = 0.5;
        upper.videoDx = 0.25;
        upper.opacity = 0.6;
        if (animated) {
            KeyframeTrack opacity(QStringLiteral("motion.opacity"));
            opacity.addKeyframe(0.0, 0.6);
            opacity.addKeyframe(1.0, 0.8);
            upper.keyframes.addTrack(opacity);
        }
        ClipInfo expectedUpper = controlClip;
        expectedUpper.videoScale = upper.videoScale;
        expectedUpper.videoDx = upper.videoDx;
        expectedUpper.opacity = upper.opacity;
        expectedUpper.keyframes = upper.keyframes;
        // V1 wins stacking: the target must be in front of the background.
        Timeline layered, expected, backgroundOnly;
        layered.restoreFromProject(QVector<QVector<ClipInfo>>{{upper}, {lower}},
            QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
        expected.restoreFromProject(QVector<QVector<ClipInfo>>{{expectedUpper}, {lower}},
            QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
        backgroundOnly.restoreFromProject(QVector<QVector<ClipInfo>>{{upper}, {lower}},
            QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
        // Preserve the background's V2 placement; hidden V1 supplies a transparent base.
        backgroundOnly.videoTracks()[0]->setHidden(true);
        const QSize canvas = animated ? QSize(360, 640) : options.outputSize;
        QVector<QImage> beforeComposite;
        for (qint64 tick : {100000LL, 500000LL, 900000LL})
            beforeComposite.append(tlrender::renderFrameAt(&layered, tick, canvas));
        auto layerOptions = options;
        layerOptions.handlesSec = 0.0;
        layerOptions.outputSize = canvas;
        QString layerPath, layerError;
        const bool layerBaked = renderinplace::renderClipInPlace(
            layered, 0, 0, layerOptions, &layerPath, &layerError);
        if (!layerBaked) std::fprintf(stderr, "G7 bake failed: %s\n", qPrintable(layerError));
        const ClipInfo actualUpper = layered.videoTracks()[0]->clips()[0];
        compositionMatches = compositionMatches && layerBaked
            && actualUpper.videoScale == upper.videoScale
            && actualUpper.videoDx == upper.videoDx && actualUpper.opacity == upper.opacity
            && clipJson(actualUpper).value("keyframes") == clipJson(upper).value("keyframes")
            && clipJson(layered.videoTracks()[1]->clips()[0]) == clipJson(lower);
        int frame = 0;
        for (qint64 tick : {100000LL, 500000LL, 900000LL}) {
            const QImage after = tlrender::renderFrameAt(&layered, tick, canvas);
            const QImage control = tlrender::renderFrameAt(&expected, tick, canvas);
            const QImage background = tlrender::renderFrameAt(&backgroundOnly, tick, canvas);
            const double visibilityMse = mse(beforeComposite[frame], background);
            const double actualMse = mse(beforeComposite[frame], after);
            const double controlMse = mse(beforeComposite[frame], control);
            const double residual = mse(control, after);
            const bool ok = std::isfinite(actualMse) && std::isfinite(controlMse)
                && std::isfinite(visibilityMse) && visibilityMse > 1.0
                && actualMse <= controlMse + margin && residual <= margin;
            std::fprintf(stderr, "G7 animated=%d frame=%d MSE=%.6f control=%.6f residual=%.6f visibility=%.6f %s\n",
                int(animated), frame, actualMse, controlMse, residual, visibilityMse, ok ? "OK" : "FAIL");
            compositionMatches = compositionMatches && ok;
            ++frame;
        }
    }
    gate(7, compositionMatches);
    // An opaque codec cannot preserve the background through keyed V1 pixels.
    // Reject before queue setup, filesystem changes, or timeline/undo mutation.
    ClipInfo keyed = original, background = original;
    keyed.effects.append(VideoEffect::createChromaKey(QColor(0, 255, 0), 442, 0));
    background.effects.clear();
    Timeline chroma;
    chroma.restoreFromProject(QVector<QVector<ClipInfo>>{{keyed}, {background}},
        QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
    chroma.undoManager()->clear();
    chroma.saveUndoState(QStringLiteral("クロマキー初期状態"));
    const quint64 chromaSerial = chroma.undoManager()->saveSerial();
    auto chromaOptions = options;
    chromaOptions.handlesSec = 0.0;
    chromaOptions.outputDir = output.filePath(QStringLiteral("rejected_chroma"));
    bool chromaQueueStarted = false;
    chromaOptions.connectProgress = [&](RenderQueue &) { chromaQueueStarted = true; };
    QString chromaPath = QStringLiteral("must be cleared"), chromaError;
    const bool chromaAccepted = renderinplace::renderClipInPlace(
        chroma, 0, 0, chromaOptions, &chromaPath, &chromaError);
    gate(8, !chromaAccepted && !chromaError.isEmpty() && chromaPath.isEmpty()
        && !chromaQueueStarted && !QFileInfo::exists(chromaOptions.outputDir)
        && chroma.undoManager()->saveSerial() == chromaSerial
        && clipJson(chroma.videoTracks()[0]->clips()[0]) == clipJson(keyed)
        && clipJson(chroma.videoTracks()[1]->clips()[0]) == clipJson(background));
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
