#pragma once

#include <QVector>
#include <array>
#include <cstdint>

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
    bool nrEnabled = false;
    bool eqEnabled = false;
    bool compEnabled = false;
    bool reverbEnabled = false;
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
    NRState m_nrState;
    ReverbState m_reverbState;
};
} // namespace trackfx
