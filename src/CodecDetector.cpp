#include "CodecDetector.h"

#include <algorithm>
#include <cstring>
#include <utility>

extern "C" {
#include <libavutil/log.h>
}

namespace {

bool isRuntimeProbedHardwareWrapper(const AVCodec *codec)
{
    if (!codec || codec->type != AVMEDIA_TYPE_VIDEO || !codec->wrapper_name)
        return false;
    return std::strcmp(codec->wrapper_name, "amf") == 0
        || std::strcmp(codec->wrapper_name, "nvenc") == 0
        || std::strcmp(codec->wrapper_name, "qsv") == 0;
}

bool supportsInputPixelFormat(const AVCodec *codec,
                              AVPixelFormat requiredFormat)
{
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void *config = nullptr;
    int count = 0;
    const int rc = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &config, &count);
    if (rc < 0 || !config || count <= 0)
        return true; // Let avcodec_open2 decide for dynamic capability lists.

    const auto *formats = static_cast<const AVPixelFormat *>(config);
    for (int i = 0; i < count; ++i) {
        if (formats[i] == requiredFormat)
            return true;
    }
    return false;
#else
    // FFmpeg 6 / libavcodec 60 predates avcodec_get_supported_config().
    // Its public AVCodec::pix_fmts list carries the same static information.
    if (!codec || !codec->pix_fmts)
        return true;
    for (const AVPixelFormat *format = codec->pix_fmts;
         *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == requiredFormat)
            return true;
    }
    return false;
#endif
}

bool openHardwareEncoderProbe(const AVCodec *codec,
                              AVPixelFormat pixelFormat)
{
    if (!supportsInputPixelFormat(codec, pixelFormat))
        return false;

    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context)
        return false;

    context->width = 1280;
    context->height = 720;
    context->time_base = AVRational{1, 30};
    context->framerate = AVRational{30, 1};
    context->pix_fmt = pixelFormat;
    context->bit_rate = 4'000'000;
    context->gop_size = 60;
    context->max_b_frames = 0;
    context->thread_count = 1;
    if (pixelFormat == AV_PIX_FMT_P010LE && codec->id == AV_CODEC_ID_HEVC)
        context->profile = AV_PROFILE_HEVC_MAIN_10;
    // Missing vendor runtimes are an expected availability result. Reduce
    // messages owned by this context; some wrappers log runtime-loader
    // diagnostics globally and may still emit a concise failure line.
    context->log_level_offset = AV_LOG_MAX_OFFSET;

    const bool available = avcodec_open2(context, codec, nullptr) >= 0;
    avcodec_free_context(&context);
    return available;
}

} // namespace

namespace codecdetector_detail {

QString selectAv1SoftwareEncoder(
    const std::function<bool(const QString &)> &isAvailable)
{
    static const QString candidates[] = {
        QStringLiteral("libaom-av1"),
        QStringLiteral("libsvtav1"),
        QStringLiteral("librav1e"),
    };
    if (isAvailable) {
        for (const auto &candidate : candidates) {
            if (isAvailable(candidate))
                return candidate;
        }
    }
    return candidates[0];
}

AvailabilityCache::AvailabilityCache(Finder finder,
                                     HardwareOpener hardwareOpener)
    : m_finder(std::move(finder))
    , m_hardwareOpener(std::move(hardwareOpener))
{
}

bool AvailabilityCache::isAvailable(const QString &name)
{
    if (!m_finder)
        return false;

    const QByteArray utf8 = name.toUtf8();
    const std::string key(utf8.constData(), static_cast<size_t>(utf8.size()));
    const AVCodec *codec = m_finder(key.c_str());
    if (!codec)
        return false;
    if (!isRuntimeProbedHardwareWrapper(codec))
        return true;

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto cached = m_hardwareResults.find(key);
    if (cached != m_hardwareResults.end())
        return cached->second;

    const bool available = m_hardwareOpener
        && m_hardwareOpener(codec);
    m_hardwareResults.emplace(key, available);
    return available;
}

} // namespace codecdetector_detail

bool CodecDetector::isEncoderAvailable(const QString &name)
{
    static codecdetector_detail::AvailabilityCache cache(
        [](const char *encoderName) {
            return avcodec_find_encoder_by_name(encoderName);
        },
        [](const AVCodec *codec) {
            return openHardwareEncoderProbe(codec, AV_PIX_FMT_NV12);
        });
    return cache.isAvailable(name);
}

bool CodecDetector::isTenBitEncoderAvailable(const QString &name)
{
    const QByteArray utf8 = name.toUtf8();
    const AVCodec *codec = avcodec_find_encoder_by_name(utf8.constData());
    if (!codec)
        return false;
    const bool supportsP010 =
        supportsInputPixelFormat(codec, AV_PIX_FMT_P010LE);
    const bool supportsPlanar10 =
        supportsInputPixelFormat(codec, AV_PIX_FMT_YUV420P10LE);
    if (!isRuntimeProbedHardwareWrapper(codec))
        return supportsP010 || supportsPlanar10;
    if (!supportsP010)
        return false;

    static codecdetector_detail::AvailabilityCache cache(
        [](const char *encoderName) {
            return avcodec_find_encoder_by_name(encoderName);
        },
        [](const AVCodec *hardwareCodec) {
            return openHardwareEncoderProbe(
                hardwareCodec, AV_PIX_FMT_P010LE);
        });
    return cache.isAvailable(name);
}

QVector<CodecOption> CodecDetector::availableVideoEncoders()
{
    QVector<CodecOption> encoders = {
        {"H.264 (x264)",       "libx264",      false, 4},
        {"H.264 NVENC",        "h264_nvenc",    false, 4},
        {"H.264 QSV",          "h264_qsv",     false, 4},
        {"H.264 AMF",          "h264_amf",      false, 4},
        {"H.265 (x265)",       "libx265",      false, 4},
        {"H.265 NVENC",        "hevc_nvenc",    false, 4},
        {"H.265 QSV",          "hevc_qsv",     false, 4},
        {"H.265 AMF",          "hevc_amf",      false, 4},
        {"AV1 (AOM)",          "libaom-av1",    false, 5},
        {"AV1 (SVT-AV1)",      "libsvtav1",    false, 5},
        {"AV1 (rav1e)",        "librav1e",     false, 5},
        {"AV1 NVENC",          "av1_nvenc",     false, 5},
        {"AV1 QSV",            "av1_qsv",      false, 5},
        {"AV1 AMF",            "av1_amf",       false, 5},
        {"VP9",                "libvpx-vp9",    false, 4},
        {"ProRes (Kostya)",    "prores_ks",     false, 5},
        {"ProRes (Anatoly)",   "prores_aw",     false, 4},
    };

    for (auto &enc : encoders)
        enc.available = isEncoderAvailable(enc.ffmpegName);

    return encoders;
}

QVector<CodecOption> CodecDetector::availableAudioEncoders()
{
    QVector<CodecOption> encoders = {
        {"AAC (FDK - Best)",     "libfdk_aac",  false, 5},
        {"AAC (CoreAudio)",      "aac_at",      false, 5},
        {"AAC (FFmpeg built-in)","aac",         false, 3},
        {"Opus",                 "libopus",     false, 5},
        {"MP3 (LAME)",           "libmp3lame",  false, 3},
        {"ALAC (Apple Lossless)","alac",        false, 5},
        {"FLAC (Lossless)",      "flac",        false, 5},
        {"PCM 16-bit (for ProRes MOV)", "pcm_s16le", false, 5},
        {"PCM 24-bit (for ProRes MOV)", "pcm_s24le", false, 5},
    };

    for (auto &enc : encoders)
        enc.available = isEncoderAvailable(enc.ffmpegName);

    return encoders;
}

QString CodecDetector::bestAACEncoder()
{
    // Priority: libfdk_aac > aac_at (macOS/iTunes) > aac (built-in)
    if (isEncoderAvailable("libfdk_aac")) return "libfdk_aac";
    if (isEncoderAvailable("aac_at"))     return "aac_at";
    return "aac";
}

QString CodecDetector::bestSoftwareAv1Encoder()
{
    return codecdetector_detail::selectAv1SoftwareEncoder(
        [](const QString &name) { return isEncoderAvailable(name); });
}

QString CodecDetector::bestVideoEncoder(const QString &codecFamily)
{
    if (codecFamily == "h264") {
        // Try HW first, then software
        if (isEncoderAvailable("h264_nvenc")) return "h264_nvenc";
        if (isEncoderAvailable("h264_qsv"))   return "h264_qsv";
        if (isEncoderAvailable("h264_amf"))   return "h264_amf";
        return "libx264";
    }
    if (codecFamily == "h265" || codecFamily == "hevc") {
        if (isEncoderAvailable("hevc_nvenc")) return "hevc_nvenc";
        if (isEncoderAvailable("hevc_qsv"))   return "hevc_qsv";
        if (isEncoderAvailable("hevc_amf"))   return "hevc_amf";
        return "libx265";
    }
    if (codecFamily == "av1") {
        if (isEncoderAvailable("av1_nvenc"))  return "av1_nvenc";
        if (isEncoderAvailable("av1_qsv"))    return "av1_qsv";
        if (isEncoderAvailable("av1_amf"))    return "av1_amf";
        return bestSoftwareAv1Encoder();
    }
    if (codecFamily == "vp9") {
        return "libvpx-vp9";
    }
    return "libx264";
}

QVector<CodecOption> CodecDetector::hwAccelVideoEncoders()
{
    QVector<CodecOption> hw;
    auto all = availableVideoEncoders();
    for (const auto &enc : all) {
        if (enc.available && (enc.ffmpegName.contains("nvenc") ||
            enc.ffmpegName.contains("qsv") || enc.ffmpegName.contains("amf"))) {
            hw.append(enc);
        }
    }
    return hw;
}
