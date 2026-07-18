#include "PlaybackQualityPolicy.h"

namespace playback {

PlaybackQualityPolicy::PlaybackQualityPolicy(PlaybackQualityConfig cfg)
    : m_cfg(cfg)
    , m_scaleFactor(1.0)
{
}

void PlaybackQualityPolicy::reset()
{
    m_scaleFactor = 1.0;
}

double PlaybackQualityPolicy::stepDown(double current) const
{
    // Ladder: 1.0 -> 0.5 -> minScale
    if (current > 0.5) {
        return 0.5;
    }
    if (current > m_cfg.minScale) {
        return m_cfg.minScale;
    }
    return m_cfg.minScale; // already at floor
}

double PlaybackQualityPolicy::stepUp(double current) const
{
    // Ladder: minScale -> 0.5 -> 1.0
    if (current < 0.5) {
        return 0.5;
    }
    if (current < 1.0) {
        return 1.0;
    }
    return 1.0; // already at ceiling
}

PlaybackQualityDecision PlaybackQualityPolicy::decide(const PlaybackMetrics& m)
{
    // Stopped: always full quality, reset internal state
    if (!m.isPlaying) {
        reset();
        return PlaybackQualityDecision{ 1.0, false, false };
    }

    // Determine render performance relative to budget
    const double slow    = m.targetFrameMs * m_cfg.slowFactor;
    const double recover = m.targetFrameMs * m_cfg.recoverFactor;

    if (m.lastFrameRenderMs > slow) {
        // Too slow: step down one rung on the quality ladder
        m_scaleFactor = stepDown(m_scaleFactor);
    } else if (m.lastFrameRenderMs < recover) {
        // Fast enough: step up one rung (hysteresis: must be clearly faster)
        m_scaleFactor = stepUp(m_scaleFactor);
    }
    // In the recover..slow band: hold current scale (no oscillation)

    // useProxy: proxy available AND (many tracks OR already degraded)
    const bool useProxy = m.proxyAvailable &&
                          (m.trackCount >= m_cfg.proxyTrackThreshold || m_scaleFactor < 1.0);

    // allowFrameDrop: pinned at minimum scale AND still rendering too slowly
    const bool allowFrameDrop = (m_scaleFactor <= m_cfg.minScale) &&
                                (m.lastFrameRenderMs > slow);

    return PlaybackQualityDecision{ m_scaleFactor, useProxy, allowFrameDrop };
}

} // namespace playback
