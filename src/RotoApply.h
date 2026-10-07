#pragma once

#include "RotoClipEntry.h"
#include <QImage>
#include <QtGlobal>

// Pure roto application shared by the special-composite preview
// (MainWindow::buildSpecialClipComposite). Mask polarity: Rotoscope paints the
// path interior 255 and applyToFrame multiplies every premultiplied channel by
// mask / 255, so the interior is kept, the exterior becomes alpha 0 (RGB 0) and
// a feathered edge carries intermediate alpha.
namespace roto {

// Keyframes take priority over the single path; a non-null brushMask is
// max-combined into the path mask. An entry with no mask data or a null frame
// returns the frame unchanged.
QImage applyEntryToFrame(const RotoClipEntry &entry, const QImage &frame,
                         int sourceFrameIndex);

// Per-pixel max of two masks as Format_Grayscale8; b is scaled to a's size.
QImage combineMasksMax(const QImage &a, const QImage &b);

// Source frame number for a source-second time: max(0, llround(sourceSec * fps)).
int sourceFrameIndexFor(double sourceSec, double fps);

// Probed stream fps when known, otherwise max(1, fallbackFps).
double clipEffectiveSourceFps(double probedFps, double fallbackFps);

// Test counter: number of sourceFrameIndexFor calls in this process.
quint64 sourceFrameIndexForCallCount();

} // namespace roto
