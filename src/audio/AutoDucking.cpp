#include "AutoDucking.h"

#include <algorithm>
#include <cmath>

namespace audio::autoducking {
namespace {

bool validSampleLayout(const float *samples, std::size_t frameCount, std::size_t channelCount)
{
    return frameCount == 0 || (samples && channelCount > 0);
}

bool validConfig(const AutoDuckingConfig &config)
{
    return config.sampleRate > 0.0
        && config.voiceThreshold >= 0.0
        && config.attackSeconds >= 0.0
        && config.releaseSeconds >= 0.0
        && config.keyframeIntervalSeconds > 0.0
        && std::isfinite(config.sampleRate)
        && std::isfinite(config.voiceThreshold)
        && std::isfinite(config.attackSeconds)
        && std::isfinite(config.releaseSeconds)
        && std::isfinite(config.duckedGainDb)
        && std::isfinite(config.keyframeIntervalSeconds);
}

double clamp01(double value)
{
    return std::max(0.0, std::min(1.0, value));
}

double smoothingStep(double current, double target, double seconds, double sampleRate)
{
    if (seconds <= 0.0)
        return target;

    const double alpha = std::exp(-1.0 / (seconds * sampleRate));
    return alpha * current + (1.0 - alpha) * target;
}

double framePeak(const float *interleaved,
                 std::size_t frame,
                 std::size_t channelCount)
{
    double peak = 0.0;
    const std::size_t base = frame * channelCount;
    for (std::size_t channel = 0; channel < channelCount; ++channel)
        peak = std::max(peak, std::abs(static_cast<double>(interleaved[base + channel])));
    return peak;
}

double envelopeValueAt(const std::vector<EnvelopePoint> &envelope,
                       std::size_t frame,
                       const AutoDuckingConfig &config)
{
    if (envelope.empty())
        return 0.0;

    const double timeSeconds = static_cast<double>(frame) / config.sampleRate;
    auto it = std::lower_bound(envelope.begin(), envelope.end(), timeSeconds,
                               [](const EnvelopePoint &point, double time) {
                                   return point.timeSeconds < time;
                               });
    if (it == envelope.begin())
        return clamp01(it->value);
    if (it == envelope.end())
        return clamp01(envelope.back().value);

    const EnvelopePoint &next = *it;
    const EnvelopePoint &prev = *(it - 1);
    const double span = next.timeSeconds - prev.timeSeconds;
    if (span <= 0.0)
        return clamp01(next.value);

    const double t = (timeSeconds - prev.timeSeconds) / span;
    return clamp01(prev.value + (next.value - prev.value) * t);
}

bool allUnityGain(const std::vector<GainKeyframe> &keyframes)
{
    return std::all_of(keyframes.begin(), keyframes.end(),
                       [](const GainKeyframe &keyframe) {
                           return std::abs(keyframe.gainDb) <= 1.0e-9;
                       });
}

} // namespace

std::vector<EnvelopePoint> detectVoiceEnvelope(const float *interleavedVoice,
                                               std::size_t frameCount,
                                               std::size_t channelCount,
                                               const AutoDuckingConfig &config)
{
    std::vector<EnvelopePoint> envelope;
    if (!validConfig(config) || !validSampleLayout(interleavedVoice, frameCount, channelCount))
        return envelope;

    envelope.reserve(frameCount);
    double activity = 0.0;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const double peak = framePeak(interleavedVoice, frame, channelCount);
        const double target = peak > config.voiceThreshold ? 1.0 : 0.0;
        const double smoothingSeconds = target > activity
            ? config.attackSeconds
            : config.releaseSeconds;
        activity = smoothingStep(activity, target, smoothingSeconds, config.sampleRate);
        envelope.push_back({static_cast<double>(frame) / config.sampleRate, clamp01(activity)});
    }
    return envelope;
}

std::vector<GainKeyframe> buildBgmGainKeyframes(const std::vector<EnvelopePoint> &voiceEnvelope,
                                                std::size_t bgmFrameCount,
                                                const AutoDuckingConfig &config)
{
    std::vector<GainKeyframe> keyframes;
    if (!validConfig(config) || bgmFrameCount == 0)
        return keyframes;

    const double duckedGainDb = std::min(0.0, config.duckedGainDb);
    const auto intervalFrames = static_cast<std::size_t>(std::max(
        1.0,
        std::round(config.keyframeIntervalSeconds * config.sampleRate)));
    const auto addKeyframe = [&](std::size_t frame, double timeSeconds) {
        const double envelopeValue = envelopeValueAt(voiceEnvelope, frame, config);
        keyframes.push_back({timeSeconds, duckedGainDb * envelopeValue});
    };

    for (std::size_t frame = 0; frame < bgmFrameCount; frame += intervalFrames)
        addKeyframe(frame, static_cast<double>(frame) / config.sampleRate);

    const double durationSeconds = static_cast<double>(bgmFrameCount) / config.sampleRate;
    if (keyframes.empty() || keyframes.back().timeSeconds < durationSeconds)
        addKeyframe(bgmFrameCount - 1, durationSeconds);

    if (allUnityGain(keyframes))
        keyframes.clear();

    return keyframes;
}

AutoDuckingResult generateDuckingGainCurve(const std::vector<float> &interleavedVoice,
                                           std::size_t voiceChannelCount,
                                           const std::vector<float> &interleavedBgm,
                                           std::size_t bgmChannelCount,
                                           const AutoDuckingConfig &config)
{
    AutoDuckingResult result;
    if (voiceChannelCount == 0 || bgmChannelCount == 0)
        return result;

    const std::size_t voiceFrameCount = interleavedVoice.size() / voiceChannelCount;
    const std::size_t bgmFrameCount = interleavedBgm.size() / bgmChannelCount;
    result.voiceEnvelope = detectVoiceEnvelope(interleavedVoice.data(),
                                               voiceFrameCount,
                                               voiceChannelCount,
                                               config);
    result.bgmGainKeyframes = buildBgmGainKeyframes(result.voiceEnvelope,
                                                    bgmFrameCount,
                                                    config);
    return result;
}

} // namespace audio::autoducking
