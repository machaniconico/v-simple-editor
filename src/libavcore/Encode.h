#pragma once

// ===========================================================================
// libavcore::Encode — Qt-friendly libav encoder helper.
//
// PRD-B campaign ④: factored out of Exporter.cpp so the same encode pipeline
// (open output -> find/open encoder (ordered fallback) -> write header ->
// scale+send_frame+write loop -> flush -> trailer -> free) can be reused by
// other call sites that currently spawn QProcess(ffmpeg.exe).
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
#include <libavutil/audio_fifo.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#ifdef VEDITOR_LIBAVCORE_WITH_QIMAGE
class QImage;
#endif

namespace libavcore {

// Generate an x265 'master-display' parameter string for HDR10 exports.
// Chromaticity coordinates (G/B/R/WP) are fixed to P3-D65-in-BT2020 primaries
// (x265 units: 1/50000 of CIE xy). Luminance terms L(max,min) are derived from
// masterMaxNits and masterMinNits (cd/m^2) converted to x265 units (nits × 10000,
// rounded to nearest integer).
// Example: hdr10MasterDisplayString(1000.0, 0.005) →
//   "G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(10000000,50)"
std::string hdr10MasterDisplayString(double masterMaxNits, double masterMinNits);

// Request descriptor mirroring the relevant fields of Exporter's ExportConfig.
// All fields here are pure data (no Qt types) so the header can be included
// from non-Qt callers.
struct EncodeRequest {
    int width = 0;
    int height = 0;
    int fps = 30;                       // Legacy integral fps fallback.
    int fpsNum = 0;                     // Optional rational fps numerator.
    int fpsDen = 0;                     // Optional rational fps denominator.
    int64_t videoBitrateBits = 0;       // Video bitrate in bits per second (caller multiplies kbps×1000)
    std::string outputPath;             // UTF-8
    std::string audioSourcePath;        // Optional UTF-8 source for stream-copy audio muxing
    // Re-encode caller-pushed audio frames as AAC. Takes priority over
    // audioSourcePath passthrough when both are set.
    bool audioEncode = false;
    int audioSampleRate = 48000;
    int audioChannels = 2;
    int64_t audioBitrateBits = 192000;

    // Preferred video encoder name. If an H.264/H.265 encoder is unavailable
    // or fails to open, FrameEncoder walks its ordered fallback chain.
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

    // [P1-M1] Optional runtime probe for encoder availability. When set,
    // openEncoderWithFallback() consults this hook for each SW/MF/HW/fallback
    // candidate name BEFORE avcodec_find_encoder_by_name(); a hook returning
    // false skips the candidate. Default (nullptr) treats every candidate as
    // available (legacy behavior of pure avcodec_find_encoder_by_name probe).
    // Used by Exporter to restore CodecDetector::isEncoderAvailable()
    // functional-probe semantics that were dropped during the libavcore
    // refactor. Kept as std::function (not Qt callable) so the header stays
    // Qt-free.
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

    // Allocate output context, find encoder (with ordered fallback), open file
    // and write the header. Returns std::nullopt on success, or an error
    // message string on failure.
    std::optional<std::string> open(const EncodeRequest& request);

    // Push one frame of packed RGB24 pixels. width/height must match
    // EncodeRequest. stride is bytes per row in src. pts increments once per
    // output frame and is interpreted in encoderTimeBase().
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

    // Push one audio frame for AAC encode mode. Valid only when
    // EncodeRequest::audioEncode is true. The caller must provide frames that
    // already match the encoder sample_fmt, sample_rate, and ch_layout
    // selected from audioSampleRate/audioChannels (perform swresample outside
    // FrameEncoder). pts is in audio encoder time_base units (1/sample_rate).
    bool pushAudioFrame(AVFrame* frame, int64_t pts);

    // Flush remaining packets and write trailer. Safe to call once.
    // Returns std::nullopt on success or an error message on failure.
    std::optional<std::string> finalize();

    // True if open() succeeded and finalize() hasn't been called yet.
    bool isOpen() const { return m_opened && !m_finalized; }

    // Name of the encoder that ended up being used (may differ from the
    // requested name due to fallback).
    std::string activeEncoderName() const { return m_activeEncoderName; }

    // Pixel format chosen for the output stream.
    AVPixelFormat outputPixelFormat() const { return m_pixFmt; }

    // Direct access to encoder time_base for caller pts computation.
    AVRational encoderTimeBase() const;

private:
    void releaseAll();
    bool openEncoderWithFallback(const EncodeRequest& request);
    bool configureEncoderContext(const EncodeRequest& request,
                                  const AVCodec* encoder,
                                  AVDictionary** outOpts);
    bool configureAudioPassthrough(const std::string& audioSourcePath);
    void muxAudioPassthroughPackets();
    bool configureAudioEncoder(const EncodeRequest& request);
    bool encodeAudioFifoSamples(int nbSamples);
    bool sendAudioEncoderFrame(AVFrame* frame);
    bool drainAudioEncoderPackets();
    bool flushAudioEncoder();

    // Attach HDR10 mastering-display + content-light side data to `frame`
    // (no-op unless m_attachHdr10Metadata is set). Idempotent per frame.
    void attachHdr10SideData(AVFrame* frame);

    AVFormatContext* m_outFmt = nullptr;
    AVFormatContext* m_audioInFmt = nullptr;
    AVCodecContext* m_encCtx = nullptr;
    AVCodecContext* m_audioEncCtx = nullptr;
    AVStream* m_outStream = nullptr;
    AVStream* m_audioInStream = nullptr;
    AVStream* m_audioOutStream = nullptr;
    AVAudioFifo* m_audioFifo = nullptr;
    SwsContext* m_rgbToYuvCtx = nullptr;   // used by pushFrameRgb24
    AVFrame* m_scratchFrame = nullptr;     // pre-allocated YUV frame for RGB path
    AVFrame* m_audioScratchFrame = nullptr;
    AVPixelFormat m_pixFmt = AV_PIX_FMT_YUV420P;
    int m_audioInStreamIndex = -1;
    int m_fpsNum = 30;                     // effective fps numerator captured in open()
    int m_fpsDen = 1;                      // effective fps denominator captured in open()
    int64_t m_audioNextPts = 0;
    bool m_audioPtsInitialized = false;
    bool m_audioEncode = false;

    // HDR10 static metadata captured in open(). When m_attachHdr10Metadata is
    // true (HDR10 export through a non-libx265 encoder, e.g. hevc_mf), every
    // pushed frame carries mastering-display + content-light side data so the
    // encoder writes the SMPTE-2086 / CEA-861.3 SEI HDR10 requires. libx265
    // gets the same data via its x265-params string instead.
    bool m_attachHdr10Metadata = false;
    double m_hdrMasterMaxNits = 1000.0;
    double m_hdrMasterMinNits = 0.0001;
    int m_hdrMaxCll = 1000;
    int m_hdrMaxFall = 400;
    int64_t m_videoFramesPushed = 0;       // [US-MF-8] count of frames successfully sent to the video encoder
    bool m_opened = false;
    bool m_finalized = false;
    bool m_hwInUse = false;
    std::string m_activeEncoderName;
};

} // namespace libavcore
