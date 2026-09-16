#include "Timecode.h"
#include <QStringList>
#include <QRegularExpression>
#include <cmath>

bool parseTimecodeInput(const QString &text, double fps, double currentSec, double *outSec)
{
    if (!outSec || !std::isfinite(fps) || fps <= 0.0 || !std::isfinite(currentSec))
        return false;
    QString input = text.trimmed();
    const bool relative = input.startsWith('+') || input.startsWith('-');
    const double sign = input.startsWith('-') ? -1.0 : 1.0;
    if (relative) input.remove(0, 1);
    const QStringList parts = input.split(':');
    if (parts.isEmpty() || parts.size() > 4) return false;
    static const QRegularExpression integer(QStringLiteral("^[0-9]+$"));
    static const QRegularExpression seconds(QStringLiteral("^[0-9]+(?:\\.[0-9]+)?$"));
    double value = 0.0;
    for (int i = 0; i < parts.size(); ++i) {
        const auto &pattern = parts.size() == 1 ? seconds : integer;
        if (!pattern.match(parts[i]).hasMatch()) return false;
        bool ok = false;
        const double component = parts[i].toDouble(&ok);
        if (!ok || !std::isfinite(component)) return false;
        const bool frames = parts.size() >= 3 && i == parts.size() - 1;
        if (frames) {
            if (component >= std::ceil(fps)) return false;
            value += component / fps;
        } else {
            if (i > 0 && component >= 60.0) return false;
            value = value * 60.0 + component;
        }
    }
    const double result = relative ? currentSec + sign * value : value;
    if (!std::isfinite(result)) return false;
    *outSec = result;
    return true;
}
