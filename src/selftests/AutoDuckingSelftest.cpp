#include "../audio/AutoDucking.h"
#include "../AudioExportMix.h"

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

bool sameKeyframes(const std::vector<audio::autoducking::GainKeyframe> &a,
                   const std::vector<audio::autoducking::GainKeyframe> &b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i].timeSeconds - b[i].timeSeconds) > 0.000001)
            return false;
        if (std::abs(a[i].gainDb - b[i].gainDb) > 0.000001)
            return false;
    }
    return true;
}

struct FakeBgmClip {
    std::vector<audio::autoducking::GainKeyframe> volumeEnvelope;
};

bool applyDuckingAsOneUndo(FakeBgmClip &clip,
                           const std::vector<audio::autoducking::GainKeyframe> &keyframes,
                           std::vector<FakeBgmClip> &undoStack)
{
    if (keyframes.empty())
        return false;
    undoStack.push_back(clip);
    clip.volumeEnvelope = keyframes;
    return true;
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

    const std::size_t intervalFrames = static_cast<std::size_t>(
        std::max(1.0, std::round(config.keyframeIntervalSeconds * config.sampleRate)));
    const std::size_t maxExpectedKeyframes = frameCount / intervalFrames + 2;
    requireGate(result.bgmGainKeyframes.size() <= maxExpectedKeyframes,
                "G11 keyframes are interval-decimated",
                "BGM curve kept per-sample keyframes instead of interval-spaced GainKeyframes",
                passed,
                failed);

    FakeBgmClip clip;
    const FakeBgmClip beforeApply = clip;
    std::vector<FakeBgmClip> undoStack;
    const bool applied = applyDuckingAsOneUndo(clip, result.bgmGainKeyframes, undoStack);
    requireGate(applied && undoStack.size() == 1,
                "G12 apply records one undo snapshot",
                "expected one logical undo snapshot for one ducking apply operation",
                passed,
                failed);
    if (!undoStack.empty())
        clip = undoStack.back();
    requireGate(sameKeyframes(clip.volumeEnvelope, beforeApply.volumeEnvelope),
                "G13 one undo restores pre-apply envelope",
                "expected one undo restore to remove the applied BGM envelope",
                passed,
                failed);

    std::vector<float> silentVoice(frameCount, 0.0f);
    const std::vector<float> silentVoiceOriginal = silentVoice;
    const std::vector<float> silentBgmOriginal = bgm;
    const AutoDuckingResult unused = generateDuckingGainCurve(silentVoice, 1, bgm, 2, config);
    requireGate(unused.bgmGainKeyframes.empty(),
                "G14 unused path stores no unity keyframes",
                "silent sidechain should not create no-op BGM envelope data",
                passed,
                failed);
    requireGate(silentVoice == silentVoiceOriginal && bgm == silentBgmOriginal,
                "G15 unused path is byte-identical",
                "silent-sidechain analysis changed input buffers",
                passed,
                failed);

    // G16: the merge applied to a clip's existing envelope (US-1820 C-01). Expected
    // values are stated literally: a unity envelope ducked to 0.2 at t=1 must become the
    // three points {0:1, 1:0.2, 2:1}, so the midpoints interpolate to 0.6; points within
    // 1 ms collapse to one point at the first time with the minimum gain.
    {
        const double eps = 1e-9;
        const QVector<AudioGainPoint> existing{{0.0, 1.0}, {1.0, 1.0}, {2.0, 1.0}};
        const QVector<AudioGainPoint> ducking{{0.0, 1.0}, {1.0, 0.2}, {2.0, 1.0}};
        const QVector<AudioGainPoint> merged =
            audioexport::mergeDuckingEnvelope(existing, ducking, 1.0);
        const QVector<AudioGainPoint> expected{{0.0, 1.0}, {1.0, 0.2}, {2.0, 1.0}};
        bool pointsOk = merged.size() == expected.size();
        for (int i = 0; pointsOk && i < expected.size(); ++i) {
            pointsOk = std::abs(merged[i].time - expected[i].time) <= eps
                       && std::abs(merged[i].gain - expected[i].gain) <= eps;
        }
        const double atHalf = audio::evaluateVolumeEnvelope(merged, 0.5, 1.0);
        const double atOneHalf = audio::evaluateVolumeEnvelope(merged, 1.5, 1.0);
        const bool midpointsOk = std::abs(atHalf - 0.6) <= eps
                                 && std::abs(atOneHalf - 0.6) <= eps;

        const QVector<AudioGainPoint> nearExisting{{1.0005, 0.9}};
        const QVector<AudioGainPoint> nearDucking{{1.0, 0.3}};
        const QVector<AudioGainPoint> nearMerged =
            audioexport::mergeDuckingEnvelope(nearExisting, nearDucking, 1.0);
        const bool nearOk = nearMerged.size() == 1
                            && std::abs(nearMerged.first().time - 1.0) <= eps
                            && std::abs(nearMerged.first().gain - 0.3) <= eps;

        std::cerr << "[auto-ducking] G16 merged points=" << merged.size();
        for (const AudioGainPoint &p : merged)
            std::cerr << " {" << p.time << ':' << p.gain << '}';
        std::cerr << " gain(0.5)=" << atHalf << " gain(1.5)=" << atOneHalf
                  << " near points=" << nearMerged.size();
        for (const AudioGainPoint &p : nearMerged)
            std::cerr << " {" << p.time << ':' << p.gain << '}';
        std::cerr << '\n';
        requireGate(pointsOk && midpointsOk && nearOk,
                    "G16 ducking merge collapses same-time points to the minimum",
                    "expected {0:1,1:0.2,2:1} with gain 0.6 at t=0.5 and t=1.5, "
                    "and {1.0005:0.9}+{1:0.3} collapsing to {1:0.3}",
                    passed,
                    failed);
    }

    std::cout << "[auto-ducking] summary: "
              << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
