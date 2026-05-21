#pragma once

// ===========================================================================
// libavcore::Concat — Qt-free libav helper for concat-demuxer-equivalent
// packet-copy remux.
//
// PRD-B campaign ④: in-process replacement for the
//   ffmpeg -f concat -safe 0 -i list.txt -c copy -y out
// subprocess that AIHighlight and NetworkRender currently spawn to join
// pre-rendered segments without re-encoding.
//
// This header has ZERO Qt dependencies — pure C++/libav so that future
// non-Qt consumers (CLI tooling, headless render workers) can link it too.
// Mirrors the style of Probe.h: extern "C" libav include, namespace
// libavcore, std::optional return, internal RAII cleanup.
// ===========================================================================

#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace libavcore {

// Join every file in inputPaths, in order, into outputPath by stream-copying
// packets (no re-encoding — the concat-demuxer equivalent of "ffmpeg -c copy").
//
// The first input defines the output muxer: every stream's codecpar is copied
// and the header is written once. For each subsequent input the packet PTS/DTS
// are shifted by the running cumulative duration so timestamps stay
// monotonically increasing across join boundaries; stream indices are mapped
// onto the output streams. A trailer is written after the last input.
//
// The output container is inferred from the outputPath extension via
// avformat_alloc_output_context2.
//
// Returns std::nullopt on success, or a descriptive error message string on
// failure. Error cases handled without crashing:
//   - inputPaths empty
//   - an input file is missing / cannot be opened (the path is named)
//   - a stream-layout mismatch between inputs
// A single-element inputPaths is valid and behaves as a plain remux.
//
// Caller cleanup is internal: all libav contexts opened here are released
// before return on every path.
std::optional<std::string> concatCopy(const std::vector<std::string>& inputPaths,
                                      const std::string& outputPath);

} // namespace libavcore
