#pragma once
// Sprint 22 — US-EASE-1: modeless easing-curve editor dialog.
#include <QDialog>
#include <QWidget>
#include "EasingCurveModel.h"

class QComboBox;
class QLabel;
class QMouseEvent;
class QPaintEvent;

// Interactive unit-square cubic-bezier curve widget. QPainter-draws the grid,
// the sampled curve and two draggable control-point handles.
class CurveWidget : public QWidget {
    Q_OBJECT
public:
    explicit CurveWidget(QWidget *parent = nullptr);

    easing::CubicBezier bezier() const { return m_bez; }
    void setBezier(const easing::CubicBezier &bez);

signals:
    void curveChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QPointF toWidget(double nx, double ny) const; // [0,1] -> screen (y inverted)
    QPointF toNormalized(const QPointF &p) const; // screen -> [0,1] (y inverted)

    easing::CubicBezier m_bez;
    int m_dragHandle = -1; // -1 none, 0 = (x1,y1), 1 = (x2,y2)
};

class EasingCurveEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit EasingCurveEditorDialog(QWidget *parent = nullptr);

    void setInitialCurve(double x1, double y1, double x2, double y2);
    void getCurve(double &x1, double &y1, double &x2, double &y2) const;

private slots:
    void onPresetChanged(int index);
    void onCurveEdited();

private:
    void updateValueLabel();

    CurveWidget *m_curve       = nullptr;
    QComboBox   *m_presetCombo = nullptr;
    QLabel      *m_valueLabel  = nullptr;
};
