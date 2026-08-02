#pragma once

#include "EffectPlugin.h"
#include "ParticleSystem.h"
#include "ShaderEffect.h"
#include "VfxGenerators.h"
#include "VideoEffect.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <memory>

struct ClipInfo;

namespace efxlib {

enum class SourceKind {
    Shader,
    Particle,
    VfxGenerator,
    Plugin,
    AeFx,
    Preset,
    Footage,
};

struct LibraryEntry {
    QString id;
    QString displayName;
    QString category;
    QStringList tags;
    SourceKind kind = SourceKind::AeFx;
    bool favorite = false;
    bool isUserPreset = false;
};

struct ParameterSpec {
    QString name;
    QString displayName;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    bool integer = false;
    bool color = false;
    QColor defaultColor;
};

// GUI-independent SSOT for the Effects Library. The panel only presents and
// routes these operations; registration, filtering, preview application,
// preset round-trips and keyframe insertion stay here for selftest coverage.
class EffectLibraryModel
{
public:
    EffectLibraryModel();

    void registerAll();
    QVector<LibraryEntry> entries() const;
    QVector<LibraryEntry> search(const QString &query) const;
    QVector<LibraryEntry> byCategory(const QString &category) const;
    QStringList categories() const;
    void setFavorite(const QString &id, bool on);
    QVector<LibraryEntry> favorites() const;
    bool saveState() const;
    bool loadState();

    bool entryById(const QString &id, LibraryEntry *entry) const;
    bool hasFootageEntries() const;
    QVector<ParameterSpec> parameters(const QString &id) const;
    bool setParameterOverride(const QString &id, const QString &name,
                              const QVariant &value);
    void clearParameterOverrides(const QString &id);

    // Apply the catalog entry through the existing CPU image APIs. The
    // shader source is represented by its existing CPU-equivalent VideoEffect
    // for thumbnail/headless preview; the GPU source remains in its registry.
    bool applyToImage(const QString &id, const QImage &input,
                      QImage *output) const;
    bool applyToClip(const QString &id, ClipInfo &clip) const;
    bool previewClip(const QString &id, const ClipInfo &source,
                     bool enabled, ClipInfo *output) const;
    bool addKeyframeToClip(const QString &id, const QString &paramName,
                           double timeSeconds, ClipInfo &clip) const;

    QImage thumbnail(const QString &id, const QImage &source,
                     const QSize &size = QSize(144, 81)) const;
    static QImage testPattern(const QSize &size = QSize(320, 180));

    // SourceKind::Footage entries only: absolute path of the scanned footage
    // file (empty for every other kind), and read access to one inspector
    // override (used by the one-click footage placement to seed clip params).
    QString footageFilePath(const QString &id) const;
    QVariant parameterOverride(const QString &id, const QString &name) const;

    bool saveUserPreset(const QString &name, const ClipInfo &clip,
                        bool includeKeyframes, QString *savedPath = nullptr);
    bool removeUserPreset(const QString &id);
    bool renameUserPreset(const QString &id, const QString &newName);

    static QString statePath();

private:
    struct EntryData {
        LibraryEntry entry;
        QString sourceName;
        ShaderEffectDef shader;
        ParticleEmitterConfig particle;
        VfxGeneratorType vfxType = VfxGeneratorType::Explosion;
        std::shared_ptr<EffectPlugin> plugin;
        VideoEffectType videoType = VideoEffectType::None;
        QString presetName;
        QString footagePath;    // SourceKind::Footage のみ。走査した素材ファイルの絶対パス
    };

    void addEntry(const EntryData &data);
    const EntryData *dataForId(const QString &id) const;
    EntryData *dataForId(const QString &id);
    void applyFavoriteIds();
    QVector<ParameterSpec> parametersForData(const EntryData &data) const;
    bool buildVideoEffect(const EntryData &data, VideoEffect *effect) const;
    bool buildPluginParams(const EntryData &data, QVector<double> *params) const;
    bool buildParticleOverlay(const EntryData &data, const QSize &size,
                              QImage *overlay) const;
    bool buildVfxOverlay(const EntryData &data, const QSize &size,
                         QImage *overlay) const;
    // SourceKind::Footage の代表フレームを遅延デコードしてキャッシュする。
    // 失敗 (null) もキャッシュし、サムネイル/プレビュー更新ごとの
    // デコード失敗リトライを防ぐ。registerAll() でクリアされる。
    QImage footageFrame(const EntryData &data) const;

    QVector<LibraryEntry> m_entries;
    QHash<QString, EntryData> m_data;
    QHash<QString, QVariantMap> m_parameterOverrides;
    QSet<QString> m_favoriteIds;
    mutable QHash<QString, QImage> m_footageFrameCache;
};

} // namespace efxlib
