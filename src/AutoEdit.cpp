#include "AutoEdit.h"
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

AutoEdit::AutoEdit(QObject *parent) : QObject(parent) {}

QVector<SilenceRegion> AutoEdit::detectSilence(const WaveformData &waveform,
                                                const AutoEditConfig &config)
{
    QVector<SilenceRegion> silences;
    if (waveform.isEmpty()) return silences;

    double secondsPerPeak = 1.0 / waveform.peaksPerSecond;
    bool inSilence = false;
    double silenceStart = 0.0;

    for (int i = 0; i < waveform.peaks.size(); ++i) {
        double time = i * secondsPerPeak;
        bool isSilent = waveform.peaks[i] < config.silenceThreshold;

        if (isSilent && !inSilence) {
            inSilence = true;
            silenceStart = time;
        } else if (!isSilent && inSilence) {
            inSilence = false;
            double duration = time - silenceStart;
            if (duration >= config.minSilenceDuration) {
                SilenceRegion region;
                region.startTime = silenceStart;
                region.endTime = time;
                silences.append(region);
            }
        }
    }

    // Handle trailing silence
    if (inSilence) {
        double endTime = waveform.duration;
        double duration = endTime - silenceStart;
        if (duration >= config.minSilenceDuration) {
            SilenceRegion region;
            region.startTime = silenceStart;
            region.endTime = endTime;
            silences.append(region);
        }
    }

    return silences;
}

QVector<SilenceRegion> AutoEdit::detectSilenceFromFile(const QString &filePath,
                                                        const AutoEditConfig &config)
{
    WaveformData waveform = WaveformGenerator::generate(filePath, 100); // higher res for detection
    return detectSilence(waveform, config);
}

QVector<double> AutoEdit::generateJumpCuts(const QVector<SilenceRegion> &silences,
                                            double totalDuration,
                                            const AutoEditConfig &config)
{
    QVector<double> cuts;
    if (silences.isEmpty()) return cuts;

    for (const auto &silence : silences) {
        // Cut point at start of silence (with padding after speech)
        double cutStart = silence.startTime + config.paddingAfter;
        // Resume point at end of silence (with padding before speech)
        double cutEnd = silence.endTime - config.paddingBefore;

        if (cutEnd > cutStart + 0.1) { // minimum 0.1s gap to be worth cutting
            cuts.append(cutStart);
            cuts.append(cutEnd);
        }
    }

    return cuts;
}

QVector<SceneChange> AutoEdit::detectSceneChanges(const QString &filePath,
                                                    const AutoEditConfig &config)
{
    QVector<SceneChange> changes;

    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return changes;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return changes;
    }

    int videoIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        return changes;
    }

    auto *codecpar = fmtCtx->streams[videoIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmtCtx); return changes; }

    AVCodecContext *decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decCtx, codecpar);
    if (avcodec_open2(decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return changes;
    }

    // Scale to small size for fast comparison
    int cmpW = 160, cmpH = 90;
    SwsContext *swsCtx = sws_getContext(
        decCtx->width, decCtx->height, decCtx->pix_fmt,
        cmpW, cmpH, AV_PIX_FMT_GRAY8,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return changes;
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    QVector<uint8_t> prevFrame(cmpW * cmpH, 0);
    QVector<uint8_t> currFrame(cmpW * cmpH, 0);
    bool hasPrev = false;
    int frameCount = 0;
    AVStream *stream = fmtCtx->streams[videoIdx];

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != videoIdx) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (avcodec_receive_frame(decCtx, frame) == 0) {
                frameCount++;
                if (frameCount % config.sceneCheckInterval != 0) continue;

                // Scale to grayscale
                uint8_t *dest[1] = { currFrame.data() };
                int destLinesize[1] = { cmpW };
                sws_scale(swsCtx, frame->data, frame->linesize, 0,
                          frame->height, dest, destLinesize);

                if (hasPrev) {
                    // Calculate difference
                    double totalDiff = 0.0;
                    int pixelCount = cmpW * cmpH;
                    for (int i = 0; i < pixelCount; ++i)
                        totalDiff += std::abs(currFrame[i] - prevFrame[i]);
                    double avgDiff = totalDiff / (pixelCount * 255.0);

                    if (avgDiff > config.sceneChangeThreshold) {
                        double time = frame->pts * av_q2d(stream->time_base);
                        SceneChange sc;
                        sc.time = time;
                        sc.confidence = qMin(avgDiff / config.sceneChangeThreshold, 1.0);
                        changes.append(sc);
                    }
                }

                std::swap(prevFrame, currFrame);
                hasPrev = true;
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    if (swsCtx) sws_freeContext(swsCtx);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    return changes;
}
