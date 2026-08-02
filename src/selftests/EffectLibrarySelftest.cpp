// Effect Library SSOT selftest.

#include "../EffectLibraryModel.h"
#include "../EffectParamSchema.h"
#include "../EffectPlugin.h"
#include "../EffectPreset.h"
#include "../ShaderEffect.h"
#include "../Timeline.h"
#include "../VfxGenerators.h"

#include <QByteArray>
#include <QImage>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>
#include <cstring>

namespace {

bool sameEffect(const VideoEffect &a, const VideoEffect &b)
{
    return a.type == b.type
        && a.enabled == b.enabled
        && a.param1 == b.param1
        && a.param2 == b.param2
        && a.param3 == b.param3
        && a.keyColor == b.keyColor
        && a.startSec == b.startSec
        && a.endSec == b.endSec;
}

bool sameEffects(const QVector<VideoEffect> &a, const QVector<VideoEffect> &b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < static_cast<int>(a.size()); ++i) {
        if (!sameEffect(a[i], b[i]))
            return false;
    }
    return true;
}

bool sameImage(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()
        || a.sizeInBytes() != b.sizeInBytes()) {
        return false;
    }
    if (a.sizeInBytes() == 0)
        return true;
    return std::memcmp(a.constBits(), b.constBits(),
                       static_cast<size_t>(a.sizeInBytes())) == 0;
}

bool hasEntryWithTag(const QVector<efxlib::LibraryEntry> &entries,
                     efxlib::SourceKind kind, const QString &tag)
{
    for (const auto &entry : entries) {
        if (entry.kind == kind && entry.tags.contains(tag))
            return true;
    }
    return false;
}

bool hasUserPreset(const QVector<efxlib::LibraryEntry> &entries,
                   const QString &name, QString *idOut)
{
    for (const auto &entry : entries) {
        if (entry.kind == efxlib::SourceKind::Preset
            && entry.isUserPreset
            && entry.displayName == name) {
            if (idOut)
                *idOut = entry.id;
            return true;
        }
    }
    return false;
}

void restoreEnv(const char *name, bool wasSet, const QByteArray &oldValue)
{
    if (wasSet)
        qputenv(name, oldValue);
    else
        qunsetenv(name);
}

} // namespace

int runEffectLibrarySelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[effect-library] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL", gate, name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        if (ok)
            ++passed;
        else
            ++failed;
    };

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        check(0, "temporary state directory", false);
        return 1;
    }

    const bool oldStateSet = qEnvironmentVariableIsSet(
        "VEDITOR_EFFECT_LIBRARY_STATE");
    const QByteArray oldState = qgetenv("VEDITOR_EFFECT_LIBRARY_STATE");
    const bool oldPresetSet = qEnvironmentVariableIsSet(
        "VEDITOR_EFFECT_PRESET_DIR");
    const QByteArray oldPreset = qgetenv("VEDITOR_EFFECT_PRESET_DIR");
    qputenv("VEDITOR_EFFECT_LIBRARY_STATE",
            tempDir.filePath(QStringLiteral("effect-library.json")).toLocal8Bit());
    qputenv("VEDITOR_EFFECT_PRESET_DIR",
            tempDir.filePath(QStringLiteral("presets")).toLocal8Bit());

    efxlib::EffectLibraryModel model;
    const QVector<efxlib::LibraryEntry> all = model.entries();

    // G1: every source registry is compared by its own current contents.
    bool complete = true;
    const auto shaderEffects = ShaderEffectLibrary::instance().allEffects();
    for (const ShaderEffectDef &shader : shaderEffects)
        complete = complete && hasEntryWithTag(all, efxlib::SourceKind::Shader,
                                               shader.name);

    const int particleTypeCount = static_cast<int>(ParticleType::Custom);
    for (int i = 0; i < particleTypeCount; ++i) {
        complete = complete && hasEntryWithTag(
            all, efxlib::SourceKind::Particle,
            QStringLiteral("ParticleType:%1").arg(i));
    }

    const QVector<VfxGeneratorType> vfxTypes = VfxGenerators::allTypes();
    for (const VfxGeneratorType type : vfxTypes) {
        complete = complete && hasEntryWithTag(
            all, efxlib::SourceKind::VfxGenerator,
            QStringLiteral("VfxGeneratorType:%1").arg(static_cast<int>(type)));
    }

    for (const auto &plugin : PluginRegistry::instance().allPlugins()) {
        if (plugin)
            complete = complete && hasEntryWithTag(
                all, efxlib::SourceKind::Plugin, plugin->name());
    }

    for (const VideoEffectType type : VideoEffect::allTypes())
        complete = complete && hasEntryWithTag(
            all, efxlib::SourceKind::AeFx, VideoEffect::typeName(type));

    const auto presets = PresetLibrary::instance().allPresets();
    for (const EffectPreset &preset : presets) {
        if (!preset.name.isEmpty())
            complete = complete && hasEntryWithTag(
                all, efxlib::SourceKind::Preset, preset.name);
    }

    check(1, "automatic registry coverage", complete,
          QStringLiteral("entries=%1 shader=%2 particle=%3 vfx=%4 plugin=%5 aefx=%6 preset=%7")
              .arg(all.size())
              .arg(shaderEffects.size())
              .arg(particleTypeCount)
              .arg(vfxTypes.size())
              .arg(PluginRegistry::instance().allPlugins().size())
              .arg(VideoEffect::allTypes().size())
              .arg(presets.size()));

    // G2: IDs are persistence keys and must remain one-to-one.
    QSet<QString> ids;
    bool uniqueIds = true;
    for (const auto &entry : all) {
        if (entry.id.isEmpty() || ids.contains(entry.id)) {
            uniqueIds = false;
            break;
        }
        ids.insert(entry.id);
    }
    check(2, "unique persistent IDs", uniqueIds);

    // G3: both display-name and tag lookup work, while a missing term is empty.
    const efxlib::LibraryEntry first = all.isEmpty()
        ? efxlib::LibraryEntry{} : all.first();
    const QString nameQuery = first.displayName.left(
        qMax(1, qMin(3, static_cast<int>(first.displayName.size()))));
    const QString tagQuery = first.tags.isEmpty()
        ? QStringLiteral("shader") : first.tags.first();
    const bool searchOk = !first.id.isEmpty()
        && !model.search(nameQuery).isEmpty()
        && !model.search(tagQuery).isEmpty()
        && model.search(QStringLiteral("__effect-library-no-such-entry__")).isEmpty();
    check(3, "name and tag search", searchOk);

    // G4: categories are unique and account for every entry.
    const QStringList categoryList = model.categories();
    QSet<QString> categorySet;
    bool categoriesUnique = true;
    for (const QString &category : categoryList) {
        if (categorySet.contains(category)) {
            categoriesUnique = false;
            break;
        }
        categorySet.insert(category);
    }
    qsizetype categoryTotal = 0;
    for (const QString &category : categoryList)
        categoryTotal += model.byCategory(category).size();
    check(4, "category index", categoriesUnique
              && categoryTotal >= static_cast<qsizetype>(all.size()));

    // G5: favorites survive a fresh model instance through JSON state.
    const QString favoriteId = first.id;
    model.setFavorite(favoriteId, true);
    const bool savedState = model.saveState();
    efxlib::EffectLibraryModel restored;
    const bool loadedState = restored.loadState();
    efxlib::LibraryEntry restoredFavorite;
    const bool favoriteRoundTrip = restored.entryById(favoriteId, &restoredFavorite)
        && restoredFavorite.favorite;
    check(5, "favorite persistence", savedState && loadedState
              && favoriteRoundTrip);

    // G6: save a real clip stack, discover it through registerAll(), then
    // apply it and compare the ordered VideoEffect values.
    ClipInfo sourceClip;
    sourceClip.displayName = QStringLiteral("effect-library-source");
    sourceClip.duration = 5.0;
    sourceClip.colorCorrection.brightness = 0.25;
    sourceClip.colorCorrection.saturation = 0.82;
    sourceClip.effects.append(VideoEffect::createVignette(0.35, 0.75));
    sourceClip.effects.append(VideoEffect::createPosterize(6.0));
    const QString presetName = QStringLiteral("Effect Library Selftest Preset");
    QString presetPath;
    const bool presetSaved = model.saveUserPreset(
        presetName, sourceClip, false, &presetPath);
    QString presetId;
    const bool presetDiscovered = hasUserPreset(model.entries(), presetName, &presetId);
    ClipInfo appliedClip;
    const bool presetApplied = presetDiscovered
        && model.applyToClip(presetId, appliedClip);
    check(6, "user preset save/discover/apply round-trip",
          presetSaved && !presetPath.isEmpty() && presetDiscovered
              && presetApplied && sameEffects(appliedClip.effects,
                                             sourceClip.effects)
              && std::abs(appliedClip.colorCorrection.brightness
                          - sourceClip.colorCorrection.brightness) < 1e-9
              && std::abs(appliedClip.colorCorrection.saturation
                          - sourceClip.colorCorrection.saturation) < 1e-9);

    // G7: the library route for a plugin is exactly the plugin's public
    // process() route with its declared defaults.
    efxlib::LibraryEntry pluginEntry;
    bool hasPlugin = false;
    for (const auto &entry : model.entries()) {
        if (entry.kind == efxlib::SourceKind::Plugin) {
            pluginEntry = entry;
            hasPlugin = true;
            break;
        }
    }
    const QImage pattern = efxlib::EffectLibraryModel::testPattern(
        QSize(64, 36));
    QImage libraryImage;
    QImage directImage;
    bool applyMatchesDirect = false;
    if (hasPlugin) {
        const auto plugin = PluginRegistry::instance().findByName(pluginEntry.displayName);
        QVector<double> params;
        if (plugin) {
            for (const auto &param : plugin->parameterDefs())
                params.append(param.defaultValue);
            directImage = plugin->process(pattern, params);
            applyMatchesDirect = model.applyToImage(pluginEntry.id, pattern,
                                                     &libraryImage)
                && sameImage(libraryImage, directImage);
        }
    }
    check(7, "apply parity with existing public API", applyMatchesDirect);

    // G8: preview is a copy operation. ON may produce a candidate, but OFF
    // returns the unmodified source stack and never mutates the source.
    efxlib::LibraryEntry aeEntry;
    bool hasAe = false;
    for (const auto &entry : model.entries()) {
        if (entry.kind == efxlib::SourceKind::AeFx
            && entry.tags.contains(VideoEffect::typeName(VideoEffectType::Vignette))) {
            aeEntry = entry;
            hasAe = true;
            break;
        }
    }
    ClipInfo originalClip;
    originalClip.effects.append(VideoEffect::createBlur(2.0));
    ClipInfo previewClip;
    ClipInfo restoredClip;
    const bool previewOn = hasAe
        && model.previewClip(aeEntry.id, originalClip, true, &previewClip);
    const bool previewOff = hasAe
        && model.previewClip(aeEntry.id, originalClip, false, &restoredClip);
    check(8, "preview on/off leaves clip stack unchanged",
          previewOn && previewOff
              && sameEffects(originalClip.effects, restoredClip.effects)
              && originalClip.effects.size() == 1);

    // G9: lazy thumbnail work accepts both the built-in test pattern and the
    // smallest legal QImage without throwing or returning an invalid image.
    bool thumbnailsSafe = false;
    try {
        QString thumbnailId = first.id;
        for (const auto &entry : model.entries()) {
            if (entry.kind == efxlib::SourceKind::Particle) {
                thumbnailId = entry.id;
                break;
            }
        }
        const QImage emptyThumb = model.thumbnail(
            thumbnailId, QImage(), QSize(32, 18));
        const QImage onePixel(QSize(1, 1), QImage::Format_ARGB32_Premultiplied);
        const QImage tinyThumb = model.thumbnail(
            thumbnailId, onePixel, QSize(1, 1));
        thumbnailsSafe = !emptyThumb.isNull() && !tinyThumb.isNull()
            && emptyThumb.size() == QSize(32, 18)
            && tinyThumb.size() == QSize(1, 1);
    } catch (...) {
        thumbnailsSafe = false;
    }
    check(9, "thumbnail generation is exception-safe", thumbnailsSafe);

    // G10: an inspector value for a shader is translated to the existing
    // VideoEffect parameter name instead of being silently discarded.
    efxlib::LibraryEntry shaderEntry;
    bool hasMappedShader = false;
    for (const auto &entry : model.entries()) {
        if (entry.kind == efxlib::SourceKind::Shader
            && entry.tags.contains(QStringLiteral("Gaussian Blur"))) {
            shaderEntry = entry;
            hasMappedShader = true;
            break;
        }
    }
    bool shaderParameterApplied = false;
    if (hasMappedShader) {
        const auto specs = model.parameters(shaderEntry.id);
        if (!specs.isEmpty()) {
            const auto &spec = specs.first();
            const double delta = qMax(0.001, (spec.maxValue - spec.minValue) * 0.1);
            const double requested = qMin(spec.maxValue, spec.defaultValue + delta);
            shaderParameterApplied = model.setParameterOverride(
                shaderEntry.id, spec.name, requested);
            ClipInfo parameterClip;
            shaderParameterApplied = shaderParameterApplied
                && model.applyToClip(shaderEntry.id, parameterClip)
                && !parameterClip.effects.isEmpty()
                && std::abs(effectctrl::paramValue(
                    parameterClip.effects.last(), QStringLiteral("radius"))
                    - requested) < 1e-9
                && model.addKeyframeToClip(shaderEntry.id, spec.name,
                                           1.0, parameterClip)
                && parameterClip.effects.size() == 1
                && parameterClip.keyframes.track(
                    QStringLiteral("effect.0.radius")) != nullptr;
        }
    }
    check(10, "shader inspector parameter reaches clip effect",
          shaderParameterApplied);

    // G11: plugin parameter labels are also translated to the existing
    // VideoEffect field when the same catalog entry is applied to a clip.
    efxlib::LibraryEntry glowEntry;
    bool hasGlow = false;
    for (const auto &entry : model.entries()) {
        if (entry.kind == efxlib::SourceKind::Plugin
            && entry.displayName == QStringLiteral("Glow")) {
            glowEntry = entry;
            hasGlow = true;
            break;
        }
    }
    bool pluginParameterApplied = false;
    if (hasGlow) {
        const auto specs = model.parameters(glowEntry.id);
        if (!specs.isEmpty()) {
            const auto &spec = specs.first();
            const double requested = qMin(spec.maxValue,
                                          spec.defaultValue + 1.0);
            pluginParameterApplied = model.setParameterOverride(
                glowEntry.id, spec.name, requested);
            ClipInfo pluginClip;
            pluginParameterApplied = pluginParameterApplied
                && model.applyToClip(glowEntry.id, pluginClip)
                && !pluginClip.effects.isEmpty()
                && std::abs(effectctrl::paramValue(
                    pluginClip.effects.last(), QStringLiteral("radius"))
                    - requested) < 1e-9;
        }
    }
    check(11, "plugin inspector parameter reaches clip effect",
          pluginParameterApplied);

    // G12: the VFX generator registry is represented in the library as one
    // catalog entry per generator, including a usable thumbnail path.
    bool vfxCatalogReady = vfxTypes.size() == 7;
    for (const VfxGeneratorType type : vfxTypes) {
        const QString id = QStringLiteral("vfx:%1").arg(
            QString::fromLatin1(QUrl::toPercentEncoding(VfxGenerators::typeName(type))));
        efxlib::LibraryEntry entry;
        if (!model.entryById(id, &entry)
            || entry.kind != efxlib::SourceKind::VfxGenerator
            || model.thumbnail(id, QImage(), QSize(32, 18)).isNull()) {
            vfxCatalogReady = false;
            break;
        }
    }
    check(12, "VFX generator registry reaches library", vfxCatalogReady);

    // G13: a registered generator is an executable catalog entry, not only a
    // thumbnail label. This also covers the ShockWave source-image route.
    const QImage vfxSource = efxlib::EffectLibraryModel::testPattern(
        QSize(320, 180));
    bool vfxApplyReady = vfxTypes.size() == 7;
    for (const VfxGeneratorType type : vfxTypes) {
        const QString id = QStringLiteral("vfx:%1").arg(
            QString::fromLatin1(QUrl::toPercentEncoding(
                VfxGenerators::typeName(type))));
        QImage applied;
        vfxApplyReady = vfxApplyReady
            && model.applyToImage(id, vfxSource, &applied)
            && !applied.isNull()
            && !sameImage(applied, vfxSource);
    }
    check(13, "VFX library entries apply to source images", vfxApplyReady);

    restoreEnv("VEDITOR_EFFECT_LIBRARY_STATE", oldStateSet, oldState);
    restoreEnv("VEDITOR_EFFECT_PRESET_DIR", oldPresetSet, oldPreset);

    std::printf("[effect-library] RESULT passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
