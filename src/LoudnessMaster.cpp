#include "LoudnessMaster.h"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace loudness {

// ---------------------------------------------------------------------------
// Gain / preset helpers
// ---------------------------------------------------------------------------
double computeGainDb(double measuredLufs, double targetLufs)
{
    // Positive when the program is quieter than target (needs boosting).
    return targetLufs - measuredLufs;
}

double presetTargetLufs(LoudnessPreset p)
{
    switch (p) {
    case LoudnessPreset::YouTube:    return -14.0;
    case LoudnessPreset::Spotify:    return -14.0;
    case LoudnessPreset::AppleMusic: return -16.0;
    case LoudnessPreset::Broadcast:  return -23.0;
    case LoudnessPreset::TikTok:     return -14.0;
    }
    return -14.0;
}

// ---------------------------------------------------------------------------
// ITU-R BS.1770-4 integrated loudness
// ---------------------------------------------------------------------------
namespace {

// Direct-form-I biquad. ITU-R BS.1770-4 K-weighting (a0 normalized to 1).
struct Biquad {
    double b0, b1, b2, a1, a2;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    inline double process(double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

} // namespace

double measureIntegratedLufsFromSamples(const QVector<float> &mono, int sampleRate)
{
    constexpr double kSilenceFloor   = -70.0;
    constexpr double kAbsoluteGate   = -70.0; // LUFS
    constexpr double kRelativeOffset = 10.0;  // LU

    const int n = mono.size();
    if (n == 0 || sampleRate <= 0)
        return kSilenceFloor;

    // Two-stage K-weighting (BS.1770-4) biquad coefficients @ 48 kHz.
    // Simplification: the 48 kHz coefficients are applied regardless of
    // sampleRate; non-48k input is filtered with the 48k constants.
    Biquad pre{ 1.53512485958697, -2.69169618940638, 1.19839281085285,
                -1.69065929318241, 0.73248077421585 };
    Biquad rlb{ 1.0, -2.0, 1.0,
                -1.99004745483398, 0.99007225036621 };

    // K-weighted signal.
    QVector<double> k(n);
    for (int i = 0; i < n; ++i) {
        const double s  = static_cast<double>(mono[i]);
        const double s1 = pre.process(s);
        k[i] = rlb.process(s1);
    }

    // 400 ms blocks, 75 % overlap (100 ms hop).
    const int blockLen = static_cast<int>(std::lround(0.400 * sampleRate));
    const int hopLen   = static_cast<int>(std::lround(0.100 * sampleRate));
    if (blockLen <= 0 || hopLen <= 0 || n < blockLen)
        return kSilenceFloor;

    QVector<double> blockMs;        // mean square per block
    QVector<double> blockLoudness;  // -0.691 + 10*log10(meanSquare)
    blockMs.reserve(n / hopLen + 1);
    blockLoudness.reserve(n / hopLen + 1);

    for (int start = 0; start + blockLen <= n; start += hopLen) {
        double sumSq = 0.0;
        for (int i = start; i < start + blockLen; ++i)
            sumSq += k[i] * k[i];

        const double ms = sumSq / static_cast<double>(blockLen);
        double l;
        if (ms > 0.0)
            l = -0.691 + 10.0 * std::log10(ms);
        else
            l = -std::numeric_limits<double>::infinity();

        blockMs.append(ms);
        blockLoudness.append(l);
    }

    if (blockMs.isEmpty())
        return kSilenceFloor;

    // Stage 1: absolute gate at -70 LUFS.
    QVector<double> absGatedMs;
    absGatedMs.reserve(blockMs.size());
    for (int i = 0; i < blockMs.size(); ++i) {
        if (blockLoudness[i] >= kAbsoluteGate && blockMs[i] > 0.0)
            absGatedMs.append(blockMs[i]);
    }

    if (absGatedMs.isEmpty())
        return kSilenceFloor;

    // Provisional integrated loudness from absolute-gated blocks.
    double meanMs = 0.0;
    for (double ms : absGatedMs)
        meanMs += ms;
    meanMs /= static_cast<double>(absGatedMs.size());

    const double provisional = -0.691 + 10.0 * std::log10(meanMs);

    // Stage 2: relative gate at (provisional - 10 LU).
    const double relThreshold = provisional - kRelativeOffset;

    double gatedSum   = 0.0;
    int    gatedCount = 0;
    for (int i = 0; i < blockMs.size(); ++i) {
        if (blockLoudness[i] >= kAbsoluteGate &&
            blockLoudness[i] >= relThreshold &&
            blockMs[i] > 0.0) {
            gatedSum += blockMs[i];
            ++gatedCount;
        }
    }

    if (gatedCount == 0)
        return kSilenceFloor;

    const double gatedMeanMs = gatedSum / static_cast<double>(gatedCount);
    if (gatedMeanMs <= 0.0)
        return kSilenceFloor;

    const double integrated = -0.691 + 10.0 * std::log10(gatedMeanMs);
    if (!std::isfinite(integrated) || integrated < kSilenceFloor)
        return kSilenceFloor;

    return integrated;
}

double measureIntegratedLufs(const QString &audioPath)
{
    // Raw PCM fast path: 32-bit float mono/interleaved is treated as mono.
    if (audioPath.endsWith(QStringLiteral(".raw"), Qt::CaseInsensitive) ||
        audioPath.endsWith(QStringLiteral(".pcm"), Qt::CaseInsensitive)) {
        QFile f(audioPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = f.readAll();
            f.close();

            const int floatCount = static_cast<int>(bytes.size() / sizeof(float));
            if (floatCount > 0) {
                QVector<float> mono(floatCount);
                std::memcpy(mono.data(), bytes.constData(),
                            static_cast<size_t>(floatCount) * sizeof(float));
                // Raw PCM carries no header; assume 48 kHz mono float.
                return measureIntegratedLufsFromSamples(mono, 48000);
            }
        }
        qWarning("loudness::measureIntegratedLufs: failed to read raw PCM '%s'",
                 qUtf8Printable(audioPath));
        return -23.0;
    }

    // Compressed/container decoding is not wired here; a later integration
    // story routes this through the project's decoder facilities.
    qWarning("loudness::measureIntegratedLufs: audio decoding not wired for "
             "'%s'; returning broadcast fallback (-23.0 LUFS)",
             qUtf8Printable(audioPath));
    return -23.0;
}

} // namespace loudness
