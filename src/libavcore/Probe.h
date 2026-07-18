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
//
// DLL-ready design (PRD-B3 / US-B3-5):
//   firstTenBitHevcEncoder() probes the *currently loaded* avcodec DLL for a
//   10-bit HEVC capable encoder at runtime.  The bundled avcodec-62.dll ships
//   without libx265/nvenc/qsv/amf, so the function returns std::nullopt on the
//   stock build.  Drop-in replacement with a full-featured DLL (libx265 or HW
//   encoder enabled) will make it return the best available encoder name
//   automatically — no recompile needed.
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

// Returns the name of the first 10-bit HEVC encoder available in the loaded
// avcodec DLL, probing in priority order: libx265, hevc_nvenc, hevc_qsv,
// hevc_amf.  An encoder qualifies only if avcodec_find_encoder_by_name()
// finds it AND it reports support for AV_PIX_FMT_YUV420P10LE or
// AV_PIX_FMT_P010LE via avcodec_get_supported_config() (FFmpeg 8 API).
// An empty or NULL pixel-format list is treated as "not 10-bit capable"
// (no optimistic fallback).  hevc_mf is intentionally excluded because
// MediaFoundation silently downgrades 10-bit input to 8-bit NV12.
// Returns std::nullopt when no qualifying encoder is found.
std::optional<std::string> firstTenBitHevcEncoder();

// Convenience wrapper: returns true iff firstTenBitHevcEncoder() has a value.
bool tenBitHevcEncoderAvailable();

} // namespace libavcore
