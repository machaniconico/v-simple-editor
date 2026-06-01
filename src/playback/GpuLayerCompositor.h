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
// Track mattes ARE composited on the GPU (Stage 4A). A layer L whose
// matteType != None and whose desc.matteSourceIndex passes
// gpucomposite::isValidMatteSource(matteSourceIndex, L, layers.size()) consumes
// the layer at that index as its matte; that matte-source layer is EXCLUDED
// from being drawn standalone (mirroring trackmatte::composite). The matte is
// applied with a TWO-PASS GPU path:
//   1. render the matte source into a canvas-sized temp FBO (premultiplied,
//      its layerTransform, opacity=1, transparent clear) -> matteTex;
//   2. render L into a canvas-sized temp FBO the same way -> srcTex;
//   3. one full-canvas pass into the MAIN FBO with a matte fragment shader that
//      samples srcTex and matteTex at the SAME canvas coord, derives the mask
//      value (alpha straight from matteTex.a; LUMA from STRAIGHT-alpha Rec.601
//      = 0.299*sr+0.587*sg+0.114*sb after un-premultiplying matteTex.rgb),
//      multiplies srcTex by mask then by opacity, and emits the premultiplied
//      result so the main FBO source-over blend places it in L's normal slot.
// This is measured against the CPU SSOT MaskSystem::applyTrackMatte + applyMask
// in gpu-composite-parity G9..G16. Alpha masks are direct alpha sampling; luma
// masks use continuous shader floats rather than GpuCompositeMath's integer
// reference-oracle helper, so float-vs-integer truncation slack is expected.
// A layer whose matteType != None but whose matte source is INVALID composites
// normally (no matte), exactly like trackmatte::composite.
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
    gpucomposite::LayerDesc desc;    // placement + opacity + matte info (matteSourceIndex / matteType)
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

    // Matte-FREE 16-bit composite (Stage 4C HDR-2). Renders into an RGBA16
    // offscreen FBO and reads back 16-bit per channel via
    // glReadPixels(GL_RGBA, GL_UNSIGNED_SHORT). Returns
    // QImage::Format_RGBA64_Premultiplied, or a NULL QImage if GL or the RGBA16
    // FBO is unavailable (caller SKIPs). MATTE-FREE: matteType is ignored, no
    // matte-source exclusion — every isLayerComposited layer is painted plainly
    // in gpucomposite::paintOrder, mirroring HdrCompositeMath::compositeReference16.
    // This is a SECOND, independent path; the 8-bit composite() above is unchanged.
    QImage composite16(const QVector<GpuLayerInput>& layers, QSize canvas);

private:
    bool ensureContext();    // lazy GL bring-up; sets m_triedInit / m_available
    bool ensureProgram();    // compile/link plain layer shader once; reused thereafter
    bool ensureMatteProgram(); // compile/link the two-pass matte shader once; reused
    bool ensureFbo(QSize canvas);  // create/keep MAIN FBO matching canvas size
    bool ensureFbo16(QSize canvas);// create/keep MAIN RGBA16 FBO matching canvas size
    void releaseGlResources();     // free all GL objects (context must be current)

    // Render ONE layer (its layerTransform, opacity=1, premultiplied) into the
    // given canvas-sized temp FBO over a transparent clear. Used to produce the
    // matte-source and matte'd-source textures for the two-pass matte path.
    // Returns false on any GL failure. Reuses the plain layer program + VBO.
    bool renderLayerToFbo(QOpenGLFramebufferObject& target,
                          const QImage& img,
                          const gpucomposite::LayerDesc& d,
                          QSize canvas,
                          const QMatrix4x4& proj);

    QOpenGLContext*    m_ctx     = nullptr;
    QOffscreenSurface* m_surface = nullptr;
    bool               m_triedInit = false;
    bool               m_available = false;

    // Persistent, reused GL resources (created lazily under a current context).
    std::unique_ptr<QOpenGLShaderProgram>        m_prog;   // plain layer shader, compiled once
    std::unique_ptr<QOpenGLShaderProgram>        m_matteProg; // matte combine shader, compiled once
    std::unique_ptr<QOpenGLFramebufferObject>    m_fbo;    // MAIN FBO; re-made only on size change
    std::unique_ptr<QOpenGLFramebufferObject>    m_fbo16;  // MAIN RGBA16 FBO (composite16); re-made only on size change
    std::unique_ptr<QOpenGLFramebufferObject>    m_matteFboSrc;   // temp: matte'd source render
    std::unique_ptr<QOpenGLFramebufferObject>    m_matteFboMatte; // temp: matte source render
    std::unique_ptr<QOpenGLBuffer>               m_vbo;    // interleaved srcPos+texCoord quad
    std::unique_ptr<QOpenGLBuffer>               m_matteVbo; // full-canvas quad for matte combine
    QVector<QOpenGLTexture*>                      m_texPool; // per-layer texture pool, reused (8-bit)
    QVector<QOpenGLTexture*>                      m_texPool16; // per-layer texture pool, reused (16-bit composite16)

    // Cached uniform/attribute locations (valid for the life of m_prog).
    int m_uLayer    = -1;
    int m_uProj     = -1;
    int m_uTex      = -1;
    int m_uOpacity  = -1;
    int m_aSrcPos   = -1;
    int m_aTexCoord = -1;

    // Cached locations for the matte combine program (life of m_matteProg).
    int m_mSrcTex     = -1;
    int m_mMatteTex   = -1;
    int m_mMatteType  = -1;
    int m_mOpacity    = -1;
    int m_mAPos       = -1;  // NDC position attribute (full-canvas quad)
    int m_mATexCoord  = -1;  // 0..1 texcoord attribute
};
