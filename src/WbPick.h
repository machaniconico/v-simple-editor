#pragma once

#include <QColor>

namespace wbpick {

struct TempTintCorrection {
    double temperature = 0.0;
    double tint = 0.0;
};

TempTintCorrection tempTintForNeutral(const QColor &pixel);

} // namespace wbpick
