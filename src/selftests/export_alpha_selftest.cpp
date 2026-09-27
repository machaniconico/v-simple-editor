#include "../ExportDialog.h"
#include "../CodecDetector.h"
#include "../RenderQueue.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../libavcore/Encode.h"
#include "../libavcore/FrameGrab.h"
#include "SelftestGate.h"
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <cerrno>
#include <cstdio>
extern "C" {
#include <libavutil/error.h>
}

namespace {
// This fixture is static, so its first frame contains the same covered and
// uncovered regions as every later frame. The native VP9 decoder ignores the
// WebM BlockAdditional alpha layer; explicitly use libvpx-vp9 to measure it.
QImage grabVp9AlphaFrame(const QString &path)
{
    struct DecoderResources {
        AVFormatContext *format = nullptr;
        AVCodecContext *decoder = nullptr;
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        AVFrame *rgba = av_frame_alloc();
        SwsContext *scaler = nullptr;
        ~DecoderResources()
        {
            sws_freeContext(scaler);
            av_frame_free(&rgba);
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&decoder);
            avformat_close_input(&format);
        }
    } resources;
    const AVCodec *codec = avcodec_find_decoder_by_name("libvpx-vp9");
    if (!codec || !resources.packet || !resources.frame || !resources.rgba
        || avformat_open_input(&resources.format, path.toUtf8().constData(), nullptr, nullptr) < 0
        || avformat_find_stream_info(resources.format, nullptr) < 0)
        return {};
    const int stream = av_find_best_stream(resources.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0 || resources.format->streams[stream]->codecpar->codec_id != AV_CODEC_ID_VP9)
        return {};
    resources.decoder = avcodec_alloc_context3(codec);
    if (!resources.decoder
        || avcodec_parameters_to_context(resources.decoder, resources.format->streams[stream]->codecpar) < 0
        || avcodec_open2(resources.decoder, codec, nullptr) < 0)
        return {};

    bool draining = false;
    for (;;) {
        const int received = avcodec_receive_frame(resources.decoder, resources.frame);
        if (received == 0) break;
        if (received != AVERROR(EAGAIN) || draining) return {};
        int read = 0;
        do {
            av_packet_unref(resources.packet);
            read = av_read_frame(resources.format, resources.packet);
        } while (read >= 0 && resources.packet->stream_index != stream);
        if (read < 0 && read != AVERROR_EOF) return {};
        draining = read == AVERROR_EOF;
        const int sent = avcodec_send_packet(resources.decoder, draining ? nullptr : resources.packet);
        av_packet_unref(resources.packet);
        if (sent < 0) return {};
    }

    const AVFrame *frame = resources.frame;
    // Reject a decoder result that has already discarded alpha.
    if (frame->format != AV_PIX_FMT_YUVA420P) return {};
    resources.scaler = sws_getContext(frame->width, frame->height,
        static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
        AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    resources.rgba->format = AV_PIX_FMT_RGBA;
    resources.rgba->width = frame->width;
    resources.rgba->height = frame->height;
    if (!resources.scaler || av_frame_get_buffer(resources.rgba, 64) < 0
        || sws_scale(resources.scaler, frame->data, frame->linesize, 0, frame->height,
            resources.rgba->data, resources.rgba->linesize) != frame->height)
        return {};
    // Copy out of the padded FFmpeg allocation before resources are released.
    return QImage(resources.rgba->data[0], frame->width, frame->height,
        resources.rgba->linesize[0], QImage::Format_RGBA8888).copy();
}

bool render(Timeline &timeline, const QString &path, bool alpha, int profile, const QString &codec = QStringLiteral("prores"))
{
    RenderQueue queue;
    RenderJob job;
    job.outputPath = path;
    job.timeline = &timeline;
    job.codec = codec;
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
    selftest::GateTally tally;
    const auto gate = [&](int n, bool ok) { tally.gate(n, ok); };
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
    const bool defaultOpaque = !ExportConfig{}.keepAlpha;
    const int opaqueAlphaFrames = libavcore::FrameEncoder::alphaFrameCountForTest();
    const int blackComposites = RenderQueue::blackCompositeCountForTest();
    std::fprintf(stderr, "export-alpha G1: defaultOpaque=%d rendered=%d alphaFrames=%d blackComposites=%d\n",
        defaultOpaque, opaque, opaqueAlphaFrames, blackComposites);
    gate(1, defaultOpaque && opaque && opaqueAlphaFrames == 0 && blackComposites >= 1);
    libavcore::FrameEncoder::setAlphaInputEnabledForTest(true);

    bool alpha = ready;
    for (const int profile : {4, 5}) {
        libavcore::FrameEncoder::resetAlphaFrameCountForTest();
        RenderQueue::resetBlackCompositeCountForTest();
        const QString path = temp.filePath(QStringLiteral("alpha-%1.mov").arg(profile));
        const bool rendered = ready && render(timeline, path, true, profile);
        const bool pixels = rendered && pixelsMatch(libavcore::grabFrameAt(path, 0.5), true);
        const int alphaFrames = libavcore::FrameEncoder::alphaFrameCountForTest();
        const int blackFrames = RenderQueue::blackCompositeCountForTest();
        std::fprintf(stderr, "export-alpha G2 profile=%d: rendered=%d pixels=%d alphaFrames=%d blackComposites=%d\n",
            profile, rendered, pixels, alphaFrames, blackFrames);
        alpha &= rendered && pixels && alphaFrames > 0 && blackFrames == 0;
    }
    gate(2, alpha);
    const bool opaquePixels = opaque && pixelsMatch(libavcore::grabFrameAt(opaquePath, 0.5), false);
    std::fprintf(stderr, "export-alpha G3: rendered=%d pixels=%d\n", opaque, opaquePixels);
    gate(3, opaquePixels);

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
    auto *checkbox = dialog.findChild<QCheckBox *>("keepAlpha");
    auto *presets = dialog.findChild<QComboBox *>("preset");
    refusal &= checkbox && presets;
    if (checkbox && presets) {
        refusal &= !checkbox->isChecked() && !checkbox->isEnabled();
        presets->setCurrentIndex(presets->findText(QStringLiteral("ProRes 4444")));
        refusal &= checkbox->isEnabled() && !checkbox->isChecked();
        checkbox->setChecked(true);
        auto *output = dialog.findChild<QLineEdit *>("outputPath");
        refusal &= output != nullptr;
        if (output) {
            output->setText(temp.filePath("dialog.mov"));
            refusal &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection)
                && dialog.config().keepAlpha && dialog.config().proresProfile == 4;
            // Mirror MainWindow::exportVideo (MainWindow.cpp:10049-10079):
            // codec and alpha config must be mapped together before addJob.
            const auto makeGuiJob = [](const ExportConfig &exportCfg) -> RenderJob {
                RenderJob job;
                job.outputPath = exportCfg.outputPath;
                QJsonObject cfg;
                cfg["videoCodec"] = exportCfg.videoCodec;
                job.codec = exportCfg.videoCodec;
                if (exportCfg.proresProfile >= 0)
                    cfg["proresProfile"] = exportCfg.proresProfile;
                if (exportCfg.keepAlpha) cfg["keepAlpha"] = true;
                job.exportConfig = cfg;
                return job;
            };
            const RenderJob guiJob = makeGuiJob(dialog.config());
            refusal &= !makeGuiJob(ExportConfig{}).exportConfig.contains("keepAlpha");
            RenderQueue guiQueue;
            guiQueue.addJob(guiJob);
            const auto jobs = guiQueue.jobs();
            refusal &= jobs.size() == 1;
            if (jobs.size() == 1)
                refusal &= jobs.front().exportConfig.value("videoCodec").toString()
                        == QStringLiteral("prores_ks")
                    && jobs.front().exportConfig.value("keepAlpha").toBool()
                    && jobs.front().exportConfig.value("proresProfile").toInt() == 4;

            // Leaving the flat codec at its default overwrites the JSON codec
            // with H.264 in addJob, so the same alpha config must be rejected.
            RenderQueue missingCodecQueue;
            QString missingCodecError;
            QObject::connect(&missingCodecQueue, &RenderQueue::jobFailed,
                &missingCodecQueue, [&](int, const QString &message) {
                    missingCodecError = message;
                });
            RenderJob missingCodecJob;
            missingCodecJob.outputPath = guiJob.outputPath;
            missingCodecJob.exportConfig = guiJob.exportConfig;
            missingCodecQueue.addJob(missingCodecJob);
            refusal &= missingCodecQueue.jobs().isEmpty()
                && missingCodecError == QStringLiteral("このコーデックはアルファを保持できません");

            ExportConfig opaqueConfig = dialog.config();
            opaqueConfig.keepAlpha = false;
            refusal &= !makeGuiJob(opaqueConfig).exportConfig.contains("keepAlpha");
        }
        presets->setCurrentIndex(presets->findText(QStringLiteral("ProRes 422")));
        refusal &= !checkbox->isEnabled() && !checkbox->isChecked();
        presets->setCurrentIndex(0);
        refusal &= !checkbox->isEnabled() && !checkbox->isChecked();
    }
    gate(4, refusal);

    QString vp9Reason;
    const bool vp9EncoderAvailable = selftest::requireEncoder("libvpx-vp9", &vp9Reason);
    const bool vp9AlphaDecoderAvailable = avcodec_find_decoder_by_name("libvpx-vp9") != nullptr;
    if (vp9EncoderAvailable && vp9AlphaDecoderAvailable) {
        const QString path = temp.filePath("alpha.webm");
        libavcore::FrameEncoder::resetAlphaFrameCountForTest();
        RenderQueue::resetBlackCompositeCountForTest();
        bool vp9 = ready && render(timeline, path, true, -1, "libvpx-vp9");
        const QImage image = grabVp9AlphaFrame(path);
        const bool vp9Pixels = pixelsMatch(image, true);
        const int vp9AlphaFrames = libavcore::FrameEncoder::alphaFrameCountForTest();
        const int vp9BlackFrames = RenderQueue::blackCompositeCountForTest();
        std::fprintf(stderr, "export-alpha G5: rendered=%d pixels=%d alphaFrames=%d blackComposites=%d\n",
            vp9, vp9Pixels, vp9AlphaFrames, vp9BlackFrames);
        vp9 &= vp9Pixels && vp9AlphaFrames > 0 && vp9BlackFrames == 0;
        if (!image.isNull()) {
            vp9 &= image.pixelColor(96, 32).alpha() == 0
                && image.pixelColor(32, 32).alpha() == 255;
        }
        vp9 &= checkbox && presets;
        if (checkbox && presets) {
            const int index = presets->findText(QStringLiteral("VP9 WebM"));
            vp9 &= index >= 0;
            if (index >= 0) {
                presets->setCurrentIndex(index);
                vp9 &= checkbox->isEnabled() && !checkbox->isChecked();
                checkbox->setChecked(true);
                if (auto *output = dialog.findChild<QLineEdit *>("outputPath"))
                    output->setText(temp.filePath("dialog.webm"));
                else vp9 = false;
                vp9 &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection)
                    && dialog.config().keepAlpha
                    && dialog.config().videoCodec == QStringLiteral("libvpx-vp9");
                presets->setCurrentIndex(0);
                vp9 &= !checkbox->isEnabled() && !checkbox->isChecked();
            }
        }
        gate(5, vp9);
    } else {
        tally.skip(5, vp9EncoderAvailable ? "libvpx-vp9 alpha decoder unavailable"
            : qPrintable(vp9Reason));
    }

    bool policy = true;
    for (const QString &codec : {QStringLiteral("prores"), QStringLiteral("prores_ks"),
            QStringLiteral("vp9"), QStringLiteral("libvpx-vp9"),
            QStringLiteral("h264"), QStringLiteral("libx264")}) {
        for (int profile = -1; profile <= 6; ++profile) {
            const bool expected = codec == "vp9" || codec == "libvpx-vp9"
                || ((codec == "prores" || codec == "prores_ks") && (profile == 4 || profile == 5));
            const QJsonObject config{{"videoCodec", codec}, {"proresProfile", profile}, {"keepAlpha", true}};
            policy &= RenderQueue::alphaExportError(config).isEmpty() == expected;
            policy &= (libavcore::alphaExportPixelFormat(codec.toStdString(), profile)
                != AV_PIX_FMT_NONE) == expected;
            auto audioOnly = config;
            audioOnly.insert("audioOnly", true);
            policy &= !RenderQueue::alphaExportError(audioOnly).isEmpty();
        }
    }
    policy &= !RenderQueue::alphaExportError(QJsonObject{{"videoCodec", "prores_ks"},
        {"proresProfile", 4.5}, {"keepAlpha", true}}).isEmpty();
    gate(6, policy);
    return tally.finish();
}
