#include "../CodecDetector.h"
#include "../ExportDialog.h"
#include "../ExportUserPresets.h"
#include "../RenderQueue.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../libavcore/Encode.h"
#include <QEventLoop>
#include <QFile>
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

bool render(Timeline &timeline, const QString &path, const QString &codec, const ExportConfig &config)
{
    RenderQueue queue;
    RenderJob job;
    job.outputPath = path;
    job.timeline = &timeline;
    job.codec = codec;
    job.width = 64;
    job.height = 64;
    job.endUs = 500000;
    job.exportConfig = {{"fps", 4}};
    config.appendMetadataSettings(job.exportConfig);
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
}

int runExportMetaSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
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
    bool outputFound = false;
    for (auto *edit : dialog.findChildren<QLineEdit *>()) {
        if (edit->placeholderText() == "Select output file...") {
            edit->setText(temp.filePath("gui.mp4"));
            outputFound = true;
        }
    }
    gui &= outputFound;
    if (gui) gui &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
    const ExportConfig config = dialog.config();
    RenderJob job;
    config.appendMetadataSettings(job.exportConfig);
    RenderQueue queue;
    job.outputPath = temp.filePath("queued.mp4");
    queue.addJob(job);
    const auto jobs = queue.jobs();
    gui &= jobs.size() == 1;
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it)
        gui &= jobs.size() == 1 && jobs.first().exportConfig.value(it.key()).toString() == it.value();
    // Check the actual two MainWindow call sites, rather than duplicating
    // either GUI's config assembly in this test. Run from repository root.
    QFile source("src/MainWindow.cpp");
    gui &= source.open(QIODevice::ReadOnly);
    const QByteArray code = source.readAll();
    gui &= code.contains("exportCfg.appendMetadataSettings(cfg);\n    job.exportConfig = cfg;")
        && code.contains("cfg.appendMetadataSettings(jcfg);\n                    job.exportConfig = jcfg;");

    // Verify the production wiring as well as the helpers: stream copy must
    // be gated before concatCopy, and subprocess tags must precede the output.
    QFile queueSource("src/RenderQueue.cpp");
    gui &= queueSource.open(QIODevice::ReadOnly);
    const QByteArray queueCode = queueSource.readAll();
    const qsizetype guardStart = queueCode.indexOf("const bool hasExportMetadata = renderqueue_metadata::hasMetadata(cfg);");
    const qsizetype copyStart = queueCode.indexOf("libavcore::concatCopy", guardStart);
    gui &= guardStart >= 0 && copyStart > guardStart
        && queueCode.mid(guardStart, copyStart - guardStart).contains("&& !hasExportMetadata");
    gui &= queueCode.contains(
        "renderqueue_metadata::appendSubprocessArgs(args, cfg, jobCopy.outputPath);\n"
        "    args << QStringLiteral(\"-progress\") << QStringLiteral(\"pipe:2\");\n"
        "    args << jobCopy.outputPath;");
    const QStringList prefix{QStringLiteral("-c:v"), QStringLiteral("libx265")};
    QJsonObject metadata;
    config.appendMetadataSettings(metadata);
    QStringList expected = prefix;
    for (const char *key : {"title", "author", "copyright", "comment"}) {
        const QString name = QString::fromLatin1(key);
        expected << QStringLiteral("-metadata") << (name + "=" + tags.value(name));
    }
    for (const QString &ext : {QStringLiteral("mp4"), QStringLiteral("MOV"), QStringLiteral("mkv")}) {
        QStringList args = prefix;
        renderqueue_metadata::appendSubprocessArgs(args, metadata, "tagged." + ext);
        QStringList wanted = expected;
        if (ext != "mkv") wanted << "-movflags" << "use_metadata_tags";
        gui &= args == wanted && renderqueue_metadata::hasMetadata(metadata);
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
        gui &= renderqueue_metadata::hasMetadata(single)
            && args == QStringList{QStringLiteral("-metadata"), it.key() + "=" + it.value()};
    }
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
    bool encoded = ready, defaults = ready;
    for (const QString &extension : {QStringLiteral("mp4"), QStringLiteral("mov"),
            QStringLiteral("mkv"), QStringLiteral("webm")}) {
        if (extension == "webm" && !CodecDetector::isEncoderAvailable("libvpx-vp9")) {
            std::fprintf(stderr, "SKIP G2/G3: webm (libvpx-vp9 encoder unavailable); mkv remains required\n");
            continue;
        }
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
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
