#include "Encode.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef VEDITOR_LIBAVCORE_WITH_QIMAGE
#include <QImage>
#endif

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

namespace libavcore {

namespace {

// Build x265-params HDR10 metadata string (BT.2020 primaries / D65). Identical
// to Exporter.cpp's static helper but Qt-free (no QByteArray).
static std::string buildX265Hdr10Params(double maxNits, double minNits,
                                         int maxCll, int maxFall)
{
    const int maxLumX10000 = static_cast<int>(std::round(maxNits * 10000.0));
    const int minLumX10000 = static_cast<int>(std::round(minNits * 10000.0));
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "hdr10=1:repeat-headers=1:colorprim=bt2020:"
                  "transfer=smpte2084:colormatrix=bt2020nc:range=limited:"
                  "master-display=G(8500,39850)B(6550,2300)R(35400,14600)"
                  "WP(15635,16450)L(%d,%d):max-cll=%d,%d",
                  maxLumX10000, minLumX10000, maxCll, maxFall);
    return std::string(buf);
}

static bool nameContains(const std::string& s, const char* needle)
{
    return s.find(needle) != std::string::npos;
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
    if (m_encCtx) { avcodec_free_context(&m_encCtx); }
    if (m_outFmt) {
        if (!(m_outFmt->oformat->flags & AVFMT_NOFILE) && m_outFmt->pb) {
            avio_closep(&m_outFmt->pb);
        }
        avformat_free_context(m_outFmt);
        m_outFmt = nullptr;
    }
    m_outStream = nullptr;
}

bool FrameEncoder::configureEncoderContext(const EncodeRequest& req,
                                            AVDictionary** outOpts)
{
    if (!m_encCtx) return false;

    m_encCtx->width = req.width;
    m_encCtx->height = req.height;
    m_encCtx->time_base = {1, req.fps};
    m_encCtx->framerate = {req.fps, 1};

    // Decide pixel format mirroring Exporter.cpp policy.
    AVPixelFormat targetPixFmt = AV_PIX_FMT_YUV420P;
    if (req.proresProfile >= 4) {
        targetPixFmt = AV_PIX_FMT_YUVA444P10LE;
    } else if (req.proresProfile >= 0) {
        targetPixFmt = AV_PIX_FMT_YUV422P10LE;
    } else if (req.isHdr10 || req.isHlg) {
        targetPixFmt = AV_PIX_FMT_YUV420P10LE;
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
    }

    if (m_outFmt && (m_outFmt->oformat->flags & AVFMT_GLOBALHEADER)) {
        m_encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Codec-specific opts — mirrors Exporter.cpp 1:1.
    const std::string& name = m_activeEncoderName;
    if (name == "h264_nvenc" || name == "hevc_nvenc") {
        av_dict_set(outOpts, "preset", "p4", 0);
        av_dict_set(outOpts, "rc", "vbr", 0);
        av_dict_set(outOpts, "cq", "23", 0);
    } else if (name == "h264_qsv" || name == "hevc_qsv") {
        av_dict_set(outOpts, "preset", "medium", 0);
    } else if (name == "h264_amf" || name == "hevc_amf") {
        av_dict_set(outOpts, "quality", "balanced", 0);
    } else if (req.videoCodecName == "libx264" || req.videoCodecName == "libx265") {
        av_dict_set(outOpts, "preset", "medium", 0);
        av_dict_set(outOpts, "crf", "23", 0);
        if (req.videoCodecName == "libx265") {
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
    } else if (req.videoCodecName == "libsvtav1") {
        av_dict_set(outOpts, "preset", "8", 0);
        av_dict_set(outOpts, "crf", "30", 0);
    } else if (req.videoCodecName == "libvpx-vp9") {
        av_dict_set(outOpts, "quality", "good", 0);
        av_dict_set(outOpts, "cpu-used", "4", 0);
    } else if (req.videoCodecName.rfind("prores", 0) == 0 && req.proresProfile >= 0) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", req.proresProfile);
        av_dict_set(outOpts, "profile", buf, 0);
    }
    return true;
}

bool FrameEncoder::openEncoderWithFallback(const EncodeRequest& req)
{
    // Find primary encoder by requested name; if missing, try CodecDetector-
    // style family detection (here inline so libavcore stays Qt-free).
    std::string resolved = req.videoCodecName;
    const AVCodec* encoder = avcodec_find_encoder_by_name(resolved.c_str());
    if (!encoder) {
        const std::string lower = resolved; // already lowercase by callers
        if (nameContains(lower, "264")) resolved = "libx264";
        else if (nameContains(lower, "265") || nameContains(lower, "hevc")) resolved = "libx265";
        else if (nameContains(lower, "av1")) resolved = "libsvtav1";
        else if (nameContains(lower, "vp9")) resolved = "libvpx-vp9";
        else resolved = "libx264";
        encoder = avcodec_find_encoder_by_name(resolved.c_str());
    }
    if (!encoder) return false;

    // HW encoder resolution (mirrors Exporter.cpp lines 165-209).
    const bool wantHW = (req.useHardwareAccel || !req.hwVendorHint.empty())
                       && req.hwVendorHint != "none";
    const bool isH264Family = (req.videoCodecName == "libx264"
                               || nameContains(req.videoCodecName, "264"));
    const bool isH265Family = (req.videoCodecName == "libx265"
                               || nameContains(req.videoCodecName, "265")
                               || nameContains(req.videoCodecName, "hevc"));

    const AVCodec* hwEncoder = nullptr;
    std::string hwEncoderName;

    if (wantHW && (isH264Family || isH265Family)) {
        std::string vendor = req.hwVendorHint;
        // Build candidate list matching Exporter.cpp.
        const char* candidates[3] = {nullptr, nullptr, nullptr};
        if (isH264Family) {
            if (vendor == "nvenc") { candidates[0] = "h264_nvenc"; }
            else if (vendor == "qsv") { candidates[0] = "h264_qsv"; }
            else if (vendor == "amf") { candidates[0] = "h264_amf"; }
            else { candidates[0] = "h264_nvenc"; candidates[1] = "h264_qsv"; candidates[2] = "h264_amf"; }
        } else {
            if (vendor == "nvenc") { candidates[0] = "hevc_nvenc"; }
            else if (vendor == "qsv") { candidates[0] = "hevc_qsv"; }
            else if (vendor == "amf") { candidates[0] = "hevc_amf"; }
            else { candidates[0] = "hevc_nvenc"; candidates[1] = "hevc_qsv"; candidates[2] = "hevc_amf"; }
        }
        for (int i = 0; i < 3 && candidates[i]; ++i) {
            // [P1-M1] Restore CodecDetector::isEncoderAvailable() runtime probe
            // semantics that were dropped during the libavcore refactor. The
            // legacy Exporter.cpp:194-203 path required BOTH (a) the encoder
            // name to be registered in this libavcodec build AND (b) the
            // CodecDetector probe to confirm a functional runtime (e.g. NVENC
            // requires a driver-loaded GPU; a stub-registered encoder must be
            // skipped). The hook is optional (default nullptr = always
            // available) so non-Qt callers retain the simple registration-only
            // probe.
            if (req.encoderAvailableHook && !req.encoderAvailableHook(candidates[i]))
                continue;
            const AVCodec* candidate = avcodec_find_encoder_by_name(candidates[i]);
            if (candidate) {
                hwEncoder = candidate;
                hwEncoderName = candidates[i];
                break;
            }
        }
        if (hwEncoder) {
            encoder = hwEncoder;
            resolved = hwEncoderName;
            m_hwInUse = true;
        }
    }

    // First attempt — primary encoder (possibly HW).
    m_encCtx = avcodec_alloc_context3(encoder);
    if (!m_encCtx) return false;
    m_activeEncoderName = encoder->name ? encoder->name : resolved;

    AVDictionary* opts = nullptr;
    configureEncoderContext(req, &opts);

    int rc = avcodec_open2(m_encCtx, encoder, &opts);
    av_dict_free(&opts);

    if (rc < 0) {
        avcodec_free_context(&m_encCtx);
        m_encCtx = nullptr;

        // HW open failed — fall back to SW encoder, mirroring Exporter.cpp.
        if (m_hwInUse) {
            m_hwInUse = false;
            const std::string swCodec = isH265Family ? "libx265" : "libx264";
            const AVCodec* swEncoder = avcodec_find_encoder_by_name(swCodec.c_str());
            if (!swEncoder) return false;

            m_encCtx = avcodec_alloc_context3(swEncoder);
            if (!m_encCtx) return false;
            m_activeEncoderName = swEncoder->name ? swEncoder->name : swCodec;

            // For SW fallback we have to rebuild opts using the libx264/265
            // baseline matching Exporter.cpp's swOpts (preset+crf, possibly
            // x265 HDR params).
            EncodeRequest swReq = req;
            swReq.videoCodecName = swCodec;  // forces the x264/x265 branch
            AVDictionary* swOpts = nullptr;
            configureEncoderContext(swReq, &swOpts);

            const int swRc = avcodec_open2(m_encCtx, swEncoder, &swOpts);
            av_dict_free(&swOpts);
            if (swRc < 0) {
                avcodec_free_context(&m_encCtx);
                m_encCtx = nullptr;
                return false;
            }
            return true;
        }
        return false;
    }
    return true;
}

std::optional<std::string> FrameEncoder::open(const EncodeRequest& req)
{
    if (m_opened) return std::string("FrameEncoder already opened");
    if (req.width <= 0 || req.height <= 0 || req.fps <= 0) {
        return std::string("Invalid frame geometry");
    }
    if (req.outputPath.empty()) {
        return std::string("Output path is empty");
    }

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

bool FrameEncoder::pushFrameNative(AVFrame* frame, int64_t pts)
{
    if (!m_opened || m_finalized || !m_encCtx || !frame) return false;

    frame->pts = pts;
    if (avcodec_send_frame(m_encCtx, frame) < 0) return false;

    AVPacket* encPkt = av_packet_alloc();
    if (!encPkt) return false;
    while (avcodec_receive_packet(m_encCtx, encPkt) == 0) {
        av_packet_rescale_ts(encPkt, m_encCtx->time_base, m_outStream->time_base);
        encPkt->stream_index = m_outStream->index;
        av_interleaved_write_frame(m_outFmt, encPkt);
        av_packet_unref(encPkt);
    }
    av_packet_free(&encPkt);
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
        avcodec_send_frame(m_encCtx, nullptr);
        AVPacket* flushPkt = av_packet_alloc();
        if (flushPkt) {
            while (avcodec_receive_packet(m_encCtx, flushPkt) == 0) {
                av_packet_rescale_ts(flushPkt, m_encCtx->time_base, m_outStream->time_base);
                flushPkt->stream_index = m_outStream->index;
                av_interleaved_write_frame(m_outFmt, flushPkt);
                av_packet_unref(flushPkt);
            }
            av_packet_free(&flushPkt);
        }
    }

    if (m_outFmt) {
        av_write_trailer(m_outFmt);
    }

    return std::nullopt;
}

} // namespace libavcore
