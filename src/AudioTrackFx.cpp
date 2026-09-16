#include "AudioTrackFx.h"

#include <QVarLengthArray>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace trackfx {
namespace {
struct BiquadCoefs { double b0, b1, b2, a1, a2; };

BiquadCoefs calcLowShelf(double freq, double gainDB, double fs) {
    const double w = 2.0 * M_PI * freq / fs;
    const double cosW = std::cos(w);
    const double sinW = std::sin(w);
    const double A = std::pow(10.0, gainDB / 40.0);
    const double twoSqrtAalpha = 2.0 * std::sqrt(A) * (sinW * std::sqrt(2.0) / 2.0);
    const double Ap1 = A + 1.0;
    const double Am1 = A - 1.0;
    double b0 = A * (Ap1 - Am1 * cosW + twoSqrtAalpha);
    double b1 = 2.0 * A * (Am1 - Ap1 * cosW);
    double b2 = A * (Ap1 - Am1 * cosW - twoSqrtAalpha);
    double a0 = Ap1 + Am1 * cosW + twoSqrtAalpha;
    double a1 = -2.0 * (Am1 + Ap1 * cosW);
    double a2 = Ap1 + Am1 * cosW - twoSqrtAalpha;
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

BiquadCoefs calcPeaking(double freq, double gainDB, double Q, double fs) {
    const double w = 2.0 * M_PI * freq / fs;
    const double cosW = std::cos(w);
    const double sinW = std::sin(w);
    const double A = std::pow(10.0, gainDB / 40.0);
    const double alpha = sinW / (2.0 * Q);
    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cosW;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cosW;
    double a2 = 1.0 - alpha / A;
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

BiquadCoefs calcHighShelf(double freq, double gainDB, double fs) {
    const double w = 2.0 * M_PI * freq / fs;
    const double cosW = std::cos(w);
    const double sinW = std::sin(w);
    const double A = std::pow(10.0, gainDB / 40.0);
    const double twoSqrtAalpha = 2.0 * std::sqrt(A) * (sinW * std::sqrt(2.0) / 2.0);
    const double Ap1 = A + 1.0;
    const double Am1 = A - 1.0;
    double b0 = A * (Ap1 + Am1 * cosW + twoSqrtAalpha);
    double b1 = -2.0 * A * (Am1 + Ap1 * cosW);
    double b2 = A * (Ap1 + Am1 * cosW - twoSqrtAalpha);
    double a0 = Ap1 - Am1 * cosW + twoSqrtAalpha;
    double a1 = 2.0 * (Am1 - Ap1 * cosW);
    double a2 = Ap1 - Am1 * cosW - twoSqrtAalpha;
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

EqBandCoefsParam computeEqBand(const EqBand &band,
                                           int kind, // 0=low shelf, 1/2=peak, 3=high shelf
                                           double fs) {
    EqBandCoefsParam out;
    if (!band.enabled || band.gainDb == 0.0) {
        out.active = false; // pass-through
        return out;
    }
    // Clamp f0 in [20, fs/2 - 100], Q in [0.1, 10], gainDb in [-24, +24].
    const double nyqMargin = fs / 2.0 - 100.0;
    const double f0 = qBound(20.0, band.freq, nyqMargin);
    const double q = qBound(0.1, band.q, 10.0);
    const double gainDb = qBound(-24.0, band.gainDb, 24.0);

    BiquadCoefs c;
    if (kind == 0)      c = calcLowShelf(f0, gainDb, fs);
    else if (kind == 3) c = calcHighShelf(f0, gainDb, fs);
    else                c = calcPeaking(f0, gainDb, q, fs);
    out.b0 = c.b0; out.b1 = c.b1; out.b2 = c.b2; out.a1 = c.a1; out.a2 = c.a2;
    out.active = true;
    return out;
}
} // namespace

Processor::Processor() : Processor(Chain{}, 48000, 2) {}
Processor::Processor(const Chain &chain, int sampleRate, int channels)
    : m_sampleRate(sampleRate), m_channels(channels)
{
    if (sampleRate < 1000 || sampleRate > 384000 || (channels != 1 && channels != 2))
        throw std::invalid_argument("trackfx: unsupported sample rate/channel count");
    m_preDelayCapacity = std::max(1, sampleRate / 5);
    setChain(chain);
}

void Processor::setChain(const Chain &chain)
{
    m_chain = chain;
    m_coefs[0] = computeEqBand(chain.eq.low, 0, m_sampleRate);
    m_coefs[1] = computeEqBand(chain.eq.lowMid, 1, m_sampleRate);
    m_coefs[2] = computeEqBand(chain.eq.highMid, 2, m_sampleRate);
    m_coefs[3] = computeEqBand(chain.eq.high, 3, m_sampleRate);
}

void Processor::resetFeedback()
{
    m_hist.fill(0.0);
    m_compState.env = 0.0;
    m_nrState.env = 0.0;
    for (int ch = 0; ch < kChannels; ++ch) {
        m_reverbState.preDelay[ch].fill(0.0f);
        m_reverbState.preDelayIdx[ch] = 0;
        for (int b = 0; b < kReverbCombCount; ++b) {
            m_reverbState.comb[ch][b].fill(0.0f);
            m_reverbState.combIdx[ch][b] = 0;
            m_reverbState.combLP[ch][b] = 0.0f;
        }
        for (int a = 0; a < kReverbAllpassCount; ++a) {
            m_reverbState.ap[ch][a].fill(0.0f);
            m_reverbState.apIdx[ch][a] = 0;
        }
    }
}

void Processor::reset()
{
    resetFeedback();
    m_compState = {};
    m_nrState = {};
}

void Processor::process(float *interleaved, int frames)
{
    if (!interleaved || frames <= 0 || frames > std::numeric_limits<int>::max() / 2) return;
    const bool eqActive = m_chain.eqEnabled && (m_coefs[0].active || m_coefs[1].active
        || m_coefs[2].active || m_coefs[3].active);
    if (!eqActive
        && !(m_chain.nrEnabled && m_chain.nr.enabled && m_chain.nr.reductionDb > 1e-6)
        && !(m_chain.compEnabled && m_chain.comp.enabled && m_chain.comp.ratio > 1.0001)
        && !(m_chain.reverbEnabled && m_chain.reverb.enabled && m_chain.reverb.mixRatio > 1e-6)) return;
    const int copySamples = frames * 2;
    QVarLengthArray<int16_t, 8192> input(copySamples);
    for (int i = 0; i < copySamples; ++i) {
        const float x = interleaved[m_channels == 1 ? i / 2 : i];
        input[i] = static_cast<int16_t>(std::clamp(std::isfinite(x) ? x * 32768.0f : 0.0f,
                                                  -32768.0f, 32767.0f));
    }
    const int16_t *src = input.constData();
    // Per-track noise reduction (Audition Voice Isolation / Resolve
    // Fairlight noise gate parity, simplified expander). Runs FIRST in
    // the per-track chain so downstream EQ / compressor / reverb
    // amplify the de-noised signal rather than the noise. Stereo-link
    // detector with attack / release envelope; static curve compares
    // the envelope (in dBFS) to a noise floor (auto-estimated as the
    // 5th-percentile of a rolling window OR user-set manual floor) and
    // applies a smooth ramp between full pass-through and reductionDb
    // of attenuation across the user-configured threshold band.
    // Disabled = bit-exact bypass (skip the entire DSP path; do not
    // touch state).
    QVarLengthArray<int16_t, 8192> scratchNr;
    if (m_chain.nrEnabled && m_chain.nr.enabled && m_chain.nr.reductionDb > 1e-6) {
        const auto &nr = m_chain.nr;
        const double fs       = static_cast<double>(m_sampleRate);
        const double atkCoef  = std::exp(-1.0 / (nr.attackMs  * fs * 0.001));
        const double relCoef  = std::exp(-1.0 / (nr.releaseMs * fs * 0.001));

        NRState &nrState = m_nrState;
        double env = nrState.env;

        // Determine the active noise floor for this fragment. Auto
        // mode pulls the 5th-percentile of the rolling envelope-dBFS
        // history; manual mode uses the user-set value directly.
        double floorDb = nr.manualFloorDb;
        if (nr.autoFloor && !nrState.recentEnvs.isEmpty()) {
            QVector<double> sorted = nrState.recentEnvs;
            std::sort(sorted.begin(), sorted.end());
            const int idx = qBound(0,
                static_cast<int>(sorted.size() * 0.05),
                sorted.size() - 1);
            floorDb = sorted[idx];
        }
        nrState.estimatedFloorDb = floorDb;

        // Threshold sets the band where we transition from full
        // attenuation to full pass-through. We center the smooth
        // ramp around (floorDb + thresholdDb) with a fixed 12 dB
        // wide ramp width (perceived smoothness target — narrower
        // pumps audibly, wider weakens the gating action).
        const double thresholdAbsDb = floorDb + nr.thresholdDb;
        constexpr double kRampWidthDb = 12.0;
        const double rampLo = thresholdAbsDb - 0.5 * kRampWidthDb;
        const double rampHi = thresholdAbsDb + 0.5 * kRampWidthDb;
        const double minGainLin = std::pow(10.0, -nr.reductionDb / 20.0);

        scratchNr.resize(copySamples);
        // Per stereo frame: linked level → envelope → static curve.
        for (int i = 0; i + 1 < copySamples; i += 2) {
            const double xL = static_cast<double>(src[i]);
            const double xR = static_cast<double>(src[i + 1]);
            const double level = std::max(std::abs(xL), std::abs(xR));
            if (level > env) {
                env = level + (env - level) * atkCoef;
            } else {
                env = level + (env - level) * relCoef;
            }

            // dBFS reference 32768 = 0 dBFS.
            const double envDb = 20.0
                * std::log10(std::max(env, 1e-9) / 32768.0);

            double nrGain;
            if (envDb >= rampHi) {
                nrGain = 1.0;            // signal loud enough — no NR
            } else if (envDb <= rampLo) {
                nrGain = minGainLin;     // signal at/below floor — full NR
            } else {
                // Smooth linear ramp in dB domain → exponential in linear.
                const double t = (envDb - rampLo) / kRampWidthDb;
                const double curveDb = -nr.reductionDb * (1.0 - t);
                nrGain = std::pow(10.0, curveDb / 20.0);
            }

            double yL = xL * nrGain;
            double yR = xR * nrGain;
            if (yL > 32767.0) yL = 32767.0;
            else if (yL < -32768.0) yL = -32768.0;
            if (yR > 32767.0) yR = 32767.0;
            else if (yR < -32768.0) yR = -32768.0;
            scratchNr[i]     = static_cast<int16_t>(yL);
            scratchNr[i + 1] = static_cast<int16_t>(yR);
        }
        // Odd-sample tail (mono runt) — pass through unchanged.
        for (int i = copySamples & ~1; i < copySamples; ++i) {
            scratchNr[i] = src[i];
        }
        nrState.env = env;

        // Append the post-fragment envelope-in-dBFS into the rolling
        // window (used next fragment for auto-floor estimation). One
        // entry per fragment keeps the percentile sort cheap; with
        // kAutoFloorWindowSize=256 entries that's ~85 s of history
        // worst case, ~5 s typical.
        const double tailEnvDb = 20.0
            * std::log10(std::max(env, 1e-9) / 32768.0);
        if (nrState.recentEnvs.size() < kAutoFloorWindowSize) {
            nrState.recentEnvs.append(tailEnvDb);
        } else {
            if (nrState.recentEnvsHead < 0
                || nrState.recentEnvsHead >= kAutoFloorWindowSize) {
                nrState.recentEnvsHead = 0;
            }
            nrState.recentEnvs[nrState.recentEnvsHead] = tailEnvDb;
            nrState.recentEnvsHead =
                (nrState.recentEnvsHead + 1) % kAutoFloorWindowSize;
        }
        src = scratchNr.data();
    }

    // Per-track 4-band parametric EQ cascade (Premiere/Audition parity).
    // Runs BEFORE the legacy 3-band path and gain/preamp stages; signal
    // flow: 4-band → 3-band → preamp → gain. When all 4 bands are flat
    // or disabled, src points at the ring buffer unchanged (bit-exact
    // bypass). When any band is active, scratchEq holds the filtered
    // samples and src is rebound to it. Disabled bands skip math
    // entirely AND skip history updates so re-enabling has no transient.
    QVarLengthArray<int16_t, 8192> scratchEq;
    if (m_chain.eqEnabled) {
        const auto &coefs = m_coefs;
        const bool anyActive = coefs[0].active || coefs[1].active
                            || coefs[2].active || coefs[3].active;
        if (anyActive) {
            auto &hist = m_hist;
            scratchEq.resize(copySamples);
            // hist layout per band b, channel ch: x1=hist[b*4+ch],
            // x2=hist[b*4+2+ch], y1=hist[b*4+ch] ... actually we use
            // 4 doubles per band (2 channels x x1/x2 OR y1/y2)? We need
            // 4 doubles per band per channel for a direct-form-I biquad
            // (x1, x2, y1, y2). But we only allocated 16 = 4 bands x
            // 2 channels x 2 history samples — encode x1/y1 in pairs by
            // using a transposed direct-form-II (z1, z2 per channel).
            // hist[b*4 + ch*2 + 0] = z1[ch], hist[b*4 + ch*2 + 1] = z2[ch].
            for (int i = 0; i < copySamples; ++i) {
                const int ch = i & 1;
                double s = static_cast<double>(src[i]);
                for (int b = 0; b < 4; ++b) {
                    const auto &c = coefs[b];
                    if (!c.active) continue; // skip math + history
                    const int base = b * 4 + ch * 2;
                    double &z1 = hist[base];
                    double &z2 = hist[base + 1];
                    // Transposed direct-form-II:
                    //   y = b0*x + z1
                    //   z1 = b1*x - a1*y + z2
                    //   z2 = b2*x - a2*y
                    const double y = c.b0 * s + z1;
                    z1 = c.b1 * s - c.a1 * y + z2;
                    z2 = c.b2 * s - c.a2 * y;
                    s = y;
                }
                // Soft saturation to s16 range — high gain peak boost on
                // a band can exceed s16 momentarily. Clamp before re-cast
                // so the existing 3-band path receives a valid s16.
                if (s > 32767.0) s = 32767.0;
                else if (s < -32768.0) s = -32768.0;
                scratchEq[i] = static_cast<int16_t>(s);
            }
            src = scratchEq.data();
        }
    }

    // Per-track feed-forward compressor / limiter (Audition / Resolve
    // Fairlight parity). Runs AFTER the 4-band EQ stage and BEFORE the
    // 3-band / preamp / gain stages so per-track meters and the master
    // bus see post-compression peaks. Stereo-link detector,
    // transposed envelope follower, Cherny-style soft knee.
    // Disabled or 1:1 ratio = bit-exact bypass (skip envelope + curve).
    QVarLengthArray<int16_t, 8192> scratchComp;
    if (m_chain.compEnabled && m_chain.comp.enabled && m_chain.comp.ratio > 1.0001) {
        const auto &comp = m_chain.comp;
        // Limiter mode collapses to 1000:1 with a fixed 0.5 ms attack.
        const bool limiter   = comp.ratio >= 20.0;
        const double effRatio = limiter ? 1000.0 : comp.ratio;
        const double effAttackMs = limiter ? 0.5 : comp.attackMs;

        const double fs       = static_cast<double>(m_sampleRate);
        const double atkCoef  = std::exp(-1.0 / (effAttackMs * fs * 0.001));
        const double relCoef  = std::exp(-1.0 / (comp.releaseMs * fs * 0.001));
        const double slope    = 1.0 - 1.0 / effRatio; // dB-per-dB above thresh
        const double makeupLin = std::pow(10.0, comp.makeupDb / 20.0);
        const double thresh   = comp.thresholdDb;
        const double kneeHalf = 0.5 * comp.kneeDb;

        double env = m_compState.env;
        double lastGrDb = m_compState.currentGrDb;

        scratchComp.resize(copySamples);
        // copySamples is interleaved L/R (stereo), so iterate by frame.
        for (int i = 0; i + 1 < copySamples; i += 2) {
            const double xL = static_cast<double>(src[i]);
            const double xR = static_cast<double>(src[i + 1]);
            const double level = std::max(std::abs(xL), std::abs(xR));
            // Envelope follower: attack on rising, release on falling.
            if (level > env) {
                env = level + (env - level) * atkCoef;
            } else {
                env = level + (env - level) * relCoef;
            }

            // Static gain curve in dB. Reference 0 dBFS at 32768.
            const double envDb = 20.0 * std::log10(std::max(env, 1e-9) / 32768.0);
            double grDb = 0.0;
            if (kneeHalf > 1e-6 && envDb > thresh - kneeHalf
                && envDb < thresh + kneeHalf) {
                // Soft knee: quadratic spline between (thresh - knee/2, 0)
                // and (thresh + knee/2, knee/2 * slope).
                const double x = envDb - (thresh - kneeHalf);
                grDb = slope * (x * x) / (2.0 * comp.kneeDb);
            } else if (envDb > thresh + kneeHalf) {
                grDb = slope * (envDb - thresh);
            }
            lastGrDb = grDb;

            const double gainLin = std::pow(10.0, (comp.makeupDb - grDb) / 20.0);
            // Skip the makeup multiply collapse — combine into one mul.
            (void)makeupLin;
            double yL = xL * gainLin;
            double yR = xR * gainLin;
            if (yL > 32767.0) yL = 32767.0;
            else if (yL < -32768.0) yL = -32768.0;
            if (yR > 32767.0) yR = 32767.0;
            else if (yR < -32768.0) yR = -32768.0;
            scratchComp[i]     = static_cast<int16_t>(yL);
            scratchComp[i + 1] = static_cast<int16_t>(yR);
        }
        // Odd-sample tail (mono runt) — extremely unlikely with stereo
        // s16 fragments but guarded for safety.
        for (int i = copySamples & ~1; i < copySamples; ++i) {
            scratchComp[i] = src[i];
        }
        m_compState.env = env;
        m_compState.currentGrDb = lastGrDb;
        src = scratchComp.data();
    }

    // Per-track reverb (Audition / Fairlight Multitap parity, simplified
    // Schroeder topology). Cascaded AFTER the compressor stage and BEFORE
    // the legacy 3-band / preamp / gain stages so meters and the master
    // bus see the post-reverb signal. Disabled or mixRatio=0 = bit-exact
    // bypass (skip the entire DSP path; do not touch state).
    //
    // Signal flow per channel:
    //   in → preDelay buffer → sum(comb1..4 with damped feedback)
    //      → allpass1 → allpass2 → wet
    //   out = dry * (1 - mix) + wet * mix
    // Width is applied as a stereo cross-feed between the two channel
    // wet outputs (0=mono, 100=fully decorrelated).
    QVarLengthArray<int16_t, 8192> scratchRev;
    if (m_chain.reverbEnabled && m_chain.reverb.enabled && m_chain.reverb.mixRatio > 1e-6) {
        const auto &rev = m_chain.reverb;
        // Comb / allpass delays: Freeverb defaults @ 44.1 kHz, scaled to
        // current sample rate so reverb character is consistent across
        // hardware. Stereo offset (~23 samples) decorrelates L/R combs.
        constexpr int kCombDelays44k1[kReverbCombCount]    = {1116, 1188, 1277, 1356};
        constexpr int kCombStereoOffset44k1                = 23;
        constexpr int kAllpassDelays44k1[kReverbAllpassCount] = {556, 441};
        const double srScale = static_cast<double>(m_sampleRate) / 44100.0;
        auto scaleDelay = [srScale](int n) {
            return std::max(1, static_cast<int>(std::lround(n * srScale)));
        };

        ReverbState &st = m_reverbState;
        // Lazy buffer initialization on first activation. Subsequent
        // calls preserve buffer contents so live tweaks don't pop.
        if (!st.initialized) {
            for (int ch = 0; ch < kChannels; ++ch) {
                for (int b = 0; b < kReverbCombCount; ++b) {
                    const int sz = scaleDelay(kCombDelays44k1[b]
                        + (ch == 1 ? kCombStereoOffset44k1 : 0));
                    st.comb[ch][b].assign(sz, 0.0f);
                    st.combIdx[ch][b] = 0;
                    st.combLP[ch][b]  = 0.0f;
                }
                for (int a = 0; a < kReverbAllpassCount; ++a) {
                    const int sz = scaleDelay(kAllpassDelays44k1[a]
                        + (ch == 1 ? kCombStereoOffset44k1 / 2 : 0));
                    st.ap[ch][a].assign(sz, 0.0f);
                    st.apIdx[ch][a] = 0;
                }
                st.preDelay[ch].fill(0.0f, m_preDelayCapacity);
                st.preDelayIdx[ch] = 0;
            }
            st.initialized = true;
        }

        // Pre-delay length in samples (clamped to buffer size).
        const int preDelaySamples = std::clamp(
            static_cast<int>(std::lround(rev.preDelayMs * m_sampleRate / 1000.0)),
            0, m_preDelayCapacity - 1);
        // Comb feedback gain: g = 0.7 * decay / standardDecay (1.0 sec).
        // Clamped to <0.98 for unconditional stability across the comb
        // bank (each comb's pole magnitude must stay inside the unit
        // circle even with HF damping subtracting energy).
        const float fbGain = static_cast<float>(
            std::clamp(0.7 * rev.decaySeconds / 1.0, 0.0, 0.97));
        // HF damping coefficient: 0 = no LP (bright tail), ~1 = heavy
        // LP (dark tail). Implemented as one-pole LP inside the comb
        // feedback path: y = (1-d)*x + d*y_prev.
        const float damp = static_cast<float>(
            std::clamp(rev.dampingHF / 100.0, 0.0, 0.95));
        const float dampInv = 1.0f - damp;
        constexpr float kAllpassGain = 0.5f;
        const float mix = static_cast<float>(std::clamp(rev.mixRatio, 0.0, 1.0));
        const float dry = 1.0f - mix;
        // Width: 0 = mono sum (collapse), 100 = pass-through.
        // We blend each channel's wet with the cross-channel wet.
        const float width = static_cast<float>(
            std::clamp(rev.widthPercent / 100.0, 0.0, 1.0));
        const float crossFeed = 0.5f * (1.0f - width); // 0 at width=100, 0.5 at width=0
        const float selfFeed  = 1.0f - crossFeed;

        scratchRev.resize(copySamples);
        // Process per stereo frame so we can apply width cross-feed.
        for (int i = 0; i + 1 < copySamples; i += 2) {
            float wet[kChannels];
            for (int ch = 0; ch < kChannels; ++ch) {
                const float xin = static_cast<float>(src[i + ch]);
                // Pre-delay: write input, read N samples back.
                auto &pd = st.preDelay[ch];
                int &pdIdx = st.preDelayIdx[ch];
                pd[pdIdx] = xin;
                int readIdx = pdIdx - preDelaySamples;
                if (readIdx < 0) readIdx += m_preDelayCapacity;
                const float xPre = pd[readIdx];
                pdIdx = (pdIdx + 1) % m_preDelayCapacity;

                // Sum 4 parallel comb filters with damped feedback.
                float combSum = 0.0f;
                for (int b = 0; b < kReverbCombCount; ++b) {
                    auto &buf = st.comb[ch][b];
                    const int sz = buf.size();
                    if (sz <= 0) continue;
                    int &idx = st.combIdx[ch][b];
                    const float yDelayed = buf[idx];
                    // One-pole LP on the feedback tap.
                    float &lp = st.combLP[ch][b];
                    lp = dampInv * yDelayed + damp * lp;
                    buf[idx] = xPre + lp * fbGain;
                    idx = (idx + 1) % sz;
                    combSum += yDelayed;
                }
                // Normalise comb output (4 parallel paths).
                combSum *= 0.25f;

                // Series allpass filters (decorrelation).
                float ap = combSum;
                for (int a = 0; a < kReverbAllpassCount; ++a) {
                    auto &buf = st.ap[ch][a];
                    const int sz = buf.size();
                    if (sz <= 0) continue;
                    int &idx = st.apIdx[ch][a];
                    const float bufIn = ap + buf[idx] * kAllpassGain;
                    const float yOut  = buf[idx] - bufIn * kAllpassGain;
                    buf[idx] = bufIn;
                    idx = (idx + 1) % sz;
                    ap = yOut;
                }
                wet[ch] = ap;
            }

            // Stereo width cross-feed: collapse toward mono as width→0.
            const float wetL = selfFeed * wet[0] + crossFeed * wet[1];
            const float wetR = selfFeed * wet[1] + crossFeed * wet[0];

            float yL = dry * static_cast<float>(src[i])     + mix * wetL;
            float yR = dry * static_cast<float>(src[i + 1]) + mix * wetR;
            if (yL > 32767.0f) yL = 32767.0f;
            else if (yL < -32768.0f) yL = -32768.0f;
            if (yR > 32767.0f) yR = 32767.0f;
            else if (yR < -32768.0f) yR = -32768.0f;
            scratchRev[i]     = static_cast<int16_t>(yL);
            scratchRev[i + 1] = static_cast<int16_t>(yR);
        }
        // Odd-sample tail (mono runt) — pass through unchanged.
        for (int i = copySamples & ~1; i < copySamples; ++i) {
            scratchRev[i] = src[i];
        }
        src = scratchRev.data();
    }


    for (int i = 0; i < frames * m_channels; ++i)
        interleaved[i] = src[m_channels == 1 ? i * 2 : i] / 32768.0f;
}
} // namespace trackfx
