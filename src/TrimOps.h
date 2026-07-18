#pragma once

// TrimOps — タイムライン上の 4 種プロ NLE トリム (Ripple / Roll / Slip / Slide)
// の純粋計算エンジン (TR-1)。
//
// QWidget / QObject を一切持たない純粋ロジックなので QApplication 不要で
// headless 単体テストできる (selftest `--selftest=trim-ops`,
// needsQApplication=false)。UI 層 (トリムツール) は applyTrim() に編集対象
// クリップ index・トリム種別・delta 秒を渡し、成功なら QVector<ClipInfo> を
// in-place 更新した結果を受け取る。境界クランプには maxTrimDelta() /
// minTrimDelta() を使う。
//
// 用語とモデル (Timeline.h / ThreePointEdit.h と同じ leadInSec 絶対秒モデル):
//   - ソース内の使用区間は [inPoint, outPoint]。outPoint<=0 は duration を終端。
//   - タイムライン実尺 effDur(i) = (outEff(i) - inPoint(i)) / speed(i)
//     ただし outEff = (outPoint>0 ? outPoint : duration)。
//   - クリップ絶対開始秒: start(0)=clips[0].leadInSec;
//     start(i)=start(i-1)+effDur(i-1)+clips[i].leadInSec。
//   - leadInSec は直前クリップとの間のギャップ (先頭クリップは原点からの空き)。
//
// delta の単位は「タイムライン秒」。source 量との換算は source量=delta*speed。
// RippleIn / RippleOut / Roll は inPoint/outPoint (source 空間) を動かすので
// delta(timeline) を speed 倍して source に変換する。Slip も同様。Slide は
// source を一切触らず leadInSec (timeline 空間) のみ動かす。

#include <QString>
#include <QVector>
#include "Timeline.h"   // ClipInfo

namespace trimops {

// 4 種トリム + 編集点トリム (Roll)。delta は正負どちらも取り得る。
enum class TrimType {
    RippleIn,   // clip[i] 左端: inPoint を動かし実尺変化、下流を ripple シフト
    RippleOut,  // clip[i] 右端: outPoint を動かし実尺変化、下流を ripple シフト
    Roll,       // clip[i]/clip[i+1] 編集点: outPoint(i)/inPoint(i+1) を同時に動かす
    Slip,       // clip[i]: in/out を同 delta シフト (見せる窓を移動、位置/実尺不変)
    Slide       // clip[i]: タイムライン上で移動 (source 不変、隣接が吸収)
};

// メイン関数。clips を対象に index のクリップへ type のトリムを deltaSec 適用する。
//
// 成功時: clips を in-place 変更し true を返す (errorOut があれば clear)。
// 失敗時 (不正 index / 境界違反): clips は一切変更せず false を返し、errorOut が
//   非 nullptr なら日本語の理由を書き込む。
//
// 各トリムの厳密な意味と境界:
//   RippleIn  : inPoint  += deltaSec*speed。clip[i] のタイムライン開始秒
//               (leadInSec) は不変のまま実尺が deltaSec 縮む/伸び、その差分だけ
//               clip[i+1] 以降がギャップを残さず自動的に deltaSec シフトする
//               (下流の leadInSec は触らない — 実尺変化が ripple を生む)。
//               境界: 結果 inPoint>=0 かつ inPoint<outEff。
//   RippleOut : outPoint += deltaSec*speed。実尺が変わり、clip[i+1] 以降が
//               deltaSec シフト (clip[i+1].leadInSec を deltaSec 動かす)。
//               境界: 結果 outPoint<=duration かつ outPoint>inPoint。
//   Roll      : clip[i].outPoint += deltaSec*speed(i) かつ
//               clip[i+1].inPoint += deltaSec*speed(i+1)。両クリップの実尺が
//               逆向きに変化し、編集点 (clip[i] の終端 = clip[i+1] の始端) が
//               deltaSec 動く。総尺・下流位置は不変。境界: clip[i].outPoint が
//               (inPoint(i), duration(i)] 内、clip[i+1].inPoint が
//               [0, outEff(i+1)) 内。
//   Slip      : clip[i].inPoint += deltaSec*speed かつ
//               clip[i].outPoint += deltaSec*speed。タイムライン位置・実尺は
//               不変。境界: inPoint+Δ>=0 かつ outPoint+Δ<=duration。
//   Slide     : clip[i] を deltaSec だけタイムライン上で移動。source 区間は
//               不変。clip[i-1] が実尺で deltaSec 吸収 (outPoint を deltaSec*
//               speed(i-1) 変更)、clip[i+1] が逆に吸収 (inPoint を deltaSec*
//               speed(i+1) 変更)。先頭/末尾クリップは leadInSec で表現。
//               境界: 隣接クリップの source headroom 内、かつ leadInSec>=0。
//
// speed!=1.0 のクリップでも timeline量=source量/speed の換算を正しく扱う。
bool applyTrim(QVector<ClipInfo> &clips, int index, TrimType type,
               double deltaSec, QString *errorOut);

// その方向に許される最大の正 delta (>=0)。トリム不能なら 0 を返す。UI の
// クランプ用。clips/index が不正なら 0。
double maxTrimDelta(const QVector<ClipInfo> &clips, int index, TrimType type);

// その方向に許される最大の負 delta (<=0)。トリム不能なら 0 を返す。UI の
// クランプ用。clips/index が不正なら 0。
double minTrimDelta(const QVector<ClipInfo> &clips, int index, TrimType type);

} // namespace trimops
