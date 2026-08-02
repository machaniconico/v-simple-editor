#include <QDebug>
#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "../SpectralEngine.h"
#include "../VoiceIsolation.h"
#include "../VoiceIsolationDialog.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

double rms(const QVector<float> &samples, int first = 0, int last = -1)
{
    // Qt6 size() returns qsizetype; mixing it with int breaks template
    // deduction for std::clamp on MSVC, so narrow explicitly first.
    const int count = static_cast<int>(samples.size());
    const int begin = std::clamp(first, 0, count);
    const int end = last < 0 ? count : std::clamp(last, begin, count);
    if (end <= begin)
        return 0.0;
    double sum = 0.0;
    for (int i = begin; i < end; ++i)
        sum += static_cast<double>(samples[i]) * samples[i];
    return std::sqrt(sum / static_cast<double>(end - begin));
}

double maxAbsDifference(const QVector<float> &a, const QVector<float> &b)
{
    if (a.size() != b.size())
        return std::numeric_limits<double>::infinity();
    double maximum = 0.0;
    for (int i = 0; i < a.size(); ++i)
        maximum = std::max(maximum, std::fabs(static_cast<double>(a[i]) - b[i]));
    return maximum;
}

double maxAbsDifferenceFrom(const QVector<float> &a,
                            const QVector<float> &b,
                            int first)
{
    if (a.size() != b.size())
        return std::numeric_limits<double>::infinity();
    const int count = static_cast<int>(a.size());
    const int begin = std::clamp(first, 0, count);
    double maximum = 0.0;
    for (int i = begin; i < count; ++i)
        maximum = std::max(maximum,
                           std::fabs(static_cast<double>(a[i]) - b[i]));
    return maximum;
}

double energyInBand(const QVector<float> &samples,
                    int sampleRate,
                    double lowHz,
                    double highHz,
                    int first,
                    int length)
{
    const int fftSize = 4096;
    std::vector<std::complex<double>> buffer(static_cast<std::size_t>(fftSize),
                                             {0.0, 0.0});
    for (int i = 0; i < length && i + first < samples.size() && i < fftSize; ++i)
        buffer[static_cast<std::size_t>(i)] = samples[first + i];
    spectral::fft(buffer, false);
    double total = 0.0;
    for (int k = 0; k <= fftSize / 2; ++k) {
        const double hz = spectral::binToHz(k, fftSize, sampleRate);
        if (hz >= lowHz && hz <= highHz)
            total += std::norm(buffer[static_cast<std::size_t>(k)]);
    }
    return total;
}

struct DeterministicNoise {
    unsigned long long state = 0x2a4d7e91c3b5d701ULL;

    double next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double unit = static_cast<double>((state >> 11) & 0xFFFFFFFFULL)
            / static_cast<double>(0xFFFFFFFFULL);
        return unit * 2.0 - 1.0;
    }
};

void addVoice(QVector<float> &voice, int sampleRate, int first)
{
    const double frequencies[] = {200.0, 400.0, 600.0, 800.0};
    const double amplitudes[] = {0.55, 0.25, 0.18, 0.12};
    for (int i = first; i < voice.size(); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        double sample = 0.0;
        for (int h = 0; h < 4; ++h)
            sample += amplitudes[h] * std::sin(2.0 * kPi * frequencies[h] * t);
        voice[i] = static_cast<float>(sample);
    }
}

QVector<float> makeVoicePlusNoise(int sampleRate,
                                  int sampleCount,
                                  QVector<float> *cleanVoice)
{
    const int noisePrefix = static_cast<int>(0.6 * sampleRate);
    QVector<float> voice(sampleCount, 0.0f);
    addVoice(voice, sampleRate, noisePrefix);
    const double voiceRms = rms(voice, noisePrefix);

    DeterministicNoise generator;
    QVector<float> result(sampleCount, 0.0f);
    for (int i = 0; i < sampleCount; ++i)
        result[i] = static_cast<float>(generator.next() * voiceRms);
    for (int i = noisePrefix; i < sampleCount; ++i)
        result[i] += voice[i];
    if (cleanVoice)
        *cleanVoice = voice;
    return result;
}

QVector<float> makeNoiseRise(int sampleRate, int sampleCount, int riseAt)
{
    DeterministicNoise generator;
    QVector<float> result(sampleCount, 0.0f);
    for (int i = 0; i < sampleCount; ++i) {
        if (i < riseAt) {
            result[i] = static_cast<float>(generator.next() * 0.01);
        } else {
            const double t = static_cast<double>(i) / sampleRate;
            result[i] = static_cast<float>(
                0.20 * std::sin(2.0 * kPi * 468.75 * t));
        }
    }
    return result;
}

QVector<float> makeLowFundamental(int sampleRate, int sampleCount)
{
    DeterministicNoise generator;
    const int toneStart = static_cast<int>(0.6 * sampleRate);
    const double frequencies[] = {100.0, 200.0, 300.0, 400.0};
    const double amplitudes[] = {0.45, 0.22, 0.14, 0.10};
    QVector<float> result(sampleCount, 0.0f);
    for (int i = 0; i < sampleCount; ++i) {
        double sample = generator.next() * 0.005;
        if (i >= toneStart) {
            const double t = static_cast<double>(i) / sampleRate;
            for (int h = 0; h < 4; ++h)
                sample += amplitudes[h]
                    * std::sin(2.0 * kPi * frequencies[h] * t);
        }
        result[i] = static_cast<float>(sample);
    }
    return result;
}

voiceiso::VoiceIsolationParams defaultTestParams()
{
    voiceiso::VoiceIsolationParams params;
    params.fftSize = 4096;
    params.hopSize = 1024;
    params.noiseLearnSeconds = 0.5;
    // Use a strong separation profile so the numerical SNR gate measures the
    // algorithm's denoising effect rather than the intentional dry-signal mix.
    params.strength = 0.92;
    params.harmonicWeight = 0.5;
    params.smoothingFactor = 0.7;
    return params;
}

} // namespace

int runVoiceIsolationSelftest()
{
    qInfo().noquote() << "[voice-isolation] selftest start";
    int passed = 0;
    int failed = 0;
    const auto pass = [&](const char *name) {
        ++passed;
        qInfo().noquote() << "[voice-isolation] PASS" << name;
    };
    const auto fail = [&](const char *name, const QString &message) {
        ++failed;
        qWarning().noquote() << "[voice-isolation] FAIL" << name << ":" << message;
    };

    const int sampleRate = 48000;
    const int sampleCount = sampleRate * 2;
    QVector<float> cleanVoice;
    const QVector<float> input = makeVoicePlusNoise(
        sampleRate, sampleCount, &cleanVoice);
    const voiceiso::VoiceIsolationParams params = defaultTestParams();
    const voiceiso::VoiceIsolationResult result = voiceiso::isolate(
        input, sampleRate, params);

    // G1: voice/background are a sample-exact complementary decomposition.
    {
        QVector<float> sum(input.size(), 0.0f);
        for (int i = 0; i < sum.size(); ++i)
            sum[i] = result.voice[i] + result.background[i];
        const double error = maxAbsDifference(sum, input);
        if (error < 1.0e-3)
            pass("G1 voice + background reconstructs input");
        else
            fail("G1 reconstruction", QStringLiteral("max error=%1").arg(error, 0, 'e', 6));
    }

    // G2: harmonic voice plus 0 dB broadband noise improves SNR by >=10 dB.
    {
        const int first = static_cast<int>(0.8 * sampleRate);
        const int length = sampleCount - first;
        double inputError = 0.0;
        double outputError = 0.0;
        double cleanEnergy = 0.0;
        for (int i = first; i < sampleCount; ++i) {
            const double clean = cleanVoice[i];
            const double inError = input[i] - clean;
            const double outError = result.voice[i] - clean;
            cleanEnergy += clean * clean;
            inputError += inError * inError;
            outputError += outError * outError;
        }
        const double inputSnrDb = 10.0 * std::log10(
            cleanEnergy / std::max(1.0e-12, inputError));
        const double outputSnrDb = 10.0 * std::log10(
            cleanEnergy / std::max(1.0e-12, outputError));
        const double improvement = outputSnrDb - inputSnrDb;
        if (length > 0 && improvement >= 10.0)
            pass("G2 VoiceOnly improves 0 dB voice/noise SNR by 10 dB");
        else
            fail("G2 SNR improvement", QStringLiteral("input=%1 dB output=%2 dB improvement=%3 dB")
                 .arg(inputSnrDb, 0, 'f', 2).arg(outputSnrDb, 0, 'f', 2)
                 .arg(improvement, 0, 'f', 2));
    }

    // G3: complementary BackgroundOnly has substantially less speech-band energy.
    {
        voiceiso::VoiceIsolationParams backgroundParams = params;
        backgroundParams.mode = voiceiso::OutputMode::BackgroundOnly;
        const auto background = voiceiso::isolate(input, sampleRate, backgroundParams);
        const int first = static_cast<int>(0.8 * sampleRate);
        const double voiceBand = energyInBand(
            result.voice, sampleRate, 80.0, 8000.0, first, 32768);
        const double backgroundBand = energyInBand(
            background.output, sampleRate, 80.0, 8000.0, first, 32768);
        const double differenceDb = 10.0 * std::log10(
            voiceBand / std::max(1.0e-12, backgroundBand));
        if (differenceDb >= 6.0)
            pass("G3 BackgroundOnly is complementary in voice band");
        else
            fail("G3 complementary background", QStringLiteral("difference=%1 dB")
                 .arg(differenceDb, 0, 'f', 2));
    }

    // G4: strength zero is a true bypass for VoiceOnly.
    {
        auto bypassParams = params;
        bypassParams.strength = 0.0;
        const auto bypass = voiceiso::isolate(input, sampleRate, bypassParams);
        const double error = maxAbsDifference(bypass.output, input);
        if (error < 1.0e-3)
            pass("G4 strength=0 leaves output unchanged");
        else
            fail("G4 bypass", QStringLiteral("max error=%1").arg(error, 0, 'e', 6));
    }

    // G5: silence produces finite, silent outputs.
    {
        QVector<float> silence(sampleCount, 0.0f);
        const auto silent = voiceiso::isolate(silence, sampleRate, params);
        bool finite = true;
        double peak = 0.0;
        for (float sample : silent.output) {
            finite = finite && std::isfinite(sample);
            peak = std::max(peak, std::fabs(static_cast<double>(sample)));
        }
        if (finite && peak == 0.0)
            pass("G5 silence is finite and silent");
        else
            fail("G5 silence", QStringLiteral("finite=%1 peak=%2")
                 .arg(finite).arg(peak, 0, 'e', 6));
    }

    // G6: stationary noise and DC are not classified as mostly voice.
    {
        DeterministicNoise generator;
        QVector<float> noise(sampleCount, 0.0f);
        for (int i = 0; i < noise.size(); ++i)
            noise[i] = static_cast<float>(0.04 + generator.next() * 0.08);
        const auto noiseResult = voiceiso::isolate(noise, sampleRate, params);
        if (noiseResult.estimatedVoiceRatio < 0.25)
            pass("G6 stationary noise voice ratio stays below 0.25");
        else
            fail("G6 stationary noise", QStringLiteral("ratio=%1")
                 .arg(noiseResult.estimatedVoiceRatio, 0, 'f', 4));
    }

    // G7: the initial noise profile tracks known white-noise power.
    {
        DeterministicNoise generator;
        const double amplitude = 0.12;
        QVector<float> noise(sampleCount, 0.0f);
        for (float &sample : noise)
            sample = static_cast<float>(generator.next() * amplitude);
        const auto floor = voiceiso::estimateNoiseFloor(noise, sampleRate, params);
        double sum = 0.0;
        int count = 0;
        for (int k = 4; k < floor.size() - 4; ++k) {
            sum += floor[k];
            ++count;
        }
        const double estimated = count > 0 ? sum / count : 0.0;
        const double truePower = amplitude * amplitude / 3.0;
        const bool ok = estimated >= truePower * 0.5
            && estimated <= truePower * 2.0;
        if (ok)
            pass("G7 noise floor is within 0.5x..2x of known power");
        else
            fail("G7 noise floor", QStringLiteral("estimated=%1 true=%2")
                 .arg(estimated, 0, 'e', 6).arg(truePower, 0, 'e', 6));
    }

    // G8: energy above voiceHighHz is attenuated by at least 12 dB.
    {
        QVector<float> withHigh = input;
        const int highBin = spectral::hzToBin(12000.0, params.fftSize, sampleRate);
        const double highHz = spectral::binToHz(highBin, params.fftSize, sampleRate);
        const int noisePrefix = static_cast<int>(0.6 * sampleRate);
        for (int i = noisePrefix; i < withHigh.size(); ++i)
            withHigh[i] += static_cast<float>(0.45 * std::sin(
                2.0 * kPi * highHz * i / sampleRate));
        const auto highResult = voiceiso::isolate(withHigh, sampleRate, params);
        const int first = static_cast<int>(0.8 * sampleRate);
        const double before = energyInBand(
            withHigh, sampleRate, highHz - 40.0, highHz + 40.0, first, 32768);
        const double after = energyInBand(
            highResult.voice, sampleRate, highHz - 40.0, highHz + 40.0, first, 32768);
        const double attenuationDb = 10.0 * std::log10(
            before / std::max(1.0e-12, after));
        if (attenuationDb >= 12.0)
            pass("G8 out-of-band 12 kHz energy is reduced by 12 dB");
        else
            fail("G8 out-of-band removal", QStringLiteral("attenuation=%1 dB")
                 .arg(attenuationDb, 0, 'f', 2));
    }

    // G9: enabling the harmonic clue changes the output and improves SNR.
    {
        auto noHarmonics = params;
        noHarmonics.harmonicWeight = 0.0;
        auto fullHarmonics = params;
        fullHarmonics.harmonicWeight = 1.0;
        const auto without = voiceiso::isolate(input, sampleRate, noHarmonics);
        const auto with = voiceiso::isolate(input, sampleRate, fullHarmonics);
        double withoutError = 0.0;
        double withError = 0.0;
        const int first = static_cast<int>(0.8 * sampleRate);
        for (int i = first; i < input.size(); ++i) {
            const double clean = cleanVoice[i];
            withoutError += std::pow(static_cast<double>(without.voice[i]) - clean, 2.0);
            withError += std::pow(static_cast<double>(with.voice[i]) - clean, 2.0);
        }
        const double difference = maxAbsDifference(without.voice, with.voice);
        if (difference > 1.0e-5 && withError < withoutError)
            pass("G9 harmonicWeight=1 changes output and improves harmonic SNR");
        else
            fail("G9 harmonic clue", QStringLiteral("difference=%1 errors=%2/%3")
                 .arg(difference, 0, 'e', 6).arg(withError, 0, 'e', 6)
                 .arg(withoutError, 0, 'e', 6));
    }

    // G10: repeated execution is bit-deterministic.
    {
        const auto second = voiceiso::isolate(input, sampleRate, params);
        const bool same = result.voice == second.voice
            && result.background == second.background
            && result.output == second.output
            && result.estimatedVoiceRatio == second.estimatedVoiceRatio;
        if (same)
            pass("G10 repeated execution is bit-deterministic");
        else
            fail("G10 determinism", QStringLiteral("outputs differ"));
    }

    // G11: maximum UI smoothing still allows the mask to react to the input.
    // Before the fix, strength-blended previous=1 made this an exact dry
    // reconstruction for every interior sample.
    {
        auto maximumSmoothing = params;
        maximumSmoothing.smoothingFactor = 1.0;
        maximumSmoothing.harmonicWeight = 0.0;
        const auto smoothed = voiceiso::isolate(
            input, sampleRate, maximumSmoothing);
        const double difference = maxAbsDifferenceFrom(
            smoothed.output, input, maximumSmoothing.fftSize * 2);
        if (difference > 1.0e-3)
            pass("G11 smoothing=1 still applies separation");
        else
            fail("G11 maximum smoothing", QStringLiteral("difference=%1")
                 .arg(difference, 0, 'e', 6));
    }

    // G12: adaptive noise tracking follows a persistent noise rise instead of
    // stopping at initial*8. A stationary in-band tone is used so the
    // expected late floor is deterministic and is not confused with the
    // harmonic detector (which is disabled for this gate).
    {
        auto risingParams = params;
        risingParams.fftSize = 2048;
        risingParams.hopSize = 512;
        risingParams.noiseLearnSeconds = 0.5;
        risingParams.adaptiveNoise = true;
        risingParams.harmonicWeight = 0.0;
        risingParams.smoothingFactor = 0.0;
        const int risingCount = sampleRate * 4;
        const auto risingInput = makeNoiseRise(
            sampleRate, risingCount, sampleRate);
        const auto rising = voiceiso::isolate(
            risingInput, sampleRate, risingParams);
        if (rising.estimatedVoiceRatio < 0.35)
            pass("G12 adaptive floor follows persistent noise rise");
        else
            fail("G12 adaptive noise rise", QStringLiteral("voice ratio=%1")
                 .arg(rising.estimatedVoiceRatio, 0, 'f', 4));
    }

    // G13: a detected fundamental below voiceLowHz cannot bypass the band
    // attenuation, and silent bins near a harmonic cannot be forced open.
    {
        auto bandParams = params;
        bandParams.fftSize = 4096;
        bandParams.hopSize = 1024;
        bandParams.voiceLowHz = 200.0;
        bandParams.voiceHighHz = 8000.0;
        bandParams.noiseLearnSeconds = 0.5;
        bandParams.harmonicWeight = 1.0;
        bandParams.smoothingFactor = 0.0;
        bandParams.strength = 0.95;
        const auto lowInput = makeLowFundamental(sampleRate, sampleCount);
        const auto lowResult = voiceiso::isolate(
            lowInput, sampleRate, bandParams);
        const int first = static_cast<int>(0.9 * sampleRate);
        const double before = energyInBand(
            lowInput, sampleRate, 80.0, 140.0, first, 32768);
        const double after = energyInBand(
            lowResult.voice, sampleRate, 80.0, 140.0, first, 32768);
        const double attenuationDb = 10.0 * std::log10(
            before / std::max(1.0e-12, after));
        if (attenuationDb >= 12.0)
            pass("G13 below-band fundamental remains attenuated");
        else
            fail("G13 harmonic band guard", QStringLiteral("attenuation=%1 dB")
                 .arg(attenuationDb, 0, 'f', 2));
    }

    // G14: isolate publishes the floor from its own analysis, so the dialog
    // can display it without invoking a second full analysis pass.
    {
        const auto separatelyQueried = voiceiso::estimateNoiseFloor(
            input, sampleRate, params);
        const bool same = !result.noiseFloor.isEmpty()
            && result.noiseFloor == separatelyQueried;
        if (same)
            pass("G14 isolate returns its analyzed noise floor");
        else
            fail("G14 analysis result reuse", QStringLiteral(
                "floor sizes=%1/%2")
                .arg(static_cast<int>(result.noiseFloor.size()))
                .arg(static_cast<int>(separatelyQueried.size())));
    }

    // G15: the busy state gets one event-loop turn before synchronous DSP.
    // The queued timer would otherwise fire only after onAnalyzeClicked()
    // returns, leaving the dialog visually frozen at its old state.
    {
        VoiceIsolationDialog dialog;
        QVector<float> dialogSamples(sampleRate / 2, 0.0f);
        dialog.setAudioSource(QStringLiteral("selftest"),
                              dialogSamples, sampleRate);
        bool returned = false;
        bool firedDuringProcessing = false;
        QTimer::singleShot(0, [&]() {
            if (!returned)
                firedDuringProcessing = true;
        });
        const bool invoked = QMetaObject::invokeMethod(
            &dialog, "onAnalyzeClicked", Qt::DirectConnection);
        returned = true;
        QCoreApplication::processEvents();
        if (invoked && firedDuringProcessing)
            pass("G15 busy state yields to the event loop");
        else
            fail("G15 busy event turn", QStringLiteral("invoked=%1 fired=%2")
                 .arg(invoked).arg(firedDuringProcessing));
    }

    // G16: a 10-minute clip with a large FFT completes while only one frame
    // is resident. The previous all-frame implementation attempts several
    // gigabytes of storage for this same input and is expected to fail on the
    // supported 8 GB memory profile.
    {
        auto longParams = params;
        longParams.fftSize = 16384;
        longParams.hopSize = 4096;
        longParams.harmonicWeight = 0.0;
        longParams.smoothingFactor = 0.0;
        const int longSampleCount = sampleRate * 600;
        const QVector<float> longSilence(longSampleCount, 0.0f);
        const auto longResult = voiceiso::isolate(
            longSilence, sampleRate, longParams);
        bool finite = std::isfinite(longResult.estimatedVoiceRatio);
        finite = finite && static_cast<int>(longResult.output.size())
            == longSampleCount;
        if (finite)
            pass("G16 ten-minute isolation stays memory bounded");
        else
            fail("G16 long clip memory bound", QStringLiteral(
                "output size=%1 ratio=%2")
                .arg(static_cast<int>(longResult.output.size()))
                .arg(longResult.estimatedVoiceRatio, 0, 'f', 4));
    }

    qInfo().noquote() << "[voice-isolation] selftest done passed="
                      << passed << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
