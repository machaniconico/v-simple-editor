#include "Exporter.h"
#include "CodecDetector.h"
#include "VideoEffect.h"
#include "SmartReframe.h"
#include "SubtitleTrackRenderer.h"
#include "libavcore/Encode.h"
#include <QFileInfo>
#include <QPainter>
#include <cmath>

Exporter::Exporter(QObject *parent)
    : QObject(parent)
{
}

void Exporter::setSmartReframe(SmartReframe *reframe)
{
    m_smartReframe = reframe;
}

void Exporter::setSubtitleRenderer(SubtitleTrackRenderer *renderer)
{
    m_subtitleRenderer = renderer;
}

void Exporter::setLoudnessGainDb(double gainDb)
{
    m_loudnessGainDb = gainDb;
}

void Exporter::cancel()
{
    m_cancelled = true;
}

void Exporter::startExport(const ExportConfig &config, const QVector<ClipInfo> &clips)
{
    m_cancelled = false;
    m_thread = QThread::create([this, config, clips]() {
        doExport(config, clips);
    });
    connect(m_thread, &QThread::finished, m_thread, &QThread::deleteLater);
    m_thread->start();
}

bool Exporter::openInputFile(const QString &path, AVFormatContext **fmtCtx, AVCodecContext **decCtx, int *streamIndex)
{
    *fmtCtx = nullptr;
    *decCtx = nullptr;
    *streamIndex = -1;

    if (avformat_open_input(fmtCtx, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;

    if (avformat_find_stream_info(*fmtCtx, nullptr) < 0) {
        avformat_close_input(fmtCtx);
        return false;
    }

    for (unsigned i = 0; i < (*fmtCtx)->nb_streams; i++) {
        if ((*fmtCtx)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *streamIndex = static_cast<int>(i);
            break;
        }
    }

    if (*streamIndex < 0) {
        avformat_close_input(fmtCtx);
        return false;
    }

    auto *codecpar = (*fmtCtx)->streams[*streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        avformat_close_input(fmtCtx);
        return false;
    }

    *decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(*decCtx, codecpar);
    if (avcodec_open2(*decCtx, codec, nullptr) < 0) {
        avcodec_free_context(decCtx);
        avformat_close_input(fmtCtx);
        return false;
    }

    return true;
}

// LEGACY — bypasses the SSOT edit graph; do not use for new code.
// Production export goes RenderQueue -> tlrender::renderFrameAt (S8). This
// CPU-only transcode applies a hard-coded effect subset and skips the graph
// for 10-bit/HDR/ProRes. No UI action reaches this as of S12 (File->Export
// and Mobile Export now route through RenderQueue). See progress.txt S12.
void Exporter::doExport(const ExportConfig &config, const QVector<ClipInfo> &clips)
{
    if (clips.isEmpty()) {
        emit exportFinished(false, "No clips to export");
        return;
    }

    // Calculate total duration for progress
    double totalDuration = 0.0;
    for (const auto &clip : clips)
        totalDuration += clip.effectiveDuration();

    if (totalDuration <= 0.0) {
        emit exportFinished(false, "Total duration is zero");
        return;
    }

    // Resolve effective HDR mode: legacy config.hdr10 flag maps to "hdr10"
    const bool isHdr10Mode = (config.hdrSettings.mode == "hdr10" || config.hdr10);
    const bool isHlgMode   = (config.hdrSettings.mode == "hlg");

    // Pre-resolve the requested encoder name (best-of-family fallback) so the
    // helper sees a registered name. This mirrors Exporter's prior behaviour
    // when config.videoCodec is a family alias like "h264".
    QString resolvedCodec = config.videoCodec;
    if (!avcodec_find_encoder_by_name(resolvedCodec.toUtf8().constData())) {
        if (resolvedCodec.contains("264")) resolvedCodec = CodecDetector::bestVideoEncoder("h264");
        else if (resolvedCodec.contains("265") || resolvedCodec.contains("hevc")) resolvedCodec = CodecDetector::bestVideoEncoder("h265");
        else if (resolvedCodec.contains("av1")) resolvedCodec = CodecDetector::bestVideoEncoder("av1");
        else if (resolvedCodec.contains("vp9")) resolvedCodec = CodecDetector::bestVideoEncoder("vp9");
        else resolvedCodec = "libx264";
    }

    // [P1-MINOR-2] Audio mux is NYI in this CPU path — libavcore::Encode does
    // not yet support audio streams. resolvedAudioCodec is computed for parity
    // with the previous Exporter behaviour (and so we keep one place where the
    // best-of-family AAC detection lives) but it is intentionally unused
    // downstream until audio mux is wired into the libavcore helper.
#if 0  // parity-only; TODO: enable when libavcore::Encode supports audio
    QString resolvedAudioCodec = config.audioCodec;
    if (resolvedAudioCodec == "aac" || resolvedAudioCodec.isEmpty()) {
        resolvedAudioCodec = CodecDetector::bestAACEncoder();
    }
    if (!CodecDetector::isEncoderAvailable(resolvedAudioCodec)) {
        resolvedAudioCodec = "aac";
    }
#endif

    // Open the encoder session via the libavcore helper (HW>SW fallback inside).
    libavcore::EncodeRequest req;
    req.width = config.width;
    req.height = config.height;
    req.fps = config.fps;
    req.videoBitrateBits = static_cast<int64_t>(config.videoBitrate) * 1000;
    req.outputPath = config.outputPath.toUtf8().toStdString();
    req.videoCodecName = resolvedCodec.toUtf8().toStdString();
    req.hwVendorHint = config.hwEncoder.toLower().toUtf8().toStdString();
    req.useHardwareAccel = config.useHardwareAccel;
    req.isHdr10 = isHdr10Mode;
    req.isHlg = isHlgMode;
    req.proresProfile = config.proresProfile;
    req.hdrMasterMaxNits = config.hdrSettings.masterDisplayLuminanceMax;
    req.hdrMasterMinNits = config.hdrSettings.masterDisplayLuminanceMin;
    req.hdrMaxCll = config.hdrSettings.maxCll;
    req.hdrMaxFall = config.hdrSettings.maxFall;

    // [P1-M1] Restore the legacy Exporter.cpp HW candidate functional probe
    // (CodecDetector::isEncoderAvailable) that was dropped during the
    // libavcore refactor. libavcore::FrameEncoder's HW candidate loop now
    // calls this hook BEFORE avcodec_find_encoder_by_name so a stub-registered
    // encoder without a working runtime (e.g. NVENC on a non-NVIDIA host) is
    // skipped exactly like the legacy code path. Qt-side hook keeps the
    // libavcore header Qt-free.
    req.encoderAvailableHook = [](const std::string& name) {
        return CodecDetector::isEncoderAvailable(QString::fromStdString(name));
    };

    libavcore::FrameEncoder encoderSession;
    if (auto err = encoderSession.open(req)) {
        emit exportFinished(false, QString::fromStdString(*err));
        return;
    }

    const AVPixelFormat targetPixFmt = encoderSession.outputPixelFormat();
    qInfo() << "Exporter: using encoder" << QString::fromStdString(encoderSession.activeEncoderName());

    if (isHdr10Mode || isHlgMode) {
        qInfo() << "HDR mode:" << config.hdrSettings.mode
                << "MaxCLL=" << config.hdrSettings.maxCll
                << "MaxFALL=" << config.hdrSettings.maxFall;
    }

    // Process each clip
    SwsContext *swsCtx = nullptr;
    int64_t globalPts = 0;
    double processedDuration = 0.0;

    for (int clipIdx = 0; clipIdx < clips.size() && !m_cancelled; ++clipIdx) {
        const auto &clip = clips[clipIdx];

        AVFormatContext *inFmt = nullptr;
        AVCodecContext *decCtx = nullptr;
        int videoIdx = -1;

        if (!openInputFile(clip.filePath, &inFmt, &decCtx, &videoIdx)) {
            processedDuration += clip.effectiveDuration();
            continue;
        }

        // Seek to in-point
        if (clip.inPoint > 0.0) {
            int64_t seekTarget = static_cast<int64_t>(clip.inPoint * AV_TIME_BASE);
            av_seek_frame(inFmt, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(decCtx);
        }

        double clipEnd = (clip.outPoint > 0.0) ? clip.outPoint : clip.duration;
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        AVFrame *outFrame = av_frame_alloc();
        outFrame->format = targetPixFmt;
        outFrame->width = config.width;
        outFrame->height = config.height;
        if (isHdr10Mode) {
            outFrame->color_primaries = AVCOL_PRI_BT2020;
            outFrame->color_trc = AVCOL_TRC_SMPTE2084;
            outFrame->colorspace = AVCOL_SPC_BT2020_NCL;
            outFrame->color_range = AVCOL_RANGE_MPEG;
        } else if (isHlgMode) {
            outFrame->color_primaries = AVCOL_PRI_BT2020;
            outFrame->color_trc = AVCOL_TRC_ARIB_STD_B67;
            outFrame->colorspace = AVCOL_SPC_BT2020_NCL;
            outFrame->color_range = AVCOL_RANGE_MPEG;
        }
        av_frame_get_buffer(outFrame, 0);

        while (av_read_frame(inFmt, packet) >= 0 && !m_cancelled) {
            if (packet->stream_index != videoIdx) {
                av_packet_unref(packet);
                continue;
            }

            if (avcodec_send_packet(decCtx, packet) < 0) {
                av_packet_unref(packet);
                continue;
            }

            while (avcodec_receive_frame(decCtx, frame) == 0 && !m_cancelled) {
                // Check time bounds
                AVStream *inStream = inFmt->streams[videoIdx];
                double framePts = static_cast<double>(frame->pts) * av_q2d(inStream->time_base);
                if (framePts < clip.inPoint) continue;
                if (framePts >= clipEnd) goto clip_done;

                // Scale frame
                if (!swsCtx || sws_getContext(
                    frame->width, frame->height, decCtx->pix_fmt,
                    config.width, config.height, targetPixFmt,
                    SWS_BILINEAR, nullptr, nullptr, nullptr) != swsCtx) {
                    if (swsCtx) sws_freeContext(swsCtx);
                    swsCtx = sws_getContext(
                        frame->width, frame->height, decCtx->pix_fmt,
                        config.width, config.height, targetPixFmt,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                }

                av_frame_make_writable(outFrame);

                // 10-bit outputs (HDR10 / HLG / ProRes) bypass the 8-bit RGB24 effect round-trip.
                const bool tenBitPath = isHdr10Mode || isHlgMode || config.proresProfile >= 0;
                bool hasEffects = !tenBitPath
                                  && (!clip.colorCorrection.isDefault() || !clip.effects.isEmpty());
                bool needsRgbPass = hasEffects
                                    || (m_smartReframe != nullptr)
                                    || (m_subtitleRenderer != nullptr);
                if (needsRgbPass) {
                    QImage workingImage;
                    if (hasEffects) {
                        // Decode to RGB for effect processing
                        workingImage = QImage(config.width, config.height, QImage::Format_RGB888);
                        SwsContext *toRgbCtx = sws_getContext(
                            frame->width, frame->height, decCtx->pix_fmt,
                            config.width, config.height, AV_PIX_FMT_RGB24,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
                        uint8_t *rgbDest[1] = { workingImage.bits() };
                        int rgbLinesize[1] = { static_cast<int>(workingImage.bytesPerLine()) };
                        sws_scale(toRgbCtx, frame->data, frame->linesize, 0, frame->height,
                                  rgbDest, rgbLinesize);
                        sws_freeContext(toRgbCtx);

                        // Apply effects
                        workingImage = VideoEffectProcessor::applyEffectStack(
                            workingImage, clip.colorCorrection, clip.effects);
                    } else {
                        // No traditional effects, but reframe/subtitles need RGB.
                        // Scale to output size first.
                        workingImage = QImage(config.width, config.height, QImage::Format_RGB888);
                        SwsContext *toRgbCtx = sws_getContext(
                            frame->width, frame->height, decCtx->pix_fmt,
                            config.width, config.height, AV_PIX_FMT_RGB24,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
                        uint8_t *rgbDest[1] = { workingImage.bits() };
                        int rgbLinesize[1] = { static_cast<int>(workingImage.bytesPerLine()) };
                        sws_scale(toRgbCtx, frame->data, frame->linesize, 0, frame->height,
                                  rgbDest, rgbLinesize);
                        sws_freeContext(toRgbCtx);
                    }

                    // SmartReframe: crop/pan to target aspect
                    if (m_smartReframe != nullptr) {
                        workingImage = m_smartReframe->applyReframe(
                            workingImage, framePts - clip.inPoint,
                            QSize(config.width, config.height));
                    }

                    // Subtitle burn-in
                    if (m_subtitleRenderer != nullptr) {
                        QPainter painter(&workingImage);
                        m_subtitleRenderer->paintOnto(
                            painter,
                            QRectF(0, 0, workingImage.width(), workingImage.height()),
                            framePts - clip.inPoint);
                    }

                    // Convert processed RGB back to YUV420P
                    SwsContext *toYuvCtx = sws_getContext(
                        workingImage.width(), workingImage.height(), AV_PIX_FMT_RGB24,
                        config.width, config.height, AV_PIX_FMT_YUV420P,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    const uint8_t *rgbSrc[1] = { workingImage.constBits() };
                    int rgbSrcLinesize[1] = { static_cast<int>(workingImage.bytesPerLine()) };
                    sws_scale(toYuvCtx, rgbSrc, rgbSrcLinesize, 0, workingImage.height(),
                              outFrame->data, outFrame->linesize);
                    sws_freeContext(toYuvCtx);
                } else {
                    sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height,
                              outFrame->data, outFrame->linesize);
                }

                // Encode via libavcore helper (handles send_frame+packet drain).
                encoderSession.pushFrameNative(outFrame, globalPts++);

                // Update progress
                double currentTime = framePts - clip.inPoint;
                int percent = static_cast<int>((processedDuration + currentTime) / totalDuration * 100.0);
                emit progressChanged(qMin(percent, 99));
            }

            av_packet_unref(packet);
        }

clip_done:
        processedDuration += clip.effectiveDuration();
        av_frame_free(&frame);
        av_frame_free(&outFrame);
        av_packet_free(&packet);
        avcodec_free_context(&decCtx);
        avformat_close_input(&inFmt);
    }

    // Flush + trailer via helper (drains remaining packets, writes trailer,
    // closes file). FrameEncoder dtor cleans the rest of the libav state.
    encoderSession.finalize();

    if (swsCtx) sws_freeContext(swsCtx);

    if (m_cancelled) {
        emit exportFinished(false, "Export cancelled");
    } else {
        emit progressChanged(100);
        emit exportFinished(true, QString("Exported via %1 to %2")
                                      .arg(QString::fromStdString(encoderSession.activeEncoderName()))
                                      .arg(config.outputPath));
    }
}

bool Exporter::transcodeClip(const ClipInfo &, AVFormatContext *, AVCodecContext *,
                              AVStream *, SwsContext *, int64_t &)
{
    // Handled inline in doExport for now
    return true;
}
