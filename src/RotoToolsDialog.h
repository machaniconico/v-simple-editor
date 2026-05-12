#pragma once

// RotoToolsDialog — editing UI for auto-trace / shape-tracking / brush touch-up
// US-PRO-7
//
// Namespace: global (class is RotoToolsDialog).
// CMake wiring deferred — add src/RotoToolsDialog.cpp to CMakeLists.txt sources.
//
// Dependencies (all existing headers):
//   Rotoscope.h      (RotoPoint, RotoPath, RotoKeyframe)
//   RotoAutoTrace.h  (rototrace namespace)
//   RotoTracking.h   (rototrack namespace)
//   RotoBrushTool.h  (rotobrush::RotoBrushTool)

#include <QDialog>
#include <QImage>
#include <QVector>

#include "Rotoscope.h"

// Forward declarations — avoid pulling heavy headers into every TU that
// includes RotoToolsDialog.h.
class QPushButton;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QSlider;
class QRadioButton;
class QLabel;
class QDialogButtonBox;

namespace rotobrush { class RotoBrushTool; }

// Private canvas widget declared in the .cpp; forward-declare here so the
// member pointer can be declared without a full definition.
class RotoCanvasWidget;

// ============================================================

class RotoToolsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RotoToolsDialog(QWidget *parent = nullptr);
    ~RotoToolsDialog() override;

    // --- Frame / sequence ---

    /// Set the single current-frame image used for display and operations.
    void setFrame(const QImage &frame);

    /// Set the full frame sequence used by Track Shape.
    /// firstFrameIndex is the absolute project frame number of frames[0].
    void setFrameSequence(const QVector<QImage> &frames, int firstFrameIndex);

    // --- Roto path ---

    void setRotoPath(const RotoPath &path);
    RotoPath rotoPath() const;

    // --- Brush mask ---

    /// Returns the current brush mask (Format_Grayscale8, same size as current frame).
    /// Returns a null QImage if brush mode has never been entered.
    QImage brushMask() const;

    // --- Tracking results ---

    /// Keyframes produced by the last Track Shape invocation.
    QVector<RotoKeyframe> trackedKeyframes() const;

    // --- Canvas render (testing / verification) ---

    /// Returns the canvas as it would be painted (frame + bezier overlay [+ brush mask]).
    /// Returns a null QImage if no frame has been set.
    QImage canvasRender() const;

signals:
    void rotoEdited();

private slots:
    void onAutoTrace();
    void onTrackShape();
    void onBrushModeToggled(bool checked);
    void onBrushClear();
    void onBrushFill();
    void onBrushUndo();
    void onBrushRadiusChanged(int value);
    void onBrushHardnessChanged(int value);
    void onBrushOpacityChanged(int value);

private:
    // Ensure brush tool mask exists and matches the current frame size.
    void ensureBrushMask();

    // Canvas widget (owns its own paint + mouse logic; see .cpp).
    RotoCanvasWidget *m_canvas = nullptr;

    // --- Side-panel widgets ---

    // Auto-Trace
    QPushButton     *m_btnAutoTrace       = nullptr;
    QDoubleSpinBox  *m_spnBlur            = nullptr;
    QDoubleSpinBox  *m_spnEdgeThresh      = nullptr;
    QSpinBox        *m_spnMaxPoints       = nullptr;

    // Snap to edge
    QCheckBox       *m_chkSnapEdge        = nullptr;

    // Track Shape
    QPushButton     *m_btnTrackShape      = nullptr;
    QSpinBox        *m_spnKeyInterval     = nullptr;

    // Brush mode
    QCheckBox       *m_chkBrushMode       = nullptr;
    QSlider         *m_sldRadius          = nullptr;
    QSlider         *m_sldHardness        = nullptr;
    QSlider         *m_sldOpacity         = nullptr;
    QLabel          *m_lblRadius          = nullptr;
    QLabel          *m_lblHardness        = nullptr;
    QLabel          *m_lblOpacity         = nullptr;
    QRadioButton    *m_radAdd             = nullptr;
    QRadioButton    *m_radSubtract        = nullptr;
    QPushButton     *m_btnBrushClear      = nullptr;
    QPushButton     *m_btnBrushFill       = nullptr;
    QPushButton     *m_btnBrushUndo       = nullptr;

    QDialogButtonBox *m_buttonBox         = nullptr;

    // --- State ---

    QImage                  m_currentFrame;
    QVector<QImage>         m_frames;
    int                     m_firstFrameIndex = 0;
    RotoPath                m_rotoPath;
    QVector<RotoKeyframe>   m_trackedKeyframes;

    // Brush tool (allocated on demand).
    rotobrush::RotoBrushTool *m_brushTool = nullptr;
};
