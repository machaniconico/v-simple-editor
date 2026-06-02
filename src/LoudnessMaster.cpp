#include "LoudnessMaster.h"

#include "libavcore/Decode.h"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
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

QString avErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, buf, sizeof(buf)) < 0)
        return QString::number(err);
    return QString::fromUtf8(buf);
}

struct SwrGuard {
    SwrContext *ctx = nullptr;

    ~SwrGuard()
    {
        if (ctx)
            swr_free(&ctx);
    }
};

struct FormatGuard {
    AVFormatContext *ctx = nullptr;

    ~FormatGuard()
    {
        if (ctx)
            avformat_close_input(&ctx);
    }
};

struct CodecGuard {
    AVCodecContext *ctx = nullptr;

    ~CodecGuard()
    {
        if (ctx)
            avcodec_free_context(&ctx);
    }
};

struct PacketGuard {
    AVPacket *pkt = nullptr;

    PacketGuard()
        : pkt(av_packet_alloc())
    {
    }

    ~PacketGuard()
    {
        if (pkt)
            av_packet_free(&pkt);
    }
};

struct FrameGuard {
    AVFrame *frame = nullptr;

    FrameGuard()
        : frame(av_frame_alloc())
    {
    }

    ~FrameGuard()
    {
        if (frame)
            av_frame_free(&frame);
    }
};

struct ChannelLayoutGuard {
    AVChannelLayout layout = {};

    ~ChannelLayoutGuard()
    {
        av_channel_layout_uninit(&layout);
    }
};

bool copyOrDefaultChannelLayout(ChannelLayoutGuard &dst,
                                const AVChannelLayout &src,
                                int channels)
{
    if (src.nb_channels > 0)
        return av_channel_layout_copy(&dst.layout, &src) >= 0;

    if (channels <= 0)
        return false;
    av_channel_layout_default(&dst.layout, channels);
    return dst.layout.nb_channels > 0;
}

bool appendResampledMonoFloat(SwrContext *swr,
                              int inSampleRate,
                              int outSampleRate,
                              const uint8_t *const *inData,
                              int inSamples,
                              QVector<float> &mono,
                              QString *error)
{
    if (!swr || inSampleRate <= 0 || outSampleRate <= 0 || inSamples < 0) {
        if (error)
            *error = QStringLiteral("invalid resampler state");
        return false;
    }

    const int64_t delay = swr_get_delay(swr, inSampleRate);
    const int64_t outSamples64 = av_rescale_rnd(
        delay + inSamples,
        outSampleRate,
        inSampleRate,
        AV_ROUND_UP);
    if (outSamples64 < 0
        || outSamples64 > std::numeric_limits<int>::max()) {
        if (error)
            *error = QStringLiteral("resampler output is too large");
        return false;
    }

    const int outSamples = static_cast<int>(outSamples64);
    if (outSamples == 0)
        return true;

    QVector<float> chunk(outSamples);
    uint8_t *outData = reinterpret_cast<uint8_t*>(chunk.data());
    const int converted = swr_convert(
        swr,
        &outData,
        outSamples,
        inData,
        inSamples);
    if (converted < 0) {
        if (error)
            *error = QStringLiteral("audio resample failed: %1")
                         .arg(avErrorString(converted));
        return false;
    }
    if (converted == 0)
        return true;
    if (mono.size() > std::numeric_limits<int>::max() - converted) {
        if (error)
            *error = QStringLiteral("decoded audio is too large");
        return false;
    }

    const int oldSize = mono.size();
    mono.resize(oldSize + converted);
    std::memcpy(mono.data() + oldSize,
                chunk.constData(),
                static_cast<size_t>(converted) * sizeof(float));
    return true;
}

bool decodeAudioWithMediaDecoder(const QString &audioPath,
                                 QVector<float> &mono,
                                 int &sampleRate,
                                 QString *error)
{
    libavcore::MediaDecoder decoder;
    if (auto openError = decoder.open(audioPath.toUtf8().constData(), true)) {
        if (error)
            *error = QString::fromStdString(*openError);
        return false;
    }
    if (!decoder.hasAudio()) {
        if (error)
            *error = QStringLiteral("input has no audio stream");
        return false;
    }

    const libavcore::AudioStreamProps props = decoder.audioProps();
    if (props.sampleRate <= 0 || props.channels <= 0
        || props.sampleFormat == AV_SAMPLE_FMT_NONE) {
        if (error)
            *error = QStringLiteral("decoder reported invalid audio properties");
        return false;
    }

    ChannelLayoutGuard inputLayout;
    if (!copyOrDefaultChannelLayout(inputLayout,
                                    props.channelLayout,
                                    props.channels)) {
        if (error)
            *error = QStringLiteral("failed to resolve input channel layout");
        return false;
    }

    ChannelLayoutGuard outputLayout;
    const AVChannelLayout monoLayout = AV_CHANNEL_LAYOUT_MONO;
    if (av_channel_layout_copy(&outputLayout.layout, &monoLayout) < 0) {
        if (error)
            *error = QStringLiteral("failed to create mono output layout");
        return false;
    }

    SwrGuard swr;
    int rc = swr_alloc_set_opts2(
        &swr.ctx,
        &outputLayout.layout,
        AV_SAMPLE_FMT_FLT,
        props.sampleRate,
        &inputLayout.layout,
        props.sampleFormat,
        props.sampleRate,
        0,
        nullptr);
    if (rc < 0 || !swr.ctx) {
        if (error)
            *error = QStringLiteral("failed to allocate audio resampler: %1")
                         .arg(rc < 0 ? avErrorString(rc)
                                     : QStringLiteral("unknown error"));
        return false;
    }

    rc = swr_init(swr.ctx);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("failed to initialize audio resampler: %1")
                         .arg(avErrorString(rc));
        return false;
    }

    sampleRate = props.sampleRate;
    while (!decoder.audioEnded()) {
        AVFrame *frame = decoder.nextAudioFrame();
        if (!frame)
            break;
        if (frame->nb_samples <= 0)
            continue;

        const uint8_t **inData =
            const_cast<const uint8_t**>(frame->extended_data);
        if (!appendResampledMonoFloat(swr.ctx,
                                      props.sampleRate,
                                      props.sampleRate,
                                      inData,
                                      frame->nb_samples,
                                      mono,
                                      error)) {
            return false;
        }
    }

    while (swr_get_delay(swr.ctx, props.sampleRate) > 0) {
        const int before = mono.size();
        if (!appendResampledMonoFloat(swr.ctx,
                                      props.sampleRate,
                                      props.sampleRate,
                                      nullptr,
                                      0,
                                      mono,
                                      error)) {
            return false;
        }
        if (mono.size() == before)
            break;
    }

    if (mono.isEmpty()) {
        if (error)
            *error = QStringLiteral("audio decode produced no samples");
        return false;
    }
    return true;
}

bool decodeAudioWithLibav(const QString &audioPath,
                          QVector<float> &mono,
                          int &sampleRate,
                          QString *error)
{
    FormatGuard input;
    int rc = avformat_open_input(&input.ctx,
                                 audioPath.toUtf8().constData(),
                                 nullptr,
                                 nullptr);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("cannot open input: %1")
                         .arg(avErrorString(rc));
        return false;
    }

    rc = avformat_find_stream_info(input.ctx, nullptr);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("cannot read stream info: %1")
                         .arg(avErrorString(rc));
        return false;
    }

    int audioStreamIndex = -1;
    for (unsigned i = 0; i < input.ctx->nb_streams; ++i) {
        AVStream *stream = input.ctx->streams[i];
        if (stream && stream->codecpar
            && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (audioStreamIndex < 0) {
        if (error)
            *error = QStringLiteral("input has no audio stream");
        return false;
    }

    AVStream *audioStream = input.ctx->streams[audioStreamIndex];
    AVCodecParameters *codecpar = audioStream->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        if (error)
            *error = QStringLiteral("no decoder for audio codec '%1'")
                         .arg(QString::fromUtf8(
                             avcodec_get_name(codecpar->codec_id)));
        return false;
    }

    CodecGuard decoder;
    decoder.ctx = avcodec_alloc_context3(codec);
    if (!decoder.ctx) {
        if (error)
            *error = QStringLiteral("failed to allocate audio decoder");
        return false;
    }

    rc = avcodec_parameters_to_context(decoder.ctx, codecpar);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("failed to copy audio codec parameters: %1")
                         .arg(avErrorString(rc));
        return false;
    }
    decoder.ctx->pkt_timebase = audioStream->time_base;

    rc = avcodec_open2(decoder.ctx, codec, nullptr);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("failed to open audio decoder: %1")
                         .arg(avErrorString(rc));
        return false;
    }
    if (decoder.ctx->sample_rate <= 0
        || decoder.ctx->sample_fmt == AV_SAMPLE_FMT_NONE) {
        if (error)
            *error = QStringLiteral("audio decoder reported invalid properties");
        return false;
    }

    ChannelLayoutGuard inputLayout;
    if (!copyOrDefaultChannelLayout(inputLayout,
                                    decoder.ctx->ch_layout,
                                    decoder.ctx->ch_layout.nb_channels)) {
        if (error)
            *error = QStringLiteral("failed to resolve input channel layout");
        return false;
    }

    ChannelLayoutGuard outputLayout;
    const AVChannelLayout monoLayout = AV_CHANNEL_LAYOUT_MONO;
    if (av_channel_layout_copy(&outputLayout.layout, &monoLayout) < 0) {
        if (error)
            *error = QStringLiteral("failed to create mono output layout");
        return false;
    }

    SwrGuard swr;
    rc = swr_alloc_set_opts2(
        &swr.ctx,
        &outputLayout.layout,
        AV_SAMPLE_FMT_FLT,
        decoder.ctx->sample_rate,
        &inputLayout.layout,
        decoder.ctx->sample_fmt,
        decoder.ctx->sample_rate,
        0,
        nullptr);
    if (rc < 0 || !swr.ctx) {
        if (error)
            *error = QStringLiteral("failed to allocate audio resampler: %1")
                         .arg(rc < 0 ? avErrorString(rc)
                                     : QStringLiteral("unknown error"));
        return false;
    }

    rc = swr_init(swr.ctx);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("failed to initialize audio resampler: %1")
                         .arg(avErrorString(rc));
        return false;
    }

    PacketGuard packet;
    FrameGuard frame;
    if (!packet.pkt || !frame.frame) {
        if (error)
            *error = QStringLiteral("failed to allocate decode buffers");
        return false;
    }

    sampleRate = decoder.ctx->sample_rate;
    bool gotSamples = false;
    auto receiveFrames = [&]() -> bool {
        while (true) {
            const int receiveRc =
                avcodec_receive_frame(decoder.ctx, frame.frame);
            if (receiveRc == AVERROR(EAGAIN) || receiveRc == AVERROR_EOF)
                return true;
            if (receiveRc < 0) {
                if (error)
                    *error = QStringLiteral("audio decode failed: %1")
                                 .arg(avErrorString(receiveRc));
                return false;
            }

            const uint8_t **inData =
                const_cast<const uint8_t**>(frame.frame->extended_data);
            const bool ok = appendResampledMonoFloat(swr.ctx,
                                                     decoder.ctx->sample_rate,
                                                     decoder.ctx->sample_rate,
                                                     inData,
                                                     frame.frame->nb_samples,
                                                     mono,
                                                     error);
            av_frame_unref(frame.frame);
            if (!ok)
                return false;
            gotSamples = true;
        }
    };

    while ((rc = av_read_frame(input.ctx, packet.pkt)) >= 0) {
        if (packet.pkt->stream_index != audioStreamIndex) {
            av_packet_unref(packet.pkt);
            continue;
        }

        rc = avcodec_send_packet(decoder.ctx, packet.pkt);
        if (rc == AVERROR(EAGAIN)) {
            if (!receiveFrames()) {
                av_packet_unref(packet.pkt);
                return false;
            }
            rc = avcodec_send_packet(decoder.ctx, packet.pkt);
        }
        av_packet_unref(packet.pkt);
        if (rc < 0) {
            if (error)
                *error = QStringLiteral("failed to send audio packet: %1")
                             .arg(avErrorString(rc));
            return false;
        }
        if (!receiveFrames())
            return false;
    }
    av_packet_unref(packet.pkt);

    if (rc != AVERROR_EOF) {
        if (error)
            *error = QStringLiteral("failed while reading input audio: %1")
                         .arg(avErrorString(rc));
        return false;
    }

    rc = avcodec_send_packet(decoder.ctx, nullptr);
    if (rc < 0 && rc != AVERROR_EOF) {
        if (error)
            *error = QStringLiteral("failed to flush audio decoder: %1")
                         .arg(avErrorString(rc));
        return false;
    }
    if (!receiveFrames())
        return false;

    while (swr_get_delay(swr.ctx, decoder.ctx->sample_rate) > 0) {
        const int before = mono.size();
        if (!appendResampledMonoFloat(swr.ctx,
                                      decoder.ctx->sample_rate,
                                      decoder.ctx->sample_rate,
                                      nullptr,
                                      0,
                                      mono,
                                      error)) {
            return false;
        }
        if (mono.size() == before)
            break;
    }

    if (!gotSamples || mono.isEmpty()) {
        if (error)
            *error = QStringLiteral("audio decode produced no samples");
        return false;
    }
    return true;
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
        return std::numeric_limits<double>::quiet_NaN();
    }

    QVector<float> mono;
    int sampleRate = 0;
    QString error;
    if (!decodeAudioWithMediaDecoder(audioPath, mono, sampleRate, &error)) {
        const QString mediaDecoderError = error;
        mono.clear();
        sampleRate = 0;
        error.clear();
        if (!decodeAudioWithLibav(audioPath, mono, sampleRate, &error)) {
            qWarning("loudness::measureIntegratedLufs: MediaDecoder failed for "
                     "'%s': %s",
                     qUtf8Printable(audioPath),
                     qUtf8Printable(mediaDecoderError));
            qWarning("loudness::measureIntegratedLufs: audio decode failed for "
                     "'%s': %s",
                     qUtf8Printable(audioPath),
                     qUtf8Printable(error));
            return std::numeric_limits<double>::quiet_NaN();
        }
    }

    if (mono.isEmpty() || sampleRate <= 0) {
        qWarning("loudness::measureIntegratedLufs: decoded no audio samples "
                 "from '%s'",
                 qUtf8Printable(audioPath));
        return std::numeric_limits<double>::quiet_NaN();
    }

    return measureIntegratedLufsFromSamples(mono, sampleRate);
}

} // namespace loudness
