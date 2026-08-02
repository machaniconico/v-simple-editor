#include "VoiceIsolation.h"

#include "SpectralEngine.h"

#include <QVector>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <deque>
#include <limits>
#include <new>
#include <vector>

namespace voiceiso {

namespace {

constexpr int kDefaultFftSize = 2048;
constexpr double kPowerEpsilon = 1.0e-12;
constexpr double kMinF0Hz = 70.0;
constexpr double kMaxF0Hz = 400.0;
constexpr double kHarmonicToleranceBins = 3.0;
constexpr double kHarmonicFloor = 0.05;
constexpr double kStatsBias = 2.0;
constexpr double kMaxSmoothingFactor = 0.95;

struct NormalizedParams {
    OutputMode mode = OutputMode::VoiceOnly;
    double strength = 0.8;
    double voiceGainDb = 0.0;
    double backgroundGainDb = -18.0;
    double voiceLowHz = 80.0;
    double voiceHighHz = 8000.0;
    int fftSize = kDefaultFftSize;
    int hopSize = kDefaultFftSize / 4;
    double noiseLearnSeconds = 0.5;
    bool adaptiveNoise = true;
    double harmonicWeight = 0.5;
    double smoothingFactor = 0.7;
};

bool isPowerOfTwo(int value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

double clamp01(double value)
{
    return std::clamp(finiteOr(value, 0.0), 0.0, 1.0);
}

NormalizedParams normalizeParams(const VoiceIsolationParams &params,
                                 int sampleRate)
{
    NormalizedParams out;
    out.mode = params.mode;
    out.strength = clamp01(params.strength);
    out.voiceGainDb = finiteOr(params.voiceGainDb, 0.0);
    out.backgroundGainDb = finiteOr(params.backgroundGainDb, -18.0);
    out.fftSize = isPowerOfTwo(params.fftSize) && params.fftSize >= 64
        ? std::min(params.fftSize, 16384) : kDefaultFftSize;
    out.hopSize = params.hopSize > 0 && params.hopSize < out.fftSize
        ? params.hopSize : out.fftSize / 4;
    out.noiseLearnSeconds = std::clamp(
        finiteOr(params.noiseLearnSeconds, 0.5), 0.0, 30.0);
    out.adaptiveNoise = params.adaptiveNoise;
    out.harmonicWeight = clamp01(params.harmonicWeight);
    out.smoothingFactor = std::clamp(
        clamp01(params.smoothingFactor), 0.0, kMaxSmoothingFactor);

    const double nyquist = sampleRate > 0
        ? static_cast<double>(sampleRate) * 0.5 : 24000.0;
    out.voiceLowHz = std::clamp(finiteOr(params.voiceLowHz, 80.0),
                                0.0, nyquist);
    out.voiceHighHz = std::clamp(finiteOr(params.voiceHighHz, 8000.0),
                                 0.0, nyquist);
    if (out.voiceHighHz < out.voiceLowHz)
        std::swap(out.voiceHighHz, out.voiceLowHz);
    return out;
}

double windowEnergy(const std::vector<double> &window)
{
    double energy = 0.0;
    for (double value : window)
        energy += value * value;
    return std::max(kPowerEpsilon, energy);
}

int frameCountForSamples(int sampleCount, int hopSize)
{
    if (sampleCount <= 0 || hopSize <= 0)
        return 0;
    const std::size_t lastStart = static_cast<std::size_t>(sampleCount - 1);
    return static_cast<int>(lastStart / static_cast<std::size_t>(hopSize) + 1);
}

int learnedFrameCount(int frameCount,
                       int sampleRate,
                       int hopSize,
                       double learnSeconds)
{
    if (frameCount <= 0)
        return 0;
    return std::clamp(
        static_cast<int>(std::ceil(learnSeconds * sampleRate
                                   / static_cast<double>(std::max(1, hopSize)))) + 1,
        1, frameCount);
}

void fillStftFrame(const QVector<float> &samples,
                   int frameIndex,
                   int fftSize,
                   int hopSize,
                   const std::vector<double> &window,
                   std::vector<std::complex<double>> &frame)
{
    frame.resize(static_cast<std::size_t>(fftSize));
    const std::size_t frameStart = static_cast<std::size_t>(frameIndex)
        * static_cast<std::size_t>(hopSize);
    const std::size_t sampleCount = static_cast<std::size_t>(samples.size());
    for (int k = 0; k < fftSize; ++k) {
        const std::size_t index = frameStart + static_cast<std::size_t>(k);
        const double sampleValue = index < sampleCount
            ? static_cast<double>(samples[static_cast<int>(index)]) : 0.0;
        const double sample = std::isfinite(sampleValue) ? sampleValue : 0.0;
        frame[static_cast<std::size_t>(k)] = std::complex<double>(
            sample * window[static_cast<std::size_t>(k)], 0.0);
    }
    spectral::fft(frame, false);
}

void calculateFramePowers(const std::vector<std::complex<double>> &frame,
                          int halfBins,
                          double normalization,
                          QVector<double> &power)
{
    if (static_cast<int>(power.size()) != halfBins + 1)
        power.resize(halfBins + 1);
    for (int k = 0; k <= halfBins; ++k)
        power[k] = std::norm(frame[static_cast<std::size_t>(k)])
            / normalization;
}

QVector<double> estimateInitialNoiseFloor(const QVector<float> &samples,
                                           int sampleRate,
                                           const NormalizedParams &params,
                                           const std::vector<double> &window,
                                           double normalization)
{
    const int sampleCount = static_cast<int>(samples.size());
    const int frameCount = frameCountForSamples(sampleCount, params.hopSize);
    const int learnedFrames = learnedFrameCount(
        frameCount, sampleRate, params.hopSize, params.noiseLearnSeconds);
    if (learnedFrames <= 0)
        return {};

    const int halfBins = params.fftSize / 2;
    QVector<double> result(halfBins + 1, 0.0);
    QVector<double> power(halfBins + 1, 0.0);
    std::vector<std::complex<double>> frame;
    for (int frameIndex = 0; frameIndex < learnedFrames; ++frameIndex) {
        fillStftFrame(samples, frameIndex, params.fftSize, params.hopSize,
                      window, frame);
        calculateFramePowers(frame, halfBins, normalization, power);
        for (int k = 0; k <= halfBins; ++k)
            result[k] += power[k];
    }
    for (double &value : result)
        value = std::max(kPowerEpsilon,
                         value / static_cast<double>(learnedFrames));
    return result;
}

struct NoiseFloorTracker {
    QVector<double> initial;
    QVector<double> last;
    bool adaptive = false;
    int windowSize = 1;
    double initialDecayFrames = 1.0;
    std::vector<std::deque<std::pair<int, double>>> minimums;

    NoiseFloorTracker(const QVector<double> &initialFloor,
                      int sampleRate,
                      int hopSize,
                      bool useAdaptive)
        : initial(initialFloor), last(initialFloor), adaptive(useAdaptive)
    {
        const int framesPerSecond = std::max(
            1, static_cast<int>(std::ceil(sampleRate
                                          / static_cast<double>(std::max(1, hopSize)))));
        windowSize = std::clamp(framesPerSecond / 4, 8, 32);
        // Keep the learned reference strong at the beginning so stationary
        // noise is not mistaken for speech. It decays over ten seconds, so a
        // contaminated learn interval can still recover toward initial*0.25.
        initialDecayFrames = std::max(1.0, framesPerSecond * 10.0);
        if (adaptive)
            minimums.resize(static_cast<std::size_t>(initial.size()));
    }

    void update(const QVector<double> &power,
                int frameIndex,
                QVector<double> &floor,
                double protectedF0,
                const NormalizedParams &params,
                int sampleRate,
                int fftSize)
    {
        const int binCount = static_cast<int>(initial.size());
        if (static_cast<int>(floor.size()) != binCount)
            floor.resize(binCount);
        if (!adaptive) {
            floor = initial;
            return;
        }

        for (int k = 0; k < binCount; ++k) {
            bool protect = false;
            if (protectedF0 > 0.0) {
                const double hz = spectral::binToHz(k, fftSize, sampleRate);
                const double binHz = sampleRate
                    / static_cast<double>(fftSize);
                const int maxHarmonic = std::min(
                    12, static_cast<int>(std::floor(params.voiceHighHz
                                                     / protectedF0)));
                if (hz >= params.voiceLowHz && hz <= params.voiceHighHz) {
                    for (int harmonic = 1;
                         harmonic <= maxHarmonic && !protect; ++harmonic) {
                        const double harmonicHz = protectedF0 * harmonic;
                        protect = std::abs(hz - harmonicHz) / binHz
                            <= kHarmonicToleranceBins;
                    }
                }
            }
            if (protect) {
                floor[k] = last[k];
                continue;
            }

            auto &queue = minimums[static_cast<std::size_t>(k)];
            const double samplePower = std::max(kPowerEpsilon, power[k]);
            while (!queue.empty() && queue.back().second >= samplePower)
                queue.pop_back();
            queue.emplace_back(frameIndex, samplePower);
            while (!queue.empty()
                   && queue.front().first <= frameIndex - windowSize)
                queue.pop_front();

            const double correctedMinimum = queue.empty()
                ? samplePower : queue.front().second * kStatsBias;
            const double initialValue = std::max(kPowerEpsilon, initial[k]);
            const double initialWeight = std::exp(
                -static_cast<double>(frameIndex) / initialDecayFrames);
            const double lowerBound = initialValue
                * (0.25 + 0.75 * initialWeight);
            // The decaying lower bound prevents a transient minimum from
            // collapsing the floor, but there is intentionally no upper
            // clamp: a new persistent noise source must be allowed to raise it.
            floor[k] = std::max(correctedMinimum, lowerBound);
            last[k] = floor[k];
        }
    }
};

double localPower(const QVector<double> &power, int center, int radius = 1)
{
    if (power.isEmpty())
        return 0.0;
    const int first = std::max(0, center - radius);
    const int last = std::min(static_cast<int>(power.size()) - 1, center + radius);
    double best = 0.0;
    for (int k = first; k <= last; ++k)
        best = std::max(best, power[k]);
    return best;
}

double harmonicCandidateScore(const QVector<double> &power,
                              const QVector<double> &noiseFloor,
                              double f0,
                              int sampleRate,
                              int fftSize,
                              double highHz,
                              int *hitCount)
{
    if (power.isEmpty() || noiseFloor.isEmpty() || f0 <= 0.0)
        return 0.0;

    const int halfBins = fftSize / 2;
    const int maxHarmonic = std::min(
        12, static_cast<int>(std::floor(highHz / f0)));
    if (maxHarmonic < 2)
        return 0.0;

    double score = 0.0;
    int hits = 0;
    double fundamentalRatio = 0.0;
    for (int harmonic = 1; harmonic <= maxHarmonic; ++harmonic) {
        const double hz = f0 * harmonic;
        const int center = static_cast<int>(std::lround(
            hz * fftSize / static_cast<double>(sampleRate)));
        if (center <= 0 || center >= halfBins)
            continue;
        const double floor = std::max(kPowerEpsilon, noiseFloor[center]);
        const double ratio = localPower(power, center, 1) / floor;
        if (harmonic == 1)
            fundamentalRatio = ratio;
        const double excess = std::max(0.0, ratio - 1.0);
        score += std::log1p(excess);
        if (ratio >= 2.0)
            ++hits;
    }
    if (hitCount)
        *hitCount = hits;
    if (fundamentalRatio < 2.0)
        return 0.0;
    return score / static_cast<double>(maxHarmonic);
}

double detectF0(const QVector<double> &power,
                const QVector<double> &noiseFloor,
                const NormalizedParams &params,
                int sampleRate,
                int fftSize)
{
    if (params.harmonicWeight <= 0.0 || power.isEmpty())
        return 0.0;

    const double highHz = std::min(params.voiceHighHz,
                                   static_cast<double>(sampleRate) * 0.5);
    double bestF0 = 0.0;
    double bestScore = 0.0;
    int bestHits = 0;
    for (double f0 = kMinF0Hz; f0 <= kMaxF0Hz; f0 += 2.0) {
        int hits = 0;
        const double score = harmonicCandidateScore(
            power, noiseFloor, f0, sampleRate, fftSize, highHz, &hits);
        if (score > bestScore || (score == bestScore && hits > bestHits)) {
            bestScore = score;
            bestF0 = f0;
            bestHits = hits;
        }
    }

    // A noise-only frame can have isolated peaks, but it should not have
    // several harmonically aligned peaks with a strong average excess.
    return bestHits >= 4 && bestScore >= 0.45 ? bestF0 : 0.0;
}

double bandWeight(double hz, const NormalizedParams &params)
{
    if (hz >= params.voiceLowHz && hz <= params.voiceHighHz)
        return 1.0;
    if (hz <= 0.0)
        return params.voiceLowHz <= 0.0 ? 1.0 : 0.0;
    if (params.voiceHighHz <= 0.0)
        return 0.0;

    double octaves = 0.0;
    if (hz < params.voiceLowHz && params.voiceLowHz > 0.0)
        octaves = std::log2(params.voiceLowHz / hz);
    else if (hz > params.voiceHighHz && params.voiceHighHz > 0.0)
        octaves = std::log2(hz / params.voiceHighHz);
    return std::clamp(std::exp2(-8.0 * std::max(0.0, octaves)), 0.0, 1.0);
}

double harmonicWeightAt(double hz,
                        double f0,
                        const NormalizedParams &params,
                        int sampleRate,
                        int fftSize)
{
    if (f0 <= 0.0 || params.harmonicWeight <= 0.0 || hz <= 0.0)
        return 1.0;

    const double binHz = sampleRate / static_cast<double>(fftSize);
    const int maxHarmonic = std::min(
        12, static_cast<int>(std::floor(params.voiceHighHz / f0)));
    double nearestBins = std::numeric_limits<double>::infinity();
    for (int harmonic = 1; harmonic <= maxHarmonic; ++harmonic) {
        const double harmonicHz = f0 * harmonic;
        nearestBins = std::min(nearestBins,
                               std::abs(hz - harmonicHz) / binHz);
    }
    if (!std::isfinite(nearestBins))
        return 1.0;

    const double structure = std::exp(
        -0.5 * std::pow(nearestBins / kHarmonicToleranceBins, 2.0));
    const double selective = kHarmonicFloor
        + (1.0 - kHarmonicFloor) * structure;
    return std::pow(selective, params.harmonicWeight);
}

QVector<double> frequencySmoothed(const QVector<double> &mask)
{
    const int binCount = static_cast<int>(mask.size());
    QVector<double> out(binCount, 0.0);
    for (int k = 0; k < binCount; ++k) {
        double sum = 0.0;
        int count = 0;
        for (int offset = -1; offset <= 1; ++offset) {
            const int index = k + offset;
            if (index >= 0 && index < binCount) {
                sum += mask[index];
                ++count;
            }
        }
        out[k] = count > 0 ? sum / count : 0.0;
    }
    return out;
}

double gainFromDb(double db)
{
    return std::pow(10.0, finiteOr(db, 0.0) / 20.0);
}

VoiceIsolationResult bypassResult(const QVector<float> &samples,
                                  const NormalizedParams &params)
{
    VoiceIsolationResult result;
    result.voice = samples;
    result.background.fill(0.0f, samples.size());
    result.output.resize(samples.size());
    const float voiceGain = static_cast<float>(gainFromDb(params.voiceGainDb));
    for (int i = 0; i < samples.size(); ++i) {
        if (params.mode == OutputMode::VoiceOnly)
            result.output[i] = samples[i];
        else if (params.mode == OutputMode::BackgroundOnly)
            result.output[i] = 0.0f;
        else
            result.output[i] = samples[i] * voiceGain;
    }
    result.estimatedVoiceRatio = samples.isEmpty() ? 0.0 : 1.0;
    return result;
}

} // namespace

QVector<double> estimateNoiseFloor(const QVector<float> &samples,
                                   int sampleRate,
                                   const VoiceIsolationParams &params)
{
    if (samples.isEmpty() || sampleRate <= 0)
        return {};
    try {
        const NormalizedParams normalized = normalizeParams(params, sampleRate);
        const std::vector<double> window = spectral::hannWindow(normalized.fftSize);
        return estimateInitialNoiseFloor(
            samples, sampleRate, normalized, window, windowEnergy(window));
    } catch (const std::bad_alloc &) {
        return {};
    }
}

VoiceIsolationResult isolate(const QVector<float> &samples,
                             int sampleRate,
                             const VoiceIsolationParams &params)
{
    try {
        const NormalizedParams normalized = normalizeParams(params, sampleRate);
        if (samples.isEmpty())
            return {};

        const int sampleCount = static_cast<int>(samples.size());
        if (normalized.strength <= 0.0 || sampleRate <= 0) {
            VoiceIsolationResult result = bypassResult(samples, normalized);
            if (sampleRate > 0) {
                const std::vector<double> window = spectral::hannWindow(
                    normalized.fftSize);
                result.noiseFloor = estimateInitialNoiseFloor(
                    samples, sampleRate, normalized, window, windowEnergy(window));
            }
            return result;
        }

        // Only one FFT frame, the sliding floor state, and overlap-add rings
        // are retained. This keeps memory independent of clip duration.
        const std::vector<double> window = spectral::hannWindow(normalized.fftSize);
        const double normalization = windowEnergy(window);
        const QVector<double> initialFloor = estimateInitialNoiseFloor(
            samples, sampleRate, normalized, window, normalization);
        if (initialFloor.isEmpty())
            return bypassResult(samples, normalized);

        const int fftSize = normalized.fftSize;
        const int hopSize = normalized.hopSize;
        const int halfBins = fftSize / 2;
        const int frameCount = frameCountForSamples(sampleCount, hopSize);
        const int learnedFrames = learnedFrameCount(
            frameCount, sampleRate, hopSize, normalized.noiseLearnSeconds);
        const int ratioStart = static_cast<int>(std::min(
            static_cast<std::size_t>(sampleCount),
            static_cast<std::size_t>(learnedFrames)
                * static_cast<std::size_t>(hopSize)));

        VoiceIsolationResult result;
        result.noiseFloor = initialFloor;
        result.voice.fill(0.0f, sampleCount);
        result.background.fill(0.0f, sampleCount);
        result.output.fill(0.0f, sampleCount);

        NoiseFloorTracker tracker(initialFloor, sampleRate, hopSize,
                                  normalized.adaptiveNoise);
        QVector<double> power(halfBins + 1, 0.0);
        QVector<double> floor(halfBins + 1, 0.0);
        QVector<double> raw(halfBins + 1, 0.0);
        QVector<double> temporal(halfBins + 1, 0.0);
        QVector<double> previous(halfBins + 1, 1.0);
        std::vector<std::complex<double>> frame;

        const int ringSize = fftSize + hopSize;
        std::vector<double> voiceRing(static_cast<std::size_t>(ringSize), 0.0);
        std::vector<double> backgroundRing(static_cast<std::size_t>(ringSize), 0.0);
        std::vector<double> weightRing(static_cast<std::size_t>(ringSize), 0.0);
        int nextOutput = 0;

        const auto shiftRing = [hopSize](std::vector<double> &ring) {
            std::move(ring.begin() + hopSize, ring.end(), ring.begin());
            std::fill(ring.end() - hopSize, ring.end(), 0.0);
        };
        const auto emitRing = [&](int requested) {
            const int remaining = sampleCount - nextOutput;
            const int emitCount = std::min(requested, remaining);
            for (int offset = 0; offset < emitCount; ++offset) {
                const double weight = weightRing[static_cast<std::size_t>(offset)];
                const int outputIndex = nextOutput + offset;
                if (weight > 1.0e-9) {
                    const double voiceValue = voiceRing[
                        static_cast<std::size_t>(offset)] / weight;
                    const double backgroundValue = backgroundRing[
                        static_cast<std::size_t>(offset)] / weight;
                    result.voice[outputIndex] = static_cast<float>(
                        std::isfinite(voiceValue) ? voiceValue : 0.0);
                    result.background[outputIndex] = static_cast<float>(
                        std::isfinite(backgroundValue) ? backgroundValue : 0.0);
                }
            }
            nextOutput += emitCount;
        };

        for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            fillStftFrame(samples, frameIndex, fftSize, hopSize, window, frame);
            calculateFramePowers(frame, halfBins, normalization, power);
            // Use the learned reference for pitch detection so a voice that
            // has lasted longer than the noise window is still recognized
            // before its harmonics could raise the adaptive floor.
            const double f0 = detectF0(
                power, initialFloor, normalized, sampleRate, fftSize);
            tracker.update(power, frameIndex, floor, f0, normalized,
                           sampleRate, fftSize);
            const double voicePresence = normalized.harmonicWeight > 0.0
                && f0 <= 0.0 ? 0.4 : 1.0;
            for (int k = 0; k <= halfBins; ++k) {
                const double hz = spectral::binToHz(k, fftSize, sampleRate);
                const double noise = std::max(kPowerEpsilon, floor[k]);
                const double snr = std::max(0.0, power[k] - noise) / noise;
                const double wiener = snr / (1.0 + snr);
                const double band = bandWeight(hz, normalized);
                const double harmonic = harmonicWeightAt(
                    hz, f0, normalized, sampleRate, fftSize);
                raw[k] = std::clamp(
                    wiener * band * harmonic * voicePresence, 0.0, 1.0);
                temporal[k] = normalized.smoothingFactor * previous[k]
                    + (1.0 - normalized.smoothingFactor) * raw[k];
            }

            const QVector<double> smoothedRaw = frequencySmoothed(temporal);
            // The IIR state is the unblended raw mask. Strength is an output
            // control and must not be recursively applied on the next frame.
            previous = smoothedRaw;
            QVector<double> mask = smoothedRaw;
            for (double &value : mask)
                value = 1.0 - normalized.strength + normalized.strength * value;

            if (f0 > 0.0) {
                const double binHz = sampleRate / static_cast<double>(fftSize);
                const int maxHarmonic = std::min(
                    12, static_cast<int>(std::floor(normalized.voiceHighHz / f0)));
                for (int harmonic = 1; harmonic <= maxHarmonic; ++harmonic) {
                    const double harmonicHz = f0 * harmonic;
                    if (harmonicHz < normalized.voiceLowHz
                        || harmonicHz > normalized.voiceHighHz)
                        continue;
                    const int center = static_cast<int>(
                        std::lround(harmonicHz / binHz));
                    const int first = std::max(0, center - 2);
                    const int last = std::min(halfBins, center + 2);
                    for (int k = first; k <= last; ++k) {
                        const double hz = spectral::binToHz(k, fftSize, sampleRate);
                        const double distanceBins = std::abs(hz - harmonicHz)
                            / binHz;
                        const double noise = std::max(kPowerEpsilon, floor[k]);
                        if (distanceBins <= 2.0 && power[k] > noise * 2.0) {
                            const double harmonicMask =
                                0.98 * bandWeight(hz, normalized);
                            mask[k] = std::max(mask[k], harmonicMask);
                        }
                    }
                }
            }

            for (int k = 0; k <= halfBins; ++k) {
                const double m = std::clamp(mask[k], 0.0, 1.0);
                const double coefficient = m;
                frame[static_cast<std::size_t>(k)] *= coefficient;
                const int mirror = fftSize - k;
                if (mirror != k && mirror < fftSize)
                    frame[static_cast<std::size_t>(mirror)] *= coefficient;
            }
            spectral::fft(frame, true);

            const std::size_t frameStart = static_cast<std::size_t>(frameIndex)
                * static_cast<std::size_t>(hopSize);
            const int ringOffset = frameIndex == 0 ? 0 : hopSize;
            for (int k = 0; k < fftSize; ++k) {
                const std::size_t index = frameStart + static_cast<std::size_t>(k);
                const double source = index < static_cast<std::size_t>(sampleCount)
                    ? static_cast<double>(samples[static_cast<int>(index)]) : 0.0;
                const double safeSource = std::isfinite(source) ? source : 0.0;
                const double wk = window[static_cast<std::size_t>(k)];
                const double voiceSample = std::isfinite(
                    frame[static_cast<std::size_t>(k)].real())
                    ? frame[static_cast<std::size_t>(k)].real() : 0.0;
                const std::size_t ringIndex = static_cast<std::size_t>(
                    ringOffset + k);
                voiceRing[ringIndex] += voiceSample * wk;
                backgroundRing[ringIndex]
                    += (safeSource * wk - voiceSample) * wk;
                weightRing[ringIndex] += wk * wk;
            }

            if (frameIndex > 0) {
                emitRing(hopSize);
                shiftRing(voiceRing);
                shiftRing(backgroundRing);
                shiftRing(weightRing);
            }
        }

        if (nextOutput < sampleCount)
            emitRing(sampleCount - nextOutput);

        // The periodic Hann convention deliberately leaves the exact first
        // sample uncovered. Put only this reconstruction residual into the
        // complementary background so the public decomposition is exact.
        double totalEnergy = 0.0;
        double voiceEnergy = 0.0;
        const float voiceGain = static_cast<float>(gainFromDb(
            normalized.voiceGainDb));
        const float backgroundGain = static_cast<float>(gainFromDb(
            normalized.backgroundGainDb));
        for (int i = 0; i < sampleCount; ++i) {
            const double sourceValue = static_cast<double>(samples[i]);
            const double source = std::isfinite(sourceValue) ? sourceValue : 0.0;
            double voiceValue = static_cast<double>(result.voice[i]);
            double backgroundValue = static_cast<double>(result.background[i]);
            if (!std::isfinite(voiceValue))
                voiceValue = 0.0;
            if (!std::isfinite(backgroundValue))
                backgroundValue = 0.0;
            backgroundValue += source - (voiceValue + backgroundValue);
            result.voice[i] = static_cast<float>(voiceValue);
            result.background[i] = static_cast<float>(
                std::isfinite(backgroundValue) ? backgroundValue : 0.0);

            if (i >= ratioStart) {
                totalEnergy += source * source;
                voiceEnergy += voiceValue * voiceValue;
            }
            if (normalized.mode == OutputMode::VoiceOnly) {
                result.output[i] = result.voice[i];
            } else if (normalized.mode == OutputMode::BackgroundOnly) {
                result.output[i] = result.background[i];
            } else {
                result.output[i] = result.voice[i] * voiceGain
                    + result.background[i] * backgroundGain;
            }
            if (!std::isfinite(result.output[i]))
                result.output[i] = 0.0f;
        }
        result.estimatedVoiceRatio = totalEnergy > kPowerEpsilon
            ? std::clamp(voiceEnergy / totalEnergy, 0.0, 1.0) : 0.0;
        return result;
    } catch (const std::bad_alloc &) {
        // A resource failure must not terminate the editor. An empty output
        // lets the dialog report the failure without replacing source audio.
        return {};
    }
}

} // namespace voiceiso
