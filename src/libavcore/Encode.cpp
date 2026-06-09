#include "Encode.h"

#include "color/SwsColorParams.h"
#include "playback/swsmatrix_flag.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef VEDITOR_LIBAVCORE_WITH_QIMAGE
#include <QImage>
#endif

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

namespace libavcore {

// P3-D65-in-BT2020 mastering display chromaticity coordinates (x265 units,
// i.e. 1/50000 of CIE xy). These are the SSOT for both hdr10MasterDisplayString
// (public API) and buildX265Hdr10Params (internal x265-params builder).
// Values match SMPTE ST 2086 / ITU-R BT.2100 Display Primaries P3-D65.
static constexpr int kHdr10GreenX  =  8500;
static constexpr int kHdr10GreenY  = 39850;
static constexpr int kHdr10BlueX   =  6550;
static constexpr int kHdr10BlueY   =  2300;
static constexpr int kHdr10RedX    = 35400;
static constexpr int kHdr10RedY    = 14600;
static constexpr int kHdr10WpX     = 15635;
static constexpr int kHdr10WpY     = 16450;

std::string hdr10MasterDisplayString(double masterMaxNits, double masterMinNits)
{
    const int maxLumX10000 = static_cast<int>(std::round(masterMaxNits * 10000.0));
    const int minLumX10000 = static_cast<int>(std::round(masterMinNits * 10000.0));
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "G(%d,%d)B(%d,%d)R(%d,%d)WP(%d,%d)L(%d,%d)",
                  kHdr10GreenX, kHdr10GreenY,
                  kHdr10BlueX,  kHdr10BlueY,
                  kHdr10RedX,   kHdr10RedY,
                  kHdr10WpX,    kHdr10WpY,
                  maxLumX10000, minLumX10000);
    return std::string(buf);
}

namespace {

// Build x265-params HDR10 metadata string (BT.2020 primaries / D65). Identical
// to Exporter.cpp's static helper but Qt-free (no QByteArray).
static std::string buildX265Hdr10Params(double maxNits, double minNits,
                                         int maxCll, int maxFall)
{
    const std::string masterDisplay =
        hdr10MasterDisplayString(maxNits, minNits);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "hdr10=1:repeat-headers=1:colorprim=bt2020:"
                  "transfer=smpte2084:colormatrix=bt2020nc:range=limited:"
                  "master-display=%s:max-cll=%d,%d",
                  masterDisplay.c_str(), maxCll, maxFall);
    return std::string(buf);
}

static bool nameContains(const std::string& s, const char* needle)
{
    return s.find(needle) != std::string::npos;
}

static std::string ffmpegErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, buf, sizeof(buf)) < 0) {
        return std::to_string(err);
    }
    return std::string(buf);
}

static AVRational effectiveFpsRational(const EncodeRequest& req)
{
    if (req.fpsNum > 0 && req.fpsDen > 0) {
        return AVRational{req.fpsNum, req.fpsDen};
    }
    return AVRational{req.fps, 1};
}

static void logAudioPassthroughDisabled(const std::string& path,
                                        const std::string& reason)
{
    std::fprintf(stderr,
                 "FrameEncoder: audio passthrough disabled for '%s': %s\n",
                 path.c_str(),
                 reason.c_str());
}

enum class EncoderFamily {
    Unknown,
    H264,
    H265,
    AV1,
    VP9
};

static EncoderFamily detectEncoderFamily(const std::string& name)
{
    if (name == "libx264" || nameContains(name, "264")) {
        return EncoderFamily::H264;
    }
    if (name == "libx265" || nameContains(name, "265") || nameContains(name, "hevc")) {
        return EncoderFamily::H265;
    }
    if (nameContains(name, "av1")) {
        return EncoderFamily::AV1;
    }
    if (nameContains(name, "vp9")) {
        return EncoderFamily::VP9;
    }
    return EncoderFamily::Unknown;
}

static const char* primarySoftwareEncoderName(EncoderFamily family)
{
    switch (family) {
    case EncoderFamily::H264: return "libx264";
    case EncoderFamily::H265: return "libx265";
    case EncoderFamily::AV1: return "libsvtav1";
    case EncoderFamily::VP9: return "libvpx-vp9";
    case EncoderFamily::Unknown: break;
    }
    return "libx264";
}

static bool isH264OrH265Family(EncoderFamily family)
{
    return family == EncoderFamily::H264 || family == EncoderFamily::H265;
}

static void appendUnique(std::vector<std::string>& names, const char* name)
{
    if (!name || !*name) return;
    for (const std::string& existing : names) {
        if (existing == name) return;
    }
    names.emplace_back(name);
}

static void appendHardwareCandidates(std::vector<std::string>& names,
                                     EncoderFamily family,
                                     const std::string& vendor)
{
    if (family == EncoderFamily::H264) {
        if (vendor == "nvenc") { appendUnique(names, "h264_nvenc"); return; }
        if (vendor == "qsv") { appendUnique(names, "h264_qsv"); return; }
        if (vendor == "amf") { appendUnique(names, "h264_amf"); return; }
        appendUnique(names, "h264_nvenc");
        appendUnique(names, "h264_qsv");
        appendUnique(names, "h264_amf");
    } else if (family == EncoderFamily::H265) {
        if (vendor == "nvenc") { appendUnique(names, "hevc_nvenc"); return; }
        if (vendor == "qsv") { appendUnique(names, "hevc_qsv"); return; }
        if (vendor == "amf") { appendUnique(names, "hevc_amf"); return; }
        appendUnique(names, "hevc_nvenc");
        appendUnique(names, "hevc_qsv");
        appendUnique(names, "hevc_amf");
    }
}

static void appendOrderedFamilyFallbacks(std::vector<std::string>& names,
                                         EncoderFamily family)
{
    if (family == EncoderFamily::H264) {
        appendUnique(names, "libx264");
        appendUnique(names, "h264_mf");
        appendUnique(names, "h264_nvenc");
        appendUnique(names, "h264_qsv");
        appendUnique(names, "h264_amf");
        appendUnique(names, "mpeg4");
    } else if (family == EncoderFamily::H265) {
        appendUnique(names, "libx265");
        appendUnique(names, "hevc_mf");
        appendUnique(names, "hevc_nvenc");
        appendUnique(names, "hevc_qsv");
        appendUnique(names, "hevc_amf");
        appendUnique(names, "mpeg4");
    }
}

static bool isVendorHardwareEncoderName(const std::string& name)
{
    return name == "h264_nvenc" || name == "h264_qsv" || name == "h264_amf"
        || name == "hevc_nvenc" || name == "hevc_qsv" || name == "hevc_amf";
}

static bool encoderAllowedByHook(const EncodeRequest& req, const std::string& name)
{
    return !req.encoderAvailableHook || req.encoderAvailableHook(name);
}

static const AVCodec* findAllowedEncoderByName(const EncodeRequest& req,
                                               const std::string& name)
{
    if (name.empty()) return nullptr;
    if (!encoderAllowedByHook(req, name)) return nullptr;
    return avcodec_find_encoder_by_name(name.c_str());
}

// Query an encoder's supported pixel-format list. FFmpeg 8 deprecated the
// AVCodec::pix_fmts array (it is NULL for Media Foundation encoders such as
// h264_mf / hevc_mf, whose real capability is only known after the wrapped
// MFT is probed) in favour of avcodec_get_supported_config(). This helper
// prefers the new API and falls back to the legacy array only when the new
// API yields nothing, so a NULL pix_fmts no longer silently mis-validates.
//
// Returns a heap-allocated, AV_PIX_FMT_NONE-terminated list the caller must
// av_free(), or nullptr when the encoder advertises no constraint (any input
// format is then assumed acceptable — legacy behaviour).
static const AVPixelFormat* querySupportedPixelFormats(const AVCodec* encoder)
{
    if (!encoder) return nullptr;

    const void* cfg = nullptr;
    int numCfg = 0;
    const int rc = avcodec_get_supported_config(
        nullptr, encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &numCfg);
    if (rc >= 0 && cfg && numCfg > 0) {
        const AVPixelFormat* src = static_cast<const AVPixelFormat*>(cfg);
        AVPixelFormat* out = static_cast<AVPixelFormat*>(
            av_malloc(sizeof(AVPixelFormat) * (static_cast<size_t>(numCfg) + 1)));
        if (!out) return nullptr;
        for (int i = 0; i < numCfg; ++i) out[i] = src[i];
        out[numCfg] = AV_PIX_FMT_NONE;
        return out;
    }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    const AVPixelFormat* legacy = encoder->pix_fmts;
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (!legacy) return nullptr;

    int n = 0;
    while (legacy[n] != AV_PIX_FMT_NONE) ++n;
    AVPixelFormat* out = static_cast<AVPixelFormat*>(
        av_malloc(sizeof(AVPixelFormat) * (static_cast<size_t>(n) + 1)));
    if (!out) return nullptr;
    for (int i = 0; i <= n; ++i) out[i] = legacy[i];
    return out;
}

static AVSampleFormat firstSupportedAudioSampleFormat(const AVCodec* encoder)
{
    if (!encoder) return AV_SAMPLE_FMT_FLTP;

    const void* cfg = nullptr;
    int numCfg = 0;
    const int rc = avcodec_get_supported_config(
        nullptr, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &numCfg);
    if (rc >= 0 && cfg && numCfg > 0) {
        const AVSampleFormat* formats = static_cast<const AVSampleFormat*>(cfg);
        return formats[0];
    }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    const AVSampleFormat* legacy = encoder->sample_fmts;
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (legacy && legacy[0] != AV_SAMPLE_FMT_NONE) return legacy[0];
    return AV_SAMPLE_FMT_FLTP;
}

// True when `encoder` lists `target` among its supported pixel formats. When
// the encoder advertises no list at all the format is assumed acceptable
// (an MFT validates the real format at avcodec_open2 / send_frame time).
static bool encoderSupportsPixelFormat(const AVCodec* encoder,
                                       AVPixelFormat target)
{
    const AVPixelFormat* list = querySupportedPixelFormats(encoder);
    if (!list) return true;
    bool found = false;
    for (const AVPixelFormat* p = list; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == target) { found = true; break; }
    }
    av_free(const_cast<AVPixelFormat*>(list));
    return found;
}

// Resolve the 10-bit pixel format an HDR10/HLG encode should feed `encoder`.
// hevc_mf (Media Foundation HEVC) takes NV12-style semi-planar input, so its
// 10-bit format is P010LE — NOT the planar YUV420P10LE libx265 uses. Picking
// the wrong one makes avcodec_send_frame fail ("encoder frame push failed").
// The chosen format is verified against the encoder's real supported list;
// on a verified mismatch AV_PIX_FMT_NONE is returned so the caller can fall
// through to the next encoder candidate instead of failing the whole encode.
static AVPixelFormat tenBitPixelFormatForEncoder(const AVCodec* encoder,
                                                 const std::string& name)
{
    const bool isMediaFoundation =
        nameContains(name, "_mf");
    AVPixelFormat preferred =
        isMediaFoundation ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P10LE;

    if (encoderSupportsPixelFormat(encoder, preferred)) return preferred;

    // The preferred format was rejected. Try the other common 10-bit layout
    // before giving up (covers HW encoders that only list one of the two).
    const AVPixelFormat alternate =
        (preferred == AV_PIX_FMT_P010LE) ? AV_PIX_FMT_YUV420P10LE
                                         : AV_PIX_FMT_P010LE;
    if (encoderSupportsPixelFormat(encoder, alternate)) return alternate;

    // Neither 10-bit format is supported -> this encoder cannot do HDR10.
    //
    // EMPIRICAL NOTE (FFmpeg n8.0 / libavcodec 62.28, bundled avcodec-62.dll):
    // hevc_mf advertises exactly {nv12, yuv420p, d3d11} — all 8-bit — and
    // avcodec_open2() hard-rejects any 10-bit pix_fmt ("Specified pixel
    // format p010le is not supported by the hevc_mf encoder") inside
    // ff_encode_preinit, BEFORE the wrapped HEVC Encoder MFT is ever
    // initialised. The FFmpeg mfenc.c HEVC wrapper simply does not register
    // P010, so there is no in-process hevc_mf path to a 10-bit HDR10 stream
    // in this build. Returning NONE here lets openEncoderWithFallback advance
    // to the next candidate rather than silently downgrading HDR10 to 8-bit.
    return AV_PIX_FMT_NONE;
}

} // namespace

FrameEncoder::FrameEncoder() = default;

FrameEncoder::~FrameEncoder()
{
    if (m_opened && !m_finalized) {
        // Best-effort cleanup if the user forgot to call finalize().
        finalize();
    }
    releaseAll();
}

AVRational FrameEncoder::encoderTimeBase() const
{
    return m_encCtx ? m_encCtx->time_base : AVRational{0, 1};
}

void FrameEncoder::releaseAll()
{
    if (m_rgbToYuvCtx) { sws_freeContext(m_rgbToYuvCtx); m_rgbToYuvCtx = nullptr; }
    if (m_scratchFrame) { av_frame_free(&m_scratchFrame); }
    if (m_audioScratchFrame) { av_frame_free(&m_audioScratchFrame); }
    if (m_audioFifo) { av_audio_fifo_free(m_audioFifo); m_audioFifo = nullptr; }
    if (m_audioEncCtx) { avcodec_free_context(&m_audioEncCtx); }
    if (m_encCtx) { avcodec_free_context(&m_encCtx); }
    if (m_audioInFmt) { avformat_close_input(&m_audioInFmt); }
    if (m_outFmt) {
        if (!(m_outFmt->oformat->flags & AVFMT_NOFILE) && m_outFmt->pb) {
            avio_closep(&m_outFmt->pb);
        }
        avformat_free_context(m_outFmt);
        m_outFmt = nullptr;
    }
    m_outStream = nullptr;
    m_audioInStream = nullptr;
    m_audioOutStream = nullptr;
    m_audioInStreamIndex = -1;
    m_audioNextPts = 0;
    m_audioPtsInitialized = false;
    m_audioEncode = false;
}

bool FrameEncoder::configureEncoderContext(const EncodeRequest& req,
                                            const AVCodec* encoder,
                                            AVDictionary** outOpts)
{
    if (!m_encCtx) return false;

    m_encCtx->width = req.width;
    m_encCtx->height = req.height;
    const AVRational fps = effectiveFpsRational(req);
    m_encCtx->time_base = {fps.den, fps.num};
    m_encCtx->framerate = {fps.num, fps.den};

    // Decide pixel format mirroring Exporter.cpp policy.
    //
    // HDR10/HLG need a 10-bit format, but WHICH 10-bit format is encoder-
    // specific: libx265 takes planar YUV420P10LE while Media Foundation's
    // hevc_mf takes the NV12-style semi-planar P010LE. Feeding YUV420P10LE to
    // hevc_mf made avcodec_send_frame reject every frame ("encoder frame push
    // failed"). tenBitPixelFormatForEncoder() picks the right layout for the
    // resolved encoder and verifies it against the encoder's REAL supported
    // list (avcodec_get_supported_config, since FFmpeg 8 leaves the legacy
    // pix_fmts array NULL for MF encoders).
    const std::string& name = m_activeEncoderName;
    AVPixelFormat targetPixFmt = AV_PIX_FMT_YUV420P;
    if (req.proresProfile >= 4) {
        targetPixFmt = AV_PIX_FMT_YUVA444P10LE;
    } else if (req.proresProfile >= 0) {
        targetPixFmt = AV_PIX_FMT_YUV422P10LE;
    } else if (req.isHdr10 || req.isHlg) {
        targetPixFmt = tenBitPixelFormatForEncoder(encoder, name);
        if (targetPixFmt == AV_PIX_FMT_NONE) {
            // The resolved encoder cannot accept any 10-bit format. Refuse to
            // configure it so openEncoderWithFallback() advances to the next
            // candidate instead of silently downgrading HDR10 to 8-bit.
            std::fprintf(stderr,
                         "FrameEncoder: encoder '%s' supports no 10-bit pixel "
                         "format — cannot satisfy HDR10/HLG, trying next "
                         "candidate\n",
                         name.c_str());
            return false;
        }
    } else if (!encoderSupportsPixelFormat(encoder, targetPixFmt)) {
        // 8-bit path: keep the legacy "first supported format" fallback so an
        // encoder that does not list yuv420p still opens.
        const AVPixelFormat* list = querySupportedPixelFormats(encoder);
        if (list && list[0] != AV_PIX_FMT_NONE) targetPixFmt = list[0];
        av_free(const_cast<AVPixelFormat*>(list));
    }
    m_encCtx->pix_fmt = targetPixFmt;
    m_pixFmt = targetPixFmt;
    m_encCtx->bit_rate = req.videoBitrateBits;

    if (req.isHdr10) {
        m_encCtx->color_primaries = AVCOL_PRI_BT2020;
        m_encCtx->color_trc = AVCOL_TRC_SMPTE2084;
        m_encCtx->colorspace = AVCOL_SPC_BT2020_NCL;
        m_encCtx->color_range = AVCOL_RANGE_MPEG;
    } else if (req.isHlg) {
        m_encCtx->color_primaries = AVCOL_PRI_BT2020;
        m_encCtx->color_trc = AVCOL_TRC_ARIB_STD_B67;
        m_encCtx->colorspace = AVCOL_SPC_BT2020_NCL;
        m_encCtx->color_range = AVCOL_RANGE_MPEG;
    } else if (swscolor::matrixEnabledFromEnv()) {
        const swscolor::SdrTags t =
            swscolor::sdrTagsFor(m_encCtx->width, m_encCtx->height);
        m_encCtx->color_primaries = t.pri;
        m_encCtx->color_trc = t.trc;
        m_encCtx->colorspace = t.spc;
        m_encCtx->color_range = t.range;
    }

    // HEVC 10-bit output requires the Main10 profile. libx265 receives this
    // via its "profile" private option below; every other HEVC encoder
    // (hevc_mf, hevc_nvenc/qsv/amf) takes it through AVCodecContext::profile.
    if ((req.isHdr10 || req.isHlg)
        && detectEncoderFamily(name) == EncoderFamily::H265
        && name != "libx265") {
        m_encCtx->profile = AV_PROFILE_HEVC_MAIN_10;
    }

    // HDR10 static metadata. libx265 emits the SMPTE-2086 mastering display
    // and CEA-861.3 content-light SEI from its x265-params string (built
    // below). Every other encoder — notably hevc_mf — receives the same
    // information as per-frame side data instead; capture the values here so
    // pushFrameNative() can attach them. HLG carries no static luminance
    // metadata, so this is HDR10-only.
    if (req.isHdr10 && name != "libx265") {
        m_attachHdr10Metadata = true;
        m_hdrMasterMaxNits = req.hdrMasterMaxNits;
        m_hdrMasterMinNits = req.hdrMasterMinNits;
        m_hdrMaxCll = req.hdrMaxCll;
        m_hdrMaxFall = req.hdrMaxFall;
    }

    std::fprintf(stderr,
                 "FrameEncoder: encoder='%s' pix_fmt=%s hdr10=%d hlg=%d "
                 "profile=%d\n",
                 name.c_str(),
                 av_get_pix_fmt_name(targetPixFmt)
                     ? av_get_pix_fmt_name(targetPixFmt) : "?",
                 req.isHdr10 ? 1 : 0, req.isHlg ? 1 : 0,
                 m_encCtx->profile);

    if (m_outFmt && (m_outFmt->oformat->flags & AVFMT_GLOBALHEADER)) {
        m_encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Codec-specific opts — mirrors Exporter.cpp 1:1. `name` was bound to
    // m_activeEncoderName above for the pixel-format / profile decisions.
    if (name == "h264_nvenc" || name == "hevc_nvenc") {
        av_dict_set(outOpts, "preset", "p4", 0);
        av_dict_set(outOpts, "rc", "vbr", 0);
        av_dict_set(outOpts, "cq", "23", 0);
    } else if (name == "h264_qsv" || name == "hevc_qsv") {
        av_dict_set(outOpts, "preset", "medium", 0);
    } else if (name == "h264_amf" || name == "hevc_amf") {
        av_dict_set(outOpts, "quality", "balanced", 0);
    } else if (name == "libx264" || name == "libx265") {
        av_dict_set(outOpts, "preset", "medium", 0);
        av_dict_set(outOpts, "crf", "23", 0);
        if (name == "libx265") {
            if (req.isHdr10) {
                av_dict_set(outOpts, "profile", "main10", 0);
                const std::string x265p = buildX265Hdr10Params(
                    req.hdrMasterMaxNits, req.hdrMasterMinNits,
                    req.hdrMaxCll, req.hdrMaxFall);
                av_dict_set(outOpts, "x265-params", x265p.c_str(), 0);
            } else if (req.isHlg) {
                av_dict_set(outOpts, "profile", "main10", 0);
                av_dict_set(outOpts,
                            "x265-params",
                            "repeat-headers=1:colorprim=bt2020:"
                            "transfer=arib-std-b67:colormatrix=bt2020nc",
                            0);
            }
        }
    } else if (name == "h264_mf" || name == "hevc_mf") {
        // Windows Media Foundation H.264/HEVC. Without explicit rate control
        // mfenc defaults to a CBR-style mode that pads easy frames and starves
        // hard ones, raising decoded-vs-source MSE at a fixed bitrate.
        // u_vbr (unconstrained VBR) treats bit_rate as an average target and
        // lets the encoder reallocate bits toward harder frames; the archive
        // scenario is a fidelity-priority hint (vs latency-priority scenarios
        // like video_conference / live_streaming). Together they lower the
        // decoded-vs-source error at the same configured bitrate.
        av_dict_set(outOpts, "rate_control", "u_vbr", 0);
        av_dict_set(outOpts, "scenario", "archive", 0);
        av_dict_set(outOpts, "quality", "100", 0);
    } else if (name == "libsvtav1") {
        av_dict_set(outOpts, "preset", "8", 0);
        av_dict_set(outOpts, "crf", "30", 0);
    } else if (name == "libvpx-vp9") {
        av_dict_set(outOpts, "quality", "good", 0);
        av_dict_set(outOpts, "cpu-used", "4", 0);
    } else if (name.rfind("prores", 0) == 0 && req.proresProfile >= 0) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", req.proresProfile);
        av_dict_set(outOpts, "profile", buf, 0);
    }
    return true;
}

bool FrameEncoder::openEncoderWithFallback(const EncodeRequest& req)
{
    m_hwInUse = false;
    m_activeEncoderName.clear();

    // Find primary encoder by requested name; if missing, try CodecDetector-
    // style family detection (here inline so libavcore stays Qt-free).
    EncoderFamily family = detectEncoderFamily(req.videoCodecName);
    std::string resolved = req.videoCodecName;
    const AVCodec* primaryEncoder = findAllowedEncoderByName(req, resolved);

    if (!primaryEncoder) {
        if (family == EncoderFamily::Unknown) {
            family = EncoderFamily::H264;
        }
        resolved = primarySoftwareEncoderName(family);
        primaryEncoder = findAllowedEncoderByName(req, resolved);
    }

    const bool wantHW = (req.useHardwareAccel || !req.hwVendorHint.empty())
                       && req.hwVendorHint != "none";

    std::vector<std::string> candidates;
    if (wantHW && primaryEncoder && isH264OrH265Family(family)) {
        // Preserve the existing HW-preferred path when a primary encoder is
        // present, then continue through the new MF/mpeg4 fallback chain if
        // opening the selected encoder fails.
        appendHardwareCandidates(candidates, family, req.hwVendorHint);
        appendUnique(candidates, primaryEncoder->name ? primaryEncoder->name : resolved.c_str());
        appendOrderedFamilyFallbacks(candidates, family);
    } else if (primaryEncoder) {
        appendUnique(candidates, primaryEncoder->name ? primaryEncoder->name : resolved.c_str());
        if (isH264OrH265Family(family)) {
            appendOrderedFamilyFallbacks(candidates, family);
        }
    } else if (isH264OrH265Family(family)) {
        // A missing libx264/libx265 primary must not short-circuit the open.
        // Windows builds with Media Foundation can still supply h264_mf/hevc_mf,
        // and mpeg4 remains a last-resort cross-build fallback.
        appendOrderedFamilyFallbacks(candidates, family);
    }

    for (const std::string& candidateName : candidates) {
        const AVCodec* encoder = findAllowedEncoderByName(req, candidateName);
        if (!encoder) continue;

        m_encCtx = avcodec_alloc_context3(encoder);
        if (!m_encCtx) return false;
        m_activeEncoderName = encoder->name ? encoder->name : candidateName;

        // Reset per-candidate HDR10 state so a rejected candidate cannot leak
        // its metadata flag into the next one.
        m_attachHdr10Metadata = false;

        AVDictionary* opts = nullptr;
        if (!configureEncoderContext(req, encoder, &opts)) {
            // configureEncoderContext now fails (rather than silently
            // downgrading) when an encoder cannot satisfy the request — e.g.
            // an HDR10 export landing on the 8-bit-only h264_mf. Skip this
            // candidate and continue the fallback chain instead of aborting
            // the whole open, so a 10-bit-capable encoder further down the
            // list (hevc_mf, hevc_nvenc, …) still gets its turn.
            av_dict_free(&opts);
            avcodec_free_context(&m_encCtx);
            m_encCtx = nullptr;
            m_activeEncoderName.clear();
            m_attachHdr10Metadata = false;
            continue;
        }

        const int rc = avcodec_open2(m_encCtx, encoder, &opts);
        av_dict_free(&opts);
        if (rc >= 0) {
            m_hwInUse = isVendorHardwareEncoderName(m_activeEncoderName);
            return true;
        }

        avcodec_free_context(&m_encCtx);
        m_encCtx = nullptr;
        m_activeEncoderName.clear();
        m_attachHdr10Metadata = false;
        m_hwInUse = false;
    }

    return false;
}

bool FrameEncoder::configureAudioPassthrough(const std::string& audioSourcePath)
{
    if (audioSourcePath.empty()) return false;
    if (!m_outFmt) {
        logAudioPassthroughDisabled(audioSourcePath, "output context is not ready");
        return false;
    }

    AVFormatContext* inFmt = nullptr;
    int rc = avformat_open_input(&inFmt, audioSourcePath.c_str(), nullptr, nullptr);
    if (rc < 0) {
        logAudioPassthroughDisabled(audioSourcePath,
                                    "open failed: " + ffmpegErrorString(rc));
        return false;
    }

    rc = avformat_find_stream_info(inFmt, nullptr);
    if (rc < 0) {
        logAudioPassthroughDisabled(audioSourcePath,
                                    "stream info parse failed: " + ffmpegErrorString(rc));
        avformat_close_input(&inFmt);
        return false;
    }

    int streamIndex = -1;
    for (unsigned int i = 0; i < inFmt->nb_streams; ++i) {
        AVStream* candidate = inFmt->streams[i];
        if (candidate && candidate->codecpar
            && candidate->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            streamIndex = static_cast<int>(i);
            break;
        }
    }
    if (streamIndex < 0) {
        logAudioPassthroughDisabled(audioSourcePath, "no audio stream found");
        avformat_close_input(&inFmt);
        return false;
    }

    AVStream* inStream = inFmt->streams[streamIndex];
    if (!inStream || !inStream->codecpar) {
        logAudioPassthroughDisabled(audioSourcePath, "audio stream has no codec parameters");
        avformat_close_input(&inFmt);
        return false;
    }

    AVCodecParameters* copiedParams = avcodec_parameters_alloc();
    if (!copiedParams) {
        logAudioPassthroughDisabled(audioSourcePath, "failed to allocate audio codec parameters");
        avformat_close_input(&inFmt);
        return false;
    }

    rc = avcodec_parameters_copy(copiedParams, inStream->codecpar);
    if (rc < 0) {
        logAudioPassthroughDisabled(audioSourcePath,
                                    "codec parameter copy failed: " + ffmpegErrorString(rc));
        avcodec_parameters_free(&copiedParams);
        avformat_close_input(&inFmt);
        return false;
    }
    copiedParams->codec_tag = 0;

    AVStream* outStream = avformat_new_stream(m_outFmt, nullptr);
    if (!outStream) {
        logAudioPassthroughDisabled(audioSourcePath, "failed to create output audio stream");
        avcodec_parameters_free(&copiedParams);
        avformat_close_input(&inFmt);
        return false;
    }

    avcodec_parameters_free(&outStream->codecpar);
    outStream->codecpar = copiedParams;
    copiedParams = nullptr;
    outStream->time_base = inStream->time_base;

    m_audioInFmt = inFmt;
    m_audioInStream = inStream;
    m_audioOutStream = outStream;
    m_audioInStreamIndex = streamIndex;
    return true;
}

bool FrameEncoder::configureAudioEncoder(const EncodeRequest& req)
{
    if (!m_outFmt) return false;
    if (req.audioSampleRate <= 0 || req.audioChannels <= 0
        || req.audioBitrateBits <= 0) {
        std::fprintf(stderr,
                     "FrameEncoder: invalid AAC audio settings "
                     "(sample_rate=%d channels=%d bitrate=%lld)\n",
                     req.audioSampleRate,
                     req.audioChannels,
                     static_cast<long long>(req.audioBitrateBits));
        return false;
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!encoder) {
        std::fprintf(stderr, "FrameEncoder: native AAC encoder not found\n");
        return false;
    }

    m_audioEncCtx = avcodec_alloc_context3(encoder);
    if (!m_audioEncCtx) {
        std::fprintf(stderr, "FrameEncoder: failed to allocate AAC encoder context\n");
        return false;
    }

    m_audioEncCtx->sample_fmt = firstSupportedAudioSampleFormat(encoder);
    m_audioEncCtx->sample_rate = req.audioSampleRate;
    av_channel_layout_default(&m_audioEncCtx->ch_layout, req.audioChannels);
    m_audioEncCtx->bit_rate = req.audioBitrateBits;
    m_audioEncCtx->time_base = AVRational{1, req.audioSampleRate};
    if (m_outFmt && (m_outFmt->oformat->flags & AVFMT_GLOBALHEADER)) {
        m_audioEncCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int rc = avcodec_open2(m_audioEncCtx, encoder, nullptr);
    if (rc < 0) {
        std::fprintf(stderr,
                     "FrameEncoder: failed to open AAC encoder: %s\n",
                     ffmpegErrorString(rc).c_str());
        return false;
    }

    AVStream* outStream = avformat_new_stream(m_outFmt, nullptr);
    if (!outStream) {
        std::fprintf(stderr, "FrameEncoder: failed to create AAC output stream\n");
        return false;
    }

    rc = avcodec_parameters_from_context(outStream->codecpar, m_audioEncCtx);
    if (rc < 0) {
        std::fprintf(stderr,
                     "FrameEncoder: AAC codec parameter copy failed: %s\n",
                     ffmpegErrorString(rc).c_str());
        return false;
    }
    outStream->time_base = m_audioEncCtx->time_base;

    const int initialFifoSize =
        (m_audioEncCtx->frame_size > 0) ? m_audioEncCtx->frame_size : 1;
    m_audioFifo = av_audio_fifo_alloc(m_audioEncCtx->sample_fmt,
                                      m_audioEncCtx->ch_layout.nb_channels,
                                      initialFifoSize);
    if (!m_audioFifo) {
        std::fprintf(stderr, "FrameEncoder: failed to allocate AAC audio FIFO\n");
        return false;
    }

    m_audioScratchFrame = av_frame_alloc();
    if (!m_audioScratchFrame) {
        std::fprintf(stderr, "FrameEncoder: failed to allocate AAC scratch frame\n");
        return false;
    }

    m_audioOutStream = outStream;
    m_audioEncode = true;
    m_audioNextPts = 0;
    m_audioPtsInitialized = false;

    std::fprintf(stderr,
                 "FrameEncoder: AAC audio encode enabled sample_fmt=%s "
                 "sample_rate=%d channels=%d bitrate=%lld\n",
                 av_get_sample_fmt_name(m_audioEncCtx->sample_fmt)
                     ? av_get_sample_fmt_name(m_audioEncCtx->sample_fmt) : "?",
                 m_audioEncCtx->sample_rate,
                 m_audioEncCtx->ch_layout.nb_channels,
                 static_cast<long long>(m_audioEncCtx->bit_rate));
    return true;
}

bool FrameEncoder::drainAudioEncoderPackets()
{
    if (!m_audioEncCtx || !m_audioOutStream || !m_outFmt) return false;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    bool ok = true;
    while (true) {
        const int rc = avcodec_receive_packet(m_audioEncCtx, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            std::fprintf(stderr,
                         "FrameEncoder: AAC packet receive failed: %s\n",
                         ffmpegErrorString(rc).c_str());
            ok = false;
            break;
        }

        av_packet_rescale_ts(pkt,
                             m_audioEncCtx->time_base,
                             m_audioOutStream->time_base);
        pkt->stream_index = m_audioOutStream->index;
        pkt->pos = -1;
        const int writeRc = av_interleaved_write_frame(m_outFmt, pkt);
        av_packet_unref(pkt);
        if (writeRc < 0) {
            std::fprintf(stderr,
                         "FrameEncoder: AAC packet write failed: %s\n",
                         ffmpegErrorString(writeRc).c_str());
            ok = false;
            break;
        }
    }

    av_packet_free(&pkt);
    return ok;
}

bool FrameEncoder::sendAudioEncoderFrame(AVFrame* frame)
{
    if (!m_audioEncCtx) return false;

    const int rc = avcodec_send_frame(m_audioEncCtx, frame);
    if (rc < 0) {
        std::fprintf(stderr,
                     "FrameEncoder: AAC frame send failed: %s\n",
                     ffmpegErrorString(rc).c_str());
        return false;
    }
    return drainAudioEncoderPackets();
}

bool FrameEncoder::encodeAudioFifoSamples(int nbSamples)
{
    if (!m_audioEncCtx || !m_audioFifo || !m_audioScratchFrame || nbSamples <= 0) {
        return false;
    }

    av_frame_unref(m_audioScratchFrame);
    m_audioScratchFrame->nb_samples = nbSamples;
    m_audioScratchFrame->format = m_audioEncCtx->sample_fmt;
    m_audioScratchFrame->sample_rate = m_audioEncCtx->sample_rate;
    if (av_channel_layout_copy(&m_audioScratchFrame->ch_layout,
                               &m_audioEncCtx->ch_layout) < 0) {
        std::fprintf(stderr,
                     "FrameEncoder: failed to copy AAC channel layout\n");
        return false;
    }

    int rc = av_frame_get_buffer(m_audioScratchFrame, 0);
    if (rc < 0) {
        std::fprintf(stderr,
                     "FrameEncoder: failed to allocate AAC frame buffer: %s\n",
                     ffmpegErrorString(rc).c_str());
        return false;
    }

    const int readSamples =
        av_audio_fifo_read(m_audioFifo,
                           reinterpret_cast<void**>(m_audioScratchFrame->extended_data),
                           nbSamples);
    if (readSamples != nbSamples) {
        std::fprintf(stderr,
                     "FrameEncoder: AAC FIFO read returned %d/%d samples\n",
                     readSamples,
                     nbSamples);
        return false;
    }

    if (!m_audioPtsInitialized) {
        m_audioNextPts = 0;
        m_audioPtsInitialized = true;
    }
    m_audioScratchFrame->pts = m_audioNextPts;
    m_audioNextPts += nbSamples;

    return sendAudioEncoderFrame(m_audioScratchFrame);
}

bool FrameEncoder::flushAudioEncoder()
{
    if (!m_audioEncode || !m_audioEncCtx) return true;

    const int encoderFrameSize =
        (m_audioEncCtx->frame_size > 0) ? m_audioEncCtx->frame_size : 0;
    if (encoderFrameSize > 0) {
        while (m_audioFifo
               && av_audio_fifo_size(m_audioFifo) >= encoderFrameSize) {
            if (!encodeAudioFifoSamples(encoderFrameSize)) return false;
        }
    }
    if (m_audioFifo && av_audio_fifo_size(m_audioFifo) > 0) {
        if (!encodeAudioFifoSamples(av_audio_fifo_size(m_audioFifo))) {
            return false;
        }
    }

    return sendAudioEncoderFrame(nullptr);
}

void FrameEncoder::muxAudioPassthroughPackets()
{
    if (!m_audioInFmt || !m_audioInStream || !m_audioOutStream) return;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        std::fprintf(stderr,
                     "FrameEncoder: audio passthrough skipped: failed to allocate packet\n");
        return;
    }

    // [US-MF-8a] Video-length cap (-shortest equivalent). The rendered video
    // runs framesPushed * fpsDen / fpsNum seconds; source audio that extends
    // past that (trim/range renders, or an asset longer than the rendered
    // range) must be truncated so the muxed audio does not outlast the video.
    // When no video frames were pushed there is nothing to bound against, so
    // the cap is disabled and every packet is copied (legacy behavior).
    const bool capEnabled =
        (m_videoFramesPushed > 0 && m_fpsNum > 0 && m_fpsDen > 0);
    const double videoDurationSec =
        capEnabled
            ? (static_cast<double>(m_videoFramesPushed)
               * static_cast<double>(m_fpsDen))
                  / static_cast<double>(m_fpsNum)
            : 0.0;

    // [US-MF-8b] Start-time normalization. Source audio whose stream start_time
    // is non-zero (AAC encoder delay, mid-stream extracted clips) would copy a
    // non-zero first PTS against a pts-0 video and desync A/V. Capture a base
    // offset in input-stream time_base units and subtract it from every
    // packet's PTS/DTS before rescaling so the audio is 0-origin. The input
    // stream's start_time is preferred; if it is unset the first decoded
    // packet's PTS is used instead. The offset is resolved lazily on the first
    // packet so a stale start_time cannot override an earlier real PTS.
    int64_t baseOffset = 0;
    bool baseOffsetResolved = false;
    if (m_audioInStream->start_time != AV_NOPTS_VALUE) {
        baseOffset = m_audioInStream->start_time;
        baseOffsetResolved = true;
    }
    const int64_t trimStartInTb =
        m_audioStartUs > 0
            ? av_rescale_q(m_audioStartUs,
                           AVRational{1, AV_TIME_BASE},
                           m_audioInStream->time_base)
            : 0;

    // Audio tracks passed here are short, pre-rendered assets. Copying them
    // after the video encoder flush is sufficient for this workflow; the muxer
    // interleave queue orders DTS as packets are written.
    while (av_read_frame(m_audioInFmt, pkt) >= 0) {
        if (pkt->stream_index == m_audioInStreamIndex) {
            // Resolve the 0-origin base offset from the first packet's PTS
            // when the container did not advertise a stream start_time.
            if (!baseOffsetResolved) {
                baseOffset = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : 0;
                baseOffsetResolved = true;
            }

            int64_t normPts = 0;
            if (pkt->pts != AV_NOPTS_VALUE) {
                normPts = pkt->pts - baseOffset;
                if (normPts < 0) normPts = 0;
            }
            if (trimStartInTb > 0 && pkt->pts != AV_NOPTS_VALUE) {
                const int64_t pktDuration =
                    pkt->duration > 0 ? pkt->duration : 0;
                if (normPts + pktDuration <= trimStartInTb) {
                    av_packet_unref(pkt);
                    continue;
                }
            }

            // -shortest cap: stop once a packet's normalized presentation time
            // (in seconds, input time_base) reaches the video duration. The
            // remaining source packets are intentionally not written.
            if (capEnabled && pkt->pts != AV_NOPTS_VALUE) {
                int64_t normPtsForCap = normPts - trimStartInTb;
                if (normPtsForCap < 0) normPtsForCap = 0;
                const double ptsSec =
                    static_cast<double>(normPtsForCap)
                    * av_q2d(m_audioInStream->time_base);
                if (ptsSec >= videoDurationSec) {
                    av_packet_unref(pkt);
                    break;
                }
            }

            // Normalize PTS/DTS to 0-origin, clamping any negative result
            // (e.g. edit-list lead-in) to 0, then rescale to the output
            // stream time_base.
            if (pkt->pts != AV_NOPTS_VALUE) {
                pkt->pts -= baseOffset + trimStartInTb;
                if (pkt->pts < 0) pkt->pts = 0;
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                pkt->dts -= baseOffset + trimStartInTb;
                if (pkt->dts < 0) pkt->dts = 0;
            }
            av_packet_rescale_ts(pkt,
                                 m_audioInStream->time_base,
                                 m_audioOutStream->time_base);
            pkt->stream_index = m_audioOutStream->index;
            pkt->pos = -1;
            const int rc = av_interleaved_write_frame(m_outFmt, pkt);
            if (rc < 0) {
                std::fprintf(stderr,
                             "FrameEncoder: audio passthrough packet write failed: %s\n",
                             ffmpegErrorString(rc).c_str());
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

std::optional<std::string> FrameEncoder::open(const EncodeRequest& req)
{
    if (m_opened) return std::string("FrameEncoder already opened");
    const AVRational fps = effectiveFpsRational(req);
    if (req.width <= 0 || req.height <= 0
        || fps.num <= 0 || fps.den <= 0) {
        return std::string("Invalid frame geometry");
    }
    if (req.outputPath.empty()) {
        return std::string("Output path is empty");
    }

    // [US-MF-8] Capture fps for video-duration computation in
    // muxAudioPassthroughPackets() (-shortest equivalent). Validated > 0 above.
    m_fpsNum = fps.num;
    m_fpsDen = fps.den;
    m_audioStartUs = req.audioStartUs > 0 ? req.audioStartUs : 0;

    if (avformat_alloc_output_context2(&m_outFmt, nullptr, nullptr,
                                       req.outputPath.c_str()) < 0 || !m_outFmt) {
        return std::string("Failed to create output context");
    }

    if (!openEncoderWithFallback(req)) {
        releaseAll();
        return std::string("Failed to open encoder");
    }

    m_outStream = avformat_new_stream(m_outFmt, nullptr);
    if (!m_outStream) {
        releaseAll();
        return std::string("Failed to create output stream");
    }
    // [P1-M2] avcodec_parameters_from_context can fail (e.g. parameter
    // allocation OOM); ignoring the return value left the stream's codecpar
    // partially-initialised and avformat_write_header would later fail with
    // a less actionable error. Surface the failure at the source.
    if (avcodec_parameters_from_context(m_outStream->codecpar, m_encCtx) < 0) {
        releaseAll();
        return std::string("avcodec_parameters_from_context failed");
    }
    m_outStream->time_base = m_encCtx->time_base;

    if (req.audioEncode) {
        if (!req.audioSourcePath.empty()) {
            std::fprintf(stderr,
                         "FrameEncoder: audioEncode enabled; ignoring "
                         "audioSourcePath '%s'\n",
                         req.audioSourcePath.c_str());
        }
        if (!configureAudioEncoder(req)) {
            releaseAll();
            return std::string("Failed to open AAC audio encoder");
        }
    } else if (!req.audioSourcePath.empty()) {
        configureAudioPassthrough(req.audioSourcePath);
    }

    if (!(m_outFmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m_outFmt->pb, req.outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
            releaseAll();
            return std::string("Failed to open output file");
        }
    }

    if (avformat_write_header(m_outFmt, nullptr) < 0) {
        releaseAll();
        return std::string("Failed to write header");
    }

    m_opened = true;
    return std::nullopt;
}

void FrameEncoder::attachHdr10SideData(AVFrame* frame)
{
    if (!m_attachHdr10Metadata || !frame) return;

    // Mastering display (SMPTE ST 2086): BT.2020 primaries + D65 white point,
    // identical to the BT.2020 set buildX265Hdr10Params() encodes for libx265
    // (G(8500,39850) B(6550,2300) R(35400,14600) WP(15635,16450), units of
    // 1/50000). The struct uses AVRational, so express each as <value>/50000
    // for the chromaticity coords and the configured nits for luminance.
    if (!av_frame_get_side_data(frame,
                                AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        AVMasteringDisplayMetadata* mdm =
            av_mastering_display_metadata_create_side_data(frame);
        if (mdm) {
            mdm->display_primaries[0][0] = AVRational{35400, 50000}; // R x
            mdm->display_primaries[0][1] = AVRational{14600, 50000}; // R y
            mdm->display_primaries[1][0] = AVRational{ 8500, 50000}; // G x
            mdm->display_primaries[1][1] = AVRational{39850, 50000}; // G y
            mdm->display_primaries[2][0] = AVRational{ 6550, 50000}; // B x
            mdm->display_primaries[2][1] = AVRational{ 2300, 50000}; // B y
            mdm->white_point[0] = AVRational{15635, 50000};
            mdm->white_point[1] = AVRational{16450, 50000};
            mdm->has_primaries = 1;
            // Luminance in cd/m^2 (the struct stores it as a rational).
            mdm->max_luminance = AVRational{
                static_cast<int>(m_hdrMasterMaxNits + 0.5), 1};
            mdm->min_luminance = AVRational{
                static_cast<int>(m_hdrMasterMinNits * 10000.0 + 0.5), 10000};
            mdm->has_luminance = 1;
        }
    }

    // Content light level (CEA-861.3): MaxCLL / MaxFALL.
    if (!av_frame_get_side_data(frame,
                                AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        AVContentLightMetadata* clm =
            av_content_light_metadata_create_side_data(frame);
        if (clm) {
            clm->MaxCLL  = static_cast<unsigned>(m_hdrMaxCll);
            clm->MaxFALL = static_cast<unsigned>(m_hdrMaxFall);
        }
    }
}

bool FrameEncoder::pushFrameNative(AVFrame* frame, int64_t pts)
{
    if (!m_opened || m_finalized || !m_encCtx || !frame) return false;

    frame->pts = pts;

    // HDR10 through a non-libx265 encoder (hevc_mf): carry the static HDR10
    // metadata + colour signalling on every frame so the encoder writes the
    // mastering-display / content-light SEI a genuine HDR10 stream needs.
    attachHdr10SideData(frame);
    if (m_attachHdr10Metadata) {
        frame->color_primaries = m_encCtx->color_primaries;
        frame->color_trc       = m_encCtx->color_trc;
        frame->colorspace      = m_encCtx->colorspace;
        frame->color_range     = m_encCtx->color_range;
    }

    const int sendRc = avcodec_send_frame(m_encCtx, frame);
    if (sendRc < 0) {
        // Surface the exact failure: the silent "encoder frame push failed"
        // string the caller reports otherwise hides whether this was a
        // pixel-format mismatch, an unsupported profile, or an MFT error.
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(sendRc, errBuf, sizeof(errBuf));
        std::fprintf(stderr,
                     "FrameEncoder: avcodec_send_frame failed (encoder='%s' "
                     "pix_fmt=%s frame.fmt=%s): %s\n",
                     m_activeEncoderName.c_str(),
                     av_get_pix_fmt_name(m_encCtx->pix_fmt)
                         ? av_get_pix_fmt_name(m_encCtx->pix_fmt) : "?",
                     av_get_pix_fmt_name(
                         static_cast<AVPixelFormat>(frame->format))
                         ? av_get_pix_fmt_name(
                               static_cast<AVPixelFormat>(frame->format))
                         : "?",
                     errBuf);
        return false;
    }

    // [US-MF-8] Count frames accepted by the video encoder so finalize() can
    // derive rational-fps video duration for the audio -shortest cap.
    // pushFrameRgb24() routes through here, so a single increment covers both
    // public push paths without double-counting.
    ++m_videoFramesPushed;

    AVPacket* encPkt = av_packet_alloc();
    if (!encPkt) return false;
    int receiveRc = 0;
    while ((receiveRc = avcodec_receive_packet(m_encCtx, encPkt)) == 0) {
        av_packet_rescale_ts(encPkt, m_encCtx->time_base, m_outStream->time_base);
        encPkt->stream_index = m_outStream->index;
        const int wr = av_interleaved_write_frame(m_outFmt, encPkt);
        av_packet_unref(encPkt);
        if (wr < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(wr, errBuf, sizeof(errBuf));
            std::fprintf(stderr,
                         "FrameEncoder: video packet write failed: %s\n",
                         errBuf);
            av_packet_free(&encPkt);
            return false;
        }
    }
    if (receiveRc < 0 && receiveRc != AVERROR(EAGAIN) && receiveRc != AVERROR_EOF) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(receiveRc, errBuf, sizeof(errBuf));
        std::fprintf(stderr,
                     "FrameEncoder: video packet receive failed: %s\n",
                     errBuf);
        av_packet_free(&encPkt);
        return false;
    }
    av_packet_free(&encPkt);
    return true;
}

bool FrameEncoder::pushAudioFrame(AVFrame* frame, int64_t pts)
{
    if (!m_opened || m_finalized || !m_audioEncode || !m_audioEncCtx
        || !m_audioFifo || !frame) {
        return false;
    }
    if (frame->nb_samples <= 0) return false;

    const bool layoutMatches =
        av_channel_layout_compare(&frame->ch_layout,
                                  &m_audioEncCtx->ch_layout) == 0;
    if (frame->format != m_audioEncCtx->sample_fmt
        || frame->sample_rate != m_audioEncCtx->sample_rate
        || !layoutMatches) {
        std::fprintf(stderr,
                     "FrameEncoder: AAC frame format mismatch "
                     "(frame fmt=%s rate=%d channels=%d; encoder fmt=%s "
                     "rate=%d channels=%d)\n",
                     av_get_sample_fmt_name(
                         static_cast<AVSampleFormat>(frame->format))
                         ? av_get_sample_fmt_name(
                               static_cast<AVSampleFormat>(frame->format))
                         : "?",
                     frame->sample_rate,
                     frame->ch_layout.nb_channels,
                     av_get_sample_fmt_name(m_audioEncCtx->sample_fmt)
                         ? av_get_sample_fmt_name(m_audioEncCtx->sample_fmt)
                         : "?",
                     m_audioEncCtx->sample_rate,
                     m_audioEncCtx->ch_layout.nb_channels);
        return false;
    }

    if (av_audio_fifo_size(m_audioFifo) == 0 && pts != AV_NOPTS_VALUE) {
        m_audioNextPts = pts;
        m_audioPtsInitialized = true;
    }

    const int fifoSize = av_audio_fifo_size(m_audioFifo);
    const int rc = av_audio_fifo_realloc(m_audioFifo,
                                         fifoSize + frame->nb_samples);
    if (rc < 0) {
        std::fprintf(stderr,
                     "FrameEncoder: AAC FIFO realloc failed: %s\n",
                     ffmpegErrorString(rc).c_str());
        return false;
    }

    const int written =
        av_audio_fifo_write(m_audioFifo,
                            reinterpret_cast<void**>(frame->extended_data),
                            frame->nb_samples);
    if (written != frame->nb_samples) {
        std::fprintf(stderr,
                     "FrameEncoder: AAC FIFO write returned %d/%d samples\n",
                     written,
                     frame->nb_samples);
        return false;
    }

    const int encoderFrameSize =
        (m_audioEncCtx->frame_size > 0)
            ? m_audioEncCtx->frame_size
            : frame->nb_samples;
    while (encoderFrameSize > 0
           && av_audio_fifo_size(m_audioFifo) >= encoderFrameSize) {
        if (!encodeAudioFifoSamples(encoderFrameSize)) return false;
    }

    return true;
}

bool FrameEncoder::pushFrameRgb24(const uint8_t* src, int stride, int64_t pts)
{
    if (!m_opened || m_finalized || !m_encCtx || !src) return false;

    // Lazy-allocate scratch YUV frame + RGB->YUV swscale context.
    if (!m_scratchFrame) {
        m_scratchFrame = av_frame_alloc();
        if (!m_scratchFrame) return false;
        m_scratchFrame->format = m_pixFmt;
        m_scratchFrame->width = m_encCtx->width;
        m_scratchFrame->height = m_encCtx->height;
        m_scratchFrame->color_primaries = m_encCtx->color_primaries;
        m_scratchFrame->color_trc = m_encCtx->color_trc;
        m_scratchFrame->colorspace = m_encCtx->colorspace;
        m_scratchFrame->color_range = m_encCtx->color_range;
        if (av_frame_get_buffer(m_scratchFrame, 0) < 0) {
            av_frame_free(&m_scratchFrame);
            return false;
        }
    }
    if (!m_rgbToYuvCtx) {
        m_rgbToYuvCtx = sws_getContext(
            m_encCtx->width, m_encCtx->height, AV_PIX_FMT_RGB24,
            m_encCtx->width, m_encCtx->height, m_pixFmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_rgbToYuvCtx) return false;
        if (swscolor::matrixEnabledFromEnv()) {
            const AVColorSpace dstCs = swscolor::resolveColorspace(
                m_encCtx->colorspace, m_encCtx->width, m_encCtx->height);
            const AVColorRange dstRange =
                swscolor::resolveRange(m_encCtx->color_range);
            int *currentInvTable = nullptr;
            int *currentTable = nullptr;
            int currentSrcRange = 0;
            int currentDstRange = 0;
            int brightness = 0;
            int contrast = 0;
            int saturation = 0;
            if (sws_getColorspaceDetails(m_rgbToYuvCtx, &currentInvTable,
                                          &currentSrcRange, &currentTable,
                                          &currentDstRange, &brightness,
                                          &contrast, &saturation) >= 0) {
                const int *srcCoeffs = sws_getCoefficients(SWS_CS_DEFAULT);
                const int *dstCoeffs =
                    sws_getCoefficients(swscolor::swsCoeffsId(dstCs));
                if (srcCoeffs && dstCoeffs) {
                    (void)sws_setColorspaceDetails(
                        m_rgbToYuvCtx, srcCoeffs, 1, dstCoeffs,
                        dstRange == AVCOL_RANGE_JPEG ? 1 : 0,
                        brightness, contrast, saturation);
                }
            }
        }
    }

    // [P1-M2] av_frame_make_writable allocates a fresh buffer if the previous
    // one is still referenced by a queued packet; ignoring its rc could leave
    // a shared buffer being concurrently overwritten by sws_scale. Surface
    // the failure so the caller can abort the encode pipeline.
    {
        const int rc = av_frame_make_writable(m_scratchFrame);
        if (rc < 0) {
            std::fprintf(stderr,
                         "FrameEncoder: av_frame_make_writable failed: %d\n",
                         rc);
            return false;
        }
    }
    const uint8_t* srcSlices[1] = { src };
    int srcStrides[1] = { stride };
    sws_scale(m_rgbToYuvCtx, srcSlices, srcStrides, 0, m_encCtx->height,
              m_scratchFrame->data, m_scratchFrame->linesize);

    return pushFrameNative(m_scratchFrame, pts);
}

#ifdef VEDITOR_LIBAVCORE_WITH_QIMAGE
bool FrameEncoder::pushFrame(const QImage& image, int64_t pts)
{
    if (image.isNull()) return false;
    QImage rgb = (image.format() == QImage::Format_RGB888)
                     ? image
                     : image.convertToFormat(QImage::Format_RGB888);
    return pushFrameRgb24(rgb.constBits(),
                          static_cast<int>(rgb.bytesPerLine()),
                          pts);
}
#endif

std::optional<std::string> FrameEncoder::finalize()
{
    if (!m_opened || m_finalized) return std::nullopt;
    m_finalized = true;

    if (m_encCtx) {
        const int sendRc = avcodec_send_frame(m_encCtx, nullptr);
        if (sendRc < 0 && sendRc != AVERROR(EAGAIN) && sendRc != AVERROR_EOF) {
            const std::string err = ffmpegErrorString(sendRc);
            std::fprintf(stderr,
                         "FrameEncoder: video encoder flush send failed: %s\n",
                         err.c_str());
            return std::string("Video encoder flush send failed: ") + err;
        }
        AVPacket* flushPkt = av_packet_alloc();
        if (!flushPkt) {
            return std::string("Failed to allocate video flush packet");
        }
        while (true) {
            const int rc = avcodec_receive_packet(m_encCtx, flushPkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
                break;
            }
            if (rc < 0) {
                const std::string err = ffmpegErrorString(rc);
                std::fprintf(stderr,
                             "FrameEncoder: video packet receive during flush failed: %s\n",
                             err.c_str());
                av_packet_free(&flushPkt);
                return std::string("Video encoder flush receive failed: ") + err;
            }

            av_packet_rescale_ts(flushPkt, m_encCtx->time_base, m_outStream->time_base);
            flushPkt->stream_index = m_outStream->index;
            const int writeRc = av_interleaved_write_frame(m_outFmt, flushPkt);
            av_packet_unref(flushPkt);
            if (writeRc < 0) {
                const std::string err = ffmpegErrorString(writeRc);
                std::fprintf(stderr,
                             "FrameEncoder: video packet write during flush failed: %s\n",
                             err.c_str());
                av_packet_free(&flushPkt);
                return std::string("Video packet write during flush failed: ") + err;
            }
        }
        av_packet_free(&flushPkt);
    }

    if (m_audioEncode) {
        if (!flushAudioEncoder()) {
            return std::string("Failed to flush AAC audio encoder");
        }
    } else {
        muxAudioPassthroughPackets();
    }

    if (m_outFmt) {
        av_write_trailer(m_outFmt);
    }

    return std::nullopt;
}

} // namespace libavcore
