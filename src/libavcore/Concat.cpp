#include "Concat.h"

#include <QByteArray>
#include <QtGlobal>

#include <cerrno>
#include <cstdint>
#include <cstdio>
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

// Mirror an error string to stderr before it is handed back to the caller, so
// failures are observable even when the caller only logs/handles the return
// value loosely. Returns the message so callers can `return concatError(...)`.
std::string concatError(std::string msg)
{
    std::fprintf(stderr, "%s\n", msg.c_str());
    return msg;
}

bool concatTestReadErrorEnabled()
{
    return qgetenv("VEDITOR_CONCAT_TEST_READ_ERROR") == QByteArray("1");
}

} // namespace

std::optional<std::string> concatCopy(const std::vector<std::string>& inputPaths,
                                      const std::string& outputPath)
{
    if (inputPaths.empty()) {
        return concatError("concatCopy: input list is empty");
    }
    if (outputPath.empty()) {
        return concatError("concatCopy: output path is empty");
    }
    const bool injectReadError = concatTestReadErrorEnabled();

    // ----- allocate the output muxer (container inferred from extension) -----
    AVFormatContext* outFmt = nullptr;
    if (avformat_alloc_output_context2(&outFmt, nullptr, nullptr,
                                       outputPath.c_str()) < 0
        || !outFmt) {
        return concatError(
            std::string("concatCopy: could not infer output container for '")
            + outputPath + "'");
    }

    // Shared cumulative timeline position, expressed once per output stream's
    // time_base. Each subsequent input's packets are shifted by this offset so
    // timestamps increase monotonically across joins while all streams start
    // from the same per-file boundary.
    std::vector<int64_t> nextStreamOffset;
    // codec_type per output stream (for AV_NOPTS / duration heuristics).
    std::vector<AVMediaType> streamType;
    // Last DTS actually handed to the muxer on each output stream, in that
    // stream's time_base. av_interleaved_write_frame rejects an output stream
    // whose DTS is not strictly increasing, so every packet's DTS is bumped to
    // stay above this watermark (zero-duration packets and join boundaries can
    // otherwise repeat or regress a DTS). AV_NOPTS_VALUE = nothing written yet.
    std::vector<int64_t> lastWrittenDts;
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
        return concatError("concatCopy: failed to allocate packet");
    }

    for (size_t inputIdx = 0; inputIdx < inputPaths.size(); ++inputIdx) {
        const std::string& inPath = inputPaths[inputIdx];

        InputCtxGuard inGuard;
        int rc = avformat_open_input(&inGuard.ctx, inPath.c_str(),
                                     nullptr, nullptr);
        if (rc < 0) {
            av_packet_free(&pkt);
            teardown();
            return concatError(
                std::string("concatCopy: cannot open input '") + inPath
                + "': " + ffmpegErrorString(rc));
        }

        rc = avformat_find_stream_info(inGuard.ctx, nullptr);
        if (rc < 0) {
            av_packet_free(&pkt);
            teardown();
            return concatError(
                std::string("concatCopy: cannot read stream info for '")
                + inPath + "': " + ffmpegErrorString(rc));
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
                    return concatError(
                        std::string("concatCopy: failed to create output "
                                    "stream for '") + inPath + "'");
                }
                rc = avcodec_parameters_copy(outStream->codecpar,
                                             inStream->codecpar);
                if (rc < 0) {
                    av_packet_free(&pkt);
                    teardown();
                    return concatError(
                        std::string("concatCopy: codec parameter copy "
                                    "failed for '") + inPath
                        + "': " + ffmpegErrorString(rc));
                }
                // codec_tag is container-specific; let the muxer pick one.
                outStream->codecpar->codec_tag = 0;
                outStream->time_base = inStream->time_base;
                streamMap[i] = outStream->index;
            }

            if (outFmt->nb_streams == 0) {
                av_packet_free(&pkt);
                teardown();
                return concatError(
                    std::string("concatCopy: first input '") + inPath
                    + "' has no usable streams");
            }

            nextStreamOffset.assign(outFmt->nb_streams, 0);
            streamType.assign(outFmt->nb_streams, AVMEDIA_TYPE_UNKNOWN);
            lastWrittenDts.assign(outFmt->nb_streams, AV_NOPTS_VALUE);
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
                    return concatError(
                        std::string("concatCopy: cannot open output '")
                        + outputPath + "': " + ffmpegErrorString(rc));
                }
            }
            fileOpened = true;

            rc = avformat_write_header(outFmt, nullptr);
            if (rc < 0) {
                av_packet_free(&pkt);
                teardown();
                return concatError(
                    std::string("concatCopy: failed to write header for '")
                    + outputPath + "': " + ffmpegErrorString(rc));
            }
            headerWritten = true;
        } else {
            // Subsequent inputs must line up positionally with the streams
            // declared by the first input. A different stream count cannot be
            // mapped 1:1 and would yield a corrupt output, so reject it before
            // copying any packet.
            if (inGuard.ctx->nb_streams != outFmt->nb_streams) {
                av_packet_free(&pkt);
                teardown();
                return concatError(
                    std::string("concatCopy: input '") + inPath + "' has "
                    + std::to_string(inGuard.ctx->nb_streams)
                    + " stream(s) but the first input declared "
                    + std::to_string(outFmt->nb_streams)
                    + " — stream counts must match");
            }
            // Map by index when codec types agree; bail out on a structural
            // mismatch.
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
                    return concatError(
                        std::string("concatCopy: stream layout of '")
                        + inPath + "' does not match the first input");
                }
                streamMap[i] = static_cast<int>(i);
            }
        }

        // Per-input running maximum end-timestamp, in output time_base, used
        // to advance nextStreamOffset for the input that follows this one.
        std::vector<int64_t> inputEndTs(outFmt->nb_streams, AV_NOPTS_VALUE);
        // Per-input common origin: the earliest packet timestamp across all
        // mapped streams. Subtracting the same origin from every stream
        // preserves real A/V lead/lag inside the input while still cancelling
        // container/file start offsets.
        int64_t inputOriginTs = 0;
        AVRational inputOriginTimeBase{1, AV_TIME_BASE};
        bool inputOriginResolved = false;

        int readRc = 0;
        while ((readRc = av_read_frame(inGuard.ctx, pkt)) >= 0) {
            const int srcIdx = pkt->stream_index;
            if (srcIdx >= 0
                && static_cast<unsigned>(srcIdx) < streamMap.size()
                && streamMap[srcIdx] >= 0) {
                AVStream* inStream = inGuard.ctx->streams[srcIdx];
                if (inStream) {
                    const int64_t ts =
                        (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
                    if (ts != AV_NOPTS_VALUE) {
                        if (!inputOriginResolved
                            || av_compare_ts(ts, inStream->time_base,
                                             inputOriginTs,
                                             inputOriginTimeBase) < 0) {
                            inputOriginTs = ts;
                            inputOriginTimeBase = inStream->time_base;
                            inputOriginResolved = true;
                        }
                    }
                }
            }
            av_packet_unref(pkt);
        }
        if (readRc != AVERROR_EOF) {
            av_packet_free(&pkt);
            teardown();
            return concatError(
                std::string("concatCopy: failed to read input '") + inPath
                + "': " + ffmpegErrorString(readRc));
        }
        if (!inputOriginResolved) {
            inputOriginTs = 0;
            inputOriginTimeBase = AVRational{1, AV_TIME_BASE};
        }

        avformat_close_input(&inGuard.ctx);
        rc = avformat_open_input(&inGuard.ctx, inPath.c_str(),
                                 nullptr, nullptr);
        if (rc < 0) {
            av_packet_free(&pkt);
            teardown();
            return concatError(
                std::string("concatCopy: cannot reopen input '") + inPath
                + "': " + ffmpegErrorString(rc));
        }
        rc = avformat_find_stream_info(inGuard.ctx, nullptr);
        if (rc < 0) {
            av_packet_free(&pkt);
            teardown();
            return concatError(
                std::string("concatCopy: cannot reread stream info for '")
                + inPath + "': " + ffmpegErrorString(rc));
        }
        if (inGuard.ctx->nb_streams != streamMap.size()) {
            av_packet_free(&pkt);
            teardown();
            return concatError(
                std::string("concatCopy: reopened input '") + inPath
                + "' changed stream layout");
        }

        // ----- copy every packet, shifted into the joined timeline -----
        readRc = 0;
        int injectedReadPackets = 0;
        while ((readRc = av_read_frame(inGuard.ctx, pkt)) >= 0) {
            if (injectReadError && ++injectedReadPackets >= 5) {
                av_packet_unref(pkt);
                readRc = AVERROR(EIO);
                break;
            }
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

            // Rescale the packet into the output stream time_base. All
            // timestamp arithmetic below is in that time_base.
            av_packet_rescale_ts(pkt, inStream->time_base,
                                 outStream->time_base);

            const int64_t origin =
                av_rescale_q(inputOriginTs, inputOriginTimeBase,
                             outStream->time_base);

            // Append after every earlier input: shift by (offset - origin) so
            // this input is 0-origin and then placed at the cumulative offset.
            const int64_t shift = nextStreamOffset[outIdx] - origin;
            if (pkt->pts != AV_NOPTS_VALUE) {
                pkt->pts += shift;
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                pkt->dts += shift;
            }

            // av_interleaved_write_frame rejects an output stream whose DTS is
            // not strictly increasing. Zero-duration packets and join
            // boundaries can repeat or regress a DTS, so bump it above the
            // last value written to this output stream, keeping PTS >= DTS.
            if (pkt->dts != AV_NOPTS_VALUE
                && lastWrittenDts[outIdx] != AV_NOPTS_VALUE
                && pkt->dts <= lastWrittenDts[outIdx]) {
                pkt->dts = lastWrittenDts[outIdx] + 1;
                if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
                    pkt->pts = pkt->dts;
                }
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

            // Remember the DTS handed to the muxer before ownership is lost.
            if (pkt->dts != AV_NOPTS_VALUE) {
                lastWrittenDts[outIdx] = pkt->dts;
            }

            const int wrc = av_interleaved_write_frame(outFmt, pkt);
            // av_interleaved_write_frame takes ownership of pkt's buffer and
            // leaves it blank, so no av_packet_unref is needed afterwards.
            if (wrc < 0) {
                av_packet_free(&pkt);
                teardown();
                return concatError(
                    std::string("concatCopy: failed to write packet from '")
                    + inPath + "': " + ffmpegErrorString(wrc));
            }
        }
        if (readRc != AVERROR_EOF) {
            av_packet_free(&pkt);
            teardown();
            return concatError(
                std::string("concatCopy: failed to read input '") + inPath
                + "': " + ffmpegErrorString(readRc));
        }

        // Advance the cumulative offset for every stream so the next input
        // is placed immediately after the longest stream of this input.
        int64_t nextInputStartUs = AV_NOPTS_VALUE;
        for (unsigned s = 0; s < outFmt->nb_streams; ++s) {
            if (inputEndTs[s] != AV_NOPTS_VALUE) {
                const int64_t endUs =
                    av_rescale_q(inputEndTs[s],
                                 outFmt->streams[s]->time_base,
                                 AVRational{1, AV_TIME_BASE});
                if (nextInputStartUs == AV_NOPTS_VALUE
                    || endUs > nextInputStartUs) {
                    nextInputStartUs = endUs;
                }
            }
        }
        if (nextInputStartUs != AV_NOPTS_VALUE) {
            for (unsigned s = 0; s < outFmt->nb_streams; ++s) {
                nextStreamOffset[s] =
                    av_rescale_q(nextInputStartUs,
                                 AVRational{1, AV_TIME_BASE},
                                 outFmt->streams[s]->time_base);
            }
        }
    }

    av_packet_free(&pkt);

    // ----- finalize: write the trailer, then release everything -----
    if (headerWritten) {
        const int rc = av_write_trailer(outFmt);
        if (rc < 0) {
            teardown();
            return concatError(
                std::string("concatCopy: failed to write trailer for '")
                + outputPath + "': " + ffmpegErrorString(rc));
        }
    }

    teardown();
    return std::nullopt;
}

} // namespace libavcore
