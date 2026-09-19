#include "../ExportDialog.h"
#include "../RenderQueue.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../libavcore/Encode.h"
#include "../libavcore/FrameGrab.h"
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

namespace {
bool render(Timeline &timeline, const QString &path, bool alpha, int profile)
{
    RenderQueue queue;
    RenderJob job;
    job.outputPath = path;
    job.timeline = &timeline;
    job.codec = "prores";
    job.width = 128;
    job.height = 64;
    job.endUs = 1000000;
    job.exportConfig = {{"fps", 4}, {"proresProfile", profile}, {"keepAlpha", alpha}};
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool done = false, success = false;
    QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
        [&](const QString &, bool ok, const QString &error) {
            done = true;
            success = ok;
            if (!ok) std::fprintf(stderr, "export-alpha: %s\n", qPrintable(error));
            loop.quit();
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    queue.addJob(job);
    timeout.start(60000);
    queue.start();
    if (!done) loop.exec();
    if (!done) queue.stop();
    return done && success;
}

bool pixelsMatch(const QImage &image, bool alpha)
{
    if (image.isNull() || image.size() != QSize(128, 64)) return false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (!alpha && pixel.alpha() != 255) return false;
            // Avoid antialiasing on the rectangle boundary, but examine every
            // interior pixel in both the covered and uncovered halves.
            if (x < 60 && y > 2 && y < 61) {
                if (pixel.alpha() < 253 || pixel.red() < 240) return false;
            } else if (x > 68) {
                if (alpha) {
                    if (pixel.alpha() > 2) return false;
                } else if (pixel.red() > 2 || pixel.green() > 2 || pixel.blue() > 2) {
                    return false;
                }
            }
        }
    }
    return true;
}
}

int runExportAlphaSelftest()
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
    fill.color = Qt::white;
    ShapeStroke stroke;
    stroke.enabled = false;
    Shape shape = ShapeLayer::createRectangle(QSizeF(64, 64), fill, stroke);
    shape.position = QPointF(32, 32);
    ClipInfo clip;
    clip.duration = 1.0;
    clip.inPoint = 0.0;
    clip.outPoint = 1.0;
    clip.shapes = {shape};
    const bool ready = temp.isValid() && timeline.insertShapeClipAtPlayhead(clip);
    timeline.refreshPlaybackSequence();

    libavcore::FrameEncoder::resetAlphaFrameCountForTest();
    RenderQueue::resetBlackCompositeCountForTest();
    libavcore::FrameEncoder::setAlphaInputEnabledForTest(false);
    const QString opaquePath = temp.filePath("opaque.mov");
    const bool opaque = ready && render(timeline, opaquePath, false, 4);
    gate(1, !ExportConfig{}.keepAlpha && opaque
        && libavcore::FrameEncoder::alphaFrameCountForTest() == 0
        && RenderQueue::blackCompositeCountForTest() >= 1);
    libavcore::FrameEncoder::setAlphaInputEnabledForTest(true);

    bool alpha = ready;
    for (const int profile : {4, 5}) {
        libavcore::FrameEncoder::resetAlphaFrameCountForTest();
        RenderQueue::resetBlackCompositeCountForTest();
        const QString path = temp.filePath(QStringLiteral("alpha-%1.mov").arg(profile));
        const bool rendered = ready && render(timeline, path, true, profile);
        alpha &= rendered && pixelsMatch(libavcore::grabFrameAt(path, 0.5), true)
            && libavcore::FrameEncoder::alphaFrameCountForTest() > 0
            && RenderQueue::blackCompositeCountForTest() == 0;
    }
    gate(2, alpha);
    gate(3, opaque && pixelsMatch(libavcore::grabFrameAt(opaquePath, 0.5), false));

    RenderQueue rejected;
    QString error;
    QObject::connect(&rejected, &RenderQueue::jobFailed, &rejected,
        [&](int, const QString &message) { error = message; });
    const QJsonObject invalid{{"videoCodec", "h264"}, {"keepAlpha", true}, {"proresProfile", 4}};
    const int id = rejected.addJob("invalid", {}, temp.filePath("invalid.mp4"), invalid);
    bool refusal = id == -1 && rejected.jobs().isEmpty() && !error.isEmpty();
    error.clear();
    RenderJob invalidJob;
    invalidJob.codec = "h264";
    invalidJob.exportConfig = invalid;
    rejected.addJob(invalidJob);
    refusal &= rejected.jobs().isEmpty() && !error.isEmpty();
    libavcore::EncodeRequest request;
    request.videoCodecName = "libx264";
    request.keepAlpha = true;
    libavcore::FrameEncoder encoder;
    const auto openError = encoder.open(request);
    refusal &= openError.has_value() && !encoder.isOpen()
        && QString::fromStdString(*openError) == QStringLiteral("このコーデックはアルファを保持できません");
    ExportDialog dialog(ProjectConfig{});
    QCheckBox *checkbox = nullptr;
    QComboBox *presets = nullptr;
    for (auto *candidate : dialog.findChildren<QCheckBox *>())
        if (candidate->text() == QStringLiteral("アルファチャンネルを保持 (ProRes 4444 のみ)"))
            checkbox = candidate;
    for (auto *candidate : dialog.findChildren<QComboBox *>())
        if (candidate->findText(QStringLiteral("ProRes 4444")) >= 0) presets = candidate;
    refusal &= checkbox && presets;
    if (checkbox && presets) {
        refusal &= !checkbox->isChecked() && !checkbox->isEnabled();
        presets->setCurrentIndex(presets->findText(QStringLiteral("ProRes 4444")));
        refusal &= checkbox->isEnabled() && !checkbox->isChecked();
        checkbox->setChecked(true);
        QLineEdit *output = nullptr;
        for (auto *candidate : dialog.findChildren<QLineEdit *>())
            if (candidate->placeholderText() == QStringLiteral("Select output file..."))
                output = candidate;
        refusal &= output != nullptr;
        if (output) {
            output->setText(temp.filePath("dialog.mov"));
            refusal &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection)
                && dialog.config().keepAlpha && dialog.config().proresProfile == 4;
            // Mirror MainWindow's GUI job mapping: opt-in survives in the
            // queued config; the default omits the key entirely.
            const auto jobConfig = [](const ExportConfig &exportCfg) {
                QJsonObject cfg;
                cfg["videoCodec"] = exportCfg.videoCodec;
                if (exportCfg.proresProfile >= 0)
                    cfg["proresProfile"] = exportCfg.proresProfile;
                if (exportCfg.keepAlpha) cfg["keepAlpha"] = true;
                return cfg;
            };
            const QJsonObject cfg = jobConfig(dialog.config());
            refusal &= cfg.value("videoCodec").toString() == QStringLiteral("prores_ks")
                && cfg.value("proresProfile").toInt() == 4
                && cfg.value("keepAlpha").toBool()
                && !jobConfig(ExportConfig{}).contains("keepAlpha");
            RenderQueue guiQueue;
            RenderJob guiJob;
            guiJob.outputPath = dialog.config().outputPath;
            guiJob.codec = dialog.config().videoCodec;
            guiJob.exportConfig = cfg;
            guiQueue.addJob(guiJob);
            const auto jobs = guiQueue.jobs();
            refusal &= jobs.size() == 1;
            if (jobs.size() == 1)
                refusal &= jobs.front().exportConfig.value("keepAlpha").toBool()
                    && jobs.front().exportConfig.value("proresProfile").toInt() == 4;
            ExportConfig opaqueConfig = dialog.config();
            opaqueConfig.keepAlpha = false;
            refusal &= !jobConfig(opaqueConfig).contains("keepAlpha");
        }
        presets->setCurrentIndex(presets->findText(QStringLiteral("ProRes 422")));
        refusal &= !checkbox->isEnabled() && !checkbox->isChecked();
        presets->setCurrentIndex(0);
        refusal &= !checkbox->isEnabled() && !checkbox->isChecked();
    }
    gate(4, refusal);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
