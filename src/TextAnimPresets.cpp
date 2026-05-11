#include "TextAnimPresets.h"
#include "TextAnimator.h"

#include <QColor>

QStringList TextAnimPresets::presetNames()
{
    return QStringList{
        QStringLiteral("Random Fade Up"),
        QStringLiteral("Stomp"),
        QStringLiteral("Decode"),
        QStringLiteral("Cinematic 1"),
        QStringLiteral("Typewriter Sound"),
        QStringLiteral("Scale Bounce"),
        QStringLiteral("Spin In Each Word"),
        QStringLiteral("Wave Up"),
        QStringLiteral("Karaoke"),
        QStringLiteral("Slide From Left"),
        QStringLiteral("Glitch In"),
        QStringLiteral("Fade In Lines"),
        QStringLiteral("Bounce In Each"),
        QStringLiteral("Drift Down"),
        QStringLiteral("Snap Pop")
    };
}

QString TextAnimPresets::presetDescription(const QString &presetName)
{
    if (presetName == QStringLiteral("Random Fade Up"))
        return QStringLiteral("Characters appear in random order with smooth fade-in and 50ms stagger.");
    if (presetName == QStringLiteral("Stomp"))
        return QStringLiteral("Heavy bounce-in with exaggerated scale overshoot for impactful text hits.");
    if (presetName == QStringLiteral("Decode"))
        return QStringLiteral("Glitch-text flash combined with random character appearance for a digital decode look.");
    if (presetName == QStringLiteral("Cinematic 1"))
        return QStringLiteral("Typewriter reveal with smooth fade-in letters for a cinematic subtitle feel.");
    if (presetName == QStringLiteral("Typewriter Sound"))
        return QStringLiteral("Classic typewriter effect with linear per-character timing.");
    if (presetName == QStringLiteral("Scale Bounce"))
        return QStringLiteral("Scale-up with bounce easing for a playful pop-in per character.");
    if (presetName == QStringLiteral("Spin In Each Word"))
        return QStringLiteral("Each word spins into place; gracefully degrades to per-character spin if word scope unavailable.");
    if (presetName == QStringLiteral("Wave Up"))
        return QStringLiteral("Characters slide up with continuous wave motion overlay.");
    if (presetName == QStringLiteral("Karaoke"))
        return QStringLiteral("Highlight sweeps across text in karaoke style with warm gold color.");
    if (presetName == QStringLiteral("Slide From Left"))
        return QStringLiteral("Characters slide in from left with smooth range selector easing.");
    if (presetName == QStringLiteral("Glitch In"))
        return QStringLiteral("Quick glitch flash as characters appear, settling into place.");
    if (presetName == QStringLiteral("Fade In Lines"))
        return QStringLiteral("Lines fade in sequentially; degrades gracefully to character scope if line scope unavailable.");
    if (presetName == QStringLiteral("Bounce In Each"))
        return QStringLiteral("Each character bounces in with generous 60ms stagger between them.");
    if (presetName == QStringLiteral("Drift Down"))
        return QStringLiteral("Characters slowly drift downward while fading out for a dreamy exit.");
    if (presetName == QStringLiteral("Snap Pop"))
        return QStringLiteral("Quick scale pop with a glitch-text flash for a snappy reveal.");
    return QString();
}

bool TextAnimPresets::applyPreset(TextAnimator *animator, const QString &presetName)
{
    if (!animator)
        return false;

    TextAnimConfig cfg = animator->config();
    AnimatorRange range = animator->animatorRange();
    WigglySelector wiggly = animator->wigglySelector();

    bool modified = false;

    if (presetName == QStringLiteral("Random Fade Up")) {
        cfg.animation = CharAnimationType::RandomAppear;
        cfg.easing = TextAnimEasing::EaseOut;
        cfg.duration = 0.3;
        cfg.delay = 0.05;
        range.ease = TextAnimEasing::EaseOut;
        range.smoothness = 0.3;
        modified = true;
    }
    else if (presetName == QStringLiteral("Stomp")) {
        cfg.animation = CharAnimationType::BounceIn;
        cfg.duration = 0.5;
        cfg.delay = 0.04;
        cfg.amplitude = 25.0;
        range.smoothness = 0.0;
        modified = true;
    }
    else if (presetName == QStringLiteral("Decode")) {
        cfg.animation = CharAnimationType::GlitchText;
        cfg.duration = 0.4;
        cfg.delay = 0.03;
        cfg.easing = TextAnimEasing::Linear;
        wiggly.enabled = true;
        wiggly.maxAmount = 1.0;
        wiggly.minAmount = 0.0;
        wiggly.wigglesPerSec = 8.0;
        wiggly.correlation = 0.0;
        wiggly.seed = 42;
        modified = true;
    }
    else if (presetName == QStringLiteral("Cinematic 1")) {
        cfg.animation = CharAnimationType::Typewriter;
        cfg.duration = 0.01;
        cfg.delay = 0.07;
        cfg.easing = TextAnimEasing::Linear;
        wiggly.enabled = true;
        wiggly.maxAmount = 1.0;
        wiggly.minAmount = 0.5;
        wiggly.wigglesPerSec = 0.5;
        wiggly.correlation = 0.8;
        modified = true;
    }
    else if (presetName == QStringLiteral("Typewriter Sound")) {
        cfg.animation = CharAnimationType::Typewriter;
        cfg.duration = 0.01;
        cfg.delay = 0.06;
        cfg.easing = TextAnimEasing::Linear;
        range.smoothness = 0.0;
        modified = true;
    }
    else if (presetName == QStringLiteral("Scale Bounce")) {
        cfg.animation = CharAnimationType::ScaleUp;
        cfg.duration = 0.5;
        cfg.delay = 0.05;
        cfg.easing = TextAnimEasing::EaseOut;
        wiggly.enabled = true;
        wiggly.maxAmount = 1.0;
        wiggly.minAmount = 0.6;
        wiggly.wigglesPerSec = 2.0;
        wiggly.correlation = 0.5;
        modified = true;
    }
    else if (presetName == QStringLiteral("Spin In Each Word")) {
        cfg.animation = CharAnimationType::SpinIn;
        cfg.duration = 0.6;
        cfg.delay = 0.08;
        cfg.easing = TextAnimEasing::EaseOut;
        range.basedOn = TextAnimBasedOn::Words;
        range.smoothness = 0.2;
        modified = true;
    }
    else if (presetName == QStringLiteral("Wave Up")) {
        cfg.animation = CharAnimationType::WaveMotion;
        cfg.duration = 1.0;
        cfg.delay = 0.0;
        cfg.amplitude = 15.0;
        wiggly.enabled = true;
        wiggly.maxAmount = 1.0;
        wiggly.minAmount = 0.3;
        wiggly.wigglesPerSec = 1.5;
        wiggly.correlation = 0.3;
        range.ease = TextAnimEasing::EaseOut;
        modified = true;
    }
    else if (presetName == QStringLiteral("Karaoke")) {
        cfg.animation = CharAnimationType::KaraokeHighlight;
        cfg.duration = 0.15;
        cfg.delay = 0.1;
        cfg.easing = TextAnimEasing::Linear;
        cfg.customColor = QColor(255, 200, 0);
        range.basedOn = TextAnimBasedOn::Characters;
        range.smoothness = 0.5;
        modified = true;
    }
    else if (presetName == QStringLiteral("Slide From Left")) {
        cfg.animation = CharAnimationType::SlideInLeft;
        cfg.duration = 0.4;
        cfg.delay = 0.03;
        cfg.easing = TextAnimEasing::EaseOut;
        range.smoothness = 0.4;
        range.ease = TextAnimEasing::EaseInOut;
        modified = true;
    }
    else if (presetName == QStringLiteral("Glitch In")) {
        cfg.animation = CharAnimationType::GlitchText;
        cfg.duration = 0.3;
        cfg.delay = 0.02;
        cfg.easing = TextAnimEasing::Linear;
        wiggly.enabled = true;
        wiggly.maxAmount = 1.0;
        wiggly.minAmount = 0.0;
        wiggly.wigglesPerSec = 12.0;
        wiggly.correlation = 0.0;
        wiggly.seed = 7;
        modified = true;
    }
    else if (presetName == QStringLiteral("Fade In Lines")) {
        cfg.animation = CharAnimationType::FadeInLetters;
        cfg.duration = 0.5;
        cfg.delay = 0.15;
        cfg.easing = TextAnimEasing::EaseOut;
        range.basedOn = TextAnimBasedOn::Lines;
        range.smoothness = 0.3;
        modified = true;
    }
    else if (presetName == QStringLiteral("Bounce In Each")) {
        cfg.animation = CharAnimationType::BounceIn;
        cfg.duration = 0.5;
        cfg.delay = 0.06;
        cfg.amplitude = 15.0;
        range.smoothness = 0.0;
        modified = true;
    }
    else if (presetName == QStringLiteral("Drift Down")) {
        cfg.animation = CharAnimationType::FadeOutLetters;
        cfg.duration = 1.2;
        cfg.delay = 0.08;
        cfg.easing = TextAnimEasing::EaseIn;
        cfg.reverse = true;
        wiggly.enabled = true;
        wiggly.maxAmount = 0.5;
        wiggly.minAmount = 0.0;
        wiggly.wigglesPerSec = 0.5;
        wiggly.correlation = 0.7;
        modified = true;
    }
    else if (presetName == QStringLiteral("Snap Pop")) {
        cfg.animation = CharAnimationType::ScaleUp;
        cfg.duration = 0.2;
        cfg.delay = 0.03;
        cfg.easing = TextAnimEasing::EaseOut;
        wiggly.enabled = true;
        wiggly.maxAmount = 1.0;
        wiggly.minAmount = 0.0;
        wiggly.wigglesPerSec = 15.0;
        wiggly.correlation = 0.0;
        wiggly.seed = 99;
        modified = true;
    }

    if (!modified)
        return false;

    animator->setAnimation(cfg);
    animator->setAnimatorRange(range);
    animator->setWigglySelector(wiggly);
    return true;
}
