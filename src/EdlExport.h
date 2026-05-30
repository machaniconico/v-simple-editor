#pragma once

// EdlExport (ED-1): CMX3600 EDL の純粋書き出しエンジン。
//
// CMX3600 EDL は放送 / Avid Media Composer / DaVinci Resolve / Premiere Pro が
// 相互に取り込める歴史あるテキスト交換形式。1 イベント = 1 編集点で、ソース側
// (srcIn/srcOut) と レコード側 (recIn/recOut) のタイムコードを並べる。
//
// この module は QObject / QWidget を一切持たず、QString / QVector / QJsonObject
// だけで完結する純粋エンジンなので QApplication 不要 = headless selftest 可能。
// 既存の交換エクスポータ (PremiereXmlExporter / FcpxmlExporter) と同じ「Timeline の
// ClipInfo 列 → 中間ドキュメント struct → テキスト直列化」の作法に倣う。

#include <QChar>
#include <QString>
#include <QVector>
#include <QJsonObject>

#include "Timeline.h"   // ClipInfo (filePath/displayName/duration/inPoint/outPoint/leadInSec/speed/effectiveDuration())

namespace edl {

// ---------------------------------------------------------------------------
// タイムコード換算
// ---------------------------------------------------------------------------

// frames を HH:MM:SS:FF (ノンドロップ ':') または HH:MM:SS;FF (ドロップ ';')
// のタイムコード文字列へ変換する。dropFrame=true かつ 29.97 / 59.94 系
// (29.97→1分あたり2フレーム, 59.94→4フレーム drop) のとき正しいドロップフレーム
// 計算を行う: 毎分先頭の N フレームをスキップし、ただし 10 分目はスキップしない。
// frameRate が整数系 (24/25/30/50/60) のときは dropFrame 指定に関わらず NDF 扱い。
QString framesToTimecode(qint64 frames, double frameRate, bool dropFrame);

// 秒 → フレーム数 (四捨五入)。負の sec は 0 にクランプ。
qint64 secToFrames(double sec, double frameRate);

// ---------------------------------------------------------------------------
// 中間ドキュメントモデル
// ---------------------------------------------------------------------------

// 1 編集イベント (= EDL の 1 行ブロック)。
struct EdlEvent {
    int number = 1;                 // イベント番号 (001 から連番)
    QString reel = QStringLiteral("AX");  // リール名 (最大 8 文字慣行)
    QString trackType = QStringLiteral("V"); // V / A / A2 / B (B=both: V+A)
    QChar transition = QLatin1Char('C');     // C=cut, D=dissolve, W=wipe
    int transitionFrames = 0;       // dissolve/wipe の継続フレーム数 (cut では 0)
    QString clipName;               // "* FROM CLIP NAME:" コメントに出すクリップ名
    qint64 srcInFrames = 0;         // ソース素材の IN  (フレーム)
    qint64 srcOutFrames = 0;        // ソース素材の OUT (フレーム)
    qint64 recInFrames = 0;         // タイムライン (レコード) の IN  (フレーム)
    qint64 recOutFrames = 0;        // タイムライン (レコード) の OUT (フレーム)
};

// EDL ドキュメント全体。
struct EdlDocument {
    QString title = QStringLiteral("VEDITOR EDL");
    double frameRate = 29.97;
    bool dropFrame = true;
    QVector<EdlEvent> events;
};

// ---------------------------------------------------------------------------
// 直列化 / 逆直列化
// ---------------------------------------------------------------------------

// EdlDocument を CMX3600 テキストへ直列化する。
//
// 出力フォーマット:
//   TITLE: <title>\n
//   FCM: DROP FRAME      (dropFrame=true)  /  FCM: NON-DROP FRAME (false)\n
//   <イベント行>...
//
// 各イベント行 (固定幅フィールド):
//   "NNN  REEL     TT    X        SRCIN SRCOUT RECIN RECOUT"
//   = number(3桁0詰め)  reel(左詰め8)  trackType(左詰め6)  transition(左詰め8)
//     srcIn srcOut recIn recOut (全てタイムコード, 半角空白区切り)
// dissolve / wipe のイベントは CMX3600 慣行に従い、当該イベントの transition 欄へ
// 'D'/'W' と transitionFrames を入れる簡略実装。
// 各イベント行の直後に "* FROM CLIP NAME: <clipName>" コメント行を出す
// (clipName が空なら省略)。
QString toCmx3600(const EdlDocument& doc);

// 1 トラック分の ClipInfo 列を EdlDocument へ変換する。
//   - number は 1 からの連番
//   - recIn/recOut はタイムライン絶対位置 (各クリップの leadInSec を累積し、
//     そのクリップの effectiveDuration() を足して算出)
//   - srcIn/srcOut は inPoint / (outPoint>0 ? outPoint : duration)
//   - rec duration は (srcOut-srcIn)/speed (speed<=0 は 1.0 扱い)
//   - reel は displayName 由来 (英数字を大文字 8 文字に整形) / 空なら "AX"
//   - transition は全て cut ('C')
EdlDocument fromClips(const QVector<ClipInfo>& clips,
                      const QString& title,
                      double frameRate,
                      bool dropFrame);

// 設定永続化用 JSON 直列化 / 逆直列化 (任意)。
QJsonObject toJson(const EdlDocument& doc);
EdlDocument fromJson(const QJsonObject& obj);

} // namespace edl
