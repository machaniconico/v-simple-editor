#pragma once

#include <QImage>
#include <QSize>
#include <QVector>

#include <memory>

#include "GpuCompositeMath.h"

// GpuLayerCompositor: Stage 2 of the GPU multi-track compositor.
//
// Purpose
// -------
// Takes the PURE compositing-math rules proven headless in Stage 1
// (gpucomposite::, see GpuCompositeMath.h) and actually executes them on the
// GPU: it uploads N decoded video layers as textures, draws them into an
// offscreen FBO with premultiplied "source over" blending in the canonical
// paint order, and reads the result back as a single QImage.
//
// This is PREVIEW-ONLY. The export path keeps the CPU pipeline as the single
// source of truth (SSOT). The goal of this class is to produce, for matte-FREE
// scenes, a frame that is pixel-equivalent to VideoPlayer's CPU
// composeMultiTrackFrame*(), so a future wiring step can swap GPU in for live
// preview without changing visible output.
//
// Self-test only for now: nothing in the app calls composite() yet. A headless
// self-test (which must create a QGuiApplication so an OpenGL context can be
// made current) is the only caller.
//
// Matte support status
// --------------------
// Stage 2's REQUIRED guarantee is GPU/CPU parity for scenes WITHOUT track
// mattes (LayerDesc::matteType == None / matteSourceIndex <= 0). Layers that
// request a matte (matteSourceIndex > 0 && gpucomposite::isValidMatteSource)
// are, in this stage, drawn WITHOUT applying the matte (i.e. as plain layers).
// Full GPU matte (alpha / luminance / inverted, sampled from the matte source
// texture in the fragment shader) is deferred to the next stage. Callers that
// need matte fidelity today must stay on the CPU path.
//
// Threading / lifetime
// ---------------------
//   * Does NOT create a QApplication; the caller must already have a
//     QGuiApplication (or QApplication) so a QOpenGLContext can be created.
//   * Owns its own QOpenGLContext + QOffscreenSurface (lazily created on first
//     composite()). All GL work happens with that context made current on the
//     calling thread; construct and call composite() on the same thread.
//   * If a GL context cannot be created (headless box, no GL), isAvailable()
//     returns false and composite() returns a null QImage so the self-test can
//     SKIP rather than fail.
//
// Single-thread / non-reentrant contract
// ---------------------------------------
// SAME-THREAD ONLY and NON-REENTRANT. The instance must be constructed, used
// (isAvailable / composite) and destroyed on ONE thread (Stage 3 wiring calls
// it from the UI thread). The persistent GL resources below (shader program,
// FBO, vertex buffers, the layer texture pool) are reused across composite()
// calls to keep live preview cheap; they are NOT guarded by any lock, so
// concurrent or re-entrant calls are undefined behaviour. Each composite()
// makeCurrent()s the owned context, reuses/refreshes resources, then releases
// it and restores any context that was current before the call.
class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLShaderProgram;
class QOpenGLFramebufferObject;
class QOpenGLBuffer;
class QOpenGLTexture;

// One decoded layer fed to the GPU compositor.
struct GpuLayerInput {
    QImage                  image;   // RGBA frame (RGBA8888 / ARGB32 family). Null/empty == skip.
    gpucomposite::LayerDesc desc;    // placement + opacity + (ignored-this-stage) matte info
};

class GpuLayerCompositor {
public:
    GpuLayerCompositor();
    ~GpuLayerCompositor();

    GpuLayerCompositor(const GpuLayerCompositor&) = delete;
    GpuLayerCompositor& operator=(const GpuLayerCompositor&) = delete;

    // True if an OpenGL context was (or can be) created successfully. Triggers
    // lazy initialization on first call.
    bool isAvailable();

    // Composite the given layers into a `canvas`-sized image, in the canonical
    // gpucomposite::paintOrder, using premultiplied source-over blending.
    // Returns an ARGB32_Premultiplied QImage matching the CPU SSOT format.
    // Returns a null QImage if GL is unavailable or canvas is empty.
    QImage composite(const QVector<GpuLayerInput>& layers, QSize canvas);

private:
    bool ensureContext();    // lazy GL bring-up; sets m_triedInit / m_available
    bool ensureProgram();    // compile/link shader once; reused thereafter
    bool ensureFbo(QSize canvas);  // create/keep FBO matching canvas size
    void releaseGlResources();     // free all GL objects (context must be current)

    QOpenGLContext*    m_ctx     = nullptr;
    QOffscreenSurface* m_surface = nullptr;
    bool               m_triedInit = false;
    bool               m_available = false;

    // Persistent, reused GL resources (created lazily under a current context).
    std::unique_ptr<QOpenGLShaderProgram>        m_prog;   // compiled once
    std::unique_ptr<QOpenGLFramebufferObject>    m_fbo;    // re-made only on size change
    std::unique_ptr<QOpenGLBuffer>               m_vbo;    // interleaved srcPos+texCoord quad
    QVector<QOpenGLTexture*>                      m_texPool; // per-layer texture pool, reused

    // Cached uniform/attribute locations (valid for the life of m_prog).
    int m_uLayer    = -1;
    int m_uProj     = -1;
    int m_uTex      = -1;
    int m_uOpacity  = -1;
    int m_aSrcPos   = -1;
    int m_aTexCoord = -1;
};
