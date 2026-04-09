#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <QMap>
#include <QVector2D>
#include <QVector3D>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLTexture>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QObject>

// ---------------------------------------------------------------------------
// ParamType — supported uniform types
// ---------------------------------------------------------------------------

enum class ParamType {
    Float,
    Int,
    Bool,
    Vec2,
    Vec3,
    Color
};

// ---------------------------------------------------------------------------
// ParamDef — defines one adjustable parameter for a shader effect
// ---------------------------------------------------------------------------

struct ParamDef {
    QString   name;
    ParamType type     = ParamType::Float;
    QVariant  minVal;
    QVariant  maxVal;
    QVariant  defaultVal;
};

// ---------------------------------------------------------------------------
// ShaderEffectDef — immutable description of one GPU effect
// ---------------------------------------------------------------------------

struct ShaderEffectDef {
    QString          name;
    QString          category;
    QString          description;
    QString          fragmentShaderSource;   // GLSL 330 core fragment shader
    QVector<ParamDef> params;
};

// ---------------------------------------------------------------------------
// ShaderEffectInstance — a def + current parameter values
// ---------------------------------------------------------------------------

class ShaderEffectInstance : public QObject
{
    Q_OBJECT

public:
    explicit ShaderEffectInstance(const ShaderEffectDef &def, QObject *parent = nullptr);

    const ShaderEffectDef &def() const { return m_def; }

    void     setParam(const QString &name, const QVariant &value);
    QVariant getParam(const QString &name) const;

    // Returns all current values keyed by param name
    QMap<QString, QVariant> allParams() const { return m_values; }

    void resetToDefaults();

signals:
    void paramChanged(const QString &name, const QVariant &value);

private:
    ShaderEffectDef         m_def;
    QMap<QString, QVariant> m_values;
};

// ---------------------------------------------------------------------------
// ShaderEffectLibrary — singleton registry of built-in GPU effects
// ---------------------------------------------------------------------------

class ShaderEffectLibrary : public QObject
{
    Q_OBJECT

public:
    static ShaderEffectLibrary &instance();

    QVector<ShaderEffectDef>  allEffects() const;
    QVector<ShaderEffectDef>  effectsByCategory(const QString &category) const;
    QStringList               categories() const;

    // Returns nullptr (invalid def) if not found — check .name.isEmpty()
    ShaderEffectDef           findByName(const QString &name) const;

private:
    explicit ShaderEffectLibrary(QObject *parent = nullptr);
    ~ShaderEffectLibrary() override = default;

    void registerBuiltins();

    QVector<ShaderEffectDef> m_effects;
};

// ---------------------------------------------------------------------------
// ShaderEffectRenderer — compiles & renders shader effect chains on the GPU
// ---------------------------------------------------------------------------

class ShaderEffectRenderer : public QObject, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit ShaderEffectRenderer(QObject *parent = nullptr);
    ~ShaderEffectRenderer() override;

    // Must be called once inside an active OpenGL context
    void initialize();

    // Compile a shader effect (caches by name); returns false on error
    bool compile(const ShaderEffectDef &def);

    // Render one effect: reads inputTextureId, writes into outputFBO.
    // width/height are the FBO / texture dimensions.
    void render(GLuint inputTextureId,
                const ShaderEffectInstance &instance,
                QOpenGLFramebufferObject *outputFBO,
                int width, int height);

    // Render a chain of effects via ping-pong FBOs.
    // Returns the texture id of the final result (owned by internal FBO).
    GLuint renderChain(GLuint inputTextureId,
                       const QVector<ShaderEffectInstance *> &chain,
                       int width, int height);

    QString lastError() const { return m_lastError; }

private:
    // One compiled program per effect name
    QMap<QString, QOpenGLShaderProgram *> m_programs;

    // Ping-pong FBOs for chaining
    QOpenGLFramebufferObject *m_fbo[2] = {nullptr, nullptr};
    int  m_fboWidth  = 0;
    int  m_fboHeight = 0;

    // Fullscreen quad geometry
    QOpenGLBuffer             m_vbo;
    QOpenGLVertexArrayObject  m_vao;

    QString m_lastError;
    bool    m_initialized = false;

    // Internal helpers
    void ensureFBOs(int width, int height);
    void setupQuad();
    void bindUniforms(QOpenGLShaderProgram *prog, const ShaderEffectInstance &instance);
    void drawQuad();

    static const char *s_vertexShaderSrc;
};
