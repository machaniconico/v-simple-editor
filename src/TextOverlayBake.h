#pragma once

#include <QImage>
#include <QVector>

struct EnhancedTextOverlay;   // TextManager.h
class QThread;

// Genuine, thread-safe text-overlay baker (extracted verbatim from the
// authoritative preview path VideoPlayer::composeFrameWithOverlays).
//
// WHY THIS EXISTS: VideoPlayer is a QWidget. The SSOT renderer
// tlrender::renderFrameAt runs on a RenderQueue WORKER THREAD
// (RenderQueue::startRenderPipe), so it must never construct a VideoPlayer
// to bake text — constructing a QWidget off the GUI thread is Qt undefined
// behaviour (asserts/crashes on Qt6/MSVC). The text-baking body is pure
// QPainter-on-QImage + the overlay list + the playhead time + a font scale,
// with NO QWidget state, so it is hoisted here as a free function.
//
// SINGLE SOURCE OF TRUTH: VideoPlayer::composeFrameWithOverlays delegates to
// this function for the actual baking (computing fontScale from its own
// widget/letterbox first, exactly as before — the preview is unchanged), and
// the SSOT renderer calls this function DIRECTLY (fontScale = 1.0, the value
// the headless/export path already produced because m_glPreview was null).
// Both paths therefore run the IDENTICAL baking code, so the S6 export-vs-
// preview text parity holds by construction.
namespace textbake {

// Bake `overlays` onto `source` at playhead `nowSec` (timeline seconds),
// skipping overlay index `hiddenIdx` (-1 = none). `fontScale` inverse-scales
// glyph point size / outline width to keep WYSIWYG when the preview later
// letterbox-scales the baked image (the preview passes H/letterboxH; the
// headless/export path passes 1.0). Returns the composited frame; an empty
// overlay list returns `source` unchanged (byte-identical no-op).
QImage bakeOverlays(const QImage &source,
                    const QVector<EnhancedTextOverlay> &overlays,
                    double nowSec,
                    int hiddenIdx,
                    double fontScale);

// Test-only observability seam (dormant in production). When the env var
// VEDITOR_TEXTEXPORT_SELFTEST is set, every bakeOverlays() call that actually
// composites text records the QThread it ran on, so the worker-thread text-
// export selftest can prove renderFrameAt's text stage executed OFF the GUI
// thread (the original defect would have constructed a QWidget there). Reads
// the most recent recorded thread; nullptr if bakeOverlays never baked text.
// Lives here (not in the test) so it observes the GENUINE production code
// path, including the RenderQueue worker thread, with zero prod overhead
// unless the env var is set.
QThread *lastBakeThreadForTest();
void resetLastBakeThreadForTest();

} // namespace textbake
