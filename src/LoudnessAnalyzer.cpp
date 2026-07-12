#include "LoudnessAnalyzer.h"

#include <QHash>
#include <QJsonArray>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr double kLufsOffset = -0.691;
constexpr double kNoSignalLufs = -70.0;
constexpr double kNoSignalDbtp = -120.0;
constexpr int kMomentarySteps = 4;   // 400 ms with 100 ms hop size.
constexpr int kShortTermSteps = 30;  // 3 s with 100 ms hop size.
constexpr double kPi = 3.14159265358979323846;
constexpr int kTruePeakFirRadius = 4;
constexpr int kTruePeakFirTaps = kTruePeakFirRadius * 2;

struct TruePeakHistory {
    std::array<double, kTruePeakFirTaps> samples{};
    int filled = 0;
};

QHash<const LoudnessAnalyzer *, QVector<TruePeakHistory>> &truePeakHistories()
{
    static QHash<const LoudnessAnalyzer *, QVector<TruePeakHistory>> histories;
    return histories;
}

double sinc(double x)
{
    if (std::abs(x) < 1.0e-12) {
        return 1.0;
    }
    const double pix = kPi * x;
    return std::sin(pix) / pix;
}

double lanczosKernel(double distance)
{
    const double x = std::abs(distance);
    if (x >= static_cast<double>(kTruePeakFirRadius)) {
        return 0.0;
    }
    return sinc(distance) * sinc(distance / static_cast<double>(kTruePeakFirRadius));
}

double estimateTruePeakBetweenDelayedSamples(const TruePeakHistory &history, double fraction)
{
    double value = 0.0;
    double weightSum = 0.0;
    for (int i = 0; i < kTruePeakFirTaps; ++i) {
        const double sampleTime = static_cast<double>(kTruePeakFirRadius - i);
        const double weight = lanczosKernel(fraction - sampleTime);
        value += history.samples[i] * weight;
        weightSum += weight;
    }
    if (std::abs(weightSum) > 1.0e-12) {
        value /= weightSum;
    }
    return value;
}

double absoluteGateEnergy()
{
    static const double value = std::pow(10.0, (kNoSignalLufs - kLufsOffset) / 10.0);
    return value;
}

LoudnessAnalyzer::Biquad makeHighShelf(double sampleRate, double cutoffHz, double gainDb)
{
    const double omega = 2.0 * kPi * cutoffHz / sampleRate;
    const double sinOmega = std::sin(omega);
    const double cosOmega = std::cos(omega);
    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double beta = std::sqrt(amplitude) * sinOmega * std::sqrt(2.0);
    const double aPlusOne = amplitude + 1.0;
    const double aMinusOne = amplitude - 1.0;

    const double b0 = amplitude * (aPlusOne + aMinusOne * cosOmega + beta);
    const double b1 = -2.0 * amplitude * (aMinusOne + aPlusOne * cosOmega);
    const double b2 = amplitude * (aPlusOne + aMinusOne * cosOmega - beta);
    const double a0 = aPlusOne - aMinusOne * cosOmega + beta;
    const double a1 = 2.0 * (aMinusOne - aPlusOne * cosOmega);
    const double a2 = aPlusOne - aMinusOne * cosOmega - beta;

    return {
        b0 / a0,
        b1 / a0,
        b2 / a0,
        a1 / a0,
        a2 / a0
    };
}

LoudnessAnalyzer::Biquad makeHighPass(double sampleRate, double cutoffHz, double q)
{
    const double omega = 2.0 * kPi * cutoffHz / sampleRate;
    const double sinOmega = std::sin(omega);
    const double cosOmega = std::cos(omega);
    const double alpha = sinOmega / (2.0 * q);

    const double b0 = (1.0 + cosOmega) / 2.0;
    const double b1 = -(1.0 + cosOmega);
    const double b2 = (1.0 + cosOmega) / 2.0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosOmega;
    const double a2 = 1.0 - alpha;

    return {
        b0 / a0,
        b1 / a0,
        b2 / a0,
        a1 / a0,
        a2 / a0
    };
}
} // namespace

LoudnessAnalyzer::LoudnessAnalyzer(QObject *parent)
    : QObject(parent)
{
    updateFilterCoefficients();
    reset();
}

double LoudnessAnalyzer::targetLUFS(DeliveryTarget target)
{
    switch (target) {
    case YouTube:
        return -14.0;
    case Spotify:
        return -14.0;
    case AppleMusic:
        return -16.0;
    case BroadcastEBU:
        return -23.0;
    case Cinema:
        return -24.0;
    case Custom:
    default:
        return -14.0;
    }
}

void LoudnessAnalyzer::setSampleRate(int sampleRate)
{
    if (sampleRate <= 0 || sampleRate == m_sampleRate) {
        return;
    }

    m_sampleRate = sampleRate;
    updateFilterCoefficients();
    reset();
}

void LoudnessAnalyzer::reset()
{
    m_channelCount = 0;
    m_currentStepEnergySum = 0.0;
    m_currentStepFrameCount = 0;
    m_maxTruePeakLinear = 0.0;
    truePeakHistories().remove(this);
    m_channelStates.clear();
    m_recentStepEnergies.clear();
    m_blockEnergies.clear();
}

void LoudnessAnalyzer::processBlock(const float *interleaved, int frames, int channels)
{
    if (!interleaved || frames <= 0 || channels <= 0) {
        return;
    }

    ensureChannelState(channels);

    for (int frame = 0; frame < frames; ++frame) {
        double frameEnergy = 0.0;
        const int baseIndex = frame * channels;
        for (int channel = 0; channel < channels; ++channel) {
            const double sample = static_cast<double>(interleaved[baseIndex + channel]);
            ChannelState &state = m_channelStates[channel];
            updateTruePeak(sample, state);

            const double pre = applyBiquad(sample, m_preFilter, state.preFilter);
            const double weighted = applyBiquad(pre, m_rlbFilter, state.rlbFilter);
            frameEnergy += weighted * weighted;
        }
        // Normalize by active channel count so a dual-mono editor preview
        // reads the same LUFS as the equivalent mono source.
        frameEnergy /= static_cast<double>(channels);

        m_currentStepEnergySum += frameEnergy;
        ++m_currentStepFrameCount;

        if (m_currentStepFrameCount >= m_stepFrames) {
            appendStepEnergy(m_currentStepEnergySum / static_cast<double>(m_currentStepFrameCount));
            m_currentStepEnergySum = 0.0;
            m_currentStepFrameCount = 0;
        }
    }
}

double LoudnessAnalyzer::integratedLUFS() const
{
    return integratedFromBlocks();
}

double LoudnessAnalyzer::momentaryLUFS() const
{
    if (!m_blockEnergies.isEmpty()) {
        return loudnessFromEnergy(m_blockEnergies.last());
    }
    return rollingWindowLoudness(kMomentarySteps);
}

double LoudnessAnalyzer::shortTermLUFS() const
{
    return rollingWindowLoudness(kShortTermSteps);
}

double LoudnessAnalyzer::truePeakDBTP() const
{
    if (m_maxTruePeakLinear <= 0.0) {
        return kNoSignalDbtp;
    }
    return 20.0 * std::log10(m_maxTruePeakLinear);
}

double LoudnessAnalyzer::gainToReach(double targetLUFSValue) const
{
    return targetLUFSValue - integratedLUFS();
}

QJsonObject LoudnessAnalyzer::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("sampleRate"), m_sampleRate);
    json.insert(QStringLiteral("channelCount"), m_channelCount);
    json.insert(QStringLiteral("currentStepEnergySum"), m_currentStepEnergySum);
    json.insert(QStringLiteral("currentStepFrameCount"), m_currentStepFrameCount);
    json.insert(QStringLiteral("maxTruePeakLinear"), m_maxTruePeakLinear);
    json.insert(QStringLiteral("integratedLUFS"), integratedLUFS());
    json.insert(QStringLiteral("momentaryLUFS"), momentaryLUFS());
    json.insert(QStringLiteral("shortTermLUFS"), shortTermLUFS());
    json.insert(QStringLiteral("truePeakDBTP"), truePeakDBTP());

    QJsonArray stepArray;
    for (double energy : m_recentStepEnergies) {
        stepArray.append(energy);
    }
    json.insert(QStringLiteral("recentStepEnergies"), stepArray);

    QJsonArray blockArray;
    for (double energy : m_blockEnergies) {
        blockArray.append(energy);
    }
    json.insert(QStringLiteral("blockEnergies"), blockArray);

    QJsonArray channelArray;
    for (const ChannelState &state : m_channelStates) {
        QJsonObject channel;
        channel.insert(QStringLiteral("lastRawSample"), state.lastRawSample);
        channel.insert(QStringLiteral("hasLastRawSample"), state.hasLastRawSample);

        QJsonObject pre;
        pre.insert(QStringLiteral("x1"), state.preFilter.x1);
        pre.insert(QStringLiteral("x2"), state.preFilter.x2);
        pre.insert(QStringLiteral("y1"), state.preFilter.y1);
        pre.insert(QStringLiteral("y2"), state.preFilter.y2);
        channel.insert(QStringLiteral("preFilter"), pre);

        QJsonObject rlb;
        rlb.insert(QStringLiteral("x1"), state.rlbFilter.x1);
        rlb.insert(QStringLiteral("x2"), state.rlbFilter.x2);
        rlb.insert(QStringLiteral("y1"), state.rlbFilter.y1);
        rlb.insert(QStringLiteral("y2"), state.rlbFilter.y2);
        channel.insert(QStringLiteral("rlbFilter"), rlb);

        channelArray.append(channel);
    }
    json.insert(QStringLiteral("channels"), channelArray);

    return json;
}

void LoudnessAnalyzer::fromJson(const QJsonObject &json)
{
    truePeakHistories().remove(this);

    const int sampleRate = json.value(QStringLiteral("sampleRate")).toInt(m_sampleRate);
    if (sampleRate > 0 && sampleRate != m_sampleRate) {
        m_sampleRate = sampleRate;
        updateFilterCoefficients();
    }

    m_stepFrames = std::max(1, m_sampleRate / 10);
    m_channelCount = std::max(0, json.value(QStringLiteral("channelCount")).toInt());
    m_currentStepEnergySum = json.value(QStringLiteral("currentStepEnergySum")).toDouble();
    m_currentStepFrameCount = std::max(0, json.value(QStringLiteral("currentStepFrameCount")).toInt());
    m_maxTruePeakLinear = std::max(0.0, json.value(QStringLiteral("maxTruePeakLinear")).toDouble());

    m_recentStepEnergies.clear();
    const QJsonArray stepArray = json.value(QStringLiteral("recentStepEnergies")).toArray();
    m_recentStepEnergies.reserve(stepArray.size());
    for (const QJsonValue &value : stepArray) {
        m_recentStepEnergies.append(std::max(0.0, value.toDouble()));
    }

    m_blockEnergies.clear();
    const QJsonArray blockArray = json.value(QStringLiteral("blockEnergies")).toArray();
    m_blockEnergies.reserve(blockArray.size());
    for (const QJsonValue &value : blockArray) {
        m_blockEnergies.append(std::max(0.0, value.toDouble()));
    }

    m_channelStates.clear();
    const QJsonArray channelArray = json.value(QStringLiteral("channels")).toArray();
    m_channelStates.reserve(channelArray.size());
    for (const QJsonValue &value : channelArray) {
        const QJsonObject channel = value.toObject();
        ChannelState state;
        state.lastRawSample = channel.value(QStringLiteral("lastRawSample")).toDouble();
        state.hasLastRawSample = channel.value(QStringLiteral("hasLastRawSample")).toBool(false);

        const QJsonObject pre = channel.value(QStringLiteral("preFilter")).toObject();
        state.preFilter.x1 = pre.value(QStringLiteral("x1")).toDouble();
        state.preFilter.x2 = pre.value(QStringLiteral("x2")).toDouble();
        state.preFilter.y1 = pre.value(QStringLiteral("y1")).toDouble();
        state.preFilter.y2 = pre.value(QStringLiteral("y2")).toDouble();

        const QJsonObject rlb = channel.value(QStringLiteral("rlbFilter")).toObject();
        state.rlbFilter.x1 = rlb.value(QStringLiteral("x1")).toDouble();
        state.rlbFilter.x2 = rlb.value(QStringLiteral("x2")).toDouble();
        state.rlbFilter.y1 = rlb.value(QStringLiteral("y1")).toDouble();
        state.rlbFilter.y2 = rlb.value(QStringLiteral("y2")).toDouble();

        m_channelStates.append(state);
    }

    if (m_channelStates.size() < static_cast<qsizetype>(m_channelCount)) {
        m_channelStates.resize(m_channelCount);
    } else {
        m_channelCount = static_cast<int>(m_channelStates.size());
    }
}

void LoudnessAnalyzer::updateFilterCoefficients()
{
    m_stepFrames = std::max(1, m_sampleRate / 10);

    if (m_sampleRate == 48000) {
        // Official BS.1770 K-weighting coefficients for 48 kHz.
        m_preFilter = {
            1.53512485958697,
            -2.69169618940638,
            1.19839281085285,
            -1.69065929318241,
            0.73248077421585
        };
        m_rlbFilter = {
            1.0,
            -2.0,
            1.0,
            -1.99004745483398,
            0.99007225036621
        };
        return;
    }

    // Non-48 kHz fallback: approximate the BS.1770 shape with a +4 dB
    // shelf around 1.5 kHz followed by the RLB high-pass near 38 Hz.
    const double safeRate = static_cast<double>(std::max(8000, m_sampleRate));
    m_preFilter = makeHighShelf(safeRate, 1500.0, 4.0);
    m_rlbFilter = makeHighPass(safeRate, 38.0, 0.5);
}

void LoudnessAnalyzer::ensureChannelState(int channels)
{
    if (channels <= 0) {
        return;
    }

    if (m_channelStates.size() < static_cast<qsizetype>(channels)) {
        m_channelStates.resize(channels);
    }
    m_channelCount = channels;
}

double LoudnessAnalyzer::applyBiquad(double sample, const Biquad &coeffs, BiquadState &state) const
{
    const double result =
        coeffs.b0 * sample +
        coeffs.b1 * state.x1 +
        coeffs.b2 * state.x2 -
        coeffs.a1 * state.y1 -
        coeffs.a2 * state.y2;

    state.x2 = state.x1;
    state.x1 = sample;
    state.y2 = state.y1;
    state.y1 = result;
    return result;
}

double LoudnessAnalyzer::integratedFromBlocks() const
{
    if (m_blockEnergies.isEmpty()) {
        return kNoSignalLufs;
    }

    double ungatedEnergySum = 0.0;
    int ungatedCount = 0;
    for (double energy : m_blockEnergies) {
        if (energy >= absoluteGateEnergy()) {
            ungatedEnergySum += energy;
            ++ungatedCount;
        }
    }

    if (ungatedCount == 0) {
        return kNoSignalLufs;
    }

    const double ungatedMean = ungatedEnergySum / static_cast<double>(ungatedCount);
    const double relativeGate = ungatedMean / 10.0;

    double gatedEnergySum = 0.0;
    int gatedCount = 0;
    for (double energy : m_blockEnergies) {
        if (energy >= absoluteGateEnergy() && energy >= relativeGate) {
            gatedEnergySum += energy;
            ++gatedCount;
        }
    }

    if (gatedCount == 0) {
        return kNoSignalLufs;
    }

    return loudnessFromEnergy(gatedEnergySum / static_cast<double>(gatedCount));
}

double LoudnessAnalyzer::loudnessFromEnergy(double energy) const
{
    if (energy <= 0.0) {
        return kNoSignalLufs;
    }
    return std::max(kNoSignalLufs, kLufsOffset + 10.0 * std::log10(energy));
}

double LoudnessAnalyzer::rollingWindowLoudness(int steps) const
{
    if (steps <= 0 || m_recentStepEnergies.isEmpty()) {
        return kNoSignalLufs;
    }

    const int stepCount = static_cast<int>(m_recentStepEnergies.size());
    const int available = std::min(steps, stepCount);
    double energySum = 0.0;
    for (int index = stepCount - available; index < stepCount; ++index) {
        energySum += m_recentStepEnergies.at(index);
    }
    return loudnessFromEnergy(energySum / static_cast<double>(available));
}

void LoudnessAnalyzer::appendStepEnergy(double meanSquare)
{
    m_recentStepEnergies.append(std::max(0.0, meanSquare));
    while (m_recentStepEnergies.size() > static_cast<qsizetype>(kShortTermSteps)) {
        m_recentStepEnergies.removeFirst();
    }

    const int stepCount = static_cast<int>(m_recentStepEnergies.size());
    if (stepCount < kMomentarySteps) {
        return;
    }

    double blockEnergy = 0.0;
    for (int index = stepCount - kMomentarySteps; index < stepCount; ++index) {
        blockEnergy += m_recentStepEnergies.at(index);
    }
    m_blockEnergies.append(blockEnergy / static_cast<double>(kMomentarySteps));
}

void LoudnessAnalyzer::updateTruePeak(double sample, ChannelState &state)
{
    const double absSample = std::abs(sample);
    m_maxTruePeakLinear = std::max(m_maxTruePeakLinear, absSample);

    int channelIndex = -1;
    if (!m_channelStates.isEmpty()) {
        const ChannelState *first = m_channelStates.constData();
        const ChannelState *last = first + m_channelStates.size();
        if (&state >= first && &state < last) {
            channelIndex = static_cast<int>(&state - first);
        }
    }

    if (channelIndex >= 0) {
        QVector<TruePeakHistory> &histories = truePeakHistories()[this];
        if (histories.size() < m_channelStates.size()) {
            histories.resize(m_channelStates.size());
        }

        TruePeakHistory &history = histories[channelIndex];
        if (!state.hasLastRawSample) {
            history = {};
        }

        for (int i = kTruePeakFirTaps - 1; i > 0; --i) {
            history.samples[i] = history.samples[i - 1];
        }
        history.samples[0] = sample;
        history.filled = std::min(history.filled + 1, kTruePeakFirTaps);

        constexpr std::array<double, 3> fractions{0.25, 0.5, 0.75};
        for (double fraction : fractions) {
            if (history.filled < kTruePeakFirTaps) {
                break;
            }
            const double interpolated = estimateTruePeakBetweenDelayedSamples(history, fraction);
            m_maxTruePeakLinear = std::max(m_maxTruePeakLinear, std::abs(interpolated));
        }
    }

    state.lastRawSample = sample;
    state.hasLastRawSample = true;
}
