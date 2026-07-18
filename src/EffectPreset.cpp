#include "EffectPreset.h"

#include "Timeline.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QUrl>

namespace {

constexpr auto kPresetDirEnv = "VEDITOR_EFFECT_PRESET_DIR";
constexpr auto kEffectTrackPrefix = "effect.";

QVector<VideoEffectType> supportedEffectTypes()
{
    QVector<VideoEffectType> types;
    types.append(VideoEffectType::None);
    for (VideoEffectType type : VideoEffect::allTypes())
        types.append(type);
    return types;
}

bool isSupportedEffectType(VideoEffectType type)
{
    const auto types = supportedEffectTypes();
    for (VideoEffectType candidate : types) {
        if (candidate == type)
            return true;
    }
    return false;
}

QString effectTypeKey(VideoEffectType type)
{
    switch (type) {
    case VideoEffectType::None: return QStringLiteral("None");
    case VideoEffectType::Blur: return QStringLiteral("Blur");
    case VideoEffectType::Sharpen: return QStringLiteral("Sharpen");
    case VideoEffectType::Mosaic: return QStringLiteral("Mosaic");
    case VideoEffectType::ChromaKey: return QStringLiteral("ChromaKey");
    case VideoEffectType::Vignette: return QStringLiteral("Vignette");
    case VideoEffectType::Sepia: return QStringLiteral("Sepia");
    case VideoEffectType::Grayscale: return QStringLiteral("Grayscale");
    case VideoEffectType::Invert: return QStringLiteral("Invert");
    case VideoEffectType::Noise: return QStringLiteral("Noise");
    case VideoEffectType::DisplacementMap: return QStringLiteral("DisplacementMap");
    case VideoEffectType::FractalNoiseGen: return QStringLiteral("FractalNoiseGen");
    case VideoEffectType::GaussianBlur: return QStringLiteral("GaussianBlur");
    case VideoEffectType::DirectionalBlur: return QStringLiteral("DirectionalBlur");
    case VideoEffectType::RadialBlur: return QStringLiteral("RadialBlur");
    case VideoEffectType::Glow: return QStringLiteral("Glow");
    case VideoEffectType::FindEdges: return QStringLiteral("FindEdges");
    case VideoEffectType::Emboss: return QStringLiteral("Emboss");
    case VideoEffectType::Posterize: return QStringLiteral("Posterize");
    case VideoEffectType::Threshold: return QStringLiteral("Threshold");
    case VideoEffectType::Solarize: return QStringLiteral("Solarize");
    case VideoEffectType::Levels: return QStringLiteral("Levels");
    case VideoEffectType::Tint: return QStringLiteral("Tint");
    case VideoEffectType::BlackWhite: return QStringLiteral("BlackWhite");
    case VideoEffectType::Exposure: return QStringLiteral("Exposure");
    case VideoEffectType::HueSaturation: return QStringLiteral("HueSaturation");
    case VideoEffectType::RGBSplit: return QStringLiteral("RGBSplit");
    case VideoEffectType::WaveWarp: return QStringLiteral("WaveWarp");
    case VideoEffectType::Ripple: return QStringLiteral("Ripple");
    case VideoEffectType::GlitchVHS: return QStringLiteral("GlitchVHS");
    case VideoEffectType::GradientRamp: return QStringLiteral("GradientRamp");
    case VideoEffectType::Fill: return QStringLiteral("Fill");
    case VideoEffectType::Bloom: return QStringLiteral("Bloom");
    case VideoEffectType::Scanlines: return QStringLiteral("Scanlines");
    case VideoEffectType::Halftone: return QStringLiteral("Halftone");
    case VideoEffectType::Curves: return QStringLiteral("Curves");
    case VideoEffectType::ChannelMixer: return QStringLiteral("ChannelMixer");
    case VideoEffectType::Vibrance: return QStringLiteral("Vibrance");
    case VideoEffectType::PhotoFilter: return QStringLiteral("PhotoFilter");
    case VideoEffectType::Tritone: return QStringLiteral("Tritone");
    case VideoEffectType::BrightnessContrast: return QStringLiteral("BrightnessContrast");
    case VideoEffectType::Bulge: return QStringLiteral("Bulge");
    case VideoEffectType::Twirl: return QStringLiteral("Twirl");
    case VideoEffectType::Mirror: return QStringLiteral("Mirror");
    case VideoEffectType::PolarCoordinates: return QStringLiteral("PolarCoordinates");
    case VideoEffectType::MotionTile: return QStringLiteral("MotionTile");
    case VideoEffectType::CornerPinSimple: return QStringLiteral("CornerPinSimple");
    }
    return QStringLiteral("None");
}

QString normalizedEffectTypeName(const QString &value)
{
    QString result;
    for (const QChar ch : value) {
        if (ch.isLetterOrNumber())
            result.append(ch.toLower());
    }
    return result;
}

VideoEffectType effectTypeFromString(const QString &value)
{
    const QString trimmed = value.trimmed();
    const QString normalized = normalizedEffectTypeName(trimmed);
    const auto types = supportedEffectTypes();
    for (VideoEffectType type : types) {
        if (trimmed == effectTypeKey(type) || trimmed == VideoEffect::typeName(type))
            return type;
        if (normalized == normalizedEffectTypeName(effectTypeKey(type))
            || normalized == normalizedEffectTypeName(VideoEffect::typeName(type))) {
            return type;
        }
    }
    return VideoEffectType::None;
}

VideoEffectType effectTypeFromJson(const QJsonObject &obj)
{
    if (obj.contains(QStringLiteral("typeId"))) {
        const auto type = static_cast<VideoEffectType>(
            obj[QStringLiteral("typeId")].toInt(static_cast<int>(VideoEffectType::None)));
        if (isSupportedEffectType(type))
            return type;
    }
    if (obj.contains(QStringLiteral("typeKey")))
        return effectTypeFromString(obj[QStringLiteral("typeKey")].toString());
    return effectTypeFromString(obj[QStringLiteral("type")].toString());
}

bool isEffectKeyframeTrack(const QString &propertyName)
{
    return propertyName.startsWith(QString::fromLatin1(kEffectTrackPrefix));
}

QString presetDirectoryPath(bool create)
{
    QString dir;
    const QByteArray overrideDir = qgetenv(kPresetDirEnv);
    if (!overrideDir.isEmpty()) {
        dir = QString::fromLocal8Bit(overrideDir);
    } else {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        if (dir.isEmpty())
            dir = QDir::homePath() + QStringLiteral("/.veditor");
        dir += QStringLiteral("/effect-presets");
    }
    if (create)
        QDir().mkpath(dir);
    return dir;
}

int presetIndexByName(const QVector<EffectPreset> &presets, const QString &name)
{
    for (int i = 0; i < presets.size(); ++i) {
        if (presets[i].name == name)
            return i;
    }
    return -1;
}

bool readPresetJsonFile(const QString &filePath, EffectPreset *preset)
{
    if (!preset)
        return false;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    EffectPreset loaded = EffectPreset::fromJson(doc.object());
    if (loaded.name.isEmpty())
        return false;

    loaded.isBuiltIn = false;
    *preset = loaded;
    return true;
}

bool writePresetJsonFile(const QString &filePath, const EffectPreset &preset)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    const QByteArray json = QJsonDocument(preset.toJson()).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size())
        return false;
    file.close();
    return file.error() == QFile::NoError;
}

void mergeUserPreset(QVector<EffectPreset> &presets, EffectPreset preset)
{
    if (preset.name.isEmpty())
        return;

    preset.isBuiltIn = false;
    const int existingIndex = presetIndexByName(presets, preset.name);
    if (existingIndex < 0) {
        presets.append(preset);
        return;
    }
    if (!presets[existingIndex].isBuiltIn)
        presets[existingIndex] = preset;
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

void replaceEffectKeyframes(KeyframeManager &target, const KeyframeManager &replacement)
{
    QStringList numericToRemove;
    for (const KeyframeTrack &track : target.tracks()) {
        if (isEffectKeyframeTrack(track.propertyName()))
            numericToRemove.append(track.propertyName());
    }
    for (const QString &propertyName : numericToRemove)
        target.removeTrack(propertyName);

    QStringList stringToRemove;
    for (const StringKeyframeTrack &track : target.stringTracks()) {
        if (isEffectKeyframeTrack(track.propertyName()))
            stringToRemove.append(track.propertyName());
    }
    for (const QString &propertyName : stringToRemove)
        target.removeStringTrack(propertyName);

    const KeyframeManager filtered = effectKeyframesOnly(replacement);
    for (const KeyframeTrack &track : filtered.tracks()) {
        target.addTrack(track);
        const LoopMode mode = filtered.loopOutMode(track.propertyName());
        if (mode != LoopMode::None)
            target.setLoopOutMode(track.propertyName(), mode);
    }
    for (const StringKeyframeTrack &track : filtered.stringTracks())
        target.addStringTrack(track);
}

QString presetFileNameForName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return QString();
    return QString::fromLatin1(QUrl::toPercentEncoding(trimmed)) + QStringLiteral(".json");
}

} // namespace

// ===== EffectPreset serialisation =====

QJsonObject EffectPreset::toJson() const
{
    QJsonObject obj;
    obj["name"]        = name;
    obj["description"] = description;
    obj["category"]    = category;
    obj["author"]      = author;
    obj["isBuiltIn"]   = isBuiltIn;
    obj["createdAt"]   = createdAt.toString(Qt::ISODate);
    obj["modifiedAt"]  = modifiedAt.toString(Qt::ISODate);

    obj["colorCorrection"] = PresetLibrary::colorCorrectionToJson(colorCorrection);

    QJsonArray arr;
    for (const auto &e : effects)
        arr.append(PresetLibrary::videoEffectToJson(e));
    obj["effects"] = arr;

    if (includesKeyframes) {
        obj["includesKeyframes"] = true;
        obj["keyframes"] = effectKeyframesOnly(keyframes).toJson();
    }

    return obj;
}

EffectPreset EffectPreset::fromJson(const QJsonObject &obj)
{
    EffectPreset p;
    p.name        = obj["name"].toString();
    p.description = obj["description"].toString();
    p.category    = obj["category"].toString();
    p.author      = obj["author"].toString();
    p.isBuiltIn   = obj["isBuiltIn"].toBool(false);
    p.createdAt   = QDateTime::fromString(obj["createdAt"].toString(), Qt::ISODate);
    p.modifiedAt  = QDateTime::fromString(obj["modifiedAt"].toString(), Qt::ISODate);

    p.colorCorrection = PresetLibrary::colorCorrectionFromJson(
        obj["colorCorrection"].toObject());

    const QJsonArray arr = obj["effects"].toArray();
    for (const auto &v : arr)
        p.effects.append(PresetLibrary::videoEffectFromJson(v.toObject()));

    p.includesKeyframes = obj["includesKeyframes"].toBool(obj.contains("keyframes"));
    if (obj.contains("keyframes")) {
        KeyframeManager km;
        km.fromJson(obj["keyframes"].toObject());
        p.keyframes = effectKeyframesOnly(km);
    }

    return p;
}

EffectPreset EffectPreset::fromClipStack(const QString &name,
                                         const ClipInfo &clip,
                                         bool includeKeyframes)
{
    EffectPreset preset;
    const QDateTime now = QDateTime::currentDateTime();
    preset.name = name.trimmed();
    preset.category = QStringLiteral("User");
    preset.author = QStringLiteral("v-editor");
    preset.colorCorrection = clip.colorCorrection;
    preset.effects = clip.effects;
    preset.createdAt = now;
    preset.modifiedAt = now;
    preset.includesKeyframes = includeKeyframes;
    if (includeKeyframes)
        preset.keyframes = effectKeyframesOnly(clip.keyframes);
    return preset;
}

void EffectPreset::applyToClipStack(ClipInfo &clip, bool applyKeyframes) const
{
    clip.colorCorrection = colorCorrection;
    clip.effects = effects;
    if (includesKeyframes && applyKeyframes)
        replaceEffectKeyframes(clip.keyframes, keyframes);
}

// ===== PresetLibrary =====

PresetLibrary &PresetLibrary::instance()
{
    static PresetLibrary lib;
    return lib;
}

PresetLibrary::PresetLibrary()
{
    registerBuiltins();
    loadLibrary();   // merge any user presets saved on disk
}

// --- Query ---

QVector<EffectPreset> PresetLibrary::allPresets() const
{
    return m_presets;
}

QVector<EffectPreset> PresetLibrary::presetsByCategory(const QString &category) const
{
    QVector<EffectPreset> result;
    for (const auto &p : m_presets)
        if (p.category == category) result.append(p);
    return result;
}

EffectPreset PresetLibrary::findByName(const QString &name) const
{
    for (const auto &p : m_presets)
        if (p.name == name) return p;
    return EffectPreset{};
}

QStringList PresetLibrary::categories() const
{
    QStringList cats;
    for (const auto &p : m_presets)
        if (!cats.contains(p.category)) cats.append(p.category);
    return cats;
}

// --- Mutate ---

bool PresetLibrary::addPreset(const EffectPreset &preset)
{
    // Reject duplicates
    for (const auto &p : m_presets)
        if (p.name == preset.name) return false;

    m_presets.append(preset);
    return true;
}

bool PresetLibrary::removePreset(const QString &name)
{
    for (int i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name == name) {
            if (m_presets[i].isBuiltIn) return false;   // cannot remove factory presets
            const QString filePath = presetFilePath(name);
            if (!filePath.isEmpty() && QFile::exists(filePath) && !QFile::remove(filePath))
                return false;
            m_presets.removeAt(i);
            saveLibrary();
            return true;
        }
    }
    return false;
}

bool PresetLibrary::updatePreset(const QString &name, const EffectPreset &preset)
{
    const int index = presetIndexByName(m_presets, name);
    if (index < 0 || m_presets[index].isBuiltIn)
        return false;   // cannot modify factory presets

    EffectPreset updated = preset;
    updated.name = updated.name.trimmed();
    if (updated.name.isEmpty())
        return false;

    const int targetIndex = presetIndexByName(m_presets, updated.name);
    if (targetIndex >= 0 && targetIndex != index)
        return false;

    if (!m_presets[index].createdAt.isValid() && !updated.createdAt.isValid())
        updated.createdAt = QDateTime::currentDateTime();
    else if (!updated.createdAt.isValid())
        updated.createdAt = m_presets[index].createdAt;
    updated.modifiedAt = QDateTime::currentDateTime();
    updated.isBuiltIn = false;

    const QString newPath = presetFilePath(updated.name);
    if (newPath.isEmpty() || !writePresetJsonFile(newPath, updated))
        return false;

    const QString oldPath = presetFilePath(name);
    if (oldPath != newPath && !oldPath.isEmpty() && QFile::exists(oldPath)
        && !QFile::remove(oldPath)) {
        QFile::remove(newPath);
        return false;
    }

    m_presets[index] = updated;
    saveLibrary();
    return true;
}

bool PresetLibrary::importPreset(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError) return false;

    EffectPreset preset = EffectPreset::fromJson(doc.object());
    if (preset.name.isEmpty()) return false;

    preset.isBuiltIn = false;
    return addPreset(preset);
}

bool PresetLibrary::exportPreset(const QString &name, const QString &filePath) const
{
    EffectPreset preset = findByName(name);
    if (preset.name.isEmpty()) return false;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) return false;

    QJsonDocument doc(preset.toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

bool PresetLibrary::saveClipStackPreset(const QString &name, const ClipInfo &clip,
                                        bool includeKeyframes, QString *savedPath)
{
    const QString trimmedName = name.trimmed();
    if (trimmedName.isEmpty())
        return false;

    for (const EffectPreset &existing : m_presets) {
        if (existing.name == trimmedName && existing.isBuiltIn)
            return false;
    }

    EffectPreset preset = EffectPreset::fromClipStack(trimmedName, clip, includeKeyframes);
    const QString filePath = presetFilePath(trimmedName);
    if (filePath.isEmpty() || !writePresetJsonFile(filePath, preset))
        return false;

    bool updated = false;
    for (EffectPreset &existing : m_presets) {
        if (existing.name == trimmedName) {
            existing = preset;
            updated = true;
            break;
        }
    }
    if (!updated)
        m_presets.append(preset);

    if (savedPath)
        *savedPath = filePath;
    return true;
}

// --- Named effect-stack JSON presets ---

bool PresetLibrary::loadClipStackPreset(const QString &name, EffectPreset *preset) const
{
    if (!preset)
        return false;

    const QString filePath = presetFilePath(name);
    if (!filePath.isEmpty()) {
        QFile file(filePath);
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                return false;
            EffectPreset loaded = EffectPreset::fromJson(doc.object());
            if (loaded.name.isEmpty())
                loaded.name = name.trimmed();
            if (loaded.name.isEmpty())
                return false;
            *preset = loaded;
            return true;
        }
    }

    const EffectPreset loaded = findByName(name);
    if (loaded.name.isEmpty())
        return false;
    *preset = loaded;
    return true;
}

bool PresetLibrary::applyClipStackPreset(const QString &name, ClipInfo &clip,
                                         bool applyKeyframes) const
{
    EffectPreset preset;
    if (!loadClipStackPreset(name, &preset))
        return false;
    preset.applyToClipStack(clip, applyKeyframes);
    return true;
}

QString PresetLibrary::presetDirectory()
{
    return presetDirectoryPath(true);
}

QString PresetLibrary::presetFilePath(const QString &name)
{
    const QString fileName = presetFileNameForName(name);
    if (fileName.isEmpty())
        return QString();
    return QDir(presetDirectory()).filePath(fileName);
}

// --- Persist library ---

QString PresetLibrary::libraryPath()
{
    QString dir = QDir::homePath() + "/.veditor";
    QDir().mkpath(dir);
    return dir + "/presets.json";
}

bool PresetLibrary::saveLibrary() const
{
    QJsonArray arr;
    for (const auto &p : m_presets)
        arr.append(p.toJson());

    QFile file(libraryPath());
    if (!file.open(QIODevice::WriteOnly)) return false;

    QJsonDocument doc(arr);
    file.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

bool PresetLibrary::loadLibrary()
{
    QFile file(libraryPath());
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) return false;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
        if (err.error != QJsonParseError::NoError) return false;

        const QJsonArray arr = doc.array();
        for (const auto &v : arr) {
            EffectPreset preset = EffectPreset::fromJson(v.toObject());
            if (preset.name.isEmpty()) continue;

            // Skip if a built-in with the same name already exists
            bool exists = false;
            for (const auto &p : m_presets) {
                if (p.name == preset.name) { exists = true; break; }
            }
            if (!exists)
                m_presets.append(preset);
        }
    }

    const QDir presetDir(presetDirectoryPath(false));
    if (presetDir.exists()) {
        const QStringList files = presetDir.entryList(QStringList() << QStringLiteral("*.json"),
                                                      QDir::Files | QDir::Readable,
                                                      QDir::Name);
        for (const QString &fileName : files) {
            EffectPreset preset;
            if (readPresetJsonFile(presetDir.filePath(fileName), &preset))
                mergeUserPreset(m_presets, preset);
        }
    }

    return true;
}

// --- Apply ---

QPair<ColorCorrection, QVector<VideoEffect>>
PresetLibrary::applyPreset(const QString &presetName) const
{
    EffectPreset preset = findByName(presetName);
    return { preset.colorCorrection, preset.effects };
}

// ===== JSON helpers =====

QJsonObject PresetLibrary::colorCorrectionToJson(const ColorCorrection &cc)
{
    QJsonObject obj;
    obj["brightness"]  = cc.brightness;
    obj["contrast"]    = cc.contrast;
    obj["saturation"]  = cc.saturation;
    obj["hue"]         = cc.hue;
    obj["temperature"] = cc.temperature;
    obj["tint"]        = cc.tint;
    obj["gamma"]       = cc.gamma;
    obj["highlights"]  = cc.highlights;
    obj["shadows"]     = cc.shadows;
    obj["exposure"]    = cc.exposure;
    auto addIfNonZero = [&obj](const QString &key, double value) {
        if (value != 0.0)
            obj[key] = value;
    };
    addIfNonZero(QStringLiteral("liftR"), cc.liftR);
    addIfNonZero(QStringLiteral("liftG"), cc.liftG);
    addIfNonZero(QStringLiteral("liftB"), cc.liftB);
    addIfNonZero(QStringLiteral("gammaR"), cc.gammaR);
    addIfNonZero(QStringLiteral("gammaG"), cc.gammaG);
    addIfNonZero(QStringLiteral("gammaB"), cc.gammaB);
    addIfNonZero(QStringLiteral("gainR"), cc.gainR);
    addIfNonZero(QStringLiteral("gainG"), cc.gainG);
    addIfNonZero(QStringLiteral("gainB"), cc.gainB);
    return obj;
}

ColorCorrection PresetLibrary::colorCorrectionFromJson(const QJsonObject &obj)
{
    ColorCorrection cc;
    cc.brightness  = obj["brightness"].toDouble(0.0);
    cc.contrast    = obj["contrast"].toDouble(0.0);
    cc.saturation  = obj["saturation"].toDouble(0.0);
    cc.hue         = obj["hue"].toDouble(0.0);
    cc.temperature = obj["temperature"].toDouble(0.0);
    cc.tint        = obj["tint"].toDouble(0.0);
    cc.gamma       = obj["gamma"].toDouble(1.0);
    cc.highlights  = obj["highlights"].toDouble(0.0);
    cc.shadows     = obj["shadows"].toDouble(0.0);
    cc.exposure    = obj["exposure"].toDouble(0.0);
    cc.liftR       = obj["liftR"].toDouble(0.0);
    cc.liftG       = obj["liftG"].toDouble(0.0);
    cc.liftB       = obj["liftB"].toDouble(0.0);
    cc.gammaR      = obj["gammaR"].toDouble(0.0);
    cc.gammaG      = obj["gammaG"].toDouble(0.0);
    cc.gammaB      = obj["gammaB"].toDouble(0.0);
    cc.gainR       = obj["gainR"].toDouble(0.0);
    cc.gainG       = obj["gainG"].toDouble(0.0);
    cc.gainB       = obj["gainB"].toDouble(0.0);
    return cc;
}

QJsonObject PresetLibrary::videoEffectToJson(const VideoEffect &ve)
{
    QJsonObject obj;
    obj["type"]      = effectTypeKey(ve.type);
    obj["typeName"]  = VideoEffect::typeName(ve.type);
    obj["typeId"]    = static_cast<int>(ve.type);
    obj["enabled"]   = ve.enabled;
    obj["param1"]    = ve.param1;
    obj["param2"]    = ve.param2;
    obj["param3"]    = ve.param3;
    obj["keyColor"]  = ve.keyColor.name(QColor::HexRgb);
    obj["keyColorR"] = ve.keyColor.red();
    obj["keyColorG"] = ve.keyColor.green();
    obj["keyColorB"] = ve.keyColor.blue();
    if (ve.startSec != -1.0)
        obj["startSec"] = ve.startSec;
    if (ve.endSec != -1.0)
        obj["endSec"] = ve.endSec;
    return obj;
}

VideoEffect PresetLibrary::videoEffectFromJson(const QJsonObject &obj)
{
    VideoEffect ve;
    ve.type     = effectTypeFromJson(obj);
    ve.enabled  = obj["enabled"].toBool(true);
    ve.param1   = obj["param1"].toDouble(0.0);
    ve.param2   = obj["param2"].toDouble(0.0);
    ve.param3   = obj["param3"].toDouble(0.0);
    ve.startSec = obj["startSec"].toDouble(-1.0);
    ve.endSec   = obj["endSec"].toDouble(-1.0);

    QColor keyColor(obj["keyColor"].toString());
    if (keyColor.isValid()) {
        ve.keyColor = keyColor;
    } else if (obj.contains("keyColorR") || obj.contains("keyColorG") || obj.contains("keyColorB")) {
        ve.keyColor = QColor(
            obj["keyColorR"].toInt(0),
            obj["keyColorG"].toInt(255),
            obj["keyColorB"].toInt(0));
    }
    return ve;
}

// ===== Built-in presets =====

void PresetLibrary::registerBuiltins()
{
    QDateTime now = QDateTime::currentDateTime();

    // --- Cinematic Warm ---
    {
        EffectPreset p;
        p.name        = "Cinematic Warm";
        p.description = "Warm temperature with slight contrast boost and vignette";
        p.category    = "Cinematic";
        p.author      = "v-editor";
        p.isBuiltIn   = true;
        p.createdAt   = now;
        p.modifiedAt  = now;

        p.colorCorrection.temperature = 30.0;
        p.colorCorrection.contrast    = 10.0;
        p.colorCorrection.saturation  = 5.0;
        p.colorCorrection.shadows     = -10.0;

        p.effects.append(VideoEffect::createVignette(0.3, 0.85));

        m_presets.append(p);
    }

    // --- Film Noir ---
    {
        EffectPreset p;
        p.name        = "Film Noir";
        p.description = "Classic grayscale with high contrast and vignette";
        p.category    = "Black & White";
        p.author      = "v-editor";
        p.isBuiltIn   = true;
        p.createdAt   = now;
        p.modifiedAt  = now;

        p.colorCorrection.contrast   = 40.0;
        p.colorCorrection.brightness = -5.0;
        p.colorCorrection.shadows    = -20.0;

        p.effects.append(VideoEffect::createGrayscale());
        p.effects.append(VideoEffect::createVignette(0.5, 0.75));

        m_presets.append(p);
    }

    // --- Vintage ---
    {
        EffectPreset p;
        p.name        = "Vintage";
        p.description = "Sepia tones with reduced saturation and slight warm shift";
        p.category    = "Vintage";
        p.author      = "v-editor";
        p.isBuiltIn   = true;
        p.createdAt   = now;
        p.modifiedAt  = now;

        p.colorCorrection.saturation  = -30.0;
        p.colorCorrection.temperature = 15.0;
        p.colorCorrection.contrast    = 5.0;
        p.colorCorrection.gamma       = 1.1;

        p.effects.append(VideoEffect::createSepia(0.6));

        m_presets.append(p);
    }

    // --- Cool Tone ---
    {
        EffectPreset p;
        p.name        = "Cool Tone";
        p.description = "Cool temperature with slight blue tint and sharpen";
        p.category    = "Color";
        p.author      = "v-editor";
        p.isBuiltIn   = true;
        p.createdAt   = now;
        p.modifiedAt  = now;

        p.colorCorrection.temperature = -25.0;
        p.colorCorrection.tint        = -10.0;
        p.colorCorrection.contrast    = 5.0;
        p.colorCorrection.saturation  = 10.0;

        p.effects.append(VideoEffect::createSharpen(1.0));

        m_presets.append(p);
    }

    // --- HDR Look ---
    {
        EffectPreset p;
        p.name        = "HDR Look";
        p.description = "Boosted highlights, shadows, saturation and contrast for an HDR look";
        p.category    = "Cinematic";
        p.author      = "v-editor";
        p.isBuiltIn   = true;
        p.createdAt   = now;
        p.modifiedAt  = now;

        p.colorCorrection.highlights = 30.0;
        p.colorCorrection.shadows    = 30.0;
        p.colorCorrection.saturation = 25.0;
        p.colorCorrection.contrast   = 20.0;
        p.colorCorrection.exposure   = 0.2;

        m_presets.append(p);
    }

    // --- Dreamy ---
    {
        EffectPreset p;
        p.name        = "Dreamy";
        p.description = "Soft glow with warm tint and reduced contrast";
        p.category    = "Stylize";
        p.author      = "v-editor";
        p.isBuiltIn   = true;
        p.createdAt   = now;
        p.modifiedAt  = now;

        p.colorCorrection.contrast    = -15.0;
        p.colorCorrection.temperature = 15.0;
        p.colorCorrection.brightness  = 5.0;
        p.colorCorrection.saturation  = -10.0;
        p.colorCorrection.gamma       = 1.15;

        p.effects.append(VideoEffect::createBlur(3.0));

        m_presets.append(p);
    }

    // --- High Contrast B&W ---
    {
        EffectPreset p;
        p.name        = "High Contrast B&W";
        p.description = "Grayscale with high contrast and sharpen";
        p.category    = "Black & White";
        p.author      = "v-editor";
        p.isBuiltIn   = true;
        p.createdAt   = now;
        p.modifiedAt  = now;

        p.colorCorrection.contrast   = 50.0;
        p.colorCorrection.brightness = 5.0;

        p.effects.append(VideoEffect::createGrayscale());
        p.effects.append(VideoEffect::createSharpen(2.0));

        m_presets.append(p);
    }

    // --- Sunset ---
    {
        EffectPreset p;
        p.name        = "Sunset";
        p.description = "Warm temperature with boosted shadows and orange hue shift";
        p.category    = "Color";
        p.author      = "v-editor";
        p.isBuiltIn   = true;
        p.createdAt   = now;
        p.modifiedAt  = now;

        p.colorCorrection.temperature = 40.0;
        p.colorCorrection.shadows     = 20.0;
        p.colorCorrection.hue         = 15.0;
        p.colorCorrection.saturation  = 15.0;
        p.colorCorrection.tint        = 10.0;

        m_presets.append(p);
    }
}
