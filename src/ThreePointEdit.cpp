// ThreePointEdit.cpp — ソースモニター3点編集の純粋エンジン (SM-1)。
// 実装方針は ThreePointEdit.h のヘッダコメント参照。QApplication 不要。

#include "ThreePointEdit.h"

#include <QObject>   // QObject::tr (日本語エラーメッセージ用)
#include <cmath>

namespace threepoint {

namespace {
// タイムライン秒比較の許容誤差。Timeline::planDrop と同じ 1e-6 を採用し、
// 境界ちょうど (split 不要) の判定が浮動小数の丸めでブレないようにする。
constexpr double kEps = 1e-6;
} // namespace

double effectiveOutSec(const SourceSelection &sel)
{
    return (sel.sourceOutSec > 0.0) ? sel.sourceOutSec : sel.durationSec;
}

double selectionDurationSec(const SourceSelection &sel)
{
    const double dur = effectiveOutSec(sel) - sel.sourceInSec;
    return (dur > 0.0) ? dur : 0.0;
}

bool validate(const SourceSelection &sel, QString *errorOut)
{
    auto setErr = [&](const QString &msg) {
        if (errorOut) *errorOut = msg;
    };

    if (sel.filePath.isEmpty()) {
        setErr(QObject::tr("ソースのファイルパスが空です。"));
        return false;
    }
    if (sel.durationSec <= 0.0) {
        setErr(QObject::tr("ソースの長さが不正です (0 秒以下)。"));
        return false;
    }
    const double outSec = effectiveOutSec(sel);
    if (sel.sourceInSec < 0.0) {
        setErr(QObject::tr("イン点が負の値です。"));
        return false;
    }
    if (sel.sourceInSec >= outSec - kEps) {
        setErr(QObject::tr("イン点がアウト点以上です (選択範囲が空です)。"));
        return false;
    }
    if (outSec > sel.durationSec + kEps) {
        setErr(QObject::tr("アウト点がソースの長さを超えています。"));
        return false;
    }
    if (errorOut) errorOut->clear();
    return true;
}

ClipInfo buildClipInfo(const SourceSelection &sel)
{
    ClipInfo clip;
    clip.filePath = sel.filePath;
    clip.displayName = sel.displayName;
    clip.duration = sel.durationSec;
    clip.inPoint = sel.sourceInSec;
    clip.outPoint = effectiveOutSec(sel);
    clip.speed = 1.0;
    return clip;
}

QVector<double> clipStartSeconds(const QVector<ClipInfo> &clips)
{
    QVector<double> starts;
    starts.reserve(clips.size());
    double cursor = 0.0; // 直前クリップの終了秒 (= clip[i] 手前のギャップ開始)
    for (int i = 0; i < clips.size(); ++i) {
        const double start = cursor + clips[i].leadInSec;
        starts.append(start);
        cursor = start + clips[i].effectiveDuration();
    }
    return starts;
}

int insertIndexForTime(const QVector<ClipInfo> &clips,
                       double timelineStartSec,
                       double *leadInOut)
{
    double T = timelineStartSec;
    if (T < 0.0) T = 0.0;

    // planDrop と同じ走査: cursor=直前クリップの終了秒。clip[i] の手前ギャップ
    // [cursor, clipStart) に T が収まる最初の i を挿入位置とする。
    double cursor = 0.0;
    for (int i = 0; i < clips.size(); ++i) {
        const double clipStart = cursor + clips[i].leadInSec;
        if (T <= clipStart + kEps) {
            if (leadInOut) *leadInOut = (T - cursor > 0.0) ? (T - cursor) : 0.0;
            return i;
        }
        cursor = clipStart + clips[i].effectiveDuration();
    }
    // 末尾以降への挿入。
    if (leadInOut) *leadInOut = (T - cursor > 0.0) ? (T - cursor) : 0.0;
    return clips.size();
}

OverwritePlan planOverwrite(const QVector<ClipInfo> &clips,
                            double timelineStartSec,
                            double newClipDurationSec)
{
    OverwritePlan plan{};

    double T = timelineStartSec;
    if (T < 0.0) T = 0.0;
    double L = newClipDurationSec;
    if (L < 0.0) L = 0.0;
    const double Tend = T + L;

    const QVector<double> starts = clipStartSeconds(clips);
    const int n = clips.size();

    // --- (A) head/tail 分割の判定 (元配列インデックス基準) -----------------
    // T が clip[i] の内部 (start < T < end) を strict に跨ぐなら head 分割。
    // 境界ちょうどなら分割不要。
    int origSplitHead = -1;
    double origSplitHeadLocal = 0.0;
    int origSplitTail = -1;
    double origSplitTailLocal = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s = starts[i];
        const double e = s + clips[i].effectiveDuration();
        if (T > s + kEps && T < e - kEps) {
            origSplitHead = i;
            origSplitHeadLocal = T - s; // clip-local 秒 (effectiveDuration 空間)
        }
        if (Tend > s + kEps && Tend < e - kEps) {
            origSplitTail = i;
            origSplitTailLocal = Tend - s;
        }
    }

    // --- (B) 分割を論理適用した後のインデックス空間を構築 -------------------
    // tail を先に分割 → head を分割、の順 (ヘッダ実行順序と一致)。各「論理
    // クリップ」を {start, end} で表し、分割で 2 要素に増えるのを反映する。
    struct Seg { double start; double end; };
    QVector<Seg> segs;
    segs.reserve(n + 2);
    for (int i = 0; i < n; ++i)
        segs.append(Seg{ starts[i], starts[i] + clips[i].effectiveDuration() });

    // tail を先に分割 (後ろ側なので head 分割の index ズレを受けない)。
    if (origSplitTail >= 0) {
        const Seg orig = segs[origSplitTail];
        segs[origSplitTail].end = Tend;                 // 前半
        segs.insert(origSplitTail + 1, Seg{ Tend, orig.end }); // 後半
    }
    // head を分割。
    if (origSplitHead >= 0) {
        // head の元 index は tail 挿入 (origSplitTail < origSplitHead はあり得ない:
        // T <= Tend なので head は tail と同じか前) を考慮する必要がない。
        // T <= Tend より origSplitHead <= origSplitTail。よって tail 分割で
        // origSplitHead より後ろに 1 要素挿入されても head の index は不変。
        const Seg orig = segs[origSplitHead];
        segs[origSplitHead].end = T;                    // 前半
        segs.insert(origSplitHead + 1, Seg{ T, orig.end }); // 後半
    }

    // --- (C) [T, Tend) に完全に収まる論理セグメントを削除対象に ------------
    int removeFrom = -1;
    int removeCount = 0;
    for (int i = 0; i < segs.size(); ++i) {
        const Seg &g = segs[i];
        const bool inside = (g.start >= T - kEps) && (g.end <= Tend + kEps)
                            && (g.end - g.start > kEps); // 0 幅は無視
        if (inside) {
            if (removeFrom < 0) removeFrom = i;
            ++removeCount;
        }
    }

    // --- (D) 挿入位置: 削除区間の先頭。削除が無い場合は T 以上で始まる最初の
    //         セグメント、無ければ末尾。挿入後ろの leadIn ギャップを算出。
    int insertIdx;
    if (removeFrom >= 0) {
        insertIdx = removeFrom;
    } else {
        insertIdx = segs.size();
        for (int i = 0; i < segs.size(); ++i) {
            if (segs[i].start >= T - kEps) { insertIdx = i; break; }
        }
    }

    // 挿入クリップ手前のギャップ = T - (直前セグメントの終了秒)。先頭挿入なら
    // 直前は 0。leadInSec は削除後の論理列に対して計算する。
    double prevEnd = 0.0;
    if (insertIdx > 0 && insertIdx - 1 < segs.size())
        prevEnd = segs[insertIdx - 1].end;
    double leadIn = T - prevEnd;
    if (leadIn < 0.0) leadIn = 0.0;

    plan.valid = true;
    plan.splitHeadIndex = origSplitHead;
    plan.splitHeadLocalSec = origSplitHeadLocal;
    plan.splitTailIndex = origSplitTail;
    plan.splitTailLocalSec = origSplitTailLocal;
    plan.removeFromIndex = removeFrom;
    plan.removeCount = removeCount;
    plan.insertIndex = insertIdx;
    plan.insertLeadInSec = leadIn;
    return plan;
}

} // namespace threepoint
