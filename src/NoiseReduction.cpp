#include "NoiseReduction.h"
#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QFileInfo>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libswresample/swresample.h>
}

namespace {

QMutex g_runStateMutex;
QHash<const NoiseReduction *, QThread *> g_runningThreads;
QHash<const NoiseReduction *, std::shared_ptr<std::atomic_bool>> g_cancelFlags;

int findStreamIndexLocal(AVFormatContext *fmtCtx, int mediaType)
{
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == mediaType)
            return static_cast<int>(i);
    }
    return -1;
}

void postProgress(const QPointer<NoiseReduction> &receiver, int percent)
{
    if (!receiver)
        return;
    QMetaObject::invokeMethod(receiver.data(), [receiver, percent]() {
        if (receiver)
            emit receiver->progressChanged(percent);
    }, Qt::QueuedConnection);
}

void postComplete(const QPointer<NoiseReduction> &receiver, bool success, const QString &message)
{
    if (!receiver)
        return;
    QMetaObject::invokeMethod(receiver.data(), [receiver, success, message]() {
        if (receiver)
            emit receiver->denoiseComplete(success, message);
    }, Qt::QueuedConnection);
}

void cleanupRunState(const NoiseReduction *owner, QThread *thread)
{
    QMutexLocker locker(&g_runStateMutex);
    if (g_runningThreads.value(owner) == thread) {
        g_runningThreads.remove(owner);
        g_cancelFlags.remove(owner);
    }
}

bool startDenoiseWorker(NoiseReduction *owner,
                        QThread *&memberThread,
                        const QString &inputPath,
                        const QString &outputPath,
                        const QString &audioFilter,
                        const QString &videoFilter,
                        const std::function<bool(const QString &, const QString &,
                                                 const QString &, const QString &,
                                                 const std::shared_ptr<std::atomic_bool> &,
                                                 const QPointer<NoiseReduction> &)> &worker)
{
    {
        QMutexLocker locker(&g_runStateMutex);
        QThread *running = g_runningThreads.value(owner, nullptr);
        if (running && running->isRunning())
            return false;
    }

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    QPointer<NoiseReduction> receiver(owner);
    QThread *thread = QThread::create([worker, inputPath, outputPath,
                                       audioFilter, videoFilter, cancelled, receiver]() {
        worker(inputPath, outputPath, audioFilter, videoFilter, cancelled, receiver);
    });

    {
        QMutexLocker locker(&g_runStateMutex);
        g_runningThreads.insert(owner, thread);
        g_cancelFlags.insert(owner, cancelled);
    }

    memberThread = thread;
    QObject::connect(thread, &QThread::finished, owner, [owner, thread, &memberThread]() {
        cleanupRunState(owner, thread);
        if (memberThread == thread)
            memberThread = nullptr;
    });
    QObject::connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
    return true;
}

} // namespace

NoiseReduction::NoiseReduction(QObject *parent)
    : QObject(parent)
{
    connect(this, &QObject::destroyed, [owner = this]() {
        std::shared_ptr<std::atomic_bool> cancelled;
        QThread *thread = nullptr;
        {
            QMutexLocker locker(&g_runStateMutex);
            cancelled = g_cancelFlags.value(owner);
            thread = g_runningThreads.value(owner, nullptr);
            g_cancelFlags.remove(owner);
            g_runningThreads.remove(owner);
        }
        if (cancelled)
            cancelled->store(true);
        if (thread && thread->isRunning())
            thread->wait();
    });
}

void NoiseReduction::cancel()
{
    m_cancelled = true;
    QMutexLocker locker(&g_runStateMutex);
    auto cancelled = g_cancelFlags.value(this);
    if (cancelled)
        cancelled->store(true);
}

// --- Filter description builders ---

QString NoiseReduction::buildAudioFilterDesc(const AudioDenoiseConfig &config)
{
    // afftdn: FFT-based audio noise reduction
    //   nr = noise reduction amount in dB (0-97, mapped from 0-1)
    //   nf = noise floor in dB
    //   tn = enable adaptive noise profiling
    double nr = config.reductionAmount * 97.0; // map 0-1 to 0-97 dB
    QString filter = QString("afftdn=nr=%1:nf=%2")
                         .arg(nr, 0, 'f', 1)
                         .arg(config.noiseFloor, 0, 'f', 1);
    if (config.adaptiveMode)
        filter += ":tn=1";
    return filter;
}

QString NoiseReduction::buildVideoFilterDesc(const VideoDenoiseConfig &config)
{
    if (config.method == VideoDenoiseMethod::NLMeans) {
        // nlmeans: non-local means denoising
        //   s = denoising strength (spatial)
        //   p = patch size (default 7)
        //   r = research window size, controls temporal-like extent
        int patchSize = 7;
        int researchSize = qBound(3, static_cast<int>(config.temporalStrength), 30);
        return QString("nlmeans=s=%1:p=%2:r=%3")
                   .arg(config.spatialStrength, 0, 'f', 1)
                   .arg(patchSize)
                   .arg(researchSize);
    }

    // hqdn3d: high quality 3D denoising
    //   luma_spatial, chroma_spatial, luma_tmp, chroma_tmp
    double lumaSpatial = config.spatialStrength;
    double chromaSpatial = config.spatialStrength * 0.75;
    double lumaTmp = config.temporalStrength;
    double chromaTmp = config.temporalStrength * 0.75;
    return QString("hqdn3d=%1:%2:%3:%4")
               .arg(lumaSpatial, 0, 'f', 1)
               .arg(chromaSpatial, 0, 'f', 1)
               .arg(lumaTmp, 0, 'f', 1)
               .arg(chromaTmp, 0, 'f', 1);
}

// --- Find stream index ---

int NoiseReduction::findStreamIndex(AVFormatContext *fmtCtx, int mediaType)
{
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == mediaType)
            return static_cast<int>(i);
    }
    return -1;
}

// --- Core filter-graph processing ---

static bool processWithFilterImpl(const QString &inputPath, const QString &outputPath,
                                  const QString &audioFilter, const QString &videoFilter,
                                  const std::shared_ptr<std::atomic_bool> &cancelled,
                                  const QPointer<NoiseReduction> &receiver)
{
    // Open input
    AVFormatContext *inFmtCtx = nullptr;
    if (avformat_open_input(&inFmtCtx, inputPath.toUtf8().constData(), nullptr, nullptr) < 0) {
        postComplete(receiver, false, "Failed to open input file");
        return false;
    }
    if (avformat_find_stream_info(inFmtCtx, nullptr) < 0) {
        avformat_close_input(&inFmtCtx);
        postComplete(receiver, false, "Failed to read stream info");
        return false;
    }

    int audioIdx = findStreamIndexLocal(inFmtCtx, AVMEDIA_TYPE_AUDIO);
    int videoIdx = findStreamIndexLocal(inFmtCtx, AVMEDIA_TYPE_VIDEO);

    bool hasAudioFilter = !audioFilter.isEmpty() && audioIdx >= 0;
    bool hasVideoFilter = !videoFilter.isEmpty() && videoIdx >= 0;

    if (!hasAudioFilter && !hasVideoFilter) {
        avformat_close_input(&inFmtCtx);
        postComplete(receiver, false, "No applicable streams for filtering");
        return false;
    }

    // Open decoders
    AVCodecContext *audioDecCtx = nullptr;
    AVCodecContext *videoDecCtx = nullptr;

    if (hasAudioFilter) {
        auto *par = inFmtCtx->streams[audioIdx]->codecpar;
        const AVCodec *dec = avcodec_find_decoder(par->codec_id);
        if (!dec) { hasAudioFilter = false; }
        else {
            audioDecCtx = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(audioDecCtx, par);
            if (avcodec_open2(audioDecCtx, dec, nullptr) < 0) {
                avcodec_free_context(&audioDecCtx);
                hasAudioFilter = false;
            }
        }
    }

    if (hasVideoFilter) {
        auto *par = inFmtCtx->streams[videoIdx]->codecpar;
        const AVCodec *dec = avcodec_find_decoder(par->codec_id);
        if (!dec) { hasVideoFilter = false; }
        else {
            videoDecCtx = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(videoDecCtx, par);
            if (avcodec_open2(videoDecCtx, dec, nullptr) < 0) {
                avcodec_free_context(&videoDecCtx);
                hasVideoFilter = false;
            }
        }
    }

    if (!hasAudioFilter && !hasVideoFilter) {
        if (audioDecCtx) avcodec_free_context(&audioDecCtx);
        if (videoDecCtx) avcodec_free_context(&videoDecCtx);
        avformat_close_input(&inFmtCtx);
        postComplete(receiver, false, "Failed to open decoders");
        return false;
    }

    // --- Build audio filter graph ---
    AVFilterGraph *audioGraph = nullptr;
    AVFilterContext *abufferSrcCtx = nullptr;
    AVFilterContext *abufferSinkCtx = nullptr;

    if (hasAudioFilter) {
        audioGraph = avfilter_graph_alloc();
        const AVFilter *abuffersrc = avfilter_get_by_name("abuffer");
        const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

        char chLayoutStr[64] = {};
        av_channel_layout_describe(&audioDecCtx->ch_layout, chLayoutStr, sizeof(chLayoutStr));

        QString srcArgs = QString("time_base=%1/%2:sample_rate=%3:sample_fmt=%4:channel_layout=%5")
                              .arg(inFmtCtx->streams[audioIdx]->time_base.num)
                              .arg(inFmtCtx->streams[audioIdx]->time_base.den)
                              .arg(audioDecCtx->sample_rate)
                              .arg(av_get_sample_fmt_name(audioDecCtx->sample_fmt))
                              .arg(chLayoutStr);

        if (avfilter_graph_create_filter(&abufferSrcCtx, abuffersrc, "in",
                                         srcArgs.toUtf8().constData(), nullptr, audioGraph) < 0 ||
            avfilter_graph_create_filter(&abufferSinkCtx, abuffersink, "out",
                                         nullptr, nullptr, audioGraph) < 0) {
            avfilter_graph_free(&audioGraph);
            hasAudioFilter = false;
        } else {
            AVFilterInOut *outputs = avfilter_inout_alloc();
            AVFilterInOut *inputs = avfilter_inout_alloc();
            outputs->name = av_strdup("in");
            outputs->filter_ctx = abufferSrcCtx;
            outputs->pad_idx = 0;
            outputs->next = nullptr;
            inputs->name = av_strdup("out");
            inputs->filter_ctx = abufferSinkCtx;
            inputs->pad_idx = 0;
            inputs->next = nullptr;

            if (avfilter_graph_parse_ptr(audioGraph, audioFilter.toUtf8().constData(),
                                         &inputs, &outputs, nullptr) < 0 ||
                avfilter_graph_config(audioGraph, nullptr) < 0) {
                avfilter_graph_free(&audioGraph);
                hasAudioFilter = false;
            }
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
        }
    }

    // --- Build video filter graph ---
    AVFilterGraph *videoGraph = nullptr;
    AVFilterContext *vbufferSrcCtx = nullptr;
    AVFilterContext *vbufferSinkCtx = nullptr;

    if (hasVideoFilter) {
        videoGraph = avfilter_graph_alloc();
        const AVFilter *buffersrc = avfilter_get_by_name("buffer");
        const AVFilter *buffersink = avfilter_get_by_name("buffersink");

        QString srcArgs = QString("video_size=%1x%2:pix_fmt=%3:time_base=%4/%5:pixel_aspect=%6/%7")
                              .arg(videoDecCtx->width)
                              .arg(videoDecCtx->height)
                              .arg(videoDecCtx->pix_fmt)
                              .arg(inFmtCtx->streams[videoIdx]->time_base.num)
                              .arg(inFmtCtx->streams[videoIdx]->time_base.den)
                              .arg(videoDecCtx->sample_aspect_ratio.num)
                              .arg(qMax(videoDecCtx->sample_aspect_ratio.den, 1));

        if (avfilter_graph_create_filter(&vbufferSrcCtx, buffersrc, "in",
                                         srcArgs.toUtf8().constData(), nullptr, videoGraph) < 0 ||
            avfilter_graph_create_filter(&vbufferSinkCtx, buffersink, "out",
                                         nullptr, nullptr, videoGraph) < 0) {
            avfilter_graph_free(&videoGraph);
            hasVideoFilter = false;
        } else {
            AVFilterInOut *outputs = avfilter_inout_alloc();
            AVFilterInOut *inputs = avfilter_inout_alloc();
            outputs->name = av_strdup("in");
            outputs->filter_ctx = vbufferSrcCtx;
            outputs->pad_idx = 0;
            outputs->next = nullptr;
            inputs->name = av_strdup("out");
            inputs->filter_ctx = vbufferSinkCtx;
            inputs->pad_idx = 0;
            inputs->next = nullptr;

            if (avfilter_graph_parse_ptr(videoGraph, videoFilter.toUtf8().constData(),
                                         &inputs, &outputs, nullptr) < 0 ||
                avfilter_graph_config(videoGraph, nullptr) < 0) {
                avfilter_graph_free(&videoGraph);
                hasVideoFilter = false;
            }
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
        }
    }

    if (!hasAudioFilter && !hasVideoFilter) {
        if (audioDecCtx) avcodec_free_context(&audioDecCtx);
        if (videoDecCtx) avcodec_free_context(&videoDecCtx);
        avformat_close_input(&inFmtCtx);
        postComplete(receiver, false, "Failed to build filter graphs");
        return false;
    }

    // --- Open output ---
    AVFormatContext *outFmtCtx = nullptr;
    if (avformat_alloc_output_context2(&outFmtCtx, nullptr, nullptr,
                                       outputPath.toUtf8().constData()) < 0) {
        if (audioGraph) avfilter_graph_free(&audioGraph);
        if (videoGraph) avfilter_graph_free(&videoGraph);
        if (audioDecCtx) avcodec_free_context(&audioDecCtx);
        if (videoDecCtx) avcodec_free_context(&videoDecCtx);
        avformat_close_input(&inFmtCtx);
        postComplete(receiver, false, "Failed to create output context");
        return false;
    }

    // Create output streams and encoders
    AVCodecContext *audioEncCtx = nullptr;
    AVStream *outAudioStream = nullptr;
    AVCodecContext *videoEncCtx = nullptr;
    AVStream *outVideoStream = nullptr;

    auto cleanupOutputSetup = [&]() {
        if (audioEncCtx) avcodec_free_context(&audioEncCtx);
        if (videoEncCtx) avcodec_free_context(&videoEncCtx);
        if (audioGraph) avfilter_graph_free(&audioGraph);
        if (videoGraph) avfilter_graph_free(&videoGraph);
        if (audioDecCtx) avcodec_free_context(&audioDecCtx);
        if (videoDecCtx) avcodec_free_context(&videoDecCtx);
        if (outFmtCtx) {
            if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE) && outFmtCtx->pb)
                avio_closep(&outFmtCtx->pb);
            avformat_free_context(outFmtCtx);
        }
        avformat_close_input(&inFmtCtx);
    };

    if (hasAudioFilter) {
        const AVCodec *enc = avcodec_find_encoder(inFmtCtx->streams[audioIdx]->codecpar->codec_id);
        if (!enc) enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (enc) {
            outAudioStream = avformat_new_stream(outFmtCtx, nullptr);
            audioEncCtx = avcodec_alloc_context3(enc);
            if (!outAudioStream || !audioEncCtx) {
                cleanupOutputSetup();
                postComplete(receiver, false, "Failed to create audio encoder");
                return false;
            }
            audioEncCtx->sample_rate = audioDecCtx->sample_rate;
            av_channel_layout_copy(&audioEncCtx->ch_layout, &audioDecCtx->ch_layout);
            audioEncCtx->sample_fmt = enc->sample_fmts ? enc->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
            audioEncCtx->bit_rate = 192000;
            audioEncCtx->time_base = {1, audioDecCtx->sample_rate};
            if (outFmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
                audioEncCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(audioEncCtx, enc, nullptr) < 0) {
                cleanupOutputSetup();
                postComplete(receiver, false, "Failed to open audio encoder");
                return false;
            }
            if (audioEncCtx->frame_size > 0 &&
                !(audioEncCtx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) {
                av_buffersink_set_frame_size(abufferSinkCtx, audioEncCtx->frame_size);
            }
            avcodec_parameters_from_context(outAudioStream->codecpar, audioEncCtx);
            outAudioStream->time_base = audioEncCtx->time_base;
        } else {
            cleanupOutputSetup();
            postComplete(receiver, false, "Failed to find audio encoder");
            return false;
        }
    }

    if (hasVideoFilter) {
        const AVCodec *enc = avcodec_find_encoder(inFmtCtx->streams[videoIdx]->codecpar->codec_id);
        if (!enc) enc = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (enc) {
            outVideoStream = avformat_new_stream(outFmtCtx, nullptr);
            videoEncCtx = avcodec_alloc_context3(enc);
            if (!outVideoStream || !videoEncCtx) {
                cleanupOutputSetup();
                postComplete(receiver, false, "Failed to create video encoder");
                return false;
            }
            videoEncCtx->width = videoDecCtx->width;
            videoEncCtx->height = videoDecCtx->height;
            videoEncCtx->pix_fmt = videoDecCtx->pix_fmt;
            // Verify encoder supports this pix_fmt, fallback to YUV420P
            if (enc->pix_fmts) {
                bool supported = false;
                for (const auto *p = enc->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
                    if (*p == videoEncCtx->pix_fmt) { supported = true; break; }
                }
                if (!supported) videoEncCtx->pix_fmt = enc->pix_fmts[0];
            }
            videoEncCtx->time_base = inFmtCtx->streams[videoIdx]->time_base;
            videoEncCtx->framerate = av_guess_frame_rate(inFmtCtx, inFmtCtx->streams[videoIdx], nullptr);
            videoEncCtx->bit_rate = inFmtCtx->streams[videoIdx]->codecpar->bit_rate;
            if (videoEncCtx->bit_rate <= 0) videoEncCtx->bit_rate = 4000000;
            if (outFmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
                videoEncCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            av_opt_set(videoEncCtx->priv_data, "preset", "medium", 0);
            if (avcodec_open2(videoEncCtx, enc, nullptr) < 0) {
                cleanupOutputSetup();
                postComplete(receiver, false, "Failed to open video encoder");
                return false;
            }
            avcodec_parameters_from_context(outVideoStream->codecpar, videoEncCtx);
            outVideoStream->time_base = videoEncCtx->time_base;
        } else {
            cleanupOutputSetup();
            postComplete(receiver, false, "Failed to find video encoder");
            return false;
        }
    }

    // Copy streams that are not being filtered (pass-through)
    // Map: input stream index -> output stream for pass-through
    QVector<int> streamMapping(inFmtCtx->nb_streams, -1);
    for (unsigned i = 0; i < inFmtCtx->nb_streams; i++) {
        if (static_cast<int>(i) == audioIdx && hasAudioFilter && outAudioStream)
            streamMapping[i] = outAudioStream->index;
        else if (static_cast<int>(i) == videoIdx && hasVideoFilter && outVideoStream)
            streamMapping[i] = outVideoStream->index;
        else {
            // Pass-through: copy non-filtered streams (e.g., subtitles, data)
            AVStream *outStream = avformat_new_stream(outFmtCtx, nullptr);
            avcodec_parameters_copy(outStream->codecpar, inFmtCtx->streams[i]->codecpar);
            outStream->time_base = inFmtCtx->streams[i]->time_base;
            streamMapping[i] = outStream->index;
        }
    }

    if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outFmtCtx->pb, outputPath.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
            // Cleanup everything
            if (audioEncCtx) avcodec_free_context(&audioEncCtx);
            if (videoEncCtx) avcodec_free_context(&videoEncCtx);
            if (audioGraph) avfilter_graph_free(&audioGraph);
            if (videoGraph) avfilter_graph_free(&videoGraph);
            if (audioDecCtx) avcodec_free_context(&audioDecCtx);
            if (videoDecCtx) avcodec_free_context(&videoDecCtx);
            avformat_free_context(outFmtCtx);
            avformat_close_input(&inFmtCtx);
            postComplete(receiver, false, "Failed to open output file");
            return false;
        }
    }

    if (avformat_write_header(outFmtCtx, nullptr) < 0) {
        cleanupOutputSetup();
        postComplete(receiver, false, "Failed to write output header");
        return false;
    }

    // --- Processing loop ---
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *filtFrame = av_frame_alloc();
    if (!packet || !frame || !filtFrame) {
        av_frame_free(&filtFrame);
        av_frame_free(&frame);
        av_packet_free(&packet);
        cleanupOutputSetup();
        postComplete(receiver, false, "Failed to allocate processing frames");
        return false;
    }

    bool processingError = false;
    QString processingErrorMessage;
    auto failProcessing = [&](const QString &message) {
        if (!processingError) {
            processingError = true;
            processingErrorMessage = message;
        }
    };

    auto drainEncoderPackets = [&](AVCodecContext *encCtx, AVStream *outStream) {
        AVPacket *encPkt = av_packet_alloc();
        if (!encPkt) {
            failProcessing("Failed to allocate encoded packet");
            return false;
        }
        bool ok = true;
        while (!processingError) {
            int ret = avcodec_receive_packet(encCtx, encPkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                failProcessing("Failed to receive encoded packet");
                ok = false;
                break;
            }
            encPkt->stream_index = outStream->index;
            av_packet_rescale_ts(encPkt, encCtx->time_base, outStream->time_base);
            if (av_interleaved_write_frame(outFmtCtx, encPkt) < 0) {
                failProcessing("Failed to write encoded packet");
                ok = false;
                break;
            }
        }
        av_packet_free(&encPkt);
        return ok;
    };

    auto encodeFrame = [&](AVCodecContext *encCtx, AVStream *outStream, AVFrame *srcFrame,
                           const QString &streamName) {
        int ret = avcodec_send_frame(encCtx, srcFrame);
        if (ret < 0) {
            failProcessing(QString("Failed to encode %1 frame").arg(streamName));
            return false;
        }
        return drainEncoderPackets(encCtx, outStream);
    };

    int64_t totalDurationUs = inFmtCtx->duration > 0 ? inFmtCtx->duration : 1;
    int lastProgress = 0;

    while (av_read_frame(inFmtCtx, packet) >= 0 && !cancelled->load() && !processingError) {
        int pktIdx = packet->stream_index;

        // Progress based on packet DTS
        if (packet->dts != AV_NOPTS_VALUE) {
            double timeSec = packet->dts * av_q2d(inFmtCtx->streams[pktIdx]->time_base);
            int progress = static_cast<int>((timeSec * AV_TIME_BASE * 100) / totalDurationUs);
            progress = qBound(0, progress, 99);
            if (progress > lastProgress) {
                lastProgress = progress;
                postProgress(receiver, progress);
            }
        }

        // Audio filtering
        if (pktIdx == audioIdx && hasAudioFilter && audioDecCtx && audioEncCtx) {
            int ret = avcodec_send_packet(audioDecCtx, packet);
            if (ret < 0)
                failProcessing("Failed to decode audio packet");
            while (!processingError && avcodec_receive_frame(audioDecCtx, frame) == 0) {
                if (av_buffersrc_add_frame(abufferSrcCtx, frame) < 0) {
                    failProcessing("Failed to feed audio filter");
                } else {
                    while (!processingError && av_buffersink_get_frame(abufferSinkCtx, filtFrame) >= 0) {
                        filtFrame->pts = filtFrame->pts;
                        encodeFrame(audioEncCtx, outAudioStream, filtFrame, "audio");
                        av_frame_unref(filtFrame);
                    }
                }
                av_frame_unref(frame);
            }
        }
        // Video filtering
        else if (pktIdx == videoIdx && hasVideoFilter && videoDecCtx && videoEncCtx) {
            int ret = avcodec_send_packet(videoDecCtx, packet);
            if (ret < 0)
                failProcessing("Failed to decode video packet");
            while (!processingError && avcodec_receive_frame(videoDecCtx, frame) == 0) {
                if (av_buffersrc_add_frame(vbufferSrcCtx, frame) < 0) {
                    failProcessing("Failed to feed video filter");
                } else {
                    while (!processingError && av_buffersink_get_frame(vbufferSinkCtx, filtFrame) >= 0) {
                        filtFrame->pts = filtFrame->pts;
                        encodeFrame(videoEncCtx, outVideoStream, filtFrame, "video");
                        av_frame_unref(filtFrame);
                    }
                }
                av_frame_unref(frame);
            }
        }
        // Pass-through for non-filtered streams
        else if (pktIdx >= 0 && pktIdx < streamMapping.size() && streamMapping[pktIdx] >= 0) {
            packet->stream_index = streamMapping[pktIdx];
            if (av_interleaved_write_frame(outFmtCtx, packet) < 0)
                failProcessing("Failed to write copied packet");
        }

        av_packet_unref(packet);
    }

    // Flush decoders and filter graphs
    if (!processingError && hasAudioFilter && audioDecCtx && audioEncCtx) {
        if (avcodec_send_packet(audioDecCtx, nullptr) < 0) {
            failProcessing("Failed to flush audio decoder");
        }
        while (!processingError && avcodec_receive_frame(audioDecCtx, frame) == 0) {
            if (av_buffersrc_add_frame(abufferSrcCtx, frame) < 0)
                failProcessing("Failed to flush audio filter");
            av_frame_unref(frame);
        }
        if (!processingError && av_buffersrc_add_frame(abufferSrcCtx, nullptr) < 0)
            failProcessing("Failed to finish audio filter");
        while (!processingError && av_buffersink_get_frame(abufferSinkCtx, filtFrame) >= 0) {
            encodeFrame(audioEncCtx, outAudioStream, filtFrame, "audio");
            av_frame_unref(filtFrame);
        }
        if (!processingError)
            encodeFrame(audioEncCtx, outAudioStream, nullptr, "audio");
    }

    if (!processingError && hasVideoFilter && videoDecCtx && videoEncCtx) {
        if (avcodec_send_packet(videoDecCtx, nullptr) < 0) {
            failProcessing("Failed to flush video decoder");
        }
        while (!processingError && avcodec_receive_frame(videoDecCtx, frame) == 0) {
            if (av_buffersrc_add_frame(vbufferSrcCtx, frame) < 0)
                failProcessing("Failed to flush video filter");
            av_frame_unref(frame);
        }
        if (!processingError && av_buffersrc_add_frame(vbufferSrcCtx, nullptr) < 0)
            failProcessing("Failed to finish video filter");
        while (!processingError && av_buffersink_get_frame(vbufferSinkCtx, filtFrame) >= 0) {
            encodeFrame(videoEncCtx, outVideoStream, filtFrame, "video");
            av_frame_unref(filtFrame);
        }
        if (!processingError)
            encodeFrame(videoEncCtx, outVideoStream, nullptr, "video");
    }

    if (!processingError && av_write_trailer(outFmtCtx) < 0)
        failProcessing("Failed to write output trailer");

    // --- Cleanup ---
    av_frame_free(&filtFrame);
    av_frame_free(&frame);
    av_packet_free(&packet);

    if (audioGraph) avfilter_graph_free(&audioGraph);
    if (videoGraph) avfilter_graph_free(&videoGraph);
    if (audioEncCtx) avcodec_free_context(&audioEncCtx);
    if (videoEncCtx) avcodec_free_context(&videoEncCtx);
    if (audioDecCtx) avcodec_free_context(&audioDecCtx);
    if (videoDecCtx) avcodec_free_context(&videoDecCtx);

    if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&outFmtCtx->pb);
    avformat_free_context(outFmtCtx);
    avformat_close_input(&inFmtCtx);

    if (cancelled->load()) {
        postProgress(receiver, 0);
        postComplete(receiver, false, "Cancelled");
        return false;
    }

    if (processingError) {
        postComplete(receiver, false, processingErrorMessage);
        return false;
    }

    postProgress(receiver, 100);
    postComplete(receiver, true, "Denoise complete");
    return true;
}

bool NoiseReduction::processWithFilter(const QString &inputPath, const QString &outputPath,
                                       const QString &audioFilter, const QString &videoFilter)
{
    m_cancelled = false;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    return processWithFilterImpl(inputPath, outputPath, audioFilter, videoFilter,
                                 cancelled, QPointer<NoiseReduction>(this));
}

// --- Public API ---

void NoiseReduction::denoiseAudio(const QString &inputPath, const QString &outputPath,
                                  const AudioDenoiseConfig &config)
{
    m_cancelled = false;
    if (!startDenoiseWorker(this, m_thread, inputPath, outputPath,
                            buildAudioFilterDesc(config), QString(), processWithFilterImpl)) {
        emit denoiseComplete(false, "Denoise already in progress");
    }
}

void NoiseReduction::denoiseVideo(const QString &inputPath, const QString &outputPath,
                                  const VideoDenoiseConfig &config)
{
    m_cancelled = false;
    if (!startDenoiseWorker(this, m_thread, inputPath, outputPath,
                            QString(), buildVideoFilterDesc(config), processWithFilterImpl)) {
        emit denoiseComplete(false, "Denoise already in progress");
    }
}

void NoiseReduction::denoiseAll(const QString &inputPath, const QString &outputPath,
                                const AudioDenoiseConfig &audioConfig,
                                const VideoDenoiseConfig &videoConfig)
{
    m_cancelled = false;
    if (!startDenoiseWorker(this, m_thread, inputPath, outputPath,
                            buildAudioFilterDesc(audioConfig),
                            buildVideoFilterDesc(videoConfig), processWithFilterImpl)) {
        emit denoiseComplete(false, "Denoise already in progress");
    }
}

// --- Noise level measurement ---

double NoiseReduction::measureNoiseLevel(const QString &filePath, double startSec, double durationSec)
{
    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return 0.0;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return 0.0;
    }

    int audioIdx = findStreamIndex(fmtCtx, AVMEDIA_TYPE_AUDIO);
    if (audioIdx < 0) {
        avformat_close_input(&fmtCtx);
        return 0.0;
    }

    auto *codecpar = fmtCtx->streams[audioIdx]->codecpar;
    const AVCodec *dec = avcodec_find_decoder(codecpar->codec_id);
    if (!dec) { avformat_close_input(&fmtCtx); return 0.0; }

    AVCodecContext *decCtx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(decCtx, codecpar);
    if (avcodec_open2(decCtx, dec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return 0.0;
    }

    // Seek to start position
    int64_t seekTarget = static_cast<int64_t>(startSec * AV_TIME_BASE);
    av_seek_frame(fmtCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(decCtx);

    double endSec = startSec + durationSec;
    double sumSquares = 0.0;
    int64_t sampleCount = 0;

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != audioIdx) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (avcodec_receive_frame(decCtx, frame) == 0) {
                double framePts = frame->pts * av_q2d(fmtCtx->streams[audioIdx]->time_base);
                if (framePts > endSec) break;
                if (framePts < startSec) { av_frame_unref(frame); continue; }

                // Measure RMS from first channel (float planar or s16)
                for (int s = 0; s < frame->nb_samples; ++s) {
                    double sample = 0.0;
                    if (decCtx->sample_fmt == AV_SAMPLE_FMT_FLTP ||
                        decCtx->sample_fmt == AV_SAMPLE_FMT_FLT) {
                        sample = reinterpret_cast<float *>(frame->data[0])[s];
                    } else if (decCtx->sample_fmt == AV_SAMPLE_FMT_S16P ||
                               decCtx->sample_fmt == AV_SAMPLE_FMT_S16) {
                        sample = reinterpret_cast<int16_t *>(frame->data[0])[s] / 32768.0;
                    } else if (decCtx->sample_fmt == AV_SAMPLE_FMT_S32P ||
                               decCtx->sample_fmt == AV_SAMPLE_FMT_S32) {
                        sample = reinterpret_cast<int32_t *>(frame->data[0])[s] / 2147483648.0;
                    }
                    sumSquares += sample * sample;
                    sampleCount++;
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);

        // Check if we've passed the end time
        double pktTime = packet->dts != AV_NOPTS_VALUE
                             ? packet->dts * av_q2d(fmtCtx->streams[audioIdx]->time_base)
                             : 0.0;
        if (pktTime > endSec) break;
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    if (sampleCount == 0) return -96.0; // silence floor

    double rms = std::sqrt(sumSquares / sampleCount);
    if (rms < 1e-10) return -96.0;
    return 20.0 * std::log10(rms);
}

// --- Preview ---

AudioDenoisePreview NoiseReduction::previewAudioDenoise(const QString &inputPath,
                                                        const AudioDenoiseConfig &config)
{
    AudioDenoisePreview result;

    // Measure original noise level from the first 2 seconds
    double originalLevel = measureNoiseLevel(inputPath, 0.0, 2.0);

    // Create a temporary filtered output to measure denoised level
    QString tempPath = QFileInfo(inputPath).absolutePath() + "/.nr_preview_temp.wav";

    // Build a minimal filter graph for the preview segment
    AVFormatContext *inFmtCtx = nullptr;
    if (avformat_open_input(&inFmtCtx, inputPath.toUtf8().constData(), nullptr, nullptr) < 0)
        return result;
    if (avformat_find_stream_info(inFmtCtx, nullptr) < 0) {
        avformat_close_input(&inFmtCtx);
        return result;
    }

    int audioIdx = findStreamIndex(inFmtCtx, AVMEDIA_TYPE_AUDIO);
    if (audioIdx < 0) {
        avformat_close_input(&inFmtCtx);
        return result;
    }

    auto *codecpar = inFmtCtx->streams[audioIdx]->codecpar;
    const AVCodec *dec = avcodec_find_decoder(codecpar->codec_id);
    if (!dec) { avformat_close_input(&inFmtCtx); return result; }

    AVCodecContext *decCtx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(decCtx, codecpar);
    if (avcodec_open2(decCtx, dec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&inFmtCtx);
        return result;
    }

    // Build audio filter graph
    AVFilterGraph *graph = avfilter_graph_alloc();
    const AVFilter *abuffersrc = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    char chLayoutStr[64] = {};
    av_channel_layout_describe(&decCtx->ch_layout, chLayoutStr, sizeof(chLayoutStr));

    QString srcArgs = QString("time_base=%1/%2:sample_rate=%3:sample_fmt=%4:channel_layout=%5")
                          .arg(inFmtCtx->streams[audioIdx]->time_base.num)
                          .arg(inFmtCtx->streams[audioIdx]->time_base.den)
                          .arg(decCtx->sample_rate)
                          .arg(av_get_sample_fmt_name(decCtx->sample_fmt))
                          .arg(chLayoutStr);

    AVFilterContext *srcCtx = nullptr;
    AVFilterContext *sinkCtx = nullptr;

    bool graphOk = avfilter_graph_create_filter(&srcCtx, abuffersrc, "in",
                                                srcArgs.toUtf8().constData(), nullptr, graph) >= 0 &&
                   avfilter_graph_create_filter(&sinkCtx, abuffersink, "out",
                                                nullptr, nullptr, graph) >= 0;
    if (graphOk) {
        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = srcCtx;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sinkCtx;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        QString filterDesc = buildAudioFilterDesc(config);
        graphOk = avfilter_graph_parse_ptr(graph, filterDesc.toUtf8().constData(),
                                           &inputs, &outputs, nullptr) >= 0 &&
                  avfilter_graph_config(graph, nullptr) >= 0;
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
    }

    if (!graphOk) {
        avfilter_graph_free(&graph);
        avcodec_free_context(&decCtx);
        avformat_close_input(&inFmtCtx);
        return result;
    }

    // Process first 2 seconds through filter and measure output RMS
    double sumSquares = 0.0;
    int64_t sampleCount = 0;
    double previewDuration = 2.0;

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *filtFrame = av_frame_alloc();
    bool done = false;

    while (av_read_frame(inFmtCtx, packet) >= 0 && !done) {
        if (packet->stream_index != audioIdx) {
            av_packet_unref(packet);
            continue;
        }

        double pktTime = packet->dts != AV_NOPTS_VALUE
                             ? packet->dts * av_q2d(inFmtCtx->streams[audioIdx]->time_base)
                             : 0.0;
        if (pktTime > previewDuration) { av_packet_unref(packet); break; }

        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (avcodec_receive_frame(decCtx, frame) == 0) {
                if (av_buffersrc_add_frame(srcCtx, frame) >= 0) {
                    while (av_buffersink_get_frame(sinkCtx, filtFrame) >= 0) {
                        // Measure filtered output
                        for (int s = 0; s < filtFrame->nb_samples; ++s) {
                            double sample = 0.0;
                            auto fmt = static_cast<AVSampleFormat>(filtFrame->format);
                            if (fmt == AV_SAMPLE_FMT_FLTP || fmt == AV_SAMPLE_FMT_FLT)
                                sample = reinterpret_cast<float *>(filtFrame->data[0])[s];
                            else if (fmt == AV_SAMPLE_FMT_S16P || fmt == AV_SAMPLE_FMT_S16)
                                sample = reinterpret_cast<int16_t *>(filtFrame->data[0])[s] / 32768.0;
                            else if (fmt == AV_SAMPLE_FMT_S32P || fmt == AV_SAMPLE_FMT_S32)
                                sample = reinterpret_cast<int32_t *>(filtFrame->data[0])[s] / 2147483648.0;
                            sumSquares += sample * sample;
                            sampleCount++;
                        }
                        av_frame_unref(filtFrame);
                    }
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&filtFrame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avfilter_graph_free(&graph);
    avcodec_free_context(&decCtx);
    avformat_close_input(&inFmtCtx);

    double denoisedLevel = -96.0;
    if (sampleCount > 0) {
        double rms = std::sqrt(sumSquares / sampleCount);
        if (rms > 1e-10)
            denoisedLevel = 20.0 * std::log10(rms);
    }

    result.originalNoiseLevel = originalLevel;
    result.denoisedNoiseLevel = denoisedLevel;
    result.reductionDb = originalLevel - denoisedLevel;
    result.success = true;
    return result;
}
