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

    // --- Drop shadow ---
    QCheckBox *m_shadowEnabled = nullptr;
    QDoubleSpinBox *m_shadowOffX = nullptr;
    QDoubleSpinBox *m_shadowOffY = nullptr;
    QDoubleSpinBox *m_shadowBlur = nullptr;
    QPushButton *m_shadowColorBtn = nullptr;
    QDoubleSpinBox *m_shadowOpacity = nullptr;
    QColor m_shadowColor = QColor(0, 0, 0, 200);

    // --- Outer glow ---
    QCheckBox *m_glowEnabled = nullptr;
    QDoubleSpinBox *m_glowRadius = nullptr;
    QPushButton *m_glowColorBtn = nullptr;
    QDoubleSpinBox *m_glowOpacity = nullptr;
    QColor m_glowColor = QColor(255, 255, 0, 255);

    // --- Outline (stroke) ---
    QCheckBox *m_outlineEnabled = nullptr;
    QSpinBox *m_outlineWidth = nullptr;
    QPushButton *m_outlineColorBtn = nullptr;
    QColor m_outlineColor = Qt::black;
};

// Persistence layer for user-saved Transition presets. Backed by
// QSettings("VSimpleEditor", "Preferences") under the "transitionPresets"
// group. All functions are static — no instance state.
class TransitionPresetStore {
public:
    static QList<TransitionPreset> loadAll();
    static void save(const QString &name, const Transition &t);
    static void remove(const QString &name);
};

class TransitionDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TransitionDialog(QWidget *parent = nullptr);
    Transition result() const { return m_result; }
private:
    void setupUI();
    void refreshPresetCombo();
    Transition m_result;
    QComboBox *m_typeCombo;
    QDoubleSpinBox *m_durationSpin;
    QComboBox *m_alignmentCombo;
    QComboBox *m_easingCombo;
    QDoubleSpinBox *m_softnessSpin;
    QDoubleSpinBox *m_borderWidthSpin;
    QPushButton *m_borderColorBtn;
    QColor m_borderColor = Qt::white;
    QComboBox *m_presetCombo;
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
