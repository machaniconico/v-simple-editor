#include "LiveMatteResolve.h"

#include <QHash>

namespace clipstack {

void resolveLiveMatteSources(
    int n,
    const std::function<QString(int)>& clipIdOf,
    const std::function<QString(int)>& matteSrcClipIdOf,
    const std::function<bool(int)>& hasMatteOf,
    const std::function<void(int idx, int srcIdx)>& setMatteSrcIndex)
{
    if (n <= 0)
        return;

    // 1. clipId -> index map over [0, n). Mirrors export's indexByClipId.
    QHash<QString, int> indexByClipId;
    indexByClipId.reserve(n);
    for (int i = 0; i < n; ++i)
        indexByClipId.insert(clipIdOf(i), i);

    // 2. Resolve each matte'd layer's source index.
    for (int i = 0; i < n; ++i) {
        if (!hasMatteOf(i))
            continue;
        const QString srcClipId = matteSrcClipIdOf(i);
        const int srcIdx = indexByClipId.value(srcClipId, -1);
        // Same guard as TimelineFrameRenderer.cpp:810 — reject base (idx 0),
        // self-reference, and unknown (-1). Leave the layer matte-free (-1)
        // so it composites normally rather than blanking against the base.
        if (srcIdx > 0 && srcIdx != i)
            setMatteSrcIndex(i, srcIdx);
    }
}

} // namespace clipstack
