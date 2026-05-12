#pragma once

#include <QObject>
#include <QJsonObject>
#include <QVector>

enum DeliveryTarget {
    YouTube,
    Spotify,
    AppleMusic,
    BroadcastEBU,
    Cinema,
    Custom
};

class LoudnessAnalyzer : public QObject {
    Q_OBJECT

public:
    explicit LoudnessAnalyzer(QObject *parent = nullptr);

    static double targetLUFS(DeliveryTarget target);

    void setSampleRate(int sampleRate);
    void reset();
    void processBlock(const float *interleaved, int frames, int channels);

    double integratedLUFS() const;
    double momentaryLUFS() const;
    double shortTermLUFS() const;
    double truePeakDBTP() const;
    double gainToReach(double targetLUFS) const;

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &json);

    // Public so the K-weighting filter coefficient helpers (free functions in
    // LoudnessAnalyzer.cpp) can return it.
    struct Biquad {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

private:
    struct BiquadState {
        double x1 = 0.0;
        double x2 = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
    };

    struct ChannelState {
        BiquadState preFilter;
        BiquadState rlbFilter;
        double lastRawSample = 0.0;
        bool hasLastRawSample = false;
    };

    void updateFilterCoefficients();
    void ensureChannelState(int channels);
    double applyBiquad(double sample, const Biquad &coeffs, BiquadState &state) const;
    double integratedFromBlocks() const;
    double loudnessFromEnergy(double energy) const;
    double rollingWindowLoudness(int steps) const;
    void appendStepEnergy(double meanSquare);
    void updateTruePeak(double sample, ChannelState &state);

    int m_sampleRate = 48000;
    int m_stepFrames = 48000 / 10;
    int m_channelCount = 0;
    double m_currentStepEnergySum = 0.0;
    int m_currentStepFrameCount = 0;
    double m_maxTruePeakLinear = 0.0;
    Biquad m_preFilter;
    Biquad m_rlbFilter;
    QVector<ChannelState> m_channelStates;
    QVector<double> m_recentStepEnergies;
    QVector<double> m_blockEnergies;
};
