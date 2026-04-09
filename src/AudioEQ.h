#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QVector>

// --- EQ Band ---

struct EQBand {
    double frequency = 1000.0;  // Hz
    double gain = 0.0;          // -20 to +20 dB
    double q = 1.0;             // 0.1 to 10.0 (bandwidth)

    bool isFlat() const { return gain == 0.0; }
};

// --- EQ Configuration ---

struct AudioEQConfig {
    QVector<EQBand> bands;
    double preamp = 0.0;        // dB pre-amplification

    bool isDefault() const {
        if (preamp != 0.0) return false;
        for (const auto &b : bands) {
            if (!b.isFlat()) return false;
        }
        return true;
    }
};

// --- Audio Effect Type ---

enum class AudioEffectType {
    EQ,
    Reverb,
    Compressor,
    Normalize,
    FadeIn,
    FadeOut,
    BassBoost,
    VoiceEnhance
};

// --- Audio Effect ---
//
//   Params — meaning depends on type:
//     Reverb:       p1=roomSize(0-1), p2=damping(0-1), p3=wetLevel(0-1)
//     Compressor:   p1=threshold(-50 to 0 dB), p2=ratio(1-20), p3=attack(ms), p4=release(ms)
//     Normalize:    p1=targetLevel(-20 to 0 dB)
//     FadeIn:       p1=duration(seconds)
//     FadeOut:      p1=duration(seconds)
//     BassBoost:    p1=gain(0-20 dB), p2=frequency(20-300 Hz)
//     VoiceEnhance: p1=clarity(0-1)

struct AudioEffect {
    AudioEffectType type = AudioEffectType::EQ;
    bool enabled = true;
    double p1 = 0.0;
    double p2 = 0.0;
    double p3 = 0.0;
    double p4 = 0.0;

    static QString typeName(AudioEffectType t);
    static QVector<AudioEffectType> allTypes();

    // Factory helpers
    static AudioEffect createReverb(double roomSize = 0.5, double damping = 0.5,
                                    double wetLevel = 0.3);
    static AudioEffect createCompressor(double threshold = -20.0, double ratio = 4.0,
                                        double attack = 5.0, double release = 50.0);
    static AudioEffect createNormalize(double targetLevel = -3.0);
    static AudioEffect createFadeIn(double duration = 1.0);
    static AudioEffect createFadeOut(double duration = 1.0);
    static AudioEffect createBassBoost(double gain = 6.0, double frequency = 100.0);
    static AudioEffect createVoiceEnhance(double clarity = 0.5);
};

// --- EQ Preset ---

struct EQPreset {
    QString name;
    AudioEQConfig config;
};

// --- Audio EQ Processor ---

class AudioEQProcessor : public QObject
{
    Q_OBJECT

public:
    explicit AudioEQProcessor(QObject *parent = nullptr);

    // Apply EQ using FFmpeg equalizer filter
    void applyEQ(const QString &inputPath, const QString &outputPath,
                 const AudioEQConfig &eqConfig);

    // Apply a single audio effect
    void applyEffect(const QString &inputPath, const QString &outputPath,
                     const AudioEffect &effect);

    // Apply EQ + multiple effects in chain
    void applyChain(const QString &inputPath, const QString &outputPath,
                    const AudioEQConfig &eqConfig,
                    const QVector<AudioEffect> &effects);

    // Build FFmpeg audio filter graph string
    static QString buildFilterString(const AudioEQConfig &eqConfig,
                                     const QVector<AudioEffect> &effects = {});

    // Built-in EQ presets
    static QVector<EQPreset> presets();

    void cancel();

signals:
    void progressChanged(int percent);
    void processComplete(bool success, const QString &message);

private:
    // Build filter string for a single EQ config
    static QString buildEQFilter(const AudioEQConfig &eqConfig);

    // Build filter string for a single effect
    static QString buildEffectFilter(const AudioEffect &effect);

    // Run FFmpeg process and parse progress
    bool runFFmpeg(const QStringList &args);

    // Parse duration and time= from FFmpeg stderr
    void parseProgress(const QString &output);

    // Find FFmpeg binary
    static QString findFFmpegBinary();

    QProcess *m_process = nullptr;
    bool m_cancelled = false;
    double m_totalDuration = 0.0;
};
