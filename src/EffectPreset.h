#pragma once

#include "Keyframe.h"
#include "VideoEffect.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QImage>
#include <QDateTime>
#include <QPair>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>

// --- Effect Preset ---

struct ClipInfo;

struct EffectPreset {
    QString name;
    QString description;
    QString category;        // "Cinematic", "Black & White", "Vintage", etc.
    QString author;

    ColorCorrection colorCorrection;
    QVector<VideoEffect> effects;
    KeyframeManager keyframes; // effect.* tracks only when includesKeyframes is true

    QImage thumbnail;        // optional preview image

    QDateTime createdAt;
    QDateTime modifiedAt;
    bool isBuiltIn = false;  // true for factory presets
    bool includesKeyframes = false;

    QJsonObject toJson() const;
    static EffectPreset fromJson(const QJsonObject &obj);

    static EffectPreset fromClipStack(const QString &name,
                                      const ClipInfo &clip,
                                      bool includeKeyframes);
    void applyToClipStack(ClipInfo &clip, bool applyKeyframes = true) const;
};

// --- Preset Library (singleton) ---

class PresetLibrary
{
public:
    static PresetLibrary &instance();

    // Query
    QVector<EffectPreset> allPresets() const;
    QVector<EffectPreset> presetsByCategory(const QString &category) const;
    EffectPreset findByName(const QString &name) const;
    QStringList categories() const;

    // Mutate
    bool addPreset(const EffectPreset &preset);
    bool removePreset(const QString &name);
    bool updatePreset(const QString &name, const EffectPreset &preset);

    // Import / Export single preset JSON
    bool importPreset(const QString &filePath);
    bool exportPreset(const QString &name, const QString &filePath) const;

    // Named effect-stack JSON presets in the settings directory.
    bool saveClipStackPreset(const QString &name, const ClipInfo &clip,
                             bool includeKeyframes, QString *savedPath = nullptr);
    bool loadClipStackPreset(const QString &name, EffectPreset *preset) const;
    bool applyClipStackPreset(const QString &name, ClipInfo &clip,
                              bool applyKeyframes = true) const;

    static QString presetDirectory();
    static QString presetFilePath(const QString &name);

    // Persist entire library to ~/.veditor/presets.json
    bool saveLibrary() const;
    bool loadLibrary();

    // Apply — returns colour-correction + effects pair ready for the processor
    QPair<ColorCorrection, QVector<VideoEffect>> applyPreset(const QString &presetName) const;

    int count() const { return m_presets.size(); }

private:
    PresetLibrary();
    void registerBuiltins();

    static QString libraryPath();

    QVector<EffectPreset> m_presets;

    // Serialisation helpers — public static so EffectPreset::toJson/fromJson can use them
public:
    static QJsonObject colorCorrectionToJson(const ColorCorrection &cc);
    static ColorCorrection colorCorrectionFromJson(const QJsonObject &obj);
    static QJsonObject videoEffectToJson(const VideoEffect &ve);
    static VideoEffect  videoEffectFromJson(const QJsonObject &obj);
};
