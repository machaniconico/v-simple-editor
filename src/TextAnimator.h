#pragma once

#include <QChar>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QPointF>
#include <QString>
#include <QVector>

// --- Per-character animation type ---

enum class CharAnimationType {
    Typewriter,
    FadeInLetters,
    FadeOutLetters,
    BounceIn,
    SlideInLeft,
    SlideInRight,
    SlideInUp,
    SlideInDown,
    ScaleUp,
    ScaleDown,
    SpinIn,
    WaveMotion,
    RandomAppear,
    GlitchText,
    KaraokeHighlight
};

// --- Easing for per-character animations ---

enum class TextAnimEasing {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut
};

// --- Animation configuration ---

struct TextAnimConfig {
    CharAnimationType animation = CharAnimationType::FadeInLetters;
    double duration = 0.5;       // per-character animation duration (seconds)
    double delay = 0.05;         // stagger delay between characters (seconds)

    TextAnimEasing easing = TextAnimEasing::EaseOut;

    QColor customColor = QColor(255, 200, 0);  // for KaraokeHighlight etc.
    double amplitude = 10.0;     // for WaveMotion (pixels)
    bool reverse = false;        // play animation in reverse

    static QString animationName(CharAnimationType type);
    static CharAnimationType animationFromName(const QString &name);
    static QString easingName(TextAnimEasing easing);
    static TextAnimEasing easingFromName(const QString &name);
};

// --- Per-character state at a point in time ---

struct CharacterState {
    QChar character;
    QPointF position;        // base position (top-left of glyph)
    double opacity = 1.0;   // 0.0 to 1.0
    double scale = 1.0;
    double rotation = 0.0;  // degrees
    QColor color;            // current color (may differ from base during highlight)
    double offsetX = 0.0;   // additional offset from animation
    double offsetY = 0.0;
};

// --- Per-character Text Animator ---

class TextAnimator
{
public:
    TextAnimator();

    // --- Text setup ---

    void setText(const QString &text, const QFont &font, const QPointF &basePosition);

    const QString &text() const { return m_text; }
    const QFont &font() const { return m_font; }
    const QPointF &basePosition() const { return m_basePosition; }

    // --- Animation configuration ---

    void setAnimation(const TextAnimConfig &config);
    const TextAnimConfig &config() const { return m_config; }

    // --- Evaluation ---

    // Return animated state for every character at the given time
    QVector<CharacterState> getCharacterStates(double time) const;

    // --- Rendering ---

    // Render the animated text into a QImage with transparency
    QImage renderFrame(const QSize &canvasSize, double time) const;

    // --- Duration ---

    // Total animation duration including all per-character stagger delays
    double totalDuration() const;

    // --- Presets ---

    // Return all named preset animation configurations
    static QMap<QString, TextAnimConfig> presetAnimations();

    // --- Serialisation ---

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    // --- Character layout ---

    // Compute base X positions for each character using QFontMetrics
    void computeCharacterPositions();

    // --- Easing ---

    static double applyEasing(double t, TextAnimEasing easing);

    // --- Per-character animation progress ---

    // Returns 0.0 (not started) to 1.0 (complete) for a character at the given index
    double characterProgress(int charIndex, double time) const;

    // Returns the character index order (respecting RandomAppear shuffle)
    int characterOrder(int charIndex) const;

    // --- Per-animation-type transforms ---

    CharacterState applyTypewriter(const CharacterState &base, double progress) const;
    CharacterState applyFadeInLetters(const CharacterState &base, double progress) const;
    CharacterState applyFadeOutLetters(const CharacterState &base, double progress) const;
    CharacterState applyBounceIn(const CharacterState &base, double progress) const;
    CharacterState applySlideIn(const CharacterState &base, double progress,
                                double dirX, double dirY) const;
    CharacterState applyScaleUp(const CharacterState &base, double progress) const;
    CharacterState applyScaleDown(const CharacterState &base, double progress) const;
    CharacterState applySpinIn(const CharacterState &base, double progress) const;
    CharacterState applyWaveMotion(const CharacterState &base, int charIndex,
                                   double time) const;
    CharacterState applyRandomAppear(const CharacterState &base, double progress) const;
    CharacterState applyGlitchText(const CharacterState &base, double progress,
                                   int charIndex) const;
    CharacterState applyKaraokeHighlight(const CharacterState &base, double progress) const;

    // --- Bounce easing helper (overshoot then settle) ---

    static double bounceEaseOut(double t);

    // --- Data ---

    QString m_text;
    QFont m_font;
    QPointF m_basePosition;        // top-left origin for the text block
    QColor m_baseColor = Qt::white;

    TextAnimConfig m_config;

    QVector<double> m_charXOffsets; // per-character X offset from base position
    QVector<int> m_shuffleOrder;    // randomized order for RandomAppear
};
