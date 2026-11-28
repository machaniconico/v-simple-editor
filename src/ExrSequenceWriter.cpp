#include "ExrSequenceWriter.h"
#include "CodecDetector.h"

#include <QFile>
#include <QImageReader>
#include <QSaveFile>
#include <QtEndian>
#include <qfloat16.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace exrseq {
namespace {
std::mutex overrideMutex;
std::optional<bool> encoderOverride;

struct ContextDeleter {
    void operator()(AVCodecContext *ctx) const { avcodec_free_context(&ctx); }
};
struct FrameDeleter {
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
struct PacketDeleter {
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};
using ContextPtr = std::unique_ptr<AVCodecContext, ContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

QString avError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

float channelValue(uchar sample, bool linearize)
{
    const float value = sample / 255.0f;
    if (!linearize) return value;
    return value <= 0.04045f ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

bool readNullString(const QByteArray &bytes, qsizetype *offset, QByteArray *value)
{
    if (*offset < 0 || *offset >= bytes.size()) return false;
    const qsizetype end = bytes.indexOf('\0', *offset);
    if (end < 0) return false;
    *value = bytes.mid(*offset, end - *offset);
    *offset = end + 1;
    return true;
}

bool parseHeaderAlpha(const QByteArray &bytes, bool *hasAlpha)
{
    if (!hasAlpha || bytes.size() < 9 || std::memcmp(bytes.constData(), "\x76\x2f\x31\x01", 4) != 0)
        return false;
    *hasAlpha = false;
    qsizetype offset = 8; // magic and version
    bool foundChannels = false;
    while (offset < bytes.size()) {
        QByteArray name, type;
        if (!readNullString(bytes, &offset, &name)) return false;
        if (name.isEmpty()) return foundChannels;
        if (!readNullString(bytes, &offset, &type) || offset + 4 > bytes.size()) return false;
        const auto size = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(bytes.constData() + offset));
        offset += 4;
        if (size > quint64(bytes.size() - offset)) return false;
        if (name == "channels" && type == "chlist") {
            foundChannels = true;
            const qsizetype end = offset + size;
            qsizetype channelOffset = offset;
            bool terminated = false;
            while (channelOffset < end) {
                QByteArray channel;
                if (!readNullString(bytes, &channelOffset, &channel) || channelOffset > end)
                    return false;
                if (channel.isEmpty()) {
                    terminated = true;
                    break;
                }
                if (channelOffset + 16 > end) return false;
                if (channel == "A") *hasAlpha = true;
                channelOffset += 16; // pixel type, pLinear, reserved, x/y sampling
            }
            if (!terminated) return false;
        }
        offset += size;
    }
    return false;
}

bool copyDecodedFloat(const AVFrame *frame, QVector<float> *rgba,
                      int *w, int *h, bool hasAlpha)
{
    if (!frame || !rgba || !w || !h || frame->width <= 0 || frame->height <= 0
        || frame->width > 32768 || frame->height > 32768
        || qint64(frame->width) * frame->height > 64 * 1024 * 1024)
        return false;
    const auto pixelFormat = static_cast<AVPixelFormat>(frame->format);
    const bool half = pixelFormat == AV_PIX_FMT_GBRAPF16LE || pixelFormat == AV_PIX_FMT_GBRPF16LE;
    const bool alphaFormat = pixelFormat == AV_PIX_FMT_GBRAPF16LE
        || pixelFormat == AV_PIX_FMT_GBRAPF32LE;
    if (!half && pixelFormat != AV_PIX_FMT_GBRAPF32LE && pixelFormat != AV_PIX_FMT_GBRPF32LE)
        return false;
    if (hasAlpha && !alphaFormat) return false;
    *w = frame->width;
    *h = frame->height;
    rgba->resize(frame->width * frame->height * 4);
    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            const int dest = (y * frame->width + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const int plane = (c + 2) % 3; // RGB from G, B, R planes
                float sample = 0;
                const uchar *source = frame->data[plane] + y * frame->linesize[plane]
                    + x * (half ? sizeof(qfloat16) : sizeof(float));
                if (half) {
                    qfloat16 value;
                    std::memcpy(&value, source, sizeof(value));
                    sample = static_cast<float>(value);
                } else {
                    std::memcpy(&sample, source, sizeof(sample));
                }
                (*rgba)[dest + c] = sample;
            }
            float alpha = 1.0f;
            if (hasAlpha) {
                const uchar *source = frame->data[3] + y * frame->linesize[3]
                    + x * (half ? sizeof(qfloat16) : sizeof(float));
                if (half) {
                    qfloat16 value;
                    std::memcpy(&value, source, sizeof(value));
                    alpha = static_cast<float>(value);
                } else {
                    std::memcpy(&alpha, source, sizeof(alpha));
                }
            }
            (*rgba)[dest + 3] = alpha;
        }
    }
    return true;
}

bool decodePacket(AVCodecContext *context, const AVPacket *packet,
                  QVector<float> *rgba, int *w, int *h, bool hasAlpha)
{
    FramePtr frame(av_frame_alloc());
    if (!frame || avcodec_send_packet(context, packet) < 0) return false;
    return avcodec_receive_frame(context, frame.get()) == 0
        && copyDecodedFloat(frame.get(), rgba, w, h, hasAlpha);
}

bool decodeWithAvformat(const QString &path, QVector<float> *rgba,
                        int *w, int *h, bool hasAlpha)
{
    AVFormatContext *format = nullptr;
    if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    const auto closeFormat = [](AVFormatContext *ctx) { avformat_close_input(&ctx); };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> owned(format, closeFormat);
    if (avformat_find_stream_info(format, nullptr) < 0) return false;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) return false;
    const AVCodecParameters *parameters = format->streams[stream]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(parameters->codec_id);
    ContextPtr context(decoder ? avcodec_alloc_context3(decoder) : nullptr);
    if (!context || avcodec_parameters_to_context(context.get(), parameters) < 0
        || avcodec_open2(context.get(), decoder, nullptr) < 0)
        return false;
    PacketPtr packet(av_packet_alloc());
    if (!packet) return false;
    while (av_read_frame(format, packet.get()) >= 0) {
        bool ok = false;
        if (packet->stream_index == stream)
            ok = decodePacket(context.get(), packet.get(), rgba, w, h, hasAlpha);
        av_packet_unref(packet.get());
        if (ok) return true;
    }
    return false;
}

bool decodeDirect(const QString &path, QVector<float> *rgba,
                  int *w, int *h, bool hasAlpha)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly) || input.size() <= 0
        || input.size() > 512 * 1024 * 1024) return false;
    const QByteArray bytes = input.readAll();
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_EXR);
    ContextPtr context(decoder ? avcodec_alloc_context3(decoder) : nullptr);
    if (!context || avcodec_open2(context.get(), decoder, nullptr) < 0) return false;
    PacketPtr packet(av_packet_alloc());
    if (!packet || av_new_packet(packet.get(), bytes.size()) < 0) return false;
    std::memcpy(packet->data, bytes.constData(), bytes.size());
    return decodePacket(context.get(), packet.get(), rgba, w, h, hasAlpha);
}
} // namespace

bool encoderAvailable()
{
    {
        std::lock_guard<std::mutex> lock(overrideMutex);
        if (encoderOverride.has_value()) return *encoderOverride;
    }
    return CodecDetector::isEncoderAvailable(QStringLiteral("exr"));
}

void setEncoderAvailableOverrideForTest(std::optional<bool> available)
{
    std::lock_guard<std::mutex> lock(overrideMutex);
    encoderOverride = available;
}

QString encodeFrame(const QImage &image, const QString &path, const Options &options)
{
    if (!encoderAvailable()) return QStringLiteral("EXR エンコーダを利用できません");
    if (image.isNull() || image.width() <= 0 || image.height() <= 0 || path.isEmpty())
        return QStringLiteral("EXR の画像または保存先が無効です");
    const AVCodec *encoder = avcodec_find_encoder_by_name("exr");
    ContextPtr context(encoder ? avcodec_alloc_context3(encoder) : nullptr);
    if (!context) return QStringLiteral("EXR エンコーダを初期化できません");
    context->width = image.width();
    context->height = image.height();
    context->time_base = AVRational{1, 1};
    context->pix_fmt = options.keepAlpha ? AV_PIX_FMT_GBRAPF32LE : AV_PIX_FMT_GBRPF32LE;
    const char *format = options.pixelType == PixelType::Half ? "half" : "float";
    const char *compression = options.compression == Compression::Zip ? "zip16"
        : options.compression == Compression::Rle ? "rle" : "none";
    if (!context->priv_data || av_opt_set(context->priv_data, "format", format, 0) < 0
        || av_opt_set(context->priv_data, "compression", compression, 0) < 0)
        return QStringLiteral("EXR の画素形式または圧縮形式を設定できません");
    int rc = avcodec_open2(context.get(), encoder, nullptr);
    if (rc < 0) return QStringLiteral("EXR エンコーダを開けません: %1").arg(avError(rc));

    FramePtr frame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());
    if (!frame || !packet) return QStringLiteral("EXR のフレームを確保できません");
    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    if ((rc = av_frame_get_buffer(frame.get(), 32)) < 0)
        return QStringLiteral("EXR の画素領域を確保できません: %1").arg(avError(rc));
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *source = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            const uchar *pixel = source + x * 4;
            const float values[4] = {
                channelValue(pixel[1], options.linearize), // G
                channelValue(pixel[2], options.linearize), // B
                channelValue(pixel[0], options.linearize), // R
                options.keepAlpha ? pixel[3] / 255.0f : 1.0f
            };
            for (int plane = 0; plane < (options.keepAlpha ? 4 : 3); ++plane)
                std::memcpy(frame->data[plane] + y * frame->linesize[plane]
                            + x * sizeof(float), &values[plane], sizeof(float));
        }
    }
    rc = avcodec_send_frame(context.get(), frame.get());
    if (rc < 0) return QStringLiteral("EXR フレームを送信できません: %1").arg(avError(rc));
    rc = avcodec_receive_packet(context.get(), packet.get());
    if (rc < 0 || packet->size <= 0)
        return QStringLiteral("EXR フレームを符号化できません: %1").arg(avError(rc));
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return output.errorString();
    if (output.write(reinterpret_cast<const char *>(packet->data), packet->size) != packet->size) {
        output.cancelWriting();
        return QStringLiteral("EXR ファイルを書き込めません: %1").arg(output.errorString());
    }
    if (!output.commit()) return output.errorString();
    return {};
}

bool headerHasAlphaForTest(const QString &path, bool *hasAlpha)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return false;
    return parseHeaderAlpha(input.read(1024 * 1024), hasAlpha);
}

bool decodeFloatForTest(const QString &path, QVector<float> *rgba,
                        int *w, int *h, bool *hasAlpha)
{
    if (!rgba || !w || !h || !hasAlpha || !headerHasAlphaForTest(path, hasAlpha))
        return false;
    rgba->clear();
    *w = *h = 0;
    if (decodeWithAvformat(path, rgba, w, h, *hasAlpha)) return true;
    return decodeDirect(path, rgba, w, h, *hasAlpha);
}

bool decodeForTest(const QString &path, QImage *rgba8, bool *hasAlpha, QString *how)
{
    if (!rgba8 || !hasAlpha || !how || !headerHasAlphaForTest(path, hasAlpha)) return false;
    *how = QStringLiteral("QImageReader");
    QImageReader reader(path, "exr");
    if (reader.canRead()) {
        *rgba8 = reader.read().convertToFormat(QImage::Format_RGBA8888);
        if (!rgba8->isNull()) return true;
    }
    QVector<float> floats;
    int width = 0, height = 0;
    if (decodeWithAvformat(path, &floats, &width, &height, *hasAlpha)) {
        *how = QStringLiteral("avformat");
    } else if (decodeDirect(path, &floats, &width, &height, *hasAlpha)) {
        *how = QStringLiteral("magic+chlist");
    } else {
        *how = QStringLiteral("decode failed");
        return false;
    }
    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull()) return false;
    for (int y = 0; y < height; ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const int index = (y * width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const float value = floats[index + c];
                row[x * 4 + c] = static_cast<uchar>(std::lround(
                    std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f) * 255.0f));
            }
        }
    }
    *rgba8 = image;
    return true;
}
} // namespace exrseq
