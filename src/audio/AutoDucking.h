#pragma once

#include <cstddef>
#include <vector>

namespace audio::autoducking {

struct AutoDuckingConfig {
    double sampleRate = 48000.0;
    double voiceThreshold = 0.05;
    double attackSeconds = 0.050;
    double releaseSeconds = 0.250;
    double duckedGainDb = -12.0;
    double keyframeIntervalSeconds = 0.010;
};

struct EnvelopePoint {
    double timeSeconds = 0.0;
    double value = 0.0;
};

struct GainKeyframe {
    double timeSeconds = 0.0;
    double gainDb = 0.0;
};

struct AutoDuckingResult {
    std::vector<EnvelopePoint> voiceEnvelope;
    std::vector<GainKeyframe> bgmGainKeyframes;
};

std::vector<EnvelopePoint> detectVoiceEnvelope(const float *interleavedVoice,
                                               std::size_t frameCount,
                                               std::size_t channelCount,
                                               const AutoDuckingConfig &config);

std::vector<GainKeyframe> buildBgmGainKeyframes(const std::vector<EnvelopePoint> &voiceEnvelope,
                                                std::size_t bgmFrameCount,
                                                const AutoDuckingConfig &config);

AutoDuckingResult generateDuckingGainCurve(const std::vector<float> &interleavedVoice,
                                           std::size_t voiceChannelCount,
                                           const std::vector<float> &interleavedBgm,
                                           std::size_t bgmChannelCount,
                                           const AutoDuckingConfig &config);

} // namespace audio::autoducking
