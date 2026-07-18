#pragma once

// ThreePointEdit — ソースモニター3点編集 (Insert / Overwrite) の純粋エンジン。
//
// QWidget / QObject を一切持たない純粋ロジックなので QApplication 不要で
// headless 単体テストできる (SM-1 selftest `--selftest=three-point-edit`,
// needsQApplication=false)。SourceMonitorDock (SM-2) が編集中の選択範囲を
// SourceSelection で受け取り、buildClipInfo() で ClipInfo に変換、SM-3 (HUB)
// が planOverwrite() / insertIndexForTime() の純粋計算結果を TimelineTrack の
// splitClipAt / removeClip / insertClipPreservingDownstream で実行する。
//
// タイムラインの絶対秒モデル: Timeline.h の leadInSec モデルに従う。
//   start(0) = clips[0].leadInSec;
//   start(i) = start(i-1) + clips[i-1].effectiveDuration() + clips[i].leadInSec;
//   end(i)   = start(i) + clips[i].effectiveDuration();

#include <QString>
#include <QVector>
#include "Timeline.h"   // ClipInfo

namespace threepoint {

// Insert = 既存クリップを右へ押し出して割り込む。
// Overwrite = [T, T+L) を上書きし、既存クリップを分割/削除する。
enum class EditMode {
    Insert,
    Overwrite
};

// ソースモニターでマークした素材の選択範囲。sourceOutSec<=0 は durationSec を
// 終端とみなす (未マーク = 素材末尾まで)。
struct SourceSelection {
    QString filePath;
    QString displayName;
    double durationSec = 0.0;
    double sourceInSec = 0.0;
    double sourceOutSec = 0.0;   // <=0 なら durationSec を終端とみなす
};

// 実効 out 秒。sourceOutSec>0 ならそれ、そうでなければ durationSec。
double effectiveOutSec(const SourceSelection &sel);

// 選択範囲の長さ (effectiveOut - sourceIn)。負値は 0 にクランプ。
double selectionDurationSec(const SourceSelection &sel);

// 選択範囲の妥当性検証。filePath 非空 / durationSec>0 /
// 0<=sourceIn<effectiveOut<=durationSec を確認する。失敗時 errorOut に
// 日本語の理由を書き込む (errorOut は nullptr 可)。
bool validate(const SourceSelection &sel, QString *errorOut);

// SourceSelection から ClipInfo を組み立てる。filePath/displayName/
// duration=durationSec/inPoint=sourceInSec/outPoint=effectiveOut/speed=1.0
// をセットする。validate() を通っている前提 (呼び出し側で検証すること)。
ClipInfo buildClipInfo(const SourceSelection &sel);

// 1 トラック分の ClipInfo 列について、各クリップの絶対開始秒を返す。
// 上記モデル: start(0)=leadIn[0]; start(i)=start(i-1)+effDur(i-1)+leadIn[i]。
QVector<double> clipStartSeconds(const QVector<ClipInfo> &clips);

// Overwrite 配置プラン。純粋計算結果のみを保持し、実行は SM-3 が行う。
// 実行順序 (HUB / SM-3): 必ず以下の順で TimelineTrack に適用すること。
//   (1) splitTailIndex>=0 なら track.splitClipAt(splitTailIndex, splitTailLocalSec)
//   (2) splitHeadIndex>=0 なら track.splitClipAt(splitHeadIndex, splitHeadLocalSec)
//   (3) removeCount>0 なら removeFromIndex から removeCount 個を末尾側から削除
//   (4) track.insertClipPreservingDownstream(insertIndex, newClip, insertLeadInSec)
// split*Index は元配列インデックス基準、removeFromIndex / insertIndex は
// (1)(2) の分割を先に適用した後の論理インデックス基準で計算済み。tail を先に
// 分割するのは、head 分割で挿入されるインデックスのズレが tail 側に波及しない
// ようにするため (tail の方が後ろなので先に切れば head 分割の +1 ズレに影響
// されない)。
struct OverwritePlan {
    bool valid = false;
    int splitHeadIndex = -1;       // T が内部を跨ぐクリップ (分割不要なら -1)
    double splitHeadLocalSec = 0.0;
    int splitTailIndex = -1;       // T+L が内部を跨ぐクリップ (分割不要なら -1)
    double splitTailLocalSec = 0.0;
    int removeFromIndex = -1;      // [T,T+L) に完全に収まる既存クリップ群の先頭
    int removeCount = 0;
    int insertIndex = 0;           // 新クリップ挿入位置 (分割適用後の論理 index)
    double insertLeadInSec = 0.0;  // 挿入クリップの先行ギャップ
};

// 新クリップが [T, T+L) を占有する Overwrite のプランを純粋計算する。
// T=timelineStartSec, L=newClipDurationSec。詳細仕様は OverwritePlan の
// コメント参照。空トラックや末尾以降 (T>=全長) はギャップ付き末尾追加として
// valid=true を返す。この関数は計算のみで TimelineTrack を一切触らない。
OverwritePlan planOverwrite(const QVector<ClipInfo> &clips,
                            double timelineStartSec,
                            double newClipDurationSec);

// Insert 用の薄いヘルパー。planDrop と同等の (挿入 index, 先行 leadIn) を
// timelineStartSec から純粋計算する。leadInOut が非 nullptr なら挿入クリップの
// leadInSec を書き込む。戻り値は挿入 index。
int insertIndexForTime(const QVector<ClipInfo> &clips,
                       double timelineStartSec,
                       double *leadInOut);

} // namespace threepoint
