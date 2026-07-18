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
// Blending uses LayerCompositor::blendImages with each layer's blendMode+opacity.
QImage composite(const QVector<CompositeLayer>& layers,
                 const QVector<QImage>& layerImages,
                 QSize canvasSize);

// Selftest hook mirroring textbake: when env VEDITOR_TRACKMATTE_SELFTEST is
// set, composite() records whether it applied >=1 non-None matte this process.
bool selftestAppliedMatte();
void selftestReset();

} // namespace trackmatte
