#include "GpuLayerCompositor.h"

#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QSurfaceFormat>
#include <QMatrix4x4>
#include <QVector>
#include <QImage>

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

namespace {

struct CurrentContextGuard {
    QOpenGLContext* ctx = nullptr;
    ~CurrentContextGuard() {
        if (ctx)
            ctx->doneCurrent();
    }
};

} // namespace

GpuLayerCompositor::GpuLayerCompositor() = default;

GpuLayerCompositor::~GpuLayerCompositor() {
    // Tear down GL objects with the context current, then destroy the context.
    if (m_ctx && m_surface) {
        if (m_ctx->makeCurrent(m_surface)) {
            m_ctx->doneCurrent();
        }
    }
    delete m_surface;
    m_surface = nullptr;
    delete m_ctx;
    m_ctx = nullptr;
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

    if (!m_ctx->makeCurrent(m_surface)) {
        delete m_surface;
        m_surface = nullptr;
        delete m_ctx;
        m_ctx = nullptr;
        m_available = false;
        return false;
    }
    m_ctx->doneCurrent();

    m_available = true;
    return true;
}

bool GpuLayerCompositor::isAvailable() {
    return ensureContext();
}

QImage GpuLayerCompositor::composite(const QVector<GpuLayerInput>& layers, QSize canvas) {
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return QImage();
    if (!ensureContext())
        return QImage();
    if (!m_ctx->makeCurrent(m_surface))
        return QImage();
    CurrentContextGuard current{m_ctx};

    QOpenGLFunctions* gl = m_ctx->functions();

    // --- Offscreen FBO (canvas, GL_RGBA8) ---
    QOpenGLFramebufferObjectFormat fboFmt;
    fboFmt.setInternalTextureFormat(GL_RGBA8);
    fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    QOpenGLFramebufferObject fbo(canvas, fboFmt);
    if (!fbo.isValid())
        return QImage();
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

    // --- Shader program ---
    QOpenGLShaderProgram prog;
    bool ok = prog.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertSrc) &&
              prog.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc) &&
              prog.link();
    if (!ok) {
        fbo.release();
        return QImage();
    }
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
    prog.setUniformValue("uProj", proj);
    prog.setUniformValue("uTex", 0);

    const int aSrcPos   = prog.attributeLocation("aSrcPos");
    const int aTexCoord = prog.attributeLocation("aTexCoord");

    // Collect LayerDescs for paint-order computation (index-aligned with layers).
    QVector<gpucomposite::LayerDesc> descs;
    descs.reserve(layers.size());
    for (const auto& in : layers)
        descs.push_back(in.desc);

    // Back-to-front: paintOrder is DESCENDING sourceTrack, so V1 (lowest) draws
    // LAST and wins (frontmost) under source-over.
    const QVector<int> order = gpucomposite::paintOrder(descs);

    for (int idx : order) {
        if (idx < 0 || idx >= layers.size())
            continue;
        const GpuLayerInput& in = layers[idx];
        const gpucomposite::LayerDesc& d = in.desc;

        if (!gpucomposite::isLayerComposited(d))
            continue;
        if (in.image.isNull() || in.image.width() <= 0 || in.image.height() <= 0)
            continue;

        // Upload premultiplied. ARGB32_Premultiplied matches CPU SSOT's frame
        // format and gives correct premultiplied texels for the blend above.
        QImage img = in.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        QOpenGLTexture tex(QOpenGLTexture::Target2D);
        tex.setFormat(QOpenGLTexture::RGBA8_UNorm);
        tex.setSize(img.width(), img.height());
        tex.setMinificationFilter(QOpenGLTexture::Nearest);
        tex.setMagnificationFilter(QOpenGLTexture::Nearest);
        tex.setWrapMode(QOpenGLTexture::ClampToEdge);
        tex.allocateStorage();
        // ARGB32 in memory is BGRA byte order on little-endian -> upload as BGRA.
        tex.setData(QOpenGLTexture::BGRA, QOpenGLTexture::UInt8, img.constBits());

        gl->glActiveTexture(GL_TEXTURE0);
        tex.bind(0);

        // src-native -> canvas-pixel (canonical CPU SSOT transform).
        const QMatrix4x4 layerMat =
            gpucomposite::layerTransform(d, canvas);
        prog.setUniformValue("uLayer", layerMat);
        prog.setUniformValue("uOpacity",
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

        prog.enableAttributeArray(aSrcPos);
        prog.enableAttributeArray(aTexCoord);
        prog.setAttributeArray(aSrcPos,   verts,     2, 4 * sizeof(float));
        prog.setAttributeArray(aTexCoord, verts + 2, 2, 4 * sizeof(float));

        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        prog.disableAttributeArray(aSrcPos);
        prog.disableAttributeArray(aTexCoord);
        tex.release();
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
