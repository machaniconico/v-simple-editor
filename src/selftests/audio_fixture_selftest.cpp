// US-704: persistent PCM fixture for the real timeline export audio mixer.
// Run from the repository root and compare data chunks across baseline/HEAD.
#include "../AudioMixer.h"
#include "../AudioTrackFx.h"
#include "../RenderInPlace.h"
#include "../Timeline.h"
#include "SelftestGate.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <limits>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int kFixtureCount = 6;
const char *const kNames[kFixtureCount] = {"a", "b", "c", "d", "e", "f"};

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

ClipInfo humClip(double in, double out)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("test_assets/e2e_hum.wav");
    clip.displayName = QStringLiteral("audio-fixture");
    clip.duration = 5.0;
    clip.inPoint = in;
    clip.outPoint = out;
    return clip;
}

// Variant e: 200 Hz low shelf -12 dB (50 Hz sits inside the shelf).
// Variant f: 1 kHz peaking -12 dB (50 Hz is outside the band: frequency control).
trackfx::Chain eqVariantChain(int variant)
{
    trackfx::Chain chain;
    chain.eqEnabled = true;
    if (variant == 4) chain.eq.low = trackfx::EqBand{200.0, -12.0, 0.7};
    if (variant == 5) chain.eq.lowMid = trackfx::EqBand{1000.0, -12.0, 1.0};
    return chain;
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
double expectedRatio(int variant, int channels, int sampleRate)
{
    if (channels <= 0 || sampleRate <= 0) return 0.0;
    const int frames = sampleRate / 2;
    QVector<float> in(frames * channels);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            in[i * channels + c] = float(0.5 * std::sin(2.0 * M_PI * 50.0 * i / sampleRate));
    QVector<float> out = in;
    trackfx::Processor processor(eqVariantChain(variant), sampleRate, channels);
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

bool generate(int variant, const QString &path, QByteArray *pcm, QString *error,
              int *channels = nullptr, int *sampleRate = nullptr)
{
    AudioMixer mixer;
    Timeline timeline;
    timeline.setAudioMixer(&mixer);
    ClipInfo first = humClip(0.0, 2.0);
    ClipInfo second = humClip(0.5, 2.5);
    first.trailOut.type = TransitionType::FadeOut;
    first.trailOut.duration = 0.3;
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
    double audioDuration = 0.0;
    for (const TimelineTrack *track : timeline.audioTracks()) {
        double trackDuration = 0.0;
        for (const ClipInfo &clip : track->clips())
            trackDuration += qMax(0.0, clip.leadInSec) + clip.effectiveDuration();
        audioDuration = qMax(audioDuration, trackDuration);
    }
    const double durationSeconds = qMax(timeline.totalDuration(), audioDuration);
    std::fprintf(stderr, "G1 %s timelineDurationSec=%.12g\n", kNames[variant], durationSeconds);
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

} // namespace

int runAudioFixtureSelftest()
{
    selftest::GateTally tally;
    const QString source = QStringLiteral("test_assets/e2e_hum.wav");
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!QFile::exists(source) || ffmpeg.isEmpty()) {
        std::fprintf(stderr, "audio-fixture prerequisites: source=%d ffmpeg=%s (cwd must be repository root)\n",
                     QFile::exists(source), ffmpeg.toUtf8().constData());
        for (int gate = 1; gate <= 7; ++gate)
            tally.skip(gate, "test_assets/e2e_hum.wav or ffmpeg unavailable");
        return tally.finish();
    }

    const QString outputPath = qEnvironmentVariable("VEDITOR_AUDIO_FIXTURE_OUT");
    if (outputPath.isEmpty()) {
        tally.gate(1, false, "VEDITOR_AUDIO_FIXTURE_OUT is required");
        for (int gate = 2; gate <= 7; ++gate)
            tally.skip(gate, "G1 output directory is unset");
        return tally.finish();
    }
    if (!QDir().mkpath(outputPath)) {
        tally.gate(1, false, "VEDITOR_AUDIO_FIXTURE_OUT cannot be created");
        for (int gate = 2; gate <= 7; ++gate)
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
        for (int gate = 2; gate <= 7; ++gate)
            tally.skip(gate, "G1 output cleanup failed");
        return tally.finish();
    }
    QByteArray pcm[kFixtureCount];
    bool generated[kFixtureCount] = {};
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
    tally.gate(1, allGenerated, "six PCM WAV files and audio_fixture.json required");

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
        repeated &= generated[i] && ok && difference == 0;
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
    return tally.finish();
}
