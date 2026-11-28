#pragma once

#include "AudioEQ.h"
#include "AudioMixer.h"
#include "AudioTrackFx.h"
#include "PlaybackTypes.h"
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <functional>
#include <vector>

class Timeline;
struct ClipInfo;
struct RenderJob;
enum class AudioChannelMode; // Timeline.h

// Shared by audio export and its DSP regression gates. Entries are stereo 48kHz
// PreFx buffers; processing retains per-track histories and skips timeline gaps.
namespace audioexport {
struct DspEntry {
    qint64 frameStart = 0;
    std::vector<float> samples;
};
void processTrackEntries(std::vector<DspEntry> &entries, const trackfx::Chain &chain,
                         const std::array<AudioMixer::EqBandCoefs, 3> &coeffs,
                         bool eqEnabled, double preampDb);

// Runs one entry's per-clip EQ (fresh filter history) before the track chain sees it.
void processClipEq(DspEntry &entry, const trackfx::EqSettings &eq);

QString findFfmpegBinary();
QString ffmpegNumber(double value);
QVector<AudioGainPoint> sortedDedupedEnvelope(QVector<AudioGainPoint> env);
// Auto-ducking: caps every existing point at the ducking curve (at the clamped fallbackGain
// outside the curve's span) and adds the ducking points. An existing and a ducking point
// within 1 ms become one point at the earlier time with the lower gain; points from one
// source stay, except the middle of a same-instant run (sortedDedupedEnvelope's rule).
// Applying the same curve again changes nothing.
QVector<AudioGainPoint> mergeDuckingEnvelope(const QVector<AudioGainPoint> &existing,
                                             const QVector<AudioGainPoint> &ducking,
                                             double fallbackGain);
QString nextExportAudioMixPath();
bool runFfmpegForAudioMix(const QStringList &args, QString *error);

struct MixOptions {
    // Non-empty: render-in-place / forced PCM mix (never skipped as passthrough).
    QString forcedOutputPath;
    double forcedDurationSeconds = 0.0;
    int audioKbps = 192;
};

// Value snapshot of everything the export mix reads from the Timeline, the mixer and the
// playback bindings. executeMixPlan works from this alone.
struct MixPlan {
    bool passthrough = false; // no mix needed: the caller muxes the source audio as is
    QVector<PlaybackEntry> entries; // mixable entries in playback-sequence order
    QVector<bool> reversed;
    // Per entry: the resample factor of exportAudioRampFactor (1.0 = no speed stage).
    QVector<double> rampFactors;
    QVector<AudioChannelMode> channelModes;
    QMap<int, trackfx::Chain> trackChains;
    QMap<int, AudioEQConfig> legacyEqConfigs;
    QMap<int, std::array<AudioMixer::EqBandCoefs, 3>> legacyEqCoeffs;
    QMap<int, bool> legacyEqEnabled;
    QMap<int, double> trackGains;
    QMap<int, double> trackPans;
    // US-1122: bus chain + AUX sends per track (AudioBusRouting::resolveTrackRouteGains).
    // Track mute reaches the export as PlaybackEntry::audioMuted (skipped in buildMixPlan);
    // track solo is applied in buildMixPlan (trackAudible: any solo -> only soloed tracks sound).
    QMap<int, audiobus::RouteGains> routeGains;
    bool useTrackFx = false;
    trackfx::Chain masterChain;
    bool useMasterEq = false;
    double durationSeconds = 0.0;
    int audioKbps = 192;
    QString outputPath;
    bool renderInPlace = false;
    // Auto loudness (filled on the GUI thread by loudnessauto::prepareJob).
    bool trimMeasurement = false;
    double measureStartSec = 0.0;
    double measureEndSec = 0.0;
    bool failAacConvertForTest = false;
};

// The export audio speed factor of the audio clip an entry plays (the clip at the entry's
// sourceTrack / sourceClipIndex, as the preview mixer's ramp list): entrySpeed x s, the
// preview mixer's speedMul, when the entry runs forward, the clip is not a sequence
// reference (a nested entry names its parent reference), the clip follows speed
// (resolveAudioAtempoEnabled(envForce, clip.atempoEnabled)) and its ramp is non-identity
// with one speed s on every keyframe. 1.0 (no stage) otherwise. *nonConstant is set when
// such a clip's ramp has keyframes of different speeds (export keeps it ramp-free).
double exportAudioRampFactor(const ClipInfo &clip, bool reversed, double entrySpeed,
                             bool envForce, bool *nonConstant = nullptr);
// GUI thread only: reads the Timeline, the mixer and the global playback bindings.
MixPlan buildMixPlan(Timeline *timeline, AudioMixer *mixer, const MixOptions &options);
// Selftest: the filter text executeMixPlan hands ffmpeg for plan entry `index`: the single
// chain (no track DSP), and the track-FX path's clip wav PreFx chain and PostFx chain.
QString singleChainForTest(const MixPlan &plan, int index);
QString trackFxPreChainForTest(const MixPlan &plan, int index);
QString trackFxPostChainForTest(const MixPlan &plan, int index);
// Touches no Timeline / QObject / MainWindow. Returns plan.outputPath, or empty on failure
// (a partly written output is removed). cancelled is polled before each ffmpeg run.
QString executeMixPlan(const MixPlan &plan, const std::function<bool()> &cancelled, QString *error);
// Selftest: executeMixPlan sleeps ms (on its own thread) before anything else. 0 (default) = none.
void setPrepareDelayForTest(int ms);
// Measures the PCM mix (mixPath; empty = no mix) over the plan's measure range, then converts it
// to the AAC intermediate the queue muxes. Registers temp files on the job; returns the status.
// cancelled is polled before each ffmpeg run: once true it returns at once, with mixPath left in
// the job's temp files and job.projectFilePath unchanged.
QString measureAndConvert(const MixPlan &plan, const QString &mixPath, RenderJob &job, QString *error,
                          const std::function<bool()> &cancelled = {});

// Selftest: buildMixPlan calls on the GUI thread / on any other thread since process start.
int buildMixPlanGuiThreadCallsForTest();
int buildMixPlanOtherThreadCallsForTest();
}

namespace audiofxexport {
void setMasterEqBypassForTest(bool bypass);
quint64 masterEqPassesForTest();
}
