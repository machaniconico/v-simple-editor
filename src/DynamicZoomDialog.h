#pragma once

#include "DynamicZoom.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;

class DynamicZoomDialog final : public QDialog
{
public:
    explicit DynamicZoomDialog(QWidget *parent = nullptr);

    dynzoom::Rect startRect() const;
    dynzoom::Rect endRect() const;
    dynzoom::Easing easing() const;

private:
    struct RectEditors {
        QDoubleSpinBox *cx = nullptr;
        QDoubleSpinBox *cy = nullptr;
        QDoubleSpinBox *w = nullptr;
        QDoubleSpinBox *h = nullptr;
    };

    static dynzoom::Rect readRect(const RectEditors& editors);
    static void writeRect(const RectEditors& editors, const dynzoom::Rect& rect);
    void applyPreset(int presetIndex);
    void markCustom();

    QComboBox *m_presetCombo = nullptr;
    QComboBox *m_easingCombo = nullptr;
    RectEditors m_startEditors;
    RectEditors m_endEditors;
    bool m_updatingPreset = false;
};
