#pragma once

// ===========================================================================
// libavcore::Probe — Qt-free libav helpers for codec/decoder/format probing.
//
// PRD-B campaign ④: shared library wrapping bare libav calls so that
// non-Exporter callers (RenderQueue, ProxyManager, AIHighlight, NetworkRender,
// VideoStabilizer) can migrate off QProcess(ffmpeg.exe) progressively.
//
// This header has ZERO Qt dependencies — pure C++/libav so that future
// non-Qt consumers (CLI tooling, headless render workers) can link it too.
// ===========================================================================

#include <optional>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace libavcore {

// Returns true if the named encoder is registered in this libavcodec build.
// Independent of CodecDetector (kept Qt-free); the two may agree.
bool encoderAvailable(const std::string& codecName);

// Returns true if the named decoder is registered in this libavcodec build.
bool decoderAvailable(const std::string& codecName);

// Probe container duration in microseconds (AV_TIME_BASE units).
// Returns std::nullopt on open/probe failure. Caller cleanup is internal.
std::optional<int64_t> probeDurationMicroseconds(const std::string& filePath);

// Probe the FFmpeg codec name of the first video stream in the file
// (e.g. "h264", "hevc", "vp9"). Returns std::nullopt if no video stream
// or the file can't be opened. Caller cleanup is internal.
std::optional<std::string> probeVideoCodecName(const std::string& filePath);

} // namespace libavcore
