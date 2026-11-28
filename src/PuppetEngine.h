#pragma once

#include "WarpDistortion.h"

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QVector>

#include <array>
#include <functional>

class Timeline;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;

namespace puppet {

enum class PinType {
    Position = 0, // Moves mesh organically with ARAP
    Bend     = 1, // Anchors position, drives local rotation & scale
    Advanced = 2, // Drives position + rotation + scale simultaneously
    Starch   = 3, // Increases rigidity weight of surrounding triangles
    Overlap  = 4, // Controls front/back Z-depth ordering when parts overlap
};

enum class EasingKind {
    Linear    = 0,
    EaseInOut = 1,
    EaseIn    = 2,
    EaseOut   = 3,
    Hold      = 4,
};

struct PinKeyframe {
    double timeSec     = 0.0; // Clip-local seconds
    QPointF position   = QPointF(0.5, 0.5); // Normalized UV [0..1]
    double rotationDeg = 0.0; // Degrees (Bend / Advanced)
    double scale       = 1.0; // Uniform scale [0.1..4.0] (Bend / Advanced)
    double inFront     = 0.0; // Z depth [-100..+100] (Overlap)
    EasingKind easing  = EasingKind::EaseInOut;

    bool operator==(const PinKeyframe &o) const;
    QJsonObject toJson() const;
    static PinKeyframe fromJson(const QJsonObject &obj);
};

struct EvaluatedPin {
    int id             = 0;
    PinType type       = PinType::Position;
    QPointF restPos    = QPointF(0.5, 0.5);
    QPointF position   = QPointF(0.5, 0.5);
    double rotationDeg = 0.0;
    double scale       = 1.0;
    double inFront     = 0.0;
    double radius      = 0.25;
    double stiffness   = 0.5;
};

struct PinDynamics {
    bool wiggleEnabled     = false;
    double wiggleFreqHz    = 2.0;   // [0.05..20.0] Hz
    double wiggleAmpUv     = 0.025; // [0.0..0.5] UV amplitude
    double wiggleRotAmpDeg = 0.0;   // [0.0..90.0] degrees (Bend / Advanced)
    int wiggleOctaves      = 2;     // [1..6]
    unsigned int wiggleSeed = 1;
    QString exprX;                  // Expression string (time, value -> UV x)
    QString exprY;                  // Expression string (time, value -> UV y)
    QString exprRot;                // Expression string (time, value -> degrees)
    QString exprScale;              // Expression string (time, value -> scale)

    bool isDefault() const;
    bool operator==(const PinDynamics &o) const;
    QJsonObject toJson() const;
    static PinDynamics fromJson(const QJsonObject &obj);
};

struct SketchSample {
    double timeSec = 0.0;
    QPointF posUv  = QPointF(0.5, 0.5);
};

struct PuppetPin {
    int id           = 0;
    QString name;
    PinType type     = PinType::Position;
    QPointF restPos  = QPointF(0.5, 0.5); // Normalized UV [0..1]
    double radius    = 0.25;              // Normalized to min(w, h)
    double stiffness = 0.5;               // [0..1] (Starch rigidity multiplier)
    QVector<PinKeyframe> keyframes;       // Sorted by timeSec
    PinDynamics dynamics;                 // Wave 32: Wiggle + Expression automation

    bool operator==(const PuppetPin &o) const;
    QJsonObject toJson() const;
    static PuppetPin fromJson(const QJsonObject &obj);
};

struct MeshSettings {
    int density        = 14;   // Grid divisions along longest axis [6..36]
    int expansionPx    = 6;    // Alpha silhouette dilation in pixels [0..48]
    int alphaThreshold = 8;    // Alpha cutoff [1..254]
    bool showMesh      = true; // Wireframe overlay in PuppetPinTool
    bool autoKey       = true; // Auto-insert keyframe at playhead when dragging

    bool operator==(const MeshSettings &o) const;
    QJsonObject toJson() const;
    static MeshSettings fromJson(const QJsonObject &obj);
};

struct PuppetRig {
    bool enabled   = true;
    int nextPinId  = 1;
    MeshSettings mesh;
    QVector<PuppetPin> pins;

    bool isEmpty() const { return pins.isEmpty(); }
    bool isActive() const { return enabled && !pins.isEmpty(); }
    bool operator==(const PuppetRig &o) const;
    QJsonObject toJson() const;
    static PuppetRig fromJson(const QJsonObject &obj);
};

// Triangulated rest/deformed mesh in pixel coordinates.
struct PuppetMesh {
    int width  = 0;
    int height = 0;
    bool alphaSilhouette = false; // true = transparent PNG contour mesh; false = full-frame grid
    QVector<QPointF> restVertices;
    QVector<std::array<int, 3>> triangles;
    QVector<double> triangleStiffness; // Per-triangle ARAP weight (boosted by Starch pins)
    QVector<double> triangleDepth;     // Per-triangle Z depth (driven by Overlap pins)
};

struct DeformedMesh {
    PuppetMesh base;
    QVector<QPointF> deformedVertices;
};

// --- Pin & keyframe helpers ---
QString pinTypeName(PinType type);
QString pinTypeDisplayName(PinType type);
PinType pinTypeFromString(const QString &s);
QString easingName(EasingKind kind);
EasingKind easingFromString(const QString &s);

EvaluatedPin evaluatePinAt(const PuppetPin &pin, double clipLocalSec);
QVector<EvaluatedPin> evaluateRigAt(const PuppetRig &rig, double clipLocalSec);

// Insert or update a keyframe at clipLocalSec (merges within 1e-3 s).
void upsertPinKeyframe(PuppetPin &pin, const PinKeyframe &kf);
bool removePinKeyframeNear(PuppetPin &pin, double clipLocalSec, double epsSec = 0.02);

// Wave 32: Puppet Sketch (real-time drag trajectory simplification via Ramer-Douglas-Peucker).
QVector<SketchSample> simplifySketchRdp(const QVector<SketchSample> &samples,
                                        double epsilonUv = 0.004,
                                        double minDtSec = 1.0 / 30.0);
int applySketchToPin(PuppetPin &pin,
                     const QVector<SketchSample> &rawSamples,
                     double epsilonUv = 0.004,
                     EasingKind easing = EasingKind::Linear,
                     bool replaceInRange = true);

// Wave 32: Built-in Sway / Breathing presets for quick character secondary motion.
enum class SwayPreset {
    None = 0,
    HairRibbon = 1, // Gentle organic wiggle
    Breathing  = 2, // Smooth sinusoidal vertical breathing expression
    WindGust   = 3, // Stronger wiggle + bend rotation
};
void applySwayPreset(PuppetPin &pin, SwayPreset preset);

// Convert between legacy static WarpPin list and PuppetRig.
PuppetRig rigFromLegacyPins(const QVector<WarpPin> &legacy);
QVector<WarpPin> toLegacyPreviewPins(const PuppetRig &rig, double clipLocalSec = 0.0);

// --- Mesh generation, 2-step ARAP solver & Z-sorted rasterizer ---
PuppetMesh buildMeshForImage(const QImage &image, const MeshSettings &settings);
DeformedMesh solveArapMesh(const PuppetMesh &mesh, const QVector<EvaluatedPin> &pins);
QImage rasterizeDeformedMesh(const QImage &source, const DeformedMesh &deformed);

// Full SSOT render entry point (cached mesh + 2-step ARAP + premultiplied Z-sorted rasterize).
QImage renderPuppetFrame(const QImage &source, const PuppetRig &rig, double clipLocalSec);

// --- Process-wide per-clip-UID RigStore + Timeline Undo bridge ---
bool hasRig(const QString &clipUid);
PuppetRig rigForClip(const QString &clipUid);
void setRigForClip(const QString &clipUid, const PuppetRig &rig);
void removeRigForClip(const QString &clipUid);
void clearAllRigs();

// Snapshot/restore all rigs for Timeline Undo/Redo alignment.
QHash<QString, PuppetRig> snapshotAllRigs();
void restoreAllRigs(const QHash<QString, PuppetRig> &snap);

// Timeline-indexed undo history helpers called from Timeline::saveUndoState / undo / redo.
void recordTimelineUndoSnapshot(const void *timelineKey, int undoIndex);
void restoreTimelineUndoSnapshot(const void *timelineKey, int undoIndex);
void clearTimelineUndoSnapshots(const void *timelineKey);

// Commit or preview a rig edit onto a Timeline video clip (auto-assigns clip.uid if empty,
// keeps legacy clip.puppetPins non-empty so clip.hasPuppetPins() stays accurate, and records 1 Undo).
bool commitRigToTimeline(Timeline *timeline,
                         int trackIdx,
                         int clipIdx,
                         const PuppetRig &rig,
                         const QString &undoLabel = QStringLiteral("パペットアニメを編集"));
void previewRigOnTimeline(Timeline *timeline,
                          int trackIdx,
                          int clipIdx,
                          const PuppetRig &rig,
                          double clipLocalSec);

// --- Interactive Rig Configuration Dialog ---
class PuppetRigDialog : public QDialog {
    Q_OBJECT
public:
    explicit PuppetRigDialog(Timeline *timeline,
                             int trackIdx,
                             int clipIdx,
                             double initialClipLocalSec,
                             std::function<void()> onPreviewRefresh = {},
                             QWidget *parent = nullptr);

    PinType selectedToolMode() const;

private:
    void reloadUi();
    void loadSelectedPinToEditor();
    void applyEditorToSelectedPin();
    void commitRig(const QString &undoLabel);

    Timeline *m_timeline = nullptr;
    int m_trackIdx = -1;
    int m_clipIdx = -1;
    double m_clipLocalSec = 0.0;
    std::function<void()> m_onPreviewRefresh;
    PuppetRig m_rig;
    bool m_updating = false;

    QComboBox *m_toolModeCombo = nullptr;
    QSpinBox *m_densitySpin = nullptr;
    QSpinBox *m_expansionSpin = nullptr;
    QSpinBox *m_alphaThreshSpin = nullptr;
    QCheckBox *m_showMeshCheck = nullptr;
    QCheckBox *m_autoKeyCheck = nullptr;
    QListWidget *m_pinList = nullptr;
    QLabel *m_timeLabel = nullptr;

    QComboBox *m_pinTypeCombo = nullptr;
    QDoubleSpinBox *m_posXSpin = nullptr;
    QDoubleSpinBox *m_posYSpin = nullptr;
    QDoubleSpinBox *m_rotSpin = nullptr;
    QDoubleSpinBox *m_scaleSpin = nullptr;
    QDoubleSpinBox *m_radiusSpin = nullptr;
    QDoubleSpinBox *m_stiffnessSpin = nullptr;
    QDoubleSpinBox *m_inFrontSpin = nullptr;
    QComboBox *m_easingCombo = nullptr;

    // Wave 32: Wiggle / Expression automation controls
    QCheckBox *m_wiggleCheck = nullptr;
    QDoubleSpinBox *m_wiggleFreqSpin = nullptr;
    QDoubleSpinBox *m_wiggleAmpSpin = nullptr;
    QDoubleSpinBox *m_wiggleRotSpin = nullptr;
    QSpinBox *m_wiggleSeedSpin = nullptr;
    QLineEdit *m_exprXEdit = nullptr;
    QLineEdit *m_exprYEdit = nullptr;
    QLineEdit *m_exprRotEdit = nullptr;
};

} // namespace puppet
