#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QImage>
#include <QVector>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QFont>
#include <QColor>
#include <array>
#include "VideoEffect.h"
#include "LutImporter.h"
#include "MotionStabilizer.h"

class Timeline;
class SurfaceTool;
class BrushAnimation;

class GLPreview : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit GLPreview(QWidget *parent = nullptr);
    ~GLPreview();

    void displayFrame(const QImage &frame);
    void setBrushAnimation(BrushAnimation *animation);
    void clearBrushAnimation();
    void setBrushAnimationProgress(double progress);
    void setDisplayAspectRatio(double aspectRatio);
    void setColorCorrection(const ColorCorrection &cc);
    // 0 = none (SDR sRGB), 1 = PQ (SMPTE ST 2084), 2 = HLG (ARIB STD-B67)
    void setHdrTransfer(int transfer);
    int hdrTransfer() const { return m_hdrTransfer; }
    // GPU-side stack: handles Blur/Sharpen/Noise/Sepia/Grayscale/Invert/Vignette.
    void setVideoEffects(const QVector<VideoEffect> &effects);
    void setEffectsEnabled(bool enabled) { m_effectsEnabled = enabled; update(); }
    bool effectsEnabled() const { return m_effectsEnabled; }
    void setLut(const LutData &lut);
    // US-FEAT-B: LUT 3D-texture blend — upload QImage grid as GL_TEXTURE_3D
    void setLutTexture(const QImage &lutGrid, float intensity);
    void setLutIntensity(double intensity);
    void clearLut();
    // US-WIRE-2: Lift/Gamma/Gain color wheels from ColorGradingPanel
    void setLiftGammaGain(const std::array<std::array<double,4>,3> &values);
    // US-CG-1: RGB curves editor — 4 channels (R, G, B, Luma) of 256 ints
    // each in [0,255]. Uploaded as a 256x4 GL_RGBA8 texture and applied
    // in the fragment shader after Lift/Gamma/Gain and before the .cube LUT.
    void setRgbCurves(const QVector<QVector<int>> &curves);
    // US-CG-2: White-balance gain triple. Multiplied into c.rgb at the very
    // top of the grade chain — BEFORE LGG, RGB curves, and the .cube LUT.
    // Identity = (1, 1, 1) → no-op.
    void setWhiteBalance(float r, float g, float b);
    // US-CG-3: Radial vignette / Power Window. Applied AFTER RGB curves and
    // BEFORE the .cube LUT. Identity (amount=0) is a free no-op.
    //   amount    : -1..+1   (negative = darken, positive = lighten)
    //   midpoint  :  0..1    (radius % of frame, 0.7 = default)
    //   roundness : -1..+1   (0 = circular, ±1 = squareness)
    //   feather   :  0..1    (edge softness, 0.3 = default)
    void setVignette(float amount, float midpoint, float roundness, float feather);
    // US-EF-1: Chroma Key (Premiere Ultra Key / Resolve 3D Keyer simplified).
    // Applied at the very TOP of the compose path — BEFORE WB / LGG / curves /
    // vignette / .cube LUT — so the key gates raw frame colour. enabled=false
    // is a free no-op (shader test guards the whole stage). All scalars are
    // already normalized to [0..1] by ColorGradingPanel.
    //   keyH/S/L   : key colour in HSL space ([0..1] each)
    //   hueTol     : hue tolerance fraction (degrees / 180)
    //   satTol     : sat tolerance fraction (percent / 100)
    //   lumTol     : lum tolerance fraction (percent / 100)
    //   spill      : spill desaturation strength [0..1]
    //   softness   : edge smoothstep half-width [0..1]
    void setChromaKey(bool enabled, float keyH, float keyS, float keyL,
                      float hueTol, float satTol, float lumTol,
                      float spill, float softness);
    // US-EF-2: Mask Animation (DaVinci Power Window simplified). Wraps the
    // entire grade chain (chroma key → WB → LGG → curves → vignette → LUT)
    // so the colour grade applies INSIDE the mask region; outside stays raw
    // (or vice versa when invert=true). enabled=false is a free no-op (the
    // shader test branches around the mix() so the previous output is
    // returned unchanged).
    //   ellipse  : false=Rect, true=Ellipse
    //   invert   : flip the mask weight
    //   feather  : edge softness in [0..1]
    //   normalizedRect : (x, y, w, h) in vTexCoord space, all in [0..1]
    void setMask(bool enabled, bool ellipse, bool invert, float feather,
                 QRectF normalizedRect);

    // US-EF-3: HSL Qualifier (DaVinci Resolve secondary grading). Isolates
    // a hue/sat/luma range and applies a SECONDARY lift/gamma/gain ONLY
    // inside the qualified region. Different from chroma key (which masks
    // for transparency); this is selective COLOUR ADJUSTMENT — e.g. "make
    // skin tones warmer" or "desaturate everything except the red flowers".
    // Sits AFTER chroma key and BEFORE WB so the qualifier acts on raw
    // colour. enabled=false is a free no-op.
    //   hueCenter   degrees [0..360]
    //   hueRange    degrees [0..180]
    //   satMin/Max  percent [0..100]
    //   lumaMin/Max percent [0..100]
    //   softness    [0..50] (degrees / percent edge slop)
    //   lift        per-channel additive offset (-0.5..+0.5 typical)
    //   gamma       per-channel gamma (>0; identity 1.0)
    //   gain        per-channel multiplier (identity 1.0)
    void setHslQualifier(bool enabled,
                         float hueCenter, float hueRange,
                         float satMin, float satMax,
                         float lumaMin, float lumaMax,
                         float softness,
                         float liftR, float liftG, float liftB,
                         float gammaR, float gammaG, float gammaB,
                         float gainR, float gainG, float gainB);

    // US-CG-4: Hue vs Saturation curve (DaVinci Resolve color page parity).
    // Uploads a 256x1 R32F texture; sampler in the fragment shader looks up
    // the saturation multiplier by the pixel's HSL hue. Default = identity
    // (lut[i] == 1.0 ∀ i) keeps the stage a free no-op. Pass an empty
    // vector to disable.
    void setHueVsSatLut(const QVector<float> &lut);

    // US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens Distortion.
    // All three stages live in the same fragment shader as the rest of the
    // grade chain. Lens distortion is a TEXTURE COORDINATE TRANSFORM applied
    // at the very TOP of main() (before the texture sample), while blur and
    // sharpen are POST stages applied at the END of main() — after the
    // entire grade chain (chroma key → HSL Q → WB → exposure → ... → LGG →
    // curves → vignette → LUT → mask wrap).
    //   sharpen   : 0..200 (% × 0.01 inside the shader, identity at 0)
    //   blur      : 0..50 px radius (identity at 0)
    //   lens      : -100..+100, scaled by 0.01 inside the shader (identity 0;
    //               negative = barrel, positive = pincushion)
    void setSharpen(float amount);
    void setBlur(float radiusPx);
    void setLensDistortion(float amount);

    // US-3D: 3-axis rotation + perspective foreshortening (Premiere "Basic 3D"
    // / Resolve "Transform" 3D rotation parity). Implemented in the fragment
    // shader as an inverse texture-coordinate warp at the very TOP of main()
    // (composed AFTER lens distortion, BEFORE the texture sample). Identity
    // (xDeg=yDeg=zDeg=0, perspectiveDist≥0.1) is a free no-op — the shader
    // detects the identity matrix and skips the stage. perspectiveDist is the
    // inverse FOV (smaller = stronger perspective); 0.1..10.0 is the UI range
    // (default 2.0). Out-of-bounds samples render black so the rotated quad
    // letterboxes against the canvas.
    void setRotation3D(float xDeg, float yDeg, float zDeg, float perspectiveDist);

    // US-INT-4: per-clip stabilizer keyframes + per-frame source time. The
    // setter copies the keyframe vector (sorted by timeUs); the time hook
    // does a std::lower_bound on every paintGL to pick the active keyframe
    // (or interpolates between two) and bakes the inverse 2D affine into
    // a uStab uniform that runs BEFORE the existing 3D-rotate stage so the
    // user 3D-rotate matrix is preserved (transforms compose by stacking).
    void setStabilizerKeyframes(const QVector<StabilizerKeyframe> &kfs);
    void setStabilizerSourceTimeUs(qint64 sourceUs);

    // US-INT-1: non-owning Timeline pointer used to query
    // composeAdjustmentLayersAt(timelineUs) in paintGL. nullptr → no-op
    // (preview is bit-identical to pre-INT-1 baseline).
    void setTimeline(Timeline *t) { m_timeline = t; }

    // Phase 1e — true only when VEDITOR_GL_INTEROP=1 AND WGL_NV_DX_interop2
    // is supported AND all 6 wglDX*NV procs resolved during initializeGL().
    bool isInteropAvailable() const noexcept { return m_interopAvailable; }
    // Section B: VideoPlayer pushes its FFmpeg-owned ID3D11Device pointer; we
    // store it and open the interop handle lazily inside paintGL where the GL
    // context is guaranteed current. nullptr means "no D3D11 device yet" or
    // "decoder closed" — close any open interop handle on the next paint.
    void setSharedD3D11Device(void *d3d11Device);
    void *interopDevice() const noexcept { return m_interopDevice; }
    // Section C: zero-copy display path. d3d11Texture is the ID3D11Texture2D*
    // owned by FFmpeg's frame pool; it stays valid while the AVFrame is held.
    void displayD3D11Frame(void *d3d11Texture, int subresource, int width, int height);
    // Drop every cached register entry. Called by VideoPlayer when the
    // decoder pool is destroyed/reset so we don't dereference recycled
    // ID3D11Texture2D pointers.
    void flushInteropCache();
    // Adobe-style text tool mode. When active, the cursor is Qt::IBeamCursor
    // and mouse press+drag+release captures a text box rectangle; on release
    // textRectRequested is emitted with the rect in normalized 0.0–1.0
    // widget-relative coordinates. If the drag is smaller than 8x8 pixels
    // the signal is suppressed so accidental clicks don't create stray boxes.
    void setTextToolActive(bool active);
    // Clear the persisted text-tool marquee. Called by MainWindow after the
    // user commits the text via 適用 so the overlay visual matches the
    // committed state.
    void clearTextToolRect();
    // Update the live-preview text style so the in-place typed text rendered
    // inside the marquee matches the current right-panel property values.
    void setTextToolStyle(const QFont &font, const QColor &color);

    // Snapshot of currently-rendered text overlays — used by click-to-edit
    // so clicking an existing overlay's area with the I-beam cursor enters
    // edit mode for THAT overlay instead of creating a new rect.
    struct TextOverlayHit {
        int index = -1;          // index into the project's clip[0].textManager
        QString text;            // current text, prefilled into the edit buffer
        QRectF normalizedRect;   // 0..1 widget-relative, center-based like EnhancedTextOverlay
    };
    void setTextOverlayHitList(const QVector<TextOverlayHit> &hits);
    // Query / force-commit the in-place edit session. Called from MainWindow
    // when the user clicks 適用 while edit mode is active so the right-panel
    // button commits the text typed directly on the preview instead of
    // reading a stale QLineEdit value.
    bool isTextToolEditing() const { return m_textToolEditing; }
    QString currentTextToolInputText() const { return m_textToolInputText; }
    int currentTextToolEditingIndex() const { return m_textToolEditingIndex; }

    // Displayed image area (QRectF in widget logical pixels). Public so
    // VideoPlayer::composeFrameWithOverlays can inverse-scale committed
    // text pointSize to match the literal display pt used by the inline
    // input renderer (US-T32 WYSIWYG).
    QRectF letterboxRect() const;
    // US-T36 16:9 base export canvas rect in widget pixels, centered in the
    // widget. Used both for the always-on reference outline and for the
    // US-T37 OBS-style snap-to-frame logic during video-source drag.
    QRectF canvasFrameRect() const;
    void commitCurrentTextToolEdit();

    // US-T34 OBS-style video source transform. scale is uniform relative
    // to the letterbox; dx/dy are offsets in fractions of the letterbox
    // width/height. Identity (1, 0, 0) reproduces the pre-transform layout.
    void setVideoSourceTransform(double scale, double dx, double dy);
    double videoSourceScale() const { return m_videoSourceScale; }
    double videoSourceDx()    const { return m_videoSourceDx; }
    double videoSourceDy()    const { return m_videoSourceDy; }
    void resetVideoSourceTransform();
    // V3 sprint — Timeline-driven selection arms the handle-draw gate
    // (m_videoTransformSelected) without requiring a separate preview click.
    // Call with `true` when an edit target is set on a non-identity clip,
    // `false` when the edit target is cleared.
    void setVideoTransformSelected(bool selected);
    // When true, paintGL ignores m_videoSourceScale/Dx/Dy on the GL
    // viewport. The multi-track compositor pre-bakes per-clip transforms
    // into the canvas image, so the GL viewport must render identity to
    // avoid double-scaling. The drag-handle UI keeps reading the live
    // transform from m_videoSourceScale so user gestures aren't fought
    // by the per-tick composite pass that previously called
    // setVideoSourceTransform(1, 0, 0) and clobbered the drag state.
    void setCompositeBakedMode(bool enabled);
    bool compositeBakedMode() const { return m_compositeBakedMode; }
    // US-T39 Snap strength adjustment — 0 disables snap, larger values make
    // it pull from farther away. MainWindow persists via QSettings.
    void setSnapStrength(double pixels) { m_snapStrength = qMax(0.0, pixels); }
    double snapStrength() const { return m_snapStrength; }
    // US-MOCHA-3: SurfaceTool integration — installs a 4-corner pin gizmo
    // that draws on top of the GLPreview and intercepts mouse events when enabled.
    void installSurfaceTool(SurfaceTool *tool);

signals:
    void textRectRequested(const QRectF &normalizedRect);
    // Fired when the user commits in-place text with Enter. MainWindow
    // builds an EnhancedTextOverlay from this text + the rect + the right
    // panel's current style/time values.
    void textInlineCommitted(const QString &text, const QRectF &normalizedRect);
    // Fired when the user commits an edit to an EXISTING overlay (click
    // the I-beam on a rendered text, edit the text, press Enter). Carries
    // the overlay index and the new text; the rect is unchanged.
    void textOverlayEditCommitted(int overlayIndex, const QString &newText);
    // In-place edit of an EXISTING overlay begins / ends. VideoPlayer hides
    // the committed render during the edit so it doesn't overlap live typing.
    void textOverlayEditStarted(int overlayIndex);
    void textOverlayEditEnded();
    // Existing-overlay drag (move or resize) committed — MainWindow rewrites
    // the rect in place. normalizedRect uses center-based normalized coords.
    void textOverlayRectChanged(int overlayIndex, const QRectF &normalizedRect);
    // US-T34 — emitted while the user drags or resizes the video source so
    // VideoPlayer / MainWindow can persist the transform across sessions.
    void videoSourceTransformChanged(double scale, double dx, double dy);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private slots:
    void cleanupGL();

private:
    void createShaderProgram();
    void updateUniforms();
    void detectInteropExtension();
    bool ensureInteropDeviceForPaint();
    void releaseRegisteredTexturesLocked();
    void renderPendingD3D11Frame();
    // letterboxRect() moved to public section (US-T32).

    QOpenGLShaderProgram *m_program = nullptr;
    QOpenGLTexture *m_texture = nullptr;
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;

    QImage m_currentFrame;
    BrushAnimation *m_brushAnimation = nullptr;
    double m_brushAnimationProgress = 0.0;
    ColorCorrection m_cc;
    bool m_effectsEnabled = true;
    bool m_needsUpload = false;
    double m_displayAspectRatio = 0.0;

    // Text tool drag state — press/move/release in widget-pixel coordinates.
    // m_textToolHasRect stays true after a successful drag so the marquee
    // keeps rendering until the user commits via 適用, starts a new drag,
    // or toggles the tool off.
    enum TextToolInteraction {
        TextToolIdle,
        TextToolCreating, // fresh drag outside any existing marquee
        TextToolMoving,   // dragging the inside of an existing marquee
        TextToolResizing  // dragging one of the 8 handles
    };
    enum TextToolHandle {
        HandleNone = 0,
        HandleTL, HandleT, HandleTR,
        HandleL,         HandleR,
        HandleBL, HandleB, HandleBR
    };
    bool m_textToolActive = false;
    bool m_textToolHasRect = false;
    TextToolInteraction m_textToolMode = TextToolIdle;
    TextToolHandle m_textToolActiveHandle = HandleNone;
    QRect m_textToolRect;           // committed rect in widget pixels
    QRect m_textToolDragStartRect;  // snapshot at mousePress for move/resize math
    QPoint m_textToolPressPos;
    QPoint m_textToolCurrentPos;
    // In-place typing state (Adobe-style). After a successful create-drag
    // the widget enters edit mode: a blinking caret is drawn inside the
    // rect, keyPressEvent appends to m_textToolInputText, Enter commits via
    // textInlineCommitted, Escape cancels.
    bool m_textToolEditing = false;
    QString m_textToolInputText;
    bool m_textToolCaretVisible = true;
    QTimer m_textToolCaretTimer;
    QFont m_textToolStyleFont;
    QColor m_textToolStyleColor = Qt::white;
    // When >= 0, the current edit session is updating an EXISTING overlay
    // at that index; Enter fires textOverlayEditCommitted. When -1, it's a
    // fresh create-drag and Enter fires textInlineCommitted.
    int m_textToolEditingIndex = -1;
    QVector<TextOverlayHit> m_textToolOverlayHits;

    // US-T34 Video source transform state. scale=1, dx=dy=0 is "fit to
    // letterbox" (pre-transform layout). dx/dy are fractions of the
    // letterbox width/height.
    double m_videoSourceScale = 1.0;
    double m_videoSourceDx = 0.0;
    double m_videoSourceDy = 0.0;
    bool m_compositeBakedMode = false;
    double m_snapStrength = 12.0;
    bool m_videoTransformSelected = false;
    enum VideoDragMode { VideoDragNone, VideoDragMoving, VideoDragResizing };
    VideoDragMode m_videoDragMode = VideoDragNone;
    TextToolHandle m_videoDragHandle = HandleNone;
    QPoint m_videoDragPressPos;
    double m_videoDragStartScale = 1.0;
    double m_videoDragStartDx = 0.0;
    double m_videoDragStartDy = 0.0;
    // Transformed video rect in widget logical pixels (for hit test + handles).
    QRectF videoDisplayRect() const;
    QRectF videoDisplayRectFor(double scale, double dx, double dy) const;
    TextToolHandle hitTestVideoHandle(const QPoint &pt) const;
    bool pointInsideVideoRect(const QPoint &pt) const;
    void enterTextToolEditMode();
    void exitTextToolEditMode();
    // Hit-test an existing text overlay at the given widget pixel. Returns
    // the first overlay whose rect (derived from normalized center + size)
    // contains the point, or -1 if no overlay is under the click.
    int hitTestExistingOverlay(const QPoint &widgetPos) const;
    // Hit-test a widget point against the committed rect's 8 handles and
    // inside-area. Returns HandleNone when the point isn't on the rect.
    TextToolHandle hitTestTextToolHandle(const QPoint &pt) const;
    bool pointInsideTextToolRect(const QPoint &pt) const;
    void emitCurrentTextToolRect();

    // Uniform locations
    int m_locTexture = -1;
    int m_locBrightness = -1;
    int m_locContrast = -1;
    int m_locSaturation = -1;
    int m_locHue = -1;
    int m_locTemperature = -1;
    int m_locTint = -1;
    int m_locGamma = -1;
    int m_locHighlights = -1;
    int m_locShadows = -1;
    int m_locExposure = -1;
    int m_locEffectsEnabled = -1;
    int m_locHdrTransfer = -1;
    int m_locClipOpacity = -1;
    int m_hdrTransfer = 0;  // 0=none, 1=PQ, 2=HLG

    QVector<VideoEffect> m_videoEffects;
    int m_locFxBlurEnable = -1,  m_locFxBlurRadius = -1,  m_locFxTexSize = -1;
    int m_locFxNoiseEnable = -1, m_locFxNoiseAmount = -1;
    int m_locFxSepiaEnable = -1, m_locFxSepiaStrength = -1;
    int m_locFxGrayEnable = -1,  m_locFxGrayStrength = -1;
    int m_locFxInvertEnable = -1, m_locFxInvertStrength = -1;
    int m_locFxVignetteEnable = -1, m_locFxVignetteIntensity = -1, m_locFxVignetteRadius = -1;
    int m_locFxSharpenEnable = -1, m_locFxSharpenAmount = -1;
    int m_locFxMosaicEnable = -1, m_locFxMosaicSize = -1;
    int m_locFxChromaKeyEnable = -1, m_locFxChromaKey = -1, m_locFxChromaTolerance = -1;
    int m_locFxTime = -1;
    QImage::Format m_textureFormat = QImage::Format_Invalid;

    // Lift/Gamma/Gain uniform locations (vec4: xyz=RGB, w=Luma)
    int m_locLift = -1, m_locLggGamma = -1, m_locGain = -1;
    // Identity: lift=(0,0,0,0), gamma=(1,1,1,1), gain=(1,1,1,1)
    std::array<std::array<double,4>,3> m_liftGammaGain = {{
        {0.0, 0.0, 0.0, 0.0},
        {1.0, 1.0, 1.0, 1.0},
        {1.0, 1.0, 1.0, 1.0}
    }};

    // LUT uniform locations and texture
    int m_locLut3D = -1;
    int m_locLutIntensity = -1;
    int m_locLutEnabled = -1;
    QOpenGLTexture *m_lutTexture = nullptr;
    float m_lutIntensity = 1.0f;
    bool m_lutEnabled = false;

    // US-CG-1: RGB Curves LUT (256x4 RGBA8 — rows R/G/B/Luma).
    // Sampled in the fragment shader between LGG and the .cube LUT.
    int m_locCurveLut = -1;
    int m_locCurvesEnabled = -1;
    QOpenGLTexture *m_curveLutTex = nullptr;
    bool m_curvesEnabled = false;
    QVector<QVector<int>> m_pendingCurves;   // queued upload (set before initializeGL)
    bool m_curvesNeedUpload = false;

    // US-CG-2: White-balance gain triple — applied at the very top of the
    // grade chain. Identity is (1, 1, 1) so existing pipelines keep working
    // until ColorGradingPanel emits a non-identity value.
    int m_locWb = -1;
    std::array<float, 3> m_wb = {1.0f, 1.0f, 1.0f};

    // US-CG-3: Radial vignette / Power Window — applied between curves and
    // the .cube LUT. amount=0 is identity (factor==1.0 → no-op).
    int m_locVigAmount = -1;
    int m_locVigMid = -1;
    int m_locVigRound = -1;
    int m_locVigFeather = -1;
    float m_vigAmount = 0.0f;
    float m_vigMid = 0.7f;
    float m_vigRound = 0.0f;
    float m_vigFeather = 0.3f;

    // US-EF-1: Chroma Key (HSL gating + spill suppression). Applied at the
    // very TOP of the compose path — BEFORE WB / LGG / curves / vignette /
    // .cube LUT. uChromaEnabled=false is a free no-op.
    int m_locChromaEnabled = -1;
    int m_locChromaKeyHsl  = -1;
    int m_locChromaTol     = -1;
    int m_locChromaSpill   = -1;
    int m_locChromaSoft    = -1;
    bool m_chromaEnabled = false;
    float m_chromaKeyH = 1.0f / 3.0f;  // pure green default (120°/360°)
    float m_chromaKeyS = 1.0f;
    float m_chromaKeyL = 0.5f;
    float m_chromaHueTol = 30.0f / 180.0f;
    float m_chromaSatTol = 0.6f;
    float m_chromaLumTol = 0.6f;
    float m_chromaSpillStrength = 0.4f;
    float m_chromaSoftness = 0.1f;

    // US-EF-3: HSL Qualifier (DaVinci-style secondary grading). Lives between
    // chroma key and WB. uHslqEnabled=false is a free no-op (entire stage is
    // skipped).
    int m_locHslqEnabled    = -1;
    int m_locHslqHueCenter  = -1;
    int m_locHslqHueRange   = -1;
    int m_locHslqSatRange   = -1;
    int m_locHslqLumaRange  = -1;
    int m_locHslqSoftness   = -1;
    int m_locHslqLift       = -1;
    int m_locHslqGamma      = -1;
    int m_locHslqGain       = -1;
    bool  m_hslqEnabled     = false;
    float m_hslqHueCenter   = 30.0f;          // skin tone default
    float m_hslqHueRange    = 30.0f;
    float m_hslqSatMin      = 0.30f;
    float m_hslqSatMax      = 1.00f;
    float m_hslqLumaMin     = 0.30f;
    float m_hslqLumaMax     = 0.80f;
    float m_hslqSoftness    = 10.0f;          // degrees / percent edge slop
    float m_hslqLift[3]     = {0.0f, 0.0f, 0.0f};
    float m_hslqGamma[3]    = {1.0f, 1.0f, 1.0f};
    float m_hslqGain[3]     = {1.0f, 1.0f, 1.0f};

    // US-CG-4: Hue vs Saturation curve. 256x1 R32F texture sampled in the
    // fragment shader; lookup is keyed on the HSL hue of the pixel and the
    // returned scalar is multiplied into the saturation channel before
    // converting back to RGB. uHueVsSatEnabled=false is a free no-op.
    int m_locHueVsSatEnabled = -1;
    int m_locHueVsSatLut     = -1;
    QOpenGLTexture *m_hueVsSatTex = nullptr;
    bool m_hueVsSatEnabled = false;
    QVector<float> m_pendingHueVsSatLut;
    bool m_hueVsSatNeedUpload = false;

    // US-EF-2: Mask Animation (Power Window simplified). uMaskEnabled=false
    // skips the entire wrap stage so the unmodified pre-mask `color` is
    // returned, preserving bit-identical output for the disabled state.
    int m_locMaskEnabled = -1;
    int m_locMaskEllipse = -1;
    int m_locMaskInvert  = -1;
    int m_locMaskFeather = -1;
    int m_locMaskRect    = -1;
    bool  m_maskEnabled  = false;
    bool  m_maskEllipse  = false;
    bool  m_maskInvert   = false;
    float m_maskFeather  = 0.10f;
    // (x, y, w, h) normalized to [0..1] in vTexCoord space.
    float m_maskRect[4]  = {0.25f, 0.25f, 0.50f, 0.50f};

    // US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens Distortion.
    // All three default to identity (no-op) so the stage is free until the
    // user touches the エフェクト sliders in ColorGradingPanel.
    int   m_locSharpenAmount  = -1;
    int   m_locBlurRadius     = -1;
    int   m_locLensDistortion = -1;
    float m_sharpenAmount  = 0.0f;
    float m_blurRadius     = 0.0f;
    float m_lensDistortion = 0.0f;

    // US-3D: 3-axis rotation + perspective foreshortening. The CPU side
    // builds a 3x3 rotation matrix from XYZ Euler angles (intrinsic
    // Tait-Bryan, R = Rx * Ry * Rz) and ships it to the fragment shader,
    // which warps the texture-coord sample at the top of main(). Identity
    // matrix (uRot3DEnabled=false) is a free no-op.
    int   m_locRot3DEnabled    = -1;
    int   m_locRot3D           = -1;
    int   m_locPerspectiveDist = -1;
    float m_rot3DMatrix[9]     = {1.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f};
    float m_perspectiveDist    = 2.0f;
    bool  m_rot3DEnabled       = false;

    // US-INT-4: per-clip stabilizer state. Keyframes are kept sorted by
    // timeUs; setStabilizerSourceTimeUs picks the active sample via
    // std::lower_bound and bakes the inverse 2D affine into m_stabMatrix.
    // Identity (m_stabEnabled=false) is a free no-op shader path.
    int   m_locStabEnabled = -1;
    int   m_locStab        = -1;
    float m_stabMatrix[9]  = {1.0f, 0.0f, 0.0f,
                              0.0f, 1.0f, 0.0f,
                              0.0f, 0.0f, 1.0f};
    bool  m_stabEnabled    = false;
    QVector<StabilizerKeyframe> m_stabKeyframes;
    qint64 m_stabSourceUs  = 0;
    int   m_stabFrameW     = 0;
    int   m_stabFrameH     = 0;

    // US-INT-1: non-owning Timeline pointer for adjustment-layer composition.
    Timeline *m_timeline = nullptr;

    // Phase 1e — m_interopDevice holds the wglDXOpenDeviceNV HANDLE once
    // Section B opens it lazily in paintGL; void* avoids leaking windows.h.
    // m_pendingD3D11Device is the ID3D11Device* the decoder gave us; the
    // paint callback opens / re-opens / closes m_interopDevice based on it.
    bool m_interopAvailable = false;
    void *m_interopDevice = nullptr;
    void *m_pendingD3D11Device = nullptr;
    void *m_currentInteropD3D11Device = nullptr;
    // Section C — pending zero-copy frame published by displayD3D11Frame.
    // Cleared after paintGL renders it.
    void *m_pendingD3D11Texture = nullptr;
    int m_pendingD3D11Subresource = 0;
    int m_pendingD3D11Width = 0;
    int m_pendingD3D11Height = 0;
    QOpenGLShaderProgram *m_nv12Program = nullptr;
    int m_locNv12TexY = -1;
    int m_locNv12TexUV = -1;

    // US-MOCHA-3: SurfaceTool for 4-corner planar tracking gizmo.
    SurfaceTool *m_surfaceTool = nullptr;
};
