#pragma once

// Data structures for the title preset / template system.
//
// A TitlePreset describes a single pre-built animated title (font + colour +
// in/out animation + optional position keyframes for slide / lower-third).
// The preset never modifies TextManager.h — it only emits an
// EnhancedTextOverlay with the relevant fields populated.

#include <QString>
#include <QFont>
#include <QColor>
#include <QVector>

#include "TextManager.h"

enum class TitlePresetId {
    SimpleCenter = 0,   // fade-in / hold / fade-out, centred
    LowerThird,         // slide-from-left, lower-third
    TitleScale,         // scale 0->1->1->0
    Typewriter,         // letters appear one at a time
    SpinIn,             // rotation in
    GlowPulse,          // alpha pulses (sin wave) on hold
    DropShadowSlide     // optional 7th — slide-in lower-third with shadow
};

struct TitlePreset {
    TitlePresetId id = TitlePresetId::SimpleCenter;
    QString displayName;        // shown in the QListWidget
    QString defaultText;        // initial QLineEdit value
    QFont   font;
    QColor  colour = Qt::white;

    // Animation / placement metadata. The dialog converts these into the
    // proper EnhancedTextOverlay fields (animIn / animOut / position
    // keyframes / shadow) at apply time.
    double  durationSec = 3.0;     // total on-screen duration
    double  inSec = 0.5;
    double  outSec = 0.5;
    double  anchorX = 0.5;         // resting position
    double  anchorY = 0.5;
    bool    lowerThird = false;    // when true, anchorY is forced to 0.85
    bool    slideFromLeft = false; // generate position keyframes left -> anchor
    bool    spinIn = false;        // pure scale animation will be reused; flag stored for thumbnails
    bool    pulse = false;
    bool    typewriter = false;
    bool    dropShadow = false;
};
