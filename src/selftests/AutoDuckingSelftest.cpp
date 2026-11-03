#include "../audio/AutoDucking.h"
#include "../AudioExportMix.h"
#include "../ProjectFile.h"
#include "SelftestGate.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

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

using Env = QVector<AudioGainPoint>;

std::string pointsText(const Env &env)
{
    std::string text = std::to_string(env.size()) + " points";
    for (const AudioGainPoint &p : env)
        text += " {" + std::to_string(p.time) + ':' + std::to_string(p.gain) + '}';
    return text;
}

bool samePoints(const Env &a, const Env &b, double eps)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (std::abs(a[i].time - b[i].time) > eps || std::abs(a[i].gain - b[i].gain) > eps)
            return false;
    }
    return true;
}

Env duckedGrid()
{
    Env grid;
    for (int i = 0; i <= 20; ++i)
        grid.append({1.0 + i * 0.05, 0.2});
    return grid;
}

} // namespace

int runAutoDuckingSelftest()
{
    using namespace audio::autoducking;

    std::cout << "[auto-ducking] selftest start\n";
    selftest::GateTally tally;

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

    tally.gate(1, result.voiceEnvelope.size() == frameCount,
               "voice envelope length: expected one envelope point per VO frame");
    tally.gate(2, result.bgmGainKeyframes.size() > 100,
               "BGM keyframe track generated: expected dense BGM gain keyframes for the synthetic track");

    tally.gate(3, std::abs(gainAt(result.bgmGainKeyframes, 0.300)) <= 0.000001,
               "pre-roll stays unity: BGM gain changed before the VO threshold crossing");
    tally.gate(4, gainAt(result.bgmGainKeyframes, 0.550) < -7.0
                    && gainAt(result.bgmGainKeyframes, 0.550) > -12.0,
               "attack ramps down: expected a partial gain reduction during attack");
    tally.gate(5, gainAt(result.bgmGainKeyframes, 0.750) <= -14.5,
               "sustained VO reaches ducked gain: expected the curve to settle near the configured ducked gain");
    tally.gate(6, gainAt(result.bgmGainKeyframes, 1.050) <= -11.0,
               "release holds through VO tail: expected release smoothing to keep BGM attenuated just after VO");
    tally.gate(7, gainAt(result.bgmGainKeyframes, 1.800) > -1.0,
               "release recovers: expected BGM gain to return near unity after release");

    const double attackGateTime = firstTimeAtOrBelow(result.bgmGainKeyframes, -1.0);
    tally.gate(8, attackGateTime >= 0.500 && attackGateTime <= 0.520,
               "attack timing gate: first attenuation keyframe landed outside the VO onset window");
    const double releaseGateTime = lastTimeAtOrBelow(result.bgmGainKeyframes, -1.0);
    tally.gate(9, releaseGateTime >= 1.480 && releaseGateTime <= 1.560,
               "release timing gate: release recovery crossed the timing gate outside the expected window");

    tally.gate(10, voice == originalVoice && bgm == originalBgm,
               "non-destructive buffers: VO or BGM source samples were modified");

    const std::size_t intervalFrames = static_cast<std::size_t>(
        std::max(1.0, std::round(config.keyframeIntervalSeconds * config.sampleRate)));
    const std::size_t maxExpectedKeyframes = frameCount / intervalFrames + 2;
    tally.gate(11, result.bgmGainKeyframes.size() <= maxExpectedKeyframes,
               "keyframes are interval-decimated: BGM curve kept per-sample keyframes instead of interval-spaced GainKeyframes");

    FakeBgmClip clip;
    const FakeBgmClip beforeApply = clip;
    std::vector<FakeBgmClip> undoStack;
    const bool applied = applyDuckingAsOneUndo(clip, result.bgmGainKeyframes, undoStack);
    tally.gate(12, applied && undoStack.size() == 1,
               "apply records one undo snapshot: expected one logical undo snapshot for one ducking apply operation");
    if (!undoStack.empty())
        clip = undoStack.back();
    tally.gate(13, sameKeyframes(clip.volumeEnvelope, beforeApply.volumeEnvelope),
               "one undo restores pre-apply envelope: expected one undo restore to remove the applied BGM envelope");

    std::vector<float> silentVoice(frameCount, 0.0f);
    const std::vector<float> silentVoiceOriginal = silentVoice;
    const std::vector<float> silentBgmOriginal = bgm;
    const AutoDuckingResult unused = generateDuckingGainCurve(silentVoice, 1, bgm, 2, config);
    tally.gate(14, unused.bgmGainKeyframes.empty(),
               "unused path stores no unity keyframes: silent sidechain should not create no-op BGM envelope data");
    tally.gate(15, silentVoice == silentVoiceOriginal && bgm == silentBgmOriginal,
               "unused path is byte-identical: silent-sidechain analysis changed input buffers");

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
        tally.gate(16, pointsOk && midpointsOk && nearOk,
                   "ducking merge collapses same-time points to the minimum: "
                   "expected {0:1,1:0.2,2:1} with gain 0.6 at t=0.5 and t=1.5, "
                   "and {1.0005:0.9}+{1:0.3} collapsing to {1:0.3}");
    }

    const double eps = 1e-9;
    const Env spikeExisting{{0.0, 1.0}, {1.02, 1.0}, {2.0, 1.0}};
    const Env spikeDucking{{1.00, 0.2}, {1.05, 0.2}};
    const Env spikeMerged = audioexport::mergeDuckingEnvelope(spikeExisting, spikeDucking, 1.0);

    // G17 (HEAD FAIL: 1.02 stays at 1.0): an existing point inside the ducked span is
    // lowered to the curve, so no spike rises out of it.
    {
        const Env expected{{0.0, 1.0}, {1.0, 0.2}, {1.02, 0.2}, {1.05, 0.2}, {2.0, 1.0}};
        double peak = 0.0;
        for (int ms = 0; ms <= 50; ++ms)
            peak = std::max(peak, audio::evaluateVolumeEnvelope(spikeMerged, 1.0 + ms * 0.001, 1.0));
        const bool pointsOk = samePoints(spikeMerged, expected, eps);
        tally.gate(17, pointsOk && peak <= 0.2 + eps,
                   "merged " + pointsText(spikeMerged) + " peak[1.00,1.05]=" + std::to_string(peak));
    }

    // G18 (pin, HEAD PASS): off-grid ramp. Inside the span the result is the lower of the
    // existing ramp and the duck; existing points outside the span keep their gain.
    const Env rampExisting{{0.0, 1.0}, {0.5, 0.4}, {3.0, 1.0}};
    const Env rampMerged = audioexport::mergeDuckingEnvelope(rampExisting, duckedGrid(), 1.0);
    {
        double worst = 0.0;
        for (int ms = 0; ms <= 1000; ++ms) {
            const double t = 1.0 + ms * 0.001;
            const double want = qMin(audio::evaluateVolumeEnvelope(rampExisting, t, 1.0), 0.2);
            worst = std::max(worst,
                             std::abs(audio::evaluateVolumeEnvelope(rampMerged, t, 1.0) - want));
        }
        bool existingOk = true;
        for (const AudioGainPoint &want : rampExisting) {
            const auto hit = std::find_if(rampMerged.cbegin(), rampMerged.cend(),
                                          [&](const AudioGainPoint &p) {
                                              return std::abs(p.time - want.time) <= eps;
                                          });
            existingOk = existingOk && hit != rampMerged.cend()
                         && std::abs(hit->gain - want.gain) <= eps;
        }
        tally.gate(18, worst <= eps && existingOk,
                   "worst " + std::to_string(worst) + " existing points kept "
                       + std::to_string(existingOk) + " merged " + std::to_string(rampMerged.size())
                       + " points");
    }

    // G19 (HEAD FAIL: the 1.0 step collapses to 0.5): a same-instant authored step stays,
    // in order; points from the two sources within 1 ms still become one (G16's near case).
    const Env stepExisting{{1.0, 1.0}, {1.0, 0.5}, {2.0, 0.5}};
    const Env stepDucking{{1.5, 0.2}, {1.55, 0.2}};
    const Env stepMerged = audioexport::mergeDuckingEnvelope(stepExisting, stepDucking, 1.0);
    {
        const Env expected{{1.0, 1.0}, {1.0, 0.5}, {1.5, 0.2}, {1.55, 0.2}, {2.0, 0.5}};
        const double before = audio::evaluateVolumeEnvelope(stepMerged, 0.999, 1.0);
        const Env nearMerged = audioexport::mergeDuckingEnvelope(Env{{1.0005, 0.9}}, Env{{1.0, 0.3}}, 1.0);
        const bool nearOk = samePoints(nearMerged, Env{{1.0, 0.3}}, eps);
        tally.gate(19, samePoints(stepMerged, expected, eps) && std::abs(before - 1.0) <= eps && nearOk,
                   "merged " + pointsText(stepMerged) + " gain(0.999)=" + std::to_string(before)
                       + " near " + pointsText(nearMerged));
    }

    // G20: applying the same curve again is a fixed point (within 1e-9), for the merged
    // results above and for an authored point just before a grid point: low, and on a
    // falling ramp. A ramp point whose re-merge read the interpolated value sinks by 0.002.
    {
        const Env lowExisting{{0.0, 1.0}, {1.0495, 0.1}, {2.0, 1.0}};
        const Env lowDucking{{1.00, 0.2}, {1.05, 0.2}, {1.10, 0.2}};
        const Env slopeExisting{{0.0, 1.0}, {1.0495, 0.3}, {2.0, 1.0}};
        const Env slopeDucking{{1.0, 0.5}, {1.05, 0.4}, {1.10, 0.1}, {1.15, 0.1}};
        const Env lowMerged = audioexport::mergeDuckingEnvelope(lowExisting, lowDucking, 1.0);
        const Env slopeMerged = audioexport::mergeDuckingEnvelope(slopeExisting, slopeDucking, 1.0);
        const struct { const char *name; Env merged; Env ducking; } cases[] = {
            {"spike", spikeMerged, spikeDucking},
            {"ramp", rampMerged, duckedGrid()},
            {"step", stepMerged, stepDucking},
            {"low", lowMerged, lowDucking},
            {"slope", slopeMerged, slopeDucking},
        };
        bool stable = true;
        std::string detail;
        for (const auto &c : cases) {
            const Env again = audioexport::mergeDuckingEnvelope(c.merged, c.ducking, 1.0);
            const Env third = audioexport::mergeDuckingEnvelope(again, c.ducking, 1.0);
            const bool same = samePoints(c.merged, again, eps) && samePoints(again, third, eps);
            stable = stable && same;
            detail += std::string(c.name) + '=' + (same ? "same" : pointsText(again)) + ' ';
        }
        const auto gainAtTime = [](const Env &env, double time) {
            for (const AudioGainPoint &p : env) {
                if (std::abs(p.time - time) <= 1e-9)
                    return p.gain;
            }
            return -1.0;
        };
        // The authored low points themselves stay, whatever the re-merges do.
        const double lowKept = gainAtTime(lowMerged, 1.0495);
        const double slopeKept = gainAtTime(slopeMerged, 1.0495);
        const Env sameSource =
            audioexport::mergeDuckingEnvelope(Env{{1.0, 1.0}, {1.0, 0.5}}, Env{{3.0, 0.2}, {3.05, 0.2}}, 1.0);
        const auto atOne = std::count_if(sameSource.cbegin(), sameSource.cend(),
                                         [](const AudioGainPoint &p) { return p.time == 1.0; });
        tally.gate(20, stable && std::abs(lowKept - 0.1) <= eps && std::abs(slopeKept - 0.3) <= eps
                           && atOne == 2,
                   detail + "low@1.0495=" + std::to_string(lowKept) + " slope@1.0495="
                       + std::to_string(slopeKept) + " points at 1.0=" + std::to_string(atOne));
    }

    // G21 (pin, not guaranteed to fail at HEAD): 20 same-time steps through the project
    // JSON come back in order. Half the steps go up, half down, so a swap is visible.
    {
        ClipInfo clip;
        clip.displayName = QStringLiteral("steps");
        clip.duration = 30.0;
        for (int i = 0; i < 20; ++i) {
            const double first = i % 2 == 0 ? 0.25 + i * 0.01 : 1.25 + i * 0.01;
            const double second = i % 2 == 0 ? 1.25 + i * 0.01 : 0.25 + i * 0.01;
            clip.volumeEnvelope.append({i * 0.5, first});
            clip.volumeEnvelope.append({i * 0.5, second});
        }
        ProjectData data;
        data.audioTracks = {{clip}};
        ProjectData loaded;
        const bool parsed = ProjectFile::fromJsonString(ProjectFile::toJsonString(data), loaded);
        const Env back = parsed && !loaded.audioTracks.isEmpty() && !loaded.audioTracks[0].isEmpty()
            ? loaded.audioTracks[0][0].volumeEnvelope : Env{};
        tally.gate(21, parsed && samePoints(back, clip.volumeEnvelope, 0.0),
                   "parsed " + std::to_string(parsed) + " in " + std::to_string(clip.volumeEnvelope.size())
                       + " out " + pointsText(back));
    }

    return tally.finish("auto-ducking");
}
