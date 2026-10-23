#pragma once

#include "ShapeLayer.h"
#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;

// Edits shapes[0], the single shape created by the shape-clip insertion UI.
class ShapeModifierDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ShapeModifierDialog(const ShapeModifiers &modifiers, QWidget *parent = nullptr);
    ShapeModifiers modifiers() const;

signals:
    void modifiersChanged();

private:
    QCheckBox *m_repeater;
    QSpinBox *m_copies;
    QDoubleSpinBox *m_offsetX;
    QDoubleSpinBox *m_offsetY;
    QDoubleSpinBox *m_rotation;
    QDoubleSpinBox *m_scale;
    QDoubleSpinBox *m_opacity;
    QCheckBox *m_trim;
    QDoubleSpinBox *m_start;
    QDoubleSpinBox *m_end;
    QDoubleSpinBox *m_offset;
    QCheckBox *m_roundCorners;
    QDoubleSpinBox *m_radius;
    QCheckBox *m_zigZag;
    QDoubleSpinBox *m_zigSize;
    QSpinBox *m_ridges;
    QCheckBox *m_zigSmooth;
    QCheckBox *m_wiggle;
    QDoubleSpinBox *m_wiggleAmp;
    QSpinBox *m_wiggleDetail;
    QSpinBox *m_wiggleSeed;
};
