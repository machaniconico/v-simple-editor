#pragma once

namespace playback {

struct PlaybackMetrics {
    int trackCount;
    int activeClipCount;
    double lastFrameRenderMs;
    double targetFrameMs;
    bool isPlaying;
    bool proxyAvailable;
};

struct PlaybackQualityDecision {
    double scaleFactor;
    bool useProxy;
    bool allowFrameDrop;
};

struct PlaybackQualityConfig {
    double slowFactor;        // default 1.2: lastFrameRenderMs > targetFrameMs*slowFactor -> degrade
    double recoverFactor;     // default 0.7: lastFrameRenderMs < targetFrameMs*recoverFactor -> recover (hysteresis)
    int    proxyTrackThreshold; // default 3: trackCount >= this -> proxy candidate
    double minScale;          // default 0.25

    PlaybackQualityConfig()
        : slowFactor(1.2)
        , recoverFactor(0.7)
        , proxyTrackThreshold(3)
        , minScale(0.25)
    {}
};

class PlaybackQualityPolicy {
public:
    explicit PlaybackQualityPolicy(PlaybackQualityConfig cfg = PlaybackQualityConfig());

    PlaybackQualityDecision decide(const PlaybackMetrics& m);
    void reset();

private:
    PlaybackQualityConfig m_cfg;
    double m_scaleFactor; // internal state: current scale (1.0, 0.5, or minScale)

    // Step array for hysteresis ladder: 1.0 -> 0.5 -> minScale
    // Returns next step down / up from current value
    double stepDown(double current) const;
    double stepUp(double current) const;
};

} // namespace playback
