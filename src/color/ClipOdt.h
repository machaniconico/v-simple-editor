#pragma once

#include <QImage>

#include "AcesColor.h"

namespace clipodt {

struct OdtParams {
    aces::ColorSpace outputSpace = aces::ColorSpace::Rec709;
    bool tonemap = true;
};

QImage applyOdt16(const QImage& linearWorkingRgba64Premul, const OdtParams& p);
bool enabledFromEnv();

} // namespace clipodt
