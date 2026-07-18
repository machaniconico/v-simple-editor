#pragma once

// BroadcastCaptionDialog — 放送用クローズドキャプション (CEA-608 / CEA-708) の
// メタデータ編集 + SCC サイドカー エクスポート ダイアログ (CC-4)。
//
// 規格 (CEA-608 / CEA-708)、CC チャンネル、SCC タイムコード換算用フレームレート、
// および取り込み済みの字幕 cue 数を編集/表示する最小 UI を提供する。cue 本体は
// MainWindow 側で既存字幕 (subtitleSegments) から充填されるため、本ダイアログでは
// cue を直接編集せず非破壊に保持する (将来スコープで cue テーブル編集を追加予定)。
//
// 設定の保持 (SSOT は MainWindow::m_broadcastCaption) と .veditor への永続化は
// MainWindow が担う。「SCC をエクスポート...」で exportSccRequested() を emit し、
// MainWindow 側で broadcastcc::exportScc を用いてファイルへ書き出す。
//
// 前例: DolbyVisionDialog (DV-4)。

#include <QDialog>

#include "BroadcastCaption.h"  // broadcastcc::BroadcastCaptionDoc

class QComboBox;
class QSpinBox;
class QLabel;

class BroadcastCaptionDialog : public QDialog {
    Q_OBJECT
public:
    explicit BroadcastCaptionDialog(QWidget *parent = nullptr);

    // 編集対象のドキュメントを UI へ反映する。
    void setDocument(const broadcastcc::BroadcastCaptionDoc &doc);
    // 現在の UI 状態から構築した BroadcastCaptionDoc を返す。
    broadcastcc::BroadcastCaptionDoc document() const;

signals:
    // 「SCC をエクスポート...」押下時に emit。MainWindow がファイル保存を担う。
    void exportSccRequested();

private:
    // 編集対象ドキュメントの基準コピー。UI に出さないフィールド (cue 本体) は
    // setDocument 時点の値を保持し、document() で書き戻して非破壊に保つ。
    broadcastcc::BroadcastCaptionDoc m_base;

    QComboBox *m_standardCombo  = nullptr;
    QSpinBox  *m_channelSpin    = nullptr;
    QComboBox *m_frameRateCombo = nullptr;
    QLabel    *m_cueCountLabel  = nullptr;
};
