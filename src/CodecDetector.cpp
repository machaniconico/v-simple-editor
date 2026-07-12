#include "CodecDetector.h"

#include <algorithm>

bool CodecDetector::isEncoderAvailable(const QString &name)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name.toUtf8().constData());
    return codec != nullptr;
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
        {"AV1 (SVT-AV1)",      "libsvtav1",    false, 5},
        {"AV1 NVENC",          "av1_nvenc",     false, 5},
        {"AV1 QSV",            "av1_qsv",      false, 5},
        {"AV1 AMF",            "av1_amf",       false, 5},
        {"VP9",                "libvpx-vp9",    false, 4},
        {"ProRes (Kostya)",    "prores_ks",     false, 5},
        {"ProRes (Anatoly)",   "prores_aw",     false, 4},
    };

    for (auto &enc : encoders)
        enc.available = isEncoderAvailable(enc.ffmpegName);
    encoders.erase(std::remove_if(encoders.begin(), encoders.end(),
                                  [](const CodecOption &enc) {
                                      return enc.ffmpegName.contains("av1")
                                             && !enc.available;
                                  }),
                   encoders.end());

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
        if (isEncoderAvailable("libsvtav1"))  return "libsvtav1";
        return bestVideoEncoder("h264");
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
