#include "Exporter.h"
#include "CodecDetector.h"
#include "VideoEffect.h"
#include <QFileInfo>

Exporter::Exporter(QObject *parent)
    : QObject(parent)
{
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
    encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    encCtx->bit_rate = static_cast<int64_t>(config.videoBitrate) * 1000;

    if (outFmt->oformat->flags & AVFMT_GLOBALHEADER)
        encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // Codec-specific options
    AVDictionary *opts = nullptr;
    if (config.videoCodec == "libx264" || config.videoCodec == "libx265") {
        av_dict_set(&opts, "preset", "medium", 0);
        av_dict_set(&opts, "crf", "23", 0);
    } else if (config.videoCodec == "libsvtav1") {
        av_dict_set(&opts, "preset", "8", 0);
        av_dict_set(&opts, "crf", "30", 0);
    } else if (config.videoCodec == "libvpx-vp9") {
        av_dict_set(&opts, "quality", "good", 0);
        av_dict_set(&opts, "cpu-used", "4", 0);
    }

    if (avcodec_open2(encCtx, encoder, &opts) < 0) {
        av_dict_free(&opts);
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        emit exportFinished(false, "Failed to open encoder");
        return;
    }
    av_dict_free(&opts);

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
        outFrame->format = AV_PIX_FMT_YUV420P;
        outFrame->width = config.width;
        outFrame->height = config.height;
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
                    config.width, config.height, AV_PIX_FMT_YUV420P,
                    SWS_BILINEAR, nullptr, nullptr, nullptr) != swsCtx) {
                    if (swsCtx) sws_freeContext(swsCtx);
                    swsCtx = sws_getContext(
                        frame->width, frame->height, decCtx->pix_fmt,
                        config.width, config.height, AV_PIX_FMT_YUV420P,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                }

                av_frame_make_writable(outFrame);

                // Apply color correction & effects if clip has any
                bool hasEffects = !clip.colorCorrection.isDefault() || !clip.effects.isEmpty();
                if (hasEffects) {
                    // Decode to RGB for effect processing
                    QImage rgbFrame(config.width, config.height, QImage::Format_RGB888);
                    SwsContext *toRgbCtx = sws_getContext(
                        frame->width, frame->height, decCtx->pix_fmt,
                        config.width, config.height, AV_PIX_FMT_RGB24,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    uint8_t *rgbDest[1] = { rgbFrame.bits() };
                    int rgbLinesize[1] = { static_cast<int>(rgbFrame.bytesPerLine()) };
                    sws_scale(toRgbCtx, frame->data, frame->linesize, 0, frame->height,
                              rgbDest, rgbLinesize);
                    sws_freeContext(toRgbCtx);

                    // Apply effects
                    rgbFrame = VideoEffectProcessor::applyEffectStack(
                        rgbFrame, clip.colorCorrection, clip.effects);

                    // Convert processed RGB back to YUV420P
                    SwsContext *toYuvCtx = sws_getContext(
                        config.width, config.height, AV_PIX_FMT_RGB24,
                        config.width, config.height, AV_PIX_FMT_YUV420P,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    const uint8_t *rgbSrc[1] = { rgbFrame.constBits() };
                    int rgbSrcLinesize[1] = { static_cast<int>(rgbFrame.bytesPerLine()) };
                    sws_scale(toYuvCtx, rgbSrc, rgbSrcLinesize, 0, config.height,
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
        emit exportFinished(true, "Export completed: " + config.outputPath);
    }
}

bool Exporter::transcodeClip(const ClipInfo &, AVFormatContext *, AVCodecContext *,
                              AVStream *, SwsContext *, int64_t &)
{
    // Handled inline in doExport for now
    return true;
}
