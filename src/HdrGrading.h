#pragma once

#include <QImage>

// ---------------------------------------------------------------------------
// HDR grading / tone mapping (Sprint 21, US-HDR-1)
//
// NOTE: this header intentionally lives in `namespace hdr` alongside the
// pre-existing HDRTransfer.h. All public symbols here (TransferFunction,
// ToneMapOperator, applyPqEotf, applyHlgOetf, toneMapHdrToSdr) are distinct
// from HDRTransfer.h's (TransferFn, pqEotf, hlgOetf, ...) so the two units
// co-exist without collision.
// ---------------------------------------------------------------------------

namespace hdr {

// Source encoding of an incoming HDR/SDR image.
enum class TransferFunction {
    SDR_Rec709,    // already (roughly) display-linear / Rec.709-ish
    PQ_Rec2100,    // SMPTE ST 2084 (PQ), Rec.2100
    HLG_Rec2100    // ARIB STD-B67 / Rec.2100 HLG
};

// Tone-mapping operator used to compress HDR linear light into SDR.
enum class ToneMapOperator {
    Reinhard,
    AcesFilmic
};

// SMPTE ST 2084 PQ EOTF.
//
// Input  : normalized [0,1] code value V.
// Output : linear luminance normalized so that 1.0 corresponds to a
//          10000-nit scene (i.e. the return value is the fraction of
//          10000 nits, in [0,1]).
//
// Monotonic increasing. applyPqEotf(0) == 0, applyPqEotf(1) ~= 1.
double applyPqEotf(double codeValue);

// ARIB STD-B67 / Rec.2100 HLG OETF.
//
// Input  : scene linear light in [0,1].
// Output : signal value in [0,1].
//
// Monotonic increasing. applyHlgOetf(0) == 0.
double applyHlgOetf(double linear);

// Convert an HDR-encoded QImage to an 8-bit SDR QImage (Format_RGB32).
//
//   src    : how the source pixels are encoded.
//   op     : tone-map operator to apply (default ACES filmic).
//   maxNits: assumed display/master peak in nits used for PQ scaling.
//
// The returned image has the same dimensions as `hdrLinear`.
QImage toneMapHdrToSdr(const QImage &hdrLinear,
                       TransferFunction src,
                       ToneMapOperator op = ToneMapOperator::AcesFilmic,
                       double maxNits = 1000.0);

} // namespace hdr
