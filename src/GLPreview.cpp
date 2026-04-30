#include "GLPreview.h"
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <QtGlobal>
#include <QDateTime>
#include <QVector2D>
#include <QOpenGLContext>
#include <QDebug>
#include <QPainter>
#include <QPen>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QFontMetrics>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// windef.h leaks `near`/`far` and min/max macros that collide with C++
// identifiers used elsewhere in this file (see hitTestTextToolHandle).
#undef near
#undef far
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// WGL_NV_DX_interop2 typedefs. Kept in the .cpp so windows.h doesn't leak
// through GLPreview.h to the rest of the codebase. WGL_ACCESS_*_NV constants
// are defined here for Section B/C use.
#ifndef WGL_ACCESS_READ_ONLY_NV
#define WGL_ACCESS_READ_ONLY_NV  0x00000000
#define WGL_ACCESS_READ_WRITE_NV 0x00000001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x00000002
#endif

// PFNWGLDXSETRESOURCESHAREHANDLENVPROC slot is resolved in Section C when
// shared-texture creation needs it; Section A does not call it.
typedef BOOL    (WINAPI *PFNWGLDXSETRESOURCESHAREHANDLENVPROC)(void *dxObject, HANDLE shareHandle);
typedef HANDLE  (WINAPI *PFNWGLDXOPENDEVICENVPROC)(void *dxDevice);
typedef BOOL    (WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE  (WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL    (WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL    (WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL    (WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);

typedef const char * (WINAPI *PFNWGLGETEXTENSIONSSTRINGARBPROC)(HDC hdc);

namespace {
PFNWGLDXOPENDEVICENVPROC       gWglDXOpenDeviceNV       = nullptr;
PFNWGLDXCLOSEDEVICENVPROC      gWglDXCloseDeviceNV      = nullptr;
PFNWGLDXREGISTEROBJECTNVPROC   gWglDXRegisterObjectNV   = nullptr;
PFNWGLDXUNREGISTEROBJECTNVPROC gWglDXUnregisterObjectNV = nullptr;
PFNWGLDXLOCKOBJECTSNVPROC      gWglDXLockObjectsNV      = nullptr;
PFNWGLDXUNLOCKOBJECTSNVPROC    gWglDXUnlockObjectsNV    = nullptr;
} // namespace
#endif // Q_OS_WIN

namespace {
// Read VEDITOR_GL_INTEROP once and cache; mirrors veditorTickTraceEnabled()
// pattern in VideoPlayer.cpp so production with the envvar unset pays a
// single env read at first call and zero work after.
bool veditorGlInteropEnabled()
{
    static const bool kEnabled = qEnvironmentVariableIntValue("VEDITOR_GL_INTEROP") != 0;
    return kEnabled;
}
} // namespace

// Vertex shader — simple fullscreen quad
static const char *vertexShaderSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

// Fragment shader — color correction + color grading pipeline on GPU
static const char *fragmentShaderSrc = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform bool uEffectsEnabled;

// Color correction uniforms
uniform float uBrightness;   // -100 to 100
uniform float uContrast;     // -100 to 100
uniform float uSaturation;   // -100 to 100
uniform float uHue;          // -180 to 180
uniform float uTemperature;  // -100 to 100
uniform float uTint;         // -100 to 100
uniform float uGamma;        // 0.1 to 3.0
uniform float uHighlights;   // -100 to 100
uniform float uShadows;      // -100 to 100
uniform float uExposure;     // -3.0 to 3.0

// Lift/Gamma/Gain color wheels (DaVinci Resolve style)
uniform float uLiftR, uLiftG, uLiftB;     // -1.0 to 1.0
uniform float uGammaR, uGammaG, uGammaB;  // -1.0 to 1.0
uniform float uGainR, uGainG, uGainB;     // -1.0 to 1.0

// 3D LUT uniforms
uniform sampler3D uLut3D;
uniform float uLutIntensity;  // 0.0 to 1.0
uniform bool uLutEnabled;

// 0=sRGB, 1=PQ, 2=HLG. Non-zero applies inverse EOTF + Hable tone map before grading.
uniform int uHdrTransfer;

// GPU Video Effects — run after CC. Sharpen/Mosaic/ChromaKey stay CPU.
uniform bool  uFxBlurEnable;
uniform float uFxBlurRadius;   // pixels (shader clamps to a small box kernel)
uniform vec2  uFxTexSize;      // texture pixel size for blur sampling
uniform bool  uFxNoiseEnable;
uniform float uFxNoiseAmount;  // 0..1 luma jitter
uniform bool  uFxSepiaEnable;
uniform float uFxSepiaStrength;     // 0..1 lerp
uniform bool  uFxGrayEnable;
uniform float uFxGrayStrength;      // 0..1 lerp
uniform bool  uFxInvertEnable;
uniform float uFxInvertStrength;    // 0..1 lerp
uniform bool  uFxVignetteEnable;
uniform float uFxVignetteIntensity; // 0..1 strength
uniform float uFxVignetteRadius;    // 0..1 inner radius
uniform float uFxTime;         // seconds, seeds the noise hash

vec3 pqInverseEotf(vec3 E) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 Ep = pow(max(E, vec3(0.0)), vec3(1.0 / m2));
    vec3 num = max(Ep - vec3(c1), vec3(0.0));
    vec3 den = vec3(c2) - vec3(c3) * Ep;
    return pow(num / den, vec3(1.0 / m1));
}

vec3 hlgInverseOetf(vec3 E) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 result;
    result.r = (E.r <= 0.5) ? (E.r * E.r / 3.0) : (exp((E.r - c) / a) + b) / 12.0;
    result.g = (E.g <= 0.5) ? (E.g * E.g / 3.0) : (exp((E.g - c) / a) + b) / 12.0;
    result.b = (E.b <= 0.5) ? (E.b * E.b / 3.0) : (exp((E.b - c) / a) + b) / 12.0;
    return result;
}

vec3 hableCurve(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 hableToneMap(vec3 color) {
    const float W = 11.2;
    vec3 mapped = hableCurve(color * 2.0);
    vec3 whiteScale = vec3(1.0) / hableCurve(vec3(W));
    return mapped * whiteScale;
}

vec3 adjustExposure(vec3 color, float exposure) {
    return color * pow(2.0, exposure);
}

vec3 adjustBrightnessContrast(vec3 color, float brightness, float contrast) {
    vec3 c = color + brightness / 100.0;
    float factor = (100.0 + contrast) / 100.0;
    factor *= factor;
    c = (c - 0.5) * factor + 0.5;
    return c;
}

vec3 adjustHighlightsShadows(vec3 color, float highlights, float shadows) {
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float hWeight = lum * lum;
    float sWeight = (1.0 - lum) * (1.0 - lum);
    float adjust = highlights / 100.0 * hWeight + shadows / 100.0 * sWeight;
    return color + adjust;
}

vec3 adjustSaturation(vec3 color, float saturation) {
    float factor = (saturation + 100.0) / 100.0;
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(lum), color, factor);
}

vec3 adjustHue(vec3 color, float hue) {
    float angle = radians(hue);
    float cosA = cos(angle);
    float sinA = sin(angle);

    mat3 hueRotation = mat3(
        0.213 + 0.787 * cosA - 0.213 * sinA,
        0.213 - 0.213 * cosA + 0.143 * sinA,
        0.213 - 0.213 * cosA - 0.787 * sinA,
        0.715 - 0.715 * cosA - 0.715 * sinA,
        0.715 + 0.285 * cosA + 0.140 * sinA,
        0.715 - 0.715 * cosA + 0.715 * sinA,
        0.072 - 0.072 * cosA + 0.928 * sinA,
        0.072 - 0.072 * cosA - 0.283 * sinA,
        0.072 + 0.928 * cosA + 0.072 * sinA
    );

    return hueRotation * color;
}

vec3 adjustTemperatureTint(vec3 color, float temperature, float tint) {
    float rShift = temperature * 0.005;
    float bShift = -temperature * 0.005;
    float gShift = -tint * 0.003;
    float mShift = tint * 0.002;
    color.r += rShift + mShift;
    color.g += gShift;
    color.b += bShift + mShift;
    return color;
}

vec3 adjustGamma(vec3 color, float gamma) {
    float invGamma = 1.0 / gamma;
    return pow(max(color, vec3(0.0)), vec3(invGamma));
}

// DaVinci Resolve-style Lift/Gamma/Gain
// Lift  = shadows offset (applied more in darks)
// Gamma = midtone power (applied via power function weighted to midtones)
// Gain  = highlight multiplier (scales the entire signal)
vec3 applyLiftGammaGain(vec3 color, vec3 lift, vec3 gamma, vec3 gain) {
    // Gain: multiply signal  (1.0 + gain adjustment)
    vec3 gained = color * (vec3(1.0) + gain);

    // Lift: add offset weighted by inverse luminance (affects shadows more)
    vec3 lifted = gained + lift * (vec3(1.0) - gained);

    // Gamma: power curve through midtones
    // gamma adjustment maps [-1,1] to a power curve: 0 = no change, negative = darken mids, positive = brighten mids
    vec3 gammaPow = vec3(1.0) / max(vec3(1.0) + gamma, vec3(0.01));
    vec3 result = pow(max(lifted, vec3(0.0)), gammaPow);

    return result;
}

vec3 fxBlur(vec2 uv) {
    float r = clamp(uFxBlurRadius, 0.0, 8.0);
    if (r < 0.5 || uFxTexSize.x <= 0.0) return texture(uTexture, uv).rgb;
    vec2 px = 1.0 / uFxTexSize;
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    int ri = int(ceil(r));
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            if (abs(dx) > ri || abs(dy) > ri) continue;
            vec2 off = vec2(float(dx), float(dy)) * px;
            acc += texture(uTexture, uv + off).rgb;
            wsum += 1.0;
        }
    }
    return (wsum > 0.0) ? acc / wsum : texture(uTexture, uv).rgb;
}

float fxHash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 fxApplyNoise(vec3 color, vec2 uv) {
    float n = fxHash(uv * uFxTexSize + vec2(uFxTime, uFxTime * 1.3));
    float j = (n - 0.5) * uFxNoiseAmount;
    return clamp(color + vec3(j), 0.0, 1.0);
}

vec3 fxApplySepia(vec3 color, float t) {
    vec3 sepia = vec3(
        dot(color, vec3(0.393, 0.769, 0.189)),
        dot(color, vec3(0.349, 0.686, 0.168)),
        dot(color, vec3(0.272, 0.534, 0.131))
    );
    return mix(color, sepia, t);
}

vec3 fxApplyGray(vec3 color, float t) {
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(color, vec3(lum), t);
}

vec3 fxApplyInvert(vec3 color, float t) {
    return mix(color, vec3(1.0) - color, t);
}

vec3 fxApplyVignette(vec3 color, vec2 uv) {
    float dist = distance(uv, vec2(0.5));
    float edge = uFxVignetteRadius;
    float falloff = smoothstep(edge, edge + 0.35, dist);
    float factor = 1.0 - falloff * uFxVignetteIntensity;
    return color * factor;
}

void main() {
    vec4 texColor = texture(uTexture, vTexCoord);
    vec3 color = uFxBlurEnable ? fxBlur(vTexCoord) : texColor.rgb;

    if (uHdrTransfer == 1) {
        color = pqInverseEotf(color);
        color = hableToneMap(color * 10.0);
    } else if (uHdrTransfer == 2) {
        color = hlgInverseOetf(color);
        color = hableToneMap(color);
    }

    if (uEffectsEnabled) {
        // Apply color correction pipeline (same order as CPU)
        if (uExposure != 0.0)
            color = adjustExposure(color, uExposure);
        if (uBrightness != 0.0 || uContrast != 0.0)
            color = adjustBrightnessContrast(color, uBrightness, uContrast);
        if (uHighlights != 0.0 || uShadows != 0.0)
            color = adjustHighlightsShadows(color, uHighlights, uShadows);
        if (uSaturation != 0.0)
            color = adjustSaturation(color, uSaturation);
        if (uHue != 0.0)
            color = adjustHue(color, uHue);
        if (uTemperature != 0.0 || uTint != 0.0)
            color = adjustTemperatureTint(color, uTemperature, uTint);
        if (uGamma != 1.0)
            color = adjustGamma(color, uGamma);

        // Lift/Gamma/Gain color wheels (DaVinci Resolve style)
        vec3 lift  = vec3(uLiftR,  uLiftG,  uLiftB);
        vec3 gamma = vec3(uGammaR, uGammaG, uGammaB);
        vec3 gain  = vec3(uGainR,  uGainG,  uGainB);
        if (lift != vec3(0.0) || gamma != vec3(0.0) || gain != vec3(0.0))
            color = applyLiftGammaGain(color, lift, gamma, gain);

        // 3D LUT application
        if (uLutEnabled) {
            vec3 lutColor = texture(uLut3D, clamp(color, 0.0, 1.0)).rgb;
            color = mix(color, lutColor, uLutIntensity);
        }
    }

    if (uFxSepiaEnable)    color = fxApplySepia(color, uFxSepiaStrength);
    if (uFxGrayEnable)     color = fxApplyGray(color, uFxGrayStrength);
    if (uFxInvertEnable)   color = fxApplyInvert(color, uFxInvertStrength);
    if (uFxVignetteEnable) color = fxApplyVignette(color, vTexCoord);
    if (uFxNoiseEnable)    color = fxApplyNoise(color, vTexCoord);

    FragColor = vec4(clamp(color, 0.0, 1.0), texColor.a);
}
)";

// Section C — NV12 zero-copy preview. We sample two single-component textures
// (Y as R8, UV as RG8) backed by the same FFmpeg-decoded ID3D11Texture2D
// subresource and convert to BT.709 limited-range RGB on the GPU.
static const char *nv12VertexShaderSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static const char *nv12FragmentShaderSrc = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexY;
uniform sampler2D uTexUV;
void main() {
    float y = (texture(uTexY, vTexCoord).r - 0.0625) * 1.164;
    vec2 cb_cr = texture(uTexUV, vTexCoord).rg - vec2(0.5, 0.5);
    float r = y                  + 1.793 * cb_cr.y;
    float g = y - 0.213 * cb_cr.x - 0.533 * cb_cr.y;
    float b = y + 2.112 * cb_cr.x;
    FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
)";

#if defined(Q_OS_WIN)
namespace {
struct NV12RegisteredTex {
    void *d3d11Tex = nullptr;
    int   subresource = 0;
    HANDLE hY = nullptr;
    HANDLE hUV = nullptr;
    GLuint glTexY = 0;
    GLuint glTexUV = 0;
    int    width = 0;
    int    height = 0;
};

struct CacheKey {
    const GLPreview *owner;
    void *d3d11Tex;
    int   subresource;
    bool operator==(const CacheKey &o) const noexcept {
        return owner == o.owner && d3d11Tex == o.d3d11Tex && subresource == o.subresource;
    }
};
struct CacheKeyHash {
    size_t operator()(const CacheKey &k) const noexcept {
        // Mix all three identity components — owner discriminates instances,
        // tex+subresource discriminate frames within an instance.
        const auto a = reinterpret_cast<uintptr_t>(k.owner);
        const auto b = reinterpret_cast<uintptr_t>(k.d3d11Tex);
        return std::hash<uintptr_t>{}(a) ^ (std::hash<uintptr_t>{}(b) << 1)
               ^ (std::hash<int>{}(k.subresource) << 2);
    }
};

std::unordered_map<CacheKey, NV12RegisteredTex, CacheKeyHash> &nv12Cache()
{
    static std::unordered_map<CacheKey, NV12RegisteredTex, CacheKeyHash> g;
    return g;
}
} // namespace
#endif // Q_OS_WIN

GLPreview::GLPreview(QWidget *parent)
    : QOpenGLWidget(parent), m_vbo(QOpenGLBuffer::VertexBuffer)
{
    setMinimumSize(320, 180);
    m_textToolCaretTimer.setInterval(500);
    connect(&m_textToolCaretTimer, &QTimer::timeout, this, [this]() {
        m_textToolCaretVisible = !m_textToolCaretVisible;
        update();
    });
}

GLPreview::~GLPreview()
{
    // Primary cleanup path is cleanupGL() via QOpenGLContext::aboutToBeDestroyed.
    // Fallback: only touch GL state here if a current context is available — calling
    // makeCurrent() on a destroyed context segfaults some drivers on shutdown.
    if (QOpenGLContext::currentContext() || context()) {
        cleanupGL();
    } else {
        // Context already gone — clear raw pointers to skip double-free, but don't
        // touch GL state.
        m_texture = nullptr;
        m_program = nullptr;
    }
}

void GLPreview::cleanupGL()
{
    if (!context())
        return;

    makeCurrent();
    releaseRegisteredTexturesLocked();
#if defined(Q_OS_WIN)
    if (m_interopDevice) {
        if (gWglDXCloseDeviceNV)
            gWglDXCloseDeviceNV(static_cast<HANDLE>(m_interopDevice));
        m_interopDevice = nullptr;
        m_currentInteropD3D11Device = nullptr;
    }
#endif
    if (m_nv12Program) {
        delete m_nv12Program;
        m_nv12Program = nullptr;
    }
    if (m_lutTexture) {
        delete m_lutTexture;
        m_lutTexture = nullptr;
    }
    if (m_texture) {
        delete m_texture;
        m_texture = nullptr;
    }
    if (m_program) {
        delete m_program;
        m_program = nullptr;
    }
    if (m_vbo.isCreated())
        m_vbo.destroy();
    if (m_vao.isCreated())
        m_vao.destroy();
    doneCurrent();
}

void GLPreview::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Qt recommends cleaning up GL resources when the context is about to be
    // destroyed rather than in the widget destructor (Qt 5/6 QOpenGLWidget docs).
    if (auto *ctx = context()) {
        connect(ctx, &QOpenGLContext::aboutToBeDestroyed,
                this, &GLPreview::cleanupGL, Qt::UniqueConnection);
    }

    createShaderProgram();

    // Fullscreen quad: position(x,y) + texcoord(u,v)
    float vertices[] = {
        // pos        // tex
        -1.0f,  1.0f,  0.0f, 0.0f,  // top-left
        -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left
         1.0f,  1.0f,  1.0f, 0.0f,  // top-right
         1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right
    };

    m_vao.create();
    m_vao.bind();

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(vertices, sizeof(vertices));

    // Position attribute
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    // TexCoord attribute
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    m_vbo.release();
    m_vao.release();

    detectInteropExtension();
}

void GLPreview::detectInteropExtension()
{
    m_interopAvailable = false;

#if defined(Q_OS_WIN)
    if (!veditorGlInteropEnabled()) {
        qInfo() << "[interop] disabled (envvar VEDITOR_GL_INTEROP not set)";
        return;
    }

    QOpenGLContext *ctx = context();
    if (!ctx) {
        qInfo() << "[interop] unavailable (no current QOpenGLContext)";
        return;
    }

    auto getExtensionsString = reinterpret_cast<PFNWGLGETEXTENSIONSSTRINGARBPROC>(
        ctx->getProcAddress("wglGetExtensionsStringARB"));
    if (!getExtensionsString) {
        qInfo() << "[interop] unavailable (wglGetExtensionsStringARB missing)";
        return;
    }

    HDC hdc = wglGetCurrentDC();
    if (!hdc) {
        qInfo() << "[interop] unavailable (wglGetCurrentDC returned null)";
        return;
    }

    const char *exts = getExtensionsString(hdc);
    if (!exts) {
        qInfo() << "[interop] unavailable (wglGetExtensionsStringARB returned null)";
        return;
    }

    // Match exact token to avoid matching WGL_NV_DX_interop (v1, no multi-thread
    // lock) or hypothetical WGL_NV_DX_interop2_extended. Leading match substitutes
    // a space sentinel for the missing predecessor; trailing must be space or NUL.
    const char *needle = "WGL_NV_DX_interop2";
    bool found = false;
    for (const char *p = std::strstr(exts, needle); p; p = std::strstr(p + 1, needle)) {
        const char before = (p == exts) ? ' ' : *(p - 1);
        const char after  = *(p + std::strlen(needle));
        if (before == ' ' && (after == ' ' || after == '\0')) {
            found = true;
            break;
        }
    }
    if (!found) {
        qInfo() << "[interop] unavailable (WGL_NV_DX_interop2 not in extension string)";
        return;
    }

    struct ProcEntry { const char *name; void **slot; };
    const ProcEntry procs[] = {
        { "wglDXOpenDeviceNV",       reinterpret_cast<void**>(&gWglDXOpenDeviceNV)       },
        { "wglDXCloseDeviceNV",      reinterpret_cast<void**>(&gWglDXCloseDeviceNV)      },
        { "wglDXRegisterObjectNV",   reinterpret_cast<void**>(&gWglDXRegisterObjectNV)   },
        { "wglDXUnregisterObjectNV", reinterpret_cast<void**>(&gWglDXUnregisterObjectNV) },
        { "wglDXLockObjectsNV",      reinterpret_cast<void**>(&gWglDXLockObjectsNV)      },
        { "wglDXUnlockObjectsNV",    reinterpret_cast<void**>(&gWglDXUnlockObjectsNV)    },
    };
    for (const ProcEntry &e : procs) {
        *e.slot = reinterpret_cast<void*>(ctx->getProcAddress(e.name));
        if (!*e.slot) {
            qInfo() << "[interop] unavailable (proc resolution failed:" << e.name << ")";
            return;
        }
    }

    m_interopAvailable = true;
    const char *renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    qInfo() << "[interop] available (WGL_NV_DX_interop2, GL_RENDERER="
            << (renderer ? renderer : "<unknown>") << ")";
#else
    qInfo() << "[interop] disabled (non-Windows platform)";
#endif
}

void GLPreview::setSharedD3D11Device(void *d3d11Device)
{
#if defined(Q_OS_WIN)
    // Store the pointer regardless of m_interopAvailable — VideoPlayer can
    // call us before initializeGL runs (loadFile races the first paint).
    // Lazy ensureInteropDeviceForPaint gates the actual open by detection.
    if (d3d11Device == m_pendingD3D11Device)
        return;
    m_pendingD3D11Device = d3d11Device;
    // Existing interop handle was opened against the previous device — drop
    // every cached register entry plus the device handle itself before the
    // next paint reopens against the new device. close on the GL thread.
    if (m_interopDevice && d3d11Device != m_currentInteropD3D11Device) {
        if (context() && QOpenGLContext::currentContext() != context()) {
            makeCurrent();
            releaseRegisteredTexturesLocked();
            if (gWglDXCloseDeviceNV)
                gWglDXCloseDeviceNV(static_cast<HANDLE>(m_interopDevice));
            doneCurrent();
        } else {
            releaseRegisteredTexturesLocked();
            if (gWglDXCloseDeviceNV)
                gWglDXCloseDeviceNV(static_cast<HANDLE>(m_interopDevice));
        }
        m_interopDevice = nullptr;
        m_currentInteropD3D11Device = nullptr;
    }
    update();
#else
    Q_UNUSED(d3d11Device);
#endif
}

void GLPreview::displayD3D11Frame(void *d3d11Texture, int subresource, int width, int height)
{
#if defined(Q_OS_WIN)
    if (!m_interopAvailable || !d3d11Texture || width <= 0 || height <= 0)
        return;
    m_pendingD3D11Texture = d3d11Texture;
    m_pendingD3D11Subresource = subresource;
    m_pendingD3D11Width = width;
    m_pendingD3D11Height = height;
    update();
#else
    Q_UNUSED(d3d11Texture);
    Q_UNUSED(subresource);
    Q_UNUSED(width);
    Q_UNUSED(height);
#endif
}

void GLPreview::flushInteropCache()
{
#if defined(Q_OS_WIN)
    if (!context())
        return;
    makeCurrent();
    releaseRegisteredTexturesLocked();
    doneCurrent();
    m_pendingD3D11Texture = nullptr;
#endif
}

bool GLPreview::ensureInteropDeviceForPaint()
{
#if defined(Q_OS_WIN)
    if (!m_interopAvailable)
        return false;
    if (m_interopDevice)
        return true;
    if (!m_pendingD3D11Device)
        return false;
    if (!gWglDXOpenDeviceNV)
        return false;
    HANDLE h = gWglDXOpenDeviceNV(m_pendingD3D11Device);
    if (!h) {
        qWarning() << "[interop] device open failed (wglDXOpenDeviceNV returned null) for d3d11Device="
                   << m_pendingD3D11Device;
        return false;
    }
    m_interopDevice = h;
    m_currentInteropD3D11Device = m_pendingD3D11Device;
    qInfo() << "[interop] device opened (d3d11Device=" << m_pendingD3D11Device
            << ", interopHandle=" << h << ")";
    return true;
#else
    return false;
#endif
}

void GLPreview::releaseRegisteredTexturesLocked()
{
#if defined(Q_OS_WIN)
    auto &cache = nv12Cache();
    for (auto it = cache.begin(); it != cache.end(); ) {
        if (it->first.owner != this) { ++it; continue; }
        NV12RegisteredTex &r = it->second;
        if (m_interopDevice && gWglDXUnregisterObjectNV) {
            if (r.hY)  gWglDXUnregisterObjectNV(static_cast<HANDLE>(m_interopDevice), r.hY);
            if (r.hUV) gWglDXUnregisterObjectNV(static_cast<HANDLE>(m_interopDevice), r.hUV);
        }
        if (r.glTexY)  glDeleteTextures(1, &r.glTexY);
        if (r.glTexUV) glDeleteTextures(1, &r.glTexUV);
        it = cache.erase(it);
    }
#endif
}

void GLPreview::createShaderProgram()
{
    m_program = new QOpenGLShaderProgram(this);
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSrc)) {
        qWarning() << "GLPreview: vertex shader compile failed:" << m_program->log();
    }
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSrc)) {
        qWarning() << "GLPreview: fragment shader compile failed:" << m_program->log();
    }
    if (!m_program->link()) {
        qWarning() << "GLPreview: shader link failed:" << m_program->log();
        delete m_program;
        m_program = nullptr;
        return;
    }

    m_locTexture        = m_program->uniformLocation("uTexture");
    m_locBrightness     = m_program->uniformLocation("uBrightness");
    m_locContrast       = m_program->uniformLocation("uContrast");
    m_locSaturation     = m_program->uniformLocation("uSaturation");
    m_locHue            = m_program->uniformLocation("uHue");
    m_locTemperature    = m_program->uniformLocation("uTemperature");
    m_locTint           = m_program->uniformLocation("uTint");
    m_locGamma          = m_program->uniformLocation("uGamma");
    m_locHighlights     = m_program->uniformLocation("uHighlights");
    m_locShadows        = m_program->uniformLocation("uShadows");
    m_locExposure       = m_program->uniformLocation("uExposure");
    m_locEffectsEnabled = m_program->uniformLocation("uEffectsEnabled");
    m_locHdrTransfer    = m_program->uniformLocation("uHdrTransfer");

    m_locFxBlurEnable        = m_program->uniformLocation("uFxBlurEnable");
    m_locFxBlurRadius        = m_program->uniformLocation("uFxBlurRadius");
    m_locFxTexSize           = m_program->uniformLocation("uFxTexSize");
    m_locFxNoiseEnable       = m_program->uniformLocation("uFxNoiseEnable");
    m_locFxNoiseAmount       = m_program->uniformLocation("uFxNoiseAmount");
    m_locFxSepiaEnable       = m_program->uniformLocation("uFxSepiaEnable");
    m_locFxSepiaStrength     = m_program->uniformLocation("uFxSepiaStrength");
    m_locFxGrayEnable        = m_program->uniformLocation("uFxGrayEnable");
    m_locFxGrayStrength      = m_program->uniformLocation("uFxGrayStrength");
    m_locFxInvertEnable      = m_program->uniformLocation("uFxInvertEnable");
    m_locFxInvertStrength    = m_program->uniformLocation("uFxInvertStrength");
    m_locFxVignetteEnable    = m_program->uniformLocation("uFxVignetteEnable");
    m_locFxVignetteIntensity = m_program->uniformLocation("uFxVignetteIntensity");
    m_locFxVignetteRadius    = m_program->uniformLocation("uFxVignetteRadius");
    m_locFxTime              = m_program->uniformLocation("uFxTime");

    // Lift/Gamma/Gain
    m_locLiftR  = m_program->uniformLocation("uLiftR");
    m_locLiftG  = m_program->uniformLocation("uLiftG");
    m_locLiftB  = m_program->uniformLocation("uLiftB");
    m_locGammaR = m_program->uniformLocation("uGammaR");
    m_locGammaG = m_program->uniformLocation("uGammaG");
    m_locGammaB = m_program->uniformLocation("uGammaB");
    m_locGainR  = m_program->uniformLocation("uGainR");
    m_locGainG  = m_program->uniformLocation("uGainG");
    m_locGainB  = m_program->uniformLocation("uGainB");

    // LUT
    m_locLut3D         = m_program->uniformLocation("uLut3D");
    m_locLutIntensity  = m_program->uniformLocation("uLutIntensity");
    m_locLutEnabled    = m_program->uniformLocation("uLutEnabled");

    // NV12 zero-copy program — only used when the interop fast path engages.
    // Failure to compile/link is non-fatal: callers fall back to the legacy
    // QImage path automatically.
    m_nv12Program = new QOpenGLShaderProgram(this);
    if (!m_nv12Program->addShaderFromSourceCode(QOpenGLShader::Vertex, nv12VertexShaderSrc)
        || !m_nv12Program->addShaderFromSourceCode(QOpenGLShader::Fragment, nv12FragmentShaderSrc)
        || !m_nv12Program->link()) {
        qWarning() << "GLPreview: NV12 shader build failed:" << m_nv12Program->log();
        delete m_nv12Program;
        m_nv12Program = nullptr;
    } else {
        m_locNv12TexY  = m_nv12Program->uniformLocation("uTexY");
        m_locNv12TexUV = m_nv12Program->uniformLocation("uTexUV");
    }
}

void GLPreview::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void GLPreview::displayFrame(const QImage &frame)
{
    if (frame.isNull()) {
        qWarning() << "GLPreview::displayFrame called with null image";
        return;
    }
    const QImage::Format inFmt = frame.format();
    const bool is16 = (inFmt == QImage::Format_RGBA64
                       || inFmt == QImage::Format_RGBA64_Premultiplied);
    // Format hot path: SDR sources from frameToImage arrive as
    // Format_RGB888 already. Stamping that straight into m_currentFrame
    // saves an entire RGBA8888 conversion per frame (~3.7 ms for a 1440p
    // frame on this hardware) at no GPU cost — paintGL uploads as
    // QOpenGLTexture::RGB / RGB8_UNorm and the shader's texture() lookup
    // returns vec4(r,g,b,1.0) per the GL spec, identical to RGBA sampling.
    //
    // Phase 1e Win #8: same trick for the multi-track compositor output,
    // which arrives as ARGB32_Premultiplied. Qt stores that format as
    // BGRA bytes on little-endian; uploading via GL_BGRA + GL_UNSIGNED_BYTE
    // saves the per-pixel byte swap + alpha-unpremul that
    // convertToFormat(RGBA8888) costs (~3-5 ms / 1080p tick). Compositor
    // output has alpha=1.0 everywhere (black-fill base + opaque overlay
    // SourceOver), so premul=straight and the fragment shader's
    // texture().rgb sampling produces the same visible pixels. Disable
    // path: VEDITOR_BGRA_UPLOAD_DISABLE=1.
    static const bool bgraUploadEnabled =
        qEnvironmentVariableIntValue("VEDITOR_BGRA_UPLOAD_DISABLE") == 0;
    if (is16) {
        m_currentFrame = (inFmt == QImage::Format_RGBA64)
            ? frame
            : frame.convertToFormat(QImage::Format_RGBA64);
    } else if (inFmt == QImage::Format_RGB888) {
        m_currentFrame = frame;
    } else if (bgraUploadEnabled && inFmt == QImage::Format_ARGB32_Premultiplied) {
        m_currentFrame = frame;
    } else {
        m_currentFrame = frame.convertToFormat(QImage::Format_RGBA8888);
    }
    if (m_currentFrame.isNull()) {
        qWarning() << "GLPreview: convertToFormat returned null";
        return;
    }
    if (m_displayAspectRatio <= 0.0 && m_currentFrame.height() > 0)
        m_displayAspectRatio = static_cast<double>(m_currentFrame.width()) / m_currentFrame.height();
    m_needsUpload = true;
    update();
}

void GLPreview::setDisplayAspectRatio(double aspectRatio)
{
    m_displayAspectRatio = (aspectRatio > 0.0) ? aspectRatio : 0.0;
    update();
}

void GLPreview::setColorCorrection(const ColorCorrection &cc)
{
    m_cc = cc;
    update();
}

void GLPreview::setHdrTransfer(int transfer)
{
    const int clamped = (transfer == 1 || transfer == 2) ? transfer : 0;
    if (m_hdrTransfer == clamped)
        return;
    m_hdrTransfer = clamped;
    update();
}

void GLPreview::setVideoEffects(const QVector<VideoEffect> &effects)
{
    m_videoEffects = effects;
    update();
}

void GLPreview::renderPendingD3D11Frame()
{
#if defined(Q_OS_WIN)
    if (!m_pendingD3D11Texture)
        return;

    void *d3d11Tex = m_pendingD3D11Texture;
    int subresource = m_pendingD3D11Subresource;
    int frameW = m_pendingD3D11Width;
    int frameH = m_pendingD3D11Height;
    // Always clear pending so a failed-to-register frame does not retry forever.
    m_pendingD3D11Texture = nullptr;

    if (!m_nv12Program || !ensureInteropDeviceForPaint())
        return;
    if (!gWglDXRegisterObjectNV || !gWglDXLockObjectsNV || !gWglDXUnlockObjectsNV)
        return;

    auto &cache = nv12Cache();
    CacheKey key{this, d3d11Tex, subresource};
    auto it = cache.find(key);
    if (it == cache.end()) {
        NV12RegisteredTex r;
        r.d3d11Tex = d3d11Tex;
        r.subresource = subresource;
        r.width = frameW;
        r.height = frameH;
        glGenTextures(1, &r.glTexY);
        glGenTextures(1, &r.glTexUV);
        // Allocate format-specific storage on the GL side BEFORE registering —
        // WGL_NV_DX_interop2 requires the GL texture's internal format to
        // match what the driver will expose. NV12 maps cleanly to Y as R8
        // (full WxH) and UV interleaved as RG8 (W/2 x H/2). Without
        // glTexImage2D the driver cannot resolve the plane mapping and
        // wglDXRegisterObjectNV either fails or returns aliased handles.
        glBindTexture(GL_TEXTURE_2D, r.glTexY);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frameW, frameH,
                     0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, r.glTexUV);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, frameW / 2, frameH / 2,
                     0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        r.hY = gWglDXRegisterObjectNV(static_cast<HANDLE>(m_interopDevice),
                                      d3d11Tex, r.glTexY,
                                      GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        if (!r.hY) {
            static bool warnedY = false;
            if (!warnedY) {
                qWarning() << "[interop] register Y plane failed for d3d11Tex=" << d3d11Tex
                           << "subres=" << subresource
                           << "— falling back to QImage path for this frame";
                warnedY = true;
            }
            glDeleteTextures(1, &r.glTexY);
            glDeleteTextures(1, &r.glTexUV);
            return;
        }
        r.hUV = gWglDXRegisterObjectNV(static_cast<HANDLE>(m_interopDevice),
                                       d3d11Tex, r.glTexUV,
                                       GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        if (!r.hUV) {
            static bool warnedUV = false;
            if (!warnedUV) {
                qWarning() << "[interop] register UV plane failed for d3d11Tex=" << d3d11Tex
                           << "subres=" << subresource
                           << "— falling back to QImage path for this frame";
                warnedUV = true;
            }
            if (gWglDXUnregisterObjectNV)
                gWglDXUnregisterObjectNV(static_cast<HANDLE>(m_interopDevice), r.hY);
            glDeleteTextures(1, &r.glTexY);
            glDeleteTextures(1, &r.glTexUV);
            return;
        }
        auto inserted = cache.emplace(key, r);
        it = inserted.first;
    }

    NV12RegisteredTex &r = it->second;

    HANDLE handles[2] = { r.hY, r.hUV };
    if (!gWglDXLockObjectsNV(static_cast<HANDLE>(m_interopDevice), 2, handles)) {
        static bool warnedLock = false;
        if (!warnedLock) {
            qWarning() << "[interop] wglDXLockObjectsNV failed — falling back this frame";
            warnedLock = true;
        }
        return;
    }

    // Letterbox the NV12 fast path the same way the legacy path does so
    // viewport math stays consistent with displayAspectRatio.
    const qreal dpr = devicePixelRatioF();
    const int physW = qMax(1, qRound(width() * dpr));
    const int physH = qMax(1, qRound(height() * dpr));
    const double frameAspect =
        (m_displayAspectRatio > 0.0 && std::isfinite(m_displayAspectRatio))
            ? m_displayAspectRatio
            : (r.height > 0 ? static_cast<double>(r.width) / r.height : 1.0);
    const double widgetAspect = physH > 0 ? static_cast<double>(physW) / physH : frameAspect;
    int viewportX = 0, viewportY = 0, viewportW = physW, viewportH = physH;
    if (frameAspect > 0.0 && widgetAspect > 0.0) {
        if (widgetAspect > frameAspect) {
            viewportW = qMax(1, qRound(physH * frameAspect));
            viewportX = (physW - viewportW) / 2;
        } else {
            viewportH = qMax(1, qRound(physW / frameAspect));
            viewportY = (physH - viewportH) / 2;
        }
    }
    glViewport(viewportX, viewportY, viewportW, viewportH);

    m_nv12Program->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r.glTexY);
    m_nv12Program->setUniformValue(m_locNv12TexY, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r.glTexUV);
    m_nv12Program->setUniformValue(m_locNv12TexUV, 1);
    m_vao.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();
    m_nv12Program->release();

    gWglDXUnlockObjectsNV(static_cast<HANDLE>(m_interopDevice), 2, handles);
#endif
}

void GLPreview::paintGL()
{
    static int paintCount = 0;
    if (++paintCount <= 5 || (paintCount % 100) == 0) {
        qInfo() << "GLPreview::paintGL #" << paintCount
                << "widget(logical)=" << width() << "x" << height()
                << "dpr=" << devicePixelRatioF()
                << "frame=" << m_currentFrame.width() << "x" << m_currentFrame.height()
                << "upload=" << m_needsUpload;
    }

    glClear(GL_COLOR_BUFFER_BIT);

#if defined(Q_OS_WIN)
    // Lazy-open the interop device the first paint after VideoPlayer hands
    // us a D3D11 device — eager open would race initializeGL.
    if (m_interopAvailable && m_pendingD3D11Device && !m_interopDevice)
        ensureInteropDeviceForPaint();

    if (m_pendingD3D11Texture && m_interopAvailable) {
        renderPendingD3D11Frame();
        return;
    }
#endif

    if (m_currentFrame.isNull()) return;

    // glViewport expects PHYSICAL pixels, but QWidget::width()/height() are
    // LOGICAL (device-independent) pixels. On a high-DPI display with DPR=1.5
    // or 2.0, using logical coordinates makes the video render in a fraction
    // of the widget — which is what the "small video in big panel" bug was.
    const qreal dpr = devicePixelRatioF();
    const int physW = qMax(1, qRound(width() * dpr));
    const int physH = qMax(1, qRound(height() * dpr));

    const double frameAspect =
        (m_displayAspectRatio > 0.0 && std::isfinite(m_displayAspectRatio))
            ? m_displayAspectRatio
            : ((m_currentFrame.height() > 0)
                   ? static_cast<double>(m_currentFrame.width()) / m_currentFrame.height()
                   : 1.0);
    const double widgetAspect =
        (physH > 0) ? static_cast<double>(physW) / physH : frameAspect;

    int viewportX = 0;
    int viewportY = 0;
    int viewportW = physW;
    int viewportH = physH;

    if (frameAspect > 0.0 && widgetAspect > 0.0) {
        if (widgetAspect > frameAspect) {
            viewportW = qMax(1, qRound(physH * frameAspect));
            viewportX = (physW - viewportW) / 2;
        } else {
            viewportH = qMax(1, qRound(physW / frameAspect));
            viewportY = (physH - viewportH) / 2;
        }
    }

    // US-T34 OBS-style source transform — shrink/move the viewport so the
    // same texture renders inside a translated+scaled sub-rect of the
    // letterbox. OpenGL's Y axis is bottom-up, so dy is inverted.
    // Skip the viewport transform when the multi-track compositor has
    // already baked per-clip scale/dx/dy into the canvas image. Without
    // this guard, the per-tick composite pass would either clobber the
    // user's drag state (if it called setVideoSourceTransform(1, 0, 0))
    // or apply the transform twice on top of the baked canvas.
    if (!m_compositeBakedMode
        && (m_videoSourceScale != 1.0 || m_videoSourceDx != 0.0 || m_videoSourceDy != 0.0)) {
        const int baseW = viewportW;
        const int baseH = viewportH;
        const int newW = qMax(1, qRound(baseW * m_videoSourceScale));
        const int newH = qMax(1, qRound(baseH * m_videoSourceScale));
        const int baseCx = viewportX + baseW / 2;
        const int baseCy = viewportY + baseH / 2;
        const int offsetPxX = qRound(m_videoSourceDx * baseW);
        const int offsetPxY = qRound(m_videoSourceDy * baseH);
        viewportX = baseCx + offsetPxX - newW / 2;
        viewportY = baseCy - offsetPxY - newH / 2;
        viewportW = newW;
        viewportH = newH;
    }

    // Upload texture if new frame.
    //
    // Re-use a single QOpenGLTexture across frames — allocating a new texture
    // per frame (~8 MB for 1080p RGBA) thrashes driver memory and has been
    // observed to crash Intel/AMD drivers after a few hundred frames.
    if (m_needsUpload) {
        const int fw = m_currentFrame.width();
        const int fh = m_currentFrame.height();
        if (fw <= 0 || fh <= 0) {
            m_needsUpload = false;
            return;
        }

        const QImage::Format frameFmt = m_currentFrame.format();
        const bool is16 = (frameFmt == QImage::Format_RGBA64
                           || frameFmt == QImage::Format_RGBA64_Premultiplied);
        const bool isRGB888 = (frameFmt == QImage::Format_RGB888);
        // Phase 1e Win #8: ARGB32_Premultiplied bytes are BGRA on
        // little-endian. Upload via GL_BGRA so the GL driver swizzles
        // into the RGBA8 internal format, skipping a CPU-side
        // convertToFormat(RGBA8888). Compositor output is always alpha=1.0
        // so premul vs straight produces identical visible pixels through
        // the existing fragment shader (texture().rgb sampling).
        const bool isARGB32Premul = (frameFmt == QImage::Format_ARGB32_Premultiplied);

        const bool sizeChanged = !m_texture
            || m_texture->width() != fw
            || m_texture->height() != fh
            || m_textureFormat != frameFmt;

        if (sizeChanged) {
            if (m_texture) {
                delete m_texture;
                m_texture = nullptr;
            }
            m_texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
            m_texture->setSize(fw, fh);
            QOpenGLTexture::TextureFormat texFormat;
            QOpenGLTexture::PixelFormat pixFormat;
            QOpenGLTexture::PixelType pixType;
            if (is16) {
                texFormat = QOpenGLTexture::RGBA16_UNorm;
                pixFormat = QOpenGLTexture::RGBA;
                pixType   = QOpenGLTexture::UInt16;
            } else if (isRGB888) {
                texFormat = QOpenGLTexture::RGB8_UNorm;
                pixFormat = QOpenGLTexture::RGB;
                pixType   = QOpenGLTexture::UInt8;
            } else if (isARGB32Premul) {
                texFormat = QOpenGLTexture::RGBA8_UNorm;
                pixFormat = QOpenGLTexture::BGRA;
                pixType   = QOpenGLTexture::UInt8;
            } else {
                texFormat = QOpenGLTexture::RGBA8_UNorm;
                pixFormat = QOpenGLTexture::RGBA;
                pixType   = QOpenGLTexture::UInt8;
            }
            m_texture->setFormat(texFormat);
            m_texture->setMinificationFilter(QOpenGLTexture::Linear);
            m_texture->setMagnificationFilter(QOpenGLTexture::Linear);
            m_texture->setWrapMode(QOpenGLTexture::ClampToEdge);
            m_texture->allocateStorage(pixFormat, pixType);
            m_textureFormat = frameFmt;
        }

        // QImage::Format_RGB888 has 3-byte pixels but bytesPerLine is rounded
        // up to a 4-byte boundary. Setting GL_UNPACK_ALIGNMENT=4 is fine for
        // both RGB888 (with stride padding) and RGBA8888 — Qt allocates
        // bytesPerLine to 4-byte alignment, so the default GL value works.
        const QOpenGLTexture::PixelFormat upPixFormat = is16
            ? QOpenGLTexture::RGBA
            : (isRGB888 ? QOpenGLTexture::RGB
                : (isARGB32Premul ? QOpenGLTexture::BGRA
                    : QOpenGLTexture::RGBA));
        const QOpenGLTexture::PixelType upPixType = is16
            ? QOpenGLTexture::UInt16
            : QOpenGLTexture::UInt8;
        m_texture->setData(0, 0, upPixFormat, upPixType,
                           static_cast<const void*>(m_currentFrame.constBits()));
        m_needsUpload = false;
    }

    if (!m_texture || !m_program) return;

    m_program->bind();
    m_texture->bind();
    glViewport(viewportX, viewportY, viewportW, viewportH);

    // Set uniforms
    m_program->setUniformValue(m_locTexture, 0);
    m_program->setUniformValue(m_locEffectsEnabled, m_effectsEnabled);
    m_program->setUniformValue(m_locHdrTransfer, m_hdrTransfer);

    // Seed every Fx uniform to off/zero; enable only those found in m_videoEffects.
    bool  fxBlur = false, fxNoise = false, fxSepia = false;
    bool  fxGray = false, fxInvert = false, fxVignette = false;
    float fxBlurR = 0.0f, fxNoiseA = 0.0f, fxSepiaS = 0.0f;
    float fxGrayS = 0.0f, fxInvertS = 0.0f;
    float fxVigI = 0.0f, fxVigR = 0.75f;
    for (const VideoEffect &e : m_videoEffects) {
        if (!e.enabled) continue;
        switch (e.type) {
        case VideoEffectType::Blur:
            fxBlur = true;
            fxBlurR = static_cast<float>(qBound(0.0, e.param1, 8.0));
            break;
        case VideoEffectType::Noise:
            fxNoise = true;
            fxNoiseA = static_cast<float>(qBound(0.0, e.param1, 100.0) / 100.0 * 0.4);
            break;
        case VideoEffectType::Sepia:
            fxSepia = true;
            fxSepiaS = static_cast<float>(qBound(0.0, e.param1, 1.0));
            break;
        case VideoEffectType::Grayscale:
            fxGray = true;
            fxGrayS = static_cast<float>(e.param1 > 0.0 ? qBound(0.0, e.param1, 1.0) : 1.0);
            break;
        case VideoEffectType::Invert:
            fxInvert = true;
            fxInvertS = static_cast<float>(e.param1 > 0.0 ? qBound(0.0, e.param1, 1.0) : 1.0);
            break;
        case VideoEffectType::Vignette:
            fxVignette = true;
            fxVigI = static_cast<float>(qBound(0.0, e.param1, 1.0));
            fxVigR = static_cast<float>(qBound(0.0, e.param2, 1.0));
            break;
        default:
            break; // Sharpen/Mosaic/ChromaKey stay on the CPU fallback path.
        }
    }
    m_program->setUniformValue(m_locFxBlurEnable, fxBlur);
    m_program->setUniformValue(m_locFxBlurRadius, fxBlurR);
    m_program->setUniformValue(m_locFxTexSize,
        QVector2D(m_texture ? static_cast<float>(m_texture->width())  : 1.0f,
                  m_texture ? static_cast<float>(m_texture->height()) : 1.0f));
    m_program->setUniformValue(m_locFxNoiseEnable, fxNoise);
    m_program->setUniformValue(m_locFxNoiseAmount, fxNoiseA);
    m_program->setUniformValue(m_locFxSepiaEnable, fxSepia);
    m_program->setUniformValue(m_locFxSepiaStrength, fxSepiaS);
    m_program->setUniformValue(m_locFxGrayEnable, fxGray);
    m_program->setUniformValue(m_locFxGrayStrength, fxGrayS);
    m_program->setUniformValue(m_locFxInvertEnable, fxInvert);
    m_program->setUniformValue(m_locFxInvertStrength, fxInvertS);
    m_program->setUniformValue(m_locFxVignetteEnable, fxVignette);
    m_program->setUniformValue(m_locFxVignetteIntensity, fxVigI);
    m_program->setUniformValue(m_locFxVignetteRadius, fxVigR);
    m_program->setUniformValue(m_locFxTime,
        static_cast<float>(QDateTime::currentMSecsSinceEpoch() % 1000000) / 1000.0f);
    m_program->setUniformValue(m_locBrightness,  static_cast<float>(m_cc.brightness));
    m_program->setUniformValue(m_locContrast,    static_cast<float>(m_cc.contrast));
    m_program->setUniformValue(m_locSaturation,  static_cast<float>(m_cc.saturation));
    m_program->setUniformValue(m_locHue,         static_cast<float>(m_cc.hue));
    m_program->setUniformValue(m_locTemperature, static_cast<float>(m_cc.temperature));
    m_program->setUniformValue(m_locTint,        static_cast<float>(m_cc.tint));
    m_program->setUniformValue(m_locGamma,       static_cast<float>(m_cc.gamma));
    m_program->setUniformValue(m_locHighlights,  static_cast<float>(m_cc.highlights));
    m_program->setUniformValue(m_locShadows,     static_cast<float>(m_cc.shadows));
    m_program->setUniformValue(m_locExposure,    static_cast<float>(m_cc.exposure));

    // Lift/Gamma/Gain
    m_program->setUniformValue(m_locLiftR,  static_cast<float>(m_cc.liftR));
    m_program->setUniformValue(m_locLiftG,  static_cast<float>(m_cc.liftG));
    m_program->setUniformValue(m_locLiftB,  static_cast<float>(m_cc.liftB));
    m_program->setUniformValue(m_locGammaR, static_cast<float>(m_cc.gammaR));
    m_program->setUniformValue(m_locGammaG, static_cast<float>(m_cc.gammaG));
    m_program->setUniformValue(m_locGammaB, static_cast<float>(m_cc.gammaB));
    m_program->setUniformValue(m_locGainR,  static_cast<float>(m_cc.gainR));
    m_program->setUniformValue(m_locGainG,  static_cast<float>(m_cc.gainG));
    m_program->setUniformValue(m_locGainB,  static_cast<float>(m_cc.gainB));

    // LUT
    m_program->setUniformValue(m_locLutEnabled, m_lutEnabled);
    m_program->setUniformValue(m_locLutIntensity, m_lutIntensity);
    if (m_lutEnabled && m_lutTexture) {
        glActiveTexture(GL_TEXTURE1);
        m_lutTexture->bind();
        m_program->setUniformValue(m_locLut3D, 1);
    }

    // Draw quad
    m_vao.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();

    if (m_lutEnabled && m_lutTexture) {
        glActiveTexture(GL_TEXTURE1);
        m_lutTexture->release();
        glActiveTexture(GL_TEXTURE0);
    }

    m_texture->release();
    m_program->release();
    glViewport(0, 0, physW, physH);

    // Adobe-style text tool overlay: draw the dashed marquee plus 8 resize
    // handles while the tool is active and a rect is present (either being
    // created/moved/resized this frame, or persisted from a previous drag).
    if (m_textToolActive && (m_textToolMode != TextToolIdle || m_textToolHasRect)) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // Match VideoPlayer::composeFrameWithOverlays so inline text edges
        // align with the committed overlay at the same font size.
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        QPen pen(QColor(255, 255, 255, 230));
        pen.setStyle(Qt::DashLine);
        pen.setWidth(2);
        painter.setPen(pen);
        painter.setBrush(QColor(255, 255, 255, 30));
        const QRect r = (m_textToolMode == TextToolCreating)
            ? QRect(m_textToolPressPos, m_textToolCurrentPos).normalized()
            : m_textToolRect;
        painter.drawRect(r);

        // Handles — only when we have a persisted rect (not while creating).
        if (m_textToolHasRect && m_textToolMode != TextToolCreating) {
            painter.setPen(QPen(QColor(255, 255, 255, 240), 1));
            painter.setBrush(QColor(40, 120, 220, 230));
            const int hs = 7; // handle size
            const QPoint corners[8] = {
                r.topLeft(),                                  // TL
                QPoint(r.center().x(), r.top()),              // T
                r.topRight(),                                 // TR
                QPoint(r.left(),  r.center().y()),            // L
                QPoint(r.right(), r.center().y()),            // R
                r.bottomLeft(),                               // BL
                QPoint(r.center().x(), r.bottom()),           // B
                r.bottomRight()                               // BR
            };
            for (const auto &c : corners)
                painter.drawRect(QRect(c.x() - hs / 2, c.y() - hs / 2, hs, hs));
        }

        // In-place text rendering (Adobe-style live edit): draw the text
        // being typed inside the rect, plus a blinking caret at the end of
        // the text. Only drawn in edit mode — the final commit goes through
        // MainWindow::applyTextToolOverlay via textInlineCommitted.
        if (m_textToolEditing && m_textToolHasRect) {
            // US-T32 WYSIWYG: draw at the literal configured pointSize in
            // widget coordinates. The committed path in
            // VideoPlayer::composeFrameWithOverlays inverse-scales by
            // imageH / letterboxH so both render at the same visible pt.
            QFont f = m_textToolStyleFont;
            if (f.pointSizeF() <= 0.0) f.setPointSizeF(32.0);
            painter.setFont(f);
            painter.setPen(m_textToolStyleColor);
            const int alignFlags = Qt::AlignCenter | Qt::TextWordWrap;
            painter.drawText(r, alignFlags, m_textToolInputText);
            if (m_textToolCaretVisible) {
                // Caret tracks the end of the last drawn glyph so it stays
                // attached to the centered text regardless of width.
                const QFontMetrics fm(f);
                const QString probe = m_textToolInputText.isEmpty()
                    ? QStringLiteral(" ")
                    : m_textToolInputText;
                const QRect textBounds = fm.boundingRect(r, alignFlags, probe);
                const int caretX = m_textToolInputText.isEmpty()
                    ? textBounds.left()
                    : textBounds.right() + 2;
                QPen caretPen(m_textToolStyleColor);
                caretPen.setWidth(2);
                painter.setPen(caretPen);
                painter.drawLine(caretX,
                                 qMax(r.top(),    textBounds.top()),
                                 caretX,
                                 qMin(r.bottom(), textBounds.bottom()));
            }
        }
    }

    // US-T36 Permanent 16:9 export canvas frame — drawn on every repaint as
    // a static visual reference so users understand what portion of their
    // composition ends up inside the exported video. Drawn BEFORE the video
    // transform selection so the selection handles render on top.
    {
        const QRectF canvasRect = canvasFrameRect();
        QPainter cpainter(this);
        cpainter.setRenderHint(QPainter::Antialiasing, true);
        QPen canvasPen(QColor(255, 200, 0, 220));
        canvasPen.setStyle(Qt::SolidLine);
        canvasPen.setWidth(2);
        cpainter.setPen(canvasPen);
        cpainter.setBrush(Qt::NoBrush);
        cpainter.drawRect(canvasRect);
    }

    // US-T34 OBS-style video transform selection overlay — drawn only when
    // the text tool is off and the user has selected the video source.
    if (!m_textToolActive && m_videoTransformSelected) {
        const QRectF vr = videoDisplayRect();
        if (vr.width() > 2.0 && vr.height() > 2.0) {
            QPainter vpainter(this);
            vpainter.setRenderHint(QPainter::Antialiasing, true);
            QPen outline(QColor(80, 200, 255, 230));
            outline.setWidth(2);
            vpainter.setPen(outline);
            vpainter.setBrush(Qt::NoBrush);
            vpainter.drawRect(vr);
            // 8 handles (4 corners + 4 edges)
            const double cx = vr.center().x();
            const double cy = vr.center().y();
            const QPointF pts[8] = {
                vr.topLeft(),    {cx, vr.top()},    vr.topRight(),
                {vr.left(), cy},                    {vr.right(), cy},
                vr.bottomLeft(), {cx, vr.bottom()}, vr.bottomRight()
            };
            const int size = 10;
            vpainter.setBrush(QColor(80, 200, 255, 230));
            QPen handlePen(QColor(255, 255, 255, 230));
            handlePen.setWidth(1);
            vpainter.setPen(handlePen);
            for (const QPointF &p : pts) {
                vpainter.drawRect(QRectF(p.x() - size / 2.0, p.y() - size / 2.0, size, size));
            }
        }
    }
}

QRectF GLPreview::letterboxRect() const
{
    const double W = qMax(1, width());
    const double H = qMax(1, height());
    if (m_displayAspectRatio <= 0.0 || !std::isfinite(m_displayAspectRatio))
        return QRectF(0, 0, W, H);
    const double widgetAspect = W / H;
    if (widgetAspect > m_displayAspectRatio) {
        // vertical bars on the sides
        const double w = H * m_displayAspectRatio;
        return QRectF((W - w) / 2.0, 0.0, w, H);
    }
    // horizontal bars on top/bottom
    const double h = W / m_displayAspectRatio;
    return QRectF(0.0, (H - h) / 2.0, W, h);
}

QRectF GLPreview::canvasFrameRect() const
{
    const double W = qMax(1, width());
    const double H = qMax(1, height());
    const double canvasAspect = 16.0 / 9.0;
    double cw = W;
    double ch = W / canvasAspect;
    if (ch > H) {
        ch = H;
        cw = H * canvasAspect;
    }
    return QRectF((W - cw) / 2.0, (H - ch) / 2.0, cw, ch);
}

void GLPreview::setVideoSourceTransform(double scale, double dx, double dy)
{
    m_videoSourceScale = qBound(0.1, scale, 10.0);
    m_videoSourceDx = qBound(-5.0, dx, 5.0);
    m_videoSourceDy = qBound(-5.0, dy, 5.0);
    update();
}

void GLPreview::resetVideoSourceTransform()
{
    m_videoSourceScale = 1.0;
    m_videoSourceDx = 0.0;
    m_videoSourceDy = 0.0;
    m_videoTransformSelected = false;
    m_videoDragMode = VideoDragNone;
    m_videoDragHandle = HandleNone;
    emit videoSourceTransformChanged(m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
    update();
}

void GLPreview::setVideoTransformSelected(bool selected)
{
    if (m_videoTransformSelected == selected)
        return;
    m_videoTransformSelected = selected;
    update();
}

void GLPreview::setCompositeBakedMode(bool enabled)
{
    if (m_compositeBakedMode == enabled) return;
    m_compositeBakedMode = enabled;
    update();
}

QRectF GLPreview::videoDisplayRectFor(double scale, double dx, double dy) const
{
    const QRectF lb = letterboxRect();
    const double w = lb.width() * scale;
    const double h = lb.height() * scale;
    const double cx = lb.x() + lb.width() / 2.0 + dx * lb.width();
    const double cy = lb.y() + lb.height() / 2.0 + dy * lb.height();
    return QRectF(cx - w / 2.0, cy - h / 2.0, w, h);
}

QRectF GLPreview::videoDisplayRect() const
{
    return videoDisplayRectFor(m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
}

GLPreview::TextToolHandle GLPreview::hitTestVideoHandle(const QPoint &pt) const
{
    const QRectF r = videoDisplayRect();
    if (r.isEmpty())
        return HandleNone;
    const int size = 14;
    auto hitBox = [&](double cx, double cy) {
        return QRectF(cx - size / 2.0, cy - size / 2.0, size, size).contains(pt);
    };
    const double cx = r.center().x();
    const double cy = r.center().y();
    const TextToolHandle ids[8] = {
        HandleTL, HandleT, HandleTR,
        HandleL,         HandleR,
        HandleBL, HandleB, HandleBR
    };
    const QPointF pts[8] = {
        r.topLeft(),     {cx, r.top()},    r.topRight(),
        {r.left(), cy},                    {r.right(), cy},
        r.bottomLeft(),  {cx, r.bottom()}, r.bottomRight()
    };
    for (int i = 0; i < 8; ++i) {
        if (hitBox(pts[i].x(), pts[i].y())) return ids[i];
    }
    return HandleNone;
}

bool GLPreview::pointInsideVideoRect(const QPoint &pt) const
{
    return videoDisplayRect().contains(pt);
}

void GLPreview::enterTextToolEditMode()
{
    m_textToolEditing = true;
    m_textToolInputText.clear();
    m_textToolCaretVisible = true;
    m_textToolCaretTimer.start();
    setFocus(Qt::OtherFocusReason);
    // Editing an existing overlay: tell compose to hide its committed
    // render so inline edit doesn't overlap. Fresh create-drag has index -1.
    if (m_textToolEditingIndex >= 0)
        emit textOverlayEditStarted(m_textToolEditingIndex);
    update();
}

void GLPreview::exitTextToolEditMode()
{
    const bool wasEditingExisting = (m_textToolEditingIndex >= 0);
    m_textToolEditing = false;
    m_textToolInputText.clear();
    m_textToolEditingIndex = -1;
    m_textToolCaretTimer.stop();
    if (wasEditingExisting)
        emit textOverlayEditEnded();
    update();
}

void GLPreview::setTextToolStyle(const QFont &font, const QColor &color)
{
    m_textToolStyleFont = font;
    m_textToolStyleColor = color;
    if (m_textToolEditing)
        update();
}

void GLPreview::setTextOverlayHitList(const QVector<TextOverlayHit> &hits)
{
    m_textToolOverlayHits = hits;
}

void GLPreview::commitCurrentTextToolEdit()
{
    if (!m_textToolEditing)
        return;
    // Capture state THEN exit edit mode BEFORE emitting. MainWindow's
    // applyTextToolOverlay slot re-checks isTextToolEditing() and calls
    // back into this function when true, so emitting first would recurse
    // through the signal chain until the stack overflows (reproduced by
    // pressing Enter on the preview text tool — the app crashes).
    const QString text = m_textToolInputText;
    const int editingIndex = m_textToolEditingIndex;
    const QRectF lb = letterboxRect();
    const double lbw = qMax(1.0, lb.width());
    const double lbh = qMax(1.0, lb.height());
    const QRectF normalized((m_textToolRect.x() - lb.x()) / lbw,
                            (m_textToolRect.y() - lb.y()) / lbh,
                            m_textToolRect.width()  / lbw,
                            m_textToolRect.height() / lbh);
    exitTextToolEditMode();
    if (text.isEmpty())
        return;
    if (editingIndex >= 0) {
        emit textOverlayEditCommitted(editingIndex, text);
    } else {
        emit textInlineCommitted(text, normalized);
    }
}

int GLPreview::hitTestExistingOverlay(const QPoint &widgetPos) const
{
    if (m_textToolOverlayHits.isEmpty())
        return -1;
    const QRectF lb = letterboxRect();
    // EnhancedTextOverlay stores IMAGE-normalized center + size; convert
    // to widget pixels via letterboxRect so the hit area lines up with the
    // actually-rendered text, not with the full widget.
    for (const auto &hit : m_textToolOverlayHits) {
        if (hit.normalizedRect.width() <= 0.0 || hit.normalizedRect.height() <= 0.0)
            continue;
        const double cx = hit.normalizedRect.x();
        const double cy = hit.normalizedRect.y();
        const double bw = hit.normalizedRect.width();
        const double bh = hit.normalizedRect.height();
        QRect r(static_cast<int>(lb.x() + (cx - bw / 2.0) * lb.width()),
                static_cast<int>(lb.y() + (cy - bh / 2.0) * lb.height()),
                static_cast<int>(bw * lb.width()),
                static_cast<int>(bh * lb.height()));
        if (r.contains(widgetPos))
            return hit.index;
    }
    return -1;
}

GLPreview::TextToolHandle GLPreview::hitTestTextToolHandle(const QPoint &pt) const
{
    if (!m_textToolHasRect)
        return HandleNone;
    const QRect &r = m_textToolRect;
    const int tol = 8;
    auto near = [tol](const QPoint &a, const QPoint &b) {
        return qAbs(a.x() - b.x()) <= tol && qAbs(a.y() - b.y()) <= tol;
    };
    const QPoint corners[8] = {
        r.topLeft(),
        QPoint(r.center().x(), r.top()),
        r.topRight(),
        QPoint(r.left(),  r.center().y()),
        QPoint(r.right(), r.center().y()),
        r.bottomLeft(),
        QPoint(r.center().x(), r.bottom()),
        r.bottomRight()
    };
    const TextToolHandle ids[8] = {
        HandleTL, HandleT, HandleTR,
        HandleL,  HandleR,
        HandleBL, HandleB, HandleBR
    };
    for (int i = 0; i < 8; ++i)
        if (near(pt, corners[i])) return ids[i];
    return HandleNone;
}

bool GLPreview::pointInsideTextToolRect(const QPoint &pt) const
{
    return m_textToolHasRect && m_textToolRect.contains(pt);
}

void GLPreview::emitCurrentTextToolRect()
{
    // Note: an 8x8 minimum is enforced in mouseReleaseEvent for the CREATE
    // path so accidental clicks don't generate ghost boxes. Move / resize
    // releases always re-emit (even if the user collapsed the rect) so
    // MainWindow's pending rect stays in sync with the visible geometry.
    const QRectF lb = letterboxRect();
    const double lbw = qMax(1.0, lb.width());
    const double lbh = qMax(1.0, lb.height());
    QRectF normalized((m_textToolRect.x() - lb.x()) / lbw,
                      (m_textToolRect.y() - lb.y()) / lbh,
                      m_textToolRect.width()  / lbw,
                      m_textToolRect.height() / lbh);
    emit textRectRequested(normalized);
}

void GLPreview::setTextToolActive(bool active)
{
    if (m_textToolActive == active)
        return;
    m_textToolActive = active;
    // Hover-cursor feedback in idle mode requires tracking mouse moves
    // without a pressed button. Focus policy allows keyPressEvent to fire
    // for in-place text editing after a drag-create.
    setMouseTracking(active);
    setFocusPolicy(active ? Qt::StrongFocus : Qt::NoFocus);
    if (active) {
        setCursor(Qt::IBeamCursor);
    } else {
        unsetCursor();
        m_textToolMode = TextToolIdle;
        m_textToolHasRect = false;
        exitTextToolEditMode();
    }
    update();
}

void GLPreview::clearTextToolRect()
{
    m_textToolHasRect = false;
    m_textToolMode = TextToolIdle;
    exitTextToolEditMode();
    update();
}

void GLPreview::mousePressEvent(QMouseEvent *event)
{
    // US-T34 Video source transform: only active when the text tool is OFF
    // so it doesn't clash with text creation / click-to-edit gestures.
    if (!m_textToolActive && event->button() == Qt::LeftButton) {
        const TextToolHandle hit = hitTestVideoHandle(event->pos());
        if (hit != HandleNone) {
            m_videoTransformSelected = true;
            m_videoDragMode = VideoDragResizing;
            m_videoDragHandle = hit;
            m_videoDragPressPos = event->pos();
            m_videoDragStartScale = m_videoSourceScale;
            m_videoDragStartDx = m_videoSourceDx;
            m_videoDragStartDy = m_videoSourceDy;
            update();
            event->accept();
            return;
        }
        if (pointInsideVideoRect(event->pos())) {
            m_videoTransformSelected = true;
            m_videoDragMode = VideoDragMoving;
            m_videoDragHandle = HandleNone;
            m_videoDragPressPos = event->pos();
            m_videoDragStartScale = m_videoSourceScale;
            m_videoDragStartDx = m_videoSourceDx;
            m_videoDragStartDy = m_videoSourceDy;
            update();
            event->accept();
            return;
        }
        if (m_videoTransformSelected) {
            m_videoTransformSelected = false;
            update();
        }
    }
    if (!m_textToolActive || event->button() != Qt::LeftButton) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }
    m_textToolPressPos = event->pos();
    m_textToolCurrentPos = event->pos();

    // Probe handle/body BEFORE committing so dragging a handle while editing
    // reshapes in place (Premiere-style). Only an outside click commits.
    const TextToolHandle hit = hitTestTextToolHandle(event->pos());
    const bool insideRect = pointInsideTextToolRect(event->pos());
    if (m_textToolEditing && hit == HandleNone && !insideRect) {
        commitCurrentTextToolEdit();
    } else if (!m_textToolEditing && m_textToolHasRect
               && hit == HandleNone && !insideRect
               && hitTestExistingOverlay(event->pos()) < 0) {
        // OBS-style deselect: outside click with a selection but no active
        // edit and no new overlay under cursor → clear the selection.
        m_textToolHasRect = false;
        m_textToolEditingIndex = -1;
        m_textToolMode = TextToolIdle;
        update();
    }

    if (hit != HandleNone) {
        m_textToolMode = TextToolResizing;
        m_textToolActiveHandle = hit;
        m_textToolDragStartRect = m_textToolRect;
    } else if (insideRect) {
        m_textToolMode = TextToolMoving;
        m_textToolActiveHandle = HandleNone;
        m_textToolDragStartRect = m_textToolRect;
    } else {
        // Adobe click-to-edit: if the click lands on an already-rendered
        // overlay, enter edit mode for THAT overlay instead of starting a
        // new create-drag. Enter commits via textOverlayEditCommitted so
        // MainWindow can update the existing overlay in place.
        const int existingIdx = hitTestExistingOverlay(event->pos());
        if (existingIdx >= 0) {
            const TextOverlayHit *hitInfo = nullptr;
            for (const auto &h : m_textToolOverlayHits) {
                if (h.index == existingIdx) { hitInfo = &h; break; }
            }
            if (hitInfo) {
                // OBS-style: single click on an existing overlay immediately
                // enters a drag mode (move or resize depending on whether the
                // press lands on a handle). We do NOT call enterTextToolEditMode
                // here — that's reserved for mouseDoubleClickEvent — so the user
                // gets press-and-drag to reshape without an in-place caret.
                const QRectF lb = letterboxRect();
                const QRectF &nr = hitInfo->normalizedRect;
                const int rw = static_cast<int>(nr.width()  * lb.width());
                const int rh = static_cast<int>(nr.height() * lb.height());
                const int rx = static_cast<int>(lb.x() + (nr.x() - nr.width()  / 2.0) * lb.width());
                const int ry = static_cast<int>(lb.y() + (nr.y() - nr.height() / 2.0) * lb.height());
                m_textToolRect = QRect(rx, ry, rw, rh);
                m_textToolHasRect = true;
                m_textToolEditingIndex = existingIdx;
                // Re-hit with the freshly-set rect so handle press enters
                // Resizing directly instead of always falling into Moving.
                const TextToolHandle freshHit = hitTestTextToolHandle(event->pos());
                if (freshHit != HandleNone) {
                    m_textToolMode = TextToolResizing;
                    m_textToolActiveHandle = freshHit;
                } else {
                    m_textToolMode = TextToolMoving;
                    m_textToolActiveHandle = HandleNone;
                }
                m_textToolDragStartRect = m_textToolRect;
                update();
                event->accept();
                return;
            }
        }
        m_textToolMode = TextToolCreating;
        m_textToolHasRect = false;
        m_textToolActiveHandle = HandleNone;
        m_textToolEditingIndex = -1;
    }
    update();
    event->accept();
}

void GLPreview::mouseMoveEvent(QMouseEvent *event)
{
    // US-T34 video transform drag / hover cursor
    if (!m_textToolActive) {
        if (m_videoDragMode != VideoDragNone) {
            const QPoint delta = event->pos() - m_videoDragPressPos;
            const QRectF lb = letterboxRect();
            const double lbw = qMax(1.0, lb.width());
            const double lbh = qMax(1.0, lb.height());
            if (m_videoDragMode == VideoDragMoving) {
                m_videoSourceDx = m_videoDragStartDx + delta.x() / lbw;
                m_videoSourceDy = m_videoDragStartDy + delta.y() / lbh;
            } else if (m_videoDragMode == VideoDragResizing) {
                // Uniform scale with opposite-corner anchoring. For corner
                // handles the anchor is the opposite corner; for side handles
                // the anchor is the opposite side's midpoint. Scale comes from
                // the mouse's absolute offset from the anchor along whichever
                // axis the handle drives.
                const QRectF startRect = videoDisplayRectFor(
                    m_videoDragStartScale, m_videoDragStartDx, m_videoDragStartDy);
                QPointF anchor = startRect.center();
                bool driveX = false, driveY = false;
                switch (m_videoDragHandle) {
                    case HandleTL: anchor = startRect.bottomRight(); driveX = driveY = true; break;
                    case HandleTR: anchor = startRect.bottomLeft();  driveX = driveY = true; break;
                    case HandleBL: anchor = startRect.topRight();    driveX = driveY = true; break;
                    case HandleBR: anchor = startRect.topLeft();     driveX = driveY = true; break;
                    case HandleT:  anchor = QPointF(startRect.center().x(), startRect.bottom()); driveY = true; break;
                    case HandleB:  anchor = QPointF(startRect.center().x(), startRect.top());    driveY = true; break;
                    case HandleL:  anchor = QPointF(startRect.right(),      startRect.center().y()); driveX = true; break;
                    case HandleR:  anchor = QPointF(startRect.left(),       startRect.center().y()); driveX = true; break;
                    default: break;
                }
                const double mouseDX = qAbs(event->pos().x() - anchor.x());
                const double mouseDY = qAbs(event->pos().y() - anchor.y());
                const double scaleFromX = mouseDX / lbw;
                const double scaleFromY = mouseDY / lbh;
                double newScale = m_videoDragStartScale;
                if (driveX && driveY) newScale = qMax(scaleFromX, scaleFromY);
                else if (driveX)      newScale = scaleFromX;
                else if (driveY)      newScale = scaleFromY;
                newScale = qBound(0.1, newScale, 10.0);
                const double newWpx = lbw * newScale;
                const double newHpx = lbh * newScale;
                QPointF newCenter = anchor;
                switch (m_videoDragHandle) {
                    case HandleTL: newCenter = QPointF(anchor.x() - newWpx / 2.0, anchor.y() - newHpx / 2.0); break;
                    case HandleTR: newCenter = QPointF(anchor.x() + newWpx / 2.0, anchor.y() - newHpx / 2.0); break;
                    case HandleBL: newCenter = QPointF(anchor.x() - newWpx / 2.0, anchor.y() + newHpx / 2.0); break;
                    case HandleBR: newCenter = QPointF(anchor.x() + newWpx / 2.0, anchor.y() + newHpx / 2.0); break;
                    case HandleT:  newCenter = QPointF(startRect.center().x(), anchor.y() - newHpx / 2.0); break;
                    case HandleB:  newCenter = QPointF(startRect.center().x(), anchor.y() + newHpx / 2.0); break;
                    case HandleL:  newCenter = QPointF(anchor.x() - newWpx / 2.0, startRect.center().y()); break;
                    case HandleR:  newCenter = QPointF(anchor.x() + newWpx / 2.0, startRect.center().y()); break;
                    default: break;
                }
                const QPointF lbCenter = lb.center();
                m_videoSourceScale = newScale;
                m_videoSourceDx = qBound(-5.0, (newCenter.x() - lbCenter.x()) / lbw, 5.0);
                m_videoSourceDy = qBound(-5.0, (newCenter.y() - lbCenter.y()) / lbh, 5.0);
            }
            // US-T37 OBS-style snap: if any edge of the transformed video
            // rect lands within a pixel tolerance of the 16:9 canvas frame
            // edge, pull it flush. We first adjust dx/dy so the nearest
            // edge matches; the scale has already been committed above.
            if (m_snapStrength > 0.0) {
                const QRectF canvas = canvasFrameRect();
                const QRectF cur = videoDisplayRectFor(
                    m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
                const double snapPx = m_snapStrength;
                double shiftX = 0.0;
                double shiftY = 0.0;
                if (qAbs(cur.left()   - canvas.left())   < snapPx) shiftX = canvas.left()   - cur.left();
                else if (qAbs(cur.right()  - canvas.right())  < snapPx) shiftX = canvas.right()  - cur.right();
                else if (qAbs(cur.center().x() - canvas.center().x()) < snapPx)
                    shiftX = canvas.center().x() - cur.center().x();
                if (qAbs(cur.top()    - canvas.top())    < snapPx) shiftY = canvas.top()    - cur.top();
                else if (qAbs(cur.bottom() - canvas.bottom()) < snapPx) shiftY = canvas.bottom() - cur.bottom();
                else if (qAbs(cur.center().y() - canvas.center().y()) < snapPx)
                    shiftY = canvas.center().y() - cur.center().y();
                if (shiftX != 0.0 || shiftY != 0.0) {
                    m_videoSourceDx = qBound(-5.0, m_videoSourceDx + shiftX / lbw, 5.0);
                    m_videoSourceDy = qBound(-5.0, m_videoSourceDy + shiftY / lbh, 5.0);
                }
            }
            emit videoSourceTransformChanged(m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
            update();
            event->accept();
            return;
        }
        // Hover cursor for idle video transform selection.
        if (m_videoTransformSelected) {
            const TextToolHandle hit = hitTestVideoHandle(event->pos());
            if (hit != HandleNone) setCursor(Qt::SizeFDiagCursor);
            else if (pointInsideVideoRect(event->pos())) setCursor(Qt::SizeAllCursor);
            else setCursor(Qt::ArrowCursor);
        }
        QOpenGLWidget::mouseMoveEvent(event);
        return;
    }

    // Hover cursor feedback when idle. Handles take precedence so the user
    // can grab a corner while typing; inside-rect shows I-beam during edit.
    if (m_textToolMode == TextToolIdle) {
        const TextToolHandle hit = hitTestTextToolHandle(event->pos());
        if (hit == HandleTL || hit == HandleBR)        setCursor(Qt::SizeFDiagCursor);
        else if (hit == HandleTR || hit == HandleBL)   setCursor(Qt::SizeBDiagCursor);
        else if (hit == HandleT  || hit == HandleB)    setCursor(Qt::SizeVerCursor);
        else if (hit == HandleL  || hit == HandleR)    setCursor(Qt::SizeHorCursor);
        else if (pointInsideTextToolRect(event->pos()))
            setCursor(m_textToolEditing ? Qt::IBeamCursor : Qt::SizeAllCursor);
        else                                           setCursor(Qt::IBeamCursor);
        return;
    }

    m_textToolCurrentPos = event->pos();
    if (m_textToolMode == TextToolCreating) {
        // rect is computed directly in paintGL from press/current
    } else if (m_textToolMode == TextToolMoving) {
        const QPoint delta = event->pos() - m_textToolPressPos;
        m_textToolRect = m_textToolDragStartRect.translated(delta);
    } else if (m_textToolMode == TextToolResizing) {
        QRect r = m_textToolDragStartRect;
        const QPoint p = event->pos();
        switch (m_textToolActiveHandle) {
            case HandleTL: r.setTopLeft(p); break;
            case HandleT:  r.setTop(p.y()); break;
            case HandleTR: r.setTopRight(p); break;
            case HandleL:  r.setLeft(p.x()); break;
            case HandleR:  r.setRight(p.x()); break;
            case HandleBL: r.setBottomLeft(p); break;
            case HandleB:  r.setBottom(p.y()); break;
            case HandleBR: r.setBottomRight(p); break;
            default: break;
        }
        m_textToolRect = r.normalized();
    }
    update();
    event->accept();
}

void GLPreview::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_textToolActive && m_videoDragMode != VideoDragNone) {
        m_videoDragMode = VideoDragNone;
        m_videoDragHandle = HandleNone;
        emit videoSourceTransformChanged(m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
        update();
        event->accept();
        return;
    }
    if (!m_textToolActive || event->button() != Qt::LeftButton
        || m_textToolMode == TextToolIdle) {
        QOpenGLWidget::mouseReleaseEvent(event);
        return;
    }
    const TextToolInteraction mode = m_textToolMode;
    m_textToolMode = TextToolIdle;
    m_textToolActiveHandle = HandleNone;
    event->accept();

    if (mode == TextToolCreating) {
        const QRect rawRect = QRect(m_textToolPressPos, event->pos()).normalized();
        // 8x8 minimum prevents accidental click-and-release from creating
        // an invisible text box.
        if (rawRect.width() < 8 || rawRect.height() < 8) {
            m_textToolHasRect = false;
            exitTextToolEditMode();
            update();
            return;
        }
        m_textToolRect = rawRect;
        m_textToolHasRect = true;
        // Adobe-style in-place editing: after a successful create-drag,
        // the widget enters edit mode and a blinking caret appears inside
        // the new rect so the user can immediately start typing.
        enterTextToolEditMode();
    }
    update();
    // Existing-overlay move/resize emits textOverlayRectChanged so MainWindow
    // rewrites the rect in place; everything else takes the generic path.
    if ((mode == TextToolMoving || mode == TextToolResizing) && m_textToolEditingIndex >= 0) {
        const QRectF lb = letterboxRect();
        const double lbw = qMax(1.0, lb.width());
        const double lbh = qMax(1.0, lb.height());
        const double nx = ((m_textToolRect.x() + m_textToolRect.width()  / 2.0) - lb.x()) / lbw;
        const double ny = ((m_textToolRect.y() + m_textToolRect.height() / 2.0) - lb.y()) / lbh;
        const double nw = m_textToolRect.width()  / lbw;
        const double nh = m_textToolRect.height() / lbh;
        emit textOverlayRectChanged(m_textToolEditingIndex, QRectF(nx, ny, nw, nh));
    } else {
        emitCurrentTextToolRect();
    }
}

void GLPreview::mouseDoubleClickEvent(QMouseEvent *event)
{
    // OBS-style edit mode: second click on an existing overlay enters inline
    // text editing with caret. Single click only selects (see mousePressEvent).
    if (!m_textToolActive || event->button() != Qt::LeftButton) {
        QOpenGLWidget::mouseDoubleClickEvent(event);
        return;
    }
    // Use the rect populated by the preceding single-click (or fall back to
    // a fresh overlay hit-test if the user somehow double-clicked without a
    // prior single-click landing on the same overlay).
    int idx = m_textToolEditingIndex;
    if (idx < 0 || !m_textToolHasRect || !m_textToolRect.contains(event->pos()))
        idx = hitTestExistingOverlay(event->pos());
    if (idx < 0) {
        QOpenGLWidget::mouseDoubleClickEvent(event);
        return;
    }
    QString prefill;
    for (const auto &h : m_textToolOverlayHits) {
        if (h.index == idx) { prefill = h.text; break; }
    }
    m_textToolEditingIndex = idx;
    enterTextToolEditMode();
    m_textToolInputText = prefill;
    update();
    event->accept();
}

void GLPreview::keyPressEvent(QKeyEvent *event)
{
    if (!m_textToolActive || !m_textToolEditing) {
        if (m_textToolActive && m_textToolHasRect
            && event->key() == Qt::Key_Escape) {
            // Deselect in OBS-style: Escape clears selection when not editing.
            m_textToolHasRect = false;
            m_textToolEditingIndex = -1;
            m_textToolMode = TextToolIdle;
            update();
            event->accept();
            return;
        }
        // US-T34 Video transform shortcuts (only when text tool off).
        if (!m_textToolActive && m_videoTransformSelected) {
            if (event->key() == Qt::Key_Escape) {
                m_videoTransformSelected = false;
                update();
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_0 || event->key() == Qt::Key_Delete) {
                resetVideoSourceTransform();
                event->accept();
                return;
            }
        }
        QOpenGLWidget::keyPressEvent(event);
        return;
    }
    const int key = event->key();
    if (key == Qt::Key_Escape) {
        exitTextToolEditMode();
        event->accept();
        return;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        // Delegate to the single commit path: it exits edit mode first
        // then emits the signal, breaking the applyTextToolOverlay recursion
        // that caused Enter-press crashes.
        commitCurrentTextToolEdit();
        event->accept();
        return;
    }
    if (key == Qt::Key_Backspace) {
        if (!m_textToolInputText.isEmpty())
            m_textToolInputText.chop(1);
        m_textToolCaretVisible = true;
        update();
        event->accept();
        return;
    }
    const QString typed = event->text();
    if (!typed.isEmpty() && typed.at(0).isPrint()) {
        m_textToolInputText.append(typed);
        m_textToolCaretVisible = true;
        update();
        event->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void GLPreview::focusOutEvent(QFocusEvent *event)
{
    // Committing here would be surprising (the user might just be clicking
    // a different widget). Silently keep the text but freeze the caret;
    // they can re-focus and continue typing.
    m_textToolCaretVisible = false;
    m_textToolCaretTimer.stop();
    update();
    QOpenGLWidget::focusOutEvent(event);
}

void GLPreview::setLut(const LutData &lut)
{
    if (!lut.isValid()) {
        clearLut();
        return;
    }

    makeCurrent();

    if (m_lutTexture) {
        delete m_lutTexture;
        m_lutTexture = nullptr;
    }

    // Create 3D texture from LUT table
    m_lutTexture = new QOpenGLTexture(QOpenGLTexture::Target3D);
    m_lutTexture->setSize(lut.size, lut.size, lut.size);
    m_lutTexture->setFormat(QOpenGLTexture::RGB32F);
    m_lutTexture->allocateStorage();
    m_lutTexture->setMinificationFilter(QOpenGLTexture::Linear);
    m_lutTexture->setMagnificationFilter(QOpenGLTexture::Linear);
    m_lutTexture->setWrapMode(QOpenGLTexture::ClampToEdge);

    // Upload LUT data as float RGB
    QVector<float> data;
    data.reserve(lut.table.size() * 3);
    for (const QVector3D &v : lut.table) {
        data.append(v.x());
        data.append(v.y());
        data.append(v.z());
    }
    m_lutTexture->setData(QOpenGLTexture::RGB, QOpenGLTexture::Float32,
                          data.constData());

    m_lutIntensity = static_cast<float>(lut.intensity);
    m_lutEnabled = true;

    doneCurrent();
    update();
}

void GLPreview::clearLut()
{
    makeCurrent();
    if (m_lutTexture) {
        delete m_lutTexture;
        m_lutTexture = nullptr;
    }
    m_lutEnabled = false;
    m_lutIntensity = 1.0f;
    doneCurrent();
    update();
}
