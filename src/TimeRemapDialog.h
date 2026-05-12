#pragma once

// ---------------------------------------------------------------------------
// TimeRemapDialog — time-remap curve editor + preview dialog
// namespace: (none — lives in the global namespace, uses timeremap:: types)
//
// Provides a QDialog with:
//   • A piecewise-linear curve editor (CurveEditorWidget, defined in .cpp)
//   • An editable table of (outTime, srcTime) keyframes
//   • Blend-mode combo + source-fps spinbox
//   • Preview slider → calls timeremap::resolveFrame and displays result
//   • OK / Cancel button box
// ---------------------------------------------------------------------------

#include <QDialog>
#include <QImage>
#include <functional>

#include "TimeRemap.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QTableWidget;
class QDialogButtonBox;

// Forward-declared; defined in TimeRemapDialog.cpp
class CurveEditorWidget;

class TimeRemapDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TimeRemapDialog(QWidget* parent = nullptr);

    // Set the curve to edit.  Populates table, graph, and controls.
    void setCurve(const timeremap::TimeRemapCurve& c);

    // Returns the current (edited) curve.
    timeremap::TimeRemapCurve curve() const;

    // Optional: supply a frame fetcher for the preview.
    // fetchFrame(srcFrameIndex) should return a decoded QImage or null QImage.
    void setFrameFetcher(std::function<QImage(int srcFrameIndex)> f);

    // Total source frame count used by the preview slider range.
    void setSourceFrameCount(int n);

signals:
    // Emitted whenever any key, blendMode, or sourceFps changes.
    void curveChanged();

private slots:
    void onTableCellChanged(int row, int col);
    void onAddRow();
    void onRemoveRow();
    void onBlendModeChanged(int index);
    void onSourceFpsChanged(double fps);
    void onPreviewSliderMoved(int value);
    void onCurveEditorChanged();   // fired by CurveEditorWidget

private:
    // -- helpers --
    void rebuildTable();           // copy m_curve.keys → QTableWidget
    void syncCurveFromTable();     // copy QTableWidget → m_curve.keys (sorted)
    void updatePreview();          // call resolveFrame and set pixmap
    void emitCurveChanged();
    void refreshAxisRange();       // recompute axis extents from keys + fps

    // -- state --
    timeremap::TimeRemapCurve               m_curve;
    std::function<QImage(int)>              m_fetcher;
    int                                     m_sourceFrameCount = 300;
    bool                                    m_blockSignals     = false;

    // -- widgets --
    CurveEditorWidget* m_curveEditor  = nullptr;
    QTableWidget*      m_table        = nullptr;
    QComboBox*         m_blendCombo   = nullptr;
    QDoubleSpinBox*    m_fpsSpin      = nullptr;
    QSlider*           m_previewSlider = nullptr;
    QLabel*            m_previewLabel  = nullptr;
    QDialogButtonBox*  m_buttonBox     = nullptr;
};
