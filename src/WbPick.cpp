#include "WbPick.h"

#include <QtGlobal>

namespace {

double clampGrade(double value)
{
    return qBound(-100.0, value, 100.0);
}

} // namespace

namespace wbpick {

TempTintCorrection tempTintForNeutral(const QColor &pixel)
{
    if (!pixel.isValid())
        return {};

    const double r = pixel.red();
    const double g = pixel.green();
    const double b = pixel.blue();
    const double rbMid = (r + b) * 0.5;

    TempTintCorrection correction;
    correction.temperature = clampGrade(b - r);
    correction.tint = clampGrade(2.0 * (g - rbMid));
    return correction;
}

} // namespace wbpick
