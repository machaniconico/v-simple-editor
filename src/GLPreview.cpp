#include "GLPreview.h"
#include <cmath>

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

// Fragment shader — color correction pipeline on GPU
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

void main() {
    vec4 texColor = texture(uTexture, vTexCoord);
    vec3 color = texColor.rgb;

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
    }

    FragColor = vec4(clamp(color, 0.0, 1.0), texColor.a);
}
)";

GLPreview::GLPreview(QWidget *parent)
    : QOpenGLWidget(parent), m_vbo(QOpenGLBuffer::VertexBuffer)
{
    setMinimumSize(320, 180);
}

GLPreview::~GLPreview()
{
    makeCurrent();
    delete m_texture;
    delete m_program;
    m_vbo.destroy();
    m_vao.destroy();
    doneCurrent();
}

void GLPreview::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

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
    m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSrc);
    m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSrc);
    m_program->link();

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
}

void GLPreview::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void GLPreview::displayFrame(const QImage &frame)
{
    m_currentFrame = frame.convertToFormat(QImage::Format_RGBA8888);
    m_needsUpload = true;
    update();
}

void GLPreview::setColorCorrection(const ColorCorrection &cc)
{
    m_cc = cc;
    update();
}

void GLPreview::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    if (m_currentFrame.isNull()) return;

    // Upload texture if new frame
    if (m_needsUpload) {
        if (m_texture) {
            delete m_texture;
            m_texture = nullptr;
        }
        m_texture = new QOpenGLTexture(m_currentFrame);
        m_texture->setMinificationFilter(QOpenGLTexture::Linear);
        m_texture->setMagnificationFilter(QOpenGLTexture::Linear);
        m_texture->setWrapMode(QOpenGLTexture::ClampToEdge);
        m_needsUpload = false;
    }

    if (!m_texture) return;

    m_program->bind();
    m_texture->bind();

    // Set uniforms
    m_program->setUniformValue(m_locTexture, 0);
    m_program->setUniformValue(m_locEffectsEnabled, m_effectsEnabled);
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

    // Draw quad
    m_vao.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();

    m_texture->release();
    m_program->release();
}
