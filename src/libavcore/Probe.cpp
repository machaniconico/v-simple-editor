#include "Probe.h"

namespace libavcore {

namespace {

// Tiny RAII guard for AVFormatContext input contexts.
struct InputCtxGuard {
    AVFormatContext* ctx = nullptr;
    ~InputCtxGuard() {
        if (ctx) {
            avformat_close_input(&ctx);
        }
    }
};

} // namespace

bool encoderAvailable(const std::string& codecName)
{
    if (codecName.empty()) return false;
    return avcodec_find_encoder_by_name(codecName.c_str()) != nullptr;
}

bool decoderAvailable(const std::string& codecName)
{
    if (codecName.empty()) return false;
    return avcodec_find_decoder_by_name(codecName.c_str()) != nullptr;
}

std::optional<int64_t> probeDurationMicroseconds(const std::string& filePath)
{
    if (filePath.empty()) return std::nullopt;

    InputCtxGuard guard;
    if (avformat_open_input(&guard.ctx, filePath.c_str(), nullptr, nullptr) < 0) {
        return std::nullopt;
    }
    if (avformat_find_stream_info(guard.ctx, nullptr) < 0) {
        return std::nullopt;
    }
    if (guard.ctx->duration == AV_NOPTS_VALUE || guard.ctx->duration < 0) {
        return std::nullopt;
    }
    return static_cast<int64_t>(guard.ctx->duration);
}

std::optional<std::string> probeVideoCodecName(const std::string& filePath)
{
    if (filePath.empty()) return std::nullopt;

    InputCtxGuard guard;
    if (avformat_open_input(&guard.ctx, filePath.c_str(), nullptr, nullptr) < 0) {
        return std::nullopt;
    }
    if (avformat_find_stream_info(guard.ctx, nullptr) < 0) {
        return std::nullopt;
    }
    for (unsigned i = 0; i < guard.ctx->nb_streams; ++i) {
        AVCodecParameters* par = guard.ctx->streams[i]->codecpar;
        if (par && par->codec_type == AVMEDIA_TYPE_VIDEO) {
            const char* name = avcodec_get_name(par->codec_id);
            if (name) {
                return std::string(name);
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> firstTenBitHevcEncoder()
{
    static const char* const kCandidates[] = {
        "libx265", "hevc_nvenc", "hevc_qsv", "hevc_amf"
    };

    for (const char* name : kCandidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) continue;

        // Query supported pixel formats via FFmpeg 8 API.
        // avcodec_get_supported_config replaces the deprecated AVCodec::pix_fmts.
        const void* cfgList = nullptr;
        int count = 0;
        int ret = avcodec_get_supported_config(
            nullptr, codec,
            AV_CODEC_CONFIG_PIX_FORMAT,
            /*flags=*/0,
            &cfgList,
            &count);

        // Treat query failure, NULL list, or empty list as "not 10-bit capable".
        if (ret < 0 || cfgList == nullptr || count <= 0) continue;

        const AVPixelFormat* fmts = static_cast<const AVPixelFormat*>(cfgList);
        bool tenBit = false;
        for (int i = 0; i < count; ++i) {
            if (fmts[i] == AV_PIX_FMT_YUV420P10LE ||
                fmts[i] == AV_PIX_FMT_P010LE) {
                tenBit = true;
                break;
            }
        }
        if (tenBit) return std::string(name);
    }
    return std::nullopt;
}

bool tenBitHevcEncoderAvailable()
{
    return firstTenBitHevcEncoder().has_value();
}

} // namespace libavcore
