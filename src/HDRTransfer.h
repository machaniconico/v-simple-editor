#pragma once
#include <QImage>

namespace hdr {

enum class TransferFn {
    Linear,
    SDR_Gamma22,
    PQ_HDR10,
    HLG
};

float pqEotf(float v);   // PQ EOTF: norm 0..1 -> linear scene-referred (peak 10000 nits)
float pqOetf(float L);   // PQ OETF: linear -> norm 0..1
float hlgEotf(float v);  // HLG EOTF: norm 0..1 -> linear (ARIB STD-B67)
float hlgOetf(float L);  // HLG OETF: linear -> norm 0..1

QImage applyToneMapReinhard(const QImage& linearRec2020, double exposure = 1.0);
QImage applyToneMapHable(const QImage& linearRec2020, double exposure = 1.0);
QImage convertColorSpace(const QImage& src, TransferFn srcFn, TransferFn dstFn);

} // namespace hdr
