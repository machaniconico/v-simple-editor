#pragma once

#include <QDockWidget>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QTimer>
#include <QVector3D>
#include <QCheckBox>
#include <QColor>
#include <QRectF>
#include "VideoEffect.h"
#include "LutImporter.h"

class ColorWheelWidget;
class CurveEditor;
class HueVsSatEditor;
class QGroupBox;
class QLayout;
class QJsonObject;

// US-FEAT-C: Lift/Gamma/Gain wheels
struct ColorWheels {
    QVector3D lift   = QVector3D(0.0f, 0.0f, 0.0f);
    QVector3D gamma  = QVector3D(1.0f, 1.0f, 1.0f);
    QVector3D gain   = QVector3D(0.0f, 0.0f, 0.0f);
    double liftLuma  = 0.0;
    double gammaLuma = 1.0;
    double gainLuma  = 0.0;

    bool operator==(const ColorWheels &o) const {
        return lift == o.lift && gamma == o.gamma && gain == o.gain
            && liftLuma == o.liftLuma && gammaLuma == o.gammaLuma && gainLuma == o.gainLuma;
    }
    bool operator!=(const ColorWheels &o) const { return !(*this == o); }
};

class ColorGradingPanel : public QDockWidget
{
    Q_OBJECT

public:
    explicit ColorGradingPanel(QWidget *parent = nullptr);

    void setColorCorrection(const ColorCorrection &cc);
    ColorCorrection colorCorrection() const { return m_cc; }

    void setLutList(const QVector<LutData> &luts);
    QString selectedLutName() const;
    double lutIntensity() const;

    ColorWheels currentWheels() const;
    void setWheels(const ColorWheels &cw);

    // US-FIX-2 (CG-1): RGB Curves persistence. Control points are stored
    // image-space [0..255], so srcWidth/srcHeight is recorded for round-trip
    // fidelity / debugging but does not scale points on load (the response
    // curve is resolution-independent).
    QJsonObject curvesToJson(int srcWidth, int srcHeight) const;
    void curvesFromJson(const QJsonObject &obj, int curWidth, int curHeight);

signals:
    void colorCorrectionChanged(const ColorCorrection &cc);
    void colorWheelsChanged(const ColorWheels &cw);
    void lutSelected(const QString &name);
    void lutIntensityChanged(double intensity);
    void resetRequested();
    // US-CG-1: re-emit of CurveEditor::curvesChanged so MainWindow can
    // forward the 4x256 RGB curves LUT to GLPreview::setRgbCurves.
    void curvesChanged(const QVector<QVector<int>> &curves);
    void whiteBalancePickModeRequested(bool enabled);
    // US-CG-4: re-emit of HueVsSatEditor::hueVsSatChanged so MainWindow can
    // forward the 256-entry sat-multiplier LUT to GLPreview::setHueVsSatLut.
    void hueVsSatChanged(const QVector<float> &lut);
    // US-CG-2: white-balance gain triple (r, g, b) — multiplied at the very
    // top of the GLPreview grade chain. Identity = (1, 1, 1).
    void whiteBalanceChanged(float r, float g, float b);
    // US-CG-3: radial vignette / Power Window. Forwarded by MainWindow to
    // GLPreview::setVignette. amount=0 = no-op identity.
    //   amount    [-1..+1] (negative=darken, positive=lighten)
    //   midpoint  [ 0..1 ] (radius % of frame)
    //   roundness [-1..+1] (squareness)
    //   feather   [ 0..1 ] (edge softness)
    void vignetteChanged(float amount, float midpoint, float roundness, float feather);
    // US-EF-1: Chroma Key (Premiere Ultra Key / Resolve 3D Keyer simplified).
    // Applied at the very TOP of the compose path (BEFORE WB / LGG / curves /
    // vignette / LUT) so the key operates on raw frame colour. enabled=false
    // is a free no-op.
    //   keyH/S/L : key colour normalized to [0..1] (HSL space)
    //   hueTol   : [0..1]   (hue tolerance / 180)
    //   satTol   : [0..1]   (sat tolerance / 100)
    //   lumTol   : [0..1]   (lum tolerance / 100)
    //   spill    : [0..1]   (spill desaturation strength)
    //   softness : [0..1]   (smoothstep edge softness around the mask)
    void chromaKeyChanged(bool enabled, float keyH, float keyS, float keyL,
                          float hueTol, float satTol, float lumTol,
                          float spill, float softness);
    // US-EF-2: Mask Animation (DaVinci Power Window simplified). Wraps the
    // grade chain so the colour grade applies INSIDE the mask (ungraded
    // outside) — invertible. enabled=false → free no-op (bit-identical).
    //   ellipse  : false=Rect, true=Ellipse
    //   invert   : flip the mask weight (graded outside, ungraded inside)
    //   feather  : edge softness in [0..1] (slider 0..50 → 0..0.5)
    //   rect     : normalized QRectF (0..1) in vTexCoord space
    void maskChanged(bool enabled, bool ellipse, bool invert, float feather,
                     QRectF rect);
    // US-EF-2: User clicked "マスクを描画". MainWindow opens enterMaskEditMode
    // on VideoPlayer; on drag-finish the callback feeds setMaskRect back here.
    void requestMaskDraw();

    // US-EF-3: HSL Qualifier (DaVinci secondary grading). Isolates a hue/
    // sat/luma range and applies a SECONDARY lift/gamma/gain ONLY inside
    // the qualified region. Sits AFTER chroma key and BEFORE WB so the
    // qualifier acts on raw frame colour. enabled=false is a free no-op.
    //   hueCenter   degrees [0..360]
    //   hueRange    degrees [0..180]
    //   satMin/Max  fractions [0..1]
    //   lumaMin/Max fractions [0..1]
    //   softness    [0..50] degrees / percent edge slop
    //   lift / gamma / gain   per-channel adjustment (identity 0/1/1)
    void hslQualifierChanged(bool enabled,
                             float hueCenter, float hueRange,
                             float satMin, float satMax,
                             float lumaMin, float lumaMax,
                             float softness,
                             float liftR, float liftG, float liftB,
                             float gammaR, float gammaG, float gammaB,
                             float gainR, float gainG, float gainB);

    // US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens Distortion.
    // Forwarded by MainWindow to the matching GLPreview setters. Identity
    // values (0, 0, 0) are a free no-op — the shader's |amount|>eps tests
    // skip the kernel/transform entirely.
    //   sharpen [ 0..200] %, identity 0
    //   blur    [ 0..50 ] px, identity 0
    //   lens    [-100..100] (negative=barrel, positive=pincushion), identity 0
    void effectsPackChanged(float sharpen, float blur, float lens);

    // US-3D: 3-axis rotation + perspective foreshortening (Premiere "Basic
    // 3D" / Resolve "Transform" 3D rotation parity). Forwarded by MainWindow
    // to GLPreview::setRotation3D. Identity (xDeg=yDeg=zDeg=0,
    // perspectiveDist=2.0) is a free no-op — the shader detects identity
    // and skips the warp entirely.
    //   xDeg/yDeg/zDeg   degrees [-180..+180], identity 0
    //   perspectiveDist  inverse FOV [0.1..10.0], identity 2.0
    //                    (smaller = stronger perspective)
    void rotation3DChanged(float xDeg, float yDeg, float zDeg, float perspectiveDist);

public slots:
    void applyWhiteBalancePick(const QColor &pixel);
    void setWhiteBalancePickModeActive(bool active);
    // US-EF-2: callback target — VideoPlayer's mask-edit overlay forwards the
    // normalized rect here, which updates the panel labels and re-emits
    // maskChanged so GLPreview picks up the new geometry.
    void setMaskRect(const QRectF &normalizedRect);

private slots:
    void onLiftChanged(double r, double g, double b);
    void onGammaWheelChanged(double r, double g, double b);
    void onGainChanged(double r, double g, double b);
    void onSliderChanged();
    void onWheelSliderChanged();
    void emitWheelsDebounced();
    void onLutComboChanged(int index);
    void onLutIntensityChanged(int value);
    void onResetClicked();
    // US-CG-2: WB Temperature/Tint sliders changed → recompute (r,g,b) gain
    // triple, update labels, emit whiteBalanceChanged.
    void onWhiteBalanceChanged();
    void onWhiteBalancePickToggled(bool enabled);
    // US-CG-3: Vignette sliders changed → normalize to [-1..+1] / [0..1]
    // ranges, update labels, emit vignetteChanged.
    void onVignetteChanged();
    // US-EF-1: Chroma Key enable/colour/tolerance/spill/softness changed →
    // recompute HSL of the key colour, normalize tolerances, emit
    // chromaKeyChanged with the full uniform packet.
    void onChromaKeyChanged();
    // US-EF-1: open QColorDialog seeded with the current key colour. On
    // accept, refresh the swatch and re-emit chromaKeyChanged.
    void onChromaKeyColourClicked();
    // US-EF-2: any mask control changed (enable / shape / invert / feather)
    // → recompute uniforms and emit maskChanged. The rect itself is updated
    // separately via setMaskRect when the user redraws on the preview.
    void onMaskChanged();
    // US-EF-2: "マスクを描画" button click → emit requestMaskDraw so MainWindow
    // can call VideoPlayer::enterMaskEditMode.
    void onMaskDrawClicked();
    // US-EF-2: NIT-1 deferred. "キーフレーム追加" stub — currently just
    // re-emits maskChanged with the current rect; full per-clip keyframe
    // storage + linear interpolation are deferred to a follow-up story.
    void onMaskAddKeyframeClicked();
    // US-EF-3: any HSL Qualifier control changed → snapshot state, refresh
    // labels, normalize sat/luma to fractions and emit hslQualifierChanged.
    void onHslQualifierChanged();
    // US-EF-4: any Effects (Sharpen / Blur / Lens Distortion) slider changed
    // → refresh labels and emit effectsPackChanged with the raw slider scalars.
    // Sliders default to 0 (identity / no-op) so the stage stays free until
    // the user moves them.
    void onEffectsPackChanged();
    // US-3D: any 3D rotation (X/Y/Z degrees + perspective distance) slider
    // changed → refresh labels and emit rotation3DChanged. Defaults
    // (0/0/0/2.0) are identity / free no-op in the shader.
    void onRotation3DChanged();

private:
    struct SliderRow {
        QSlider *slider;
        QLabel *valueLabel;
        QString paramName;
    };

    struct WheelSliderGroup {
        QSlider *r, *g, *b, *luma;
        QLabel *rLabel, *gLabel, *bLabel, *lumaLabel;
    };

    SliderRow addSlider(QLayout *layout, const QString &label,
                        int min, int max, int initial, int scale = 1);
    enum WheelType { LiftWheel, GammaWheel, GainWheel };
    WheelSliderGroup addWheelSliders(QGroupBox *group, WheelType type);
    void updateSlidersFromCC();
    void blockSliderSignals(bool block);
    static double sliderToGamma(int v);
    static int gammaToSlider(double g);
    static double sliderToLiftGain(int v);
    static int liftGainToSlider(double v);
    static double wheelGammaToCorrection(double g);
    static double correctionGammaToWheel(double g);
    static ColorWheels wheelsFromColorCorrection(const ColorCorrection &cc);
    void syncColorCorrectionFromWheels(const ColorWheels &cw);
    void updateWhiteBalanceControlsFromCC();
    void updateBasicTemperatureTintFromCC();
    void updateGraphicalWheelsFromCC();

    ColorCorrection m_cc;
    ColorWheels m_wheels;

    // Color wheels (graphical)
    ColorWheelWidget *m_liftWheel;
    ColorWheelWidget *m_gammaWheel;
    ColorWheelWidget *m_gainWheel;

    // Lift/Gamma/Gain slider groups
    WheelSliderGroup m_liftSliders;
    WheelSliderGroup m_gammaSliders;
    WheelSliderGroup m_gainSliders;

    // Basic correction sliders
    SliderRow m_exposure, m_brightness, m_contrast;
    SliderRow m_highlights, m_shadows;
    SliderRow m_saturation, m_hue;
    SliderRow m_temperature, m_tint;
    SliderRow m_gamma;

    // US-CG-2: White-balance sliders (independent of m_temperature/m_tint
    // CPU-style adjustment). These drive the GLPreview uWb uniform — a
    // (r, g, b) gain triple multiplied at the very top of the grade chain.
    QSlider *m_wbTemperature = nullptr;
    QSlider *m_wbTint = nullptr;
    QLabel  *m_wbTemperatureLabel = nullptr;
    QLabel  *m_wbTintLabel = nullptr;
    QPushButton *m_wbPickButton = nullptr;

    // US-CG-3: Vignette / Power Window sliders. These drive the GLPreview
    // uVigAmount/uVigMid/uVigRound/uVigFeather uniforms. Identity =
    // amount slider == 0 (factor==1.0 in the shader → free no-op).
    QSlider *m_vigAmount = nullptr;
    QSlider *m_vigMidpoint = nullptr;
    QSlider *m_vigRoundness = nullptr;
    QSlider *m_vigFeather = nullptr;
    QLabel  *m_vigAmountLabel = nullptr;
    QLabel  *m_vigMidpointLabel = nullptr;
    QLabel  *m_vigRoundnessLabel = nullptr;
    QLabel  *m_vigFeatherLabel = nullptr;

    // US-EF-1: Chroma Key (Premiere Ultra Key / Resolve 3D Keyer simplified).
    // Sits at the very top of the compose path (BEFORE WB / LGG / curves /
    // vignette / LUT) so the key gates raw frame colour. enabled=unchecked
    // emits keyEnabled=false → free no-op in the shader.
    QCheckBox   *m_chromaEnabled        = nullptr;
    QPushButton *m_chromaKeyColourBtn   = nullptr;
    QColor       m_chromaKey            = QColor(0, 255, 0); // default green screen
    QSlider     *m_chromaHueTol         = nullptr;
    QSlider     *m_chromaSatTol         = nullptr;
    QSlider     *m_chromaLumTol         = nullptr;
    QSlider     *m_chromaSpill          = nullptr;
    QSlider     *m_chromaSoftness       = nullptr;
    QLabel      *m_chromaHueTolLabel    = nullptr;
    QLabel      *m_chromaSatTolLabel    = nullptr;
    QLabel      *m_chromaLumTolLabel    = nullptr;
    QLabel      *m_chromaSpillLabel     = nullptr;
    QLabel      *m_chromaSoftnessLabel  = nullptr;

    // US-EF-2: Mask Animation (Power Window simplified). enabled=false is a
    // free no-op so the existing grade pipeline keeps producing identical
    // output. The "rect" lives on the panel for this story; per-clip
    // keyframe storage + linear interpolation are deferred (NIT-1).
    QCheckBox   *m_maskEnabled          = nullptr;
    QComboBox   *m_maskShape            = nullptr; // 0=Rect, 1=Ellipse
    QCheckBox   *m_maskInvert           = nullptr;
    QSlider     *m_maskFeather          = nullptr; // 0..50 → /100 → [0..0.5]
    QPushButton *m_maskDrawBtn          = nullptr;
    QPushButton *m_maskAddKeyframeBtn   = nullptr;
    QLabel      *m_maskFeatherLabel     = nullptr;
    QLabel      *m_maskRectXLabel       = nullptr;
    QLabel      *m_maskRectYLabel       = nullptr;
    QLabel      *m_maskRectWLabel       = nullptr;
    QLabel      *m_maskRectHLabel       = nullptr;
    bool         m_maskEnabledState     = false;
    bool         m_maskEllipseState     = false;
    bool         m_maskInvertState      = false;
    int          m_maskFeatherSlider    = 10;
    QRectF       m_maskCurrentRect      = QRectF(0.25, 0.25, 0.50, 0.50);

    // US-EF-3: HSL Qualifier (DaVinci secondary grading). Sits between クロマ
    // キー and マスク in the panel because the GLPreview shader applies it
    // AFTER chroma key and BEFORE WB so it operates on raw frame colour.
    // Default-disabled = free no-op.
    QCheckBox *m_hslqEnabled        = nullptr;
    QSlider   *m_hslqHueCenter      = nullptr; // degrees [0..360], default 30
    QSlider   *m_hslqHueRange       = nullptr; // degrees [0..180], default 30
    QSlider   *m_hslqSatMin         = nullptr; // percent [0..100], default 30
    QSlider   *m_hslqSatMax         = nullptr; // percent [0..100], default 100
    QSlider   *m_hslqLumaMin        = nullptr; // percent [0..100], default 30
    QSlider   *m_hslqLumaMax        = nullptr; // percent [0..100], default 80
    QSlider   *m_hslqSoftness       = nullptr; // [0..50],          default 10
    QSlider   *m_hslqLiftR          = nullptr; // [-50..+50] per ch,  default 0
    QSlider   *m_hslqLiftG          = nullptr;
    QSlider   *m_hslqLiftB          = nullptr;
    QSlider   *m_hslqGammaR         = nullptr; // [-100..+100] per ch, default 0
    QSlider   *m_hslqGammaG         = nullptr;
    QSlider   *m_hslqGammaB         = nullptr;
    QSlider   *m_hslqGainR          = nullptr; // [-100..+100] per ch, default 0
    QSlider   *m_hslqGainG          = nullptr;
    QSlider   *m_hslqGainB          = nullptr;
    QLabel    *m_hslqHueCenterLabel = nullptr;
    QLabel    *m_hslqHueRangeLabel  = nullptr;
    QLabel    *m_hslqSatMinLabel    = nullptr;
    QLabel    *m_hslqSatMaxLabel    = nullptr;
    QLabel    *m_hslqLumaMinLabel   = nullptr;
    QLabel    *m_hslqLumaMaxLabel   = nullptr;
    QLabel    *m_hslqSoftnessLabel  = nullptr;
    QLabel    *m_hslqLiftRLabel     = nullptr;
    QLabel    *m_hslqLiftGLabel     = nullptr;
    QLabel    *m_hslqLiftBLabel     = nullptr;
    QLabel    *m_hslqGammaRLabel    = nullptr;
    QLabel    *m_hslqGammaGLabel    = nullptr;
    QLabel    *m_hslqGammaBLabel    = nullptr;
    QLabel    *m_hslqGainRLabel     = nullptr;
    QLabel    *m_hslqGainGLabel     = nullptr;
    QLabel    *m_hslqGainBLabel     = nullptr;

    // US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens Distortion.
    // Sits BELOW the HSL Qualifier group in the panel; GLPreview applies
    // lens distortion at the very TOP of the fragment shader (texture
    // coord transform) and blur + sharpen at the very END (POST stage).
    // All sliders default to 0 (identity / free no-op).
    QSlider *m_efSharpen     = nullptr; //  [0..200] %, default 0
    QSlider *m_efBlur        = nullptr; //  [0..50] px, default 0
    QSlider *m_efLens        = nullptr; // [-100..+100], default 0
    QLabel  *m_efSharpenLabel = nullptr;
    QLabel  *m_efBlurLabel    = nullptr;
    QLabel  *m_efLensLabel    = nullptr;

    // US-3D: 3-axis rotation + perspective foreshortening sliders. Sits
    // BELOW エフェクト. GLPreview applies the inverse warp at the very TOP
    // of the fragment shader (composed AFTER lens distortion) so the
    // rotation re-projects the texture sample BEFORE the colour grade
    // pipeline runs. Defaults are identity (X=Y=Z=0, perspective=2.0).
    QSlider *m_rot3DX             = nullptr; // [-180..+180], default 0
    QSlider *m_rot3DY             = nullptr; // [-180..+180], default 0
    QSlider *m_rot3DZ             = nullptr; // [-180..+180], default 0
    QSlider *m_rot3DPerspective   = nullptr; //   [1..100] = 0.1..10.0, default 20
    QLabel  *m_rot3DXLabel        = nullptr;
    QLabel  *m_rot3DYLabel        = nullptr;
    QLabel  *m_rot3DZLabel        = nullptr;
    QLabel  *m_rot3DPerspectiveLabel = nullptr;
    QPushButton *m_rot3DResetBtn  = nullptr;

    // LUT controls
    QComboBox *m_lutCombo;
    QSlider *m_lutIntensitySlider;
    QLabel *m_lutIntensityLabel;

    QPushButton *m_resetButton;
    QTimer *m_wheelDebounce;
    bool m_updating = false;

    // US-CG-1: RGB curves editor embedded below the LGG sliders.
    CurveEditor *m_curveEditor = nullptr;
    // US-CG-4: Hue vs Saturation curve editor embedded below RGB Curves.
    HueVsSatEditor *m_hueVsSatEditor = nullptr;
};
