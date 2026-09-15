#include <QVector>

#include <cmath>
#include <iostream>

#include "../DialogueLeveler.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../UndoManager.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

QVector<float> sine(int sampleRate, double durationSec, double amplitudeDb,
                    double frequency = 1000.0)
{
    const int count = qRound(sampleRate * durationSec);
    const double amplitude = std::pow(10.0, amplitudeDb / 20.0);
    QVector<float> samples;
    samples.reserve(count);
    for (int i = 0; i < count; ++i) {
        samples.append(static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequency * i / sampleRate)));
    }
    return samples;
}

double envelopeGainAt(const QVector<AudioGainPoint> &envelope, double timeSec)
{
    if (envelope.isEmpty())
        return 1.0;
    if (timeSec <= envelope.first().time)
        return envelope.first().gain;
    for (int i = 1; i < envelope.size(); ++i) {
        if (timeSec <= envelope[i].time) {
            const AudioGainPoint &left = envelope[i - 1];
            const AudioGainPoint &right = envelope[i];
            const double span = right.time - left.time;
            if (span <= 0.0)
                return right.gain;
            const double f = (timeSec - left.time) / span;
            return left.gain + f * (right.gain - left.gain);
        }
    }
    return envelope.last().gain;
}

double rmsDb(const QVector<float> &samples, int sampleRate,
             double fromSec, double toSec,
             const QVector<AudioGainPoint> &envelope = {})
{
    const int sampleCount = static_cast<int>(samples.size());
    const int first = qBound(0, qRound(fromSec * sampleRate), sampleCount);
    const int last = qBound(first, qRound(toSec * sampleRate), sampleCount);
    if (last <= first)
        return -120.0;
    double sum = 0.0;
    for (int i = first; i < last; ++i) {
        const double gain = envelopeGainAt(
            envelope, static_cast<double>(i) / sampleRate);
        const double value = samples[i] * gain;
        sum += value * value;
    }
    const double rms = std::sqrt(sum / (last - first));
    return rms > 0.0 ? 20.0 * std::log10(rms) : -120.0;
}

bool sameEnvelope(const QVector<AudioGainPoint> &a,
                  const QVector<AudioGainPoint> &b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (a[i].time != b[i].time || a[i].gain != b[i].gain)
            return false;
    }
    return true;
}

} // namespace

int runDialogueLevelerSelftest()
{
    int passed = 0;
    int failed = 0;
    const auto pass = [&](const char *gate) {
        ++passed;
        std::cerr << "PASS " << gate << '\n';
    };
    const auto fail = [&](const char *gate, const QString &reason) {
        ++failed;
        std::cerr << "FAIL " << gate << " " << reason.toStdString() << '\n';
    };

    constexpr int sampleRate = 16000;
    QVector<float> stepped = sine(sampleRate, 4.0, -30.0);
    stepped += sine(sampleRate, 4.0, -10.0);
    leveler::Config levelingConfig;
    levelingConfig.targetShortTermLufs = -23.0;
    const QVector<AudioGainPoint> leveled = leveler::computeEnvelope(
        stepped, sampleRate, levelingConfig);
    const double beforeDifference = qAbs(
        rmsDb(stepped, sampleRate, 1.0, 3.0)
        - rmsDb(stepped, sampleRate, 5.0, 7.0));
    const double afterDifference = qAbs(
        rmsDb(stepped, sampleRate, 1.0, 3.0, leveled)
        - rmsDb(stepped, sampleRate, 5.0, 7.0, leveled));
    const bool g1 = !leveled.isEmpty()
        && beforeDifference >= 19.9 && afterDifference <= 3.0;
    g1 ? pass("G1")
       : fail("G1", QStringLiteral("RMS difference before=%1 after=%2")
                        .arg(beforeDifference).arg(afterDifference));

    QVector<float> silence(sampleRate * 2, 0.0f);
    const QVector<AudioGainPoint> silenceEnvelope =
        leveler::computeEnvelope(silence, sampleRate);
    bool silenceNotBoosted = !silenceEnvelope.isEmpty();
    for (const AudioGainPoint &point : silenceEnvelope)
        silenceNotBoosted = silenceNotBoosted && point.gain <= 1.0;
    silenceNotBoosted ? pass("G2")
                      : fail("G2", QStringLiteral("silence received positive gain"));

    const QVector<float> clampTone = sine(sampleRate, 2.0, -20.0);
    leveler::Config boostConfig;
    boostConfig.targetShortTermLufs = 0.0;
    boostConfig.smoothingSec = 0.0;
    leveler::Config cutConfig = boostConfig;
    cutConfig.targetShortTermLufs = -60.0;
    const QVector<AudioGainPoint> boosted = leveler::computeEnvelope(
        clampTone, sampleRate, boostConfig);
    const QVector<AudioGainPoint> cut = leveler::computeEnvelope(
        clampTone, sampleRate, cutConfig);
    const double boostLimit = std::pow(10.0, boostConfig.maxBoostDb / 20.0);
    const double cutLimit = std::pow(10.0, -cutConfig.maxCutDb / 20.0);
    bool clampsOk = !boosted.isEmpty() && !cut.isEmpty();
    for (const AudioGainPoint &point : boosted)
        clampsOk = clampsOk && qAbs(point.gain - boostLimit) <= 1.0e-9;
    for (const AudioGainPoint &point : cut)
        clampsOk = clampsOk && qAbs(point.gain - cutLimit) <= 1.0e-9;
    clampsOk ? pass("G3")
             : fail("G3", QStringLiteral("boost/cut clamp diverged"));

    const QVector<AudioGainPoint> deterministicA = leveler::computeEnvelope(
        stepped, sampleRate, levelingConfig);
    const QVector<AudioGainPoint> deterministicB = leveler::computeEnvelope(
        stepped, sampleRate, levelingConfig);
    sameEnvelope(deterministicA, deterministicB)
        ? pass("G4")
        : fail("G4", QStringLiteral("identical input produced different points"));

    Timeline timeline;
    TimelineTrack *audioTrack = timeline.trackAt(true, 0);
    bool g5 = false;
    QString applyError;
    if (audioTrack) {
        ClipInfo clip;
        clip.filePath = QStringLiteral("dialogue-leveler-synthetic.wav");
        clip.displayName = clip.filePath;
        clip.duration = 8.0;
        clip.outPoint = 8.0;
        audioTrack->setClips({clip});
        timeline.clearSelection();
        timeline.undoManager()->clear();
        timeline.undoManager()->saveState(
            timeline.currentState(), QStringLiteral("dialogue level baseline"));

        const bool applied = timeline.applyDialogueLevel(
            0, 0, deterministicA, &applyError);
        const bool stored = applied
            && sameEnvelope(audioTrack->clips().first().volumeEnvelope,
                            deterministicA);

        ProjectData project;
        project.audioTracks = timeline.allAudioTracks();
        const QString json = ProjectFile::toJsonString(project);
        ProjectData loaded;
        const bool loadedOk = ProjectFile::fromJsonString(json, loaded);
        const bool roundTrip = loadedOk && !loaded.audioTracks.isEmpty()
            && !loaded.audioTracks.first().isEmpty()
            && sameEnvelope(loaded.audioTracks.first().first().volumeEnvelope,
                            deterministicA);

        timeline.undo();
        const bool oneUndo = !timeline.canUndo()
            && audioTrack->clips().size() == 1
            && audioTrack->clips().first().volumeEnvelope.isEmpty();
        g5 = stored && roundTrip && oneUndo;
    }
    g5 ? pass("G5")
       : fail("G5", applyError.isEmpty()
                         ? QStringLiteral("apply, undo, or persistence failed")
                         : applyError);

    std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    return failed;
}
