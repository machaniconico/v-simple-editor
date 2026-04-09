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

// --- Enhanced Text Overlay ---

struct EnhancedTextOverlay {
    // Basic text
    QString text;
    QFont font = QFont("Arial", 32, QFont::Bold);
    QColor color = Qt::white;
    QColor backgroundColor = QColor(0, 0, 0, 160);

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

    // Shadow
    TextShadow shadow;

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
    static void renderOutline(QPainter &painter, const QString &text, const QRect &rect,
                               const EnhancedTextOverlay &overlay);
    static void renderRuby(QPainter &painter, const QString &baseText, const QRect &rect,
                            const QVector<RubyAnnotation> &annotations, const QFont &baseFont);
};
