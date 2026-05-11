#pragma once

#include <QDialog>
#include <QFont>
#include <QPointF>
#include <QString>
#include "BrushAnimation.h"

class QFontComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QLineEdit;
class QDialogButtonBox;

class BrushAnimationDialog : public QDialog
{
    Q_OBJECT

public:
    struct BrushAnimationParams {
        QString text;
        QFont font;
        double brushWidth = 4.0;
        double durationSec = 2.0;
        BrushAnimationMode mode = PerStroke;
        QPointF basePosition;
    };

    explicit BrushAnimationDialog(QWidget *parent = nullptr);

    BrushAnimationParams params() const;

private:
    void setupUI();

    QLineEdit *m_textEdit = nullptr;
    QFontComboBox *m_fontCombo = nullptr;
    QSpinBox *m_fontSizeSpin = nullptr;
    QDoubleSpinBox *m_brushWidthSpin = nullptr;
    QDoubleSpinBox *m_durationSpin = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
};
