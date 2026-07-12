#include "LoudnessMaster.h"

#include <QDebug>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace loudness {

// ---------------------------------------------------------------------------
// Gain / preset helpers
// ---------------------------------------------------------------------------
double computeGainDb(double measuredLufs, double targetLufs)
{
    // Positive when the program is quieter than target (needs boosting).
    return targetLufs - measuredLufs;
}

double presetTargetLufs(LoudnessPreset p)
{
    switch (p) {
    case LoudnessPreset::YouTube:    return -14.0;
    case LoudnessPreset::Spotify:    return -14.0;
    case LoudnessPreset::AppleMusic: return -16.0;
    case LoudnessPreset::Broadcast:  return -23.0;
    case LoudnessPreset::TikTok:     return -14.0;
    }
    return -14.0;
}

// ---------------------------------------------------------------------------
// ITU-R BS.1770-4 integrated loudness
// ---------------------------------------------------------------------------
namespace {

const double kMeasurementFailed = std::numeric_limits<double>::quiet_NaN();

// Direct-form-I biquad. ITU-R BS.1770-4 K-weighting (a0 normalized to 1).
struct Biquad {
    double b0, b1, b2, a1, a2;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    inline double process(double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

struct DecodeContext {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    SwrContext *swr = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int streamIndex = -1;

    ~DecodeContext()
    {
        if (frame) {
            av_frame_free(&frame);
        }
        if (packet) {
            av_packet_free(&packet);
        }
        if (swr) {
            swr_free(&swr);
        }
        if (codec) {
            avcodec_free_context(&codec);
        }
        if (fmt) {
            avformat_close_input(&fmt);
        }
    }
};

bool appendDecodedFrames(DecodeContext &ctx, QVector<float> &mono)
{
    QVector<float> converted;
    bool decodedAny = false;

    while (avcodec_receive_frame(ctx.codec, ctx.frame) == 0) {
        const int outSamples = swr_get_out_samples(ctx.swr, ctx.frame->nb_samples);
        if (outSamples <= 0) {
            av_frame_unref(ctx.frame);
            continue;
        }

        converted.resize(outSamples);
        uint8_t *outData = reinterpret_cast<uint8_t *>(converted.data());
        const int got = swr_convert(ctx.swr,
                                    &outData,
                                    outSamples,
                                    const_cast<const uint8_t **>(ctx.frame->extended_data),
                                    ctx.frame->nb_samples);
        av_frame_unref(ctx.frame);
        if (got <= 0) {
            continue;
        }

        const int oldSize = mono.size();
        mono.resize(oldSize + got);
        std::copy(converted.constData(), converted.constData() + got, mono.data() + oldSize);
        decodedAny = true;
    }

    return decodedAny;
}

bool decodeAudioFileToMono(const QString &audioPath, QVector<float> &mono, int &sampleRate)
{
    DecodeContext ctx;
    if (avformat_open_input(&ctx.fmt, audioPath.toUtf8().constData(), nullptr, nullptr) < 0) {
        return false;
    }
    if (avformat_find_stream_info(ctx.fmt, nullptr) < 0) {
        return false;
    }

    ctx.streamIndex = av_find_best_stream(ctx.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (ctx.streamIndex < 0) {
        return false;
    }

    AVCodecParameters *params = ctx.fmt->streams[ctx.streamIndex]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(params->codec_id);
    if (!decoder) {
        return false;
    }

    ctx.codec = avcodec_alloc_context3(decoder);
    if (!ctx.codec || avcodec_parameters_to_context(ctx.codec, params) < 0) {
        return false;
    }
    if (ctx.codec->ch_layout.nb_channels <= 0 && params->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&ctx.codec->ch_layout, &params->ch_layout);
    }
    if (avcodec_open2(ctx.codec, decoder, nullptr) < 0) {
        return false;
    }
    if (ctx.codec->sample_rate <= 0 || ctx.codec->ch_layout.nb_channels <= 0) {
        return false;
    }

    AVChannelLayout monoLayout = AV_CHANNEL_LAYOUT_MONO;
    if (swr_alloc_set_opts2(&ctx.swr,
                            &monoLayout,
                            AV_SAMPLE_FMT_FLT,
                            ctx.codec->sample_rate,
                            &ctx.codec->ch_layout,
                            ctx.codec->sample_fmt,
                            ctx.codec->sample_rate,
                            0,
                            nullptr) < 0) {
        return false;
    }
    if (!ctx.swr || swr_init(ctx.swr) < 0) {
        return false;
    }

    ctx.packet = av_packet_alloc();
    ctx.frame = av_frame_alloc();
    if (!ctx.packet || !ctx.frame) {
        return false;
    }

    bool decodedAny = false;
    while (av_read_frame(ctx.fmt, ctx.packet) >= 0) {
        if (ctx.packet->stream_index == ctx.streamIndex &&
            avcodec_send_packet(ctx.codec, ctx.packet) == 0) {
            decodedAny = appendDecodedFrames(ctx, mono) || decodedAny;
        }
        av_packet_unref(ctx.packet);
    }

    if (avcodec_send_packet(ctx.codec, nullptr) == 0) {
        decodedAny = appendDecodedFrames(ctx, mono) || decodedAny;
    }

    sampleRate = ctx.codec->sample_rate;
    return decodedAny && !mono.isEmpty();
}

} // namespace

double measureIntegratedLufsFromSamples(const QVector<float> &mono, int sampleRate)
{
    constexpr double kSilenceFloor   = -70.0;
    constexpr double kAbsoluteGate   = -70.0; // LUFS
    constexpr double kRelativeOffset = 10.0;  // LU

    const int n = mono.size();
    if (n == 0 || sampleRate <= 0)
        return kSilenceFloor;

    // Two-stage K-weighting (BS.1770-4) biquad coefficients @ 48 kHz.
    // Simplification: the 48 kHz coefficients are applied regardless of
    // sampleRate; non-48k input is filtered with the 48k constants.
    Biquad pre{ 1.53512485958697, -2.69169618940638, 1.19839281085285,
                -1.69065929318241, 0.73248077421585 };
    Biquad rlb{ 1.0, -2.0, 1.0,
                -1.99004745483398, 0.99007225036621 };

    // K-weighted signal.
    QVector<double> k(n);
    for (int i = 0; i < n; ++i) {
        const double s  = static_cast<double>(mono[i]);
        const double s1 = pre.process(s);
        k[i] = rlb.process(s1);
    }

    // 400 ms blocks, 75 % overlap (100 ms hop).
    const int blockLen = static_cast<int>(std::lround(0.400 * sampleRate));
    const int hopLen   = static_cast<int>(std::lround(0.100 * sampleRate));
    if (blockLen <= 0 || hopLen <= 0 || n < blockLen)
        return kSilenceFloor;

    QVector<double> blockMs;        // mean square per block
    QVector<double> blockLoudness;  // -0.691 + 10*log10(meanSquare)
    blockMs.reserve(n / hopLen + 1);
    blockLoudness.reserve(n / hopLen + 1);

    for (int start = 0; start + blockLen <= n; start += hopLen) {
        double sumSq = 0.0;
        for (int i = start; i < start + blockLen; ++i)
            sumSq += k[i] * k[i];

        const double ms = sumSq / static_cast<double>(blockLen);
        double l;
        if (ms > 0.0)
            l = -0.691 + 10.0 * std::log10(ms);
        else
            l = -std::numeric_limits<double>::infinity();

        blockMs.append(ms);
        blockLoudness.append(l);
    }

    if (blockMs.isEmpty())
        return kSilenceFloor;

    // Stage 1: absolute gate at -70 LUFS.
    QVector<double> absGatedMs;
    absGatedMs.reserve(blockMs.size());
    for (int i = 0; i < blockMs.size(); ++i) {
        if (blockLoudness[i] >= kAbsoluteGate && blockMs[i] > 0.0)
            absGatedMs.append(blockMs[i]);
    }

    if (absGatedMs.isEmpty())
        return kSilenceFloor;

    // Provisional integrated loudness from absolute-gated blocks.
    double meanMs = 0.0;
    for (double ms : absGatedMs)
        meanMs += ms;
    meanMs /= static_cast<double>(absGatedMs.size());

    const double provisional = -0.691 + 10.0 * std::log10(meanMs);

    // Stage 2: relative gate at (provisional - 10 LU).
    const double relThreshold = provisional - kRelativeOffset;

    double gatedSum   = 0.0;
    int    gatedCount = 0;
    for (int i = 0; i < blockMs.size(); ++i) {
        if (blockLoudness[i] >= kAbsoluteGate &&
            blockLoudness[i] >= relThreshold &&
            blockMs[i] > 0.0) {
            gatedSum += blockMs[i];
            ++gatedCount;
        }
    }

    if (gatedCount == 0)
        return kSilenceFloor;

    const double gatedMeanMs = gatedSum / static_cast<double>(gatedCount);
    if (gatedMeanMs <= 0.0)
        return kSilenceFloor;

    const double integrated = -0.691 + 10.0 * std::log10(gatedMeanMs);
    if (!std::isfinite(integrated) || integrated < kSilenceFloor)
        return kSilenceFloor;

    return integrated;
}

double measureIntegratedLufs(const QString &audioPath)
{
    // Raw PCM fast path: 32-bit float mono/interleaved is treated as mono.
    if (audioPath.endsWith(QStringLiteral(".raw"), Qt::CaseInsensitive) ||
        audioPath.endsWith(QStringLiteral(".pcm"), Qt::CaseInsensitive)) {
        QFile f(audioPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = f.readAll();
            f.close();

            const int floatCount = static_cast<int>(bytes.size() / sizeof(float));
            if (floatCount > 0) {
                QVector<float> mono(floatCount);
                std::memcpy(mono.data(), bytes.constData(),
                            static_cast<size_t>(floatCount) * sizeof(float));
                // Raw PCM carries no header; assume 48 kHz mono float.
                return measureIntegratedLufsFromSamples(mono, 48000);
            }
        }
        qWarning("loudness::measureIntegratedLufs: failed to read raw PCM '%s'",
                 qUtf8Printable(audioPath));
        return -23.0;
    }

    QVector<float> mono;
    int sampleRate = 0;
    if (!decodeAudioFileToMono(audioPath, mono, sampleRate)) {
        qWarning("loudness::measureIntegratedLufs: failed to decode '%s'",
                 qUtf8Printable(audioPath));
        return kMeasurementFailed;
    }

    return measureIntegratedLufsFromSamples(mono, sampleRate);
}

} // namespace loudness
