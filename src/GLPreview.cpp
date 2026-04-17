#include "GLPreview.h"
#include <algorithm>
#include <cmath>
#include <QtGlobal>
#include <QDateTime>
#include <QHash>
#include <QVector2D>
#include <QOpenGLContext>
#include <QDebug>
#include <QPainter>
#include <QPen>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QFontMetrics>

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

// Stage 2 PiP — per-layer opacity multiplied into alpha for straight-alpha
// blend (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA). 1.0 preserves legacy output.
uniform float uOpacity;

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

    FragColor = vec4(clamp(color, 0.0, 1.0), texColor.a * uOpacity);
}
)";

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
        for (auto &layer : m_layers) layer.texture = nullptr;
        m_layers.clear();
        m_pendingTextureDeletes.clear();
        m_program = nullptr;
    }
}

void GLPreview::cleanupGL()
{
    if (!context())
        return;

    makeCurrent();
    if (m_lutTexture) {
        delete m_lutTexture;
        m_lutTexture = nullptr;
    }
    for (auto *t : m_pendingTextureDeletes) {
        delete t;
    }
    m_pendingTextureDeletes.clear();
    for (auto &layer : m_layers) {
        if (layer.texture) {
            delete layer.texture;
            layer.texture = nullptr;
        }
    }
    m_layers.clear();
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
    m_locOpacity             = m_program->uniformLocation("uOpacity");

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
    // Stage 2 PiP — legacy single-frame entry reuses the multi-layer path.
    // The OBS-style live transform (m_videoSource*) is treated as the
    // foreground layer's transform so existing behavior is identical when
    // only one layer is in use.
    LayerFrame lf;
    lf.image       = frame;
    lf.scale       = m_videoSourceScale;
    lf.dx          = m_videoSourceDx;
    lf.dy          = m_videoSourceDy;
    lf.opacity     = 1.0;
    lf.sourceTrack = 0;
    lf.aspectRatio = (m_displayAspectRatio > 0.0) ? m_displayAspectRatio : 0.0;
    QVector<LayerFrame> v;
    v.append(lf);
    setLayers(v);
}

void GLPreview::setLayers(const QVector<LayerFrame> &frames)
{
    // Reuse existing GLLayer textures keyed by sourceTrack so we don't
    // thrash GPU allocations between frames. Un-matched textures are
    // deferred for deletion until the next paintGL when the GL context
    // is guaranteed current.
    QHash<int, int> existingBySource;
    existingBySource.reserve(m_layers.size());
    for (int i = 0; i < m_layers.size(); ++i)
        existingBySource.insert(m_layers[i].sourceTrack, i);

    QVector<bool> reused(m_layers.size(), false);
    QVector<GLLayer> newLayers;
    newLayers.reserve(frames.size());

    for (const LayerFrame &lf : frames) {
        if (lf.image.isNull()) continue;
        GLLayer layer;
        const bool is16 = (lf.image.format() == QImage::Format_RGBA64
                           || lf.image.format() == QImage::Format_RGBA64_Premultiplied);
        layer.image = lf.image.convertToFormat(is16 ? QImage::Format_RGBA64
                                                   : QImage::Format_RGBA8888);
        if (layer.image.isNull()) {
            qWarning() << "GLPreview::setLayers: convertToFormat returned null";
            continue;
        }
        layer.needsUpload = true;
        layer.scale       = lf.scale;
        layer.dx          = lf.dx;
        layer.dy          = lf.dy;
        layer.opacity     = qBound(0.0, lf.opacity, 1.0);
        layer.sourceTrack = lf.sourceTrack;
        layer.aspectRatio = (lf.aspectRatio > 0.0 && std::isfinite(lf.aspectRatio))
            ? lf.aspectRatio
            : (layer.image.height() > 0
                   ? static_cast<double>(layer.image.width()) / layer.image.height()
                   : 1.0);

        auto it = existingBySource.find(lf.sourceTrack);
        if (it != existingBySource.end()) {
            const int idx = it.value();
            layer.texture       = m_layers[idx].texture;
            layer.textureFormat = m_layers[idx].textureFormat;
            m_layers[idx].texture = nullptr;
            reused[idx] = true;
        }

        newLayers.append(std::move(layer));
    }

    for (int i = 0; i < m_layers.size(); ++i) {
        if (!reused[i] && m_layers[i].texture) {
            m_pendingTextureDeletes.append(m_layers[i].texture);
            m_layers[i].texture = nullptr;
        }
    }

    m_layers = std::move(newLayers);

    // Keep the legacy single-aspect member in sync with the first layer so
    // letterboxRect() and canvasFrameRect() callers see a consistent value.
    if (!m_layers.isEmpty() && m_layers.first().aspectRatio > 0.0)
        m_displayAspectRatio = m_layers.first().aspectRatio;

    update();
}

void GLPreview::setActiveTransformLayer(int sourceTrack)
{
    if (m_activeTransformLayer == sourceTrack) return;
    m_activeTransformLayer = sourceTrack;
    update();
}

int GLPreview::activeTrackForEmit() const
{
    if (m_activeTransformLayer >= 0) return m_activeTransformLayer;
    if (m_layers.isEmpty()) return 0;
    int highest = m_layers.first().sourceTrack;
    for (const auto &l : m_layers)
        if (l.sourceTrack > highest) highest = l.sourceTrack;
    return highest;
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

void GLPreview::paintGL()
{
    static int paintCount = 0;
    if (++paintCount <= 5 || (paintCount % 100) == 0) {
        qInfo() << "GLPreview::paintGL #" << paintCount
                << "widget(logical)=" << width() << "x" << height()
                << "dpr=" << devicePixelRatioF()
                << "layers=" << m_layers.size();
    }

    glClear(GL_COLOR_BUFFER_BIT);

    // Drain any textures retired by setLayers. Only safe now that the GL
    // context is current — setLayers() can be called from any thread that
    // ultimately owns the decoded frames.
    if (!m_pendingTextureDeletes.isEmpty()) {
        for (auto *t : m_pendingTextureDeletes) delete t;
        m_pendingTextureDeletes.clear();
    }

    if (m_layers.isEmpty() || !m_program) return;

    // glViewport expects PHYSICAL pixels, but QWidget::width()/height() are
    // LOGICAL (device-independent) pixels. On a high-DPI display with DPR=1.5
    // or 2.0, using logical coordinates makes the video render in a fraction
    // of the widget — which is what the "small video in big panel" bug was.
    const qreal dpr = devicePixelRatioF();
    const int physW = qMax(1, qRound(width() * dpr));
    const int physH = qMax(1, qRound(height() * dpr));
    const double widgetAspect =
        (physH > 0) ? static_cast<double>(physW) / physH : 1.0;

    // Stage 2 PiP — sort by sourceTrack DESC so V2+ (behind) draws first and
    // V1 (sourceTrack=0, upper track in UI) draws last on top as the PiP
    // overlay. Matches Premiere/DaVinci/Final Cut conventions where the
    // UPPER track is the FOREGROUND. The user scales V1 via the OBS drag
    // handles to reveal V2 behind.
    QVector<int> drawOrder;
    drawOrder.reserve(m_layers.size());
    for (int i = 0; i < m_layers.size(); ++i) drawOrder.append(i);
    std::sort(drawOrder.begin(), drawOrder.end(), [this](int a, int b) {
        return m_layers[a].sourceTrack > m_layers[b].sourceTrack;
    });

    // Which layer receives the live OBS-drag transform? -1 means "highest
    // sourceTrack present" so PiP overlays are targeted by default. For the
    // legacy single-layer path this resolves to that sole layer.
    int activeTrack = m_activeTransformLayer;
    if (activeTrack < 0 || m_layers.size() == 1) {
        activeTrack = m_layers.first().sourceTrack;
        for (const auto &l : m_layers)
            if (l.sourceTrack > activeTrack) activeTrack = l.sourceTrack;
    }

    m_program->bind();

    // Project-scope uniforms (color correction / effects / LUT / HDR) apply
    // uniformly to every layer in the MVP. Stage 3 will make effects and
    // color grading per-layer; for now they carry the V1 meaning.
    m_program->setUniformValue(m_locTexture, 0);
    m_program->setUniformValue(m_locEffectsEnabled, m_effectsEnabled);
    m_program->setUniformValue(m_locHdrTransfer, m_hdrTransfer);

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
            break;
        }
    }
    m_program->setUniformValue(m_locFxBlurEnable, fxBlur);
    m_program->setUniformValue(m_locFxBlurRadius, fxBlurR);
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
    m_program->setUniformValue(m_locLiftR,  static_cast<float>(m_cc.liftR));
    m_program->setUniformValue(m_locLiftG,  static_cast<float>(m_cc.liftG));
    m_program->setUniformValue(m_locLiftB,  static_cast<float>(m_cc.liftB));
    m_program->setUniformValue(m_locGammaR, static_cast<float>(m_cc.gammaR));
    m_program->setUniformValue(m_locGammaG, static_cast<float>(m_cc.gammaG));
    m_program->setUniformValue(m_locGammaB, static_cast<float>(m_cc.gammaB));
    m_program->setUniformValue(m_locGainR,  static_cast<float>(m_cc.gainR));
    m_program->setUniformValue(m_locGainG,  static_cast<float>(m_cc.gainG));
    m_program->setUniformValue(m_locGainB,  static_cast<float>(m_cc.gainB));
    m_program->setUniformValue(m_locLutEnabled, m_lutEnabled);
    m_program->setUniformValue(m_locLutIntensity, m_lutIntensity);
    if (m_lutEnabled && m_lutTexture) {
        glActiveTexture(GL_TEXTURE1);
        m_lutTexture->bind();
        m_program->setUniformValue(m_locLut3D, 1);
        glActiveTexture(GL_TEXTURE0);
    }

    // Straight-alpha blending for multi-layer compositing. Qt converts frames
    // to Format_RGBA8888 / Format_RGBA64 (non-premultiplied) in setLayers, so
    // this matches.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_vao.bind();
    for (int idx : drawOrder) {
        GLLayer &layer = m_layers[idx];

        // Per-layer letterbox viewport based on this layer's own aspect.
        const double frameAspect = (layer.aspectRatio > 0.0 && std::isfinite(layer.aspectRatio))
            ? layer.aspectRatio
            : (layer.image.height() > 0
                   ? static_cast<double>(layer.image.width()) / layer.image.height()
                   : 1.0);
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

        // The layer whose sourceTrack matches the active transform slot
        // reads live m_videoSource* so OBS-drag updates feel frame-accurate
        // without waiting for the next decoded frame. Other layers use
        // whatever the caller stored in the LayerFrame.
        const bool isActive = (layer.sourceTrack == activeTrack);
        const double layerScale = isActive ? m_videoSourceScale : layer.scale;
        const double layerDx    = isActive ? m_videoSourceDx    : layer.dx;
        const double layerDy    = isActive ? m_videoSourceDy    : layer.dy;
        if (layerScale != 1.0 || layerDx != 0.0 || layerDy != 0.0) {
            const int baseW = viewportW;
            const int baseH = viewportH;
            const int newW = qMax(1, qRound(baseW * layerScale));
            const int newH = qMax(1, qRound(baseH * layerScale));
            const int baseCx = viewportX + baseW / 2;
            const int baseCy = viewportY + baseH / 2;
            const int offsetPxX = qRound(layerDx * baseW);
            const int offsetPxY = qRound(layerDy * baseH);
            viewportX = baseCx + offsetPxX - newW / 2;
            viewportY = baseCy - offsetPxY - newH / 2;
            viewportW = newW;
            viewportH = newH;
        }

        // (Re)create or update this layer's texture. Reuse when size+format
        // are unchanged so we don't thrash GPU memory on every frame.
        if (layer.needsUpload && !layer.image.isNull()) {
            const int fw = layer.image.width();
            const int fh = layer.image.height();
            if (fw <= 0 || fh <= 0) {
                layer.needsUpload = false;
                continue;
            }
            const QImage::Format fmt = layer.image.format();
            const bool is16 = (fmt == QImage::Format_RGBA64
                               || fmt == QImage::Format_RGBA64_Premultiplied);
            const bool sizeChanged = !layer.texture
                || layer.texture->width() != fw
                || layer.texture->height() != fh
                || layer.textureFormat != fmt;
            if (sizeChanged) {
                if (layer.texture) {
                    delete layer.texture;
                    layer.texture = nullptr;
                }
                layer.texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
                layer.texture->setSize(fw, fh);
                layer.texture->setFormat(is16 ? QOpenGLTexture::RGBA16_UNorm
                                              : QOpenGLTexture::RGBA8_UNorm);
                layer.texture->setMinificationFilter(QOpenGLTexture::Linear);
                layer.texture->setMagnificationFilter(QOpenGLTexture::Linear);
                layer.texture->setWrapMode(QOpenGLTexture::ClampToEdge);
                layer.texture->allocateStorage(QOpenGLTexture::RGBA,
                                               is16 ? QOpenGLTexture::UInt16
                                                    : QOpenGLTexture::UInt8);
                layer.textureFormat = fmt;
            }
            layer.texture->setData(0, 0,
                                   QOpenGLTexture::RGBA,
                                   is16 ? QOpenGLTexture::UInt16 : QOpenGLTexture::UInt8,
                                   static_cast<const void*>(layer.image.constBits()));
            layer.needsUpload = false;
        }

        if (!layer.texture) continue;

        glActiveTexture(GL_TEXTURE0);
        layer.texture->bind();
        m_program->setUniformValue(m_locFxTexSize,
            QVector2D(static_cast<float>(layer.texture->width()),
                      static_cast<float>(layer.texture->height())));
        m_program->setUniformValue(m_locOpacity, static_cast<float>(layer.opacity));
        glViewport(viewportX, viewportY, viewportW, viewportH);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        layer.texture->release();
    }
    m_vao.release();

    if (m_lutEnabled && m_lutTexture) {
        glActiveTexture(GL_TEXTURE1);
        m_lutTexture->release();
        glActiveTexture(GL_TEXTURE0);
    }

    m_program->release();
    glDisable(GL_BLEND);
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
    emit videoSourceTransformChanged(activeTrackForEmit(), m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
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
            emit videoSourceTransformChanged(activeTrackForEmit(), m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
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
        emit videoSourceTransformChanged(activeTrackForEmit(), m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
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
