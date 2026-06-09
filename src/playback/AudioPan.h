#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace audiopan {

struct BalanceGains {
    double left = 1.0;
    double right = 1.0;
};

inline double clampPan(double pan) noexcept
{
    return std::clamp(pan, -1.0, 1.0);
}

inline BalanceGains balanceGains(double pan) noexcept
{
    pan = clampPan(pan);
    if (pan >= 0.0)
        return {1.0 - pan, 1.0};
    return {1.0, 1.0 + pan};
}

inline double gainForChannel(const BalanceGains &gains, int channel) noexcept
{
    return (channel == 0) ? gains.left : gains.right;
}

template <typename Sample>
inline Sample scaleSample(Sample sample, double gain) noexcept
{
    if constexpr (std::is_same<Sample, int16_t>::value) {
        double scaled = static_cast<double>(sample) * gain;
        if (scaled > 32767.0)
            scaled = 32767.0;
        else if (scaled < -32768.0)
            scaled = -32768.0;
        return static_cast<int16_t>(scaled);
    } else {
        return static_cast<Sample>(static_cast<double>(sample) * gain);
    }
}

template <typename Sample>
inline void applyBalanceInterleavedStereo(Sample *samples,
                                          std::size_t frameCount,
                                          double pan) noexcept
{
    if (!samples || frameCount == 0 || pan == 0.0)
        return;

    const BalanceGains gains = balanceGains(pan);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        Sample *lr = samples + frame * 2;
        lr[0] = scaleSample(lr[0], gains.left);
        lr[1] = scaleSample(lr[1], gains.right);
    }
}

inline void applyBalanceInterleavedStereoInt16(int16_t *samples,
                                               std::size_t frameCount,
                                               double pan) noexcept
{
    applyBalanceInterleavedStereo(samples, frameCount, pan);
}

inline void applyBalanceInterleavedStereoFloat(float *samples,
                                               std::size_t frameCount,
                                               double pan) noexcept
{
    applyBalanceInterleavedStereo(samples, frameCount, pan);
}

} // namespace audiopan
