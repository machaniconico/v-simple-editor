#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include <QString>
#include <QVector>
#include <QPair>

extern "C" {
#include <libavcodec/avcodec.h>
}

struct CodecOption {
    QString name;       // Display name
    QString ffmpegName; // FFmpeg encoder name
    bool available;
    int quality;        // 1-5 stars
};

namespace codecdetector_detail {

// Pick an available software AV1 encoder without changing codec families.
QString selectAv1SoftwareEncoder(
    const std::function<bool(const QString &)> &isAvailable);

// Process-local availability cache. Hardware wrappers (amf / nvenc / qsv) are
// opened once via a real avcodec_open2 probe so a codec compiled into FFmpeg
// is never presented as usable when its GPU runtime is absent.
class AvailabilityCache
{
public:
    using Finder = std::function<const AVCodec *(const char *)>;
    using HardwareOpener = std::function<bool(const AVCodec *)>;

    AvailabilityCache(Finder finder, HardwareOpener hardwareOpener);
    bool isAvailable(const QString &name);

private:
    Finder m_finder;
    HardwareOpener m_hardwareOpener;
    std::mutex m_mutex;
    std::unordered_map<std::string, bool> m_hardwareResults;
};

} // namespace codecdetector_detail

class CodecDetector
{
public:
    static bool isEncoderAvailable(const QString &name);
    // Registration + advertised planar/P010 10-bit support, with a real P010
    // Main10 open for AMF/NVENC/QSV so HDR routing does not trust a missing
    // vendor runtime.
    static bool isTenBitEncoderAvailable(const QString &name);

    static QVector<CodecOption> availableVideoEncoders();
    static QVector<CodecOption> availableAudioEncoders();

    static QString bestAACEncoder();
    static QString bestSoftwareAv1Encoder();
    static QString bestVideoEncoder(const QString &codecFamily);

    static QVector<CodecOption> hwAccelVideoEncoders();
};
