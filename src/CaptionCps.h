#ifndef CAPTION_CPS_H
#define CAPTION_CPS_H

#include <QString>

namespace captioncps {

constexpr double kDefaultMaxCps = 20.0;
constexpr double kInvalidDurationSentinel = 1.0e9;
constexpr double kMinimumDurationSeconds = 1.0e-9;

inline int visibleCharCount(const QString& text)
{
    const QString trimmed = text.trimmed();
    int count = 0;

    for (int i = 0; i < trimmed.size(); ++i) {
        const QChar ch = trimmed.at(i);
        if (ch.isLowSurrogate())
            continue;
        if (!ch.isSpace())
            ++count;
    }

    return count;
}

inline double cps(const QString& text, double durationSeconds)
{
    if (durationSeconds <= 0.0)
        return kInvalidDurationSentinel;

    const double safeDuration = durationSeconds > kMinimumDurationSeconds
        ? durationSeconds
        : kMinimumDurationSeconds;
    return static_cast<double>(visibleCharCount(text)) / safeDuration;
}

inline bool exceeds(const QString& text,
                    double durationSeconds,
                    double threshold = kDefaultMaxCps)
{
    return cps(text, durationSeconds) > threshold;
}

} // namespace captioncps

#endif // CAPTION_CPS_H
