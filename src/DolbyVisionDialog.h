#pragma once

// DolbyVisionDialog — Dolby Vision 動的メタデータ (dolbyvision::DolbyVisionMetadata)
// の編集ダイアログ (DV-4)。プロファイル (5 / 8.1)、タイトル、Level6 (CLL/FALL/
// マスタリングディスプレイ輝度)、および先頭ショットの Level1 輝度 (min/avg/max nits)
// を編集できる最小 UI を提供する。詳細な per-shot / Level2 trim 編集は後続スコープ。
//
// 設定の保持 (SSOT は MainWindow::m_dolbyVision) と .veditor への永続化までを担う。
// 「XML をエクスポート...」で exportXmlRequested() を emit し、MainWindow 側で
// dolbyvision::toDolbyVisionXml を用いてファイルへ書き出す。
//
// 前例: ColorManagementDialog (AC-4)。

#include <QDialog>

#include "DolbyVisionMetadata.h"  // dolbyvision::DolbyVisionMetadata

class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;

class DolbyVisionDialog : public QDialog {
    Q_OBJECT
public:
    explicit DolbyVisionDialog(QWidget *parent = nullptr);

    // 編集対象のメタデータを UI へ反映する。
    void setMetadata(const dolbyvision::DolbyVisionMetadata &meta);
    // 現在の UI 状態から構築した DolbyVisionMetadata を返す。
    dolbyvision::DolbyVisionMetadata metadata() const;

signals:
    // 「XML をエクスポート...」押下時に emit。MainWindow がファイル保存を担う。
    void exportXmlRequested();

private:
    // 編集対象メタデータの基準コピー。UI に出さないフィールド (Level2 trim,
    // Level5 アクティブエリア, 2 ショット目以降) は setMetadata 時点の値を保持し、
    // metadata() で書き戻して非破壊に保つ。
    dolbyvision::DolbyVisionMetadata m_base;

    QComboBox      *m_profileCombo      = nullptr;
    QLineEdit      *m_titleEdit         = nullptr;
    QSpinBox       *m_maxCllSpin        = nullptr;
    QSpinBox       *m_maxFallSpin       = nullptr;
    QSpinBox       *m_masteringMaxSpin  = nullptr;
    QSpinBox       *m_masteringMinSpin  = nullptr;
    QLabel         *m_shotCountLabel    = nullptr;
    QDoubleSpinBox *m_shotMinNitsSpin   = nullptr;
    QDoubleSpinBox *m_shotAvgNitsSpin   = nullptr;
    QDoubleSpinBox *m_shotMaxNitsSpin   = nullptr;
};
