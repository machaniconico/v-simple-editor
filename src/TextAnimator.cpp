#include "TextAnimator.h"

#include <QPainter>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>

// --- TextAnimConfig helpers ---

QString TextAnimConfig::animationName(CharAnimationType type)
{
    switch (type) {
    case CharAnimationType::Typewriter:        return QStringLiteral("Typewriter");
    case CharAnimationType::FadeInLetters:     return QStringLiteral("FadeInLetters");
    case CharAnimationType::FadeOutLetters:    return QStringLiteral("FadeOutLetters");
    case CharAnimationType::BounceIn:          return QStringLiteral("BounceIn");
    case CharAnimationType::SlideInLeft:       return QStringLiteral("SlideInLeft");
    case CharAnimationType::SlideInRight:      return QStringLiteral("SlideInRight");
    case CharAnimationType::SlideInUp:         return QStringLiteral("SlideInUp");
    case CharAnimationType::SlideInDown:       return QStringLiteral("SlideInDown");
    case CharAnimationType::ScaleUp:           return QStringLiteral("ScaleUp");
    case CharAnimationType::ScaleDown:         return QStringLiteral("ScaleDown");
    case CharAnimationType::SpinIn:            return QStringLiteral("SpinIn");
    case CharAnimationType::WaveMotion:        return QStringLiteral("WaveMotion");
    case CharAnimationType::RandomAppear:      return QStringLiteral("RandomAppear");
    case CharAnimationType::GlitchText:        return QStringLiteral("GlitchText");
    case CharAnimationType::KaraokeHighlight:  return QStringLiteral("KaraokeHighlight");
    }
    return QStringLiteral("FadeInLetters");
}

CharAnimationType TextAnimConfig::animationFromName(const QString &name)
{
    if (name == QLatin1String("Typewriter"))        return CharAnimationType::Typewriter;
    if (name == QLatin1String("FadeInLetters"))     return CharAnimationType::FadeInLetters;
    if (name == QLatin1String("FadeOutLetters"))    return CharAnimationType::FadeOutLetters;
    if (name == QLatin1String("BounceIn"))          return CharAnimationType::BounceIn;
    if (name == QLatin1String("SlideInLeft"))       return CharAnimationType::SlideInLeft;
    if (name == QLatin1String("SlideInRight"))      return CharAnimationType::SlideInRight;
    if (name == QLatin1String("SlideInUp"))         return CharAnimationType::SlideInUp;
    if (name == QLatin1String("SlideInDown"))       return CharAnimationType::SlideInDown;
    if (name == QLatin1String("ScaleUp"))           return CharAnimationType::ScaleUp;
    if (name == QLatin1String("ScaleDown"))         return CharAnimationType::ScaleDown;
    if (name == QLatin1String("SpinIn"))            return CharAnimationType::SpinIn;
    if (name == QLatin1String("WaveMotion"))        return CharAnimationType::WaveMotion;
    if (name == QLatin1String("RandomAppear"))      return CharAnimationType::RandomAppear;
    if (name == QLatin1String("GlitchText"))        return CharAnimationType::GlitchText;
    if (name == QLatin1String("KaraokeHighlight"))  return CharAnimationType::KaraokeHighlight;
    return CharAnimationType::FadeInLetters; // fallback
}

QString TextAnimConfig::easingName(TextAnimEasing easing)
{
    switch (easing) {
    case TextAnimEasing::Linear:    return QStringLiteral("Linear");
    case TextAnimEasing::EaseIn:    return QStringLiteral("EaseIn");
    case TextAnimEasing::EaseOut:   return QStringLiteral("EaseOut");
    case TextAnimEasing::EaseInOut: return QStringLiteral("EaseInOut");
    }
    return QStringLiteral("Linear");
}

TextAnimEasing TextAnimConfig::easingFromName(const QString &name)
{
    if (name == QLatin1String("EaseIn"))    return TextAnimEasing::EaseIn;
    if (name == QLatin1String("EaseOut"))   return TextAnimEasing::EaseOut;
    if (name == QLatin1String("EaseInOut")) return TextAnimEasing::EaseInOut;
    return TextAnimEasing::Linear; // fallback
}

// --- Constructor ---

TextAnimator::TextAnimator() = default;

// --- Text setup ---

void TextAnimator::setText(const QString &text, const QFont &font, const QPointF &basePosition)
{
    m_text = text;
    m_font = font;
    m_basePosition = basePosition;

    computeCharacterPositions();

    // Generate shuffle order for RandomAppear
    m_shuffleOrder.resize(m_text.size());
    for (int i = 0; i < m_text.size(); ++i)
        m_shuffleOrder[i] = i;

    // Deterministic shuffle using a seeded generator
    auto *rng = QRandomGenerator::global();
    for (int i = m_shuffleOrder.size() - 1; i > 0; --i) {
        int j = static_cast<int>(rng->bounded(i + 1));
        std::swap(m_shuffleOrder[i], m_shuffleOrder[j]);
    }
}

// --- Animation configuration ---

void TextAnimator::setAnimation(const TextAnimConfig &config)
{
    m_config = config;
}

// --- Character layout ---

void TextAnimator::computeCharacterPositions()
{
    m_charXOffsets.clear();
    if (m_text.isEmpty())
        return;

    m_charXOffsets.reserve(m_text.size());
    QFontMetricsF fm(m_font);

    double xAccum = 0.0;
    for (int i = 0; i < m_text.size(); ++i) {
        m_charXOffsets.append(xAccum);
        xAccum += fm.horizontalAdvance(m_text[i]);
    }
}

// --- Easing ---

double TextAnimator::applyEasing(double t, TextAnimEasing easing)
{
    t = std::clamp(t, 0.0, 1.0);

    switch (easing) {
    case TextAnimEasing::Linear:
        return t;
    case TextAnimEasing::EaseIn:
        return t * t;
    case TextAnimEasing::EaseOut:
        return 1.0 - (1.0 - t) * (1.0 - t);
    case TextAnimEasing::EaseInOut:
        return t < 0.5 ? 2.0 * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 2.0) / 2.0;
    }
    return t;
}

// --- Bounce easing (overshoot then settle) ---

double TextAnimator::bounceEaseOut(double t)
{
    if (t < 1.0 / 2.75) {
        return 7.5625 * t * t;
    } else if (t < 2.0 / 2.75) {
        t -= 1.5 / 2.75;
        return 7.5625 * t * t + 0.75;
    } else if (t < 2.5 / 2.75) {
        t -= 2.25 / 2.75;
        return 7.5625 * t * t + 0.9375;
    } else {
        t -= 2.625 / 2.75;
        return 7.5625 * t * t + 0.984375;
    }
}

// --- Per-character animation progress ---

int TextAnimator::characterOrder(int charIndex) const
{
    if (m_config.animation == CharAnimationType::RandomAppear
        && charIndex < m_shuffleOrder.size()) {
        return m_shuffleOrder[charIndex];
    }
    return charIndex;
}

double TextAnimator::characterProgress(int charIndex, double time) const
{
    int order = characterOrder(charIndex);
    double charStart = order * m_config.delay;

    if (m_config.duration <= 0.0)
        return (time >= charStart) ? 1.0 : 0.0;

    double raw = (time - charStart) / m_config.duration;
    raw = std::clamp(raw, 0.0, 1.0);

    if (m_config.reverse)
        raw = 1.0 - raw;

    return applyEasing(raw, m_config.easing);
}

// --- Per-animation-type transforms ---

CharacterState TextAnimator::applyTypewriter(const CharacterState &base, double progress) const
{
    CharacterState state = base;
    // Instant switch: invisible until progress > 0
    state.opacity = (progress > 0.0) ? 1.0 : 0.0;
    return state;
}

CharacterState TextAnimator::applyFadeInLetters(const CharacterState &base, double progress) const
{
    CharacterState state = base;
    state.opacity = progress;
    return state;
}

CharacterState TextAnimator::applyFadeOutLetters(const CharacterState &base, double progress) const
{
    CharacterState state = base;
    state.opacity = 1.0 - progress;
    return state;
}

CharacterState TextAnimator::applyBounceIn(const CharacterState &base, double progress) const
{
    CharacterState state = base;
    double bounced = bounceEaseOut(progress);
    // Drop from above: start at -amplitude, settle at 0
    state.offsetY = -(1.0 - bounced) * m_config.amplitude * 3.0;
    state.opacity = std::clamp(progress * 3.0, 0.0, 1.0);
    return state;
}

CharacterState TextAnimator::applySlideIn(const CharacterState &base, double progress,
                                           double dirX, double dirY) const
{
    CharacterState state = base;
    double dist = 100.0; // slide distance in pixels
    double remaining = 1.0 - progress;
    state.offsetX = dirX * dist * remaining;
    state.offsetY = dirY * dist * remaining;
    state.opacity = progress;
    return state;
}

CharacterState TextAnimator::applyScaleUp(const CharacterState &base, double progress) const
{
    CharacterState state = base;
    state.scale = progress;
    state.opacity = progress;
    return state;
}

CharacterState TextAnimator::applyScaleDown(const CharacterState &base, double progress) const
{
    CharacterState state = base;
    state.scale = 1.0 + (1.0 - progress); // 2.0 -> 1.0
    state.opacity = progress;
    return state;
}

CharacterState TextAnimator::applySpinIn(const CharacterState &base, double progress) const
{
    CharacterState state = base;
    state.rotation = 360.0 * (1.0 - progress);
    state.scale = progress;
    state.opacity = progress;
    return state;
}

CharacterState TextAnimator::applyWaveMotion(const CharacterState &base, int charIndex,
                                              double time) const
{
    CharacterState state = base;
    // Continuous sinusoidal wave — never settles, oscillates forever
    double phase = time * 4.0 + charIndex * 0.5;
    state.offsetY = std::sin(phase) * m_config.amplitude;
    return state;
}

CharacterState TextAnimator::applyRandomAppear(const CharacterState &base, double progress) const
{
    // Same as FadeIn but order is shuffled (handled by characterOrder)
    CharacterState state = base;
    state.opacity = progress;
    state.scale = 0.5 + 0.5 * progress;
    return state;
}

CharacterState TextAnimator::applyGlitchText(const CharacterState &base, double progress,
                                              int charIndex) const
{
    CharacterState state = base;

    if (progress >= 1.0) {
        // Settled
        return state;
    }

    // Deterministic pseudo-random jitter using charIndex + progress
    double jitterSeed = charIndex * 137.0 + progress * 1000.0;
    double jitterX = std::sin(jitterSeed) * (1.0 - progress) * 15.0;
    double jitterY = std::cos(jitterSeed * 1.3) * (1.0 - progress) * 10.0;

    state.offsetX = jitterX;
    state.offsetY = jitterY;
    state.opacity = (progress < 0.1)
                        ? (std::fmod(jitterSeed, 2.0) > 1.0 ? 1.0 : 0.0)
                        : std::clamp(progress, 0.0, 1.0);
    return state;
}

CharacterState TextAnimator::applyKaraokeHighlight(const CharacterState &base,
                                                    double progress) const
{
    CharacterState state = base;
    // Sweep color: once progress > 0, switch to custom color; full at 1.0
    if (progress > 0.0) {
        double blend = std::clamp(progress, 0.0, 1.0);
        int r = static_cast<int>(state.color.red()   * (1.0 - blend) + m_config.customColor.red()   * blend);
        int g = static_cast<int>(state.color.green() * (1.0 - blend) + m_config.customColor.green() * blend);
        int b = static_cast<int>(state.color.blue()  * (1.0 - blend) + m_config.customColor.blue()  * blend);
        int a = static_cast<int>(state.color.alpha() * (1.0 - blend) + m_config.customColor.alpha() * blend);
        state.color = QColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255),
                             std::clamp(b, 0, 255), std::clamp(a, 0, 255));
    }
    return state;
}

// --- Evaluation ---

QVector<CharacterState> TextAnimator::getCharacterStates(double time) const
{
    QVector<CharacterState> states;
    if (m_text.isEmpty())
        return states;

    states.reserve(m_text.size());
    QFontMetricsF fm(m_font);
    double baseY = m_basePosition.y();

    for (int i = 0; i < m_text.size(); ++i) {
        CharacterState base;
        base.character = m_text[i];
        base.position = QPointF(m_basePosition.x() + m_charXOffsets[i], baseY);
        base.opacity = 1.0;
        base.scale = 1.0;
        base.rotation = 0.0;
        base.color = m_baseColor;
        base.offsetX = 0.0;
        base.offsetY = 0.0;

        double progress = characterProgress(i, time);

        CharacterState animated;
        switch (m_config.animation) {
        case CharAnimationType::Typewriter:
            animated = applyTypewriter(base, progress);
            break;
        case CharAnimationType::FadeInLetters:
            animated = applyFadeInLetters(base, progress);
            break;
        case CharAnimationType::FadeOutLetters:
            animated = applyFadeOutLetters(base, progress);
            break;
        case CharAnimationType::BounceIn:
            animated = applyBounceIn(base, progress);
            break;
        case CharAnimationType::SlideInLeft:
            animated = applySlideIn(base, progress, -1.0, 0.0);
            break;
        case CharAnimationType::SlideInRight:
            animated = applySlideIn(base, progress, 1.0, 0.0);
            break;
        case CharAnimationType::SlideInUp:
            animated = applySlideIn(base, progress, 0.0, -1.0);
            break;
        case CharAnimationType::SlideInDown:
            animated = applySlideIn(base, progress, 0.0, 1.0);
            break;
        case CharAnimationType::ScaleUp:
            animated = applyScaleUp(base, progress);
            break;
        case CharAnimationType::ScaleDown:
            animated = applyScaleDown(base, progress);
            break;
        case CharAnimationType::SpinIn:
            animated = applySpinIn(base, progress);
            break;
        case CharAnimationType::WaveMotion:
            animated = applyWaveMotion(base, i, time);
            break;
        case CharAnimationType::RandomAppear:
            animated = applyRandomAppear(base, progress);
            break;
        case CharAnimationType::GlitchText:
            animated = applyGlitchText(base, progress, i);
            break;
        case CharAnimationType::KaraokeHighlight:
            animated = applyKaraokeHighlight(base, progress);
            break;
        }

        states.append(animated);
    }

    return states;
}

// --- Rendering ---

QImage TextAnimator::renderFrame(const QSize &canvasSize, double time) const
{
    QImage image(canvasSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    if (m_text.isEmpty())
        return image;

    QVector<CharacterState> states = getCharacterStates(time);
    QFontMetricsF fm(m_font);
    double fontHeight = fm.height();
    double ascent = fm.ascent();

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    for (const auto &cs : states) {
        if (cs.opacity <= 0.0)
            continue;

        painter.save();

        // Compute the draw position (center of character for rotation/scale)
        double charWidth = fm.horizontalAdvance(cs.character);
        double cx = cs.position.x() + cs.offsetX + charWidth / 2.0;
        double cy = cs.position.y() + cs.offsetY + fontHeight / 2.0;

        painter.translate(cx, cy);

        if (cs.rotation != 0.0)
            painter.rotate(cs.rotation);

        if (cs.scale != 1.0)
            painter.scale(cs.scale, cs.scale);

        painter.translate(-cx, -cy);

        painter.setOpacity(std::clamp(cs.opacity, 0.0, 1.0));
        painter.setFont(m_font);
        painter.setPen(cs.color);

        // Draw the character at its animated position
        double drawX = cs.position.x() + cs.offsetX;
        double drawY = cs.position.y() + cs.offsetY + ascent;
        painter.drawText(QPointF(drawX, drawY), QString(cs.character));

        painter.restore();
    }

    painter.end();
    return image;
}

// --- Duration ---

double TextAnimator::totalDuration() const
{
    if (m_text.isEmpty())
        return 0.0;

    // WaveMotion is continuous — return a nominal cycle duration
    if (m_config.animation == CharAnimationType::WaveMotion)
        return m_config.duration + m_config.delay * m_text.size();

    // Last character start + its animation duration
    int lastOrder = 0;
    for (int i = 0; i < m_text.size(); ++i) {
        int order = characterOrder(i);
        if (order > lastOrder)
            lastOrder = order;
    }

    return lastOrder * m_config.delay + m_config.duration;
}

// --- Presets ---

QMap<QString, TextAnimConfig> TextAnimator::presetAnimations()
{
    QMap<QString, TextAnimConfig> presets;

    // Typewriter
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::Typewriter;
        c.duration = 0.01;
        c.delay = 0.06;
        c.easing = TextAnimEasing::Linear;
        presets[QStringLiteral("Typewriter")] = c;
    }

    // Fade In Letters
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::FadeInLetters;
        c.duration = 0.4;
        c.delay = 0.04;
        c.easing = TextAnimEasing::EaseOut;
        presets[QStringLiteral("Fade In Letters")] = c;
    }

    // Fade Out Letters
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::FadeOutLetters;
        c.duration = 0.4;
        c.delay = 0.04;
        c.easing = TextAnimEasing::EaseIn;
        presets[QStringLiteral("Fade Out Letters")] = c;
    }

    // Bounce In
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::BounceIn;
        c.duration = 0.6;
        c.delay = 0.05;
        c.easing = TextAnimEasing::Linear; // bounce uses its own easing
        c.amplitude = 10.0;
        presets[QStringLiteral("Bounce In")] = c;
    }

    // Slide In Left
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::SlideInLeft;
        c.duration = 0.4;
        c.delay = 0.03;
        c.easing = TextAnimEasing::EaseOut;
        presets[QStringLiteral("Slide In Left")] = c;
    }

    // Slide In Right
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::SlideInRight;
        c.duration = 0.4;
        c.delay = 0.03;
        c.easing = TextAnimEasing::EaseOut;
        presets[QStringLiteral("Slide In Right")] = c;
    }

    // Slide In Up
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::SlideInUp;
        c.duration = 0.4;
        c.delay = 0.03;
        c.easing = TextAnimEasing::EaseOut;
        presets[QStringLiteral("Slide In Up")] = c;
    }

    // Slide In Down
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::SlideInDown;
        c.duration = 0.4;
        c.delay = 0.03;
        c.easing = TextAnimEasing::EaseOut;
        presets[QStringLiteral("Slide In Down")] = c;
    }

    // Scale Up
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::ScaleUp;
        c.duration = 0.5;
        c.delay = 0.04;
        c.easing = TextAnimEasing::EaseOut;
        presets[QStringLiteral("Scale Up")] = c;
    }

    // Scale Down
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::ScaleDown;
        c.duration = 0.5;
        c.delay = 0.04;
        c.easing = TextAnimEasing::EaseOut;
        presets[QStringLiteral("Scale Down")] = c;
    }

    // Spin In
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::SpinIn;
        c.duration = 0.6;
        c.delay = 0.06;
        c.easing = TextAnimEasing::EaseOut;
        presets[QStringLiteral("Spin In")] = c;
    }

    // Wave Motion
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::WaveMotion;
        c.duration = 1.0;
        c.delay = 0.0;
        c.easing = TextAnimEasing::Linear;
        c.amplitude = 10.0;
        presets[QStringLiteral("Wave Motion")] = c;
    }

    // Random Appear
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::RandomAppear;
        c.duration = 0.3;
        c.delay = 0.06;
        c.easing = TextAnimEasing::EaseOut;
        presets[QStringLiteral("Random Appear")] = c;
    }

    // Glitch Text
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::GlitchText;
        c.duration = 0.5;
        c.delay = 0.02;
        c.easing = TextAnimEasing::Linear;
        presets[QStringLiteral("Glitch Text")] = c;
    }

    // Karaoke Highlight
    {
        TextAnimConfig c;
        c.animation = CharAnimationType::KaraokeHighlight;
        c.duration = 0.2;
        c.delay = 0.08;
        c.easing = TextAnimEasing::Linear;
        c.customColor = QColor(255, 200, 0);
        presets[QStringLiteral("Karaoke Highlight")] = c;
    }

    return presets;
}

// --- Serialisation ---

QJsonObject TextAnimator::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("text")] = m_text;
    obj[QStringLiteral("baseX")] = m_basePosition.x();
    obj[QStringLiteral("baseY")] = m_basePosition.y();

    // Font
    QJsonObject fontObj;
    fontObj[QStringLiteral("family")] = m_font.family();
    fontObj[QStringLiteral("pointSize")] = m_font.pointSize();
    fontObj[QStringLiteral("weight")] = m_font.weight();
    fontObj[QStringLiteral("italic")] = m_font.italic();
    obj[QStringLiteral("font")] = fontObj;

    // Animation config
    QJsonObject animObj;
    animObj[QStringLiteral("type")] = TextAnimConfig::animationName(m_config.animation);
    animObj[QStringLiteral("duration")] = m_config.duration;
    animObj[QStringLiteral("delay")] = m_config.delay;
    animObj[QStringLiteral("easing")] = TextAnimConfig::easingName(m_config.easing);
    animObj[QStringLiteral("customColor")] = m_config.customColor.name(QColor::HexArgb);
    animObj[QStringLiteral("amplitude")] = m_config.amplitude;
    animObj[QStringLiteral("reverse")] = m_config.reverse;
    obj[QStringLiteral("animation")] = animObj;

    return obj;
}

void TextAnimator::fromJson(const QJsonObject &obj)
{
    QString text = obj[QStringLiteral("text")].toString();
    double baseX = obj[QStringLiteral("baseX")].toDouble();
    double baseY = obj[QStringLiteral("baseY")].toDouble();

    // Font
    QJsonObject fontObj = obj[QStringLiteral("font")].toObject();
    QFont font;
    font.setFamily(fontObj[QStringLiteral("family")].toString(QStringLiteral("Arial")));
    font.setPointSize(fontObj[QStringLiteral("pointSize")].toInt(32));
    font.setWeight(static_cast<QFont::Weight>(fontObj[QStringLiteral("weight")].toInt(QFont::Bold)));
    font.setItalic(fontObj[QStringLiteral("italic")].toBool(false));

    setText(text, font, QPointF(baseX, baseY));

    // Animation config
    QJsonObject animObj = obj[QStringLiteral("animation")].toObject();
    TextAnimConfig config;
    config.animation = TextAnimConfig::animationFromName(
        animObj[QStringLiteral("type")].toString(QStringLiteral("FadeInLetters")));
    config.duration = animObj[QStringLiteral("duration")].toDouble(0.5);
    config.delay = animObj[QStringLiteral("delay")].toDouble(0.05);
    config.easing = TextAnimConfig::easingFromName(
        animObj[QStringLiteral("easing")].toString(QStringLiteral("EaseOut")));
    config.customColor = QColor(
        animObj[QStringLiteral("customColor")].toString(QStringLiteral("#ffffff00")));
    config.amplitude = animObj[QStringLiteral("amplitude")].toDouble(10.0);
    config.reverse = animObj[QStringLiteral("reverse")].toBool(false);

    setAnimation(config);
}
