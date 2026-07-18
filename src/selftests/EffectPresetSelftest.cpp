// src/selftests/EffectPresetSelftest.cpp
// FXP-1: effect stack preset save/load/apply round-trip gates.

#include "../EffectPreset.h"
#include "../Timeline.h"

#include <cmath>
#include <cstdio>

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

constexpr double kEps = 1e-9;
constexpr auto kPresetDirEnv = "VEDITOR_EFFECT_PRESET_DIR";

bool near(double a, double b, double eps = kEps)
{
    return std::abs(a - b) <= eps;
}

bool sameColorCorrection(const ColorCorrection &a, const ColorCorrection &b)
{
    return near(a.brightness, b.brightness)
        && near(a.contrast, b.contrast)
        && near(a.saturation, b.saturation)
        && near(a.hue, b.hue)
        && near(a.temperature, b.temperature)
        && near(a.tint, b.tint)
        && near(a.gamma, b.gamma)
        && near(a.highlights, b.highlights)
        && near(a.shadows, b.shadows)
        && near(a.exposure, b.exposure)
        && near(a.liftR, b.liftR)
        && near(a.liftG, b.liftG)
        && near(a.liftB, b.liftB)
        && near(a.gammaR, b.gammaR)
        && near(a.gammaG, b.gammaG)
        && near(a.gammaB, b.gammaB)
        && near(a.gainR, b.gainR)
        && near(a.gainG, b.gainG)
        && near(a.gainB, b.gainB);
}

bool sameEffect(const VideoEffect &a, const VideoEffect &b)
{
    return a.type == b.type
        && a.enabled == b.enabled
        && near(a.param1, b.param1)
        && near(a.param2, b.param2)
        && near(a.param3, b.param3)
        && a.keyColor == b.keyColor
        && near(a.startSec, b.startSec)
        && near(a.endSec, b.endSec);
}

bool sameEffects(const QVector<VideoEffect> &a, const QVector<VideoEffect> &b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (!sameEffect(a[i], b[i]))
            return false;
    }
    return true;
}

bool sameKeyframePoint(const KeyframePoint &a, const KeyframePoint &b)
{
    return near(a.time, b.time)
        && near(a.value, b.value)
        && a.interpolation == b.interpolation
        && near(a.bezX1, b.bezX1)
        && near(a.bezY1, b.bezY1)
        && near(a.bezX2, b.bezX2)
        && near(a.bezY2, b.bezY2)
        && a.hasSpatialTangent == b.hasSpatialTangent
        && near(a.spatialOutX, b.spatialOutX)
        && near(a.spatialOutY, b.spatialOutY)
        && near(a.spatialInX, b.spatialInX)
        && near(a.spatialInY, b.spatialInY);
}

bool sameKeyframeTrack(const KeyframeTrack &a, const KeyframeTrack &b)
{
    if (a.propertyName() != b.propertyName()
        || !near(a.defaultValue(), b.defaultValue())
        || a.keyframes().size() != b.keyframes().size()) {
        return false;
    }
    for (int i = 0; i < a.keyframes().size(); ++i) {
        if (!sameKeyframePoint(a.keyframes()[i], b.keyframes()[i]))
            return false;
    }
    return true;
}

bool sameStringKeyframeTrack(const StringKeyframeTrack &a, const StringKeyframeTrack &b)
{
    if (a.propertyName() != b.propertyName()
        || a.defaultValue() != b.defaultValue()
        || a.keyframes().size() != b.keyframes().size()) {
        return false;
    }
    for (int i = 0; i < a.keyframes().size(); ++i) {
        if (!near(a.keyframes()[i].time, b.keyframes()[i].time)
            || a.keyframes()[i].value != b.keyframes()[i].value) {
            return false;
        }
    }
    return true;
}

bool sameKeyframes(const KeyframeManager &a, const KeyframeManager &b)
{
    if (a.tracks().size() != b.tracks().size()
        || a.stringTracks().size() != b.stringTracks().size()) {
        return false;
    }
    for (const KeyframeTrack &track : a.tracks()) {
        const KeyframeTrack *other = b.track(track.propertyName());
        if (!other || !sameKeyframeTrack(track, *other)
            || a.loopOutMode(track.propertyName()) != b.loopOutMode(track.propertyName())) {
            return false;
        }
    }
    for (const StringKeyframeTrack &track : a.stringTracks()) {
        const StringKeyframeTrack *other = b.stringTrack(track.propertyName());
        if (!other || !sameStringKeyframeTrack(track, *other))
            return false;
    }
    return true;
}

bool isEffectKeyframeTrack(const QString &propertyName)
{
    return propertyName.startsWith(QStringLiteral("effect."));
}

KeyframeManager effectKeyframesOnly(const KeyframeManager &source)
{
    KeyframeManager filtered;
    for (const KeyframeTrack &track : source.tracks()) {
        if (!isEffectKeyframeTrack(track.propertyName()))
            continue;
        filtered.addTrack(track);
        const LoopMode mode = source.loopOutMode(track.propertyName());
        if (mode != LoopMode::None)
            filtered.setLoopOutMode(track.propertyName(), mode);
    }
    for (const StringKeyframeTrack &track : source.stringTracks()) {
        if (isEffectKeyframeTrack(track.propertyName()))
            filtered.addStringTrack(track);
    }
    return filtered;
}

QVector<VideoEffectType> supportedTypes()
{
    QVector<VideoEffectType> types;
    types.append(VideoEffectType::None);
    for (VideoEffectType type : VideoEffect::allTypes())
        types.append(type);
    return types;
}

VideoEffect sampleEffect(VideoEffectType type, int index)
{
    VideoEffect effect;
    effect.type = type;
    effect.enabled = (index % 5) != 0;
    effect.param1 = 1.25 + index * 0.75;
    effect.param2 = -2.5 + index * 0.5;
    effect.param3 = 3.75 + index * 0.25;
    effect.keyColor = QColor((index * 37) % 256,
                             (index * 71 + 13) % 256,
                             (index * 29 + 97) % 256);
    if ((index % 4) == 0) {
        effect.startSec = index * 0.01;
        effect.endSec = effect.startSec + 0.5;
    }
    return effect;
}

ClipInfo makeSourceClip()
{
    ClipInfo clip;
    clip.displayName = QStringLiteral("fxp1-source");
    clip.duration = 10.0;
    clip.colorCorrection.brightness = 11.0;
    clip.colorCorrection.contrast = -12.0;
    clip.colorCorrection.saturation = 13.0;
    clip.colorCorrection.hue = -14.0;
    clip.colorCorrection.temperature = 15.0;
    clip.colorCorrection.tint = -16.0;
    clip.colorCorrection.gamma = 1.25;
    clip.colorCorrection.highlights = 17.0;
    clip.colorCorrection.shadows = -18.0;
    clip.colorCorrection.exposure = 0.75;
    clip.colorCorrection.liftR = 0.10;
    clip.colorCorrection.liftG = -0.11;
    clip.colorCorrection.liftB = 0.12;
    clip.colorCorrection.gammaR = -0.13;
    clip.colorCorrection.gammaG = 0.14;
    clip.colorCorrection.gammaB = -0.15;
    clip.colorCorrection.gainR = 0.16;
    clip.colorCorrection.gainG = -0.17;
    clip.colorCorrection.gainB = 0.18;

    const auto types = supportedTypes();
    for (int i = 0; i < types.size(); ++i)
        clip.effects.append(sampleEffect(types[i], i));

    KeyframeTrack radius(QStringLiteral("effect.1.radius"), clip.effects[1].param1);
    radius.addKeyframe(0.0, 2.0);
    radius.addKeyframe(1.0, 12.0, KeyframePoint::Bezier,
                       0.2, 0.1, 0.8, 0.9);
    clip.keyframes.addTrack(radius);
    clip.keyframes.setLoopOutMode(radius.propertyName(), LoopMode::Cycle);

    KeyframeTrack red(QStringLiteral("effect.4.color.r"), 0.0);
    red.addKeyframe(0.0, 10.0);
    red.addKeyframe(2.0, 240.0, KeyframePoint::EaseInOut);
    clip.keyframes.addTrack(red);

    StringKeyframeTrack note(QStringLiteral("effect.2.note"), QStringLiteral("cold"));
    note.addKeyframe(0.25, QStringLiteral("warm"));
    note.addKeyframe(1.25, QStringLiteral("hot"));
    clip.keyframes.addStringTrack(note);

    KeyframeTrack motion(QStringLiteral("motion.scale"), 1.0);
    motion.addKeyframe(0.0, 1.0);
    motion.addKeyframe(1.0, 2.0);
    clip.keyframes.addTrack(motion);

    StringKeyframeTrack sourceText(QStringLiteral("sourceText"), QStringLiteral("keep"));
    sourceText.addKeyframe(0.0, QStringLiteral("outside-effect"));
    clip.keyframes.addStringTrack(sourceText);
    return clip;
}

ClipInfo makeTargetClip()
{
    ClipInfo clip;
    clip.displayName = QStringLiteral("fxp1-target");
    clip.duration = 10.0;
    clip.effects.append(VideoEffect::createInvert());

    KeyframeTrack stale(QStringLiteral("effect.0.radius"), 99.0);
    stale.addKeyframe(0.0, 99.0);
    stale.addKeyframe(1.0, 100.0);
    clip.keyframes.addTrack(stale);

    KeyframeTrack motion(QStringLiteral("motion.scale"), 3.0);
    motion.addKeyframe(0.0, 3.0);
    motion.addKeyframe(1.0, 4.0);
    clip.keyframes.addTrack(motion);
    return clip;
}

bool readJsonObject(const QString &path, QJsonObject *obj)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    *obj = doc.object();
    return true;
}

} // namespace

int runEffectPresetSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[effect-preset] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        check(0, "temp dir", false, QStringLiteral("QTemporaryDir invalid"));
        return 1;
    }

    const bool hadPresetDirEnv = qEnvironmentVariableIsSet(kPresetDirEnv);
    const QByteArray oldPresetDirEnv = qgetenv(kPresetDirEnv);
    qputenv(kPresetDirEnv, tempDir.filePath(QStringLiteral("effect-presets")).toLocal8Bit());

    const ClipInfo source = makeSourceClip();
    PresetLibrary &library = PresetLibrary::instance();

    // G1: include-KF preset saves to the configured settings directory.
    QString includePath;
    const QString includeName = QStringLiteral("FXP-1 Include KF");
    const bool savedInclude =
        library.saveClipStackPreset(includeName, source, true, &includePath);
    check(1, "save include-KF preset to settings dir",
          savedInclude
              && QFile::exists(includePath)
              && includePath.startsWith(PresetLibrary::presetDirectory())
              && includePath == PresetLibrary::presetFilePath(includeName),
          includePath);

    // G2: JSON round-trip preserves all 47 supported enum cases and params.
    EffectPreset includePreset;
    const bool loadedInclude = library.loadClipStackPreset(includeName, &includePreset);
    const KeyframeManager sourceEffectKf = effectKeyframesOnly(source.keyframes);
    check(2, "load round-trip preserves 47 effect entries and parameters",
          loadedInclude
              && includePreset.includesKeyframes
              && includePreset.effects.size() == supportedTypes().size()
              && supportedTypes().size() == 47
              && sameColorCorrection(includePreset.colorCorrection, source.colorCorrection)
              && sameEffects(includePreset.effects, source.effects)
              && sameKeyframes(includePreset.keyframes, sourceEffectKf),
          QStringLiteral("effects=%1 types=%2")
              .arg(includePreset.effects.size())
              .arg(supportedTypes().size()));

    // G3: applying include-KF replaces only effect.* keyframes and preserves motion tracks.
    ClipInfo appliedInclude = makeTargetClip();
    const KeyframeManager targetMotionBefore = appliedInclude.keyframes;
    const bool appliedIncludeOk =
        library.applyClipStackPreset(includeName, appliedInclude, true);
    check(3, "apply include-KF preset matches saved stack",
          appliedIncludeOk
              && sameColorCorrection(appliedInclude.colorCorrection, source.colorCorrection)
              && sameEffects(appliedInclude.effects, source.effects)
              && sameKeyframes(effectKeyframesOnly(appliedInclude.keyframes), sourceEffectKf)
              && appliedInclude.keyframes.hasTrack(QStringLiteral("motion.scale"))
              && sameKeyframeTrack(*appliedInclude.keyframes.track(QStringLiteral("motion.scale")),
                                   *targetMotionBefore.track(QStringLiteral("motion.scale")))
              && !appliedInclude.keyframes.hasTrack(QStringLiteral("effect.0.radius")));

    // G4: no-KF preset omits keyframes and leaves target effect keyframes untouched.
    QString noKfPath;
    const QString noKfName = QStringLiteral("FXP-1 No KF");
    const bool savedNoKf =
        library.saveClipStackPreset(noKfName, source, false, &noKfPath);
    QJsonObject noKfJson;
    const bool readNoKf = readJsonObject(noKfPath, &noKfJson);
    EffectPreset noKfPreset;
    ClipInfo appliedNoKf = makeTargetClip();
    const KeyframeManager targetEffectBefore = effectKeyframesOnly(appliedNoKf.keyframes);
    const bool loadedNoKf = library.loadClipStackPreset(noKfName, &noKfPreset);
    const bool appliedNoKfOk =
        library.applyClipStackPreset(noKfName, appliedNoKf, true);
    check(4, "save/apply no-KF preset leaves existing keyframes",
          savedNoKf
              && readNoKf
              && !noKfJson.value(QStringLiteral("includesKeyframes")).toBool(false)
              && !noKfJson.contains(QStringLiteral("keyframes"))
              && loadedNoKf
              && !noKfPreset.includesKeyframes
              && appliedNoKfOk
              && sameColorCorrection(appliedNoKf.colorCorrection, source.colorCorrection)
              && sameEffects(appliedNoKf.effects, source.effects)
              && sameKeyframes(effectKeyframesOnly(appliedNoKf.keyframes), targetEffectBefore));

    // G5: caller can suppress keyframe application even from an include-KF preset.
    ClipInfo skippedKf = makeTargetClip();
    const KeyframeManager skippedEffectBefore = effectKeyframesOnly(skippedKf.keyframes);
    const bool skippedKfOk = library.applyClipStackPreset(includeName, skippedKf, false);
    check(5, "apply include-KF preset with applyKeyframes=false",
          skippedKfOk
              && sameEffects(skippedKf.effects, source.effects)
              && sameKeyframes(effectKeyframesOnly(skippedKf.keyframes), skippedEffectBefore));

    if (hadPresetDirEnv)
        qputenv(kPresetDirEnv, oldPresetDirEnv);
    else
        qunsetenv(kPresetDirEnv);

    std::printf("[effect-preset] RESULT passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
