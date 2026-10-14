// US-704: persistent PCM fixture for the real timeline export audio mixer.
// Run from the repository root and compare data chunks across baseline/HEAD.
#include "../AudioMixer.h"
#include "../AudioTrackFx.h"
#include "../MainWindow.h"
#include "../RenderInPlace.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpToolRegistry.h"
#include "SelftestGate.h"

#include <QAction>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int kFixtureCount = 8;
// Negative-control variant (not part of the fixture set): legacy 1 kHz in the low-shelf slot.
constexpr int kLegacyWrongSlot = 100;
// G10 (US-1020): g's legacy EQ migrated by trackfx::eqFromLegacy, and the
// control that lands the migrated peaking band in the low shelf instead.
constexpr int kLegacyMigrated = 101;
constexpr int kMigratedWrongShelf = 102;
// US-1126: variant h (index 7) puts a 1 kHz peaking -12 dB clip EQ on the first clip. Controls:
// the same clip EQ with every band at 0 dB (coefficients are identity), and the -12 dB EQ stored
// but switched off.
constexpr int kClipEq = 7;
constexpr int kClipEqFlat = 103;
constexpr int kClipEqOff = 104;
const char *const kNames[kFixtureCount] = {"a", "b", "c", "d", "e", "f", "g", "h"};

quint16 readLe16(const char *p)
{
    const auto *u = reinterpret_cast<const unsigned char *>(p);
    return quint16(u[0]) | (quint16(u[1]) << 8);
}

quint32 readLe32(const char *p)
{
    const auto *u = reinterpret_cast<const unsigned char *>(p);
    return quint32(u[0]) | (quint32(u[1]) << 8)
        | (quint32(u[2]) << 16) | (quint32(u[3]) << 24);
}

// RIFF chunks may include metadata before or after PCM and need word padding.
// Only the data payload is part of the byte comparison.
bool readPcmData(const QString &path, QByteArray *pcm, QString *error,
                 int *channels = nullptr, int *sampleRate = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    const QByteArray wav = file.readAll();
    if (wav.size() < 12 || wav.left(4) != "RIFF" || wav.mid(8, 4) != "WAVE") {
        *error = QStringLiteral("invalid RIFF/WAVE: %1").arg(path);
        return false;
    }
    bool pcm16 = false;
    bool foundData = false;
    qsizetype offset = 12;
    while (offset + 8 <= wav.size()) {
        const char *chunk = wav.constData() + offset;
        const quint32 length = readLe32(chunk + 4);
        const qsizetype payload = offset + 8;
        if (quint64(length) > quint64(wav.size() - payload)) {
            *error = QStringLiteral("truncated WAV chunk: %1").arg(path);
            return false;
        }
        if (QByteArrayView(chunk, 4) == QByteArrayView("fmt ", 4) && length >= 16) {
            pcm16 = readLe16(wav.constData() + payload) == 1
                && readLe16(wav.constData() + payload + 14) == 16;
            if (channels) *channels = readLe16(wav.constData() + payload + 2);
            if (sampleRate) *sampleRate = int(readLe32(wav.constData() + payload + 4));
        } else if (QByteArrayView(chunk, 4) == QByteArrayView("data", 4)) {
            *pcm = wav.mid(payload, length);
            foundData = true;
        }
        offset = payload + length + (length & 1u);
    }
    if (!pcm16 || !foundData || pcm->isEmpty() || (pcm->size() & 1)) {
        *error = QStringLiteral("missing nonempty PCM16 data chunk: %1").arg(path);
        return false;
    }
    return true;
}

double rms(const QByteArray &pcm)
{
    if (pcm.isEmpty()) return 0.0;
    double squares = 0.0;
    const qsizetype samples = pcm.size() / 2;
    for (qsizetype i = 0; i < samples; ++i) {
        const qint16 sample = static_cast<qint16>(readLe16(pcm.constData() + 2 * i));
        const double normalized = double(sample) / 32768.0;
        squares += normalized * normalized;
    }
    return std::sqrt(squares / double(samples));
}

// Writes a 48 kHz mono PCM16 sine WAV (G8 in-band source: e2e_hum.wav is a pure
// 50 Hz sine and carries no 1 kHz energy).
bool writeToneWav(const QString &path, double freqHz, double amplitude, double seconds)
{
    constexpr int rate = 48000;
    const quint32 frames = quint32(seconds * rate);
    const quint32 dataBytes = frames * 2;
    QByteArray wav;
    const auto put32 = [&](quint32 v) { for (int i = 0; i < 4; ++i) wav.append(char((v >> (8 * i)) & 0xff)); };
    const auto put16 = [&](quint16 v) { for (int i = 0; i < 2; ++i) wav.append(char((v >> (8 * i)) & 0xff)); };
    wav.append("RIFF", 4); put32(36 + dataBytes); wav.append("WAVEfmt ", 8);
    put32(16); put16(1); put16(1); put32(rate); put32(rate * 2); put16(2); put16(16);
    wav.append("data", 4); put32(dataBytes);
    for (quint32 i = 0; i < frames; ++i)
        put16(quint16(qint16(std::lround(amplitude * 32767.0 * std::sin(2.0 * M_PI * freqHz * i / rate)))));
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(wav) == wav.size();
}

ClipInfo humClip(double in, double out, const QString &source = QStringLiteral("test_assets/e2e_hum.wav"))
{
    ClipInfo clip;
    clip.filePath = source;
    clip.displayName = QStringLiteral("audio-fixture");
    clip.duration = 5.0;
    clip.inPoint = in;
    clip.outPoint = out;
    return clip;
}

// Variant e: 200 Hz low shelf -12 dB (50 Hz sits inside the shelf).
// Variant f: 1 kHz peaking -12 dB (50 Hz is outside the band: frequency control).
// Variant g: the same 1 kHz peaking -12 dB through the legacy 3-band AudioEQConfig.
trackfx::Chain eqVariantChain(int variant)
{
    trackfx::Chain chain;
    chain.eqEnabled = true;
    if (variant == 4) chain.eq.low = trackfx::EqBand{200.0, -12.0, 0.7};
    if (variant == 5) chain.eq.lowMid = trackfx::EqBand{1000.0, -12.0, 1.0};
    return chain;
}

// Legacy EQ band slots are fixed: 0 = low shelf, 1 = peaking, 2 = high shelf
// (AudioMixer::setTrackEqConfig). A peaking band therefore lives in slot 1.
// peakingSlot = 0 builds the wrong-filter negative control (low shelf at 1 kHz).
AudioEQConfig legacyEqConfig(int peakingSlot)
{
    AudioEQConfig cfg;
    cfg.bands.resize(peakingSlot + 1);
    cfg.bands[peakingSlot] = EQBand{1000.0, -12.0, 1.0};
    return cfg;
}

// RMS over the steady window [0.1 s, 0.4 s) of interleaved PCM16.
double windowRms(const QByteArray &pcm, int channels, int sampleRate)
{
    if (channels <= 0 || sampleRate <= 0) return 0.0;
    const qsizetype begin = qsizetype(0.1 * sampleRate) * channels * 2;
    const qsizetype end = qMin(pcm.size(), qsizetype(0.4 * sampleRate) * channels * 2);
    if (end <= begin) return 0.0;
    return rms(pcm.mid(begin, end - begin));
}

// Expected attenuation: the same Chain run through trackfx::Processor on a pure
// 50 Hz sine, measured over the same steady window (no hand-computed values).
double expectedRatioAt(const trackfx::Chain &chain, double freqHz, int channels, int sampleRate)
{
    if (channels <= 0 || sampleRate <= 0) return 0.0;
    const int frames = sampleRate / 2;
    QVector<float> in(frames * channels);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            in[i * channels + c] = float(0.5 * std::sin(2.0 * M_PI * freqHz * i / sampleRate));
    QVector<float> out = in;
    trackfx::Processor processor(chain, sampleRate, channels);
    processor.process(out.data(), frames);
    const auto toPcm = [&](const QVector<float> &v) {
        QByteArray bytes(v.size() * 2, 0);
        for (qsizetype i = 0; i < v.size(); ++i) {
            const qint16 q = qint16(std::lround(qBound(-1.0f, v[i], 1.0f) * 32767.0f));
            bytes[2 * i] = char(q & 0xff);
            bytes[2 * i + 1] = char((q >> 8) & 0xff);
        }
        return bytes;
    };
    const double inRms = windowRms(toPcm(in), channels, sampleRate);
    return inRms > 0.0 ? windowRms(toPcm(out), channels, sampleRate) / inRms : 0.0;
}

double expectedRatio(int variant, int channels, int sampleRate)
{
    return expectedRatioAt(eqVariantChain(variant), 50.0, channels, sampleRate);
}

// Variant h's clip EQ (and the flat control's): 1 kHz peaking -12 dB.
trackfx::EqSettings clipEqFor(int variant)
{
    trackfx::EqSettings eq;
    if (variant != kClipEqFlat) eq.lowMid = trackfx::EqBand{1000.0, -12.0, 1.0};
    return eq;
}

// Interleaved PCM16 bytes of [fromSec, toSec).
QByteArray pcmRange(const QByteArray &pcm, int channels, int sampleRate, double fromSec, double toSec)
{
    const qsizetype begin = qsizetype(fromSec * sampleRate) * channels * 2;
    const qsizetype end = qMin(pcm.size(), qsizetype(toSec * sampleRate) * channels * 2);
    return end > begin ? pcm.mid(begin, end - begin) : QByteArray();
}

bool setupVariant(int variant, AudioMixer &mixer, Timeline &timeline, QString *error,
                  const QString &source)
{
    timeline.setAudioMixer(&mixer);
    ClipInfo first = humClip(0.0, 2.0, source);
    ClipInfo second = humClip(0.5, 2.5, source);
    first.trailOut.type = TransitionType::FadeOut;
    first.trailOut.duration = 0.3;
    if (variant == kClipEq || variant == kClipEqFlat || variant == kClipEqOff) {
        first.clipEq = clipEqFor(variant);
        first.clipEqEnabled = variant != kClipEqOff;
    }
    timeline.audioTracks().first()->setClips({first, second});
    if (variant == 1 || variant == 3)
        mixer.setTrackGain(0, 0.5);
    if (variant == 2 || variant == 3) {
        trackfx::Chain chain;
        chain.eqEnabled = true; // four default 0 dB EQ bands
        mixer.setTrackChain(0, chain);
    }
    if (variant == 4 || variant == 5)
        mixer.setTrackChain(0, eqVariantChain(variant));
    if (variant == 6 || variant == kLegacyWrongSlot || variant == kLegacyMigrated
        || variant == kMigratedWrongShelf) {
        mixer.setTrackEqEnabled(0, true);
        mixer.setTrackEqConfig(0, legacyEqConfig(variant == kLegacyWrongSlot ? 0 : 1));
    }
    if (variant == kLegacyMigrated || variant == kMigratedWrongShelf) {
        trackfx::Chain chain = mixer.trackChain(0);
        if (!trackfx::eqFromLegacy(mixer.trackEqConfig(0), &chain.eq)) {
            *error = QStringLiteral("eqFromLegacy rejected variant g's legacy EQ");
            return false;
        }
        if (variant == kMigratedWrongShelf) std::swap(chain.eq.low, chain.eq.lowMid);
        chain.eqEnabled = true;
        mixer.setTrackChain(0, chain);
        mixer.setTrackEqEnabled(0, false);
        mixer.setTrackEqConfig(0, AudioEQConfig{});
    }
    return true;
}

bool generate(int variant, const QString &path, QByteArray *pcm, QString *error,
              int *channels = nullptr, int *sampleRate = nullptr,
              const QString &source = QStringLiteral("test_assets/e2e_hum.wav"))
{
    AudioMixer mixer;
    Timeline timeline;
    if (!setupVariant(variant, mixer, timeline, error, source)) return false;
    double audioDuration = 0.0;
    for (const TimelineTrack *track : timeline.audioTracks()) {
        double trackDuration = 0.0;
        for (const ClipInfo &clip : track->clips())
            trackDuration += qMax(0.0, clip.leadInSec) + clip.effectiveDuration();
        audioDuration = qMax(audioDuration, trackDuration);
    }
    const double durationSeconds = qMax(timeline.totalDuration(), audioDuration);
    std::fprintf(stderr, "G1 %s timelineDurationSec=%.12g\n",
                 variant < kFixtureCount ? kNames[variant] : "control", durationSeconds);
    // This wrapper calls MainWindow.cpp's prepareTimelineAudioMixForExport
    // with the same forced WAV arguments.
    const QString written = renderinplace::prepareAudioMix(&timeline, path, durationSeconds, error);
    if (written != path) {
        if (error->isEmpty()) *error = QStringLiteral("export returned no WAV: %1").arg(path);
        return false;
    }
    return readPcmData(path, pcm, error, channels, sampleRate);
}

qsizetype differentBytes(const QByteArray &a, const QByteArray &b)
{
    qsizetype count = a.size() >= b.size() ? a.size() - b.size() : b.size() - a.size();
    for (qsizetype i = 0; i < qMin(a.size(), b.size()); ++i)
        count += a[i] != b[i];
    return count;
}

struct PcmComparison {
    bool identical = false;
    int maxDiffLsb = 0;
    qsizetype firstDiffSample = -1; // interleaved PCM16 sample index, -1 when none
    qsizetype lengthDiffSamples = 0;
};

// Byte-level comparison of two PCM16 payloads. A length mismatch counts as a
// difference at the first sample past the shorter payload.
PcmComparison comparePcm16(const QByteArray &a, const QByteArray &b)
{
    PcmComparison result;
    result.lengthDiffSamples = (a.size() >= b.size() ? a.size() - b.size() : b.size() - a.size()) / 2;
    const qsizetype common = qMin(a.size(), b.size()) / 2;
    for (qsizetype i = 0; i < common; ++i) {
        const auto sample = [](const QByteArray &pcm, qsizetype index) {
            return qint16(readLe16(pcm.constData() + 2 * index));
        };
        const int diff = std::abs(int(sample(a, i)) - int(sample(b, i)));
        if (diff != 0 && result.firstDiffSample < 0) result.firstDiffSample = i;
        result.maxDiffLsb = qMax(result.maxDiffLsb, diff);
    }
    if (a.size() != b.size() && result.firstDiffSample < 0) result.firstDiffSample = common;
    result.identical = a == b;
    return result;
}

} // namespace

int runAudioFixtureSelftest()
{
    selftest::GateTally tally;
    const QString source = QStringLiteral("test_assets/e2e_hum.wav");
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!QFile::exists(source) || ffmpeg.isEmpty()) {
        std::fprintf(stderr, "audio-fixture prerequisites: source=%d ffmpeg=%s (cwd must be repository root)\n",
                     QFile::exists(source), ffmpeg.toUtf8().constData());
        for (int gate = 1; gate <= 12; ++gate)
            tally.skip(gate, "test_assets/e2e_hum.wav or ffmpeg unavailable");
        return tally.finish();
    }

    const QString outputPath = qEnvironmentVariable("VEDITOR_AUDIO_FIXTURE_OUT");
    if (outputPath.isEmpty()) {
        tally.gate(1, false, "VEDITOR_AUDIO_FIXTURE_OUT is required");
        for (int gate = 2; gate <= 12; ++gate)
            tally.skip(gate, "G1 output directory is unset");
        return tally.finish();
    }
    if (!QDir().mkpath(outputPath)) {
        tally.gate(1, false, "VEDITOR_AUDIO_FIXTURE_OUT cannot be created");
        for (int gate = 2; gate <= 12; ++gate)
            tally.skip(gate, "G1 output directory unavailable");
        return tally.finish();
    }
    const QDir out(outputPath);
    bool outputCleared = true;
    for (const char *name : kNames) {
        const QString path = out.filePath(QString::fromLatin1(name) + QStringLiteral(".wav"));
        if (QFile::exists(path) && !QFile::remove(path)) {
            std::fprintf(stderr, "G1 cannot remove stale WAV: %s\n", path.toUtf8().constData());
            outputCleared = false;
        }
    }
    const QString jsonPath = out.filePath(QStringLiteral("audio_fixture.json"));
    if (QFile::exists(jsonPath) && !QFile::remove(jsonPath)) {
        std::fprintf(stderr, "G1 cannot remove stale JSON: %s\n", jsonPath.toUtf8().constData());
        outputCleared = false;
    }
    if (!outputCleared) {
        tally.gate(1, false, "stale audio fixture output cannot be removed");
        for (int gate = 2; gate <= 12; ++gate)
            tally.skip(gate, "G1 output cleanup failed");
        return tally.finish();
    }
    QByteArray pcm[kFixtureCount];
    bool generated[kFixtureCount] = {};
    bool repeatEqual[kFixtureCount] = {};
    int channels[kFixtureCount] = {};
    int sampleRates[kFixtureCount] = {};
    QJsonObject manifest;
    for (int i = 0; i < kFixtureCount; ++i) {
        const QString name = QString::fromLatin1(kNames[i]);
        const QString path = out.filePath(name + QStringLiteral(".wav"));
        QString error;
        generated[i] = generate(i, path, &pcm[i], &error, &channels[i], &sampleRates[i]);
        const double level = rms(pcm[i]);
        std::fprintf(stderr, "G1 %s path=%s dataBytes=%lld rms=%.12g error=%s\n",
                     kNames[i], path.toUtf8().constData(), static_cast<long long>(pcm[i].size()),
                     level, error.toUtf8().constData());
        if (generated[i]) {
            manifest.insert(name, QJsonObject{
                {QStringLiteral("dataBytes"), double(pcm[i].size())},
                {QStringLiteral("pcmSha256"), QString::fromLatin1(
                    QCryptographicHash::hash(pcm[i], QCryptographicHash::Sha256).toHex())},
                {QStringLiteral("rms"), level}});
        }
    }
    QFile json(jsonPath);
    const QByteArray jsonBytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    const bool jsonWritten = json.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && json.write(jsonBytes) == jsonBytes.size() && json.flush();
    bool allGenerated = jsonWritten;
    for (bool ok : generated) allGenerated &= ok;
    std::fprintf(stderr, "G1 json=%s bytes=%lld written=%d\n",
                 json.fileName().toUtf8().constData(), static_cast<long long>(jsonBytes.size()), jsonWritten);
    tally.gate(1, allGenerated, "eight PCM WAV files and audio_fixture.json required");

    QTemporaryDir repeat(out.filePath(QStringLiteral("repeat_XXXXXX")));
    bool repeated = repeat.isValid();
    for (int i = 0; i < kFixtureCount; ++i) {
        QByteArray second;
        QString error;
        const bool ok = repeat.isValid() && generate(i, QDir(repeat.path()).filePath(
            QString::fromLatin1(kNames[i]) + QStringLiteral(".wav")), &second, &error);
        const qsizetype difference = ok ? differentBytes(pcm[i], second) : -1;
        std::fprintf(stderr, "G2 %s firstBytes=%lld secondBytes=%lld diffBytes=%lld error=%s\n",
                     kNames[i], static_cast<long long>(pcm[i].size()),
                     static_cast<long long>(second.size()), static_cast<long long>(difference),
                     error.toUtf8().constData());
        repeatEqual[i] = generated[i] && ok && difference == 0;
        repeated &= repeatEqual[i];
    }
    tally.gate(2, repeated, "two generations must have identical PCM data");

    const double a = rms(pcm[0]), b = rms(pcm[1]);
    const double c = rms(pcm[2]), d = rms(pcm[3]);
    const double abError = a > 0.0 ? std::abs(b / (a * 0.5) - 1.0)
                                   : std::numeric_limits<double>::infinity();
    std::fprintf(stderr, "G3 aRms=%.12g bRms=%.12g target=%.12g relativeError=%.12g\n",
                 a, b, a * 0.5, abError);
    tally.gate(3, generated[0] && generated[1] && abError <= 0.01,
               "A1 gain 0.5 must halve export RMS across default/FX paths");

    const double cdError = c > 0.0 ? std::abs(d / (c * 0.5) - 1.0)
                                   : std::numeric_limits<double>::infinity();
    const double acDb = a > 0.0 && c > 0.0 ? 20.0 * std::log10(c / a)
                                         : std::numeric_limits<double>::infinity();
    std::fprintf(stderr, "G4 aRms=%.12g cRms=%.12g dRms=%.12g target=%.12g "
                         "cdRelativeError=%.12g acDeltaDb=%.12g\n",
                 a, c, d, c * 0.5, cdError, acDb);
    tally.gate(4, generated[0] && generated[2] && generated[3]
                   && cdError <= 0.01 && std::abs(acDb) < 0.1,
               "FX-path gain must halve RMS; flat EQ must stay within 0.1 dB");

    const qsizetype abDifferent = differentBytes(pcm[0], pcm[1]);
    std::fprintf(stderr, "G5 aBytes=%lld bBytes=%lld diffBytes=%lld\n",
                 static_cast<long long>(pcm[0].size()),
                 static_cast<long long>(pcm[1].size()), static_cast<long long>(abDifferent));
    tally.gate(5, generated[0] && generated[1] && abDifferent > 0,
               "negative control must detect PCM differences");

    // G6/G7: band coefficients must act at the right frequency. Expected ratios
    // come from the same Chain run through trackfx::Processor, each with a
    // sanity band so a mutation moving expectation and measurement together fails.
    const auto bandGate = [&](int gate, int variant, double tolerance, double lo, double hi,
                              const char *what) {
        const double aWin = windowRms(pcm[0], channels[0], sampleRates[0]);
        const double vWin = windowRms(pcm[variant], channels[variant], sampleRates[variant]);
        const double measured = aWin > 0.0 ? vWin / aWin : 0.0;
        const double expected = expectedRatio(variant, channels[variant], sampleRates[variant]);
        const double error = expected > 0.0 ? std::abs(measured / expected - 1.0)
                                            : std::numeric_limits<double>::infinity();
        const bool sane = expected >= lo && expected <= hi;
        std::fprintf(stderr, "G%d %s aWinRms=%.12g winRms=%.12g measuredRatio=%.12g "
                             "expectedRatio=%.12g relativeError=%.12g sanity=[%g,%g] sane=%d\n",
                     gate, kNames[variant], aWin, vWin, measured, expected, error, lo, hi, sane);
        tally.gate(gate, generated[0] && generated[variant] && sane && error <= tolerance, what);
    };
    bandGate(6, 4, 0.02, 0.15, 0.5,
             "200 Hz low shelf -12 dB must attenuate 50 Hz (catches low-band coefficient regressions)");
    bandGate(7, 5, 0.01, 0.97, 10.0,
             "1 kHz peaking -12 dB must leave 50 Hz unchanged (catches out-of-band EQ leakage)");

    // G8: variant g (legacy 3-band EQ, 1 kHz peaking -12 dB) is deterministic and
    // attenuates like variant f. The 50 Hz hum only shows out-of-band
    // pass-through, so the in-band ratio is measured on a 1 kHz tone (0.125 FS):
    // fTone = f/a, gTone = g/a must both sit in the -12 dB sanity band and agree
    // within 1%. Detects: variant g missing (HEAD), legacy EQ not applied to the
    // export, wrong filter slot, wrong legacy center frequency/gain/sample rate.
    // Negative controls: the low-shelf slot (50 Hz and 1 kHz tone) must fall
    // outside the 1% band, and g must differ from a.
    {
        const QString tonePath = QDir(repeat.path()).filePath(QStringLiteral("tone_1k.wav"));
        const bool toneWritten = repeat.isValid() && writeToneWav(tonePath, 1000.0, 0.125, 5.0);
        QByteArray toneA, toneF, toneG, toneWrong;
        QString toneError;
        int tc[4] = {}, tr[4] = {};
        const bool toneOk = toneWritten
            && generate(0, QDir(repeat.path()).filePath(QStringLiteral("tone_a.wav")), &toneA, &toneError, &tc[0], &tr[0], tonePath)
            && generate(5, QDir(repeat.path()).filePath(QStringLiteral("tone_f.wav")), &toneF, &toneError, &tc[1], &tr[1], tonePath)
            && generate(6, QDir(repeat.path()).filePath(QStringLiteral("tone_g.wav")), &toneG, &toneError, &tc[2], &tr[2], tonePath)
            && generate(kLegacyWrongSlot, QDir(repeat.path()).filePath(QStringLiteral("tone_w.wav")), &toneWrong, &toneError, &tc[3], &tr[3], tonePath);
        const double aTone = toneOk ? windowRms(toneA, tc[0], tr[0]) : 0.0;
        const double fTone = aTone > 0.0 ? windowRms(toneF, tc[1], tr[1]) / aTone : 0.0;
        const double gTone = aTone > 0.0 ? windowRms(toneG, tc[2], tr[2]) / aTone : 0.0;
        const double wTone = aTone > 0.0 ? windowRms(toneWrong, tc[3], tr[3]) / aTone : 0.0;
        const double toneRelError = fTone > 0.0 ? std::abs(gTone / fTone - 1.0)
                                                : std::numeric_limits<double>::infinity();
        const double wToneError = fTone > 0.0 ? std::abs(wTone / fTone - 1.0)
                                              : std::numeric_limits<double>::infinity();
        const bool toneSane = fTone >= 0.22 && fTone <= 0.29;
        const bool toneControlRejected = toneOk && wToneError > 0.01;
        std::fprintf(stderr, "G8 tone1k ok=%d aTone=%.12g fTone=%.12g gTone=%.12g relativeError=%.12g "
                             "sane=%d sanity=[0.22,0.29] lowShelfSlotTone=%.12g toneControlRelErr=%.12g "
                             "toneControlRejected=%d err=%s\n",
                     toneOk, aTone, fTone, gTone, toneRelError, toneSane, wTone, wToneError,
                     toneControlRejected, toneError.toUtf8().constData());

        const double aWin = windowRms(pcm[0], channels[0], sampleRates[0]);
        const double gWin = windowRms(pcm[6], channels[6], sampleRates[6]);
        const double fWin = windowRms(pcm[5], channels[5], sampleRates[5]);
        const double gRatio = aWin > 0.0 ? gWin / aWin : 0.0;
        const double fRatio = aWin > 0.0 ? fWin / aWin : 0.0;
        const double expected = expectedRatio(5, channels[5], sampleRates[5]);
        const double error = expected > 0.0 ? std::abs(gRatio / expected - 1.0)
                                            : std::numeric_limits<double>::infinity();
        const bool sane = expected >= 0.97 && expected <= 10.0;
        const qsizetype gVsA = differentBytes(pcm[6], pcm[0]);
        QByteArray wrongPcm;
        QString wrongError;
        int wrongChannels = 0, wrongRate = 0;
        const bool wrongOk = generate(kLegacyWrongSlot,
            QDir(repeat.path()).filePath(QStringLiteral("g_wrong_slot.wav")), &wrongPcm,
            &wrongError, &wrongChannels, &wrongRate);
        const double wrongWin = windowRms(wrongPcm, wrongChannels, wrongRate);
        const double wrongRatio = aWin > 0.0 ? wrongWin / aWin : 0.0;
        const double wrongError2 = expected > 0.0 ? std::abs(wrongRatio / expected - 1.0)
                                                  : std::numeric_limits<double>::infinity();
        const bool controlRejected = wrongOk && wrongError2 > 0.01;
        std::fprintf(stderr, "G8 g measuredRatio=%.12g fMeasuredRatio=%.12g expectedRatio=%.12g "
                             "relativeError=%.12g sane=%d repeatEqual=%d diffBytesVsA=%lld\n",
                     gRatio, fRatio, expected, error, sane, repeatEqual[6], static_cast<long long>(gVsA));
        std::fprintf(stderr, "G8 control lowShelfSlot ratio=%.12g relativeError=%.12g rejected=%d error=%s\n",
                     wrongRatio, wrongError2, controlRejected, wrongError.toUtf8().constData());
        QFile firstWavFile(out.filePath(QStringLiteral("g.wav")));
        QFile repeatWavFile(QDir(repeat.path()).filePath(QStringLiteral("g.wav")));
        const bool wavRead = repeat.isValid() && firstWavFile.open(QIODevice::ReadOnly)
            && repeatWavFile.open(QIODevice::ReadOnly);
        const QByteArray firstWav = wavRead ? firstWavFile.readAll() : QByteArray();
        const QByteArray repeatWav = wavRead ? repeatWavFile.readAll() : QByteArray();
        const bool wavRepeatEqual = wavRead && !firstWav.isEmpty() && !repeatWav.isEmpty()
            && firstWav == repeatWav;
        std::fprintf(stderr, "G8 wavBytes=%lld repeatWavBytes=%lld wavDiffBytes=%lld wavRepeatEqual=%d\n",
                     static_cast<long long>(firstWav.size()), static_cast<long long>(repeatWav.size()),
                     static_cast<long long>(differentBytes(firstWav, repeatWav)), wavRepeatEqual);
        tally.gate(8, generated[0] && generated[5] && generated[6] && repeatEqual[6] && wavRepeatEqual
                       && sane && error <= 0.01 && gVsA > 0 && controlRejected
                       && toneOk && toneSane && toneRelError <= 0.01 && toneControlRejected,
                   "legacy 3-band 1 kHz peaking -12 dB (variant g) must be deterministic, pass 50 Hz like f and match f's 1 kHz tone ratio (catches wrong legacy center frequency/gain)");
    }

    // G9: record whether the legacy-EQ export (g) is byte-identical to the 4-band
    // export (f). PASS means the comparison ran and produced values; identical=0
    // is a legitimate result (US-1020 branches on it). Control: the comparator must
    // report a difference for a vs b and identity for g vs g.
    {
        const PcmComparison gf = comparePcm16(pcm[6], pcm[5]);
        const PcmComparison control = comparePcm16(pcm[0], pcm[1]);
        const PcmComparison self = comparePcm16(pcm[6], pcm[6]);
        std::fprintf(stderr, "G9 identical=%d maxDiffLsb=%d firstDiffSample=%lld lengthDiffSamples=%lld\n",
                     gf.identical, gf.maxDiffLsb, static_cast<long long>(gf.firstDiffSample),
                     static_cast<long long>(gf.lengthDiffSamples));
        std::fprintf(stderr, "G9 gBytes=%lld fBytes=%lld control(a,b) identical=%d maxDiffLsb=%d self(g,g) identical=%d\n",
                     static_cast<long long>(pcm[6].size()), static_cast<long long>(pcm[5].size()),
                     control.identical, control.maxDiffLsb, self.identical);
        tally.gate(9, generated[5] && generated[6] && !pcm[5].isEmpty() && !pcm[6].isEmpty()
                       && !control.identical && control.maxDiffLsb > 0 && self.identical,
                   "g vs f PCM byte comparison must run and report values (control: a vs b differs)");
    }

    // G10 (US-1020): the explicit legacy -> 4-band EQ migration. Variant g's legacy
    // EQ goes through trackfx::eqFromLegacy into the trackfx chain with the legacy
    // EQ off (kLegacyMigrated). The branch follows G9: A (g == f bytes) needs the
    // migrated PCM to equal g; B needs the migrated 1 kHz tone ratio to match g's
    // within 1% inside the -12 dB sanity band, records maxDiffLsb vs g (<= 8), and
    // needs the migrated chain to reproduce f byte for byte. Controls: the band
    // landing in the low shelf must miss g's tone ratio; preamp 3 dB and three
    // active bands return false with *out untouched. The Track EQ menu action
    // records one undo "EQ を 4 バンドへ移行" that undo / redo reverse; an
    // unrepresentable EQ only shows the status message. Detects (HEAD): no
    // eqFromLegacy, no menu action.
    {
        const PcmComparison gf = comparePcm16(pcm[6], pcm[5]);
        const bool branchA = gf.identical;
        const QDir tmp(repeat.path());
        QByteArray migrated;
        QString migError;
        const bool migOk = repeat.isValid() && generate(kLegacyMigrated,
            tmp.filePath(QStringLiteral("g_migrated.wav")), &migrated, &migError);
        const PcmComparison mg = comparePcm16(migrated, pcm[6]);
        const PcmComparison mf = comparePcm16(migrated, pcm[5]);
        std::fprintf(stderr, "G10 branch=%s identical=%d maxDiffLsb=%d\n",
                     branchA ? "A" : "B", mg.identical, mg.maxDiffLsb);
        std::fprintf(stderr, "G10 migrated ok=%d bytes=%lld vsG firstDiffSample=%lld vsF identical=%d "
                             "vsF maxDiffLsb=%d error=%s\n",
                     migOk, static_cast<long long>(migrated.size()),
                     static_cast<long long>(mg.firstDiffSample), mf.identical, mf.maxDiffLsb,
                     migError.toUtf8().constData());

        const QString tonePath = tmp.filePath(QStringLiteral("tone10_1k.wav"));
        QByteArray toneA, toneG, toneM, toneW;
        QString toneError;
        int tc[4] = {}, tr[4] = {};
        const bool toneOk = repeat.isValid() && writeToneWav(tonePath, 1000.0, 0.125, 5.0)
            && generate(0, tmp.filePath(QStringLiteral("tone10_a.wav")), &toneA, &toneError, &tc[0], &tr[0], tonePath)
            && generate(6, tmp.filePath(QStringLiteral("tone10_g.wav")), &toneG, &toneError, &tc[1], &tr[1], tonePath)
            && generate(kLegacyMigrated, tmp.filePath(QStringLiteral("tone10_m.wav")), &toneM, &toneError, &tc[2], &tr[2], tonePath)
            && generate(kMigratedWrongShelf, tmp.filePath(QStringLiteral("tone10_w.wav")), &toneW, &toneError, &tc[3], &tr[3], tonePath);
        const double aTone = toneOk ? windowRms(toneA, tc[0], tr[0]) : 0.0;
        const double gTone = aTone > 0.0 ? windowRms(toneG, tc[1], tr[1]) / aTone : 0.0;
        const double mTone = aTone > 0.0 ? windowRms(toneM, tc[2], tr[2]) / aTone : 0.0;
        const double wTone = aTone > 0.0 ? windowRms(toneW, tc[3], tr[3]) / aTone : 0.0;
        const double mRelError = gTone > 0.0 ? std::abs(mTone / gTone - 1.0)
                                             : std::numeric_limits<double>::infinity();
        const double wRelError = gTone > 0.0 ? std::abs(wTone / gTone - 1.0)
                                             : std::numeric_limits<double>::infinity();
        const bool toneSane = gTone >= 0.22 && gTone <= 0.29;
        const bool shelfControlRejected = toneOk && wRelError > 0.01;
        std::fprintf(stderr, "G10 tone1k ok=%d gTone=%.12g migratedTone=%.12g relativeError=%.12g "
                             "sane=%d sanity=[0.22,0.29] lowShelfControlTone=%.12g controlRelErr=%.12g "
                             "controlRejected=%d error=%s\n",
                     toneOk, gTone, mTone, mRelError, toneSane, wTone, wRelError,
                     shelfControlRejected, toneError.toUtf8().constData());
        const bool pcmOk = branchA
            ? migOk && mg.identical
            : migOk && mf.identical && mg.maxDiffLsb <= 8
                && toneOk && toneSane && mRelError <= 0.01 && shelfControlRejected;

        // eqFromLegacy negative controls: *out must keep the sentinel.
        const auto sameBand = [](const trackfx::EqBand &x, const trackfx::EqBand &y) {
            return x.freq == y.freq && x.gainDb == y.gainDb && x.q == y.q && x.enabled == y.enabled;
        };
        const auto sameEq = [&](const trackfx::EqSettings &x, const trackfx::EqSettings &y) {
            return sameBand(x.low, y.low) && sameBand(x.lowMid, y.lowMid)
                && sameBand(x.highMid, y.highMid) && sameBand(x.high, y.high);
        };
        trackfx::EqSettings sentinel;
        sentinel.highMid = trackfx::EqBand{4321.0, 3.0, 2.0};
        AudioEQConfig preampCfg = legacyEqConfig(1);
        preampCfg.preamp = 3.0;
        AudioEQConfig threeBandCfg;
        threeBandCfg.bands = {EQBand{100.0, 4.0, 1.0}, EQBand{1000.0, -12.0, 1.0}, EQBand{8000.0, 3.0, 1.0}};
        trackfx::EqSettings preampOut = sentinel, threeOut = sentinel, gOut = sentinel;
        const bool preampResult = trackfx::eqFromLegacy(preampCfg, &preampOut);
        const bool threeResult = trackfx::eqFromLegacy(threeBandCfg, &threeOut);
        const bool gResult = trackfx::eqFromLegacy(legacyEqConfig(1), &gOut);
        const bool rejectOk = !preampResult && sameEq(preampOut, sentinel)
            && !threeResult && sameEq(threeOut, sentinel);
        const bool gMapped = gResult && sameBand(gOut.lowMid, trackfx::EqBand{1000.0, -12.0, 1.0})
            && gOut.low.gainDb == 0.0 && gOut.highMid.gainDb == 0.0 && gOut.high.gainDb == 0.0;
        std::fprintf(stderr, "G10 eqFromLegacy preamp3dB=%d outUnchanged=%d threeBands=%d outUnchanged=%d "
                             "g=%d lowMid=%g/%g/%g\n",
                     preampResult, sameEq(preampOut, sentinel), threeResult, sameEq(threeOut, sentinel),
                     gResult, gOut.lowMid.freq, gOut.lowMid.gainDb, gOut.lowMid.q);

        // Track EQ > A1 > "4 バンド EQ へ移行" in a real MainWindow.
        bool menuOk = false, migratedOk = false, undoOk = false, redoOk = false, refusedOk = true;
        {
            MainWindow window;
            Timeline *timeline = window.findChild<Timeline *>();
            AudioMixer *mixer = timeline ? timeline->audioMixer() : nullptr;
            if (timeline && timeline->audioTrackCount() < 1)
                timeline->restoreFromProject(QVector<QVector<ClipInfo>>{},
                    QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
            auto *trackEqMenu = window.findChild<QMenu *>(QStringLiteral("trackEqMenu"));
            QMenu *a1 = nullptr;
            if (trackEqMenu) {
                emit trackEqMenu->aboutToShow();
                for (QAction *action : trackEqMenu->actions())
                    if (action->menu() && action->text() == QStringLiteral("A1")) a1 = action->menu();
            }
            QAction *migrate = a1 ? a1->findChild<QAction *>(QStringLiteral("trackEqMigrateTo4Band")) : nullptr;
            menuOk = migrate && migrate->text() == QStringLiteral("4 バンド EQ へ移行")
                && migrate->shortcut().isEmpty();
            std::fprintf(stderr, "G10 menu trackEqMenu=%d A1=%d action=%d text=%s shortcutEmpty=%d\n",
                         trackEqMenu != nullptr, a1 != nullptr, migrate != nullptr,
                         migrate ? migrate->text().toUtf8().constData() : "",
                         migrate ? migrate->shortcut().isEmpty() : 0);
            if (menuOk && mixer) {
                UndoManager *undo = timeline->undoManager();
                mixer->setTrackEqEnabled(0, true);
                mixer->setTrackEqConfig(0, legacyEqConfig(1));
                trackfx::Chain before = mixer->trackChain(0);
                before.eq.highMid = trackfx::EqBand{5000.0, 4.0, 1.0}; // must come back on undo
                mixer->setTrackChain(0, before);
                const QJsonObject chainBefore = mixer->trackChain(0).toJson();
                const quint64 serial = undo->saveSerial();
                migrate->trigger();
                const trackfx::Chain after = mixer->trackChain(0);
                migratedOk = undo->saveSerial() == serial + 1
                    && undo->undoDescription() == QStringLiteral("EQ を 4 バンドへ移行")
                    && !mixer->trackEqEnabled(0) && mixer->trackEqConfig(0).bands.isEmpty()
                    && after.eqEnabled && sameEq(after.eq, gOut);
                std::fprintf(stderr, "G10 migrate saves=%llu undoText=%s legacyEnabled=%d legacyBands=%lld "
                                     "eqEnabled=%d lowMid=%g/%g/%g highMidGain=%g\n",
                             static_cast<unsigned long long>(undo->saveSerial() - serial),
                             undo->undoDescription().toUtf8().constData(), mixer->trackEqEnabled(0),
                             static_cast<long long>(mixer->trackEqConfig(0).bands.size()), after.eqEnabled,
                             after.eq.lowMid.freq, after.eq.lowMid.gainDb, after.eq.lowMid.q,
                             after.eq.highMid.gainDb);
                timeline->undo();
                const AudioEQConfig undone = mixer->trackEqConfig(0);
                undoOk = mixer->trackEqEnabled(0) && undone.bands.size() == 2
                    && undone.bands[1].frequency == 1000.0 && undone.bands[1].gain == -12.0
                    && undone.bands[1].q == 1.0 && mixer->trackChain(0).toJson() == chainBefore;
                std::fprintf(stderr, "G10 undo legacyEnabled=%d legacyBands=%lld chainRestored=%d highMidGain=%g\n",
                             mixer->trackEqEnabled(0), static_cast<long long>(undone.bands.size()),
                             mixer->trackChain(0).toJson() == chainBefore,
                             mixer->trackChain(0).eq.highMid.gainDb);
                timeline->redo();
                redoOk = !mixer->trackEqEnabled(0) && mixer->trackEqConfig(0).bands.isEmpty()
                    && mixer->trackChain(0).toJson() == after.toJson();
                std::fprintf(stderr, "G10 redo legacyEnabled=%d legacyBands=%lld chainMigrated=%d\n",
                             mixer->trackEqEnabled(0),
                             static_cast<long long>(mixer->trackEqConfig(0).bands.size()),
                             mixer->trackChain(0).toJson() == after.toJson());
                // Unrepresentable legacy EQs: only the status message, no state change.
                for (const AudioEQConfig &cfg : {preampCfg, threeBandCfg}) {
                    mixer->setTrackEqEnabled(0, true);
                    mixer->setTrackEqConfig(0, cfg);
                    const QJsonObject trackState = mixer->collectTrackState();
                    const QJsonObject chainState = mixer->trackChain(0).toJson();
                    const quint64 refusedSerial = undo->saveSerial();
                    window.statusBar()->clearMessage();
                    migrate->trigger();
                    const QString message = window.statusBar()->currentMessage();
                    const bool ok = message.contains(QStringLiteral(
                            "この EQ 設定は 4 バンド EQ で表現できません (preamp / 3 帯以上)"))
                        && mixer->collectTrackState() == trackState
                        && mixer->trackChain(0).toJson() == chainState
                        && undo->saveSerial() == refusedSerial;
                    std::fprintf(stderr, "G10 refused preamp=%g bands=%lld message=%s unchanged=%d\n",
                                 cfg.preamp, static_cast<long long>(cfg.bands.size()),
                                 message.toUtf8().constData(), ok);
                    refusedOk &= ok;
                }
                // A representable legacy EQ with an active post-EQ stage (compressor):
                // migrating would move the EQ ahead of it, so it is refused too.
                {
                    mixer->setTrackEqEnabled(0, true);
                    mixer->setTrackEqConfig(0, legacyEqConfig(1));
                    trackfx::Chain withComp = mixer->trackChain(0);
                    withComp.compEnabled = true;
                    withComp.comp.enabled = true;
                    withComp.comp.ratio = 4.0;
                    withComp.comp.thresholdDb = -20.0;
                    mixer->setTrackChain(0, withComp);
                    const QJsonObject trackState = mixer->collectTrackState();
                    const QJsonObject chainState = mixer->trackChain(0).toJson();
                    const quint64 refusedSerial = undo->saveSerial();
                    window.statusBar()->clearMessage();
                    migrate->trigger();
                    const QString message = window.statusBar()->currentMessage();
                    const bool ok = message.contains(QStringLiteral(
                            "ディエッサー / コンプレッサー / リバーブが有効なため 4 バンド EQ へ移行できません (処理順が変わります)"))
                        && mixer->collectTrackState() == trackState
                        && mixer->trackChain(0).toJson() == chainState
                        && undo->saveSerial() == refusedSerial;
                    std::fprintf(stderr, "G10 refused postEqFx=comp message=%s unchanged=%d\n",
                                 message.toUtf8().constData(), ok);
                    refusedOk &= ok;
                }
            } else {
                refusedOk = false;
            }
        }

        // "4 バンド EQ へ移行" of A1 / A2 after one aboutToShow (it rebuilds the submenus).
        const auto migrateActions = [](MainWindow &window, QAction **a1, QAction **a2) {
            auto *trackEqMenu = window.findChild<QMenu *>(QStringLiteral("trackEqMenu"));
            if (!trackEqMenu) return;
            emit trackEqMenu->aboutToShow();
            for (QAction *action : trackEqMenu->actions()) {
                if (!action->menu()) continue;
                QAction *migrate = action->menu()->findChild<QAction *>(QStringLiteral("trackEqMigrateTo4Band"));
                if (action->text() == QStringLiteral("A1")) *a1 = migrate;
                if (a2 && action->text() == QStringLiteral("A2")) *a2 = migrate;
            }
        };
        const auto legacyOn = [](AudioMixer *mixer, int track) {
            const AudioEQConfig cfg = mixer->trackEqConfig(track);
            return mixer->trackEqEnabled(track) && cfg.bands.size() == 2
                && cfg.bands[1].frequency == 1000.0 && cfg.bands[1].gain == -12.0;
        };
        const auto legacyOff = [](AudioMixer *mixer, int track) {
            return !mixer->trackEqEnabled(track) && mixer->trackEqConfig(track).bands.isEmpty();
        };

        // Two migrations (A1, then A2): the second keeps the first one's pin on the
        // entry between them, so redo to it disables A1's legacy EQ again.
        bool twoTrackOk = false;
        {
            MainWindow window;
            Timeline *timeline = window.findChild<Timeline *>();
            AudioMixer *mixer = timeline ? timeline->audioMixer() : nullptr;
            if (timeline)
                timeline->restoreFromProject(QVector<QVector<ClipInfo>>{},
                    QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}, QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
            QAction *m1 = nullptr, *m2 = nullptr;
            migrateActions(window, &m1, &m2);
            const int tracks = timeline ? timeline->audioTrackCount() : 0;
            std::fprintf(stderr, "G10 twoTrack setup tracks=%d A1action=%d A2action=%d\n",
                         tracks, m1 != nullptr, m2 != nullptr);
            if (mixer && m1 && m2 && tracks == 2) {
                QJsonObject chainBefore[2], chainMigrated[2];
                for (int t = 0; t < 2; ++t) {
                    mixer->setTrackEqEnabled(t, true);
                    mixer->setTrackEqConfig(t, legacyEqConfig(1));
                    chainBefore[t] = mixer->trackChain(t).toJson();
                }
                m1->trigger();
                chainMigrated[0] = mixer->trackChain(0).toJson();
                m2->trigger();
                chainMigrated[1] = mixer->trackChain(1).toJson();
                const auto chainIs = [&](int t, const QJsonObject &json) {
                    return mixer->trackChain(t).toJson() == json;
                };
                const bool migrated = legacyOff(mixer, 0) && legacyOff(mixer, 1)
                    && chainMigrated[0] != chainBefore[0] && chainMigrated[1] != chainBefore[1];
                timeline->undo();
                timeline->undo();
                const bool undone = legacyOn(mixer, 0) && legacyOn(mixer, 1)
                    && chainIs(0, chainBefore[0]) && chainIs(1, chainBefore[1]);
                std::fprintf(stderr, "G10 twoTrack migrated=%d undo2 A1legacyOn=%d A2legacyOn=%d "
                                     "A1chainBefore=%d A2chainBefore=%d\n",
                             migrated, legacyOn(mixer, 0), legacyOn(mixer, 1),
                             chainIs(0, chainBefore[0]), chainIs(1, chainBefore[1]));
                timeline->redo();
                const bool redo1 = legacyOff(mixer, 0) && chainIs(0, chainMigrated[0])
                    && legacyOn(mixer, 1) && chainIs(1, chainBefore[1]);
                std::fprintf(stderr, "G10 twoTrack redo1 A1legacyOff=%d A1chainMigrated=%d "
                                     "A2legacyOn=%d A2chainBefore=%d\n",
                             legacyOff(mixer, 0), chainIs(0, chainMigrated[0]),
                             legacyOn(mixer, 1), chainIs(1, chainBefore[1]));
                timeline->redo();
                const bool redo2 = legacyOff(mixer, 0) && chainIs(0, chainMigrated[0])
                    && legacyOff(mixer, 1) && chainIs(1, chainMigrated[1]);
                std::fprintf(stderr, "G10 twoTrack redo2 A1legacyOff=%d A1chainMigrated=%d "
                                     "A2legacyOff=%d A2chainMigrated=%d\n",
                             legacyOff(mixer, 0), chainIs(0, chainMigrated[0]),
                             legacyOff(mixer, 1), chainIs(1, chainMigrated[1]));
                twoTrackOk = migrated && undone && redo1 && redo2;
            }
        }

        // Relink after a migration (MCP relink_media -> relinkMediaPaths, which
        // re-captures the current entry first) keeps the migration's pin.
        bool relinkOk = false;
        {
            const QString fromPath = tmp.filePath(QStringLiteral("relink10_from.wav"));
            const QString toPath = tmp.filePath(QStringLiteral("relink10_to.wav"));
            const bool filesOk = repeat.isValid() && writeToneWav(fromPath, 440.0, 0.125, 1.0)
                && writeToneWav(toPath, 440.0, 0.125, 1.0);
            mcp::McpToolRegistry registry;
            MainWindow window;
            mcp::McpEditorTools tools(&window, &registry);
            tools.registerWriteTools();
            Timeline *timeline = window.findChild<Timeline *>();
            AudioMixer *mixer = timeline ? timeline->audioMixer() : nullptr;
            if (timeline)
                timeline->restoreFromProject(QVector<QVector<ClipInfo>>{},
                    QVector<QVector<ClipInfo>>{QVector<ClipInfo>{humClip(0.0, 1.0, fromPath)}},
                    0.0, -1.0, -1.0, 100);
            QAction *m1 = nullptr;
            migrateActions(window, &m1, nullptr);
            std::fprintf(stderr, "G10 relink setup files=%d A1action=%d\n", filesOk, m1 != nullptr);
            if (filesOk && mixer && m1) {
                UndoManager *undo = timeline->undoManager();
                mixer->setTrackEqEnabled(0, true);
                mixer->setTrackEqConfig(0, legacyEqConfig(1));
                m1->trigger();
                const QJsonObject migratedChain = mixer->trackChain(0).toJson();
                const bool migrated = legacyOff(mixer, 0)
                    && undo->undoDescription() == QStringLiteral("EQ を 4 バンドへ移行");
                bool found = false;
                QString relinkError;
                const QJsonObject result = registry.callTool(QStringLiteral("relink_media"),
                    QJsonObject{{QStringLiteral("mapping"), QJsonArray{QJsonObject{
                        {QStringLiteral("from"), fromPath}, {QStringLiteral("to"), toPath}}}}},
                    &found, &relinkError);
                const QVector<ClipInfo> clips = timeline->audioTracks().first()->clips();
                const bool relinked = found && result.value(QStringLiteral("ok")).toBool()
                    && clips.size() == 1 && clips.first().filePath == toPath
                    && undo->undoDescription() == QStringLiteral("メディアを再リンク");
                std::fprintf(stderr, "G10 relink migrated=%d found=%d ok=%d undoText=%s error=%s\n",
                             migrated, found, result.value(QStringLiteral("ok")).toBool(),
                             undo->undoDescription().toUtf8().constData(),
                             relinkError.toUtf8().constData());
                timeline->undo();
                timeline->undo();
                const bool undone = legacyOn(mixer, 0);
                timeline->redo();
                const bool redone = legacyOff(mixer, 0) && mixer->trackChain(0).toJson() == migratedChain;
                std::fprintf(stderr, "G10 relink undo2 A1legacyOn=%d redo1 A1legacyOff=%d A1chainMigrated=%d\n",
                             undone, legacyOff(mixer, 0), mixer->trackChain(0).toJson() == migratedChain);
                relinkOk = migrated && relinked && undone && redone;
            }
        }
        std::fprintf(stderr, "G10 terms pcmOk=%d rejectOk=%d gMapped=%d menuOk=%d migratedOk=%d "
                             "undoOk=%d redoOk=%d refusedOk=%d twoTrackOk=%d relinkOk=%d\n",
                     pcmOk, rejectOk, gMapped, menuOk, migratedOk, undoOk, redoOk, refusedOk,
                     twoTrackOk, relinkOk);
        tally.gate(10, generated[5] && generated[6] && pcmOk && rejectOk && gMapped
                       && menuOk && migratedOk && undoOk && redoOk && refusedOk
                       && twoTrackOk && relinkOk,
                   "migrated legacy EQ must match g (branch A bytes / branch B 1 kHz ratio), reject preamp / three bands, and undo / redo as one step");
    }
    // G11 (US-1126): variant h (clip EQ 1 kHz peaking -12 dB on the first clip) on a 1 kHz tone.
    // The first clip's window ratio matches the Processor-derived expectation (the same one
    // variant f meets on the track chain), the second clip [2.1 s, 4.0 s) is byte-identical to
    // control h0 (clip EQ on, every band 0 dB) and h0 stays within 2 LSB of a. Negative controls:
    // h must differ from h0 over the first clip, and the second-clip range must carry signal.
    // Detects: clip EQ not applied in the export, applied to the wrong clip, or leaking forward.
    // G12: playback (renderPcmForTest) shows the same first-clip ratio as the export within
    // 0.5 dB; the -12 dB EQ switched off exports byte-identical to a.
    {
        const QString tonePath = QDir(repeat.path()).filePath(QStringLiteral("tone_1k_h.wav"));
        const bool toneWritten = repeat.isValid() && writeToneWav(tonePath, 1000.0, 0.125, 5.0);
        const auto toneFile = [&](const char *name) { return QDir(repeat.path()).filePath(QString::fromLatin1(name)); };
        QByteArray toneA, toneH, toneH2, toneH0, toneOff;
        QString toneError;
        int tc[5] = {}, tr[5] = {};
        const bool toneOk = toneWritten
            && generate(0, toneFile("h_tone_a.wav"), &toneA, &toneError, &tc[0], &tr[0], tonePath)
            && generate(kClipEq, toneFile("h_tone_h.wav"), &toneH, &toneError, &tc[1], &tr[1], tonePath)
            && generate(kClipEq, toneFile("h_tone_h2.wav"), &toneH2, &toneError, &tc[2], &tr[2], tonePath)
            && generate(kClipEqFlat, toneFile("h_tone_h0.wav"), &toneH0, &toneError, &tc[3], &tr[3], tonePath)
            && generate(kClipEqOff, toneFile("h_tone_off.wav"), &toneOff, &toneError, &tc[4], &tr[4], tonePath);
        const double aTone = toneOk ? windowRms(toneA, tc[0], tr[0]) : 0.0;
        const double hRatio = aTone > 0.0 ? windowRms(toneH, tc[1], tr[1]) / aTone : 0.0;
        trackfx::Chain eqChain;
        eqChain.eq = clipEqFor(kClipEq);
        eqChain.eqEnabled = true;
        const double expectedH = toneOk ? expectedRatioAt(eqChain, 1000.0, tc[1], tr[1]) : 0.0;
        const double hError = expectedH > 0.0 ? std::abs(hRatio / expectedH - 1.0)
                                              : std::numeric_limits<double>::infinity();
        const bool sane = expectedH >= 0.22 && expectedH <= 0.29;
        QFile firstWavFile(toneFile("h_tone_h.wav"));
        QFile repeatWavFile(toneFile("h_tone_h2.wav"));
        const bool wavRead = toneOk && firstWavFile.open(QIODevice::ReadOnly)
            && repeatWavFile.open(QIODevice::ReadOnly);
        const QByteArray firstWav = wavRead ? firstWavFile.readAll() : QByteArray();
        const QByteArray repeatWav = wavRead ? repeatWavFile.readAll() : QByteArray();
        const bool wavRepeatEqual = wavRead && !firstWav.isEmpty() && !repeatWav.isEmpty()
            && firstWav == repeatWav;
        const bool deterministic = toneOk && differentBytes(toneH, toneH2) == 0 && wavRepeatEqual;
        const bool formatsMatch = toneOk && tc[1] == tc[3] && tc[3] == tc[0]
            && tr[1] == tr[3] && tr[3] == tr[0];
        const QByteArray h2nd = pcmRange(toneH, tc[1], tr[1], 2.1, 4.0);
        const QByteArray h02nd = pcmRange(toneH0, tc[3], tr[3], 2.1, 4.0);
        const qsizetype expectedBytes = toneOk
            ? (qsizetype(4.0 * tr[1]) - qsizetype(2.1 * tr[1])) * tc[1] * 2 : 0;
        const bool secondEqual = toneOk && formatsMatch && expectedBytes > 0
            && h2nd.size() == expectedBytes && h02nd.size() == expectedBytes
            && h2nd == h02nd && rms(h2nd) > 0.01;
        const bool firstDiffers = toneOk
            && pcmRange(toneH, tc[1], tr[1], 0.1, 0.4) != pcmRange(toneH0, tc[3], tr[3], 0.1, 0.4);
        const PcmComparison h0a = comparePcm16(toneH0, toneA);
        std::fprintf(stderr, "G11 tone1k ok=%d deterministic=%d aTone=%.12g hRatio=%.12g expectedRatio=%.12g "
                             "relativeError=%.12g sane=%d sanity=[0.22,0.29]\n",
                     toneOk, deterministic, aTone, hRatio, expectedH, hError, sane);
        std::fprintf(stderr, "G11 wavBytes=%lld repeatWavBytes=%lld wavDiffBytes=%lld wavRepeatEqual=%d "
                             "formatsMatch=%d channels=[%d,%d,%d] rates=[%d,%d,%d]\n",
                     static_cast<long long>(firstWav.size()), static_cast<long long>(repeatWav.size()),
                     static_cast<long long>(differentBytes(firstWav, repeatWav)), wavRepeatEqual,
                     formatsMatch, tc[0], tc[1], tc[3], tr[0], tr[1], tr[3]);
        std::fprintf(stderr, "G11 secondClip expectedBytes=%lld hBytes=%lld h0Bytes=%lld equalToH0=%d rms=%.12g "
                             "firstClipDiffersFromH0=%d h0VsA identical=%d maxDiffLsb=%d "
                             "lengthDiffSamples=%lld firstDiffSample=%lld error=%s\n",
                     static_cast<long long>(expectedBytes), static_cast<long long>(h2nd.size()),
                     static_cast<long long>(h02nd.size()), secondEqual, rms(h2nd), firstDiffers,
                     h0a.identical, h0a.maxDiffLsb, static_cast<long long>(h0a.lengthDiffSamples),
                     static_cast<long long>(h0a.firstDiffSample), toneError.toUtf8().constData());
        tally.gate(11, toneOk && deterministic && sane && hError <= 0.01 && secondEqual && firstDiffers
                       && h0a.maxDiffLsb <= 2 && h0a.lengthDiffSamples == 0,
                   "clip EQ must attenuate the first clip's 1 kHz like the expected ratio and leave the second clip equal to the flat control");

        double playbackRatio = 0.0;
        {
            const auto playback = [&](int variant) {
                AudioMixer mixer;
                Timeline timeline;
                QString error;
                if (!setupVariant(variant, mixer, timeline, &error, tonePath)) return QByteArray();
                mixer.setSequence(timeline.computeAudioPlaybackSequence());
                return mixer.renderPcmForTest(48000);
            };
            const QByteArray playA = toneOk ? playback(0) : QByteArray();
            const QByteArray playH = toneOk ? playback(kClipEq) : QByteArray();
            const double playARms = windowRms(playA, 2, 48000);
            playbackRatio = playARms > 0.0 ? windowRms(playH, 2, 48000) / playARms : 0.0;
        }
        const double deltaDb = (hRatio > 0.0 && playbackRatio > 0.0)
            ? std::abs(20.0 * std::log10(playbackRatio / hRatio))
            : std::numeric_limits<double>::infinity();
        const bool playbackSane = playbackRatio >= 0.22 && playbackRatio <= 0.29;
        const bool offIdentical = toneOk && toneOff == toneA;
        std::fprintf(stderr, "G12 exportRatio=%.12g playbackRatio=%.12g deltaDb=%.12g playbackSane=%d "
                             "clipEqOffIdenticalToA=%d\n",
                     hRatio, playbackRatio, deltaDb, playbackSane, offIdentical);
        tally.gate(12, toneOk && playbackSane && deltaDb <= 0.5 && offIdentical,
                   "playback and export must agree within 0.5 dB on the clip EQ ratio; clip EQ off must export like a");
    }
    return tally.finish();
}
