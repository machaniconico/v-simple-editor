#include "../ExportDialog.h"
#include "../ExportUserPresets.h"
#include "../RenderQueue.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../libavcore/Encode.h"
#include <QEventLoop>
#include <QFile>
#include <QMap>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

namespace {
using Tags = QMap<QString, QString>;
const Tags tags{{"title", QStringLiteral("題名 / Title")},
    {"author", QStringLiteral("作成者 / Author")},
    {"copyright", QStringLiteral("© 2026 制作者")},
    {"comment", QStringLiteral("コメント / Comment")}};

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
    for (const QString &extension : {QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("webm")}) {
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
    if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
    else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    gate(4, presets);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
