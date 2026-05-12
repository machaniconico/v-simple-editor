#pragma once

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include "ProjectSettings.h"
#include "Timeline.h"
#include "VideoEffect.h"
#include "Keyframe.h"
#include "Overlay.h"
#include "PlanarTracker.h"

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
};

class ProjectFile
{
public:
    static bool save(const QString &filePath, const ProjectData &data);
    static bool load(const QString &filePath, ProjectData &data);

    static QString toJsonString(const ProjectData &data);
    static bool fromJsonString(const QString &json, ProjectData &data);

    static const QString fileFilter() { return "V Editor Project (*.veditor);;All Files (*)"; }

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
