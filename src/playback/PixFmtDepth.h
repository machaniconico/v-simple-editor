#pragma once

// Stage10 — per-component (luma) bit depth for an AVPixelFormat.
//
// Lives here (FFmpeg-aware playback zone) rather than in color/ClipColor so that
// ClipColor stays FFmpeg-header-free: fromCodecParams() deliberately takes raw
// ints. This helper is shared by Timeline::addClip (the ingest probe) and the
// hdr-ingest-colormeta selftest so the load-bearing extraction is covered by a
// headless gate.
namespace pixfmtdepth {

// Returns the true per-component depth of the pixel format (e.g. 10 for
// yuv420p10le / p010le, 12 for yuv420p12le, 8 for yuv420p), floored at 8.
// Returns 8 for AV_PIX_FMT_NONE / unknown formats (null descriptor).
//
// NOTE: this intentionally reads comp[0].depth for ordinary component formats,
// NOT av_get_bits_per_pixel()/3. Bayer is the exception: FFmpeg's component
// depths describe the CFA channel packing, so use total bits-per-pixel there.
// av_get_bits_per_pixel() is the chroma-subsampling-weighted AVERAGE bits per
// pixel, so dividing by 3 only yields the per-component depth for 4:4:4 and
// collapses 10-bit 4:2:0 HDR (yuv420p10le = 15 avg bpp -> 15/3 = 5 -> 8) to an
// SDR bit depth.
int bitDepthFromPixFmt(int avPixFmt);

} // namespace pixfmtdepth
