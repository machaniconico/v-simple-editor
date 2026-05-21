#include "Concat.h"

#include <cstdint>
#include <limits>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

namespace libavcore {

namespace {

// Tiny RAII guard for AVFormatContext input contexts (mirrors Probe.cpp).
struct InputCtxGuard {
    AVFormatContext* ctx = nullptr;
    ~InputCtxGuard() {
        if (ctx) {
            avformat_close_input(&ctx);
        }
    }
};

std::string ffmpegErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, buf, sizeof(buf)) < 0) {
        return std::to_string(err);
    }
    return std::string(buf);
}

} // namespace

std::optional<std::string> concatCopy(const std::vector<std::string>& inputPaths,
                                      const std::string& outputPath)
{
    if (inputPaths.empty()) {
        return std::string("concatCopy: input list is empty");
    }
    if (outputPath.empty()) {
        return std::string("concatCopy: output path is empty");
    }

    // ----- allocate the output muxer (container inferred from extension) -----
    AVFormatContext* outFmt = nullptr;
    if (avformat_alloc_output_context2(&outFmt, nullptr, nullptr,
                                       outputPath.c_str()) < 0
        || !outFmt) {
        return std::string("concatCopy: could not infer output container for '")
               + outputPath + "'";
    }

    // Per-output-stream cumulative duration, expressed in that stream's
    // time_base. Each subsequent input's packets are shifted by this offset so
    // timestamps increase monotonically across joins. Maps 1:1 onto the output
    // streams created from the first input.
    std::vector<int64_t> nextStreamOffset;
    // Highest (end) timestamp seen so far on each output stream, in its
    // time_base — used to derive the offset applied to the *next* input.
    std::vector<int64_t> streamEndTs;
    // codec_type per output stream (for AV_NOPTS / duration heuristics).
    std::vector<AVMediaType> streamType;
    bool headerWritten = false;
    bool fileOpened = false;

    // Single cleanup point: close the IO and free the muxer on every exit.
    auto teardown = [&]() {
        if (outFmt) {
            if (fileOpened && !(outFmt->oformat->flags & AVFMT_NOFILE)
                && outFmt->pb) {
                avio_closep(&outFmt->pb);
            }
            avformat_free_context(outFmt);
            outFmt = nullptr;
        }
    };

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        teardown();
        return std::string("concatCopy: failed to allocate packet");
    }

    for (size_t inputIdx = 0; inputIdx < inputPaths.size(); ++inputIdx) {
        const std::string& inPath = inputPaths[inputIdx];

        InputCtxGuard inGuard;
        int rc = avformat_open_input(&inGuard.ctx, inPath.c_str(),
                                     nullptr, nullptr);
        if (rc < 0) {
            av_packet_free(&pkt);
            teardown();
            return std::string("concatCopy: cannot open input '") + inPath
                   + "': " + ffmpegErrorString(rc);
        }

        rc = avformat_find_stream_info(inGuard.ctx, nullptr);
        if (rc < 0) {
            av_packet_free(&pkt);
            teardown();
            return std::string("concatCopy: cannot read stream info for '")
                   + inPath + "': " + ffmpegErrorString(rc);
        }

        // Map this input's stream indices onto the output stream indices.
        // -1 means "no matching output stream" (packet is dropped).
        std::vector<int> streamMap(inGuard.ctx->nb_streams, -1);

        if (inputIdx == 0) {
            // First input defines the output: clone every stream's codecpar.
            for (unsigned i = 0; i < inGuard.ctx->nb_streams; ++i) {
                AVStream* inStream = inGuard.ctx->streams[i];
                if (!inStream || !inStream->codecpar) {
                    continue;
                }
                AVStream* outStream = avformat_new_stream(outFmt, nullptr);
                if (!outStream) {
                    av_packet_free(&pkt);
                    teardown();
                    return std::string("concatCopy: failed to create output "
                                       "stream for '") + inPath + "'";
                }
                rc = avcodec_parameters_copy(outStream->codecpar,
                                             inStream->codecpar);
                if (rc < 0) {
                    av_packet_free(&pkt);
                    teardown();
                    return std::string("concatCopy: codec parameter copy "
                                       "failed for '") + inPath
                           + "': " + ffmpegErrorString(rc);
                }
                // codec_tag is container-specific; let the muxer pick one.
                outStream->codecpar->codec_tag = 0;
                outStream->time_base = inStream->time_base;
                streamMap[i] = outStream->index;
            }

            if (outFmt->nb_streams == 0) {
                av_packet_free(&pkt);
                teardown();
                return std::string("concatCopy: first input '") + inPath
                       + "' has no usable streams";
            }

            nextStreamOffset.assign(outFmt->nb_streams, 0);
            streamEndTs.assign(outFmt->nb_streams, AV_NOPTS_VALUE);
            streamType.assign(outFmt->nb_streams, AVMEDIA_TYPE_UNKNOWN);
            for (unsigned i = 0; i < inGuard.ctx->nb_streams; ++i) {
                const int outIdx = streamMap[i];
                if (outIdx >= 0 && inGuard.ctx->streams[i]
                    && inGuard.ctx->streams[i]->codecpar) {
                    streamType[outIdx] =
                        inGuard.ctx->streams[i]->codecpar->codec_type;
                }
            }

            // Open the output IO and write the header exactly once.
            if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
                rc = avio_open(&outFmt->pb, outputPath.c_str(),
                               AVIO_FLAG_WRITE);
                if (rc < 0) {
                    av_packet_free(&pkt);
                    teardown();
                    return std::string("concatCopy: cannot open output '")
                           + outputPath + "': " + ffmpegErrorString(rc);
                }
            }
            fileOpened = true;

            rc = avformat_write_header(outFmt, nullptr);
            if (rc < 0) {
                av_packet_free(&pkt);
                teardown();
                return std::string("concatCopy: failed to write header for '")
                       + outputPath + "': " + ffmpegErrorString(rc);
            }
            headerWritten = true;
        } else {
            // Subsequent inputs must line up positionally with the streams
            // declared by the first input. Map by index when codec types
            // agree; bail out on a structural mismatch.
            for (unsigned i = 0;
                 i < inGuard.ctx->nb_streams
                 && i < outFmt->nb_streams; ++i) {
                AVStream* inStream = inGuard.ctx->streams[i];
                if (!inStream || !inStream->codecpar) {
                    continue;
                }
                if (inStream->codecpar->codec_type
                    != outFmt->streams[i]->codecpar->codec_type) {
                    av_packet_free(&pkt);
                    teardown();
                    return std::string("concatCopy: stream layout of '")
                           + inPath + "' does not match the first input";
                }
                streamMap[i] = static_cast<int>(i);
            }
        }

        // Per-input running maximum end-timestamp, in output time_base, used
        // to advance nextStreamOffset for the input that follows this one.
        std::vector<int64_t> inputEndTs(outFmt->nb_streams, AV_NOPTS_VALUE);

        // ----- copy every packet, shifted into the joined timeline -----
        while (av_read_frame(inGuard.ctx, pkt) >= 0) {
            const int srcIdx = pkt->stream_index;
            if (srcIdx < 0
                || static_cast<unsigned>(srcIdx) >= streamMap.size()
                || streamMap[srcIdx] < 0) {
                av_packet_unref(pkt);
                continue;
            }
            const int outIdx = streamMap[srcIdx];
            AVStream* inStream = inGuard.ctx->streams[srcIdx];
            AVStream* outStream = outFmt->streams[outIdx];

            // Rescale the packet into the output stream time_base, then apply
            // this input's accumulated offset so it is appended after every
            // earlier input rather than overlapping it.
            av_packet_rescale_ts(pkt, inStream->time_base,
                                 outStream->time_base);

            const int64_t offset = nextStreamOffset[outIdx];
            if (pkt->pts != AV_NOPTS_VALUE) {
                pkt->pts += offset;
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                pkt->dts += offset;
            }

            // Track where this input ends so the next input starts after it.
            // duration may be 0 for some demuxers; fall back to the timestamp.
            int64_t endTs = AV_NOPTS_VALUE;
            if (pkt->pts != AV_NOPTS_VALUE) {
                endTs = pkt->pts + (pkt->duration > 0 ? pkt->duration : 0);
            } else if (pkt->dts != AV_NOPTS_VALUE) {
                endTs = pkt->dts + (pkt->duration > 0 ? pkt->duration : 0);
            }
            if (endTs != AV_NOPTS_VALUE
                && (inputEndTs[outIdx] == AV_NOPTS_VALUE
                    || endTs > inputEndTs[outIdx])) {
                inputEndTs[outIdx] = endTs;
            }

            pkt->stream_index = outIdx;
            pkt->pos = -1;

            const int wrc = av_interleaved_write_frame(outFmt, pkt);
            // av_interleaved_write_frame takes ownership of pkt's buffer and
            // leaves it blank, so no av_packet_unref is needed afterwards.
            if (wrc < 0) {
                av_packet_free(&pkt);
                teardown();
                return std::string("concatCopy: failed to write packet from '")
                       + inPath + "': " + ffmpegErrorString(wrc);
            }
        }

        // Advance the cumulative offset for every stream so the next input
        // is placed immediately after the longest stream of this input.
        for (unsigned s = 0; s < outFmt->nb_streams; ++s) {
            if (inputEndTs[s] != AV_NOPTS_VALUE) {
                nextStreamOffset[s] = inputEndTs[s];
                streamEndTs[s] = inputEndTs[s];
            }
        }
    }

    av_packet_free(&pkt);

    // ----- finalize: write the trailer, then release everything -----
    if (headerWritten) {
        const int rc = av_write_trailer(outFmt);
        if (rc < 0) {
            teardown();
            return std::string("concatCopy: failed to write trailer for '")
                   + outputPath + "': " + ffmpegErrorString(rc);
        }
    }

    teardown();
    return std::nullopt;
}

} // namespace libavcore
