#pragma once

#include "GpuCompositeMath.h"
#include "HdrCompositeMath.h"
#include "../color/ClipOdt.h"

#include <QImage>
#include <QSize>
#include <QVector>

namespace trackmatte16 {

struct MatteColorCtx {
    bool matteSourceIsLinear = false;
    clipodt::OdtParams odtForLuma{};
};

quint16 matteMaskValue16(gpucomposite::MatteType type,
                         quint16 straightR16,
                         quint16 straightG16,
                         quint16 straightB16,
                         quint16 premulA16);

void applyMaskPremul16(hdrcomposite::Rgba16& px, quint16 maskVal);

QImage composeExport(const QVector<gpucomposite::LayerDesc>& layers,
                     const QVector<QImage>& canvasSizedImages,
                     QSize canvas,
                     const MatteColorCtx& ctx);

} // namespace trackmatte16
