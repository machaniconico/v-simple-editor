#include "MotionPreset.h"

#include <initializer_list>

#include <QtGlobal>

namespace motionpreset {
namespace {

const QString kScaleTrack = QStringLiteral("motion.scale");
const QString kPosXTrack = QStringLiteral("motion.position.x");
const QString kPosYTrack = QStringLiteral("motion.position.y");
const QString kRotationTrack = QStringLiteral("motion.rotation");
const QString kOpacityTrack = QStringLiteral("motion.opacity");

struct PresetDef {
    const char *id;
    const char *displayNameUtf8;
};

const PresetDef kPresets[] = {
    { "FadeIn",        "フェードイン" },
    { "FadeOut",       "フェードアウト" },
    { "PopIn",         "ポップイン" },
    { "SlideInLeft",   "スライドイン(左)" },
    { "SlideInRight",  "スライドイン(右)" },
    { "SlideInTop",    "スライドイン(上)" },
    { "SlideInBottom", "スライドイン(下)" },
    { "SpinIn",        "スピンイン" },
    { "BounceIn",      "バウンスイン" },
    { "ZoomOutFade",   "ズームアウト" },
};

QString idFromDef(const PresetDef &def)
{
    return QString::fromLatin1(def.id);
}

QString displayFromDef(const PresetDef &def)
{
    return QString::fromUtf8(def.displayNameUtf8);
}

QString compactAlias(QString s)
{
    s = s.trimmed().toLower();
    s.remove(QChar(' '));
    s.remove(QChar('-'));
    s.remove(QChar('_'));
    s.remove(QChar('/'));
    return s;
}

double leadSeconds(double duration)
{
    if (duration <= 0.0)
        return 0.5;
    return qMin(duration, qBound(0.5, duration * 0.25, 0.75));
}

struct KeySpec {
    double time;
    double value;
    KeyframePoint::Interpolation interpolation = KeyframePoint::Linear;
};

void replaceMotionTracks(KeyframeManager &km)
{
    km.removeTrack(kScaleTrack);
    km.removeTrack(kPosXTrack);
    km.removeTrack(kPosYTrack);
    km.removeTrack(kRotationTrack);
    km.removeTrack(kOpacityTrack);
}

void addTrack(KeyframeManager &km, const QString &trackName, double defaultValue,
              std::initializer_list<KeySpec> keyframes)
{
    KeyframeTrack track(trackName, defaultValue);
    for (const KeySpec &kf : keyframes)
        track.addKeyframe(kf.time, kf.value, kf.interpolation);
    km.addTrack(track);
}

} // namespace

QStringList presetIds()
{
    QStringList ids;
    ids.reserve(static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0])));
    for (const PresetDef &def : kPresets)
        ids.append(idFromDef(def));
    return ids;
}

QString displayName(const QString &presetId)
{
    for (const PresetDef &def : kPresets) {
        if (presetId == idFromDef(def))
            return displayFromDef(def);
    }
    return QString();
}

QString presetIdForDisplayName(const QString &name)
{
    const QString trimmed = name.trimmed();
    for (const PresetDef &def : kPresets) {
        const QString id = idFromDef(def);
        if (trimmed == id || trimmed == displayFromDef(def))
            return id;
    }

    const QString alias = compactAlias(trimmed);
    if (alias == QStringLiteral("fadein") || alias == QStringLiteral("randomfadeup")
        || alias == QStringLiteral("fadeinlines") || alias == QStringLiteral("cinematic1")
        || alias == QStringLiteral("typewritersound")) {
        return QStringLiteral("FadeIn");
    }
    if (alias == QStringLiteral("fadeout") || alias == QStringLiteral("driftdown"))
        return QStringLiteral("FadeOut");
    if (alias == QStringLiteral("popin") || alias == QStringLiteral("stomp")
        || alias == QStringLiteral("scalebounce") || alias == QStringLiteral("snappop")) {
        return QStringLiteral("PopIn");
    }
    if (alias == QStringLiteral("slideinleft") || alias == QStringLiteral("slidefromleft"))
        return QStringLiteral("SlideInLeft");
    if (alias == QStringLiteral("slideinright"))
        return QStringLiteral("SlideInRight");
    if (alias == QStringLiteral("slideintop") || alias == QStringLiteral("slideinup")
        || alias == QStringLiteral("waveup")) {
        return QStringLiteral("SlideInTop");
    }
    if (alias == QStringLiteral("slideinbottom") || alias == QStringLiteral("slideindown"))
        return QStringLiteral("SlideInBottom");
    if (alias == QStringLiteral("spinin") || alias == QStringLiteral("spinineachword"))
        return QStringLiteral("SpinIn");
    if (alias == QStringLiteral("bouncein") || alias == QStringLiteral("bounceineach"))
        return QStringLiteral("BounceIn");
    if (alias == QStringLiteral("zoomout") || alias == QStringLiteral("zoomoutfade"))
        return QStringLiteral("ZoomOutFade");
    return QString();
}

void applyPreset(KeyframeManager &km, const QString &presetId,
                 double clipStartSec, double clipDurationSec)
{
    const QString id = presetIdForDisplayName(presetId);
    if (id.isEmpty())
        return;

    const double start = qMax(0.0, clipStartSec);
    const double duration = qMax(0.001, clipDurationSec);
    const double end = start + duration;
    const double lead = leadSeconds(duration);
    const double inEnd = start + lead;
    const double outStart = end - lead;

    replaceMotionTracks(km);

    if (id == QStringLiteral("FadeIn")) {
        addTrack(km, kOpacityTrack, 1.0, {
            { start, 0.0, KeyframePoint::EaseOut },
            { inEnd, 1.0, KeyframePoint::Linear },
        });
    } else if (id == QStringLiteral("FadeOut")) {
        addTrack(km, kOpacityTrack, 1.0, {
            { outStart, 1.0, KeyframePoint::EaseIn },
            { end, 0.0, KeyframePoint::Linear },
        });
    } else if (id == QStringLiteral("PopIn")) {
        addTrack(km, kScaleTrack, 1.0, {
            { start, 0.65, KeyframePoint::EaseOut },
            { start + lead * 0.68, 1.12, KeyframePoint::EaseInOut },
            { inEnd, 1.0, KeyframePoint::Linear },
        });
        addTrack(km, kOpacityTrack, 1.0, {
            { start, 0.0, KeyframePoint::EaseOut },
            { start + lead * 0.35, 1.0, KeyframePoint::Linear },
        });
    } else if (id == QStringLiteral("SlideInLeft")) {
        addTrack(km, kPosXTrack, 0.0, {
            { start, -1.15, KeyframePoint::EaseOut },
            { inEnd, 0.0, KeyframePoint::Linear },
        });
        addTrack(km, kOpacityTrack, 1.0, {
            { start, 0.0, KeyframePoint::EaseOut },
            { start + lead * 0.45, 1.0, KeyframePoint::Linear },
        });
    } else if (id == QStringLiteral("SlideInRight")) {
        addTrack(km, kPosXTrack, 0.0, {
            { start, 1.15, KeyframePoint::EaseOut },
            { inEnd, 0.0, KeyframePoint::Linear },
        });
        addTrack(km, kOpacityTrack, 1.0, {
            { start, 0.0, KeyframePoint::EaseOut },
            { start + lead * 0.45, 1.0, KeyframePoint::Linear },
        });
    } else if (id == QStringLiteral("SlideInTop")) {
        addTrack(km, kPosYTrack, 0.0, {
            { start, -1.15, KeyframePoint::EaseOut },
            { inEnd, 0.0, KeyframePoint::Linear },
        });
        addTrack(km, kOpacityTrack, 1.0, {
            { start, 0.0, KeyframePoint::EaseOut },
            { start + lead * 0.45, 1.0, KeyframePoint::Linear },
        });
    } else if (id == QStringLiteral("SlideInBottom")) {
        addTrack(km, kPosYTrack, 0.0, {
            { start, 1.15, KeyframePoint::EaseOut },
            { inEnd, 0.0, KeyframePoint::Linear },
        });
        addTrack(km, kOpacityTrack, 1.0, {
            { start, 0.0, KeyframePoint::EaseOut },
            { start + lead * 0.45, 1.0, KeyframePoint::Linear },
        });
    } else if (id == QStringLiteral("SpinIn")) {
        addTrack(km, kRotationTrack, 0.0, {
            { start, -180.0, KeyframePoint::EaseOut },
            { inEnd, 0.0, KeyframePoint::Linear },
        });
        addTrack(km, kScaleTrack, 1.0, {
            { start, 0.75, KeyframePoint::EaseOut },
            { inEnd, 1.0, KeyframePoint::Linear },
        });
        addTrack(km, kOpacityTrack, 1.0, {
            { start, 0.0, KeyframePoint::EaseOut },
            { start + lead * 0.5, 1.0, KeyframePoint::Linear },
        });
    } else if (id == QStringLiteral("BounceIn")) {
        addTrack(km, kScaleTrack, 1.0, {
            { start, 0.45, KeyframePoint::EaseOut },
            { start + lead * 0.52, 1.18, KeyframePoint::EaseInOut },
            { start + lead * 0.78, 0.94, KeyframePoint::EaseOut },
            { inEnd, 1.0, KeyframePoint::Linear },
        });
        addTrack(km, kPosYTrack, 0.0, {
            { start, 0.12, KeyframePoint::EaseOut },
            { start + lead * 0.52, -0.04, KeyframePoint::EaseInOut },
            { inEnd, 0.0, KeyframePoint::Linear },
        });
        addTrack(km, kOpacityTrack, 1.0, {
            { start, 0.0, KeyframePoint::EaseOut },
            { start + lead * 0.35, 1.0, KeyframePoint::Linear },
        });
    } else if (id == QStringLiteral("ZoomOutFade")) {
        addTrack(km, kScaleTrack, 1.0, {
            { outStart, 1.0, KeyframePoint::EaseInOut },
            { outStart + lead * 0.35, 1.08, KeyframePoint::EaseIn },
            { end, 0.85, KeyframePoint::Linear },
        });
        addTrack(km, kOpacityTrack, 1.0, {
            { outStart, 1.0, KeyframePoint::EaseIn },
            { end, 0.0, KeyframePoint::Linear },
        });
    }
}

} // namespace motionpreset
