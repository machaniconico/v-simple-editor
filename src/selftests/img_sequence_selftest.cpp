#include "../ExportDialog.h"
#include "../ExrSequenceWriter.h"
#include "../RenderQueue.h"
#include "../SequenceExport.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../ExportUserPresets.h"
#include "SelftestGate.h"
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
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
    selftest::GateTally tally;
    const auto gate = [&](int n, bool ok) { tally.gate(n, ok); };
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

    bool mapped = true;
    QJsonObject cfg, jcfg;
    config.appendSequenceSettings(cfg);
    jcfg = exportcfg::toJobJson(config);
    const QJsonObject expected{{"imageSequence", true}, {"sequenceFolder", config.outputPath},
        {"sequencePattern", "shot_%04d.png"}, {"sequenceDigits", 4}, {"sequenceStart", 17}, {"keepAlpha", true}};
    for (auto it = expected.begin(); it != expected.end(); ++it)
        mapped &= cfg.value(it.key()) == it.value() && jcfg.value(it.key()) == it.value();
    const bool sequenceMapped = mapped;
    RenderQueue saved;
    saved.addJob(job);
    RenderQueue loaded;
    mapped &= saved.saveQueue(temp.filePath("queue.json")) && loaded.loadQueue(temp.filePath("queue.json"));
    const auto jobs = loaded.jobs();
    mapped &= jobs.size() == 1;
    if (jobs.size() == 1)
        for (auto it = expected.begin(); it != expected.end(); ++it)
            mapped &= jobs.first().exportConfig.value(it.key()) == it.value();
    std::fprintf(stderr, "img-sequence G5 sequenceMapped=%d persisted=%d queued=%lld keys=%lld\n",
        sequenceMapped, mapped, static_cast<long long>(jobs.size()), static_cast<long long>(jcfg.size()));
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

    // G7-G10: EXR sequences (display-referred by default, scene-linear on request).
    const bool exrEncoder = exrseq::encoderAvailable();
    const auto pngPath = [&](const QString &dir, int f) {
        return QDir(dir).filePath(QString("shot_%1.png").arg(17 + f, 4, 10, QLatin1Char('0')));
    };
    const auto exrPath = [&](const QString &dir, int f) {
        return QDir(dir).filePath(QString("shot_%1.exr").arg(17 + f, 4, 10, QLatin1Char('0')));
    };
    const auto maxRgbDiff = [](const QImage &a, const QImage &b) {
        if (a.isNull() || b.isNull() || a.size() != b.size()) return 255;
        int worst = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x) {
                const QColor p = a.pixelColor(x, y), q = b.pixelColor(x, y);
                worst = std::max({worst, std::abs(p.red() - q.red()),
                                  std::abs(p.green() - q.green()), std::abs(p.blue() - q.blue())});
            }
        return worst;
    };
    RenderJob exrJob = opaqueJob;
    exrJob.outputPath = temp.filePath("exr");
    exrJob.exportConfig["sequenceFolder"] = exrJob.outputPath;
    exrJob.exportConfig["sequencePattern"] = "shot_%04d.exr";
    RenderJob exrAlphaJob = exrJob;
    exrAlphaJob.outputPath = temp.filePath("exr-alpha");
    exrAlphaJob.exportConfig["sequenceFolder"] = exrAlphaJob.outputPath;
    exrAlphaJob.exportConfig["keepAlpha"] = true;
    if (!exrEncoder) {
        for (int n : {7, 8, 10}) tally.skip(n, "exrseq::encoderAvailable() is false");
    } else {
        const Result exr = ready ? runJob(exrJob) : Result{};
        bool exrOk = exr.accepted && exr.success && exr.progress == 100
            && QDir(exrJob.outputPath).entryList(QDir::Files) == QStringList{
                "shot_0017.exr", "shot_0018.exr", "shot_0019.exr"};
        int worst = 0, crossWorst = 0;
        for (int f = 0; exrOk && f < 3; ++f) {
            QImage decoded;
            bool hasAlpha = true;
            QString how;
            exrOk &= exrseq::decodeForTest(exrPath(exrJob.outputPath, f), &decoded, &hasAlpha, &how);
            worst = std::max(worst, maxRgbDiff(decoded, QImage(pngPath(opaqueJob.outputPath, f))));
            // Negative control: a different frame's PNG must NOT match, or the +-1 check proves nothing.
            crossWorst = std::max(crossWorst, maxRgbDiff(decoded, QImage(pngPath(opaqueJob.outputPath, (f + 1) % 3))));
        }
        std::fprintf(stderr, "img-sequence G7 exrRan=%d maxDiffVsPng=%d crossFrameDiff=%d\n",
            exrOk, worst, crossWorst);
        gate(7, exrOk && worst <= 1 && crossWorst > 8);

        const Result exrAlpha = ready ? runJob(exrAlphaJob) : Result{};
        bool withAlpha = false, withoutAlpha = true;
        const bool headerRead = exrseq::headerHasAlphaForTest(exrPath(exrAlphaJob.outputPath, 0), &withAlpha)
            && exrseq::headerHasAlphaForTest(exrPath(exrJob.outputPath, 0), &withoutAlpha);
        std::fprintf(stderr, "img-sequence G8 ran=%d headerRead=%d keepAlpha=%d opaque=%d\n",
            exrAlpha.success, headerRead, withAlpha, withoutAlpha);
        gate(8, exrAlpha.success && headerRead && withAlpha && !withoutAlpha);

        // 128 written scene-linear must be the sRGB EOTF value, not 128/255.
        const auto grayPixel = [&](bool linear, float *r, float *g, float *b) {
            auto s = sequenceexport::Settings::fromJson(exrJob.exportConfig, temp.filePath(linear ? "lin" : "disp"));
            s.folder = temp.filePath(linear ? "lin" : "disp");
            s.exrLinear = linear;
            const QString err = sequenceexport::write(&timeline, s, QSize(128, 64), 4, 0, 1, {}, {},
                [](QImage &frame, qint64) { frame.fill(QColor(128, 128, 128)); });
            QVector<float> rgba;
            int w = 0, h = 0;
            bool alpha = false;
            if (!err.isEmpty() || !exrseq::decodeFloatForTest(sequenceexport::fileName(s, 0), &rgba, &w, &h, &alpha)
                || rgba.size() < 4) return false;
            const int i = ((h / 2) * w + w / 2) * 4;
            *r = rgba[i]; *g = rgba[i + 1]; *b = rgba[i + 2];
            return true;
        };
        float lr = 0, lg = 0, lb = 0, dr = 0, dg = 0, db = 0;
        const bool readLin = grayPixel(true, &lr, &lg, &lb);
        const bool readDisp = grayPixel(false, &dr, &dg, &db);
        const float tol = 1.0f / 1024.0f;
        const auto within = [&](float v, float e) { return std::fabs(v - e) <= tol; };
        std::fprintf(stderr, "img-sequence G10 linear=(%.5f %.5f %.5f) display=(%.5f %.5f %.5f)\n",
            lr, lg, lb, dr, dg, db);
        gate(10, readLin && readDisp && within(lr, 0.2158f) && within(lg, 0.2158f) && within(lb, 0.2158f)
            && within(dr, 128.0f / 255.0f) && within(dg, 128.0f / 255.0f) && within(db, 128.0f / 255.0f));
    }

    // G9: without the encoder neither the combo entry nor an .exr pattern is accepted.
    bool comboOn = false, comboOff = true, validOn = false, validOff = true, presetOn = false, reloadedExrLinear = false;
    for (const bool available : {true, false}) {
        exrseq::setEncoderAvailableOverrideForTest(available);
        ExportDialog exrDialog(ProjectConfig{});
        if (auto *t = exrDialog.findChild<QComboBox *>("exportType"))
            t->setCurrentIndex(t->findData(static_cast<int>(ExportType::ImageSequence)));
        auto *f = exrDialog.findChild<QComboBox *>("sequenceFormat");
        auto *linear = exrDialog.findChild<QCheckBox *>("exrLinear");
        auto probe = sequenceexport::Settings::fromJson(exrJob.exportConfig, temp.filePath("probe"));
        const bool hasExr = f && f->findData("exr") >= 0;
        const bool valid = sequenceexport::validate(probe).isEmpty();
        if (available) {
            comboOn = hasExr && linear && !linear->isChecked() && !linear->isVisibleTo(&exrDialog);
            if (hasExr) {
                f->setCurrentIndex(f->findData("exr"));
                comboOn &= linear->isVisibleTo(&exrDialog) && exrDialog.findChild<QLineEdit *>("sequencePattern")->text().endsWith(".exr");
            }
            validOn = valid;
            ExportDialog presetDialog(ProjectConfig{});
            ExportConfig exrPreset;
            exrPreset.imageSequence = true;
            exrPreset.sequencePattern = "shot_%04d.exr";
            exrPreset.exrLinear = true;
            const ExportConfig reloaded = ExportUserPresets::fromJson(ExportUserPresets::toJson(exrPreset));
            reloadedExrLinear = reloaded.exrLinear;
            presetDialog.applyPresetForTest(reloaded);
            auto *pf = presetDialog.findChild<QComboBox *>("sequenceFormat");
            auto *pl = presetDialog.findChild<QCheckBox *>("exrLinear");
            const QJsonObject exrJson = exportcfg::toJobJson(presetDialog.currentConfigForTest());
            presetOn = pf && pl && pf->currentData() == "exr" && pl->isVisibleTo(&presetDialog) && pl->isChecked() && reloaded.exrLinear
                && exrJson.value("exrLinear").toBool()
                && sequenceexport::Settings::fromJson(exrJson, temp.filePath("preset")).exrLinear;
            ExportDialog pngDialog(ProjectConfig{});
            ExportConfig pngPreset = exrPreset;
            pngPreset.sequencePattern = "shot_%04d.png";
            pngPreset.exrLinear = false;
            pngDialog.applyPresetForTest(pngPreset);
            presetOn &= !exportcfg::toJobJson(pngDialog.currentConfigForTest()).contains("exrLinear");
        } else {
            comboOff = hasExr;
            validOff = valid;
        }
    }
    exrseq::setEncoderAvailableOverrideForTest(std::nullopt);
    std::fprintf(stderr, "img-sequence G9 comboOn=%d validOn=%d comboOffHasExr=%d validOff=%d presetOn=%d reloadedExrLinear=%d\n",
        comboOn, validOn, comboOff, validOff, presetOn, reloadedExrLinear);
    gate(9, comboOn && validOn && !comboOff && !validOff && presetOn);
    return tally.finish("img-sequence");
}
