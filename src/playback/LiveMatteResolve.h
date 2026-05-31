#pragma once

#include <QString>
#include <functional>

// clipstack::resolveLiveMatteSources — PURE, headless (no QApplication, no
// QWidget, no QImage, no GL) resolver that maps each live layer's
// matteSourceClipId to an INDEX into the layers vector, mirroring the proven
// export-path resolution in TimelineFrameRenderer.cpp:792-814 byte-for-byte.
//
// STAGE4B: unit-tested via the `live-matte-resolve` selftest AND wired into
// VideoPlayer::tryGpuComposeLayers. The caller MUST present layers in the
// export index convention (V1 at index 0, ascending sourceTrack) so the
// srcIdx>0 "index 0 == V1 base" rule holds; tryGpuComposeLayers reorders the
// paintOrder-sorted layers V1-first before calling this (C1 fix). The resolved
// indices point into the EXACT vector composite() iterates (index-space safety).
//
// Resolution rule (identical to export):
//   1. Build clipId -> index map over [0, n).
//   2. For each layer i that HAS a matte (matteType != None):
//        srcIdx = map.value(matteSourceClipId, -1)
//        if srcIdx > 0 && srcIdx != i  -> assign srcIdx (valid matte source)
//        else                          -> leave -1 (composite normally)
//      Rejections (all leave -1):
//        - srcIdx == 0  : layer 0 is the V1 base; never a matte source.
//        - srcIdx == i  : self-reference.
//        - srcIdx == -1 : unknown / unresolved clipId.
//   3. Layers without a matte are left untouched (caller's default -1).
//
// Callbacks are used (rather than concrete container types) so the same
// resolver drives both the export-style CompositeLayer vector and the live
// DecodedLayer vector without either type leaking into this header.
namespace clipstack {

void resolveLiveMatteSources(
    int n,
    const std::function<QString(int)>& clipIdOf,         // clipId of layer i
    const std::function<QString(int)>& matteSrcClipIdOf, // matteSourceClipId ("" == none)
    const std::function<bool(int)>& hasMatteOf,          // matteType != None for layer i
    const std::function<void(int idx, int srcIdx)>& setMatteSrcIndex);

} // namespace clipstack
