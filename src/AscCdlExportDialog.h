#pragma once
// AscCdlExportDialog: ASC CDL (Color Decision List) 書き出しの UI 層。
//
// 役割は「現在のカラーグレーディングからの SOP 抽出」と「ファイル I/O」のみ。
// 実際の XML 生成は純粋エンジン asccdl::buildCc / buildCcc / buildCdl に委譲する
// (このダイアログは Qt/Widget 層)。
//
// 入力:
//   - setCorrection(asccdl::CdlCorrection) … 現在のグレーディングから抽出した SOP+Sat。
//     MainWindow が ColorGradingPanel の currentWheels()/colorCorrection() を
//     asccdl::fromLgg() で変換して渡す。
//
// 形式 (Format):
//   - Cc  … 単一 <ColorCorrection> ドキュメント (.cc)
//   - Ccc … <ColorCorrectionCollection>          (.ccc)
//   - Cdl … <ColorDecisionList>                   (.cdl)
//
// 「書き出し」押下で出力先へ UTF-8 で QFile 書き込みし、QMessageBox で結果を通知して accept()。

#include <QDialog>

#include "colorexport/AscCdlExport.h"   // asccdl::CdlCorrection

class QComboBox;
class QLineEdit;
class QPushButton;

class AscCdlExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit AscCdlExportDialog(QWidget *parent = nullptr);

    // 書き出す補正 (SOP+Saturation) をセットする。
    // id はダイアログの id 入力欄の初期値にも反映する。
    void setCorrection(const asccdl::CdlCorrection &correction);

private slots:
    // 出力先参照 (QFileDialog::getSaveFileName)。選択中の形式で拡張子フィルタを切替える。
    void browseOutputPath();
    // 形式コンボ変更: 出力欄の拡張子を選択形式に合わせて補正する。
    void onFormatChanged();
    // 「書き出し」押下: id 反映 → 形式に応じて buildCc/Ccc/Cdl → QFile 書き込み → 通知 → accept。
    void doExport();

private:
    // 形式種別。コンボの itemData と一致させる。
    enum Format {
        FormatCc  = 0,  // .cc  単一 ColorCorrection
        FormatCcc = 1,  // .ccc ColorCorrectionCollection
        FormatCdl = 2,  // .cdl ColorDecisionList
    };

    // 選択中の形式に対応する拡張子 (".cc" / ".ccc" / ".cdl") を返す。
    static QString extForFormat(int format);

    asccdl::CdlCorrection m_correction;

    QComboBox   *m_formatCombo = nullptr;
    QLineEdit   *m_idEdit      = nullptr;
    QLineEdit   *m_outputEdit  = nullptr;
    QPushButton *m_browseBtn   = nullptr;
    QPushButton *m_exportBtn   = nullptr;
    QPushButton *m_closeBtn    = nullptr;
};
