#pragma once

// ColorManagementDialog — ACES シーンリファード色管理パイプライン (aces::AcesPipeline)
// の編集ダイアログ (AC-4)。有効化チェック + 入力/作業/出力色空間の選択を提供し、
// 代表色 (中間グレー) を aces::process した結果をスウォッチでプレビューする。
//
// 設定の保持 (SSOT は MainWindow::m_acesPipeline) と .veditor への永続化までを担う。
// TODO: レンダーパイプラインへの ACES 適用 (プレビュー/エクスポート) は後続スコープ。

#include <QDialog>

#include "AcesColor.h"  // aces::AcesPipeline / aces::ColorSpace

class QCheckBox;
class QComboBox;
class QLabel;

class ColorManagementDialog : public QDialog {
    Q_OBJECT
public:
    explicit ColorManagementDialog(QWidget *parent = nullptr);

    // 編集対象のパイプラインを UI へ反映する。
    void setPipeline(const aces::AcesPipeline &pipeline);
    // 現在の UI 状態から構築した AcesPipeline を返す。
    aces::AcesPipeline pipeline() const;

private slots:
    // チェック/コンボの変更でプレビュースウォッチを更新する。
    void onSettingsChanged();

private:
    // コンボボックスへ全色空間を充填し、cs を選択状態にする。
    static void populateColorSpaceCombo(QComboBox *combo, aces::ColorSpace cs);
    // コンボの現在選択を ColorSpace へ変換する。
    static aces::ColorSpace colorSpaceFromCombo(const QComboBox *combo);
    // 代表色 (中間グレー) を現在設定で process し、スウォッチへ反映する。
    void updatePreview();

    QCheckBox *m_enabledCheck   = nullptr;
    QComboBox *m_inputCombo     = nullptr;
    QComboBox *m_workingCombo   = nullptr;
    QComboBox *m_outputCombo    = nullptr;
    QLabel    *m_previewSwatch  = nullptr;
    QLabel    *m_previewValue   = nullptr;
};
