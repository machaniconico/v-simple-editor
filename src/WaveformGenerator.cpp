#include "WaveformGenerator.h"
#include "libavcore/AudioExtract.h"
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QTemporaryFile>
#include <QThread>
#include <QDebug>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

WaveformGenerator::WaveformGenerator(QObject *parent)
    : QObject(parent) {}

void WaveformGenerator::generateAsync(const QString &filePath, int peaksPerSecond)
{
    QPointer<WaveformGenerator> self(this);
    auto *thread = QThread::create([self, filePath, peaksPerSecond]() {
        WaveformData data = generate(filePath, peaksPerSecond);
        if (!self)
            return;

        QMetaObject::invokeMethod(self.data(), [self, filePath, data]() {
            if (!self)
                return;
            emit self->waveformReady(filePath, data);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// -----------------------------------------------------------------------------
// Streaming waveform generation.
//
// Previous implementation decoded the entire audio stream into a QVector<float>
// and then built peaks in a second pass. For a 4-hour 48 kHz mono float stream
// that is ~3 GB of RAM (plus QVector reallocs), which was causing out-of-memory
// crashes on long clips.
//
// The new implementation:
//   1. Estimates duration from the container (avformat).
//   2. Computes the total number of peaks up-front (duration * peaksPerSecond).
//   3. Pre-allocates a tiny QVector<float>(totalPeaks) and fills it while
//      streaming packets through the decoder + resampler.
//   4. Never holds more than one AVFrame worth of PCM at a time.
//
// Memory usage drops from ~O(sampleRate * duration) bytes to O(duration * pps)
// floats — roughly 4 MB for a 4-hour clip at 50 pps.
// -----------------------------------------------------------------------------

WaveformData WaveformGenerator::generate(const QString &filePath, int peaksPerSecond)
{
    WaveformData data;
    if (peaksPerSecond <= 0)
        peaksPerSecond = 50;

    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return data;

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return data;
    }

    int audioIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIdx = static_cast<int>(i);
            break;
        }
    }
    if (audioIdx < 0) {
        avformat_close_input(&fmtCtx);
        return data;
    }

    AVStream *audioStream = fmtCtx->streams[audioIdx];
    auto *codecpar = audioStream->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        return data;
    }

    AVCodecContext *decCtx = avcodec_alloc_context3(codec);
    if (!decCtx) {
        avformat_close_input(&fmtCtx);
        return data;
    }
    avcodec_parameters_to_context(decCtx, codecpar);
    if (avcodec_open2(decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return data;
    }

    SwrContext *swrCtx = nullptr;
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;
    swr_alloc_set_opts2(&swrCtx,
        &outLayout, AV_SAMPLE_FMT_FLT, decCtx->sample_rate,
        &decCtx->ch_layout, decCtx->sample_fmt, decCtx->sample_rate,
        0, nullptr);

    if (!swrCtx || swr_init(swrCtx) < 0) {
        if (swrCtx) swr_free(&swrCtx);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return data;
    }

    const int sampleRate = decCtx->sample_rate;

    // Determine duration. Prefer container duration, then stream duration.
    double durationSec = 0.0;
    if (fmtCtx->duration > 0) {
        durationSec = static_cast<double>(fmtCtx->duration) / AV_TIME_BASE;
    } else if (audioStream->duration > 0) {
        durationSec = audioStream->duration * av_q2d(audioStream->time_base);
    }
    if (durationSec <= 0.0) {
        // Unknown duration — cap at a reasonable ceiling so we bound memory.
        durationSec = 600.0;
    }

    // Pre-compute peak bucket sizing.
    const qint64 totalPeaks = qMax<qint64>(
        1, static_cast<qint64>(durationSec * peaksPerSecond));
    const qint64 samplesPerPeak = qMax<qint64>(
        1, static_cast<qint64>(sampleRate / peaksPerSecond));

    // Hard cap so a pathological duration never explodes memory.
    constexpr qint64 kMaxPeaks = 2'000'000; // ~8 MB of floats
    const qint64 capPeaks = qMin(totalPeaks, kMaxPeaks);

    data.sampleRate = sampleRate;
    data.duration = durationSec;
    data.peaksPerSecond = peaksPerSecond;
    data.peaks.resize(static_cast<int>(capPeaks));
    for (int i = 0; i < data.peaks.size(); ++i)
        data.peaks[i] = 0.0f;

    // Streaming state
    qint64 totalSamplesSeen = 0;
    qint64 samplesInCurrentBucket = 0;
    float  currentBucketMax = 0.0f;
    qint64 currentBucketIdx = 0;

    // Reusable output buffer for one frame worth of resampled samples.
    QVector<float> outBuf;

    AVPacket *packet = av_packet_alloc();
    AVFrame  *frame  = av_frame_alloc();
    if (!packet || !frame) {
        if (packet) av_packet_free(&packet);
        if (frame)  av_frame_free(&frame);
        swr_free(&swrCtx);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return data;
    }

    auto commitSamples = [&](const float *src, int n) {
        for (int i = 0; i < n; ++i) {
            const float amp = std::abs(src[i]);
            if (amp > currentBucketMax) currentBucketMax = amp;
            ++samplesInCurrentBucket;
            ++totalSamplesSeen;

            if (samplesInCurrentBucket >= samplesPerPeak) {
                if (currentBucketIdx < capPeaks) {
                    data.peaks[static_cast<int>(currentBucketIdx)] =
                        qMin(currentBucketMax, 1.0f);
                }
                ++currentBucketIdx;
                samplesInCurrentBucket = 0;
                currentBucketMax = 0.0f;

                if (currentBucketIdx >= capPeaks) {
                    // Stop early — we have all the peaks we need.
                    return false;
                }
            }
        }
        return true;
    };

    bool keepGoing = true;
    while (keepGoing && av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != audioIdx) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (avcodec_receive_frame(decCtx, frame) == 0) {
                int outSamples = swr_get_out_samples(swrCtx, frame->nb_samples);
                if (outSamples <= 0) continue;

                if (outBuf.size() < outSamples)
                    outBuf.resize(outSamples);
                uint8_t *outPtr = reinterpret_cast<uint8_t*>(outBuf.data());
                int converted = swr_convert(swrCtx, &outPtr, outSamples,
                    const_cast<const uint8_t**>(frame->extended_data),
                    frame->nb_samples);
                if (converted > 0) {
                    if (!commitSamples(outBuf.constData(), converted)) {
                        keepGoing = false;
                        break;
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    if (keepGoing) {
        // Flush remaining buffered frames.
        avcodec_send_packet(decCtx, nullptr);
        while (avcodec_receive_frame(decCtx, frame) == 0) {
            int outSamples = swr_get_out_samples(swrCtx, frame->nb_samples);
            if (outSamples <= 0) continue;
            if (outBuf.size() < outSamples)
                outBuf.resize(outSamples);
            uint8_t *outPtr = reinterpret_cast<uint8_t*>(outBuf.data());
            int converted = swr_convert(swrCtx, &outPtr, outSamples,
                const_cast<const uint8_t**>(frame->extended_data),
                frame->nb_samples);
            if (converted > 0) {
                if (!commitSamples(outBuf.constData(), converted))
                    break;
            }
        }
    }

    // Write the last partial bucket if we have one.
    if (samplesInCurrentBucket > 0 && currentBucketIdx < capPeaks) {
        data.peaks[static_cast<int>(currentBucketIdx)] =
            qMin(currentBucketMax, 1.0f);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swrCtx);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    qInfo() << "WaveformGenerator: produced" << data.peaks.size()
            << "peaks for duration" << durationSec << "s sr=" << sampleRate;
    return data;
}

// Legacy helpers kept as no-ops for API compatibility — unused by the
// streaming generate() above.
bool WaveformGenerator::decodeAudio(const QString &filePath, QVector<float> &samples, int &sampleRate)
{
    QTemporaryFile tempWav;
    tempWav.setAutoRemove(false);
    if (!tempWav.open())
        return false;

    const QString wavPath = tempWav.fileName();
    tempWav.close();

    QString err;
    if (!libavcore::extractAudioToWav(filePath, wavPath, 16000, &err)) {
        QFile::remove(wavPath);
        return false;
    }

    QFile wavFile(wavPath);
    if (!wavFile.open(QIODevice::ReadOnly)) {
        QFile::remove(wavPath);
        return false;
    }

    const QByteArray wavData = wavFile.readAll();
    wavFile.close();
    QFile::remove(wavPath);

    const int dataTagOffset = wavData.indexOf("data");
    if (dataTagOffset < 0)
        return false;
    if (dataTagOffset > wavData.size() - 8)
        return false;

    const int dataSizeOffset = dataTagOffset + 4;
    const quint32 declaredDataSize =
        static_cast<quint32>(static_cast<quint8>(wavData.at(dataSizeOffset)))
        | (static_cast<quint32>(static_cast<quint8>(wavData.at(dataSizeOffset + 1))) << 8)
        | (static_cast<quint32>(static_cast<quint8>(wavData.at(dataSizeOffset + 2))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(wavData.at(dataSizeOffset + 3))) << 24);

    const int dataOffset = dataTagOffset + 8;
    if (dataOffset > wavData.size())
        return false;

    const int availableBytes = wavData.size() - dataOffset;
    const int dataBytes = qMin<qint64>(declaredDataSize, availableBytes);
    samples.clear();
    samples.reserve(dataBytes / 2);
    for (int offset = 0; offset + 1 < dataBytes; offset += 2) {
        const int byteOffset = dataOffset + offset;
        const quint16 pcm =
            static_cast<quint16>(static_cast<quint8>(wavData.at(byteOffset)))
            | (static_cast<quint16>(static_cast<quint8>(wavData.at(byteOffset + 1))) << 8);
        samples.append(static_cast<qint16>(pcm) / 32768.0f);
    }

    sampleRate = 16000;
    return true;
}

WaveformData WaveformGenerator::buildPeaks(const QVector<float> &, int, double, int)
{
    return {};
}
