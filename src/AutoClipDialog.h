#pragma once

#include "AutoClipGenerator.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;
class QDialogButtonBox;

class AutoClipDialog : public QDialog {
    Q_OBJECT

public:
    explicit AutoClipDialog(QWidget* parent = nullptr);

    // UI 値から AutoClipConfig を構築して返す
    autoclip::AutoClipConfig config() const;

    // 検出済みハイライト件数 (0 なら『カット範囲を計算』ボタンを disable)
    void setHighlightCount(int count);

    // ソース動画の長さ (秒) — 説明表示用
    void setSourceDuration(double durationSec);

private:
    QComboBox*       m_aspectCombo   = nullptr;
    QDoubleSpinBox*  m_minDurationSpin = nullptr;
    QDoubleSpinBox*  m_maxDurationSpin = nullptr;
    QSpinBox*        m_maxClipsSpin  = nullptr;
    QDialogButtonBox* m_buttonBox    = nullptr;
    QPushButton*     m_computeButton = nullptr;
};
