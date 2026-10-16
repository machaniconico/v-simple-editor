#pragma once

#include <QDialog>
#include <QImage>
#include <QList>
#include <QMetaType>
#include <QPointF>
#include <QWidget>

#include "PlanarTracker.h"
#include "PlanarTrackerPreset.h"
#include "ProjectFile.h"
#include "CameraSolver.h"
#include "BundleAdjustment.h"
#include <functional>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QThread;

// ---------------------------------------------------------------------------
// PlanarCornerWidget — reference image display with 4 draggable corner pins
// ---------------------------------------------------------------------------
class PlanarCornerWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlanarCornerWidget(QWidget* parent = nullptr);

    void setReferenceImage(const QImage& image);
    void setCorners(const planar::CornerSet& corners);
    planar::CornerSet corners() const;

signals:
    void cornersChanged(const planar::CornerSet& corners);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    // Returns index 0..3 (tl/tr/br/bl) if widgetPx is within 12 px of a corner, else -1
    int     hitTest(const QPointF& widgetPx) const;
    QPointF imageToWidget(const QPointF& imgPt) const;
    QPointF widgetToImage(const QPointF& widgetPt) const;

    // Computes the image rect inside the widget (letterbox / aspect-preserving)
    QRectF  imageRect() const;

    QImage          m_image;
    planar::CornerSet m_corners;
    int             m_dragIndex = -1;
};

// ---------------------------------------------------------------------------
// PlanarTrackerDialog — UI dialog for planar (homography) corner tracking
// ---------------------------------------------------------------------------
class PlanarTrackerDialog : public QDialog {
    Q_OBJECT
public:
    explicit PlanarTrackerDialog(QWidget* parent = nullptr);

    ~PlanarTrackerDialog() override;
    // Source-media sample indices at fps (independent of timeline speed/trim).
    // Zero-based; end is inclusive.
    void setFeatureTrackingSource(const QString& path, double fps, int frameCount);
    void setFeatureClipTiming(double inPoint, double outPoint, double speed, bool reversed,
                              bool nonLinearTiming = false);
    void setFeatureTrackingResult(const sfm::MultiViewTracks& tracks, QSize size, int first);
    bool solveFeatureCamera();
    const sfm::CameraTrackSolve& featureSolveResult() const { return m_featureSolveResult; }
    const sfm::MultiViewTracks& featureTracks() const { return m_featureTracks; }
    QSize featureAnalysisSize() const { return m_featureAnalysisSize; }
    int featureStartFrame() const { return m_featureStartFrame; }
    double featureFps() const { return m_featureFps; }
    bool featureTrackingRunning() const { return m_featureWorker != nullptr; }
    int featureFrameCount() const { return m_featureFrameCount; }
    QThread* featureWorkerThread() const { return m_featureWorker; }
    QObject* featureWorkerContext() const { return m_featureWorkerContext; }
    // Test seam: the feature worker decodes through this function. The default
    // wraps libavcore::grabFrameAt, so production behavior is unchanged.
    static std::function<QImage(const QString&, double, const QSize&)>& featureGrabFunctionForTest();
    // Test seam: called on the GUI thread when a worker callback arrives after
    // the dialog was destroyed (the callback is dropped). Empty by default.
    static std::function<void()>& featureOrphanCallbackHookForTest();
    void setPlanarFrameLoader(std::function<QList<QImage>()> loader);

    // Set the reference frame (first frame / anchor image)
    void setReferenceFrame(const QImage& frame);

    // Supply the sequence of frames to track against
    void setFrames(const QList<QImage>& frames);

    // Corner pin accessors (image-coordinate space)
    planar::CornerSet currentCorners() const;
    void setCorners(const planar::CornerSet& corners);

    // Result after tracking
    QList<planar::Frame> trackResult() const;
    void setCameraSolveContext(QSize canvas, double fov, double fps, double startSec);

    // PRD-PROJECT-PRESET US-PP-4: project-state persistence hooks
    void setInitialState(const PlanarTrackerProjectState& s);
    PlanarTrackerProjectState currentState() const;

public slots:
    void accept() override;

signals:
    void featureTrackingFinished(bool success);
    void trackComputed(const QList<planar::Frame>& frames);
    void cameraSolveApplied(const QVector<camsolve::Pose>& poses, double fps, double startSec);
    void presetApplied(planar_tracker_preset::PlanarTrackerPreset);

private slots:
    void onPresetSelectionChanged(int index);
    void onSaveCustomPreset();
    void onDeleteSelectedPreset();
    void onResetPresetToDefaults();
    void onExportPreset();
    void onImportPreset();
    void onResetCorners();
    void onTrackClicked();
    void onCameraSolveClicked();
    void onFeatureTrackClicked();
    void onPatchSizeChanged(int value);
    void onSearchRadiusChanged(int value);
    void onDampingChanged(int value);

private:
    planar_tracker_preset::PlanarTrackerPreset selectedPreset() const;
    void rebuildPresetCombo(const QString& selectedId = QString());
    void applyPresetToWidgets(const planar_tracker_preset::PlanarTrackerPreset& preset);
    void setPresetWidgetSignalsBlocked(bool blocked);
    void updateDeletePresetButton();
    int currentPresetIndex() const;
    void rebuildSummary();

    std::function<QList<QImage>()> m_planarFrameLoader;
    sfm::CameraTrackSolve m_featureSolveResult;
    double m_featureIn = 0, m_featureOut = 0, m_featureSpeed = 1;
    bool m_featureReversed = false;
    // A speed ramp or a time remap makes source time a non-linear function of
    // timeline time, which the linear inverse below cannot express.
    bool m_featureNonLinearTiming = false;
    QPushButton* m_featureSolveButton = nullptr;
    QLabel* m_featureSolveSummary = nullptr;
    QString m_featurePath;
    double m_featureFps = 0;
    int m_featureFrameCount = 0;
    int m_featureStartFrame = 0;
    QSize m_featureAnalysisSize;
    sfm::MultiViewTracks m_featureTracks;
    QThread* m_featureWorker = nullptr;
    // Per-run GUI-thread object receiving the worker's queued callbacks. It is
    // not parented to the dialog, so it outlives it until the worker finishes.
    QObject* m_featureWorkerContext = nullptr;
    QSpinBox* m_featureStart = nullptr;
    QSpinBox* m_featureEnd = nullptr;
    QSpinBox* m_featureMaxPoints = nullptr;
    QPushButton* m_featureButton = nullptr;
    QLabel* m_featureSummary = nullptr;
    QProgressBar* m_featureProgress = nullptr;

    QImage               m_reference;
    QList<QImage>        m_frames;
    planar::CornerSet    m_corners;
    QList<planar::Frame> m_result;
    planar::TrackParams  m_params;
    QSize               m_cameraCanvas{1920, 1080};
    double              m_cameraFov = 60.0;
    double              m_cameraFps = 30.0;
    double              m_cameraStartSec = 0.0;
    QPushButton*        m_cameraSolveButton = nullptr;

    QLabel*             m_descriptionLabel       = nullptr;
    QComboBox*          m_presetCombo            = nullptr;
    QPushButton*        m_saveCustomPresetButton = nullptr;
    QPushButton*        m_deletePresetBtn        = nullptr;
    QPushButton*        m_resetPresetButton      = nullptr;
    QPushButton*        m_exportPresetButton     = nullptr;
    QPushButton*        m_importPresetButton     = nullptr;
    PlanarCornerWidget* m_cornerWidget      = nullptr;
    QSpinBox*           m_patchSizeSpin     = nullptr;  // px, 16..128
    QSpinBox*           m_searchRadiusSpin  = nullptr;  // px, 4..64
    QSpinBox*           m_dampingSpin       = nullptr;  // %, 0..100
    QPushButton*        m_resetButton       = nullptr;
    QPushButton*        m_trackButton       = nullptr;
    QProgressBar*       m_progress          = nullptr;
    QLabel*             m_summaryLabel      = nullptr;
    QDialogButtonBox*   m_buttonBox         = nullptr;

    QList<planar_tracker_preset::PlanarTrackerPreset> m_presets;
};

Q_DECLARE_METATYPE(planar_tracker_preset::PlanarTrackerPreset)
