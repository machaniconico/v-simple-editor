#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QFont>
#include <QColor>
#include "VideoEffect.h"
#include "LutImporter.h"

class GLPreview : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit GLPreview(QWidget *parent = nullptr);
    ~GLPreview();

    void displayFrame(const QImage &frame);
    void setDisplayAspectRatio(double aspectRatio);
    void setColorCorrection(const ColorCorrection &cc);
    // 0 = none (SDR sRGB), 1 = PQ (SMPTE ST 2084), 2 = HLG (ARIB STD-B67)
    void setHdrTransfer(int transfer);
    int hdrTransfer() const { return m_hdrTransfer; }
    // GPU-side stack: handles Blur/Noise/Sepia/Grayscale/Invert/Vignette.
    void setVideoEffects(const QVector<VideoEffect> &effects);
    void setEffectsEnabled(bool enabled) { m_effectsEnabled = enabled; update(); }
    bool effectsEnabled() const { return m_effectsEnabled; }
    void setLut(const LutData &lut);
    void clearLut();

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
    int m_hdrTransfer = 0;  // 0=none, 1=PQ, 2=HLG

    QVector<VideoEffect> m_videoEffects;
    int m_locFxBlurEnable = -1,  m_locFxBlurRadius = -1,  m_locFxTexSize = -1;
    int m_locFxNoiseEnable = -1, m_locFxNoiseAmount = -1;
    int m_locFxSepiaEnable = -1, m_locFxSepiaStrength = -1;
    int m_locFxGrayEnable = -1,  m_locFxGrayStrength = -1;
    int m_locFxInvertEnable = -1, m_locFxInvertStrength = -1;
    int m_locFxVignetteEnable = -1, m_locFxVignetteIntensity = -1, m_locFxVignetteRadius = -1;
    int m_locFxTime = -1;
    QImage::Format m_textureFormat = QImage::Format_Invalid;

    // Lift/Gamma/Gain uniform locations
    int m_locLiftR = -1, m_locLiftG = -1, m_locLiftB = -1;
    int m_locGammaR = -1, m_locGammaG = -1, m_locGammaB = -1;
    int m_locGainR = -1, m_locGainG = -1, m_locGainB = -1;

    // LUT uniform locations and texture
    int m_locLut3D = -1;
    int m_locLutIntensity = -1;
    int m_locLutEnabled = -1;
    QOpenGLTexture *m_lutTexture = nullptr;
    float m_lutIntensity = 1.0f;
    bool m_lutEnabled = false;

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
};
