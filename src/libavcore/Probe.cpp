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

bool supportsTenBitHevcInput(const AVCodec* codec)
{
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void* cfgList = nullptr;
    int count = 0;
    const int ret = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfgList, &count);
    if (ret < 0 || cfgList == nullptr || count <= 0)
        return false;

    const auto* formats = static_cast<const AVPixelFormat*>(cfgList);
    for (int i = 0; i < count; ++i) {
        if (formats[i] == AV_PIX_FMT_YUV420P10LE
            || formats[i] == AV_PIX_FMT_P010LE) {
            return true;
        }
    }
#else
    if (!codec || !codec->pix_fmts)
        return false;
    for (const AVPixelFormat* format = codec->pix_fmts;
         *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_YUV420P10LE
            || *format == AV_PIX_FMT_P010LE) {
            return true;
        }
    }
#endif
    return false;
}

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

std::optional<std::string> firstTenBitHevcEncoder(
    const std::vector<std::string>& candidates,
    const std::function<bool(const std::string&)>& runtimeAvailable)
{
    for (const std::string& name : candidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) continue;

        if (supportsTenBitHevcInput(codec)) {
            if (!runtimeAvailable || runtimeAvailable(name))
                return name;
        }
    }
    return std::nullopt;
}

std::vector<std::string> tenBitHevcEncoderCandidateNames(
    bool useHardwareAccel,
    const std::string& hwVendorHint)
{
    std::vector<std::string> candidates;
    if (useHardwareAccel && hwVendorHint != "none") {
        if (hwVendorHint == "nvenc") {
            candidates.emplace_back("hevc_nvenc");
        } else if (hwVendorHint == "qsv") {
            candidates.emplace_back("hevc_qsv");
        } else if (hwVendorHint == "amf") {
            candidates.emplace_back("hevc_amf");
        } else {
            candidates.emplace_back("hevc_nvenc");
            candidates.emplace_back("hevc_qsv");
            candidates.emplace_back("hevc_amf");
        }
    }
    candidates.emplace_back("libx265");
    return candidates;
}

std::optional<std::string> firstTenBitHevcEncoder(
    const std::function<bool(const std::string&)>& runtimeAvailable)
{
    static const std::vector<std::string> kCandidates = {
        "libx265", "hevc_nvenc", "hevc_qsv", "hevc_amf"
    };
    return firstTenBitHevcEncoder(kCandidates, runtimeAvailable);
}

std::optional<std::string> firstTenBitHevcEncoder()
{
    return firstTenBitHevcEncoder({});
}

bool tenBitHevcEncoderAvailable()
{
    return firstTenBitHevcEncoder().has_value();
}

} // namespace libavcore
