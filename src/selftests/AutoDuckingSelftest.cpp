#include "../audio/AutoDucking.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

bool requireGate(bool condition,
                 const char *gate,
                 const char *message,
                 int &passed,
                 int &failed)
{
    if (condition) {
        ++passed;
        std::cout << "[auto-ducking] PASS " << gate << '\n';
        return true;
    }

    ++failed;
    std::cerr << "[auto-ducking] FAIL " << gate << ": " << message << '\n';
    return false;
}

std::vector<float> makeVoiceOver(std::size_t frameCount, double sampleRate)
{
    std::vector<float> voice(frameCount, 0.0f);
    const std::size_t start = static_cast<std::size_t>(0.500 * sampleRate);
    const std::size_t end = static_cast<std::size_t>(1.000 * sampleRate);
    for (std::size_t frame = start; frame < end && frame < voice.size(); ++frame)
        voice[frame] = 0.80f;
    return voice;
}

std::vector<float> makeStereoBgm(std::size_t frameCount, double sampleRate)
{
    std::vector<float> bgm(frameCount * 2, 0.0f);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const double t = static_cast<double>(frame) / sampleRate;
        const float sample = static_cast<float>(0.25 * std::sin(2.0 * kPi * 110.0 * t));
        bgm[frame * 2] = sample;
        bgm[frame * 2 + 1] = -sample;
    }
    return bgm;
}

double gainAt(const std::vector<audio::autoducking::GainKeyframe> &keyframes,
              double timeSeconds)
{
    if (keyframes.empty())
        return 0.0;

    auto it = std::lower_bound(keyframes.begin(), keyframes.end(), timeSeconds,
                               [](const audio::autoducking::GainKeyframe &keyframe,
                                  double time) {
                                   return keyframe.timeSeconds < time;
                               });
    if (it == keyframes.begin())
        return it->gainDb;
    if (it == keyframes.end())
        return keyframes.back().gainDb;

    const auto &next = *it;
    const auto &prev = *(it - 1);
    const double span = next.timeSeconds - prev.timeSeconds;
    if (span <= 0.0)
        return next.gainDb;

    const double t = (timeSeconds - prev.timeSeconds) / span;
    return prev.gainDb + (next.gainDb - prev.gainDb) * t;
}

double firstTimeAtOrBelow(const std::vector<audio::autoducking::GainKeyframe> &keyframes,
                          double gainDb)
{
    for (const auto &keyframe : keyframes) {
        if (keyframe.gainDb <= gainDb)
            return keyframe.timeSeconds;
    }
    return -1.0;
}

double lastTimeAtOrBelow(const std::vector<audio::autoducking::GainKeyframe> &keyframes,
                         double gainDb)
{
    double last = -1.0;
    for (const auto &keyframe : keyframes) {
        if (keyframe.gainDb <= gainDb)
            last = keyframe.timeSeconds;
    }
    return last;
}

} // namespace

int runAutoDuckingSelftest()
{
    using namespace audio::autoducking;

    std::cout << "[auto-ducking] selftest start\n";
    int passed = 0;
    int failed = 0;

    AutoDuckingConfig config;
    config.sampleRate = 1000.0;
    config.voiceThreshold = 0.20;
    config.attackSeconds = 0.050;
    config.releaseSeconds = 0.200;
    config.duckedGainDb = -15.0;
    config.keyframeIntervalSeconds = 0.010;

    const std::size_t frameCount = static_cast<std::size_t>(2.000 * config.sampleRate);
    std::vector<float> voice = makeVoiceOver(frameCount, config.sampleRate);
    std::vector<float> bgm = makeStereoBgm(frameCount, config.sampleRate);
    const std::vector<float> originalVoice = voice;
    const std::vector<float> originalBgm = bgm;

    const AutoDuckingResult result = generateDuckingGainCurve(voice, 1, bgm, 2, config);

    requireGate(result.voiceEnvelope.size() == frameCount,
                "G1 voice envelope length",
                "expected one envelope point per VO frame",
                passed,
                failed);
    requireGate(result.bgmGainKeyframes.size() > 100,
                "G2 BGM keyframe track generated",
                "expected dense BGM gain keyframes for the synthetic track",
                passed,
                failed);

    requireGate(std::abs(gainAt(result.bgmGainKeyframes, 0.300)) <= 0.000001,
                "G3 pre-roll stays unity",
                "BGM gain changed before the VO threshold crossing",
                passed,
                failed);
    requireGate(gainAt(result.bgmGainKeyframes, 0.550) < -7.0
                    && gainAt(result.bgmGainKeyframes, 0.550) > -12.0,
                "G4 attack ramps down",
                "expected a partial gain reduction during attack",
                passed,
                failed);
    requireGate(gainAt(result.bgmGainKeyframes, 0.750) <= -14.5,
                "G5 sustained VO reaches ducked gain",
                "expected the curve to settle near the configured ducked gain",
                passed,
                failed);
    requireGate(gainAt(result.bgmGainKeyframes, 1.050) <= -11.0,
                "G6 release holds through VO tail",
                "expected release smoothing to keep BGM attenuated just after VO",
                passed,
                failed);
    requireGate(gainAt(result.bgmGainKeyframes, 1.800) > -1.0,
                "G7 release recovers",
                "expected BGM gain to return near unity after release",
                passed,
                failed);

    const double attackGateTime = firstTimeAtOrBelow(result.bgmGainKeyframes, -1.0);
    requireGate(attackGateTime >= 0.500 && attackGateTime <= 0.520,
                "G8 attack timing gate",
                "first attenuation keyframe landed outside the VO onset window",
                passed,
                failed);
    const double releaseGateTime = lastTimeAtOrBelow(result.bgmGainKeyframes, -1.0);
    requireGate(releaseGateTime >= 1.480 && releaseGateTime <= 1.560,
                "G9 release timing gate",
                "release recovery crossed the timing gate outside the expected window",
                passed,
                failed);

    requireGate(voice == originalVoice && bgm == originalBgm,
                "G10 non-destructive buffers",
                "VO or BGM source samples were modified",
                passed,
                failed);

    std::cout << "[auto-ducking] summary: "
              << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
