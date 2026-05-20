#pragma once

// ===========================================================================
// libavcore::Encode — Qt-friendly libav encoder helper.
//
// PRD-B campaign ④: factored out of Exporter.cpp so the same encode pipeline
// (open output -> find/open encoder (HW>SW fallback) -> write header ->
// scale+send_frame+write loop -> flush -> trailer -> free) can be reused by
// other call sites that currently spawn QProcess(ffmpeg.exe).
//
// Behavior is IDENTICAL to the original Exporter code path. No new policy.
//
// The first-class API (pushFrameRgb24) takes raw uint8_t* + stride so this
// header stays usable from non-Qt translation units; a QImage convenience
// wrapper is provided behind VEDITOR_LIBAVCORE_WITH_QIMAGE.
// ===========================================================================

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#ifdef VEDITOR_LIBAVCORE_WITH_QIMAGE
class QImage;
#endif

namespace libavcore {

// Request descriptor mirroring the relevant fields of Exporter's ExportConfig.
// All fields here are pure data (no Qt types) so the header can be included
// from non-Qt callers.
struct EncodeRequest {
    int width = 0;
    int height = 0;
    int fps = 30;                       // numerator; denominator is always 1
    int64_t videoBitrateBits = 0;       // Video bitrate in bits per second (caller multiplies kbps×1000)
    std::string outputPath;             // UTF-8

    // Preferred video encoder name. If a HW encoder is requested and fails to
    // open, FrameEncoder falls back to libx264/libx265 automatically.
    std::string videoCodecName;         // "libx264", "h264_nvenc", "libx265", "libsvtav1", "libvpx-vp9", "prores_ks", ...

    // HW selection hint mirroring ExportConfig::hwEncoder ("", "auto", "nvenc",
    // "qsv", "amf", "none"). Empty string means software-only.
    std::string hwVendorHint;
    bool useHardwareAccel = false;

    // HDR / wide-gamut switches mirroring ExportConfig.
    bool isHdr10 = false;
    bool isHlg = false;

    // ProRes profile index (-1 = not ProRes, 0..5 = prores_ks profile id).
    int proresProfile = -1;

    // HDR10 mastering metadata (used by x265 only when isHdr10 is true).
    double hdrMasterMaxNits = 1000.0;
    double hdrMasterMinNits = 0.005;
    int hdrMaxCll = 1000;
    int hdrMaxFall = 400;

    // [P1-M1] Optional runtime probe for HW encoder availability. When set,
    // openEncoderWithFallback() consults this hook for each HW candidate name
    // BEFORE avcodec_find_encoder_by_name(); a hook returning false skips the
    // candidate. Default (nullptr) treats every candidate as available
    // (legacy behavior of pure avcodec_find_encoder_by_name probe). Used by
    // Exporter to restore CodecDetector::isEncoderAvailable() functional-probe
    // semantics that were dropped during the libavcore refactor. Kept as
    // std::function (not Qt callable) so the header stays Qt-free.
    std::function<bool(const std::string&)> encoderAvailableHook;
};

// RAII encoder session. Construct, then call open(), then pushFrameRgb24()
// repeatedly with monotonically-increasing pts, then finalize() once.
// Errors are reported via std::optional<std::string> error messages and
// internal state is left in a safe "no-op" condition on failure.
class FrameEncoder {
public:
    FrameEncoder();
    ~FrameEncoder();

    FrameEncoder(const FrameEncoder&) = delete;
    FrameEncoder& operator=(const FrameEncoder&) = delete;

    // Allocate output context, find encoder (with HW>SW fallback), open file
    // and write the header. Returns std::nullopt on success, or an error
    // message string on failure.
    std::optional<std::string> open(const EncodeRequest& request);

    // Push one frame of packed RGB24 pixels. width/height must match
    // EncodeRequest. stride is bytes per row in src. pts is the output stream
    // pts (in time_base units of 1/fps). Returns false on send/encode error.
    bool pushFrameRgb24(const uint8_t* src, int stride, int64_t pts);

#ifdef VEDITOR_LIBAVCORE_WITH_QIMAGE
    // Convenience wrapper: pushes a QImage. The image is converted to
    // Format_RGB888 internally if needed. Returns false on failure.
    bool pushFrame(const QImage& image, int64_t pts);
#endif

    // Push one already-formatted AVFrame matching the encoder pix_fmt
    // (caller is responsible for filling YUV planes). Used by Exporter for
    // the existing decode->sws->encode path which already produces an
    // outFrame in the encoder's pixel format.
    bool pushFrameNative(AVFrame* frame, int64_t pts);

    // Flush remaining packets and write trailer. Safe to call once.
    // Returns std::nullopt on success or an error message on failure.
    std::optional<std::string> finalize();

    // True if open() succeeded and finalize() hasn't been called yet.
    bool isOpen() const { return m_opened && !m_finalized; }

    // Name of the encoder that ended up being used (may differ from the
    // requested name due to HW>SW fallback).
    std::string activeEncoderName() const { return m_activeEncoderName; }

    // Pixel format chosen for the output stream.
    AVPixelFormat outputPixelFormat() const { return m_pixFmt; }

    // Direct access to encoder time_base for caller pts computation.
    AVRational encoderTimeBase() const;

private:
    void releaseAll();
    bool openEncoderWithFallback(const EncodeRequest& request);
    bool configureEncoderContext(const EncodeRequest& request,
                                  AVDictionary** outOpts);

    AVFormatContext* m_outFmt = nullptr;
    AVCodecContext* m_encCtx = nullptr;
    AVStream* m_outStream = nullptr;
    SwsContext* m_rgbToYuvCtx = nullptr;   // used by pushFrameRgb24
    AVFrame* m_scratchFrame = nullptr;     // pre-allocated YUV frame for RGB path
    AVPixelFormat m_pixFmt = AV_PIX_FMT_YUV420P;
    bool m_opened = false;
    bool m_finalized = false;
    bool m_hwInUse = false;
    std::string m_activeEncoderName;
};

} // namespace libavcore
