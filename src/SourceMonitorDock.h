#pragma once

// src/SourceMonitorDock.h
// SM-4: ソースモニターの UI ドック。中央に VideoPlayer を 1 個 new して
// 素材プレビューを表示し、スクラブ用 QSlider とマークイン/アウトボタン、
// 「挿入」/「上書き」ボタンを持つ。VideoPlayer は singleton ではないので
// 2 個目を生成してよい (MainWindow 固有シグナルには接続しない)。
//
// マークした範囲は threepoint::SourceSelection として currentSelection() /
// insertRequested / overwriteRequested 経由で外部へ渡す。3 点編集の純粋計算は
// SM-1 (ThreePointEdit.h) が、TimelineTrack への実適用は SM-3 (HUB) が行う。

#include <QDockWidget>

#include "ThreePointEdit.h"

class VideoPlayer;
class QSlider;
class QLabel;
class QPushButton;

class SourceMonitorDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit SourceMonitorDock(QWidget *parent = nullptr);

    // 素材をロードする。durationSec<=0 のときは VideoPlayer の durationChanged
    // シグナル経由で実尺が判明した時点でスライダー範囲を補正する。
    void loadSource(const threepoint::SourceSelection &sel);
    void loadSource(const QString &filePath, double durationSec,
                    const QString &displayName);

    // 現在マークしている選択範囲。filePath/displayName/durationSec/
    // sourceInSec/sourceOutSec を詰めて返す。未ロード時は空の filePath。
    threepoint::SourceSelection currentSelection() const;

signals:
    // 「挿入 (Insert)」押下時。現在の選択範囲を渡す。
    void insertRequested(const threepoint::SourceSelection &sel);
    // 「上書き (Overwrite)」押下時。現在の選択範囲を渡す。
    void overwriteRequested(const threepoint::SourceSelection &sel);

private slots:
    void onScrub(int positionMs);
    void onMarkIn();
    void onMarkOut();
    void onInsertClicked();
    void onOverwriteClicked();
    void onPlayerDurationChanged(double durationSeconds);

private:
    void setupUI();
    // ボタンの有効/無効と各ラベルを現在状態から更新する。
    void updateControls();
    // m_durationSec を秒で受け取り、スライダーの ms レンジを張り直す。
    void applyDurationToSlider();

    VideoPlayer *m_viewer = nullptr;
    QSlider     *m_scrubBar    = nullptr;
    QLabel      *m_inLabel     = nullptr;
    QLabel      *m_outLabel    = nullptr;
    QLabel      *m_posLabel    = nullptr;
    QPushButton *m_markInBtn   = nullptr;
    QPushButton *m_markOutBtn  = nullptr;
    QPushButton *m_insertBtn   = nullptr;
    QPushButton *m_overwriteBtn = nullptr;

    // 現在ロード中の素材。未ロード時は filePath が空。
    QString m_filePath;
    QString m_displayName;
    double  m_durationSec = 0.0;
    // 現在のスクラブ位置 (秒)。マークイン/アウトの記録元。
    double  m_scrubSec = 0.0;
    // マーク済み in/out (秒)。out<=0 は未マーク (素材末尾を終端とみなす)。
    double  m_sourceInSec  = 0.0;
    double  m_sourceOutSec = 0.0;
    bool    m_loaded = false;
};
