#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QColorDialog>
#include <QFontComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include "Overlay.h"

class TextOverlayDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TextOverlayDialog(QWidget *parent = nullptr);
    TextOverlay result() const { return m_result; }
private:
    void setupUI();
    TextOverlay m_result;
    QLineEdit *m_textEdit;
    QFontComboBox *m_fontCombo;
    QSpinBox *m_fontSizeSpin;
    QPushButton *m_colorBtn;
    QPushButton *m_bgColorBtn;
    QDoubleSpinBox *m_xSpin;
    QDoubleSpinBox *m_ySpin;
    QDoubleSpinBox *m_startSpin;
    QDoubleSpinBox *m_endSpin;
    QComboBox *m_positionPreset;
    QColor m_textColor = Qt::white;
    QColor m_bgColor = QColor(0, 0, 0, 160);
};

class TransitionDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TransitionDialog(QWidget *parent = nullptr);
    Transition result() const { return m_result; }
private:
    void setupUI();
    Transition m_result;
    QComboBox *m_typeCombo;
    QDoubleSpinBox *m_durationSpin;
};

class ImageOverlayDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ImageOverlayDialog(QWidget *parent = nullptr);
    ImageOverlay result() const { return m_result; }
private:
    void setupUI();
    ImageOverlay m_result;
    QLineEdit *m_pathEdit;
    QDoubleSpinBox *m_xSpin;
    QDoubleSpinBox *m_ySpin;
    QDoubleSpinBox *m_wSpin;
    QDoubleSpinBox *m_hSpin;
    QDoubleSpinBox *m_opacitySpin;
    QCheckBox *m_aspectCheck;
};

class PipDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PipDialog(int maxClipIndex, QWidget *parent = nullptr);
    PipConfig result() const { return m_result; }
private:
    void setupUI(int maxClipIndex);
    PipConfig m_result;
    QSpinBox *m_clipSpin;
    QComboBox *m_positionCombo;
    QDoubleSpinBox *m_sizeSpin;
    QDoubleSpinBox *m_opacitySpin;
};
