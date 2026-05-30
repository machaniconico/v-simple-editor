#pragma once
// PptxExportDialog: PowerPoint (.pptx) 書き出しの UI 層。
//
// 役割は「デッキの組み立て」と「ファイル I/O」のみ。実際の OOXML / store-ZIP 生成は
// 純粋エンジン pptxexport::buildPptx に委譲する (このダイアログは Qt/Widget 層)。
//
// 入力:
//   - setCaptions(QList<caption::Clip>) … 文字起こし結果 (Whisper → 字幕エディタ経由)。
//   - setMarkers(QVector<Marker>)        … タイムラインのマーカー/章一覧 (任意)。
//
// デッキ種別 (DeckKind):
//   - Transcript … 1 clip = 1 slide (連番タイトル + 本文 1 行)。
//   - Markers    … 1 marker = 1 slide (タイムコード + ラベル)。
//   - TitleOnly  … タイトル 1 枚のみ。
//
// 「書き出し」押下で出力先へ QFile 書き込みし、QMessageBox で結果を通知して accept()。

#include <QDialog>
#include <QList>
#include <QVector>

#include "CaptionTrack.h"   // caption::Clip
#include "MarkerData.h"     // Marker
#include "PptxExport.h"     // pptxexport::Deck

class QComboBox;
class QLineEdit;
class QPushButton;

class PptxExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit PptxExportDialog(QWidget *parent = nullptr);

    // 文字起こし結果をセット (Transcript デッキの元データ)。
    void setCaptions(const QList<caption::Clip> &captions);
    // マーカー/章一覧をセット (Markers デッキの元データ)。容易に取れない場合は未設定でよい。
    void setMarkers(const QVector<Marker> &markers);

private slots:
    // 出力先参照 (QFileDialog::getSaveFileName)。
    void browseOutputPath();
    // 「書き出し」押下: デッキ組み立て → buildPptx → QFile 書き込み → 結果通知 → accept。
    void doExport();

private:
    // デッキ種別。コンボのインデックスと一致させる。
    enum DeckKind {
        DeckTranscript = 0,
        DeckMarkers    = 1,
        DeckTitleOnly  = 2,
    };

    // 現在の選択・入力から pptxexport::Deck を組み立てる純粋ヘルパ。
    // UI 状態には依存するが副作用 (I/O) は持たない。
    pptxexport::Deck buildDeck() const;

    QList<caption::Clip> m_captions;
    QVector<Marker>      m_markers;

    QComboBox  *m_kindCombo   = nullptr;
    QLineEdit  *m_titleEdit   = nullptr;
    QLineEdit  *m_authorEdit  = nullptr;
    QLineEdit  *m_outputEdit  = nullptr;
    QPushButton *m_browseBtn  = nullptr;
    QPushButton *m_exportBtn  = nullptr;
    QPushButton *m_closeBtn   = nullptr;
};
