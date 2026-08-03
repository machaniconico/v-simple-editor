#include "EffectLibraryModel.h"

#include "EffectParamSchema.h"
#include "EffectPreset.h"
#include "LayerCompositor.h"
#include "Timeline.h"
#include "VfxFootageLibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QVector3D>
#include <QtMath>

#include <algorithm>

namespace efxlib {

namespace {

QString encodedId(const QString &prefix, const QString &name)
{
    return prefix + QStringLiteral(":")
        + QString::fromLatin1(QUrl::toPercentEncoding(name));
}

QString kindTag(SourceKind kind)
{
    switch (kind) {
    case SourceKind::Shader:   return QStringLiteral("shader");
    case SourceKind::Particle: return QStringLiteral("particle");
    case SourceKind::VfxGenerator: return QStringLiteral("vfx-generator");
    case SourceKind::Plugin:   return QStringLiteral("plugin");
    case SourceKind::AeFx:     return QStringLiteral("aefx");
    case SourceKind::Preset:   return QStringLiteral("preset");
    case SourceKind::Footage:  return QStringLiteral("footage");
    }
    return QStringLiteral("effect");
}

QString shaderCategory(const QString &category)
{
    if (category.compare(QStringLiteral("Blur"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("ブラー");
    if (category.compare(QStringLiteral("Color"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("カラー");
    if (category.compare(QStringLiteral("Distort"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("ディストーション");
    if (category.compare(QStringLiteral("Stylize"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("スタイライズ");
    return category.isEmpty() ? QStringLiteral("その他") : category;
}

QString particleName(ParticleType type)
{
    switch (type) {
    case ParticleType::Snow:     return QStringLiteral("スノー");
    case ParticleType::Rain:     return QStringLiteral("レイン");
    case ParticleType::Spark:    return QStringLiteral("スパーク");
    case ParticleType::Smoke:    return QStringLiteral("スモーク");
    case ParticleType::Fire:     return QStringLiteral("ファイア");
    case ParticleType::Confetti: return QStringLiteral("コンフェッティ");
    case ParticleType::Dust:     return QStringLiteral("ダスト");
    case ParticleType::Bubble:   return QStringLiteral("バブル");
    case ParticleType::Star:     return QStringLiteral("スター");
    case ParticleType::Custom:   return QStringLiteral("カスタム");
    }
    return QStringLiteral("パーティクル");
}

QString videoCategory(VideoEffectType type)
{
    switch (type) {
    case VideoEffectType::Blur:
    case VideoEffectType::GaussianBlur:
    case VideoEffectType::DirectionalBlur:
    case VideoEffectType::RadialBlur:
        return QStringLiteral("ブラー");
    case VideoEffectType::ChromaKey:
    case VideoEffectType::Sepia:
    case VideoEffectType::Grayscale:
    case VideoEffectType::Invert:
    case VideoEffectType::Levels:
    case VideoEffectType::Tint:
    case VideoEffectType::BlackWhite:
    case VideoEffectType::Exposure:
    case VideoEffectType::HueSaturation:
    case VideoEffectType::Curves:
    case VideoEffectType::ChannelMixer:
    case VideoEffectType::Vibrance:
    case VideoEffectType::PhotoFilter:
    case VideoEffectType::Tritone:
    case VideoEffectType::BrightnessContrast:
        return QStringLiteral("カラー");
    case VideoEffectType::Mosaic:
    case VideoEffectType::DisplacementMap:
    case VideoEffectType::FractalNoiseGen:
    case VideoEffectType::RGBSplit:
    case VideoEffectType::WaveWarp:
    case VideoEffectType::Ripple:
    case VideoEffectType::GlitchVHS:
    case VideoEffectType::Bulge:
    case VideoEffectType::Twirl:
    case VideoEffectType::Mirror:
    case VideoEffectType::PolarCoordinates:
    case VideoEffectType::MotionTile:
    case VideoEffectType::CornerPinSimple:
        return QStringLiteral("ディストーション");
    case VideoEffectType::Vignette:
    case VideoEffectType::Noise:
    case VideoEffectType::Glow:
    case VideoEffectType::FindEdges:
    case VideoEffectType::Emboss:
    case VideoEffectType::Posterize:
    case VideoEffectType::Threshold:
    case VideoEffectType::Solarize:
    case VideoEffectType::GradientRamp:
    case VideoEffectType::Fill:
    case VideoEffectType::Bloom:
    case VideoEffectType::Scanlines:
    case VideoEffectType::Halftone:
    case VideoEffectType::Sharpen:
        return QStringLiteral("スタイライズ");
    case VideoEffectType::None:
        return QStringLiteral("その他");
    }
    return QStringLiteral("その他");
}

QString localizedVideoName(VideoEffectType type)
{
    const QString original = VideoEffect::typeName(type);
    if (original == QStringLiteral("Blur")) return QStringLiteral("ブラー");
    if (original == QStringLiteral("Sharpen")) return QStringLiteral("シャープ");
    if (original == QStringLiteral("Mosaic")) return QStringLiteral("モザイク");
    if (original == QStringLiteral("Vignette")) return QStringLiteral("ビネット");
    if (original == QStringLiteral("Noise")) return QStringLiteral("ノイズ");
    if (original == QStringLiteral("Displacement Map")) return QStringLiteral("ディスプレイスメント");
    if (original == QStringLiteral("Fractal Noise")) return QStringLiteral("フラクタルノイズ");
    return original;
}

VideoEffectType shaderVideoType(const QString &name)
{
    if (name.contains(QStringLiteral("Chromatic"), Qt::CaseInsensitive))
        return VideoEffectType::RGBSplit;
    if (name.contains(QStringLiteral("Halftone"), Qt::CaseInsensitive))
        return VideoEffectType::Halftone;
    if (name == QStringLiteral("Duotone"))
        return VideoEffectType::Tritone;
    if (name.contains(QStringLiteral("Gradient"), Qt::CaseInsensitive))
        return VideoEffectType::GradientRamp;
    if (name == QStringLiteral("Gaussian Blur"))
        return VideoEffectType::GaussianBlur;
    if (name == QStringLiteral("Radial Blur"))
        return VideoEffectType::RadialBlur;
    if (name == QStringLiteral("Directional Blur"))
        return VideoEffectType::DirectionalBlur;
    if (name == QStringLiteral("Tilt Shift"))
        return VideoEffectType::GaussianBlur;
    if (name == QStringLiteral("Barrel Distortion"))
        return VideoEffectType::Bulge;
    if (name == QStringLiteral("Ripple"))
        return VideoEffectType::Ripple;
    if (name == QStringLiteral("Pixelate"))
        return VideoEffectType::Mosaic;
    if (name == QStringLiteral("Glitch"))
        return VideoEffectType::GlitchVHS;
    if (name == QStringLiteral("Film Grain"))
        return VideoEffectType::Noise;
    if (name == QStringLiteral("Vignette"))
        return VideoEffectType::Vignette;
    if (name.contains(QStringLiteral("CRT"), Qt::CaseInsensitive))
        return VideoEffectType::Scanlines;
    if (name.contains(QStringLiteral("Sketch"), Qt::CaseInsensitive))
        return VideoEffectType::FindEdges;
    if (name == QStringLiteral("Oil Paint"))
        return VideoEffectType::Posterize;
    return VideoEffectType::None;
}

VideoEffectType pluginVideoType(const QString &name)
{
    if (name == QStringLiteral("Glow")) return VideoEffectType::Glow;
    if (name == QStringLiteral("Emboss")) return VideoEffectType::Emboss;
    if (name == QStringLiteral("Posterize")) return VideoEffectType::Posterize;
    if (name == QStringLiteral("Edge Detect")) return VideoEffectType::FindEdges;
    if (name == QStringLiteral("Color Shift")) return VideoEffectType::ChannelMixer;
    return VideoEffectType::None;
}

QString shaderNativeParam(const QString &effectName, const QString &shaderParam)
{
    if (effectName == QStringLiteral("Chromatic Aberration")
        && shaderParam == QStringLiteral("uAmount")) return QStringLiteral("offsetX");
    if (effectName == QStringLiteral("Color Halftone")
        && shaderParam == QStringLiteral("uDotSize")) return QStringLiteral("dotSize");
    if (effectName == QStringLiteral("Duotone")
        && shaderParam == QStringLiteral("uColorHighlight")) return QStringLiteral("keyColor");
    if (effectName == QStringLiteral("Gradient Map")
        && shaderParam == QStringLiteral("uStop4")) return QStringLiteral("keyColor");
    if (effectName == QStringLiteral("Gaussian Blur")
        && shaderParam == QStringLiteral("uRadius")) return QStringLiteral("radius");
    if (effectName == QStringLiteral("Radial Blur")
        && shaderParam == QStringLiteral("uStrength")) return QStringLiteral("amount");
    if (effectName == QStringLiteral("Directional Blur")) {
        if (shaderParam == QStringLiteral("uAngle")) return QStringLiteral("angle");
        if (shaderParam == QStringLiteral("uStrength")) return QStringLiteral("length");
    }
    if (effectName == QStringLiteral("Tilt Shift")
        && shaderParam == QStringLiteral("uBlurRadius")) return QStringLiteral("radius");
    if (effectName == QStringLiteral("Barrel Distortion")) {
        if (shaderParam == QStringLiteral("uK1")) return QStringLiteral("amount");
        if (shaderParam == QStringLiteral("uK2")) return QStringLiteral("radius");
    }
    if (effectName == QStringLiteral("Ripple")) {
        if (shaderParam == QStringLiteral("uAmplitude")) return QStringLiteral("amplitude");
        if (shaderParam == QStringLiteral("uFrequency")) return QStringLiteral("wavelength");
    }
    if (effectName == QStringLiteral("Pixelate")
        && shaderParam == QStringLiteral("uBlockSize")) return QStringLiteral("blockSize");
    if (effectName == QStringLiteral("Glitch")) {
        if (shaderParam == QStringLiteral("uIntensity")) return QStringLiteral("intensity");
        if (shaderParam == QStringLiteral("uScanlines")) return QStringLiteral("opacity");
    }
    if (effectName == QStringLiteral("Film Grain")
        && shaderParam == QStringLiteral("uAmount")) return QStringLiteral("amount");
    if (effectName == QStringLiteral("Vignette")) {
        if (shaderParam == QStringLiteral("uIntensity")) return QStringLiteral("intensity");
        if (shaderParam == QStringLiteral("uRadius")) return QStringLiteral("radius");
    }
    if (effectName == QStringLiteral("CRT / Retro")
        && shaderParam == QStringLiteral("uScanlineIntensity")) return QStringLiteral("opacity");
    if (effectName == QStringLiteral("Sketch / Pencil")
        && shaderParam == QStringLiteral("uEdgeStrength")) return QStringLiteral("intensity");
    if (effectName == QStringLiteral("Oil Paint")
        && shaderParam == QStringLiteral("uRadius")) return QStringLiteral("levels");
    return QString();
}

QString pluginNativeParam(const QString &pluginName, const QString &pluginParam)
{
    if (pluginName == QStringLiteral("Glow")) {
        if (pluginParam == QStringLiteral("Radius")) return QStringLiteral("radius");
        if (pluginParam == QStringLiteral("Intensity")) return QStringLiteral("intensity");
    }
    if (pluginName == QStringLiteral("Emboss")
        && pluginParam == QStringLiteral("Strength")) return QStringLiteral("amount");
    if (pluginName == QStringLiteral("Posterize")
        && pluginParam == QStringLiteral("Levels")) return QStringLiteral("levels");
    if (pluginName == QStringLiteral("Edge Detect")
        && pluginParam == QStringLiteral("Threshold")) return QStringLiteral("intensity");
    if (pluginName == QStringLiteral("Color Shift")) {
        if (pluginParam == QStringLiteral("Red Shift")) return QStringLiteral("redFromRed");
        if (pluginParam == QStringLiteral("Green Shift")) return QStringLiteral("greenFromGreen");
        if (pluginParam == QStringLiteral("Blue Shift")) return QStringLiteral("blueFromBlue");
    }
    return QString();
}

QColor colorFromVariant(const QVariant &value)
{
    if (value.canConvert<QColor>())
        return value.value<QColor>();
    if (value.canConvert<QVector3D>()) {
        const QVector3D rgb = value.value<QVector3D>();
        return QColor::fromRgbF(qBound(0.0f, rgb.x(), 1.0f),
                                qBound(0.0f, rgb.y(), 1.0f),
                                qBound(0.0f, rgb.z(), 1.0f));
    }
    return QColor();
}

QColor colorFromEncodedDouble(double value)
{
    return QColor::fromRgb(static_cast<QRgb>(qRound64(value)));
}

void addCommonTags(LibraryEntry &entry, const QString &sourceName)
{
    entry.tags.append(sourceName);
    entry.tags.append(sourceName.toCaseFolded());
    entry.tags.append(kindTag(entry.kind));
    if (!entry.category.isEmpty())
        entry.tags.append(entry.category);
    if (!entry.displayName.isEmpty() && entry.displayName != sourceName)
        entry.tags.append(entry.displayName);
}

} // namespace

EffectLibraryModel::EffectLibraryModel()
{
    registerAll();
    loadState();
}

void EffectLibraryModel::addEntry(const EntryData &data)
{
    if (data.entry.id.isEmpty() || m_data.contains(data.entry.id))
        return;
    m_entries.append(data.entry);
    m_data.insert(data.entry.id, data);
}

void EffectLibraryModel::registerAll()
{
    for (const LibraryEntry &entry : m_entries) {
        if (entry.favorite)
            m_favoriteIds.insert(entry.id);
    }

    m_entries.clear();
    m_data.clear();
    m_parameterOverrides.clear();
    m_footageFrameCache.clear();

    const auto shaderEffects = ShaderEffectLibrary::instance().allEffects();
    for (const ShaderEffectDef &def : shaderEffects) {
        EntryData data;
        data.entry.id = encodedId(QStringLiteral("shader"), def.name);
        data.entry.displayName = def.name;
        data.entry.category = shaderCategory(def.category);
        data.entry.kind = SourceKind::Shader;
        data.sourceName = def.name;
        data.shader = def;
        addCommonTags(data.entry, def.name);
        addEntry(data);
    }

    // ParticleType uses Custom as a sentinel. Iterating the enum range keeps
    // registration synchronized with every concrete type without a second
    // fixed list in the library model.
    const QMap<QString, ParticleEmitterConfig> particleConfigs =
        ParticleSystem::presetConfigs();
    const int particleTypeCount = static_cast<int>(ParticleType::Custom);
    for (int typeIndex = 0; typeIndex < particleTypeCount; ++typeIndex) {
        const ParticleType type = static_cast<ParticleType>(typeIndex);
        ParticleEmitterConfig config;
        config.type = type;
        for (auto it = particleConfigs.cbegin(); it != particleConfigs.cend(); ++it) {
            if (it.value().type == type) {
                config = it.value();
                break;
            }
        }

        EntryData data;
        data.entry.id = QStringLiteral("particle:%1").arg(typeIndex);
        data.entry.displayName = particleName(type);
        data.entry.category = QStringLiteral("パーティクル");
        data.entry.kind = SourceKind::Particle;
        data.sourceName = particleName(type);
        data.particle = config;
        addCommonTags(data.entry, data.sourceName);
        data.entry.tags.append(QStringLiteral("ParticleType:%1").arg(typeIndex));
        addEntry(data);
    }

    // VfxGenerators owns the concrete-type registry. The library only walks
    // it, so adding a generator cannot silently omit its catalog entry.
    for (const VfxGeneratorType type : VfxGenerators::allTypes()) {
        const int typeIndex = static_cast<int>(type);
        const QString sourceName = VfxGenerators::typeName(type);
        EntryData data;
        data.entry.id = encodedId(QStringLiteral("vfx"), sourceName);
        data.entry.displayName = VfxGenerators::displayName(type);
        data.entry.category = QStringLiteral("VFX ジェネレータ");
        data.entry.kind = SourceKind::VfxGenerator;
        data.sourceName = sourceName;
        data.vfxType = type;
        addCommonTags(data.entry, sourceName);
        data.entry.tags.append(QStringLiteral("VfxGeneratorType:%1").arg(typeIndex));
        addEntry(data);
    }

    const auto plugins = PluginRegistry::instance().allPlugins();
    for (const auto &plugin : plugins) {
        if (!plugin)
            continue;
        EntryData data;
        data.entry.id = encodedId(QStringLiteral("plugin"), plugin->name());
        data.entry.displayName = plugin->name();
        data.entry.category = shaderCategory(plugin->category());
        data.entry.kind = SourceKind::Plugin;
        data.sourceName = plugin->name();
        data.plugin = plugin;
        addCommonTags(data.entry, plugin->name());
        addEntry(data);
    }

    const auto aeTypes = VideoEffect::allTypes();
    for (VideoEffectType type : aeTypes) {
        EntryData data;
        const QString sourceName = VideoEffect::typeName(type);
        data.entry.id = encodedId(QStringLiteral("aefx"), sourceName);
        data.entry.displayName = localizedVideoName(type);
        data.entry.category = videoCategory(type);
        data.entry.kind = SourceKind::AeFx;
        data.sourceName = sourceName;
        data.videoType = type;
        addCommonTags(data.entry, sourceName);
        addEntry(data);
    }

    const auto presets = PresetLibrary::instance().allPresets();
    for (const EffectPreset &preset : presets) {
        if (preset.name.isEmpty())
            continue;
        EntryData data;
        data.entry.id = encodedId(QStringLiteral("preset"), preset.name);
        data.entry.displayName = preset.name;
        data.entry.category = QStringLiteral("プリセット");
        data.entry.kind = SourceKind::Preset;
        data.entry.isUserPreset = !preset.isBuiltIn;
        data.sourceName = preset.name;
        data.presetName = preset.name;
        addCommonTags(data.entry, preset.name);
        if (!preset.category.isEmpty())
            data.entry.tags.append(preset.category);
        addEntry(data);
    }

    // VFX-C: footage is a first-class library source, but the directory is
    // intentionally optional. VfxFootageLibrary keeps unreadable supported
    // files in the catalog, so a bad file is visible and diagnosable rather
    // than silently discarded.
    const QVector<vfxfootage::FootageItem> footageItems =
        vfxfootage::VfxFootageLibrary::scan(
            vfxfootage::VfxFootageLibrary::defaultDirectory());
    for (const vfxfootage::FootageItem &item : footageItems) {
        EntryData data;
        data.entry.id = encodedId(QStringLiteral("footage"), item.filePath);
        data.entry.displayName = item.displayName;
        data.entry.category = item.category.isEmpty()
            ? QStringLiteral("その他") : item.category;
        data.entry.kind = SourceKind::Footage;
        data.sourceName = item.filePath;
        data.footagePath = item.filePath;
        data.entry.tags = item.tags;
        addCommonTags(data.entry, item.displayName);
        addEntry(data);
    }

    applyFavoriteIds();
}

QVector<LibraryEntry> EffectLibraryModel::entries() const
{
    return m_entries;
}

QVector<LibraryEntry> EffectLibraryModel::search(const QString &query) const
{
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return entries();

    QVector<LibraryEntry> result;
    for (const LibraryEntry &entry : m_entries) {
        bool match = entry.displayName.contains(needle, Qt::CaseInsensitive)
            || entry.id.contains(needle, Qt::CaseInsensitive);
        if (!match) {
            for (const QString &tag : entry.tags) {
                if (tag.contains(needle, Qt::CaseInsensitive)) {
                    match = true;
                    break;
                }
            }
        }
        if (match)
            result.append(entry);
    }
    return result;
}

QVector<LibraryEntry> EffectLibraryModel::byCategory(const QString &category) const
{
    QVector<LibraryEntry> result;
    for (const LibraryEntry &entry : m_entries) {
        if (entry.category == category)
            result.append(entry);
    }
    return result;
}

QStringList EffectLibraryModel::categories() const
{
    QStringList result;
    for (const LibraryEntry &entry : m_entries) {
        if (!result.contains(entry.category))
            result.append(entry.category);
    }
    return result;
}

void EffectLibraryModel::setFavorite(const QString &id, bool on)
{
    if (!m_data.contains(id))
        return;
    if (on)
        m_favoriteIds.insert(id);
    else
        m_favoriteIds.remove(id);
    applyFavoriteIds();
}

QVector<LibraryEntry> EffectLibraryModel::favorites() const
{
    QVector<LibraryEntry> result;
    for (const LibraryEntry &entry : m_entries) {
        if (entry.favorite)
            result.append(entry);
    }
    return result;
}

void EffectLibraryModel::applyFavoriteIds()
{
    for (LibraryEntry &entry : m_entries)
        entry.favorite = m_favoriteIds.contains(entry.id);
    for (auto it = m_data.begin(); it != m_data.end(); ++it)
        it.value().entry.favorite = m_favoriteIds.contains(it.key());
}

QString EffectLibraryModel::statePath()
{
    const QString overridePath = qEnvironmentVariable(
        "VEDITOR_EFFECT_LIBRARY_STATE");
    if (!overridePath.isEmpty())
        return overridePath;

    QString dir = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.veditor");
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("effect-library.json"));
}

bool EffectLibraryModel::saveState() const
{
    const QString path = statePath();
    const QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    QJsonObject root;
    QJsonArray favoriteIds;
    QJsonArray userPresetIds;
    for (const LibraryEntry &entry : m_entries) {
        if (entry.favorite)
            favoriteIds.append(entry.id);
        if (entry.isUserPreset)
            userPresetIds.append(entry.id);
    }
    root[QStringLiteral("favorites")] = favoriteIds;
    root[QStringLiteral("userPresets")] = userPresetIds;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool EffectLibraryModel::loadState()
{
    const QString path = statePath();
    QFile file(path);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;

    m_favoriteIds.clear();
    const QJsonArray favoriteIds = document.object().value(
        QStringLiteral("favorites")).toArray();
    for (const QJsonValue &value : favoriteIds)
        m_favoriteIds.insert(value.toString());
    applyFavoriteIds();
    return true;
}

bool EffectLibraryModel::entryById(const QString &id, LibraryEntry *entry) const
{
    const EntryData *data = dataForId(id);
    if (!data)
        return false;
    if (entry)
        *entry = data->entry;
    return true;
}

bool EffectLibraryModel::hasFootageEntries() const
{
    for (const LibraryEntry &entry : m_entries) {
        if (entry.kind == SourceKind::Footage)
            return true;
    }
    return false;
}

const EffectLibraryModel::EntryData *EffectLibraryModel::dataForId(
    const QString &id) const
{
    const auto it = m_data.constFind(id);
    return it == m_data.constEnd() ? nullptr : &it.value();
}

EffectLibraryModel::EntryData *EffectLibraryModel::dataForId(const QString &id)
{
    const auto it = m_data.find(id);
    return it == m_data.end() ? nullptr : &it.value();
}

QVector<ParameterSpec> EffectLibraryModel::parametersForData(
    const EntryData &data) const
{
    QVector<ParameterSpec> result;
    if (data.entry.kind == SourceKind::Shader) {
        for (const ParamDef &param : data.shader.params) {
            // ShaderEffect stores GPU uniform names while the existing CPU
            // VideoEffect API stores named effect parameters. Expose only
            // parameters that have a real CPU counterpart so inspector edits
            // and keyframes cannot silently write an unused value.
            if (shaderNativeParam(data.sourceName, param.name).isEmpty()
                || param.type == ParamType::Vec2
                || param.type == ParamType::Vec3)
                continue;
            ParameterSpec spec;
            spec.name = param.name;
            spec.displayName = param.name;
            spec.minValue = param.minVal.isValid() ? param.minVal.toDouble() : 0.0;
            spec.maxValue = param.maxVal.isValid() ? param.maxVal.toDouble() : 1.0;
            spec.defaultValue = param.defaultVal.isValid()
                ? param.defaultVal.toDouble() : 0.0;
            spec.integer = param.type == ParamType::Int;
            spec.color = param.type == ParamType::Color;
            if (spec.color)
                spec.defaultColor = colorFromVariant(param.defaultVal);
            result.append(spec);
        }
    } else if (data.entry.kind == SourceKind::Plugin && data.plugin) {
        for (const EffectPlugin::ParamDef &param : data.plugin->parameterDefs()) {
            ParameterSpec spec;
            spec.name = param.name;
            spec.displayName = param.name;
            spec.minValue = param.min;
            spec.maxValue = param.max;
            spec.defaultValue = param.defaultValue;
            spec.integer = false;
            result.append(spec);
        }
    } else if (data.entry.kind == SourceKind::AeFx) {
        for (const effectctrl::ParamDef &param : effectctrl::paramSchemaFor(data.videoType)) {
            ParameterSpec spec;
            spec.name = param.name;
            spec.displayName = param.displayLabel;
            spec.minValue = param.minVal;
            spec.maxValue = param.maxVal;
            spec.defaultValue = param.defaultVal;
            spec.integer = param.type == effectctrl::ParamType::Int
                || param.type == effectctrl::ParamType::Choice;
            spec.color = param.type == effectctrl::ParamType::Color;
            if (spec.color)
                spec.defaultColor = colorFromEncodedDouble(param.defaultVal);
            result.append(spec);
        }
    } else if (data.entry.kind == SourceKind::VfxGenerator) {
        for (const VfxParameterSpec &param : VfxGenerators::parameterSpecs(data.vfxType)) {
            ParameterSpec spec;
            spec.name = param.name;
            spec.displayName = param.displayName;
            spec.minValue = param.minValue;
            spec.maxValue = param.maxValue;
            spec.defaultValue = param.defaultValue;
            spec.integer = param.integer;
            result.append(spec);
        }
    } else if (data.entry.kind == SourceKind::Footage) {
        ParameterSpec intensity;
        intensity.name = QStringLiteral("vfxIntensity");
        intensity.displayName = QStringLiteral("強度");
        intensity.minValue = 0.0;
        intensity.maxValue = 8.0;
        intensity.defaultValue = 1.0;
        result.append(intensity);

        ParameterSpec blackLevel;
        blackLevel.name = QStringLiteral("vfxBlackLevel");
        blackLevel.displayName = QStringLiteral("黒レベル");
        blackLevel.minValue = 0.0;
        blackLevel.maxValue = 64.0;
        blackLevel.defaultValue = 16.0;
        blackLevel.integer = true;
        result.append(blackLevel);
    }
    return result;
}

QVector<ParameterSpec> EffectLibraryModel::parameters(const QString &id) const
{
    const EntryData *data = dataForId(id);
    return data ? parametersForData(*data) : QVector<ParameterSpec>();
}

bool EffectLibraryModel::setParameterOverride(const QString &id,
                                              const QString &name,
                                              const QVariant &value)
{
    const EntryData *data = dataForId(id);
    if (!data)
        return false;
    if (data->entry.kind == SourceKind::Footage
        && name == QStringLiteral("blendMode")) {
        const QString mode = value.toString();
        if (mode.isEmpty())
            return false;
        m_parameterOverrides[id].insert(name, mode);
        return true;
    }
    const auto specs = parametersForData(*data);
    for (const ParameterSpec &spec : specs) {
        if (spec.name == name) {
            m_parameterOverrides[id].insert(name, value);
            return true;
        }
    }
    return false;
}

void EffectLibraryModel::clearParameterOverrides(const QString &id)
{
    m_parameterOverrides.remove(id);
}

bool EffectLibraryModel::buildVideoEffect(const EntryData &data,
                                          VideoEffect *effect) const
{
    if (!effect)
        return false;

    VideoEffectType type = VideoEffectType::None;
    if (data.entry.kind == SourceKind::AeFx)
        type = data.videoType;
    else if (data.entry.kind == SourceKind::Shader)
        type = shaderVideoType(data.sourceName);
    else if (data.entry.kind == SourceKind::Plugin)
        type = pluginVideoType(data.sourceName);
    else
        return false;

    if (type == VideoEffectType::None)
        return false;

    VideoEffect result;
    result.type = type;
    const auto schema = effectctrl::paramSchemaFor(type);
    for (const effectctrl::ParamDef &param : schema) {
        if (param.type == effectctrl::ParamType::Color) {
            effectctrl::setColorParam(result, param.name,
                                      colorFromEncodedDouble(param.defaultVal));
        } else {
            effectctrl::setParamValue(result, param.name, param.defaultVal);
        }
    }

    if (data.entry.kind == SourceKind::Shader) {
        for (const ParamDef &param : data.shader.params) {
            const QString nativeName = shaderNativeParam(data.sourceName, param.name);
            if (nativeName.isEmpty())
                continue;
            const QVariant sourceValue = param.defaultVal;
            if (param.type == ParamType::Color)
                effectctrl::setColorParam(result, nativeName,
                                          colorFromVariant(sourceValue));
            else if (sourceValue.isValid())
                effectctrl::setParamValue(result, nativeName,
                                          sourceValue.toDouble());
        }
    } else if (data.entry.kind == SourceKind::Plugin && data.plugin) {
        for (const EffectPlugin::ParamDef &param : data.plugin->parameterDefs()) {
            const QString nativeName = pluginNativeParam(data.sourceName, param.name);
            if (!nativeName.isEmpty())
                effectctrl::setParamValue(result, nativeName, param.defaultValue);
        }
    }

    const QVariantMap overrides = m_parameterOverrides.value(data.entry.id);
    if (data.entry.kind == SourceKind::Shader) {
        for (const ParamDef &param : data.shader.params) {
            const QString nativeName = shaderNativeParam(data.sourceName, param.name);
            if (nativeName.isEmpty() || !overrides.contains(param.name))
                continue;
            const QVariant value = overrides.value(param.name);
            if (param.type == ParamType::Color)
                effectctrl::setColorParam(result, nativeName,
                                          value.value<QColor>());
            else
                effectctrl::setParamValue(result, nativeName, value.toDouble());
        }
    } else if (data.entry.kind == SourceKind::Plugin && data.plugin) {
        for (const EffectPlugin::ParamDef &param : data.plugin->parameterDefs()) {
            const QString nativeName = pluginNativeParam(data.sourceName, param.name);
            if (nativeName.isEmpty() || !overrides.contains(param.name))
                continue;
            effectctrl::setParamValue(result, nativeName,
                                      overrides.value(param.name).toDouble());
        }
    } else {
        for (const auto &param : schema) {
            if (!overrides.contains(param.name))
                continue;
            const QVariant value = overrides.value(param.name);
            if (param.type == effectctrl::ParamType::Color)
                effectctrl::setColorParam(result, param.name, value.value<QColor>());
            else
                effectctrl::setParamValue(result, param.name, value.toDouble());
        }
    }

    *effect = result;
    return true;
}

bool EffectLibraryModel::buildPluginParams(const EntryData &data,
                                           QVector<double> *params) const
{
    if (!params || !data.plugin)
        return false;
    params->clear();
    const QVariantMap overrides = m_parameterOverrides.value(data.entry.id);
    for (const EffectPlugin::ParamDef &param : data.plugin->parameterDefs())
        params->append(overrides.value(param.name, param.defaultValue).toDouble());
    return true;
}

bool EffectLibraryModel::buildParticleOverlay(const EntryData &data,
                                              const QSize &size,
                                              QImage *overlay) const
{
    if (!overlay || data.entry.kind != SourceKind::Particle)
        return false;
    const QSize safeSize(qMax(1, size.width()), qMax(1, size.height()));
    ParticleSystem system;
    system.setConfig(data.particle);
    const QVector<QImage> frames = system.renderParticleSequence(
        safeSize, 0.0, 1.0 / 30.0, 30.0);
    if (!frames.isEmpty())
        *overlay = frames.last();
    else
        *overlay = system.renderFrame(safeSize, 0.0);
    return !overlay->isNull();
}

bool EffectLibraryModel::buildVfxOverlay(const EntryData &data,
                                         const QSize &size,
                                         QImage *overlay) const
{
    if (!overlay || data.entry.kind != SourceKind::VfxGenerator)
        return false;
    const QSize safeSize(qMax(1, size.width()), qMax(1, size.height()));
    VfxGeneratorParameters parameters = VfxGenerators::defaultParameters(data.vfxType);
    const QVariantMap overrides = m_parameterOverrides.value(data.entry.id);
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
        VfxGenerators::setParameter(parameters, it.key(), it.value());

    const double duration = VfxGenerators::durationSeconds(data.vfxType, parameters);
    const double sampleTime = duration > 0.0 ? qMin(duration * 0.35, 0.35) : 0.0;
    *overlay = VfxGenerators::render(data.vfxType, safeSize, parameters, sampleTime);
    return !overlay->isNull();
}

bool EffectLibraryModel::applyToImage(const QString &id, const QImage &input,
                                      QImage *output) const
{
    if (!output || input.isNull())
        return false;
    const EntryData *data = dataForId(id);
    if (!data)
        return false;

    if (data->entry.kind == SourceKind::Footage) {
        QImage frame = footageFrame(*data);
        if (frame.isNull())
            return false;
        const QVariantMap overrides = m_parameterOverrides.value(data->entry.id);
        const int blackLevel = overrides.value(
            QStringLiteral("vfxBlackLevel"), 16).toInt();
        const double intensity = overrides.value(
            QStringLiteral("vfxIntensity"), 1.0).toDouble();
        const BlendMode blendMode = CompositeLayer::blendModeFromName(
            overrides.value(QStringLiteral("blendMode"),
                            QStringLiteral("Screen")).toString());
        frame = vfxfootage::VfxFootageLibrary::applyBlackLevel(
            frame, blackLevel);
        frame = vfxfootage::VfxFootageLibrary::applyIntensity(frame, intensity);
        *output = LayerCompositor::blendImages(input, frame, blendMode, 1.0);
        return !output->isNull();
    }

    if (data->entry.kind == SourceKind::Plugin) {
        QVector<double> params;
        if (!buildPluginParams(*data, &params))
            return false;
        *output = data->plugin->process(input, params);
        return !output->isNull();
    }

    if (data->entry.kind == SourceKind::Particle) {
        QImage overlay;
        if (!buildParticleOverlay(*data, input.size(), &overlay))
            return false;
        QImage result = input.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&result);
        painter.drawImage(0, 0, overlay);
        painter.end();
        *output = result;
        return true;
    }

    if (data->entry.kind == SourceKind::VfxGenerator) {
        VfxGeneratorParameters parameters = VfxGenerators::defaultParameters(
            data->vfxType);
        const QVariantMap overrides = m_parameterOverrides.value(data->entry.id);
        for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
            VfxGenerators::setParameter(parameters, it.key(), it.value());

        if (data->vfxType == VfxGeneratorType::ShockWave) {
            const auto *shockWave = std::get_if<ShockWaveParameters>(&parameters);
            if (!shockWave)
                return false;
            const double duration = VfxGenerators::durationSeconds(
                data->vfxType, parameters);
            const double sampleTime = duration > 0.0
                ? qMin(duration * 0.35, 0.35) : 0.0;
            *output = VfxGenerators::applyShockWave(input, sampleTime, *shockWave);
            return !output->isNull();
        }

        QImage overlay;
        if (!buildVfxOverlay(*data, input.size(), &overlay))
            return false;
        const QImage base = input.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        *output = LayerCompositor::blendImages(base, overlay,
                                                BlendMode::Screen, 1.0);
        return !output->isNull();
    }

    if (data->entry.kind == SourceKind::Preset) {
        const EffectPreset preset = PresetLibrary::instance().findByName(
            data->presetName);
        if (preset.name.isEmpty())
            return false;
        *output = VideoEffectProcessor::applyEffectStack(
            input, preset.colorCorrection, preset.effects);
        return !output->isNull();
    }

    VideoEffect effect;
    if (!buildVideoEffect(*data, &effect))
        return false;
    *output = VideoEffectProcessor::applyEffect(input, effect);
    return !output->isNull();
}

bool EffectLibraryModel::applyToClip(const QString &id, ClipInfo &clip) const
{
    const EntryData *data = dataForId(id);
    if (!data)
        return false;

    if (data->entry.kind == SourceKind::Footage) {
        const QVariantMap overrides = m_parameterOverrides.value(data->entry.id);
        clip.filePath = data->footagePath;
        if (clip.displayName.isEmpty())
            clip.displayName = data->entry.displayName;
        clip.isVfxFootage = true;
        clip.blendMode = CompositeLayer::blendModeFromName(
            overrides.value(QStringLiteral("blendMode"),
                            QStringLiteral("Screen")).toString());
        clip.vfxIntensity = qMax(0.0, overrides.value(
            QStringLiteral("vfxIntensity"), 1.0).toDouble());
        clip.vfxBlackLevel = qBound(0, overrides.value(
            QStringLiteral("vfxBlackLevel"), 16).toInt(), 64);
        if (clip.duration <= 0.0)
            clip.duration = vfxfootage::VfxFootageLibrary::probeDurationSeconds(
                clip.filePath);
        if (clip.outPoint <= 0.0 && clip.duration > 0.0)
            clip.outPoint = clip.duration;
        return !clip.filePath.isEmpty();
    }

    if (data->entry.kind == SourceKind::Preset)
        return PresetLibrary::instance().applyClipStackPreset(
            data->presetName, clip, true);

    VideoEffect effect;
    if (!buildVideoEffect(*data, &effect))
        return false;
    clip.effects.append(effect);
    return true;
}

bool EffectLibraryModel::previewClip(const QString &id, const ClipInfo &source,
                                     bool enabled, ClipInfo *output) const
{
    if (!output)
        return false;
    *output = source;
    if (!enabled)
        return true;
    return applyToClip(id, *output);
}

bool EffectLibraryModel::addKeyframeToClip(const QString &id,
                                           const QString &paramName,
                                           double timeSeconds,
                                           ClipInfo &clip) const
{
    const EntryData *data = dataForId(id);
    if (!data || data->entry.kind == SourceKind::Preset
        || data->entry.kind == SourceKind::Particle
        || data->entry.kind == SourceKind::VfxGenerator)
        return false;

    bool color = false;
    for (const ParameterSpec &spec : parametersForData(*data)) {
        if (spec.name == paramName) {
            color = spec.color;
            break;
        }
    }
    if (color)
        return false;

    VideoEffect effect;
    if (!buildVideoEffect(*data, &effect))
        return false;
    QString nativeParam = paramName;
    if (data->entry.kind == SourceKind::Shader)
        nativeParam = shaderNativeParam(data->sourceName, paramName);
    else if (data->entry.kind == SourceKind::Plugin)
        nativeParam = pluginNativeParam(data->sourceName, paramName);
    if (nativeParam.isEmpty())
        return false;
    int effectIndex = -1;
    for (int i = static_cast<int>(clip.effects.size()) - 1; i >= 0; --i) {
        if (clip.effects[i].type == effect.type) {
            effectIndex = i;
            break;
        }
    }
    if (effectIndex < 0) {
        clip.effects.append(effect);
        effectIndex = static_cast<int>(clip.effects.size()) - 1;
    }
    const double nativeValue = effectctrl::paramValue(
        clip.effects[effectIndex], nativeParam);
    const QString trackName = QStringLiteral("effect.%1.%2")
        .arg(effectIndex).arg(nativeParam);
    KeyframeTrack *track = clip.keyframes.track(trackName);
    if (!track) {
        KeyframeTrack created(trackName, nativeValue);
        created.addKeyframe(timeSeconds, nativeValue);
        clip.keyframes.addTrack(created);
    } else {
        track->addKeyframe(timeSeconds, nativeValue);
    }
    return true;
}

QImage EffectLibraryModel::testPattern(const QSize &size)
{
    const QSize safeSize(qMax(1, size.width()), qMax(1, size.height()));
    QImage image(safeSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(18, 22, 30));

    QPainter painter(&image);
    QLinearGradient gradient(0.0, 0.0, safeSize.width(), safeSize.height());
    gradient.setColorAt(0.0, QColor(22, 92, 140));
    gradient.setColorAt(0.5, QColor(122, 44, 113));
    gradient.setColorAt(1.0, QColor(226, 144, 42));
    painter.fillRect(image.rect(), gradient);

    const int barWidth = qMax(1, safeSize.width() / 8);
    const QColor bars[] = {
        QColor(255, 255, 255), QColor(255, 255, 0), QColor(0, 255, 255),
        QColor(0, 255, 0), QColor(255, 0, 255), QColor(255, 0, 0),
        QColor(0, 0, 255), QColor(20, 20, 20)
    };
    for (int i = 0; i < 8; ++i)
        painter.fillRect(i * barWidth, 0, barWidth, qMax(1, safeSize.height() / 5), bars[i]);

    painter.setPen(QPen(QColor(255, 255, 255, 180), qMax(1, safeSize.width() / 160)));
    painter.drawEllipse(QRectF(safeSize.width() * 0.33,
                               safeSize.height() * 0.26,
                               safeSize.width() * 0.34,
                               safeSize.height() * 0.48));
    painter.end();
    return image;
}

QImage EffectLibraryModel::thumbnail(const QString &id, const QImage &source,
                                     const QSize &size) const
{
    const QSize safeSize(qMax(1, size.width()), qMax(1, size.height()));
    const EntryData *data = dataForId(id);
    if (data && data->entry.kind == SourceKind::Footage) {
        QImage frame = footageFrame(*data);
        if (!frame.isNull()) {
            const QVariantMap overrides = m_parameterOverrides.value(id);
            frame = vfxfootage::VfxFootageLibrary::applyBlackLevel(
                frame, overrides.value(QStringLiteral("vfxBlackLevel"), 16).toInt());
            frame = vfxfootage::VfxFootageLibrary::applyIntensity(
                frame, overrides.value(QStringLiteral("vfxIntensity"), 1.0).toDouble());
            return frame.scaled(safeSize, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
        }
    }
    const QImage base = source.isNull() ? testPattern(QSize(320, 180)) : source;
    QImage processed;
    if (!applyToImage(id, base, &processed))
        processed = base;
    if (processed.isNull())
        processed = testPattern(safeSize);
    return processed.scaled(safeSize, Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation);
}

QString EffectLibraryModel::footageFilePath(const QString &id) const
{
    const EntryData *data = dataForId(id);
    return data && data->entry.kind == SourceKind::Footage
        ? data->footagePath : QString();
}

QVariant EffectLibraryModel::parameterOverride(const QString &id,
                                               const QString &name) const
{
    return m_parameterOverrides.value(id).value(name);
}

QImage EffectLibraryModel::footageFrame(const EntryData &data) const
{
    if (data.entry.kind != SourceKind::Footage || data.footagePath.isEmpty())
        return {};
    const auto it = m_footageFrameCache.constFind(data.entry.id);
    if (it != m_footageFrameCache.constEnd())
        return it.value();
    const QImage frame = vfxfootage::VfxFootageLibrary::representativeFrame(
        data.footagePath);
    m_footageFrameCache.insert(data.entry.id, frame);
    return frame;
}

bool EffectLibraryModel::saveUserPreset(const QString &name,
                                        const ClipInfo &clip,
                                        bool includeKeyframes,
                                        QString *savedPath)
{
    if (!PresetLibrary::instance().saveClipStackPreset(
            name, clip, includeKeyframes, savedPath)) {
        return false;
    }
    registerAll();
    return saveState();
}

bool EffectLibraryModel::removeUserPreset(const QString &id)
{
    const EntryData *data = dataForId(id);
    if (!data || !data->entry.isUserPreset || data->presetName.isEmpty())
        return false;
    if (!PresetLibrary::instance().removePreset(data->presetName))
        return false;
    m_favoriteIds.remove(id);
    registerAll();
    return saveState();
}

bool EffectLibraryModel::renameUserPreset(const QString &id,
                                          const QString &newName)
{
    const EntryData *data = dataForId(id);
    if (!data || !data->entry.isUserPreset || data->presetName.isEmpty())
        return false;
    EffectPreset preset;
    if (!PresetLibrary::instance().loadClipStackPreset(data->presetName, &preset))
        return false;
    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty())
        return false;
    preset.name = trimmed;
    preset.isBuiltIn = false;
    if (!PresetLibrary::instance().updatePreset(data->presetName, preset))
        return false;

    const bool wasFavorite = m_favoriteIds.contains(id);
    m_favoriteIds.remove(id);
    if (wasFavorite)
        m_favoriteIds.insert(encodedId(QStringLiteral("preset"), trimmed));
    registerAll();
    return saveState();
}

} // namespace efxlib
