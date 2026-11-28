#include "../CodecDetector.h"
#include "../ExportDialog.h"
#include "../ExportUserPresets.h"
#include "../RenderQueue.h"
#include "../ShapeLayer.h"
#include "../SmartRender.h"
#include "../Timeline.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"
#include <QEventLoop>
#include <QImage>
#include <QMap>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

// Exercise the production helpers without starting an HDR subprocess.
namespace renderqueue_metadata {
bool hasMetadata(const QJsonObject &cfg);
void appendSubprocessArgs(QStringList &args, const QJsonObject &cfg,
                          const QString &outputPath);
}

namespace {
using Tags = QMap<QString, QString>;
const Tags tags{{"title", QStringLiteral("題名 / Title")},
    {"author", QStringLiteral("作成者 / Author")},
    {"copyright", QStringLiteral("© 2026 制作者")},
    {"comment", QStringLiteral("コメント / Comment = value")}};

bool readTags(const QString &path, Tags &result)
{
    AVFormatContext *format = nullptr;
    if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0) return false;
    const AVDictionaryEntry *entry = nullptr;
    while ((entry = av_dict_get(format->metadata, "", entry, AV_DICT_IGNORE_SUFFIX)))
        result.insert(QString::fromUtf8(entry->key).toLower(), QString::fromUtf8(entry->value));
    avformat_close_input(&format);
    return true;
}

bool render(Timeline &timeline, const QString &path, const QString &codec, const ExportConfig &config,
            bool wholeTimeline = false)
{
    RenderQueue queue;
    RenderJob job;
    job.outputPath = path;
    job.timeline = &timeline;
    job.codec = codec;
    job.width = 64;
    job.height = 64;
    job.endUs = wholeTimeline ? 0 : 500000;
    job.exportConfig = exportcfg::toJobJson(config);
    job.exportConfig["width"] = 64;
    job.exportConfig["height"] = 64;
    job.exportConfig["fps"] = 4;
    job.exportConfig["videoCodec"] = codec;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool done = false, success = false;
    QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
        [&](const QString &, bool ok, const QString &error) {
            done = true;
            success = ok;
            if (!ok) std::fprintf(stderr, "export-meta: %s\n", qPrintable(error));
            loop.quit();
        });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    queue.addJob(job);
    timer.start(60000);
    queue.start();
    if (!done) loop.exec();
    if (!done) queue.stop();
    return done && success;
}

bool writeSource(const QString &path)
{
    libavcore::EncodeRequest request;
    request.outputPath = path.toStdString();
    request.width = request.height = 64;
    request.fps = request.fpsNum = 4;
    request.fpsDen = 1;
    request.videoCodecName = "mpeg4";
    request.videoBitrateBits = 200000;
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(64, 64, QImage::Format_RGB888);
    frame.fill(Qt::red);
    for (int i = 0; i < 2; ++i)
        if (!encoder.pushFrame(frame, i)) return false;
    return !encoder.finalize().has_value();
}
}

int runExportMetaSelftest()
{
    selftest::GateTally tally;
    const auto gate = [&](int n, bool ok) { tally.gate(n, ok); };
    QTemporaryDir temp;
    ExportDialog dialog(ProjectConfig{});
    bool gui = temp.isValid();
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
        QString suffix = it.key();
        suffix[0] = suffix[0].toUpper();
        auto *edit = dialog.findChild<QLineEdit *>("exportMetadata" + suffix);
        gui &= edit != nullptr;
        if (edit) edit->setText(it.value());
    }
    auto *outputEdit = dialog.findChild<QLineEdit *>("outputPath");
    gui &= outputEdit != nullptr;
    if (outputEdit) outputEdit->setText(temp.filePath("gui.mp4"));
    if (gui) gui &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
    const ExportConfig config = dialog.config();
    RenderJob job;
    job.exportConfig = exportcfg::toJobJson(config);
    RenderQueue queue;
    job.outputPath = temp.filePath("queued.mp4");
    queue.addJob(job);
    const auto jobs = queue.jobs();
    gui &= jobs.size() == 1;
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it)
        gui &= jobs.size() == 1 && jobs.first().exportConfig.value(it.key()).toString() == it.value();
    const bool guiMapped = gui;
    const QStringList prefix{QStringLiteral("-c:v"), QStringLiteral("libx265")};
    QJsonObject metadata;
    config.appendMetadataSettings(metadata);
    QStringList expected = prefix;
    for (const char *key : {"title", "author", "copyright", "comment"}) {
        const QString name = QString::fromLatin1(key);
        expected << QStringLiteral("-metadata") << (name + "=" + tags.value(name));
    }
    bool helperArgs = true;
    for (const QString &ext : {QStringLiteral("mp4"), QStringLiteral("MOV"), QStringLiteral("mkv")}) {
        QStringList args = prefix;
        renderqueue_metadata::appendSubprocessArgs(args, metadata, "tagged." + ext);
        QStringList wanted = expected;
        if (ext != "mkv") wanted << "-movflags" << "use_metadata_tags";
        helperArgs &= args == wanted && renderqueue_metadata::hasMetadata(metadata);
    }
    // Any one nonempty field blocks stream copy; omitted/empty fields must
    // not generate empty tag arguments. Preserve whitespace and '=' verbatim.
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
        QJsonObject single;
        for (auto emptyIt = tags.constBegin(); emptyIt != tags.constEnd(); ++emptyIt)
            single.insert(emptyIt.key(), QString());
        single.insert(it.key(), it.value());
        QStringList args;
        renderqueue_metadata::appendSubprocessArgs(args, single, "single.mkv");
        helperArgs &= renderqueue_metadata::hasMetadata(single)
            && args == QStringList{QStringLiteral("-metadata"), it.key() + "=" + it.value()};
    }
    gui &= helperArgs;
    // The removed source-text check also claimed that the HDR fallback placed
    // metadata before the ffmpeg subprocess output argument. That branch
    // requires an external 10-bit encoder and has no deterministic subprocess
    // seam here, so this gate cannot replace its ordering assertion. The
    // helper checks above verify the generated arguments; the render below
    // verifies the in-process output path only.
    // ClipInfo does not persist the source signature used by canStreamCopy.
    // The test seam lets timelinePassThrough use its probed signature so G1
    // checks an eligible source. Metadata must then prevent concatCopy and
    // reach the encoder and output container.
    const QByteArray oldSmart = qgetenv("VEDITOR_SMART_RENDER");
    const bool hadSmart = qEnvironmentVariableIsSet("VEDITOR_SMART_RENDER");
    const QByteArray oldSeam = qgetenv("VEDITOR_SMARTRENDER_TEST_SEAM");
    const bool hadSeam = qEnvironmentVariableIsSet("VEDITOR_SMARTRENDER_TEST_SEAM");
    qputenv("VEDITOR_SMART_RENDER", "1");
    qputenv("VEDITOR_SMARTRENDER_TEST_SEAM", "1");
    const QString smartSource = temp.filePath("smart-source.mp4");
    Timeline smartTimeline;
    smartTimeline.setProjectOutputConfig(64, 64, true);
    ClipInfo smartClip;
    smartClip.filePath = smartSource;
    smartClip.duration = smartClip.outPoint = 0.5;
    const bool smartReady = writeSource(smartSource);
    if (smartReady) smartTimeline.restoreFromProject(
        QVector<QVector<ClipInfo>>{{smartClip}}, QVector<QVector<ClipInfo>>{},
        0.0, -1.0, -1.0, 100);
    smartTimeline.refreshPlaybackSequence();
    const QString smartOutput = temp.filePath("smart-tagged.mp4");
    const auto eligibility = smartrender::timelinePassThrough(
        &smartTimeline, smartOutput, "mpeg4", 64, 64, 4.0, 0, 0,
        false, false, false, false, false, false, false);
    libavcore::FrameEncoder::resetMetadataWriteCountForTest();
    const bool smartRendered = smartReady && render(smartTimeline, smartOutput, "mpeg4", config, true);
    Tags smartTags;
    const bool smartTagged = smartRendered && readTags(smartOutput, smartTags)
        && smartTags.value("title") == config.title;
    const bool smartEncoded = libavcore::FrameEncoder::metadataWriteCountForTest() > 0;
    std::fprintf(stderr, "export-meta smart render: ready=%d eligible=%d reason=%s rendered=%d tagged=%d encoded=%d title=%s\n",
        smartReady, eligibility.eligible, qPrintable(eligibility.reason), smartRendered, smartTagged,
        smartEncoded, qPrintable(smartTags.value("title")));
    gui &= smartReady && eligibility.eligible && smartTagged && smartEncoded;
    std::fprintf(stderr, "export-meta G1 guiMapped=%d helperArgs=%d smartReady=%d eligible=%d tagged=%d encoded=%d\n",
        guiMapped, helperArgs, smartReady, eligibility.eligible, smartTagged, smartEncoded);
    if (hadSmart) qputenv("VEDITOR_SMART_RENDER", oldSmart);
    else qunsetenv("VEDITOR_SMART_RENDER");
    if (hadSeam) qputenv("VEDITOR_SMARTRENDER_TEST_SEAM", oldSeam);
    else qunsetenv("VEDITOR_SMARTRENDER_TEST_SEAM");
    gate(1, gui);

    Timeline timeline;
    timeline.setProjectOutputConfig(64, 64, true);
    ShapeFill fill;
    fill.color = Qt::red;
    ShapeStroke stroke;
    stroke.enabled = false;
    Shape shape = ShapeLayer::createRectangle(QSizeF(64, 64), fill, stroke);
    shape.position = QPointF(32, 32);
    ClipInfo clip;
    clip.duration = 0.5;
    clip.outPoint = 0.5;
    clip.shapes = {shape};
    const bool ready = temp.isValid() && timeline.insertShapeClipAtPlayhead(clip);
    timeline.refreshPlaybackSequence();
    const auto checkContainer = [&](const QString &extension, bool &encoded, bool &defaults) {
        const QString codec = extension == "webm" ? "libvpx-vp9" : "mpeg4";
        const QString taggedPath = temp.filePath("tagged." + extension);
        Tags actual;
        libavcore::FrameEncoder::resetMetadataWriteCountForTest();
        const bool wrote = ready && render(timeline, taggedPath, codec, config) && readTags(taggedPath, actual);
        encoded &= wrote && libavcore::FrameEncoder::metadataWriteCountForTest() > 0;
        for (auto it = tags.constBegin(); it != tags.constEnd(); ++it)
            encoded &= actual.value(it.key()) == it.value();
        if (!wrote) std::fprintf(stderr, "export-meta: failed container %s\n", qPrintable(extension));

        const QString baselinePath = temp.filePath("baseline." + extension);
        const QString emptyPath = temp.filePath("empty." + extension);
        libavcore::FrameEncoder::resetMetadataWriteCountForTest();
        libavcore::FrameEncoder::setMetadataEnabledForTest(false);
        const bool baseline = ready && render(timeline, baselinePath, codec, ExportConfig{});
        libavcore::FrameEncoder::setMetadataEnabledForTest(true);
        const bool empty = ready && render(timeline, emptyPath, codec, ExportConfig{});
        Tags baselineTags, emptyTags;
        defaults &= baseline && empty && readTags(baselinePath, baselineTags) && readTags(emptyPath, emptyTags)
            && baselineTags == emptyTags && libavcore::FrameEncoder::metadataWriteCountForTest() == 0;
        for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) defaults &= !emptyTags.contains(it.key());
        std::fprintf(stderr, "export-meta %s: tagged=%d metadata=%d baseline=%d empty=%d defaults=%d\n",
            qPrintable(extension), wrote, encoded, baseline, empty, defaults);
    };
    bool encoded = ready, defaults = ready;
    for (const QString &extension : {QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv")}) {
        checkContainer(extension, encoded, defaults);
    }
    gate(2, encoded);
    QJsonObject empty;
    ExportConfig{}.appendMetadataSettings(empty);
    for (bool explicitEmpty : {false, true}) {
        QJsonObject emptyConfig;
        if (explicitEmpty) {
            for (auto it = tags.constBegin(); it != tags.constEnd(); ++it)
                emptyConfig.insert(it.key(), QString());
        }
        for (const QString &ext : {QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv")}) {
            QStringList args = prefix;
            renderqueue_metadata::appendSubprocessArgs(args, emptyConfig, "empty." + ext);
            defaults &= args == prefix && !renderqueue_metadata::hasMetadata(emptyConfig);
        }
    }
    gate(3, defaults && empty.isEmpty());

    const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
    const QByteArray oldEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
    qputenv("VEDITOR_EXPORT_PRESET_DIR", temp.filePath("presets").toUtf8());
    bool presets = ExportUserPresets::save({QStringLiteral("メタデータ"), config});
    ExportConfig restored;
    presets &= ExportUserPresets::resolve(QStringLiteral("メタデータ"), &restored);
    QJsonObject restoredJson;
    restored.appendMetadataSettings(restoredJson);
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it)
        presets &= restoredJson.value(it.key()).toString() == it.value();
    const QJsonObject defaultPreset = ExportUserPresets::toJson(ExportConfig{});
    QJsonObject legacyPreset = ExportUserPresets::toJson(config);
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
        presets &= !defaultPreset.contains(it.key());
        legacyPreset.remove(it.key());
    }
    const ExportConfig legacyRestored = ExportUserPresets::fromJson(legacyPreset);
    presets &= legacyRestored.title.isEmpty() && legacyRestored.author.isEmpty()
        && legacyRestored.copyright.isEmpty() && legacyRestored.comment.isEmpty();
    if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
    else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    gate(4, presets);
    QString webmReason;
    if (selftest::requireEncoder("libvpx-vp9", &webmReason)) {
        bool webmEncoded = ready, webmDefaults = ready;
        checkContainer(QStringLiteral("webm"), webmEncoded, webmDefaults);
        gate(5, webmEncoded);
        gate(6, webmDefaults);
    } else {
        tally.skip(5, qPrintable(webmReason));
        tally.skip(6, qPrintable(webmReason));
    }
    return tally.finish();
}
