#pragma once

#include <QVector>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <array>
#include <cstdint>

struct AudioEQConfig;

namespace trackfx {
struct EqBand {
    double freq;
    double gainDb;
    double q;
    bool enabled = true;
};

struct EqSettings {
    EqBand low{80.0, 0.0, 0.7};
    EqBand lowMid{250.0, 0.0, 1.0};
    EqBand highMid{3000.0, 0.0, 1.0};
    EqBand high{10000.0, 0.0, 0.7};
};

// Maps the legacy 3-band track EQ onto the 4-band EQ when the same filters can
// express it: preamp 0 dB, at most two non-flat bands, values inside the 4-band
// clamps. Otherwise returns false and leaves *out untouched.
bool eqFromLegacy(const AudioEQConfig &legacy, EqSettings *out);

struct EqBandCoefsParam {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    bool active = false; // false = pass-through (skip math, keep history pristine)
};

struct CompressorSettings {
    double thresholdDb = 0.0;     // 0 = no compression at any signal level
    double ratio = 1.0;           // 1:1 = no compression (pass-through)
    double attackMs = 5.0;
    double releaseMs = 100.0;
    double kneeDb = 2.0;
    double makeupDb = 0.0;
    bool enabled = false;
};

struct CompressorState {
    double env = 0.0;        // envelope follower (linear amplitude)
    double currentGrDb = 0.0;// last gain reduction in dB (for meter)
};

struct DeEsserSettings {
    double frequencyHz = 6000.0;
    double thresholdDb = -24.0;
    double ratio = 4.0;
    double rangeDb = 12.0;
    bool listen = false;
    bool enabled = false;
};

// Stereo-linked band-only attenuation. Samples use the PCM16 amplitude scale,
// but remain double so the master bus retains its above-full-scale headroom.
class DeEsserProcessor {
public:
    void setSettings(const DeEsserSettings &settings, int sampleRate);
    void processFrame(double &left, double &right);
    void reset();
    uint64_t processCallsForTest() const { return m_processCalls; }
private:
    DeEsserSettings m_settings;
    EqBandCoefsParam m_band;
    std::array<double, 4> m_history{};
    double m_env = 0.0;
    double m_attack = 0.0, m_release = 0.0;
    uint64_t m_processCalls = 0;
};

// Wave 34: 4-band phase-coherent Linkwitz-Riley (LR4) Multiband Compressor.
struct MultibandCompBand {
    double thresholdDb = -18.0; // -60..0 dBFS
    double ratio = 1.0;         // 1..20 (1:1 = pass-through)
    double attackMs = 10.0;     // 0.1..100 ms
    double releaseMs = 120.0;   // 10..1000 ms
    double kneeDb = 3.0;        // 0..12 dB
    double makeupDb = 0.0;      // -12..24 dB
    bool bypass = false;
    bool solo = false;          // preview audition only (ignored on export)
};

struct MultibandCompSettings {
    static constexpr int kBandCount = 4;
    static constexpr int kCrossoverCount = 3;
    std::array<double, kCrossoverCount> crossoverHz = {200.0, 1200.0, 5000.0};
    std::array<MultibandCompBand, kBandCount> bands = {
        MultibandCompBand{-20.0, 2.5, 20.0, 180.0, 4.0, 0.0, false, false},
        MultibandCompBand{-18.0, 2.0, 12.0, 140.0, 3.0, 0.0, false, false},
        MultibandCompBand{-16.0, 2.0,  8.0, 110.0, 3.0, 0.0, false, false},
        MultibandCompBand{-14.0, 2.5,  5.0,  80.0, 3.0, 0.0, false, false},
    };
    double outputGainDb = 0.0; // -24..24 dB
    double wetMix = 1.0;       // 0..1 (parallel compression blend)
    bool enabled = false;      // false = bit-exact bypass
};

// Built-in multiband compressor presets ("flat", "vocal", "master_punch",
// "narration", "podcast"). Returned settings have enabled=true.
QStringList multibandCompPresetIds();
QString multibandCompPresetLabel(const QString &presetId);
MultibandCompSettings multibandCompPreset(const QString &presetId);

// Phase-aligned 4-band LR4 crossover + per-band stereo-linked soft-knee
// compressor. Samples use the PCM16 amplitude scale (0 dBFS = 32768.0) as
// doubles, matching DeEsserProcessor so master bus headroom is preserved.
class MultibandCompProcessor {
public:
    static constexpr int kBandCount = MultibandCompSettings::kBandCount;
    void setSettings(const MultibandCompSettings &settings, int sampleRate);
    void setAllowSolo(bool allow) { m_allowSolo = allow; }
    void processFrame(double &left, double &right);
    void reset();
    std::array<double, kBandCount> currentGrDb() const { return m_currentGrDb; }
    uint64_t processCallsForTest() const { return m_processCalls; }

private:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };
    struct BiquadState {
        double z1 = 0.0, z2 = 0.0;
        inline double process(double x, const Biquad &c) {
            const double y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
        inline void reset() { z1 = 0.0; z2 = 0.0; }
    };
    // LR4 split = two cascaded 2nd-order Butterworth LP/HP filters.
    struct Lr4Split {
        Biquad lp, hp;
    };
    struct Lr4State {
        BiquadState lp1, lp2, hp1, hp2;
        inline void process(double x, const Lr4Split &c, double &lo, double &hi) {
            lo = lp2.process(lp1.process(x, c.lp), c.lp);
            hi = hp2.process(hp1.process(x, c.hp), c.hp);
        }
        inline void reset() { lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); }
    };
    struct ChannelState {
        Lr4State splitMid;     // f2
        BiquadState apLowPath; // AP2(f3) on loHalf
        BiquadState apHiPath;  // AP2(f1) on hiHalf
        Lr4State splitLow;     // f1 -> B0, B1
        Lr4State splitHigh;    // f3 -> B2, B3
        void reset() {
            splitMid.reset();
            apLowPath.reset();
            apHiPath.reset();
            splitLow.reset();
            splitHigh.reset();
        }
    };

    MultibandCompSettings m_settings;
    int m_sampleRate = 48000;
    bool m_allowSolo = true;
    Lr4Split m_split1{}, m_split2{}, m_split3{};
    Biquad m_ap1{}, m_ap3{};
    std::array<ChannelState, 2> m_ch{};
    std::array<double, kBandCount> m_env{};
    std::array<double, kBandCount> m_attack{};
    std::array<double, kBandCount> m_release{};
    std::array<double, kBandCount> m_currentGrDb{};
    double m_outputGainLin = 1.0;
    uint64_t m_processCalls = 0;
};

struct ReverbSettings {
    double mixRatio = 0.0;       // 0.0..1.0  (UI 0..100 / 100)
    double decaySeconds = 1.0;   // 0.1..5.0
    double preDelayMs = 20.0;    // 0..200
    double dampingHF = 30.0;     // 0..100
    double widthPercent = 50.0;  // 0..100
    bool enabled = false;
};

struct NoiseReductionSettings {
    double thresholdDb = -20.0;   // gate engages this many dB above floor
    double reductionDb = 12.0;    // max attenuation when fully gated
    double attackMs = 5.0;        // envelope attack
    double releaseMs = 200.0;     // envelope release
    double manualFloorDb = -50.0; // used when autoFloor == false
    bool autoFloor = true;
    bool enabled = false;
};

struct NRState {
    double env = 0.0;                  // envelope follower (linear amplitude)
    double estimatedFloorDb = -60.0;   // last computed auto-floor
    QVector<double> recentEnvs;        // rolling-window dBFS samples
    int recentEnvsHead = 0;            // circular write index
};

// Chain flags gate the stages in addition to each legacy settings.enabled flag.
struct Chain {
    NoiseReductionSettings nr;
    EqSettings eq;
    CompressorSettings comp;
    ReverbSettings reverb;
    DeEsserSettings deesser;
    MultibandCompSettings multibandComp;
    bool nrEnabled = false;
    bool eqEnabled = false;
    bool compEnabled = false;
    bool reverbEnabled = false;

    QJsonObject toJson() const;
    static Chain fromJson(const QJsonObject &object);
    bool isDefault() const;
};

// Normalized interleaved float input/output. Active stages deliberately retain
// the playback engine's PCM16 quantization at every stage boundary. Disabled
// chains return without touching input (including sub-PCM16 float precision).
// Mono is stereo-linked internally; supported channel counts are 1 and 2.
class Processor {
public:
    Processor();
    Processor(const Chain &chain, int sampleRate, int channels);
    void setChain(const Chain &chain); // preserves histories when settings change
    void process(float *interleaved, int frames);
    void reset();
    void resetFeedback(); // seek: preserve NR statistics and GR meter
    void setDeEsserBypassForTest(bool bypass) { m_deesserBypassForTest = bypass; }
    uint64_t deEsserProcessCallsForTest() const { return m_deesser.processCallsForTest(); }
    void setMultibandCompAllowSolo(bool allow) { m_multibandComp.setAllowSolo(allow); }
    std::array<double, MultibandCompSettings::kBandCount> currentMultibandGrDb() const {
        return m_multibandComp.currentGrDb();
    }
    uint64_t multibandCompProcessCallsForTest() const { return m_multibandComp.processCallsForTest(); }
    double currentGainReductionDb() const { return m_compState.currentGrDb; }
    double estimatedNoiseFloorDb() const { return m_nrState.estimatedFloorDb; }
private:
    static constexpr int kChannels = 2;
    static constexpr int kReverbCombCount = 4;
    static constexpr int kReverbAllpassCount = 2;
    static constexpr int kAutoFloorWindowSize = 256;
    struct ReverbState {
        // Per-channel circular pre-delay buffer, sized for 200 ms at sampleRate.
        std::array<QVector<float>, kChannels> preDelay{};
        std::array<int, kChannels> preDelayIdx{};
        // Per-channel comb buffers + indices + LP histories.
        std::array<std::array<QVector<float>, kReverbCombCount>, kChannels> comb{};
        std::array<std::array<int, kReverbCombCount>, kChannels> combIdx{};
        std::array<std::array<float, kReverbCombCount>, kChannels> combLP{};
        // Per-channel allpass buffers + indices.
        std::array<std::array<QVector<float>, kReverbAllpassCount>, kChannels> ap{};
        std::array<std::array<int, kReverbAllpassCount>, kChannels> apIdx{};
        bool initialized = false;
    };
    Chain m_chain;
    int m_sampleRate = 48000;
    int m_channels = 2;
    int m_preDelayCapacity = 9600;
    std::array<EqBandCoefsParam, 4> m_coefs{};
    std::array<double, 16> m_hist{};
    CompressorState m_compState;
    DeEsserProcessor m_deesser;
    bool m_deesserBypassForTest = false;
    MultibandCompProcessor m_multibandComp;
    NRState m_nrState;
    ReverbState m_reverbState;
};
} // namespace trackfx
