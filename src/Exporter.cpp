#include "Exporter.h"
#include "CodecDetector.h"
#include "VideoEffect.h"
#include "SmartReframe.h"
#include "SubtitleTrackRenderer.h"
#include <QFileInfo>
#include <QPainter>
#include <cmath>

// ---------------------------------------------------------------------------
// Helper: build x265-params HDR10 metadata string (BT.2020 primaries / D65).
// Primaries per Rec.2020 (ITU-R BT.2020) × 50000 (x265 unit = 0.00002):
//   R (0.708, 0.292) → (35400, 14600)
//   G (0.170, 0.797) → ( 8500, 39850)
//   B (0.131, 0.046) → ( 6550,  2300)
//   WP D65 (0.3127, 0.3290) → (15635, 16450)
// ---------------------------------------------------------------------------
namespace {
static QByteArray buildX265Hdr10Params(const HDRSettings& hdr)
{
    const int maxLumX10000 = static_cast<int>(std::round(hdr.masterDisplayLuminanceMax * 10000));
    const int minLumX10000 = static_cast<int>(std::round(hdr.masterDisplayLuminanceMin * 10000));
    QByteArray s;
    s.append("hdr10=1:repeat-headers=1:colorprim=bt2020:"
             "transfer=smpte2084:colormatrix=bt2020nc:range=limited:"
             "master-display=G(8500,39850)B(6550,2300)R(35400,14600)"
             "WP(15635,16450)L(");
    s.append(QByteArray::number(maxLumX10000));
    s.append(',');
    s.append(QByteArray::number(minLumX10000));
    s.append("):max-cll=");
    s.append(QByteArray::number(hdr.maxCll));
    s.append(',');
    s.append(QByteArray::number(hdr.maxFall));
    return s;
}
} // namespace

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

    // Open output
    AVFormatContext *outFmt = nullptr;
    if (avformat_alloc_output_context2(&outFmt, nullptr, nullptr, config.outputPath.toUtf8().constData()) < 0) {
        emit exportFinished(false, "Failed to create output context");
        return;
    }

    // Find encoder — use best available if requested isn't found
    QString resolvedCodec = config.videoCodec;
    const AVCodec *encoder = avcodec_find_encoder_by_name(resolvedCodec.toUtf8().constData());
    if (!encoder) {
        // Auto-detect best encoder for this codec family
        if (resolvedCodec.contains("264")) resolvedCodec = CodecDetector::bestVideoEncoder("h264");
        else if (resolvedCodec.contains("265") || resolvedCodec.contains("hevc")) resolvedCodec = CodecDetector::bestVideoEncoder("h265");
        else if (resolvedCodec.contains("av1")) resolvedCodec = CodecDetector::bestVideoEncoder("av1");
        else if (resolvedCodec.contains("vp9")) resolvedCodec = CodecDetector::bestVideoEncoder("vp9");
        else resolvedCodec = "libx264";
        encoder = avcodec_find_encoder_by_name(resolvedCodec.toUtf8().constData());
    }
    if (!encoder) {
        avformat_free_context(outFmt);
        emit exportFinished(false, "Video encoder not found");
        return;
    }

    // HW encoder resolution: try hardware path when useHardwareAccel or hwEncoder is set
    // hwEncoder == "none" forces software only; "" or "auto" means auto-detect
    bool wantHW = (config.useHardwareAccel || !config.hwEncoder.isEmpty())
                  && config.hwEncoder != "none";
    bool isH264Family = (config.videoCodec == "libx264"
                         || config.videoCodec.contains("264"));
    bool isH265Family = (config.videoCodec == "libx265"
                         || config.videoCodec.contains("265")
                         || config.videoCodec.contains("hevc"));
    QString hwVendor = config.hwEncoder.toLower(); // "nvenc", "qsv", "amf", "auto", ""

    const AVCodec *hwEncoder = nullptr;
    QString hwEncoderName;

    if (wantHW && (isH264Family || isH265Family)) {
        // Build candidate list based on codec family and vendor hint
        QVector<QString> candidates;
        if (isH264Family) {
            if (hwVendor == "nvenc") candidates = {"h264_nvenc"};
            else if (hwVendor == "qsv") candidates = {"h264_qsv"};
            else if (hwVendor == "amf") candidates = {"h264_amf"};
            else candidates = {"h264_nvenc", "h264_qsv", "h264_amf"}; // auto
        } else {
            if (hwVendor == "nvenc") candidates = {"hevc_nvenc"};
            else if (hwVendor == "qsv") candidates = {"hevc_qsv"};
            else if (hwVendor == "amf") candidates = {"hevc_amf"};
            else candidates = {"hevc_nvenc", "hevc_qsv", "hevc_amf"}; // auto
        }
        for (const QString &candidateName : candidates) {
            if (CodecDetector::isEncoderAvailable(candidateName)) {
                hwEncoder = avcodec_find_encoder_by_name(candidateName.toUtf8().constData());
                if (hwEncoder) {
                    hwEncoderName = candidateName;
                    break;
                }
            }
        }
        if (hwEncoder) {
            encoder = hwEncoder;
            resolvedCodec = hwEncoderName;
            qInfo() << "Exporter: HW encoder selected:" << hwEncoderName;
        } else {
            qInfo() << "Exporter: no HW encoder available, falling back to SW encoder";
        }
    }

    qInfo() << "Exporter: using encoder" << encoder->name;

    // Resolve audio encoder — auto-detect best AAC if needed
    QString resolvedAudioCodec = config.audioCodec;
    if (resolvedAudioCodec == "aac" || resolvedAudioCodec.isEmpty()) {
        resolvedAudioCodec = CodecDetector::bestAACEncoder();
    }
    if (!CodecDetector::isEncoderAvailable(resolvedAudioCodec)) {
        resolvedAudioCodec = "aac"; // final fallback
    }

    // Create output stream
    AVStream *outStream = avformat_new_stream(outFmt, nullptr);
    if (!outStream) {
        avformat_free_context(outFmt);
        emit exportFinished(false, "Failed to create output stream");
        return;
    }

    // Setup encoder context
    AVCodecContext *encCtx = avcodec_alloc_context3(encoder);
    encCtx->width = config.width;
    encCtx->height = config.height;
    encCtx->time_base = {1, config.fps};
    encCtx->framerate = {config.fps, 1};
    // Resolve effective HDR mode: legacy config.hdr10 flag maps to "hdr10"
    const bool isHdr10Mode = (config.hdrSettings.mode == "hdr10" || config.hdr10);
    const bool isHlgMode   = (config.hdrSettings.mode == "hlg");

    AVPixelFormat targetPixFmt = AV_PIX_FMT_YUV420P;
    if (config.proresProfile >= 4) {
        targetPixFmt = AV_PIX_FMT_YUVA444P10LE;
    } else if (config.proresProfile >= 0) {
        targetPixFmt = AV_PIX_FMT_YUV422P10LE;
    } else if (isHdr10Mode || isHlgMode) {
        targetPixFmt = AV_PIX_FMT_YUV420P10LE;
    }
    encCtx->pix_fmt = targetPixFmt;
    encCtx->bit_rate = static_cast<int64_t>(config.videoBitrate) * 1000;

    if (isHdr10Mode) {
        encCtx->color_primaries = AVCOL_PRI_BT2020;
        encCtx->color_trc = AVCOL_TRC_SMPTE2084;
        encCtx->colorspace = AVCOL_SPC_BT2020_NCL;
        encCtx->color_range = AVCOL_RANGE_MPEG;
    } else if (isHlgMode) {
        encCtx->color_primaries = AVCOL_PRI_BT2020;
        encCtx->color_trc = AVCOL_TRC_ARIB_STD_B67;
        encCtx->colorspace = AVCOL_SPC_BT2020_NCL;
        encCtx->color_range = AVCOL_RANGE_MPEG;
    }

    if (outFmt->oformat->flags & AVFMT_GLOBALHEADER)
        encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // Codec-specific options
    AVDictionary *opts = nullptr;
    if (resolvedCodec == "h264_nvenc" || resolvedCodec == "hevc_nvenc") {
        av_dict_set(&opts, "preset", "p4", 0);
        av_dict_set(&opts, "rc", "vbr", 0);
        av_dict_set(&opts, "cq", "23", 0);
    } else if (resolvedCodec == "h264_qsv" || resolvedCodec == "hevc_qsv") {
        av_dict_set(&opts, "preset", "medium", 0);
    } else if (resolvedCodec == "h264_amf" || resolvedCodec == "hevc_amf") {
        av_dict_set(&opts, "quality", "balanced", 0);
    } else if (config.videoCodec == "libx264" || config.videoCodec == "libx265") {
        av_dict_set(&opts, "preset", "medium", 0);
        av_dict_set(&opts, "crf", "23", 0);
        if (config.videoCodec == "libx265") {
            if (isHdr10Mode) {
                av_dict_set(&opts, "profile", "main10", 0);
                const QByteArray x265p = buildX265Hdr10Params(config.hdrSettings);
                av_dict_set(&opts, "x265-params", x265p.constData(), 0);
            } else if (isHlgMode) {
                av_dict_set(&opts, "profile", "main10", 0);
                av_dict_set(&opts,
                            "x265-params",
                            "repeat-headers=1:colorprim=bt2020:"
                            "transfer=arib-std-b67:colormatrix=bt2020nc",
                            0);
            }
        }
    } else if (config.videoCodec == "libsvtav1") {
        av_dict_set(&opts, "preset", "8", 0);
        av_dict_set(&opts, "crf", "30", 0);
    } else if (config.videoCodec == "libvpx-vp9") {
        av_dict_set(&opts, "quality", "good", 0);
        av_dict_set(&opts, "cpu-used", "4", 0);
    } else if (config.videoCodec.startsWith("prores") && config.proresProfile >= 0) {
        const QByteArray profileStr = QByteArray::number(config.proresProfile);
        av_dict_set(&opts, "profile", profileStr.constData(), 0);
    }

    if (avcodec_open2(encCtx, encoder, &opts) < 0) {
        av_dict_free(&opts);
        avcodec_free_context(&encCtx);

        // HW encoder open failed — fall back to SW encoder (libx264/libx265)
        if (hwEncoder) {
            qInfo() << "Exporter: HW encoder open failed, falling back to SW encoder";
            QString swCodec = isH265Family ? "libx265" : "libx264";
            encoder = avcodec_find_encoder_by_name(swCodec.toUtf8().constData());
            resolvedCodec = swCodec;
            if (!encoder) {
                avformat_free_context(outFmt);
                emit exportFinished(false, "Video encoder not found (SW fallback failed)");
                return;
            }
            encCtx = avcodec_alloc_context3(encoder);
            encCtx->width = config.width;
            encCtx->height = config.height;
            encCtx->time_base = {1, config.fps};
            encCtx->framerate = {config.fps, 1};
            encCtx->pix_fmt = targetPixFmt;
            encCtx->bit_rate = static_cast<int64_t>(config.videoBitrate) * 1000;
            if (isHdr10Mode) {
                encCtx->color_primaries = AVCOL_PRI_BT2020;
                encCtx->color_trc = AVCOL_TRC_SMPTE2084;
                encCtx->colorspace = AVCOL_SPC_BT2020_NCL;
                encCtx->color_range = AVCOL_RANGE_MPEG;
            } else if (isHlgMode) {
                encCtx->color_primaries = AVCOL_PRI_BT2020;
                encCtx->color_trc = AVCOL_TRC_ARIB_STD_B67;
                encCtx->colorspace = AVCOL_SPC_BT2020_NCL;
                encCtx->color_range = AVCOL_RANGE_MPEG;
            }
            if (outFmt->oformat->flags & AVFMT_GLOBALHEADER)
                encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            AVDictionary *swOpts = nullptr;
            av_dict_set(&swOpts, "preset", "medium", 0);
            av_dict_set(&swOpts, "crf", "23", 0);
            if (swCodec == "libx265") {
                if (isHdr10Mode) {
                    av_dict_set(&swOpts, "profile", "main10", 0);
                    const QByteArray x265p = buildX265Hdr10Params(config.hdrSettings);
                    av_dict_set(&swOpts, "x265-params", x265p.constData(), 0);
                } else if (isHlgMode) {
                    av_dict_set(&swOpts, "profile", "main10", 0);
                    av_dict_set(&swOpts,
                                "x265-params",
                                "repeat-headers=1:colorprim=bt2020:"
                                "transfer=arib-std-b67:colormatrix=bt2020nc",
                                0);
                }
            }
            if (avcodec_open2(encCtx, encoder, &swOpts) < 0) {
                av_dict_free(&swOpts);
                avcodec_free_context(&encCtx);
                avformat_free_context(outFmt);
                emit exportFinished(false, "Failed to open SW fallback encoder");
                return;
            }
            av_dict_free(&swOpts);
            qInfo() << "Exporter: using encoder" << encoder->name;
        } else {
            avformat_free_context(outFmt);
            emit exportFinished(false, "Failed to open encoder");
            return;
        }
    } else {
        av_dict_free(&opts);
    }

    if (isHdr10Mode || isHlgMode) {
        qInfo() << "HDR mode:" << config.hdrSettings.mode
                << "MaxCLL=" << config.hdrSettings.maxCll
                << "MaxFALL=" << config.hdrSettings.maxFall;
    }

    avcodec_parameters_from_context(outStream->codecpar, encCtx);
    outStream->time_base = encCtx->time_base;

    // Open output file
    if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outFmt->pb, config.outputPath.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
            avcodec_free_context(&encCtx);
            avformat_free_context(outFmt);
            emit exportFinished(false, "Failed to open output file");
            return;
        }
    }

    if (avformat_write_header(outFmt, nullptr) < 0) {
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        emit exportFinished(false, "Failed to write header");
        return;
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

                outFrame->pts = globalPts++;

                // Encode
                if (avcodec_send_frame(encCtx, outFrame) == 0) {
                    AVPacket *encPkt = av_packet_alloc();
                    while (avcodec_receive_packet(encCtx, encPkt) == 0) {
                        av_packet_rescale_ts(encPkt, encCtx->time_base, outStream->time_base);
                        encPkt->stream_index = outStream->index;
                        av_interleaved_write_frame(outFmt, encPkt);
                        av_packet_unref(encPkt);
                    }
                    av_packet_free(&encPkt);
                }

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

    // Flush encoder
    avcodec_send_frame(encCtx, nullptr);
    AVPacket *flushPkt = av_packet_alloc();
    while (avcodec_receive_packet(encCtx, flushPkt) == 0) {
        av_packet_rescale_ts(flushPkt, encCtx->time_base, outStream->time_base);
        flushPkt->stream_index = outStream->index;
        av_interleaved_write_frame(outFmt, flushPkt);
        av_packet_unref(flushPkt);
    }
    av_packet_free(&flushPkt);

    av_write_trailer(outFmt);

    if (swsCtx) sws_freeContext(swsCtx);
    avcodec_free_context(&encCtx);
    if (!(outFmt->oformat->flags & AVFMT_NOFILE))
        avio_closep(&outFmt->pb);
    avformat_free_context(outFmt);

    if (m_cancelled) {
        emit exportFinished(false, "Export cancelled");
    } else {
        emit progressChanged(100);
        emit exportFinished(true, QString("Exported via %1 to %2")
                                      .arg(QString::fromUtf8(encoder->name))
                                      .arg(config.outputPath));
    }
}

bool Exporter::transcodeClip(const ClipInfo &, AVFormatContext *, AVCodecContext *,
                              AVStream *, SwsContext *, int64_t &)
{
    // Handled inline in doExport for now
    return true;
}
