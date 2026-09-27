#include "../ExportDialog.h"
#include "../RenderQueue.h"
#include "../SequenceExport.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

namespace {
struct Result {
    bool accepted = false;
    bool completed = false;
    bool success = false;
    int progress = 0;
    RenderJobStatus status = RenderJobStatus::Pending;
};
Result runJob(RenderJob job, bool cancel = false)
{
    RenderQueue queue;
    Result result;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&queue, &RenderQueue::jobProgressUuid, &loop,
        [&](const QString &, int percent) { result.progress = qMax(result.progress, percent); });
    QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
        [&](const QString &, bool ok, const QString &error) {
            result.completed = true;
            result.success = ok;
            if (!ok && !cancel) std::fprintf(stderr, "img-sequence: %s\n", qPrintable(error));
            loop.quit();
        });
    if (cancel) QObject::connect(&queue, &RenderQueue::jobStarted, &queue,
        [&](int) { queue.cancelCurrent(); });
    queue.addJob(job);
    result.accepted = queue.jobs().size() == 1;
    if (!result.accepted) return result;
    timeout.start(60000);
    queue.start();
    if (!result.completed) loop.exec();
    if (!result.completed) queue.stop();
    result.status = queue.jobs().first().status;
    return result;
}
QString readSource(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}
bool equalPixels(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (a.pixelColor(x, y) != b.pixelColor(x, y)) return false;
    return true;
}
}

int runImgSequenceSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    QTemporaryDir temp;
    Timeline timeline;
    timeline.setProjectOutputConfig(128, 64, true);
    ShapeFill fill;
    fill.color = QColor(230, 60, 30);
    ShapeStroke stroke;
    stroke.enabled = false;
    Shape shape = ShapeLayer::createRectangle(QSizeF(64, 64), fill, stroke);
    shape.position = QPointF(32, 32);
    ClipInfo clip;
    clip.duration = 0.25;
    clip.inPoint = 0;
    clip.outPoint = 0.25;
    bool ready = temp.isValid();
    for (int f = 0; f < 3; ++f) {
        fill.color = QColor(230 - f * 50, 60 + f * 40, 30 + f * 50);
        shape = ShapeLayer::createRectangle(QSizeF(64, 64), fill, stroke);
        shape.position = QPointF(32, 32);
        clip.shapes = {shape};
        timeline.setPlayheadPosition(f * 0.25);
        ready &= timeline.insertShapeClipAtPlayhead(clip);
    }
    timeline.refreshPlaybackSequence();

    ExportConfig config;
    config.imageSequence = true;
    config.outputPath = temp.filePath("alpha");
    config.sequenceFolder = config.outputPath;
    config.sequencePattern = "shot_%04d.png";
    config.sequenceDigits = 4;
    config.sequenceStart = 17;
    config.keepAlpha = true;
    RenderJob job;
    job.timeline = &timeline;
    job.width = 128;
    job.height = 64;
    job.outputPath = config.outputPath;
    job.exportConfig = {{"fps", 4}};
    config.appendSequenceSettings(job.exportConfig);
    const Result alpha = ready ? runJob(job) : Result{};
    gate(1, alpha.accepted && alpha.success && alpha.progress == 100
        && QDir(config.outputPath).entryList(QDir::Files) == QStringList{
            "shot_0017.png", "shot_0018.png", "shot_0019.png"});

    bool pixels = alpha.success;
    for (int f = 0; f < 3; ++f) {
        QImage image;
        pixels &= image.load(QDir(config.outputPath).filePath(QString("shot_%1.png").arg(17 + f, 4, 10, QLatin1Char('0'))));
        const QImage reference = tlrender::renderFrameAt(&timeline, f * 250000, QSize(128, 64), 250000.0);
        pixels &= equalPixels(image, reference);
    }
    gate(2, pixels);

    RenderJob opaqueJob = job;
    opaqueJob.outputPath = temp.filePath("opaque");
    opaqueJob.exportConfig["sequenceFolder"] = opaqueJob.outputPath;
    opaqueJob.exportConfig["keepAlpha"] = false;
    const Result opaque = ready ? runJob(opaqueJob) : Result{};
    const QImage transparent(QDir(config.outputPath).filePath("shot_0017.png"));
    const QImage flattened(QDir(opaqueJob.outputPath).filePath("shot_0017.png"));
    bool alphaOk = alpha.accepted && alpha.success && opaque.success
        && !transparent.isNull() && !flattened.isNull();
    if (alphaOk) {
        alphaOk &= transparent.pixelColor(96, 32).alpha() == 0
            && flattened.pixelColor(96, 32) == QColor(Qt::black)
            && transparent.pixelColor(32, 32) == flattened.pixelColor(32, 32);
        for (int y = 0; y < flattened.height(); ++y)
            for (int x = 0; x < flattened.width(); ++x)
                alphaOk &= flattened.pixelColor(x, y).alpha() == 255;
    }
    RenderQueue legacy;
    alphaOk &= legacy.addJob("sequence", {}, config.outputPath, job.exportConfig) > 0;
    gate(3, alphaOk);

    sequenceexport::resetFrameCountForTest();
    sequenceexport::setEnabledForTest(false);
    RenderJob video;
    video.timeline = &timeline;
    video.width = 128;
    video.height = 64;
    video.codec = "prores";
    video.outputPath = temp.filePath("video.mov");
    video.exportConfig = {{"fps", 4}, {"proresProfile", 4}};
    const Result movie = ready ? runJob(video) : Result{};
    sequenceexport::setEnabledForTest(true);
    QJsonObject defaults;
    ExportConfig{}.appendSequenceSettings(defaults);
    bool policy = !ExportConfig{}.imageSequence && defaults.isEmpty()
        && movie.success && QFileInfo(video.outputPath).size() > 0
        && sequenceexport::frameCountForTest() == 0
        && sequenceexport::writeCallCountForTest() == 0;
    for (const QString &codec : {QString("prores"), QString("prores_ks"), QString("libx264"), QString("h264"), QString("vp9"), QString("libvpx-vp9")}) {
        const bool allowed = codec != "libx264" && codec != "h264";
        policy &= RenderQueue::alphaExportError({{"videoCodec", codec}, {"proresProfile", 4}, {"keepAlpha", true}}).isEmpty() == allowed;
    }
    gate(4, policy);

    // Inspect the actual two production call sites, not replicas of their mapping.
    // Both must use the single production mapper before assigning exportConfig.
    const QString main = readSource("src/MainWindow.cpp");
    const int normalStart = main.indexOf("void MainWindow::exportVideo()");
    const int normalEnd = main.indexOf("job.exportConfig = cfg;", normalStart);
    const int batchStart = main.indexOf("QJsonObject jcfg;");
    const int batchEnd = main.indexOf("job.exportConfig = jcfg;", batchStart);
    bool mapped = normalStart >= 0 && normalEnd > normalStart && batchStart >= 0 && batchEnd > batchStart
        && main.mid(normalStart, normalEnd - normalStart).contains("exportCfg.appendSequenceSettings(cfg);")
        && main.mid(batchStart, batchEnd - batchStart).contains("cfg.appendSequenceSettings(jcfg);");
    QJsonObject cfg, jcfg;
    config.appendSequenceSettings(cfg);
    config.appendSequenceSettings(jcfg);
    const QJsonObject expected{{"imageSequence", true}, {"sequenceFolder", config.outputPath},
        {"sequencePattern", "shot_%04d.png"}, {"sequenceDigits", 4}, {"sequenceStart", 17}, {"keepAlpha", true}};
    mapped &= cfg == expected && cfg == jcfg;
    RenderQueue saved;
    saved.addJob(job);
    RenderQueue loaded;
    mapped &= saved.saveQueue(temp.filePath("queue.json")) && loaded.loadQueue(temp.filePath("queue.json"));
    const auto jobs = loaded.jobs();
    mapped &= jobs.size() == 1;
    if (jobs.size() == 1)
        for (auto it = expected.begin(); it != expected.end(); ++it)
            mapped &= jobs.first().exportConfig.value(it.key()) == it.value();
    gate(5, mapped);

    ExportDialog dialog(ProjectConfig{});
    auto *type = dialog.findChild<QComboBox *>("exportType");
    auto *crf = dialog.findChild<QSpinBox *>("crf");
    auto *videoBitrate = dialog.findChild<QSpinBox *>("videoBitrate");
    auto *audioBitrate = dialog.findChild<QSpinBox *>("audioBitrate");
    auto *videoCodec = dialog.findChild<QComboBox *>("videoCodec");
    auto *keepAlpha = dialog.findChild<QCheckBox *>("keepAlpha");
    auto *output = dialog.findChild<QLineEdit *>("outputPath");
    bool ui = type && crf && videoBitrate && audioBitrate && videoCodec && keepAlpha && output
        && !dialog.findChild<QComboBox *>("sequenceFormat")
        && !dialog.findChild<QLineEdit *>("sequencePattern");
    if (type)
        type->setCurrentIndex(type->findData(static_cast<int>(ExportType::ImageSequence)));
    auto *formats = dialog.findChild<QComboBox *>("sequenceFormat");
    auto *pattern = dialog.findChild<QLineEdit *>("sequencePattern");
    auto *digits = dialog.findChild<QSpinBox *>("sequenceDigits");
    auto *start = dialog.findChild<QSpinBox *>("sequenceStart");
    ui &= type && formats && pattern && digits && start;
    if (ui) {
        ui &= (formats->findData("tiff") >= 0) == QImageWriter::supportedImageFormats().contains("tiff");
        ui &= !crf->isEnabled() && !videoBitrate->isEnabled()
            && !audioBitrate->isEnabled() && !videoCodec->isEnabled();
        digits->setValue(5);
        start->setValue(25);
        ui &= pattern->text() == "name_%05d.png";
        type->setCurrentIndex(type->findData(static_cast<int>(ExportType::Video)));
        type->setCurrentIndex(type->findData(static_cast<int>(ExportType::ImageSequence)));
        ui &= dialog.findChild<QSpinBox *>("sequenceDigits") == digits
            && digits->value() == 5 && start->value() == 25
            && pattern->text() == "name_%05d.png";
        ui &= keepAlpha->isEnabled();
        keepAlpha->setChecked(true);
        output->setText(temp.filePath("dialog"));
        ui &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection)
            && dialog.config().imageSequence && dialog.config().sequenceDigits == 5
            && dialog.config().sequenceStart == 25 && !dialog.config().audioOnly
            && dialog.config().keepAlpha;
    }
    RenderJob range = job;
    range.startUs = 250000;
    range.endUs = 500000;
    range.outputPath = temp.filePath("range");
    range.exportConfig["sequenceFolder"] = range.outputPath;
    const Result ranged = ready ? runJob(range) : Result{};
    ui &= ranged.success && QDir(range.outputPath).entryList(QDir::Files).size() == 1
        && equalPixels(QImage(QDir(range.outputPath).filePath("shot_0017.png")),
                       tlrender::renderFrameAt(&timeline, 250000, QSize(128, 64), 250000.0));
    RenderJob cancelled = job;
    cancelled.outputPath = temp.filePath("cancelled");
    cancelled.exportConfig["sequenceFolder"] = cancelled.outputPath;
    const Result cancel = ready ? runJob(cancelled, true) : Result{};
    ui &= cancel.completed && !cancel.success && cancel.status == RenderJobStatus::Cancelled
        && QDir(cancelled.outputPath).entryList(QDir::Files).isEmpty();
    auto settings = sequenceexport::Settings::fromJson(job.exportConfig, job.outputPath);
    settings.folder = temp.filePath("partial");
    int progress = 0;
    const QString stopped = sequenceexport::write(&timeline, settings, QSize(128, 64), 4, 0, 3,
        [&]() { return progress > 0; }, [&](int p) { progress = p; });
    ui &= !stopped.isEmpty() && QDir(settings.folder).entryList(QDir::Files).size() == 1;
    settings.pattern = "../bad_%06d.png";
    ui &= !sequenceexport::validate(settings).isEmpty();
    settings.pattern = "bad_%d_%d.png";
    ui &= !sequenceexport::validate(settings).isEmpty();
    settings.pattern = "shot_%04d.tiff";
    if (QImageWriter::supportedImageFormats().contains("tiff")) {
        settings.folder = temp.filePath("tiff");
        ui &= sequenceexport::write(&timeline, settings, QSize(128, 64), 4, 250000, 1).isEmpty();
        QImage image(sequenceexport::fileName(settings, 0));
        ui &= equalPixels(image, tlrender::renderFrameAt(&timeline, 250000, QSize(128, 64), 250000.0));
    } else {
        ui &= !sequenceexport::validate(settings).isEmpty();
    }
    gate(6, ui);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
