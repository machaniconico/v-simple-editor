#pragma once

#include <QWidget>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QGroupBox>
#include <QVBoxLayout>

struct GlowState {
    bool enabled = false;
    float threshold = 0.5f;
    float radius = 10.0f;
    float intensity = 1.0f;
};

struct BloomState {
    bool enabled = false;
    float threshold = 0.5f;
    float intensity = 1.0f;
    float spread = 0.3f;
};

struct ChromaticAberrationState {
    bool enabled = false;
    float amount = 5.0f;
    float radialFalloff = 2.0f;
};

struct LightWrapState {
    bool enabled = false;
    float amount = 0.5f;
    float radius = 10.0f;
};

class VfxControlsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit VfxControlsPanel(QWidget *parent = nullptr);

    GlowState glowState() const;
    BloomState bloomState() const;
    ChromaticAberrationState chromaticAberrationState() const;
    LightWrapState lightWrapState() const;

    void blockAndReset();

signals:
    void glowChanged(bool enabled, float threshold, float radius, float intensity);
    void bloomChanged(bool enabled, float threshold, float intensity, float spread);
    void chromaticAberrationChanged(bool enabled, float amount, float radialFalloff);
    void lightWrapChanged(bool enabled, float amount, float radius);

private:
    void buildGlowSection();
    void buildBloomSection();
    void buildChromaticAberrationSection();
    void buildLightWrapSection();

    void emitGlow();
    void emitBloom();
    void emitChromaticAberration();
    void emitLightWrap();

    // Glow
    QGroupBox *m_glowGroup = nullptr;
    QCheckBox *m_glowEnable = nullptr;
    QDoubleSpinBox *m_glowThreshold = nullptr;
    QDoubleSpinBox *m_glowRadius = nullptr;
    QDoubleSpinBox *m_glowIntensity = nullptr;

    // Bloom
    QGroupBox *m_bloomGroup = nullptr;
    QCheckBox *m_bloomEnable = nullptr;
    QDoubleSpinBox *m_bloomThreshold = nullptr;
    QDoubleSpinBox *m_bloomIntensity = nullptr;
    QDoubleSpinBox *m_bloomSpread = nullptr;

    // Chromatic Aberration
    QGroupBox *m_chromaGroup = nullptr;
    QCheckBox *m_chromaEnable = nullptr;
    QDoubleSpinBox *m_chromaAmount = nullptr;
    QDoubleSpinBox *m_chromaFalloff = nullptr;

    // Light Wrap
    QGroupBox *m_lightWrapGroup = nullptr;
    QCheckBox *m_lightWrapEnable = nullptr;
    QDoubleSpinBox *m_lightWrapAmount = nullptr;
    QDoubleSpinBox *m_lightWrapRadius = nullptr;

    bool m_blocking = false;
};
