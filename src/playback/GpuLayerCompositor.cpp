#include "GpuLayerCompositor.h"

#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QSurface>
#include <QSurfaceFormat>
#include <QMatrix3x3>
#include <QMatrix4x4>
#include <QVector>
#include <QImage>
#include <QSet>

#include "AcesColor.h"
#include "color/ClipColor.h"
#include "color/ClipColorTransform.h"

#include <cstring>   // std::memcpy (16-bit readback row copy in composite16)

// ---------------------------------------------------------------------------
// Shaders
//
// We feed the vertex shader native-source quad coordinates (0,0)..(srcW,srcH).
// The CPU SSOT transform gpucomposite::layerTransform(desc, canvas) maps those
// native-pixel coordinates into CANVAS-PIXEL space (top-left origin, Y down,
// anchor = canvas/2 + normalized offset). We then apply an orthographic
// projection from canvas-pixel space to NDC. Building that ortho with
// top==0 / bottom==canvasH gives a Y-DOWN clip space that matches the CPU's
// top-left pixel convention; GL still stores framebuffer row 0 at the bottom,
// so the default toImage() (flipped=true) restores top-left orientation on
// read-back (see composite()).
// ---------------------------------------------------------------------------
static const char* kVertSrc = R"(
#version 120
attribute vec2 aSrcPos;     // native source-pixel coords (0..srcW, 0..srcH)
attribute vec2 aTexCoord;   // 0..1 texture coords
uniform mat4 uLayer;        // src-native -> canvas-pixel (gpucomposite::layerTransform)
uniform mat4 uProj;         // canvas-pixel -> NDC (Y-down ortho)
varying vec2 vTexCoord;
void main() {
    vec4 canvasPos = uLayer * vec4(aSrcPos, 0.0, 1.0);
    gl_Position = uProj * vec4(canvasPos.xy, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static const char* kFragSrc = R"(
#version 120
uniform sampler2D uTex;
uniform float uOpacity;     // clampOpacity(desc.opacity)
varying vec2 vTexCoord;
void main() {
    // Texture is uploaded PREMULTIPLIED (ARGB32_Premultiplied). For premultiplied
    // source-over, opacity scales ALL components (rgb already carry alpha).
    vec4 c = texture2D(uTex, vTexCoord);
    gl_FragColor = c * uOpacity;
}
)";

static const char* kIdtFragSrc = R"(#version 120
uniform sampler2D uTex;
uniform float uOpacity;
uniform mat3  uConvMatrix;
uniform int   uPassthrough;
uniform int   uApplyEotf;
uniform int   uApplyOetf;
varying vec2 vTexCoord;
float srgbEotf(float e){ return (e <= 0.04045) ? (e/12.92) : pow((e+0.055)/1.055, 2.4); }
float srgbOetf(float l){ l = max(l, 0.0); return (l <= 0.0031308) ? (12.92*l) : (1.055*pow(l, 1.0/2.4) - 0.055); }
void main(){
    vec4 c = texture2D(uTex, vTexCoord);
    float a = c.a;
    if (a <= 0.0) { gl_FragColor = vec4(0.0); return; }
    if (uPassthrough == 1) { gl_FragColor = c * uOpacity; return; }
    vec3 straight = c.rgb / a;
    vec3 lin = (uApplyEotf == 1) ? vec3(srgbEotf(straight.r), srgbEotf(straight.g), srgbEotf(straight.b)) : straight;
    vec3 outLin = uConvMatrix * lin;
    vec3 outRgb;
    if (uApplyOetf == 1) outRgb = vec3(srgbOetf(outLin.r), srgbOetf(outLin.g), srgbOetf(outLin.b));
    else outRgb = max(outLin, vec3(0.0));
    outRgb = clamp(outRgb, 0.0, 1.0);
    gl_FragColor = vec4(outRgb * a, a) * uOpacity;
}
)";

// ---------------------------------------------------------------------------
// Matte combine shaders (Stage 4A two-pass track matte).
//
// A full-canvas quad whose NDC positions cover [-1,1]^2 and whose texcoords are
// 0..1 across the canvas. Both srcTex (the matte'd layer, premultiplied) and
// matteTex (the matte source, premultiplied) are canvas-sized renders sampled
// at the SAME canvas coordinate, so no transform is needed here — texcoord IS
// the shared canvas uv. (Both temp FBOs are produced by the same Y-down ortho
// path as the main FBO, so their rows align; the single read-back flip on the
// main FBO restores orientation for the final image.)
// ---------------------------------------------------------------------------
static const char* kMatteVertSrc = R"(
#version 120
attribute vec2 aPos;        // NDC position (-1..1)
attribute vec2 aTexCoord;   // 0..1 canvas uv
varying vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

// uMatteType ordinals MIRROR gpucomposite::MatteType / TrackMatteType:
//   0 None, 1 Alpha, 2 AlphaInverted, 3 Luma, 4 LumaInverted.
static const char* kMatteFragSrc = R"(
#version 120
uniform sampler2D uSrcTex;     // matte'd layer, PREMULTIPLIED, canvas-sized
uniform sampler2D uMatteTex;   // matte source, PREMULTIPLIED, canvas-sized
uniform int   uMatteType;
uniform float uOpacity;        // clampOpacity(desc.opacity) of the matte'd layer
varying vec2 vTexCoord;
void main() {
    vec4 src   = texture2D(uSrcTex,   vTexCoord);   // premultiplied
    vec4 matte = texture2D(uMatteTex, vTexCoord);   // premultiplied

    // Premultiplied alpha == straight alpha numerically.
    float a = matte.a;

    // Luma uses STRAIGHT-alpha Rec.601 (CPU un-premultiplies into ARGB32 first).
    // Un-premultiply matteTex.rgb; guard a==0 (CPU's qRed/qGreen/qBlue on a
    // fully-transparent premultiplied pixel are 0, matching straight=vec3(0)).
    vec3 straight = (a > 0.0) ? (matte.rgb / a) : vec3(0.0);
    float luma = 0.299 * straight.r + 0.587 * straight.g + 0.114 * straight.b;

    float maskVal = 1.0;
    if      (uMatteType == 1) maskVal = a;          // Alpha
    else if (uMatteType == 2) maskVal = 1.0 - a;    // AlphaInverted
    else if (uMatteType == 3) maskVal = luma;       // Luma
    else if (uMatteType == 4) maskVal = 1.0 - luma; // LumaInverted
    // uMatteType == 0 (None) -> maskVal stays 1.0 (caller never uses this path).

    maskVal = clamp(maskVal, 0.0, 1.0);

    // applyMask scales every premultiplied component by maskVal, then the layer's
    // own opacity scales all components again (premultiplied source-over slot).
    gl_FragColor = src * maskVal * uOpacity;
}
)";

namespace {

struct CurrentContextGuard {
    QOpenGLContext* previousCtx = nullptr;
    QSurface*       previousSurface = nullptr;
    QOpenGLContext* targetCtx = nullptr;
    bool            targetWasMadeCurrent = false;

    explicit CurrentContextGuard(QOpenGLContext* target)
        : previousCtx(QOpenGLContext::currentContext())
        , previousSurface(previousCtx ? previousCtx->surface() : nullptr)
        , targetCtx(target)
    {
    }

    bool makeCurrent(QSurface* surface) {
        if (!targetCtx || !surface)
            return false;
        if (!targetCtx->makeCurrent(surface))
            return false;
        targetWasMadeCurrent = true;
        return true;
    }

    ~CurrentContextGuard() {
        if (targetWasMadeCurrent && QOpenGLContext::currentContext() == targetCtx)
            targetCtx->doneCurrent();
        if (previousCtx && previousCtx != targetCtx && previousSurface)
            previousCtx->makeCurrent(previousSurface);
    }
};

} // namespace

GpuLayerCompositor::GpuLayerCompositor() = default;

GpuLayerCompositor::~GpuLayerCompositor() {
    // Tear down GL objects with the context current, then destroy the context.
    // (Stage 2's Codex fix: GL resources must be released while the context is
    // current, NOT after doneCurrent(), or destruction leaks / crashes.)
    if (m_ctx && m_surface) {
        CurrentContextGuard current(m_ctx);
        if (current.makeCurrent(m_surface))
            releaseGlResources();
    }
    delete m_surface;
    m_surface = nullptr;
    delete m_ctx;
    m_ctx = nullptr;
}

void GpuLayerCompositor::releaseGlResources() {
    // Caller must hold the owned context CURRENT. Destroys every persistent GL
    // object (textures, VBO, FBO, program) so their GL handles are freed under
    // the right context.
    for (QOpenGLTexture* t : m_texPool) {
        if (t) {
            if (t->isCreated())
                t->destroy();
            delete t;
        }
    }
    m_texPool.clear();

    // 16-bit composite16 texture pool (same ownership rules as m_texPool).
    for (QOpenGLTexture* t : m_texPool16) {
        if (t) {
            if (t->isCreated())
                t->destroy();
            delete t;
        }
    }
    m_texPool16.clear();

    if (m_vbo) {
        if (m_vbo->isCreated())
            m_vbo->destroy();
        m_vbo.reset();
    }
    if (m_matteVbo) {
        if (m_matteVbo->isCreated())
            m_matteVbo->destroy();
        m_matteVbo.reset();
    }
    m_fbo.reset();           // QOpenGLFramebufferObject dtor frees its GL handle
    m_fbo16.reset();         // RGBA16 main FBO (composite16)
    m_matteFboSrc.reset();
    m_matteFboMatte.reset();
    m_matteFboSrc16.reset();
    m_matteFboMatte16.reset();
    m_prog.reset();          // QOpenGLShaderProgram dtor frees the program
    m_idtProg.reset();
    m_matteProg.reset();

    m_uLayer = m_uProj = m_uTex = m_uOpacity = -1;
    m_aSrcPos = m_aTexCoord = -1;
    m_idt_uLayer = m_idt_uProj = m_idt_uTex = m_idt_uOpacity = -1;
    m_idt_uConvMatrix = m_idt_uPassthrough = m_idt_uApplyEotf = -1;
    m_idt_uApplyOetf = -1;
    m_idt_aSrcPos = m_idt_aTexCoord = -1;
    m_mSrcTex = m_mMatteTex = m_mMatteType = m_mOpacity = -1;
    m_mAPos = m_mATexCoord = -1;
}

bool GpuLayerCompositor::ensureContext() {
    if (m_triedInit)
        return m_available;
    m_triedInit = true;

    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(2, 1);
    fmt.setProfile(QSurfaceFormat::NoProfile);
    fmt.setRedBufferSize(8);
    fmt.setGreenBufferSize(8);
    fmt.setBlueBufferSize(8);
    fmt.setAlphaBufferSize(8);

    m_ctx = new QOpenGLContext();
    m_ctx->setFormat(fmt);
    if (!m_ctx->create()) {
        delete m_ctx;
        m_ctx = nullptr;
        m_available = false;
        return false;
    }

    m_surface = new QOffscreenSurface();
    m_surface->setFormat(m_ctx->format());
    m_surface->create();
    if (!m_surface->isValid()) {
        delete m_surface;
        m_surface = nullptr;
        delete m_ctx;
        m_ctx = nullptr;
        m_available = false;
        return false;
    }

    CurrentContextGuard current(m_ctx);
    if (!current.makeCurrent(m_surface)) {
        delete m_surface;
        m_surface = nullptr;
        delete m_ctx;
        m_ctx = nullptr;
        m_available = false;
        return false;
    }
    m_available = true;
    return true;
}

bool GpuLayerCompositor::isAvailable() {
    return ensureContext();
}

bool GpuLayerCompositor::ensureProgram() {
    // Compile/link the shader program ONCE; reuse on every later composite().
    if (m_prog)
        return true;

    auto prog = std::make_unique<QOpenGLShaderProgram>();
    const bool ok =
        prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertSrc) &&
        prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc) &&
        prog->link();
    if (!ok)
        return false;

    // Cache attribute/uniform locations for the program's lifetime.
    m_aSrcPos   = prog->attributeLocation("aSrcPos");
    m_aTexCoord = prog->attributeLocation("aTexCoord");
    m_uLayer    = prog->uniformLocation("uLayer");
    m_uProj     = prog->uniformLocation("uProj");
    m_uTex      = prog->uniformLocation("uTex");
    m_uOpacity  = prog->uniformLocation("uOpacity");

    m_prog = std::move(prog);
    return true;
}

bool GpuLayerCompositor::ensureIdtProgram() {
    // Compile/link the per-fragment IDT program ONCE. It deliberately reuses
    // kVertSrc but has its own fragment shader, program object, and locations.
    if (m_idtProg)
        return true;

    auto prog = std::make_unique<QOpenGLShaderProgram>();
    const bool ok =
        prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertSrc) &&
        prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kIdtFragSrc) &&
        prog->link();
    if (!ok)
        return false;

    m_idt_aSrcPos      = prog->attributeLocation("aSrcPos");
    m_idt_aTexCoord    = prog->attributeLocation("aTexCoord");
    m_idt_uLayer       = prog->uniformLocation("uLayer");
    m_idt_uProj        = prog->uniformLocation("uProj");
    m_idt_uTex         = prog->uniformLocation("uTex");
    m_idt_uOpacity     = prog->uniformLocation("uOpacity");
    m_idt_uConvMatrix  = prog->uniformLocation("uConvMatrix");
    m_idt_uPassthrough = prog->uniformLocation("uPassthrough");
    m_idt_uApplyEotf   = prog->uniformLocation("uApplyEotf");
    m_idt_uApplyOetf   = prog->uniformLocation("uApplyOetf");

    m_idtProg = std::move(prog);
    return true;
}

bool GpuLayerCompositor::ensureMatteProgram() {
    // Compile/link the matte-combine program ONCE; reuse on every later matte'd
    // layer composite.
    if (m_matteProg)
        return true;

    auto prog = std::make_unique<QOpenGLShaderProgram>();
    const bool ok =
        prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kMatteVertSrc) &&
        prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kMatteFragSrc) &&
        prog->link();
    if (!ok)
        return false;

    m_mAPos      = prog->attributeLocation("aPos");
    m_mATexCoord = prog->attributeLocation("aTexCoord");
    m_mSrcTex    = prog->uniformLocation("uSrcTex");
    m_mMatteTex  = prog->uniformLocation("uMatteTex");
    m_mMatteType = prog->uniformLocation("uMatteType");
    m_mOpacity   = prog->uniformLocation("uOpacity");

    m_matteProg = std::move(prog);
    return true;
}

bool GpuLayerCompositor::ensureFbo(QSize canvas) {
    // Reuse the FBO while its size matches the requested canvas; only rebuild
    // when the canvas dimensions actually change (the dominant live-preview
    // case is a fixed canvas, so this allocates exactly once).
    if (m_fbo && m_fbo->size() == canvas && m_fbo->isValid())
        return true;

    m_fbo.reset();

    QOpenGLFramebufferObjectFormat fboFmt;
    fboFmt.setInternalTextureFormat(GL_RGBA8);
    fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);

    auto fbo = std::make_unique<QOpenGLFramebufferObject>(canvas, fboFmt);
    if (!fbo->isValid())
        return false;

    m_fbo = std::move(fbo);
    return true;
}

bool GpuLayerCompositor::ensureFbo16(QSize canvas) {
    // RGBA16 sibling of ensureFbo(), used only by composite16(). Reuse while the
    // size matches; rebuild only on canvas-size change. The internal texture
    // format is GL_RGBA16 (16 bits per channel) instead of GL_RGBA8. If the GL
    // driver cannot give a valid RGBA16 FBO we return false so composite16()
    // gracefully returns a NULL QImage (caller SKIPs) — never a crash or 8-bit
    // fallback.
    if (m_fbo16 && m_fbo16->size() == canvas && m_fbo16->isValid())
        return true;

    m_fbo16.reset();

    QOpenGLFramebufferObjectFormat fboFmt;
    fboFmt.setInternalTextureFormat(GL_RGBA16);
    fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);

    auto fbo = std::make_unique<QOpenGLFramebufferObject>(canvas, fboFmt);
    if (!fbo->isValid())
        return false;

    m_fbo16 = std::move(fbo);
    return true;
}

QOpenGLFramebufferObject* GpuLayerCompositor::ensureTempFbo16(
    std::unique_ptr<QOpenGLFramebufferObject>& slot,
    QSize canvas) {
    // RGBA16 temp FBO for composite16Matte's two-pass matte intermediates.
    // Same format/validity policy as ensureFbo16(): never silently fall back to
    // RGBA8, because that would destroy sub-8-bit matte precision.
    if (slot && slot->size() == canvas && slot->isValid())
        return slot.get();

    slot.reset();

    QOpenGLFramebufferObjectFormat fboFmt;
    fboFmt.setInternalTextureFormat(GL_RGBA16);
    fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);

    auto fbo = std::make_unique<QOpenGLFramebufferObject>(canvas, fboFmt);
    if (!fbo->isValid())
        return nullptr;

    slot = std::move(fbo);
    return slot.get();
}

bool GpuLayerCompositor::renderLayerToFbo(QOpenGLFramebufferObject& target,
                                          const QImage& img,
                                          const gpucomposite::LayerDesc& d,
                                          QSize canvas,
                                          const QMatrix4x4& proj) {
    // Render ONE layer into `target` (canvas-sized) using the plain layer
    // program, the layer's gpucomposite::layerTransform, opacity FORCED to 1,
    // over a transparent clear. This reproduces CPU's canvas-sized intermediate
    // image (renderLayer output) for a matte source / matte'd source.
    if (img.isNull() || img.width() <= 0 || img.height() <= 0)
        return false;
    if (!target.bind())
        return false;

    QOpenGLFunctions* gl = m_ctx->functions();
    gl->glViewport(0, 0, canvas.width(), canvas.height());
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);

    QOpenGLShaderProgram& prog = *m_prog;
    if (!prog.bind()) {
        target.release();
        return false;
    }
    prog.setUniformValue(m_uProj, proj);
    prog.setUniformValue(m_uTex, 0);

    QImage src = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // The temp FBO renders allocate their own throwaway texture rather than
    // consuming the main loop's texture-pool cursor (kept separate so pooling
    // logic stays simple). It is destroyed before returning.
    auto* tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
    tex->setFormat(QOpenGLTexture::RGBA8_UNorm);
    tex->setSize(src.width(), src.height());
    tex->setMinificationFilter(QOpenGLTexture::Nearest);
    tex->setMagnificationFilter(QOpenGLTexture::Nearest);
    tex->setWrapMode(QOpenGLTexture::ClampToEdge);
    tex->allocateStorage();
    tex->setData(QOpenGLTexture::BGRA, QOpenGLTexture::UInt8, src.constBits());

    gl->glActiveTexture(GL_TEXTURE0);
    tex->bind(0);

    prog.setUniformValue(m_uLayer, gpucomposite::layerTransform(d, canvas));
    prog.setUniformValue(m_uOpacity, 1.0f);   // matte intermediates are opacity=1

    const float sw = float(src.width());
    const float sh = float(src.height());
    const float verts[] = {
        0.0f, 0.0f,  0.0f, 0.0f,
        sw,   0.0f,  1.0f, 0.0f,
        0.0f, sh,    0.0f, 1.0f,
        sw,   sh,    1.0f, 1.0f,
    };

    m_vbo->bind();
    m_vbo->allocate(verts, int(sizeof(verts)));
    prog.enableAttributeArray(m_aSrcPos);
    prog.enableAttributeArray(m_aTexCoord);
    prog.setAttributeBuffer(m_aSrcPos,   GL_FLOAT, 0, 2, 4 * sizeof(float));
    prog.setAttributeBuffer(m_aTexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    prog.disableAttributeArray(m_aSrcPos);
    prog.disableAttributeArray(m_aTexCoord);
    m_vbo->release();
    tex->release();
    prog.release();

    tex->destroy();
    delete tex;

    target.release();
    return true;
}

bool GpuLayerCompositor::renderLayerToFbo16(QOpenGLFramebufferObject& target,
                                            const QImage& img,
                                            const gpucomposite::LayerDesc& d,
                                            QSize canvas,
                                            const QMatrix4x4& proj) {
    // RGBA16 sibling of renderLayerToFbo(). Renders exactly one premultiplied
    // layer into a canvas-sized RGBA16 target with opacity forced to 1, preserving
    // 16-bit inputs all the way through the temp matte pass.
    if (img.isNull() || img.width() <= 0 || img.height() <= 0)
        return false;
    if (!m_vbo)
        return false;
    if (!target.bind())
        return false;

    QOpenGLFunctions* gl = m_ctx->functions();
    gl->glViewport(0, 0, canvas.width(), canvas.height());
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);

    QOpenGLShaderProgram& prog = *m_prog;
    if (!prog.bind()) {
        target.release();
        return false;
    }
    prog.setUniformValue(m_uProj, proj);
    prog.setUniformValue(m_uTex, 0);

    QImage src = img.convertToFormat(QImage::Format_RGBA64_Premultiplied);

    auto* tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
    tex->setFormat(QOpenGLTexture::RGBA16_UNorm);
    tex->setSize(src.width(), src.height());
    tex->setMinificationFilter(QOpenGLTexture::Nearest);
    tex->setMagnificationFilter(QOpenGLTexture::Nearest);
    tex->setWrapMode(QOpenGLTexture::ClampToEdge);
    tex->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16);
    if (!tex->isCreated() || !tex->isStorageAllocated()) {
        prog.release();
        tex->destroy();
        delete tex;
        target.release();
        return false;
    }
    tex->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16, src.constBits());

    gl->glActiveTexture(GL_TEXTURE0);
    tex->bind(0);

    prog.setUniformValue(m_uLayer, gpucomposite::layerTransform(d, canvas));
    prog.setUniformValue(m_uOpacity, 1.0f);

    const float sw = float(src.width());
    const float sh = float(src.height());
    const float verts[] = {
        0.0f, 0.0f,  0.0f, 0.0f,
        sw,   0.0f,  1.0f, 0.0f,
        0.0f, sh,    0.0f, 1.0f,
        sw,   sh,    1.0f, 1.0f,
    };

    m_vbo->bind();
    m_vbo->allocate(verts, int(sizeof(verts)));
    prog.enableAttributeArray(m_aSrcPos);
    prog.enableAttributeArray(m_aTexCoord);
    prog.setAttributeBuffer(m_aSrcPos,   GL_FLOAT, 0, 2, 4 * sizeof(float));
    prog.setAttributeBuffer(m_aTexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    prog.disableAttributeArray(m_aSrcPos);
    prog.disableAttributeArray(m_aTexCoord);
    m_vbo->release();
    tex->release();
    prog.release();

    tex->destroy();
    delete tex;

    target.release();
    return true;
}

bool GpuLayerCompositor::renderLayerToFbo16Idt(QOpenGLFramebufferObject& target,
                                               const QImage& img,
                                               const gpucomposite::LayerDesc& d,
                                               const clipcolor::ColorMeta& colorMeta,
                                               aces::ColorSpace outSpace,
                                               QSize canvas,
                                               const QMatrix4x4& proj) {
    // IDT sibling of renderLayerToFbo16(). Used only for matte intermediates:
    // opacity is forced to 1 here, and the matte combine pass applies the real
    // matte'd layer opacity after alpha/luma masking.
    if (img.isNull() || img.width() <= 0 || img.height() <= 0)
        return false;
    if (!m_vbo)
        return false;
    if (!target.bind())
        return false;

    QOpenGLFunctions* gl = m_ctx->functions();
    gl->glViewport(0, 0, canvas.width(), canvas.height());
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);

    QOpenGLShaderProgram& prog = *m_idtProg;
    if (!prog.bind()) {
        target.release();
        return false;
    }
    prog.setUniformValue(m_idt_uProj, proj);
    prog.setUniformValue(m_idt_uTex, 0);

    QImage src = img.convertToFormat(QImage::Format_RGBA64_Premultiplied);

    auto* tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
    tex->setFormat(QOpenGLTexture::RGBA16_UNorm);
    tex->setSize(src.width(), src.height());
    tex->setMinificationFilter(QOpenGLTexture::Nearest);
    tex->setMagnificationFilter(QOpenGLTexture::Nearest);
    tex->setWrapMode(QOpenGLTexture::ClampToEdge);
    tex->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16);
    if (!tex->isCreated() || !tex->isStorageAllocated()) {
        prog.release();
        tex->destroy();
        delete tex;
        target.release();
        return false;
    }
    tex->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16, src.constBits());

    gl->glActiveTexture(GL_TEXTURE0);
    tex->bind(0);

    auto toQMatrix3x3 = [](const aces::Mat3& m) -> QMatrix3x3 {
        // aces::Mat3 is row-major. QMatrix3x3 consumes row-major floats and
        // QOpenGLShaderProgram transposes to the column-major GLSL upload.
        const float values[9] = {
            float(m[0][0]), float(m[0][1]), float(m[0][2]),
            float(m[1][0]), float(m[1][1]), float(m[1][2]),
            float(m[2][0]), float(m[2][1]), float(m[2][2])
        };
        return QMatrix3x3(values);
    };

    const aces::ColorSpace inSpace = clipcolor::acesSpaceFor(colorMeta);
    const bool passthrough =
        colorMeta.transfer == clipcolor::Transfer::PQ
        || colorMeta.transfer == clipcolor::Transfer::HLG
        || inSpace == outSpace;
    const aces::Mat3 conv =
        passthrough ? aces::identity3()
                    : aces::conversionMatrix(inSpace, outSpace);
    const QMatrix3x3 convMatrix = toQMatrix3x3(conv);

    const QMatrix4x4 layerMat = gpucomposite::layerTransform(d, canvas);
    prog.setUniformValue(m_idt_uLayer, layerMat);
    prog.setUniformValue(m_idt_uOpacity, 1.0f);
    prog.setUniformValue(m_idt_uConvMatrix, convMatrix);
    prog.setUniformValue(m_idt_uPassthrough, passthrough ? 1 : 0);
    const bool inputIsLinear =
        colorMeta.transfer == clipcolor::Transfer::Linear
        || aces::isLinearSpace(inSpace);
    prog.setUniformValue(m_idt_uApplyEotf, inputIsLinear ? 0 : 1);
    prog.setUniformValue(m_idt_uApplyOetf,
                         aces::isLinearSpace(outSpace) ? 0 : 1);

    const float sw = float(src.width());
    const float sh = float(src.height());
    const float verts[] = {
        0.0f, 0.0f,  0.0f, 0.0f,
        sw,   0.0f,  1.0f, 0.0f,
        0.0f, sh,    0.0f, 1.0f,
        sw,   sh,    1.0f, 1.0f,
    };

    m_vbo->bind();
    m_vbo->allocate(verts, int(sizeof(verts)));
    prog.enableAttributeArray(m_idt_aSrcPos);
    prog.enableAttributeArray(m_idt_aTexCoord);
    prog.setAttributeBuffer(m_idt_aSrcPos,   GL_FLOAT, 0, 2, 4 * sizeof(float));
    prog.setAttributeBuffer(m_idt_aTexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    prog.disableAttributeArray(m_idt_aSrcPos);
    prog.disableAttributeArray(m_idt_aTexCoord);
    m_vbo->release();
    tex->release();
    prog.release();

    tex->destroy();
    delete tex;

    target.release();
    return true;
}

QImage GpuLayerCompositor::composite(const QVector<GpuLayerInput>& layers, QSize canvas) {
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return QImage();
    if (!ensureContext())
        return QImage();
    CurrentContextGuard current(m_ctx);
    if (!current.makeCurrent(m_surface))
        return QImage();

    QOpenGLFunctions* gl = m_ctx->functions();

    // --- Persistent shader program (compiled once, reused) ---
    if (!ensureProgram())
        return QImage();

    // --- Persistent offscreen FBO (reused while canvas size is unchanged) ---
    if (!ensureFbo(canvas))
        return QImage();
    QOpenGLFramebufferObject& fbo = *m_fbo;
    if (!fbo.bind())
        return QImage();

    gl->glViewport(0, 0, canvas.width(), canvas.height());

    // Transparent clear so empty canvas == fully transparent, matching the CPU
    // SSOT which starts from a transparent ARGB32_Premultiplied frame.
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);

    // Premultiplied source-over: rgb and a both blend with (1 - src.a).
    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);

    // --- Bind the persistent shader program ---
    QOpenGLShaderProgram& prog = *m_prog;
    if (!prog.bind()) {
        fbo.release();
        return QImage();
    }

    // Projection: canvas-pixel (top-left origin, Y down) -> NDC.
    // QMatrix4x4::ortho(left, right, bottom, top, near, far). Passing
    // bottom=canvasH, top=0 yields a Y-down clip space.
    QMatrix4x4 proj;
    proj.ortho(0.0f, float(canvas.width()),
               float(canvas.height()), 0.0f,
               -1.0f, 1.0f);
    prog.setUniformValue(m_uProj, proj);
    prog.setUniformValue(m_uTex, 0);

    const int aSrcPos   = m_aSrcPos;
    const int aTexCoord = m_aTexCoord;

    // Collect LayerDescs for paint-order computation (index-aligned with layers).
    QVector<gpucomposite::LayerDesc> descs;
    descs.reserve(layers.size());
    for (const auto& in : layers)
        descs.push_back(in.desc);

    // Back-to-front: paintOrder is DESCENDING sourceTrack, so V1 (lowest) draws
    // LAST and wins (frontmost) under source-over.
    const QVector<int> order = gpucomposite::paintOrder(descs);

    // Matte sources: a layer i whose matteType != None with a VALID
    // matteSourceIndex (gpucomposite::isValidMatteSource, 3-arg) consumes that
    // index as its matte. Those source layers are EXCLUDED from being painted
    // standalone — identical to trackmatte::composite. matteSourceIndex indexes
    // the ORIGINAL `layers` vector (not paint order).
    QSet<int> matteSourceIndices;
    for (int i = 0; i < descs.size(); ++i) {
        const gpucomposite::LayerDesc& d = descs[i];
        if (d.matteType == gpucomposite::MatteType::None)
            continue;
        if (gpucomposite::isValidMatteSource(d.matteSourceIndex, i, descs.size()))
            matteSourceIndices.insert(d.matteSourceIndex);
    }

    // Persistent VBO for the interleaved (aSrcPos, aTexCoord) quad. Position
    // depends on per-layer src size, so contents are re-uploaded per draw, but
    // the buffer object itself is allocated once and reused.
    if (!m_vbo) {
        m_vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        if (!m_vbo->create()) {
            m_vbo.reset();
            prog.release();
            fbo.release();
            return QImage();
        }
        m_vbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);
    }

    // Lazily (re)create a canvas-sized temp FBO for matte intermediates. Reused
    // across composite() calls; rebuilt only on canvas-size change. Returns
    // nullptr on failure.
    auto ensureTempFbo = [&](std::unique_ptr<QOpenGLFramebufferObject>& slot)
        -> QOpenGLFramebufferObject* {
        if (slot && slot->size() == canvas && slot->isValid())
            return slot.get();
        slot.reset();
        QOpenGLFramebufferObjectFormat fboFmt;
        fboFmt.setInternalTextureFormat(GL_RGBA8);
        fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        auto f = std::make_unique<QOpenGLFramebufferObject>(canvas, fboFmt);
        if (!f->isValid())
            return nullptr;
        slot = std::move(f);
        return slot.get();
    };

    // Draw a full-canvas matte-combine pass into the CURRENTLY-BOUND main FBO,
    // sampling srcTex (matte'd layer) and matteTex (matte source) at the same
    // canvas uv, using the matte program. Assumes the main FBO is already bound
    // with the premultiplied source-over blend state. Returns false on failure.
    auto drawMatteCombine = [&](GLuint srcTexId, GLuint matteTexId,
                                int matteTypeOrdinal, float opacity) -> bool {
        QOpenGLShaderProgram& mp = *m_matteProg;
        if (!mp.bind())
            return false;

        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, srcTexId);
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindTexture(GL_TEXTURE_2D, matteTexId);

        mp.setUniformValue(m_mSrcTex, 0);
        mp.setUniformValue(m_mMatteTex, 1);
        mp.setUniformValue(m_mMatteType, matteTypeOrdinal);
        mp.setUniformValue(m_mOpacity, opacity);

        // Full-canvas quad: NDC corners + matching 0..1 canvas uv. The temp FBOs
        // were rendered with the SAME Y-down ortho as the main FBO, so uv aligns
        // row-for-row with the main FBO's pixels (the single read-back flip later
        // restores screen orientation).
        const float quad[] = {
            // aPos        aTexCoord
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 1.0f,
             1.0f,  1.0f,  1.0f, 1.0f,
        };
        m_matteVbo->bind();
        m_matteVbo->allocate(quad, int(sizeof(quad)));
        mp.enableAttributeArray(m_mAPos);
        mp.enableAttributeArray(m_mATexCoord);
        mp.setAttributeBuffer(m_mAPos,      GL_FLOAT, 0, 2, 4 * sizeof(float));
        mp.setAttributeBuffer(m_mATexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        mp.disableAttributeArray(m_mAPos);
        mp.disableAttributeArray(m_mATexCoord);
        m_matteVbo->release();

        // Leave texture unit 0 active (the plain path assumes GL_TEXTURE0).
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        mp.release();
        return true;
    };

    // Texture-pool cursor: we hand out one pooled texture per drawn layer and
    // reuse its GL storage (glTexSubImage2D) when size/format already match.
    int poolCursor = 0;
    auto acquireTexture = [&](int w, int h) -> QOpenGLTexture* {
        if (poolCursor >= m_texPool.size())
            m_texPool.push_back(nullptr);
        QOpenGLTexture*& slot = m_texPool[poolCursor];
        ++poolCursor;

        // Reuse in place if the existing texture already matches w x h RGBA8.
        if (slot && slot->isCreated() &&
            slot->width() == w && slot->height() == h &&
            slot->format() == QOpenGLTexture::RGBA8_UNorm) {
            return slot;   // storage kept; caller updates via setData (glTexSubImage2D)
        }

        // Size/format mismatch (or first use): (re)allocate this slot's storage.
        if (slot) {
            if (slot->isCreated())
                slot->destroy();
            delete slot;
            slot = nullptr;
        }
        auto* tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
        tex->setFormat(QOpenGLTexture::RGBA8_UNorm);
        tex->setSize(w, h);
        tex->setMinificationFilter(QOpenGLTexture::Nearest);
        tex->setMagnificationFilter(QOpenGLTexture::Nearest);
        tex->setWrapMode(QOpenGLTexture::ClampToEdge);
        tex->allocateStorage();
        slot = tex;
        return slot;
    };

    for (int idx : order) {
        if (idx < 0 || idx >= layers.size())
            continue;
        const GpuLayerInput& in = layers[idx];
        const gpucomposite::LayerDesc& d = in.desc;

        if (!gpucomposite::isLayerComposited(d))
            continue;
        if (in.image.isNull() || in.image.width() <= 0 || in.image.height() <= 0)
            continue;

        // A matte-source layer is consumed by its matte'd layer and never
        // painted standalone (mirrors trackmatte::composite).
        if (matteSourceIndices.contains(idx))
            continue;

        // ----------------------------------------------------------------
        // Matte path: this layer requests a matte AND its source is valid.
        // Render matte source + this layer to canvas-sized temp FBOs, then a
        // full-canvas combine pass into the main FBO. The combine emits the
        // masked + opacity-scaled premultiplied layer, which the main FBO's
        // source-over blend places in this layer's normal paint-order slot.
        // ----------------------------------------------------------------
        const bool wantsMatte =
            d.matteType != gpucomposite::MatteType::None &&
            gpucomposite::isValidMatteSource(d.matteSourceIndex, idx, layers.size());

        if (wantsMatte) {
            const int mIdx = d.matteSourceIndex;
            const GpuLayerInput& matteIn = layers[mIdx];

            QOpenGLFramebufferObject* srcF   = ensureTempFbo(m_matteFboSrc);
            QOpenGLFramebufferObject* matteF = ensureTempFbo(m_matteFboMatte);

            if (m_matteVbo == nullptr) {
                m_matteVbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
                if (m_matteVbo->create())
                    m_matteVbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);
                else
                    m_matteVbo.reset();
            }

            const bool matteReady =
                srcF && matteF && m_matteVbo && ensureMatteProgram() &&
                !matteIn.image.isNull() &&
                matteIn.image.width() > 0 && matteIn.image.height() > 0;

            if (matteReady) {
                // The current plain prog is bound from before the loop; the two
                // temp renders below rebind/release prog internally, and we
                // restore prog + main FBO state afterward.
                prog.release();

                const bool okMatte =
                    renderLayerToFbo(*matteF, matteIn.image, matteIn.desc, canvas, proj);
                const bool okSrc =
                    renderLayerToFbo(*srcF, in.image, d, canvas, proj);

                if (okMatte && okSrc) {
                    // Rebind the MAIN FBO + state for the combine pass.
                    fbo.bind();
                    gl->glViewport(0, 0, canvas.width(), canvas.height());
                    gl->glEnable(GL_BLEND);
                    gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    gl->glDisable(GL_DEPTH_TEST);
                    gl->glDisable(GL_CULL_FACE);

                    drawMatteCombine(srcF->texture(), matteF->texture(),
                                     static_cast<int>(d.matteType),
                                     float(gpucomposite::clampOpacity(d.opacity)));
                }

                // Restore the plain path's expected state for subsequent layers:
                // main FBO bound, plain prog bound, projection/tex sampler set.
                fbo.bind();
                gl->glViewport(0, 0, canvas.width(), canvas.height());
                prog.bind();
                prog.setUniformValue(m_uProj, proj);
                prog.setUniformValue(m_uTex, 0);
                gl->glActiveTexture(GL_TEXTURE0);
                continue;
            }
            // matte not ready -> fall through and composite this layer plainly
            // (matches trackmatte's invalid-matte "composite normally" rule).
        }

        // Upload premultiplied. ARGB32_Premultiplied matches CPU SSOT's frame
        // format and gives correct premultiplied texels for the blend above.
        QImage img = in.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        // Reuse a pooled texture; setData on an already-allocated, matching
        // texture issues glTexSubImage2D (no reallocation), only re-creating
        // storage when the layer's size/format changed.
        QOpenGLTexture* tex = acquireTexture(img.width(), img.height());
        if (!tex)
            continue;
        // ARGB32 in memory is BGRA byte order on little-endian -> upload as BGRA.
        tex->setData(QOpenGLTexture::BGRA, QOpenGLTexture::UInt8, img.constBits());

        gl->glActiveTexture(GL_TEXTURE0);
        tex->bind(0);

        // src-native -> canvas-pixel (canonical CPU SSOT transform).
        const QMatrix4x4 layerMat =
            gpucomposite::layerTransform(d, canvas);
        prog.setUniformValue(m_uLayer, layerMat);
        prog.setUniformValue(m_uOpacity,
                             float(gpucomposite::clampOpacity(d.opacity)));

        const float sw = float(img.width());
        const float sh = float(img.height());

        // Native-source quad (0,0)-(sw,sh). Texture coords use top-left origin
        // (v=0 at y=0) so the image is NOT pre-flipped here; the single flip on
        // read-back below restores screen orientation.
        const float verts[] = {
            //  aSrcPos      aTexCoord
            0.0f, 0.0f,      0.0f, 0.0f,
            sw,   0.0f,      1.0f, 0.0f,
            0.0f, sh,        0.0f, 1.0f,
            sw,   sh,        1.0f, 1.0f,
        };

        m_vbo->bind();
        m_vbo->allocate(verts, int(sizeof(verts)));

        prog.enableAttributeArray(aSrcPos);
        prog.enableAttributeArray(aTexCoord);
        // Attribute pointers reference the bound VBO (offset-based).
        prog.setAttributeBuffer(aSrcPos,   GL_FLOAT, 0,
                                2, 4 * sizeof(float));
        prog.setAttributeBuffer(aTexCoord, GL_FLOAT, 2 * sizeof(float),
                                2, 4 * sizeof(float));

        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        prog.disableAttributeArray(aSrcPos);
        prog.disableAttributeArray(aTexCoord);
        m_vbo->release();
        tex->release();
    }

    prog.release();

    // Read back. Our uProj is a Y-DOWN ortho, so canvas-y=0 (image TOP) maps to
    // NDC +1 and is written to the LAST rows of GL framebuffer memory (GL row 0
    // is the bottom). QOpenGLFramebufferObject::toImage() with the default
    // flipped=true un-flips that GL memory, yielding a top-left-origin
    // ARGB32_Premultiplied QImage — exactly the orientation and format the CPU
    // SSOT composeMultiTrackFrame produces. (Using the default avoids the
    // deprecated QImage::mirrored().)
    QImage out = fbo.toImage();   // flipped=true (default), premultiplied
    fbo.release();

    if (out.isNull())
        return QImage();
    if (out.format() != QImage::Format_ARGB32_Premultiplied)
        out = out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return out;
}

QImage GpuLayerCompositor::composite16(const QVector<GpuLayerInput>& layers, QSize canvas) {
    // 16-bit-per-channel sibling of composite(), restricted to the MATTE-FREE
    // plain path (mirrors HdrCompositeMath::compositeReference16). It reuses the
    // SAME plain-layer shader program (m_prog), the SAME m_vbo, and the SAME
    // Y-down ortho as composite(); only the FBO format (RGBA16), texture format
    // (RGBA16), and the read-back (glReadPixels GL_UNSIGNED_SHORT) differ.
    // Returns a NULL QImage on any GL failure so the caller can SKIP.
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return QImage();
    if (!ensureContext())
        return QImage();
    CurrentContextGuard current(m_ctx);
    if (!current.makeCurrent(m_surface))
        return QImage();

    QOpenGLFunctions* gl = m_ctx->functions();

    // Reuse the persistent plain-layer program (sampler2D + opacity multiply works
    // identically for a 16-bit texture).
    if (!ensureProgram())
        return QImage();

    // RGBA16 main FBO. If the driver cannot provide a valid RGBA16 FBO, skip.
    if (!ensureFbo16(canvas))
        return QImage();
    QOpenGLFramebufferObject& fbo = *m_fbo16;
    if (!fbo.bind())
        return QImage();

    gl->glViewport(0, 0, canvas.width(), canvas.height());

    // Transparent clear (matches the RGBA64 transparent canvas the CPU oracle
    // starts from).
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);

    // Premultiplied source-over, identical to composite().
    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);

    QOpenGLShaderProgram& prog = *m_prog;
    if (!prog.bind()) {
        fbo.release();
        return QImage();
    }

    // SAME Y-down ortho as composite(): canvas-pixel (top-left, Y down) -> NDC.
    QMatrix4x4 proj;
    proj.ortho(0.0f, float(canvas.width()),
               float(canvas.height()), 0.0f,
               -1.0f, 1.0f);
    prog.setUniformValue(m_uProj, proj);
    prog.setUniformValue(m_uTex, 0);

    const int aSrcPos   = m_aSrcPos;
    const int aTexCoord = m_aTexCoord;

    // Paint order (matte-free: NO matte-source exclusion at all).
    QVector<gpucomposite::LayerDesc> descs;
    descs.reserve(layers.size());
    for (const auto& in : layers)
        descs.push_back(in.desc);
    const QVector<int> order = gpucomposite::paintOrder(descs);

    // Persistent VBO (shared with composite()); created lazily here too.
    if (!m_vbo) {
        m_vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        if (!m_vbo->create()) {
            m_vbo.reset();
            prog.release();
            fbo.release();
            return QImage();
        }
        m_vbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);
    }

    // 16-bit texture-pool acquire, analogous to composite()'s acquireTexture but
    // RGBA16_UNorm. Reuses GL storage in place (glTexSubImage2D via setData) when
    // size/format already match.
    int poolCursor = 0;
    auto acquireTexture16 = [&](int w, int h) -> QOpenGLTexture* {
        if (poolCursor >= m_texPool16.size())
            m_texPool16.push_back(nullptr);
        QOpenGLTexture*& slot = m_texPool16[poolCursor];
        ++poolCursor;

        if (slot && slot->isCreated() &&
            slot->width() == w && slot->height() == h &&
            slot->format() == QOpenGLTexture::RGBA16_UNorm) {
            return slot;
        }

        if (slot) {
            if (slot->isCreated())
                slot->destroy();
            delete slot;
            slot = nullptr;
        }
        auto* tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
        tex->setFormat(QOpenGLTexture::RGBA16_UNorm);
        tex->setSize(w, h);
        tex->setMinificationFilter(QOpenGLTexture::Nearest);
        tex->setMagnificationFilter(QOpenGLTexture::Nearest);
        tex->setWrapMode(QOpenGLTexture::ClampToEdge);
        tex->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16);
        if (!tex->isCreated() || !tex->isStorageAllocated()) {
            delete tex;
            return nullptr;
        }
        slot = tex;
        return slot;
    };

    for (int idx : order) {
        if (idx < 0 || idx >= layers.size())
            continue;
        const GpuLayerInput& in = layers[idx];
        const gpucomposite::LayerDesc& d = in.desc;

        if (!gpucomposite::isLayerComposited(d))
            continue;
        if (in.image.isNull() || in.image.width() <= 0 || in.image.height() <= 0)
            continue;

        // MATTE-FREE: matteType is intentionally ignored; no matte-source layer
        // is skipped. Every composited layer is painted plainly.

        // Upload as RGBA16, PREMULTIPLIED. QRgba64 on little-endian stores the
        // four shorts in memory order R,G,B,A, which matches GL's RGBA channel
        // order with QOpenGLTexture::UInt16 — NO swap (unlike the 8-bit BGRA path).
        QImage img = in.image.convertToFormat(QImage::Format_RGBA64_Premultiplied);

        QOpenGLTexture* tex = acquireTexture16(img.width(), img.height());
        if (!tex) {
            prog.release();
            fbo.release();
            return QImage();
        }
        tex->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16, img.constBits());

        gl->glActiveTexture(GL_TEXTURE0);
        tex->bind(0);

        const QMatrix4x4 layerMat = gpucomposite::layerTransform(d, canvas);
        prog.setUniformValue(m_uLayer, layerMat);
        prog.setUniformValue(m_uOpacity,
                             float(gpucomposite::clampOpacity(d.opacity)));

        const float sw = float(img.width());
        const float sh = float(img.height());
        const float verts[] = {
            //  aSrcPos      aTexCoord
            0.0f, 0.0f,      0.0f, 0.0f,
            sw,   0.0f,      1.0f, 0.0f,
            0.0f, sh,        0.0f, 1.0f,
            sw,   sh,        1.0f, 1.0f,
        };

        m_vbo->bind();
        m_vbo->allocate(verts, int(sizeof(verts)));
        prog.enableAttributeArray(aSrcPos);
        prog.enableAttributeArray(aTexCoord);
        prog.setAttributeBuffer(aSrcPos,   GL_FLOAT, 0, 2, 4 * sizeof(float));
        prog.setAttributeBuffer(aTexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        prog.disableAttributeArray(aSrcPos);
        prog.disableAttributeArray(aTexCoord);
        m_vbo->release();
        tex->release();
    }

    prog.release();

    // 16-bit read-back. QOpenGLFramebufferObject::toImage() is 8-BIT ONLY and
    // must NOT be used here — we read the RGBA16 framebuffer directly as
    // GL_UNSIGNED_SHORT into a w*h*4 quint16 buffer.
    const int w = canvas.width();
    const int h = canvas.height();
    QVector<quint16> buf(w * h * 4);
    gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_SHORT, buf.data());

    fbo.release();

    // Build a top-left-origin RGBA64_Premultiplied QImage. GL row 0 is the BOTTOM
    // of the framebuffer, and our Y-down ortho put canvas-y=0 (image TOP) into the
    // LAST GL row, so we flip vertically on copy: dest scanline y comes from GL row
    // (h-1-y). Each RGBA64 scanline is w*4 shorts in R,G,B,A order, matching the
    // glReadPixels GL_RGBA/UInt16 layout exactly — a straight memcpy per row.
    QImage out(canvas, QImage::Format_RGBA64_Premultiplied);
    if (out.isNull())
        return QImage();
    const int rowShorts = w * 4;
    const size_t rowBytes = size_t(rowShorts) * sizeof(quint16);
    for (int y = 0; y < h; ++y) {
        const quint16* srcRow = buf.constData() + size_t(h - 1 - y) * rowShorts;
        std::memcpy(out.scanLine(y), srcRow, rowBytes);
    }
    return out;
}

QImage GpuLayerCompositor::composite16Idt(const QVector<GpuLayerInput>& layers, QSize canvas) {
    // Story1 GPU IDT capability path. Structurally mirrors composite16(): same
    // RGBA16 FBO, texture upload, matte-free paint order, Y-down ortho, blend
    // state, and glReadPixels(GL_UNSIGNED_SHORT) readback. The only rendering
    // difference is the separate IDT shader program and its per-layer uniforms.
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return QImage();
    if (!ensureContext())
        return QImage();
    CurrentContextGuard current(m_ctx);
    if (!current.makeCurrent(m_surface))
        return QImage();

    QOpenGLFunctions* gl = m_ctx->functions();

    if (!ensureIdtProgram())
        return QImage();

    if (!ensureFbo16(canvas))
        return QImage();
    QOpenGLFramebufferObject& fbo = *m_fbo16;
    if (!fbo.bind())
        return QImage();

    gl->glViewport(0, 0, canvas.width(), canvas.height());

    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);

    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);

    QOpenGLShaderProgram& prog = *m_idtProg;
    if (!prog.bind()) {
        fbo.release();
        return QImage();
    }

    QMatrix4x4 proj;
    proj.ortho(0.0f, float(canvas.width()),
               float(canvas.height()), 0.0f,
               -1.0f, 1.0f);
    prog.setUniformValue(m_idt_uProj, proj);
    prog.setUniformValue(m_idt_uTex, 0);

    const int aSrcPos   = m_idt_aSrcPos;
    const int aTexCoord = m_idt_aTexCoord;

    aces::ColorSpace outSpace = aces::ColorSpace::sRGB;
    bool haveOutSpace = false;
    int minSourceTrack = 0;
    for (const GpuLayerInput& in : layers) {
        if (!haveOutSpace || in.desc.sourceTrack < minSourceTrack) {
            haveOutSpace = true;
            minSourceTrack = in.desc.sourceTrack;
            outSpace = clipcolor::acesSpaceFor(in.colorMeta);
        }
    }

    QVector<gpucomposite::LayerDesc> descs;
    descs.reserve(layers.size());
    for (const auto& in : layers)
        descs.push_back(in.desc);
    const QVector<int> order = gpucomposite::paintOrder(descs);

    if (!m_vbo) {
        m_vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        if (!m_vbo->create()) {
            m_vbo.reset();
            prog.release();
            fbo.release();
            return QImage();
        }
        m_vbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);
    }

    int poolCursor = 0;
    auto acquireTexture16 = [&](int w, int h) -> QOpenGLTexture* {
        if (poolCursor >= m_texPool16.size())
            m_texPool16.push_back(nullptr);
        QOpenGLTexture*& slot = m_texPool16[poolCursor];
        ++poolCursor;

        if (slot && slot->isCreated() &&
            slot->width() == w && slot->height() == h &&
            slot->format() == QOpenGLTexture::RGBA16_UNorm) {
            return slot;
        }

        if (slot) {
            if (slot->isCreated())
                slot->destroy();
            delete slot;
            slot = nullptr;
        }
        auto* tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
        tex->setFormat(QOpenGLTexture::RGBA16_UNorm);
        tex->setSize(w, h);
        tex->setMinificationFilter(QOpenGLTexture::Nearest);
        tex->setMagnificationFilter(QOpenGLTexture::Nearest);
        tex->setWrapMode(QOpenGLTexture::ClampToEdge);
        tex->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16);
        if (!tex->isCreated() || !tex->isStorageAllocated()) {
            delete tex;
            return nullptr;
        }
        slot = tex;
        return slot;
    };

    auto toQMatrix3x3 = [](const aces::Mat3& m) -> QMatrix3x3 {
        // aces::Mat3 is row-major. QMatrix3x3 consumes row-major floats and
        // QOpenGLShaderProgram transposes to the column-major GLSL upload.
        const float values[9] = {
            float(m[0][0]), float(m[0][1]), float(m[0][2]),
            float(m[1][0]), float(m[1][1]), float(m[1][2]),
            float(m[2][0]), float(m[2][1]), float(m[2][2])
        };
        return QMatrix3x3(values);
    };

    for (int idx : order) {
        if (idx < 0 || idx >= layers.size())
            continue;
        const GpuLayerInput& in = layers[idx];
        const gpucomposite::LayerDesc& d = in.desc;

        if (!gpucomposite::isLayerComposited(d))
            continue;
        if (in.image.isNull() || in.image.width() <= 0 || in.image.height() <= 0)
            continue;

        QImage img = in.image.convertToFormat(QImage::Format_RGBA64_Premultiplied);

        QOpenGLTexture* tex = acquireTexture16(img.width(), img.height());
        if (!tex) {
            prog.release();
            fbo.release();
            return QImage();
        }
        tex->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16, img.constBits());

        gl->glActiveTexture(GL_TEXTURE0);
        tex->bind(0);

        const aces::ColorSpace inSpace = clipcolor::acesSpaceFor(in.colorMeta);
        const bool passthrough =
            in.colorMeta.transfer == clipcolor::Transfer::PQ
            || in.colorMeta.transfer == clipcolor::Transfer::HLG
            || inSpace == outSpace;
        const aces::Mat3 conv =
            passthrough ? aces::identity3()
                        : aces::conversionMatrix(inSpace, outSpace);
        const QMatrix3x3 convMatrix = toQMatrix3x3(conv);

        const QMatrix4x4 layerMat = gpucomposite::layerTransform(d, canvas);
        prog.setUniformValue(m_idt_uLayer, layerMat);
        prog.setUniformValue(m_idt_uOpacity,
                             float(gpucomposite::clampOpacity(d.opacity)));
        prog.setUniformValue(m_idt_uConvMatrix, convMatrix);
        prog.setUniformValue(m_idt_uPassthrough, passthrough ? 1 : 0);
        const bool inputIsLinear =
            in.colorMeta.transfer == clipcolor::Transfer::Linear
            || aces::isLinearSpace(inSpace);
        prog.setUniformValue(m_idt_uApplyEotf, inputIsLinear ? 0 : 1);
        prog.setUniformValue(m_idt_uApplyOetf,
                             aces::isLinearSpace(outSpace) ? 0 : 1);

        const float sw = float(img.width());
        const float sh = float(img.height());
        const float verts[] = {
            //  aSrcPos      aTexCoord
            0.0f, 0.0f,      0.0f, 0.0f,
            sw,   0.0f,      1.0f, 0.0f,
            0.0f, sh,        0.0f, 1.0f,
            sw,   sh,        1.0f, 1.0f,
        };

        m_vbo->bind();
        m_vbo->allocate(verts, int(sizeof(verts)));
        prog.enableAttributeArray(aSrcPos);
        prog.enableAttributeArray(aTexCoord);
        prog.setAttributeBuffer(aSrcPos,   GL_FLOAT, 0, 2, 4 * sizeof(float));
        prog.setAttributeBuffer(aTexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        prog.disableAttributeArray(aSrcPos);
        prog.disableAttributeArray(aTexCoord);
        m_vbo->release();
        tex->release();
    }

    prog.release();

    const int w = canvas.width();
    const int h = canvas.height();
    QVector<quint16> buf(w * h * 4);
    gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_SHORT, buf.data());

    fbo.release();

    QImage out(canvas, QImage::Format_RGBA64_Premultiplied);
    if (out.isNull())
        return QImage();
    const int rowShorts = w * 4;
    const size_t rowBytes = size_t(rowShorts) * sizeof(quint16);
    for (int y = 0; y < h; ++y) {
        const quint16* srcRow = buf.constData() + size_t(h - 1 - y) * rowShorts;
        std::memcpy(out.scanLine(y), srcRow, rowBytes);
    }
    return out;
}

QImage GpuLayerCompositor::composite16Matte(const QVector<GpuLayerInput>& layers, QSize canvas) {
    // 16-bit track-matte sibling of composite(): main FBO, per-layer textures, and
    // matte temp FBOs are RGBA16; matte combine remains the existing float shader.
    // Returns NULL on GL/RGBA16 failures so parity can SKIP on unsupported systems.
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return QImage();
    if (!ensureContext())
        return QImage();
    CurrentContextGuard current(m_ctx);
    if (!current.makeCurrent(m_surface))
        return QImage();

    QOpenGLFunctions* gl = m_ctx->functions();

    if (!ensureProgram())
        return QImage();

    if (!ensureFbo16(canvas))
        return QImage();
    QOpenGLFramebufferObject& fbo = *m_fbo16;
    if (!fbo.bind())
        return QImage();

    gl->glViewport(0, 0, canvas.width(), canvas.height());
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);

    QOpenGLShaderProgram& prog = *m_prog;
    if (!prog.bind()) {
        fbo.release();
        return QImage();
    }

    QMatrix4x4 proj;
    proj.ortho(0.0f, float(canvas.width()),
               float(canvas.height()), 0.0f,
               -1.0f, 1.0f);
    prog.setUniformValue(m_uProj, proj);
    prog.setUniformValue(m_uTex, 0);

    const int aSrcPos   = m_aSrcPos;
    const int aTexCoord = m_aTexCoord;

    QVector<gpucomposite::LayerDesc> descs;
    descs.reserve(layers.size());
    for (const auto& in : layers)
        descs.push_back(in.desc);
    const QVector<int> order = gpucomposite::paintOrder(descs);

    QSet<int> matteSourceIndices;
    for (int i = 0; i < descs.size(); ++i) {
        const gpucomposite::LayerDesc& d = descs[i];
        if (d.matteType == gpucomposite::MatteType::None)
            continue;
        if (gpucomposite::isValidMatteSource(d.matteSourceIndex, i, descs.size()))
            matteSourceIndices.insert(d.matteSourceIndex);
    }

    if (!m_vbo) {
        m_vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        if (!m_vbo->create()) {
            m_vbo.reset();
            prog.release();
            fbo.release();
            return QImage();
        }
        m_vbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);
    }

    auto drawMatteCombine = [&](GLuint srcTexId, GLuint matteTexId,
                                int matteTypeOrdinal, float opacity) -> bool {
        QOpenGLShaderProgram& mp = *m_matteProg;
        if (!mp.bind())
            return false;

        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, srcTexId);
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindTexture(GL_TEXTURE_2D, matteTexId);

        mp.setUniformValue(m_mSrcTex, 0);
        mp.setUniformValue(m_mMatteTex, 1);
        mp.setUniformValue(m_mMatteType, matteTypeOrdinal);
        mp.setUniformValue(m_mOpacity, opacity);

        const float quad[] = {
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 1.0f,
             1.0f,  1.0f,  1.0f, 1.0f,
        };
        m_matteVbo->bind();
        m_matteVbo->allocate(quad, int(sizeof(quad)));
        mp.enableAttributeArray(m_mAPos);
        mp.enableAttributeArray(m_mATexCoord);
        mp.setAttributeBuffer(m_mAPos,      GL_FLOAT, 0, 2, 4 * sizeof(float));
        mp.setAttributeBuffer(m_mATexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        mp.disableAttributeArray(m_mAPos);
        mp.disableAttributeArray(m_mATexCoord);
        m_matteVbo->release();

        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        mp.release();
        return true;
    };

    int poolCursor = 0;
    auto acquireTexture16 = [&](int w, int h) -> QOpenGLTexture* {
        if (poolCursor >= m_texPool16.size())
            m_texPool16.push_back(nullptr);
        QOpenGLTexture*& slot = m_texPool16[poolCursor];
        ++poolCursor;

        if (slot && slot->isCreated() &&
            slot->width() == w && slot->height() == h &&
            slot->format() == QOpenGLTexture::RGBA16_UNorm) {
            return slot;
        }

        if (slot) {
            if (slot->isCreated())
                slot->destroy();
            delete slot;
            slot = nullptr;
        }
        auto* tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
        tex->setFormat(QOpenGLTexture::RGBA16_UNorm);
        tex->setSize(w, h);
        tex->setMinificationFilter(QOpenGLTexture::Nearest);
        tex->setMagnificationFilter(QOpenGLTexture::Nearest);
        tex->setWrapMode(QOpenGLTexture::ClampToEdge);
        tex->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16);
        if (!tex->isCreated() || !tex->isStorageAllocated()) {
            delete tex;
            return nullptr;
        }
        slot = tex;
        return slot;
    };

    for (int idx : order) {
        if (idx < 0 || idx >= layers.size())
            continue;
        const GpuLayerInput& in = layers[idx];
        const gpucomposite::LayerDesc& d = in.desc;

        if (!gpucomposite::isLayerComposited(d))
            continue;
        if (in.image.isNull() || in.image.width() <= 0 || in.image.height() <= 0)
            continue;
        if (matteSourceIndices.contains(idx))
            continue;

        const bool wantsMatte =
            d.matteType != gpucomposite::MatteType::None &&
            gpucomposite::isValidMatteSource(d.matteSourceIndex, idx, layers.size());

        if (wantsMatte) {
            const int mIdx = d.matteSourceIndex;
            const GpuLayerInput& matteIn = layers[mIdx];

            if (!matteIn.image.isNull() &&
                matteIn.image.width() > 0 && matteIn.image.height() > 0) {
                QOpenGLFramebufferObject* srcF =
                    ensureTempFbo16(m_matteFboSrc16, canvas);
                QOpenGLFramebufferObject* matteF =
                    ensureTempFbo16(m_matteFboMatte16, canvas);

                if (!srcF || !matteF) {
                    prog.release();
                    fbo.release();
                    return QImage();
                }

                if (m_matteVbo == nullptr) {
                    m_matteVbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
                    if (m_matteVbo->create())
                        m_matteVbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);
                    else
                        m_matteVbo.reset();
                }
                if (!m_matteVbo || !ensureMatteProgram()) {
                    prog.release();
                    fbo.release();
                    return QImage();
                }

                prog.release();

                const bool okMatte =
                    renderLayerToFbo16(*matteF, matteIn.image, matteIn.desc, canvas, proj);
                const bool okSrc =
                    renderLayerToFbo16(*srcF, in.image, d, canvas, proj);
                if (!okMatte || !okSrc) {
                    fbo.release();
                    return QImage();
                }

                if (!fbo.bind())
                    return QImage();
                gl->glViewport(0, 0, canvas.width(), canvas.height());
                gl->glEnable(GL_BLEND);
                gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                gl->glDisable(GL_DEPTH_TEST);
                gl->glDisable(GL_CULL_FACE);

                const bool okCombine =
                    drawMatteCombine(srcF->texture(), matteF->texture(),
                                     static_cast<int>(d.matteType),
                                     float(gpucomposite::clampOpacity(d.opacity)));

                if (!fbo.bind())
                    return QImage();
                gl->glViewport(0, 0, canvas.width(), canvas.height());
                if (!prog.bind()) {
                    fbo.release();
                    return QImage();
                }
                prog.setUniformValue(m_uProj, proj);
                prog.setUniformValue(m_uTex, 0);
                gl->glActiveTexture(GL_TEXTURE0);

                if (!okCombine) {
                    prog.release();
                    fbo.release();
                    return QImage();
                }
                continue;
            }
            // Valid matte relationship but missing matte image: match the existing
            // matte-not-ready rule and composite the layer plainly.
        }

        QImage img = in.image.convertToFormat(QImage::Format_RGBA64_Premultiplied);

        QOpenGLTexture* tex = acquireTexture16(img.width(), img.height());
        if (!tex) {
            prog.release();
            fbo.release();
            return QImage();
        }
        tex->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16, img.constBits());

        gl->glActiveTexture(GL_TEXTURE0);
        tex->bind(0);

        const QMatrix4x4 layerMat = gpucomposite::layerTransform(d, canvas);
        prog.setUniformValue(m_uLayer, layerMat);
        prog.setUniformValue(m_uOpacity,
                             float(gpucomposite::clampOpacity(d.opacity)));

        const float sw = float(img.width());
        const float sh = float(img.height());
        const float verts[] = {
            0.0f, 0.0f,  0.0f, 0.0f,
            sw,   0.0f,  1.0f, 0.0f,
            0.0f, sh,    0.0f, 1.0f,
            sw,   sh,    1.0f, 1.0f,
        };

        m_vbo->bind();
        m_vbo->allocate(verts, int(sizeof(verts)));
        prog.enableAttributeArray(aSrcPos);
        prog.enableAttributeArray(aTexCoord);
        prog.setAttributeBuffer(aSrcPos,   GL_FLOAT, 0, 2, 4 * sizeof(float));
        prog.setAttributeBuffer(aTexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        prog.disableAttributeArray(aSrcPos);
        prog.disableAttributeArray(aTexCoord);
        m_vbo->release();
        tex->release();
    }

    prog.release();

    const int w = canvas.width();
    const int h = canvas.height();
    QVector<quint16> buf(w * h * 4);
    gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_SHORT, buf.data());

    fbo.release();

    QImage out(canvas, QImage::Format_RGBA64_Premultiplied);
    if (out.isNull())
        return QImage();
    const int rowShorts = w * 4;
    const size_t rowBytes = size_t(rowShorts) * sizeof(quint16);
    for (int y = 0; y < h; ++y) {
        const quint16* srcRow = buf.constData() + size_t(h - 1 - y) * rowShorts;
        std::memcpy(out.scanLine(y), srcRow, rowBytes);
    }
    return out;
}

QImage GpuLayerCompositor::composite16IdtMatte(const QVector<GpuLayerInput>& layers, QSize canvas) {
    // IDT-aware 16-bit track matte compositor. The shape is intentionally the
    // same as composite16Matte(), but every layer render uses m_idtProg. For
    // luma mattes this means the matte source RGB is transformed to V1 output
    // space before kMatteFragSrc computes Rec.601 luma.
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return QImage();
    if (!ensureContext())
        return QImage();
    CurrentContextGuard current(m_ctx);
    if (!current.makeCurrent(m_surface))
        return QImage();

    QOpenGLFunctions* gl = m_ctx->functions();

    if (!ensureIdtProgram())
        return QImage();

    if (!ensureFbo16(canvas))
        return QImage();
    QOpenGLFramebufferObject& fbo = *m_fbo16;
    if (!fbo.bind())
        return QImage();

    gl->glViewport(0, 0, canvas.width(), canvas.height());
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);

    QOpenGLShaderProgram& prog = *m_idtProg;
    if (!prog.bind()) {
        fbo.release();
        return QImage();
    }

    QMatrix4x4 proj;
    proj.ortho(0.0f, float(canvas.width()),
               float(canvas.height()), 0.0f,
               -1.0f, 1.0f);
    prog.setUniformValue(m_idt_uProj, proj);
    prog.setUniformValue(m_idt_uTex, 0);

    const int aSrcPos   = m_idt_aSrcPos;
    const int aTexCoord = m_idt_aTexCoord;

    aces::ColorSpace outSpace = aces::ColorSpace::sRGB;
    bool haveOutSpace = false;
    int minSourceTrack = 0;
    for (const GpuLayerInput& in : layers) {
        if (!haveOutSpace || in.desc.sourceTrack < minSourceTrack) {
            haveOutSpace = true;
            minSourceTrack = in.desc.sourceTrack;
            outSpace = clipcolor::acesSpaceFor(in.colorMeta);
        }
    }

    QVector<gpucomposite::LayerDesc> descs;
    descs.reserve(layers.size());
    for (const auto& in : layers)
        descs.push_back(in.desc);
    const QVector<int> order = gpucomposite::paintOrder(descs);

    QSet<int> matteSourceIndices;
    for (int i = 0; i < descs.size(); ++i) {
        const gpucomposite::LayerDesc& d = descs[i];
        if (d.matteType == gpucomposite::MatteType::None)
            continue;
        if (gpucomposite::isValidMatteSource(d.matteSourceIndex, i, descs.size()))
            matteSourceIndices.insert(d.matteSourceIndex);
    }

    if (!m_vbo) {
        m_vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        if (!m_vbo->create()) {
            m_vbo.reset();
            prog.release();
            fbo.release();
            return QImage();
        }
        m_vbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);
    }

    auto drawMatteCombine = [&](GLuint srcTexId, GLuint matteTexId,
                                int matteTypeOrdinal, float opacity) -> bool {
        QOpenGLShaderProgram& mp = *m_matteProg;
        if (!mp.bind())
            return false;

        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, srcTexId);
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindTexture(GL_TEXTURE_2D, matteTexId);

        mp.setUniformValue(m_mSrcTex, 0);
        mp.setUniformValue(m_mMatteTex, 1);
        mp.setUniformValue(m_mMatteType, matteTypeOrdinal);
        mp.setUniformValue(m_mOpacity, opacity);

        const float quad[] = {
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 1.0f,
             1.0f,  1.0f,  1.0f, 1.0f,
        };
        m_matteVbo->bind();
        m_matteVbo->allocate(quad, int(sizeof(quad)));
        mp.enableAttributeArray(m_mAPos);
        mp.enableAttributeArray(m_mATexCoord);
        mp.setAttributeBuffer(m_mAPos,      GL_FLOAT, 0, 2, 4 * sizeof(float));
        mp.setAttributeBuffer(m_mATexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        mp.disableAttributeArray(m_mAPos);
        mp.disableAttributeArray(m_mATexCoord);
        m_matteVbo->release();

        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        mp.release();
        return true;
    };

    int poolCursor = 0;
    auto acquireTexture16 = [&](int w, int h) -> QOpenGLTexture* {
        if (poolCursor >= m_texPool16.size())
            m_texPool16.push_back(nullptr);
        QOpenGLTexture*& slot = m_texPool16[poolCursor];
        ++poolCursor;

        if (slot && slot->isCreated() &&
            slot->width() == w && slot->height() == h &&
            slot->format() == QOpenGLTexture::RGBA16_UNorm) {
            return slot;
        }

        if (slot) {
            if (slot->isCreated())
                slot->destroy();
            delete slot;
            slot = nullptr;
        }
        auto* tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
        tex->setFormat(QOpenGLTexture::RGBA16_UNorm);
        tex->setSize(w, h);
        tex->setMinificationFilter(QOpenGLTexture::Nearest);
        tex->setMagnificationFilter(QOpenGLTexture::Nearest);
        tex->setWrapMode(QOpenGLTexture::ClampToEdge);
        tex->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16);
        if (!tex->isCreated() || !tex->isStorageAllocated()) {
            delete tex;
            return nullptr;
        }
        slot = tex;
        return slot;
    };

    auto toQMatrix3x3 = [](const aces::Mat3& m) -> QMatrix3x3 {
        // aces::Mat3 is row-major. QMatrix3x3 consumes row-major floats and
        // QOpenGLShaderProgram transposes to the column-major GLSL upload.
        const float values[9] = {
            float(m[0][0]), float(m[0][1]), float(m[0][2]),
            float(m[1][0]), float(m[1][1]), float(m[1][2]),
            float(m[2][0]), float(m[2][1]), float(m[2][2])
        };
        return QMatrix3x3(values);
    };

    for (int idx : order) {
        if (idx < 0 || idx >= layers.size())
            continue;
        const GpuLayerInput& in = layers[idx];
        const gpucomposite::LayerDesc& d = in.desc;

        if (!gpucomposite::isLayerComposited(d))
            continue;
        if (in.image.isNull() || in.image.width() <= 0 || in.image.height() <= 0)
            continue;
        if (matteSourceIndices.contains(idx))
            continue;

        const bool wantsMatte =
            d.matteType != gpucomposite::MatteType::None &&
            gpucomposite::isValidMatteSource(d.matteSourceIndex, idx, layers.size());

        if (wantsMatte) {
            const int mIdx = d.matteSourceIndex;
            const GpuLayerInput& matteIn = layers[mIdx];

            if (!matteIn.image.isNull() &&
                matteIn.image.width() > 0 && matteIn.image.height() > 0) {
                QOpenGLFramebufferObject* srcF =
                    ensureTempFbo16(m_matteFboSrc16, canvas);
                QOpenGLFramebufferObject* matteF =
                    ensureTempFbo16(m_matteFboMatte16, canvas);

                if (!srcF || !matteF) {
                    prog.release();
                    fbo.release();
                    return QImage();
                }

                if (m_matteVbo == nullptr) {
                    m_matteVbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
                    if (m_matteVbo->create())
                        m_matteVbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);
                    else
                        m_matteVbo.reset();
                }
                if (!m_matteVbo || !ensureMatteProgram()) {
                    prog.release();
                    fbo.release();
                    return QImage();
                }

                prog.release();

                const bool okMatte =
                    renderLayerToFbo16Idt(*matteF, matteIn.image, matteIn.desc,
                                          matteIn.colorMeta, outSpace, canvas, proj);
                const bool okSrc =
                    renderLayerToFbo16Idt(*srcF, in.image, d,
                                          in.colorMeta, outSpace, canvas, proj);
                if (!okMatte || !okSrc) {
                    fbo.release();
                    return QImage();
                }

                if (!fbo.bind())
                    return QImage();
                gl->glViewport(0, 0, canvas.width(), canvas.height());
                gl->glEnable(GL_BLEND);
                gl->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                gl->glDisable(GL_DEPTH_TEST);
                gl->glDisable(GL_CULL_FACE);

                const bool okCombine =
                    drawMatteCombine(srcF->texture(), matteF->texture(),
                                     static_cast<int>(d.matteType),
                                     float(gpucomposite::clampOpacity(d.opacity)));

                if (!fbo.bind())
                    return QImage();
                gl->glViewport(0, 0, canvas.width(), canvas.height());
                if (!prog.bind()) {
                    fbo.release();
                    return QImage();
                }
                prog.setUniformValue(m_idt_uProj, proj);
                prog.setUniformValue(m_idt_uTex, 0);
                gl->glActiveTexture(GL_TEXTURE0);

                if (!okCombine) {
                    prog.release();
                    fbo.release();
                    return QImage();
                }
                continue;
            }
            // Valid matte relationship but missing matte image: match the existing
            // matte-not-ready rule and composite the layer plainly.
        }

        QImage img = in.image.convertToFormat(QImage::Format_RGBA64_Premultiplied);

        QOpenGLTexture* tex = acquireTexture16(img.width(), img.height());
        if (!tex) {
            prog.release();
            fbo.release();
            return QImage();
        }
        tex->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt16, img.constBits());

        gl->glActiveTexture(GL_TEXTURE0);
        tex->bind(0);

        const aces::ColorSpace inSpace = clipcolor::acesSpaceFor(in.colorMeta);
        const bool passthrough =
            in.colorMeta.transfer == clipcolor::Transfer::PQ
            || in.colorMeta.transfer == clipcolor::Transfer::HLG
            || inSpace == outSpace;
        const aces::Mat3 conv =
            passthrough ? aces::identity3()
                        : aces::conversionMatrix(inSpace, outSpace);
        const QMatrix3x3 convMatrix = toQMatrix3x3(conv);

        const QMatrix4x4 layerMat = gpucomposite::layerTransform(d, canvas);
        prog.setUniformValue(m_idt_uLayer, layerMat);
        prog.setUniformValue(m_idt_uOpacity,
                             float(gpucomposite::clampOpacity(d.opacity)));
        prog.setUniformValue(m_idt_uConvMatrix, convMatrix);
        prog.setUniformValue(m_idt_uPassthrough, passthrough ? 1 : 0);
        const bool inputIsLinear =
            in.colorMeta.transfer == clipcolor::Transfer::Linear
            || aces::isLinearSpace(inSpace);
        prog.setUniformValue(m_idt_uApplyEotf, inputIsLinear ? 0 : 1);
        prog.setUniformValue(m_idt_uApplyOetf,
                             aces::isLinearSpace(outSpace) ? 0 : 1);

        const float sw = float(img.width());
        const float sh = float(img.height());
        const float verts[] = {
            0.0f, 0.0f,  0.0f, 0.0f,
            sw,   0.0f,  1.0f, 0.0f,
            0.0f, sh,    0.0f, 1.0f,
            sw,   sh,    1.0f, 1.0f,
        };

        m_vbo->bind();
        m_vbo->allocate(verts, int(sizeof(verts)));
        prog.enableAttributeArray(aSrcPos);
        prog.enableAttributeArray(aTexCoord);
        prog.setAttributeBuffer(aSrcPos,   GL_FLOAT, 0, 2, 4 * sizeof(float));
        prog.setAttributeBuffer(aTexCoord, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        prog.disableAttributeArray(aSrcPos);
        prog.disableAttributeArray(aTexCoord);
        m_vbo->release();
        tex->release();
    }

    prog.release();

    const int w = canvas.width();
    const int h = canvas.height();
    QVector<quint16> buf(w * h * 4);
    gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_SHORT, buf.data());

    fbo.release();

    QImage out(canvas, QImage::Format_RGBA64_Premultiplied);
    if (out.isNull())
        return QImage();
    const int rowShorts = w * 4;
    const size_t rowBytes = size_t(rowShorts) * sizeof(quint16);
    for (int y = 0; y < h; ++y) {
        const quint16* srcRow = buf.constData() + size_t(h - 1 - y) * rowShorts;
        std::memcpy(out.scanLine(y), srcRow, rowBytes);
    }
    return out;
}
