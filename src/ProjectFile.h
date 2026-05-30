#pragma once

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QImage>
#include "ProjectSettings.h"
#include "Timeline.h"
#include "VideoEffect.h"
#include "Keyframe.h"
#include "Overlay.h"
#include "MaskSystem.h"
#include "PlanarTracker.h"
#include "ParticleSystem.h"
#include "Rotoscope.h"
#include "TimeRemap.h"
#include "ClipExpressionBindings.h"
#include "WiggleTransform.h"
#include "AudioDucking.h"
#include "ExportDialog.h"           // HDRSettings struct
#include "AIProcessingDialog.h"     // AIProcessingSettings struct
#include "MediaPool.h"              // PRD-PHASE1-MEDIA-POOL: mediapool::MediaPool
#include "AudioBusRouting.h"        // PRD-PHASE2-AUDIO-BUS: audiobus::AudioBusRouting
#include "AcesColor.h"              // PRD-PHASE3-ACES: aces::AcesPipeline

// --- Audio mixer serialization sub-types ---

struct TrackEqState {
    int trackIdx = -1;
    bool enabled = false;
    double low = 0.0;
    double mid = 0.0;
    double high = 0.0;
    double lowFreqHz = 200.0;
    double midFreqHz = 1000.0;
    double highFreqHz = 5000.0;
    double qFactor = 1.0;

    bool isDefault() const {
        return low == 0.0 && mid == 0.0 && high == 0.0
            && qFactor == 1.0 && lowFreqHz == 200.0
            && midFreqHz == 1000.0 && highFreqHz == 5000.0;
    }
};

struct CompressorState {
    double thresholdDb = -12.0;
    double ratio = 4.0;
    double attackMs = 10.0;
    double releaseMs = 120.0;
    double makeupDb = 0.0;
    bool enabled = false;
};

struct AutoDuckState {
    double thresholdDb = -20.0;
    double ratio = 4.0;
    double attackMs = 5.0;
    double releaseMs = 250.0;
};

// US-FEAT-A: overlay persistence — unified overlay item for project serialization
struct OverlayItem {
    int id = -1;
    QString type;               // "text", "image", "shape"
    QRectF bounds;              // normalized (0..1), center-based x/y + width/height
    double startTimeMs = 0.0;
    double durationMs = 0.0;
    QString color;              // #RRGGBBAA hex
    QString text;
    double opacity = 1.0;
    int trackIdx = 0;
};

// US-BRUSH-5: brush animation persistence entry
struct BrushAnimationEntry {
    QString clipId;           // "trackIndex:clipIndex" reference to owning clip
    QJsonObject brushData;    // serialized BrushAnimation via toJson()
};

// US-AETEXT-12: AE text feature persistence entries
struct PathTextEntry {
    QJsonObject data;
};

struct Text3DLayerEntry {
    QJsonObject data;
};

struct TextMaskRevealEntry {
    QJsonObject data;
};

struct TextPathWarpEntry {
    QJsonObject data;
};

struct VariableFontAxisEntry {
    QJsonObject data;
};

struct MographTextEntry {
    QJsonObject data;
};

// US-SNS-6: subtitle segment persistence for burn-in export
struct SubtitleEntry {
    double start = 0.0;
    double end = 0.0;
    QString text;
};

struct ParticleClipEntry {
    int trackIndex = -1;
    int clipIndex = -1;
    QString clipFilePath;
    ParticleEmitterConfig config;
};

struct ClipNodeGraph {
    QString clipId;
    QJsonObject graph;
    QString compositingMode = QStringLiteral("layer");
};

struct RotoClipEntry {
    QString clipId;
    RotoPath path;
    QVector<RotoKeyframe> keyframes;
    QImage brushMask;
};

struct TimeRemapClipEntry {
    QString clipId;
    timeremap::TimeRemapCurve curve;
};

struct TrackMatteClipEntry {
    QString clipId;
    TrackMatteType matteType = TrackMatteType::None;
    QString matteSourceClipId;
};

// US-3D-11: per-clip 3D extruded-text layer config. `config` is a
// Text3DLayer::toJson() blob (Text3DLayer is a non-copyable QObject so we
// stash the serialized form rather than the object).
struct Text3DClipEntry {
    QString clipId;
    QJsonObject config;
};

// US-3D-11: per-clip expression bindings (transform.* etc).
struct ExpressionBindingsClipEntry {
    QString clipId;
    exprbind::ClipExpressionBindings bindings;
};

// US-3D-11: per-clip wiggle / handheld-camera transform jitter.
struct WiggleClipEntry {
    QString clipId;
    wiggle::WiggleParams params;
};

struct ProjectGlowState {
    bool enabled = false;
    float threshold = 0.5f;
    float radius = 10.0f;
    float intensity = 1.0f;
};

struct ProjectBloomState {
    bool enabled = false;
    float threshold = 0.5f;
    float intensity = 1.0f;
    float spread = 0.3f;
};

struct ProjectChromaticAberrationState {
    bool enabled = false;
    float amount = 5.0f;
    float radialFalloff = 2.0f;
};

struct ProjectLightWrapState {
    bool enabled = false;
    float amount = 0.5f;
    float radius = 10.0f;
};

struct ProjectVfxState {
    ProjectGlowState glow;
    ProjectBloomState bloom;
    ProjectChromaticAberrationState chromaticAberration;
    ProjectLightWrapState lightWrap;
};

// PRD-PROJECT-PRESET: tracker preset persistence — value-only structs (no TrackerPreset.h / PlanarTrackerPreset.h dependency)
struct MotionTrackerProjectState {
    QString lastPresetId;
    int searchRadius = 16;
    QString matchMetric = "NCC";
    bool kalmanEnabled = false;
    double kalmanProcessNoise = 0.01;
    double kalmanMeasurementNoise = 0.1;
    double occlusionGate = 30.0;
    bool subPixelEnabled = true;
    double minConfidence = 0.5;
};

struct PlanarTrackerProjectState {
    QString lastPresetId;
    double searchRadiusPx = 16.0;
    double patchSizePx = 32.0;
    double dampingFactor = 0.3;
    int maxFramesPerCall = 0;
};

// Full project state for serialization
struct ProjectData {
    ProjectConfig config;
    QVector<QVector<ClipInfo>> videoTracks;
    QVector<QVector<ClipInfo>> audioTracks;
    double playheadPos = 0.0;
    double markIn = -1.0;
    double markOut = -1.0;
    int zoomLevel = 10;

    // Audio mixer state
    QVector<TrackEqState> trackEqStates;
    CompressorState masterCompressor;
    AutoDuckState autoDuck;
    bool audioMetersDockVisible = true;

    // US-FEAT-A: overlay persistence
    QList<OverlayItem> overlays;

    // US-MOCHA-2: planar track persistence
    QVector<planartrack::PlanarTrack> planarTracks;

    // US-BRUSH-5: brush animation persistence
    QVector<BrushAnimationEntry> brushAnimations;

    // US-AETEXT-12: AE text feature persistence
    QVector<PathTextEntry> pathTexts;
    QVector<Text3DLayerEntry> text3DLayers;
    QVector<TextMaskRevealEntry> textMaskReveals;
    QVector<TextPathWarpEntry> textPathWarps;
    QVector<VariableFontAxisEntry> variableFontAxes;
    QVector<MographTextEntry> mographTexts;

    // US-SNS-6: SmartReframe, subtitle burn-in, loudness normalization
    QJsonObject smartReframe;
    QVector<SubtitleEntry> subtitleSegments;
    QJsonObject subtitleStyle;
    QJsonObject loudnessSettings;
    QVector<ParticleClipEntry> particleClipEntries;
    QVector<ClipNodeGraph> clipNodeGraphs;
    QVector<RotoClipEntry> rotoClipEntries;
    QVector<TimeRemapClipEntry> timeRemapClipEntries;
    QVector<TrackMatteClipEntry> trackMatteClipEntries;
    ProjectVfxState vfxState;

    // US-3D-11: motion-graphics sprint persistence
    QVector<Text3DClipEntry> text3DClipEntries;
    QVector<ExpressionBindingsClipEntry> expressionBindingsEntries;
    QVector<WiggleClipEntry> wiggleClipEntries;
    QJsonObject projectCamera;   // Camera3D::toJson() — single per-project camera

    // US-HW-10: audio ducking project state (Sprint 9)
    DuckingParams duckingParams;
    bool duckingEnabled = false;

    // US-EXT-10: pro-extension project state (Sprint 10) — HDR output settings
    // (HDR10 / HLG metadata + preview tone-map) and AI processing settings
    // (upscale + frame interpolation). Persisted in .veditor JSON; missing keys
    // restore defaults to keep older project files loadable.
    HDRSettings          hdrSettings;
    AIProcessingSettings aiSettings;

    // US-INT-2: Sprint 16 — last-used mobile export device id (MobileDeviceProfile.id),
    // empty when never set. Persisted under root["mobileExport"]["lastDeviceId"].
    QString mobileExportLastDeviceId;
    // US-INT-2: Sprint 16 — last folder browsed in ImportHubDialog,
    // empty when never set. Persisted under root["lastImportFolder"].
    QString lastImportFolder;

    // US-INT-3: Sprint 17 — YouTube upload preferences. Persisted under
    // root["youtube"]. All keys optional; older project files default to "".
    QString youtubeLastChannelId;
    QString youtubeLastPrivacy;        // "private" / "unlisted" / "public"

    // US-INT-3: Sprint 18 — Collaboration session info. Persisted under root["collab"].
    QString collabCurrentUserId;
    QString collabCurrentDisplayName;

    // US-INT-3: Sprint 19 — Auto color match memory. Persisted under root["colormatch"].
    QString colorMatchLastReferenceClip;

    // PRD-PROJECT-PRESET: tracker preset persistence
    MotionTrackerProjectState motionTrackerState;
    PlanarTrackerProjectState planarTrackerState;

    // PRD-PHASE1-MEDIA-POOL: メディアプール永続化
    mediapool::MediaPool mediaPool;

    // PRD-PHASE2-AUDIO-BUS: バス/サブミックス/AUXセンド ルーティング永続化
    audiobus::AudioBusRouting audioBusRouting;

    // PRD-PHASE3-ACES: ACES カラーマネジメント設定永続化
    aces::AcesPipeline acesPipeline;
};

class ProjectFile
{
public:
    static bool save(const QString &filePath, const ProjectData &data);
    static bool load(const QString &filePath, ProjectData &data);

    static QString toJsonString(const ProjectData &data);
    static bool fromJsonString(const QString &json, ProjectData &data);

    static const QString fileFilter() { return "V Editor Project (*.veditor);;All Files (*)"; }

    // PRD-PROJECT-PRESET: tracker preset state serialization helpers (public for selftest access)
    static QJsonObject motionTrackerStateToJson(const MotionTrackerProjectState &s);
    static MotionTrackerProjectState motionTrackerStateFromJson(const QJsonObject &obj);
    static QJsonObject planarTrackerStateToJson(const PlanarTrackerProjectState &s);
    static PlanarTrackerProjectState planarTrackerStateFromJson(const QJsonObject &obj);

private:
    // Serialization helpers
    static QJsonObject configToJson(const ProjectConfig &config);
    static ProjectConfig configFromJson(const QJsonObject &obj);

    static QJsonObject clipToJson(const ClipInfo &clip);
    static ClipInfo clipFromJson(const QJsonObject &obj);

    static QJsonObject colorCorrectionToJson(const ColorCorrection &cc);
    static ColorCorrection colorCorrectionFromJson(const QJsonObject &obj);

    static QJsonObject effectToJson(const VideoEffect &effect);
    static VideoEffect effectFromJson(const QJsonObject &obj);

    static QJsonObject keyframeTrackToJson(const KeyframeTrack &track);
    static KeyframeTrack keyframeTrackFromJson(const QJsonObject &obj);

    static QJsonObject keyframeManagerToJson(const KeyframeManager &km);
    static KeyframeManager keyframeManagerFromJson(const QJsonObject &obj);

    static QJsonObject transitionToJson(const Transition &t);
    static Transition transitionFromJson(const QJsonObject &obj);

    static QJsonArray tracksToJson(const QVector<QVector<ClipInfo>> &tracks);
    static QVector<QVector<ClipInfo>> tracksFromJson(const QJsonArray &arr);

    // Audio mixer serialization
    static QJsonObject trackEqToJson(const TrackEqState &s);
    static TrackEqState trackEqFromJson(const QJsonObject &obj);
    static QJsonObject compressorToJson(const CompressorState &cs);
    static CompressorState compressorFromJson(const QJsonObject &obj);
    static QJsonObject autoDuckToJson(const AutoDuckState &ad);
    static AutoDuckState autoDuckFromJson(const QJsonObject &obj);

    // US-FEAT-A: overlay persistence
    static QJsonObject overlayToJson(const OverlayItem &o);
    static OverlayItem overlayFromJson(const QJsonObject &obj);

    // US-MOCHA-2: planar track persistence
    static QJsonObject planarTrackToJson(const planartrack::PlanarTrack &t);
    static planartrack::PlanarTrack planarTrackFromJson(const QJsonObject &obj);

    static QJsonObject forceFieldToJson(const ForceField &field);
    static ForceField forceFieldFromJson(const QJsonObject &obj);
    static QJsonObject particleEmitterConfigToJson(const ParticleEmitterConfig &config);
    static ParticleEmitterConfig particleEmitterConfigFromJson(const QJsonObject &obj);
    static QJsonObject particleClipEntryToJson(const ParticleClipEntry &entry);
    static ParticleClipEntry particleClipEntryFromJson(const QJsonObject &obj);
    static QJsonObject clipNodeGraphToJson(const ClipNodeGraph &entry);
    static ClipNodeGraph clipNodeGraphFromJson(const QJsonObject &obj);
    static QJsonObject rotoClipEntryToJson(const RotoClipEntry &entry);
    static RotoClipEntry rotoClipEntryFromJson(const QJsonObject &obj);
    static QJsonObject timeRemapClipEntryToJson(const TimeRemapClipEntry &entry);
    static TimeRemapClipEntry timeRemapClipEntryFromJson(const QJsonObject &obj);
    static QJsonObject trackMatteClipEntryToJson(const TrackMatteClipEntry &entry);
    static TrackMatteClipEntry trackMatteClipEntryFromJson(const QJsonObject &obj);
    static QJsonObject vfxStateToJson(const ProjectVfxState &state);
    static ProjectVfxState vfxStateFromJson(const QJsonObject &obj);

    // US-3D-11: motion-graphics sprint persistence helpers
    static QJsonObject text3DClipEntryToJson(const Text3DClipEntry &entry);
    static Text3DClipEntry text3DClipEntryFromJson(const QJsonObject &obj);
    static QJsonObject expressionBindingsEntryToJson(const ExpressionBindingsClipEntry &entry);
    static ExpressionBindingsClipEntry expressionBindingsEntryFromJson(const QJsonObject &obj);
    static QJsonObject wiggleClipEntryToJson(const WiggleClipEntry &entry);
    static WiggleClipEntry wiggleClipEntryFromJson(const QJsonObject &obj);

    // US-AETEXT-12: AE text feature persistence
    static QJsonObject pathTextToJson(const PathTextEntry &e);
    static PathTextEntry pathTextFromJson(const QJsonObject &obj);
    static QJsonObject text3DLayerToJson(const Text3DLayerEntry &e);
    static Text3DLayerEntry text3DLayerFromJson(const QJsonObject &obj);
    static QJsonObject textMaskRevealToJson(const TextMaskRevealEntry &e);
    static TextMaskRevealEntry textMaskRevealFromJson(const QJsonObject &obj);
    static QJsonObject textPathWarpToJson(const TextPathWarpEntry &e);
    static TextPathWarpEntry textPathWarpFromJson(const QJsonObject &obj);
    static QJsonObject variableFontAxisToJson(const VariableFontAxisEntry &e);
    static VariableFontAxisEntry variableFontAxisFromJson(const QJsonObject &obj);
    static QJsonObject mographTextToJson(const MographTextEntry &e);
    static MographTextEntry mographTextFromJson(const QJsonObject &obj);
};
