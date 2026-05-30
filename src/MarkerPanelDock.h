#pragma once

// src/MarkerPanelDock.h
// MK-2: Premiere Pro / DaVinci Resolve の「マーカー」パネル相当の常設ドック。
// 既存のマーカー機能は「全マーカーを表示...」のモーダルダイアログだけだったが、
// このドックはタイムライン上の全マーカーを表形式で常時表示し、ダブルクリックで
// 再生ヘッドをジャンプ、ノート列の編集、削除ボタンを提供する。
//
// Timeline / Marker を所有はしない。MainWindow が Timeline の markersChanged を
// 受けて setMarkers() で表示内容を流し込むだけのビュー (空リストでも安全)。

#include <QDockWidget>
#include <QVector>

#include "MarkerData.h"

class QTableWidget;
class QTableWidgetItem;
class QPushButton;

class MarkerPanelDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit MarkerPanelDock(QWidget *parent = nullptr);

    // 表示するマーカー一覧を差し替えて再描画する。所有はしない。
    void setMarkers(const QVector<Marker> &markers);

signals:
    // 行をダブルクリックしたとき。再生ヘッドをこの時刻 (マイクロ秒) へ移動する。
    void jumpToMarker(qint64 timelineUs);
    // ノート列を編集して確定したとき。
    void markerNoteEdited(int markerId, const QString &note);
    // 「削除」ボタン押下時。選択行のマーカー id を渡す。
    void markerDeleteRequested(int markerId);

private slots:
    void onItemDoubleClicked(QTableWidgetItem *item);
    void onItemChanged(QTableWidgetItem *item);
    void onDeleteClicked();

private:
    // durationUs を mm:ss.mmm 形式 (0 は「—」) に整形する。
    static QString formatDuration(qint64 durationUs);
    // timelineUs を mm:ss.mmm 形式に整形する。
    static QString formatTime(qint64 timelineUs);

    // 行 row に対応するマーカー id を返す (見つからなければ -1)。
    int markerIdForRow(int row) const;

    QTableWidget *m_table  = nullptr;
    QPushButton  *m_delBtn = nullptr;

    // setMarkers() 中の cellChanged / itemChanged を無視するためのガード。
    bool m_populating = false;
};
