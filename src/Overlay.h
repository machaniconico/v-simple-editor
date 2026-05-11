#pragma once

#include <QString>
#include <QColor>
#include <QFont>
#include <QRectF>
#include <QImage>
#include <QVector>
#include <QPolygonF>

class BrushAnimation;

// --- Text Overlay (Telop) ---

struct TextOverlay {
    QString text;
    QFont font = QFont("Arial", 32, QFont::Bold);
    QColor color = Qt::white;
    QColor backgroundColor = QColor(0, 0, 0, 160);
    QColor outlineColor = Qt::black;
    int outlineWidth = 2;
    double x = 0.5;      // 0.0-1.0 normalized position
    double y = 0.85;     // 0.0-1.0 normalized position
    double startTime = 0.0;
    double endTime = 0.0; // 0 = until end of clip
    int alignment = Qt::AlignCenter;
    bool visible = true;

    // Drop shadow (rendered behind the text). Disabled by default — when
    // shadow=false the renderer must skip the offscreen buffer entirely.
    bool   shadow = false;
    double shadowOffsetX = 4.0;
    double shadowOffsetY = 4.0;
    double shadowBlur    = 6.0;
    QColor shadowColor   = QColor(0, 0, 0, 200);
    double shadowOpacity = 0.8;

    // Outer glow (rendered behind shadow). Disabled by default.
    bool   glow = false;
    double glowRadius  = 8.0;
    QColor glowColor   = QColor(255, 255, 0, 255);
    double glowOpacity = 0.9;
};

// --- Transition ---

enum class TransitionType {
    None,
    FadeIn,
    FadeOut,
    CrossDissolve,
    WipeLeft,
    WipeRight,
    WipeUp,
    WipeDown,
    SlideLeft,
    SlideRight,
    SlideUp,
    SlideDown,
    DipToBlack,
    DipToWhite,
    IrisRound,
    IrisBox,
    ClockWipe,
    BarnDoorHorizontal,
    BarnDoorVertical,
    PushLeft,
    PushRight,
    PushUp,
    PushDown,
    CrossZoom,
    FilmDissolve,
    SpinCW,
    SpinCCW,
    DitherDissolve,
    IrisRoundClose,
    IrisBoxClose,
    BarnDoorHClose,
    BarnDoorVClose,
    ClockWipeCCW,
    WhipPanLeft,
    WhipPanRight,
    Glitch,
    LightLeak,
    FlipHorizontal,
    FlipVertical,
    LensFlare,
    FilmBurn,
    Pixelate,
    BlurDissolve,
    CameraShake,
    ColorChannelShift
};

// True when the type renders as a "boundary" blend between two clips:
// the timeline overlaps the pair and both frames are needed at every
// tick during the transition window. False for FadeIn/FadeOut which
// blend a single clip against black with no neighbour involvement.
inline bool isOverlapTransition(TransitionType t) {
    switch (t) {
        case TransitionType::CrossDissolve:
        case TransitionType::WipeLeft:
        case TransitionType::WipeRight:
        case TransitionType::WipeUp:
        case TransitionType::WipeDown:
        case TransitionType::SlideLeft:
        case TransitionType::SlideRight:
        case TransitionType::SlideUp:
        case TransitionType::SlideDown:
        case TransitionType::DipToBlack:
        case TransitionType::DipToWhite:
        case TransitionType::IrisRound:
        case TransitionType::IrisBox:
        case TransitionType::ClockWipe:
        case TransitionType::BarnDoorHorizontal:
        case TransitionType::BarnDoorVertical:
        case TransitionType::PushLeft:
        case TransitionType::PushRight:
        case TransitionType::PushUp:
        case TransitionType::PushDown:
        case TransitionType::CrossZoom:
        case TransitionType::FilmDissolve:
        case TransitionType::SpinCW:
        case TransitionType::SpinCCW:
        case TransitionType::DitherDissolve:
        case TransitionType::IrisRoundClose:
        case TransitionType::IrisBoxClose:
        case TransitionType::BarnDoorHClose:
        case TransitionType::BarnDoorVClose:
        case TransitionType::ClockWipeCCW:
        case TransitionType::WhipPanLeft:
        case TransitionType::WhipPanRight:
        case TransitionType::Glitch:
        case TransitionType::LightLeak:
        case TransitionType::FlipHorizontal:
        case TransitionType::FlipVertical:
        case TransitionType::LensFlare:
        case TransitionType::FilmBurn:
        case TransitionType::Pixelate:
        case TransitionType::BlurDissolve:
        case TransitionType::CameraShake:
        case TransitionType::ColorChannelShift:
            return true;
        default:
            return false;
    }
}

// Progress shaping curve. Pro NLEs default to Linear; Ease* curves give a
// more natural feel — slow at the edges, fast in the middle (EaseInOut),
// or accelerating / decelerating motion. Applied uniformly to every
// transition type so the easing choice composes with type.
enum class TransitionEasing {
    Linear,
    EaseIn,    // slow start, fast end (cubic-in)
    EaseOut,   // fast start, slow end (cubic-out)
    EaseInOut  // slow at both ends, fast in the middle (cubic-inout)
};

// Apply the easing curve to a linear progress value in [0,1]. Output is
// also in [0,1]. The cubic family is cheap (no transcendentals) and what
// Premiere/After Effects use as defaults for keyframe interpolation.
inline double applyEasing(double progress, TransitionEasing easing) {
    if (progress <= 0.0) return 0.0;
    if (progress >= 1.0) return 1.0;
    switch (easing) {
        case TransitionEasing::Linear:    return progress;
        case TransitionEasing::EaseIn:    return progress * progress * progress;
        case TransitionEasing::EaseOut: {
            const double inv = 1.0 - progress;
            return 1.0 - inv * inv * inv;
        }
        case TransitionEasing::EaseInOut: {
            if (progress < 0.5) return 4.0 * progress * progress * progress;
            const double inv = 1.0 - progress;
            return 1.0 - 4.0 * inv * inv * inv;
        }
    }
    return progress;
}

// Alignment of an overlap transition relative to the cut between the two
// clips it lives on. Premiere terminology — Center is the pro-NLE default.
enum class TransitionAlignment {
    Center, // half before cut (A.trailHandle), half after (B.leadHandle)
    Start,  // entire transition AFTER the cut, consumes A.trailHandle
    End,    // entire transition BEFORE the cut, consumes B.leadHandle
};

struct Transition {
    TransitionType type = TransitionType::None;
    double duration = 0.5; // seconds
    TransitionAlignment alignment = TransitionAlignment::Center;
    TransitionEasing easing = TransitionEasing::Linear;

    static QString typeName(TransitionType t) {
        switch (t) {
            case TransitionType::None:               return "None";
            case TransitionType::FadeIn:             return "Fade In";
            case TransitionType::FadeOut:            return "Fade Out";
            case TransitionType::CrossDissolve:      return "Cross Dissolve";
            case TransitionType::WipeLeft:           return "Wipe Left";
            case TransitionType::WipeRight:          return "Wipe Right";
            case TransitionType::WipeUp:             return "Wipe Up";
            case TransitionType::WipeDown:           return "Wipe Down";
            case TransitionType::SlideLeft:          return "Slide Left";
            case TransitionType::SlideRight:         return "Slide Right";
            case TransitionType::SlideUp:            return "Slide Up";
            case TransitionType::SlideDown:          return "Slide Down";
            case TransitionType::DipToBlack:         return "Dip to Black";
            case TransitionType::DipToWhite:         return "Dip to White";
            case TransitionType::IrisRound:          return "Iris Round";
            case TransitionType::IrisBox:            return "Iris Box";
            case TransitionType::ClockWipe:          return "Clock Wipe";
            case TransitionType::BarnDoorHorizontal: return "Barn Door (H)";
            case TransitionType::BarnDoorVertical:   return "Barn Door (V)";
            case TransitionType::PushLeft:           return "Push Left";
            case TransitionType::PushRight:          return "Push Right";
            case TransitionType::PushUp:             return "Push Up";
            case TransitionType::PushDown:           return "Push Down";
            case TransitionType::CrossZoom:          return "Cross Zoom";
            case TransitionType::FilmDissolve:       return "Film Dissolve";
            case TransitionType::SpinCW:             return "Spin CW";
            case TransitionType::SpinCCW:            return "Spin CCW";
            case TransitionType::DitherDissolve:     return "Dither Dissolve";
            case TransitionType::IrisRoundClose:     return "Iris Round (Close)";
            case TransitionType::IrisBoxClose:       return "Iris Box (Close)";
            case TransitionType::BarnDoorHClose:     return "Barn Door H (Close)";
            case TransitionType::BarnDoorVClose:     return "Barn Door V (Close)";
            case TransitionType::ClockWipeCCW:       return "Clock Wipe (CCW)";
            case TransitionType::WhipPanLeft:        return "Whip Pan Left";
            case TransitionType::WhipPanRight:       return "Whip Pan Right";
            case TransitionType::Glitch:             return "Glitch";
            case TransitionType::LightLeak:          return "Light Leak";
            case TransitionType::FlipHorizontal:     return "Flip Horizontal";
            case TransitionType::FlipVertical:       return "Flip Vertical";
            case TransitionType::LensFlare:          return "Lens Flare";
            case TransitionType::FilmBurn:           return "Film Burn";
            case TransitionType::Pixelate:           return "Pixelate";
            case TransitionType::BlurDissolve:       return "Blur Dissolve";
            case TransitionType::CameraShake:        return "Camera Shake";
            case TransitionType::ColorChannelShift:  return "Color Channel Shift";
        }
        return "Unknown";
    }

    static QString easingName(TransitionEasing e) {
        switch (e) {
            case TransitionEasing::Linear:    return "Linear";
            case TransitionEasing::EaseIn:    return "Ease In";
            case TransitionEasing::EaseOut:   return "Ease Out";
            case TransitionEasing::EaseInOut: return "Ease In/Out";
        }
        return "Linear";
    }
};

// --- Transition Preset ---

// User-named saved transition. Persisted in QSettings under
// "transitionPresets/<index>/name|type|duration|alignment|easing". Pro
// NLEs (Premiere effect presets, FCP X effect templates) all expose this
// pattern — pick a transition once, name it, recall it from a menu.
struct TransitionPreset {
    QString name;
    Transition transition;
};

// --- Image Overlay ---

struct ImageOverlay {
    QString filePath;
    QRectF rect = QRectF(0.1, 0.1, 0.3, 0.3); // normalized x,y,w,h
    double startTime = 0.0;
    double endTime = 0.0;
    double opacity = 1.0;
    bool keepAspectRatio = true;
    bool visible = true;

    // Planar tracking render quad (4 corners in pixel coordinates).
    // When non-empty, overrides the axis-aligned rect for rendering.
    QPolygonF renderQuad;

    void setRenderQuad(const QPolygonF &quad) { renderQuad = quad; }
};

// --- Picture in Picture ---

struct PipConfig {
    int sourceClipIndex = -1;
    QRectF rect = QRectF(0.65, 0.05, 0.3, 0.3); // bottom-right corner default
    double opacity = 1.0;
    int borderWidth = 2;
    QColor borderColor = Qt::white;
    double startTime = 0.0;
    double endTime = 0.0;
    bool visible = true;

    enum Position { TopLeft, TopRight, BottomLeft, BottomRight, Custom };
    static QRectF presetRect(Position pos) {
        switch (pos) {
            case TopLeft:     return QRectF(0.05, 0.05, 0.3, 0.3);
            case TopRight:    return QRectF(0.65, 0.05, 0.3, 0.3);
            case BottomLeft:  return QRectF(0.05, 0.65, 0.3, 0.3);
            case BottomRight: return QRectF(0.65, 0.65, 0.3, 0.3);
            default:          return QRectF(0.65, 0.05, 0.3, 0.3);
        }
    }
};

// --- Brush / Write-On Overlay ---

struct BrushOverlay {
    BrushAnimation *animation = nullptr;
    double startTime = 0.0;
    double durationSec = 0.0;
    bool visible = true;

    void setBrushAnimation(BrushAnimation *brushAnimation) { animation = brushAnimation; }
    BrushAnimation *brushAnimation() const { return animation; }
};

// --- Composite render layer ---

class OverlayRenderer
{
public:
    static void renderTextOverlay(QImage &frame, const TextOverlay &overlay, double currentTime);
    static void renderImageOverlay(QImage &frame, const ImageOverlay &overlay, double currentTime);
    static void renderBrushOverlay(QImage &frame, const BrushOverlay &overlay, double currentTime);
    static void renderBrushOverlay(QImage &frame, BrushAnimation *brushAnimation, double progress);
    static void renderPip(QImage &frame, const QImage &pipSource, const PipConfig &config);
    static QImage applyTransition(const QImage &from, const QImage &to, TransitionType type, double progress);
};
