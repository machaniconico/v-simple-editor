#pragma once

#include <QImage>

#include "AcesColor.h"
#include "ClipColor.h"

namespace clipcolor {

aces::ColorSpace acesSpaceFor(const ColorMeta& meta);

QImage toUnifiedSpace(const QImage& rgba64Premul,
                      const ColorMeta& in,
                      aces::ColorSpace outputSpace);

} // namespace clipcolor
