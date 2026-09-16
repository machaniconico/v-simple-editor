#include "AudioExtract.h"

#include <QFileInfo>
#include <algorithm>
#include <QFile>
#include <QIODevice>
#include <QString>

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

#include <cstdint>
#include <limits>

namespace libavcore {

// 16bit PCM WAV (RIFF/fmt/data) を読み込み mono の double サンプル列へ展開する。
// 多チャンネルは平均してモノラル化。成功時 true、sampleRate を *sampleRate へ。
// 簡易パーサ — float WAV や 24/32bit は非対応 (抽出側を 16bit に固定する前提)。
bool readPcm16WavToMono(const QString &wavPath,
                        std::vector<double> &outSamples,
                        int &sampleRate,
                        QString *error)
{
    outSamples.clear();
    sampleRate = 0;

    constexpr qint64 kMaxWavBytes = 512LL * 1024LL * 1024LL;
    const QFileInfo info(wavPath);
    if (!info.exists()) {
        if (error) *error = QStringLiteral("WAV が存在しません: %1").arg(wavPath);
        return false;
    }
    if (info.size() <= 0) {
        if (error) *error = QStringLiteral("WAV が空です: %1").arg(wavPath);
        return false;
    }
    if (info.size() > kMaxWavBytes) {
        if (error) *error =
            QStringLiteral("WAV が大きすぎます (%1 MB、上限 %2 MB)。")
                .arg(info.size() / (1024.0 * 1024.0), 0, 'f', 1)
                .arg(kMaxWavBytes / (1024 * 1024));
        return false;
    }

    QFile f(wavPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("WAV を開けません: %1").arg(wavPath);
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();

    if (data.size() != info.size()) {
        if (error) *error =
            QStringLiteral("WAV の読み込みが途中で終了しました (%1/%2 bytes)。")
                .arg(data.size()).arg(info.size());
        return false;
    }
    if (data.size() < 44 || !data.startsWith("RIFF") ||
        data.mid(8, 4) != QByteArray("WAVE")) {
        if (error) *error = QStringLiteral("RIFF/WAVE ヘッダが不正です。");
        return false;
    }

    const auto rdU16 = [&](int off) -> quint16 {
        return static_cast<quint16>(static_cast<unsigned char>(data[off])) |
               (static_cast<quint16>(static_cast<unsigned char>(data[off + 1])) << 8);
    };
    const auto rdU32 = [&](int off) -> quint32 {
        return static_cast<quint32>(static_cast<unsigned char>(data[off])) |
               (static_cast<quint32>(static_cast<unsigned char>(data[off + 1])) << 8) |
               (static_cast<quint32>(static_cast<unsigned char>(data[off + 2])) << 16) |
               (static_cast<quint32>(static_cast<unsigned char>(data[off + 3])) << 24);
    };

    int channels = 1;
    int sr = 0;
    int bitsPerSample = 16;
    int audioFormat = 1;
    int blockAlign = 0;
    bool haveFmt = false;
    int dataOff = -1;
    int dataLen = 0;

    // チャンク走査 (fmt と data を探す)。
    int pos = 12;
    while (pos + 8 <= data.size()) {
        const QByteArray id = data.mid(pos, 4);
        const quint32 sz = rdU32(pos + 4);
        const int body = pos + 8;
        const qint64 chunkEnd = static_cast<qint64>(body) + static_cast<qint64>(sz);
        const qint64 nextChunk = chunkEnd + ((sz & 1u) ? 1 : 0);
        if (chunkEnd > data.size()) {
            if (error) *error =
                QStringLiteral("WAV チャンク '%1' がファイル終端を越えています。")
                    .arg(QString::fromLatin1(id.constData(), id.size()));
            return false;
        }

        if (id == QByteArray("fmt ")) {
            if (sz < 16) {
                if (error) *error = QStringLiteral("WAV fmt チャンクが短すぎます。");
                return false;
            }
            audioFormat   = rdU16(body);
            channels      = rdU16(body + 2);
            const quint32 parsedSr = rdU32(body + 4);
            if (parsedSr > static_cast<quint32>(std::numeric_limits<int>::max())) {
                if (error) *error = QStringLiteral("WAV の sample rate が大きすぎます。");
                return false;
            }
            sr            = static_cast<int>(parsedSr);
            blockAlign    = rdU16(body + 12);
            bitsPerSample = rdU16(body + 14);
            haveFmt = true;
        } else if (id == QByteArray("data")) {
            dataOff = body;
            dataLen = static_cast<int>(sz);
        }
        // チャンクは 2byte 境界 (奇数長は +1 パディング)。
        pos = static_cast<int>(std::min<qint64>(nextChunk, data.size()));
        if (dataOff >= 0 && haveFmt) break;
    }

    if (audioFormat != 1 || bitsPerSample != 16) {
        if (error) *error =
            QStringLiteral("対応するのは 16bit PCM WAV のみです (format=%1, bits=%2)。")
                .arg(audioFormat).arg(bitsPerSample);
        return false;
    }
    if (!haveFmt || dataOff < 0 || dataLen <= 0 || sr <= 0 || channels <= 0) {
        if (error) *error = QStringLiteral("WAV の data/fmt チャンクが見つかりません。");
        return false;
    }

    if (channels > 64) {
        if (error) *error = QStringLiteral("WAV の channel 数が大きすぎます (%1)。").arg(channels);
        return false;
    }

    const int frameBytes = 2 * channels;
    if (blockAlign != frameBytes) {
        if (error) *error =
            QStringLiteral("WAV の block align が不正です (blockAlign=%1, expected=%2)。")
                .arg(blockAlign).arg(frameBytes);
        return false;
    }

    const int frameCount = dataLen / frameBytes;
    if (frameCount <= 0) {
        if (error) *error = QStringLiteral("WAV の data チャンクにサンプルがありません。");
        return false;
    }

    outSamples.clear();
    outSamples.reserve(frameCount);
    const char *p = data.constData() + dataOff;
    for (int i = 0; i < frameCount; ++i) {
        double acc = 0.0;
        for (int c = 0; c < channels; ++c) {
            const int o = i * frameBytes + c * 2;
            const qint16 s = static_cast<qint16>(
                static_cast<quint16>(static_cast<unsigned char>(p[o])) |
                (static_cast<quint16>(static_cast<unsigned char>(p[o + 1])) << 8));
            acc += static_cast<double>(s) / 32768.0;
        }
        outSamples.push_back(acc / channels);
    }
    sampleRate = sr;
    return true;
}



namespace {

QString avErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, buf, sizeof(buf)) < 0)
        return QString::number(err);
    return QString::fromUtf8(buf);
}

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool writeAll(QFile& file, const char* data, qint64 bytes)
{
    return file.write(data, bytes) == bytes;
}

bool writeFourCc(QFile& file, const char (&value)[5])
{
    return writeAll(file, value, 4);
}

bool writeLe16(QFile& file, std::uint16_t value)
{
    const char data[2] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8) & 0xffu)
    };
    return writeAll(file, data, 2);
}

bool writeLe32(QFile& file, std::uint32_t value)
{
    const char data[4] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8) & 0xffu),
        static_cast<char>((value >> 16) & 0xffu),
        static_cast<char>((value >> 24) & 0xffu)
    };
    return writeAll(file, data, 4);
}

struct InputFormatGuard {
    AVFormatContext* ctx = nullptr;

    ~InputFormatGuard()
    {
        if (ctx)
            avformat_close_input(&ctx);
    }
};

struct CodecContextGuard {
    AVCodecContext* ctx = nullptr;

    ~CodecContextGuard()
    {
        if (ctx)
            avcodec_free_context(&ctx);
    }
};

struct PacketGuard {
    AVPacket* pkt = nullptr;

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
    AVFrame* frame = nullptr;

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

struct SwrGuard {
    SwrContext* ctx = nullptr;

    ~SwrGuard()
    {
        if (ctx)
            swr_free(&ctx);
    }
};

struct ChannelLayoutGuard {
    AVChannelLayout layout = {};

    ~ChannelLayoutGuard()
    {
        av_channel_layout_uninit(&layout);
    }
};

bool appendResampledPcm(SwrContext* swr,
                        int inSampleRate,
                        int outSampleRate,
                        const uint8_t* const* inData,
                        int inSamples,
                        QByteArray& pcm,
                        QString* error)
{
    if (!swr || inSampleRate <= 0 || outSampleRate <= 0 || inSamples < 0)
        return fail(error, QStringLiteral("invalid resampler state"));

    const int64_t delay = swr_get_delay(swr, inSampleRate);
    const int64_t outSamples64 = av_rescale_rnd(
        delay + inSamples,
        outSampleRate,
        inSampleRate,
        AV_ROUND_UP);
    if (outSamples64 < 0
        || outSamples64 > std::numeric_limits<int>::max()) {
        return fail(error, QStringLiteral("resampler output is too large"));
    }

    const int outSamples = static_cast<int>(outSamples64);
    if (outSamples == 0)
        return true;

    const int bufferSize = av_samples_get_buffer_size(
        nullptr,
        1,
        outSamples,
        AV_SAMPLE_FMT_S16,
        1);
    if (bufferSize < 0)
        return fail(error, QStringLiteral("failed to size resampler buffer: %1")
                              .arg(avErrorString(bufferSize)));

    QByteArray chunk;
    chunk.resize(bufferSize);
    uint8_t* outData = reinterpret_cast<uint8_t*>(chunk.data());

    const int converted = swr_convert(
        swr,
        &outData,
        outSamples,
        inData,
        inSamples);
    if (converted < 0)
        return fail(error, QStringLiteral("audio resample failed: %1")
                              .arg(avErrorString(converted)));
    if (converted == 0)
        return true;

    const int convertedBytes = av_samples_get_buffer_size(
        nullptr,
        1,
        converted,
        AV_SAMPLE_FMT_S16,
        1);
    if (convertedBytes < 0)
        return fail(error, QStringLiteral("failed to size converted audio: %1")
                              .arg(avErrorString(convertedBytes)));
    if (pcm.size() > std::numeric_limits<int>::max() - convertedBytes)
        return fail(error, QStringLiteral("decoded PCM is too large"));

    pcm.append(chunk.constData(), convertedBytes);
    return true;
}

} // namespace

bool writePcm16AsWav(const QString& wavPath,
                     const QByteArray& pcmS16le,
                     int sampleRate,
                     int channels,
                     QString* error)
{
    if (error)
        error->clear();

    if (wavPath.isEmpty())
        return fail(error, QStringLiteral("WAV path is empty"));
    if (sampleRate <= 0)
        return fail(error, QStringLiteral("sample rate must be positive"));
    if (channels <= 0)
        return fail(error, QStringLiteral("channel count must be positive"));

    const std::uint64_t bytesPerSample = 2;
    const std::uint64_t blockAlign64 =
        static_cast<std::uint64_t>(channels) * bytesPerSample;
    const std::uint64_t byteRate64 =
        static_cast<std::uint64_t>(sampleRate) * blockAlign64;
    const std::uint64_t dataSize =
        static_cast<std::uint64_t>(pcmS16le.size());

    if (blockAlign64 > std::numeric_limits<std::uint16_t>::max())
        return fail(error, QStringLiteral("WAV block alignment is too large"));
    if (byteRate64 > std::numeric_limits<std::uint32_t>::max())
        return fail(error, QStringLiteral("WAV byte rate is too large"));
    if (dataSize > std::numeric_limits<std::uint32_t>::max() - 36u)
        return fail(error, QStringLiteral("PCM data is too large for RIFF/WAVE"));
    if (blockAlign64 > 0 && (dataSize % blockAlign64) != 0)
        return fail(error, QStringLiteral("PCM data size is not sample-aligned"));

    QFile file(wavPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(error, QStringLiteral("failed to open WAV output '%1': %2")
                              .arg(wavPath, file.errorString()));

    const std::uint32_t riffSize = static_cast<std::uint32_t>(36u + dataSize);
    const std::uint32_t dataSize32 = static_cast<std::uint32_t>(dataSize);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(blockAlign64);
    const std::uint32_t byteRate = static_cast<std::uint32_t>(byteRate64);

    const bool ok =
        writeFourCc(file, "RIFF")
        && writeLe32(file, riffSize)
        && writeFourCc(file, "WAVE")
        && writeFourCc(file, "fmt ")
        && writeLe32(file, 16)
        && writeLe16(file, 1)
        && writeLe16(file, static_cast<std::uint16_t>(channels))
        && writeLe32(file, static_cast<std::uint32_t>(sampleRate))
        && writeLe32(file, byteRate)
        && writeLe16(file, blockAlign)
        && writeLe16(file, 16)
        && writeFourCc(file, "data")
        && writeLe32(file, dataSize32)
        && writeAll(file, pcmS16le.constData(), pcmS16le.size());

    if (!ok)
        return fail(error, QStringLiteral("failed to write WAV output '%1': %2")
                              .arg(wavPath, file.errorString()));

    if (!file.flush())
        return fail(error, QStringLiteral("failed to flush WAV output '%1': %2")
                              .arg(wavPath, file.errorString()));

    return true;
}

bool extractAudioToWav(const QString& videoPath,
                       const QString& wavPath,
                       int sampleRate,
                       QString* error)
{
    if (error)
        error->clear();

    if (videoPath.isEmpty())
        return fail(error, QStringLiteral("input media path is empty"));
    if (wavPath.isEmpty())
        return fail(error, QStringLiteral("WAV path is empty"));
    if (sampleRate <= 0)
        return fail(error, QStringLiteral("sample rate must be positive"));

    InputFormatGuard input;
    int rc = avformat_open_input(&input.ctx,
                                 videoPath.toUtf8().constData(),
                                 nullptr,
                                 nullptr);
    if (rc < 0)
        return fail(error, QStringLiteral("cannot open input '%1': %2")
                              .arg(videoPath, avErrorString(rc)));

    rc = avformat_find_stream_info(input.ctx, nullptr);
    if (rc < 0)
        return fail(error, QStringLiteral("cannot read stream info for '%1': %2")
                              .arg(videoPath, avErrorString(rc)));

    int audioStreamIndex = -1;
    for (unsigned i = 0; i < input.ctx->nb_streams; ++i) {
        AVStream* stream = input.ctx->streams[i];
        if (stream && stream->codecpar
            && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (audioStreamIndex < 0)
        return fail(error, QStringLiteral("input has no audio stream: '%1'")
                              .arg(videoPath));

    AVStream* audioStream = input.ctx->streams[audioStreamIndex];
    AVCodecParameters* codecpar = audioStream->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder)
        return fail(error, QStringLiteral("no decoder for audio codec '%1'")
                              .arg(QString::fromUtf8(
                                  avcodec_get_name(codecpar->codec_id))));

    CodecContextGuard decoderCtx;
    decoderCtx.ctx = avcodec_alloc_context3(decoder);
    if (!decoderCtx.ctx)
        return fail(error, QStringLiteral("failed to allocate audio decoder"));

    rc = avcodec_parameters_to_context(decoderCtx.ctx, codecpar);
    if (rc < 0)
        return fail(error, QStringLiteral("failed to copy audio codec parameters: %1")
                              .arg(avErrorString(rc)));
    decoderCtx.ctx->pkt_timebase = audioStream->time_base;

    rc = avcodec_open2(decoderCtx.ctx, decoder, nullptr);
    if (rc < 0)
        return fail(error, QStringLiteral("failed to open audio decoder: %1")
                              .arg(avErrorString(rc)));
    if (decoderCtx.ctx->sample_rate <= 0)
        return fail(error, QStringLiteral("audio decoder reported invalid sample rate"));
    if (decoderCtx.ctx->ch_layout.nb_channels <= 0)
        return fail(error, QStringLiteral("audio decoder reported invalid channel layout"));

    ChannelLayoutGuard inputLayout;
    rc = av_channel_layout_copy(&inputLayout.layout,
                                &decoderCtx.ctx->ch_layout);
    if (rc < 0)
        return fail(error, QStringLiteral("failed to copy input channel layout: %1")
                              .arg(avErrorString(rc)));

    ChannelLayoutGuard outputLayout;
    av_channel_layout_default(&outputLayout.layout, 1);

    SwrGuard swr;
    rc = swr_alloc_set_opts2(
        &swr.ctx,
        &outputLayout.layout,
        AV_SAMPLE_FMT_S16,
        sampleRate,
        &inputLayout.layout,
        decoderCtx.ctx->sample_fmt,
        decoderCtx.ctx->sample_rate,
        0,
        nullptr);
    if (rc < 0 || !swr.ctx)
        return fail(error, QStringLiteral("failed to allocate audio resampler: %1")
                              .arg(rc < 0 ? avErrorString(rc)
                                          : QStringLiteral("unknown error")));

    rc = swr_init(swr.ctx);
    if (rc < 0)
        return fail(error, QStringLiteral("failed to initialize audio resampler: %1")
                              .arg(avErrorString(rc)));

    PacketGuard packet;
    FrameGuard frame;
    if (!packet.pkt || !frame.frame)
        return fail(error, QStringLiteral("failed to allocate decode buffers"));

    QByteArray pcm;
    bool gotSamples = false;

    auto receiveFrames = [&]() -> bool {
        while (true) {
            const int receiveRc =
                avcodec_receive_frame(decoderCtx.ctx, frame.frame);
            if (receiveRc == AVERROR(EAGAIN) || receiveRc == AVERROR_EOF)
                return true;
            if (receiveRc < 0)
                return fail(error, QStringLiteral("audio decode failed: %1")
                                      .arg(avErrorString(receiveRc)));

            const uint8_t* const* inData =
                const_cast<const uint8_t* const*>(frame.frame->extended_data);
            if (!appendResampledPcm(swr.ctx,
                                    decoderCtx.ctx->sample_rate,
                                    sampleRate,
                                    inData,
                                    frame.frame->nb_samples,
                                    pcm,
                                    error)) {
                av_frame_unref(frame.frame);
                return false;
            }
            gotSamples = true;
            av_frame_unref(frame.frame);
        }
    };

    while ((rc = av_read_frame(input.ctx, packet.pkt)) >= 0) {
        if (packet.pkt->stream_index != audioStreamIndex) {
            av_packet_unref(packet.pkt);
            continue;
        }

        rc = avcodec_send_packet(decoderCtx.ctx, packet.pkt);
        if (rc == AVERROR(EAGAIN)) {
            if (!receiveFrames()) {
                av_packet_unref(packet.pkt);
                return false;
            }
            rc = avcodec_send_packet(decoderCtx.ctx, packet.pkt);
        }
        av_packet_unref(packet.pkt);

        if (rc < 0)
            return fail(error, QStringLiteral("failed to send audio packet: %1")
                                  .arg(avErrorString(rc)));
        if (!receiveFrames())
            return false;
    }
    av_packet_unref(packet.pkt);

    if (rc != AVERROR_EOF)
        return fail(error, QStringLiteral("failed while reading input audio: %1")
                              .arg(avErrorString(rc)));

    rc = avcodec_send_packet(decoderCtx.ctx, nullptr);
    if (rc < 0 && rc != AVERROR_EOF)
        return fail(error, QStringLiteral("failed to flush audio decoder: %1")
                              .arg(avErrorString(rc)));
    if (!receiveFrames())
        return false;

    while (swr_get_delay(swr.ctx, decoderCtx.ctx->sample_rate) > 0) {
        const int before = pcm.size();
        if (!appendResampledPcm(swr.ctx,
                                decoderCtx.ctx->sample_rate,
                                sampleRate,
                                nullptr,
                                0,
                                pcm,
                                error)) {
            return false;
        }
        if (pcm.size() == before)
            break;
    }

    if (!gotSamples || pcm.isEmpty())
        return fail(error, QStringLiteral("audio decode produced no PCM samples"));

    return writePcm16AsWav(wavPath, pcm, sampleRate, 1, error);
}

} // namespace libavcore
