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
    QVector<AudioChannelMode> channelModes;
    QMap<int, trackfx::Chain> trackChains;
    QMap<int, AudioEQConfig> legacyEqConfigs;
    QMap<int, std::array<AudioMixer::EqBandCoefs, 3>> legacyEqCoeffs;
    QMap<int, bool> legacyEqEnabled;
    QMap<int, double> trackGains;
    QMap<int, double> trackPans;
    // US-1122: bus chain + AUX sends per track (AudioBusRouting::resolveTrackRouteGains).
    // Track mute / solo stay out of the export (decision 3-3).
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

// GUI thread only: reads the Timeline, the mixer and the global playback bindings.
MixPlan buildMixPlan(Timeline *timeline, AudioMixer *mixer, const MixOptions &options);
// Touches no Timeline / QObject / MainWindow. Returns plan.outputPath, or empty on failure
// (a partly written output is removed). cancelled is polled before each ffmpeg run.
QString executeMixPlan(const MixPlan &plan, const std::function<bool()> &cancelled, QString *error);
// Measures the PCM mix (mixPath; empty = no mix) over the plan's measure range, then converts it
// to the AAC intermediate the queue muxes. Registers temp files on the job; returns the status.
QString measureAndConvert(const MixPlan &plan, const QString &mixPath, RenderJob &job, QString *error);

// Selftest: buildMixPlan calls on the GUI thread / on any other thread since process start.
int buildMixPlanGuiThreadCallsForTest();
int buildMixPlanOtherThreadCallsForTest();
}

namespace audiofxexport {
void setMasterEqBypassForTest(bool bypass);
quint64 masterEqPassesForTest();
}
