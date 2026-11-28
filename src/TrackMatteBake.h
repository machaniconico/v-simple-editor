#pragma once

#include <QImage>
#include <QSize>
#include <QVector>

#include "LayerCompositor.h"

namespace trackmatte {

// AE/Premiere track-matte semantics. `layers` is the FULL z-ordered layer
// list; `layerImages[i]` is layers[i]'s already-transformed RGBA image sized
// to canvasSize (parallel arrays, equal length). Returns the final
// composited canvas. A layer with matteType != TrackMatteType::None consumes
// the layer at matteSourceLayerIndex as its matte via
// MaskSystem::applyTrackMatte, and that matte-source layer is NOT drawn
// standalone. Out-of-range / self-referential / null-image matteSourceLayerIndex
// => that layer composites normally (no matte applied, never crashes).
// Layers are stacked with premultiplied SourceOver at their opacity. A visible
// layer with a blendMode other than Normal (and not a matte source) is left out
// of that stack and then blended over the result in ascending order through
// LayerCompositor::blendImages with its blendMode+opacity, after its own matte
// (if any) has cut its alpha. With no such layer the stack is the whole result.
QImage composite(const QVector<CompositeLayer>& layers,
                 const QVector<QImage>& layerImages,
                 QSize canvasSize);

// Selftest hook mirroring textbake: when env VEDITOR_TRACKMATTE_SELFTEST is
// set, composite() records whether it applied >=1 non-None matte this process.
bool selftestAppliedMatte();
void selftestReset();

} // namespace trackmatte
