#pragma once

#include <QString>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>

// --- Text Animation ---

enum class TextAnimationType {
    None,
    FadeIn,
    FadeOut,
    FadeInOut,
    SlideLeft,
    SlideRight,
    SlideUp,
    SlideDown,
    Typewriter,
    Bounce,
    ScaleIn,
    Pop
};

struct TextAnimation {
    TextAnimationType type = TextAnimationType::None;
    double duration = 0.5;  // seconds for in/out
    double delay = 0.0;     // delay before animation starts

    static QString typeName(TextAnimationType t);
    static QVector<TextAnimationType> allTypes();
};

// --- Drop Shadow ---

struct TextShadow {
    bool enabled = false;
    double offsetX = 3.0;
    double offsetY = 3.0;
    double blur = 4.0;
    QColor color = QColor(0, 0, 0, 180);
    double opacity = 1.0; // multiplied with color.alpha at render time
};

// --- Outer Glow ---

struct TextGlow {
    bool enabled = false;
    double radius = 8.0;
    QColor color = QColor(255, 255, 0, 255);
    double opacity = 0.9;
};

// --- Rich Text Segment ---

struct RichTextSegment {
    QString text;
    QFont font;       // override font for this segment (if different)
    QColor color;     // override color
    bool useCustomFont = false;
    bool useCustomColor = false;
};

// --- Ruby / Furigana ---

struct RubyAnnotation {
    int startIndex = 0;   // char index in base text
    int length = 1;       // number of chars annotated
    QString ruby;         // ruby text (furigana)
};

// --- Gradient Stop ---

struct GradientStop {
    double position = 0.0; // 0.0 - 1.0
    QColor color = Qt::white;
    double opacity = 1.0;  // 0.0 - 1.0, multiplied into color alpha at render time
};

// --- Position keyframe for motion-tracked overlays ---

struct PositionKeyframe {
    double time = 0.0;  // time offset in seconds from overlay start
    double cx = 0.5;    // normalized center x (0.0-1.0)
    double cy = 0.5;    // normalized center y (0.0-1.0)
};

// --- Enhanced Text Overlay ---

struct EnhancedTextOverlay {
    // Basic text
    QString text;
    QFont font = QFont("Arial", 32, QFont::Bold);
    QColor color = Qt::white;
    QColor backgroundColor = QColor(0, 0, 0, 160);
    double letterSpacing = 0.0; // QFont absolute spacing in pixels; 0 = legacy metrics
    double lineSpacing = 0.0;   // extra leading in pixels between laid-out lines; 0 = legacy metrics

    // Position & transform
    double x = 0.5;          // 0.0-1.0 normalized
    double y = 0.85;
    double width = 0.0;      // 0 = auto-size
    double height = 0.0;
    double rotation = 0.0;   // degrees
    double scale = 1.0;
    double opacity = 1.0;

    // Timing
    double startTime = 0.0;
    double endTime = 0.0;    // 0 = until end

    // Outline
    QColor outlineColor = Qt::black;
    int outlineWidth = 2;
    QColor outline2Color = QColor(0, 0, 0, 0); // second outline (0 alpha = disabled)
    int outline2Width = 0;

    // Gradient fill overrides `color` when enabled. Angle in degrees: 0=L→R, 90=T→B.
    // Legacy 2-stop fields (start/end/midpoint) remain for backward compat when
    // gradientStops is empty — rendering synthesizes a 2-stop list from them.
    bool gradientEnabled = false;
    QColor gradientStart = Qt::white;
    QColor gradientEnd = QColor(255, 200, 0);
    double gradientAngle = 90.0;
    int    gradientType = 0;          // 0=Linear, 1=Radial
    double gradientMidpoint = 50.0;   // 0-100 %, where the 50% blend occurs
    bool   gradientReverse = false;
    // Multi-stop gradient (Illustrator-style). Empty = fall back to legacy
    // start/end/midpoint pair above. Must have 2+ entries when populated.
    QVector<GradientStop> gradientStops;

    // Shadow
    TextShadow shadow;

    // Outer glow (rendered behind text/outline, in front of shadow). Skipped
    // entirely when glow.enabled=false so disabled overlays incur no cost.
    TextGlow glow;

    // Animation
    TextAnimation animIn;
    TextAnimation animOut;

    // Rich text segments (empty = use plain text)
    QVector<RichTextSegment> richSegments;

    // Ruby/Furigana annotations
    QVector<RubyAnnotation> rubyAnnotations;

    // Alignment
    int alignment = Qt::AlignCenter;
    bool wordWrap = true;
    bool visible = true;

    // Template name (if saved from template)
    QString templateName;

    // Motion-tracking position keyframes (per-frame normalized center)
    QVector<PositionKeyframe> positionKeyframes;
};

// --- Text Template ---

struct TextTemplate {
    QString name;
    QFont font;
    QColor color;
    QColor backgroundColor;
    QColor outlineColor;
    int outlineWidth;
    QColor outline2Color;
    int outline2Width;
    TextShadow shadow;
    TextGlow glow;
    TextAnimation animIn;
    TextAnimation animOut;
    double opacity;

    static TextTemplate fromOverlay(const EnhancedTextOverlay &overlay, const QString &name);
    EnhancedTextOverlay applyTo(const EnhancedTextOverlay &overlay) const;
};

// --- Text Manager ---

class TextManager
{
public:
    // Per-clip text overlays
    void addOverlay(const EnhancedTextOverlay &overlay);
    void removeOverlay(int index);
    void updateOverlay(int index, const EnhancedTextOverlay &overlay);
    int count() const { return m_overlays.size(); }
    const EnhancedTextOverlay &overlay(int index) const { return m_overlays[index]; }
    EnhancedTextOverlay &overlay(int index) { return m_overlays[index]; }
    const QVector<EnhancedTextOverlay> &overlays() const { return m_overlays; }
    void setOverlays(const QVector<EnhancedTextOverlay> &overlays) { m_overlays = overlays; }
    void moveOverlay(int from, int to);

    // Render all overlays at a given time
    void renderAll(QImage &frame, double currentTime) const;

    // Template management
    static void saveTemplate(const TextTemplate &tmpl);
    static void removeTemplate(const QString &name);
    static QVector<TextTemplate> loadTemplates();
    static TextTemplate defaultTemplate(const QString &name);

    // Subtitle import (SRT/VTT)
    static QVector<EnhancedTextOverlay> importSRT(const QString &filePath);
    static QVector<EnhancedTextOverlay> importVTT(const QString &filePath);
    // Subtitle / spreadsheet export — uses overlay.startTime / endTime
    // as the subtitle time range. Returns true on success.
    static bool exportSRT(const QVector<EnhancedTextOverlay> &overlays, const QString &filePath);
    static bool exportCSV(const QVector<EnhancedTextOverlay> &overlays, const QString &filePath);

    // Serialization
    static QJsonArray toJson(const QVector<EnhancedTextOverlay> &overlays);
    static QVector<EnhancedTextOverlay> fromJson(const QJsonArray &arr);

private:
    QVector<EnhancedTextOverlay> m_overlays;
};

// --- Enhanced Renderer ---

class EnhancedTextRenderer
{
public:
    static void render(QImage &frame, const EnhancedTextOverlay &overlay, double currentTime);

private:
    static double calcAnimationProgress(const EnhancedTextOverlay &overlay, double currentTime);
    static void applyAnimation(const TextAnimation &anim, double progress,
                                double &x, double &y, double &opacity, double &scale,
                                int textLen, int &visibleChars);
    static void renderShadow(QPainter &painter, const QString &text, const QRect &rect,
                              const EnhancedTextOverlay &overlay);
    static void renderGlow(QPainter &painter, const QString &text, const QRect &rect,
                            const EnhancedTextOverlay &overlay);
    static void renderOutline(QPainter &painter, const QString &text, const QRect &rect,
                               const EnhancedTextOverlay &overlay);
    static void renderRuby(QPainter &painter, const QString &baseText, const QRect &rect,
                            const QVector<RubyAnnotation> &annotations, const QFont &baseFont);
};
