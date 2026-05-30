#include "AutoProxyPolicy.h"

namespace playback {

AutoProxyPlan AutoProxyPolicy::decide(const QVector<AutoProxyClip>& clips,
                                      const AutoProxyConfig&        cfg)
{
    AutoProxyPlan plan;

    if (!cfg.enabled) {
        return plan;
    }

    int trackCount = 0;
    for (const AutoProxyClip& clip : clips) {
        if (!clip.filePath.isEmpty())
            ++trackCount;
    }
    if (trackCount < cfg.trackThreshold) {
        return plan;
    }

    for (const AutoProxyClip& clip : clips) {
        if (clip.filePath.isEmpty()) {
            continue;
        }

        // Determine whether this clip is "heavy"
        const bool widthHeavy = (clip.width >= cfg.heavyWidthThreshold);
        const bool heightHeavy = (clip.height >= cfg.heavyWidthThreshold);

        const QString codecNorm = clip.codec.trimmed().toLower();
        bool codecHeavy = false;
        if (!codecNorm.isEmpty()) {
            for (const QString &heavyCodec : cfg.heavyCodecs) {
                if (codecNorm == heavyCodec.trimmed().toLower()) {
                    codecHeavy = true;
                    break;
                }
            }
        }

        const bool isHeavy = widthHeavy || heightHeavy || codecHeavy;

        if (!isHeavy) {
            continue;
        }

        if (clip.proxyReady) {
            // Add to useProxyFor if not already present (preserve input order)
            if (!plan.useProxyFor.contains(clip.filePath)) {
                plan.useProxyFor.append(clip.filePath);
            }
        } else {
            // Add to generateFor if not already present (preserve input order)
            if (!plan.generateFor.contains(clip.filePath)) {
                plan.generateFor.append(clip.filePath);
            }
        }
    }

    plan.anyProxyActive = !plan.useProxyFor.isEmpty();

    return plan;
}

} // namespace playback
