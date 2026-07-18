// TrimOps.cpp — 4 種プロ NLE トリム (Ripple / Roll / Slip / Slide) の純粋
// 計算エンジン (TR-1)。実装方針は TrimOps.h のヘッダコメント参照。QApplication
// 不要。

#include "TrimOps.h"

#include <QObject>   // QObject::tr (日本語エラーメッセージ用)
#include <cmath>
#include <limits>

namespace trimops {

namespace {

// タイムライン秒比較の許容誤差。ThreePointEdit.cpp と同じ 1e-6 を採用し、
// 境界ちょうどの判定が浮動小数の丸めでブレないようにする。
constexpr double kEps = 1e-6;

// クリップのソース実効 outPoint。outPoint<=0 は duration を終端とみなす。
double outEff(const ClipInfo &c)
{
    return (c.outPoint > 0.0) ? c.outPoint : c.duration;
}

// speed の安全値 (0 以下は 1.0 にフォールバック。実尺除算でゼロ割を避ける)。
double safeSpeed(const ClipInfo &c)
{
    return (c.speed > 0.0) ? c.speed : 1.0;
}

bool validIndex(const QVector<ClipInfo> &clips, int index)
{
    return index >= 0 && index < clips.size();
}

double sourceBoundaryMargin(const ClipInfo &c)
{
    return (2.0 * kEps) / safeSpeed(c);
}

double positiveSourceExclusive(double limitSec, const ClipInfo &c)
{
    const double margin = sourceBoundaryMargin(c);
    return (limitSec > margin) ? (limitSec - margin) : 0.0;
}

double negativeSourceExclusive(double limitSec, const ClipInfo &c)
{
    const double margin = sourceBoundaryMargin(c);
    return (limitSec < -margin) ? (limitSec + margin) : 0.0;
}

} // namespace

bool applyTrim(QVector<ClipInfo> &clips, int index, TrimType type,
               double deltaSec, QString *errorOut)
{
    auto setErr = [&](const QString &msg) {
        if (errorOut) *errorOut = msg;
    };
    if (errorOut) errorOut->clear();

    if (!validIndex(clips, index)) {
        setErr(QObject::tr("トリム対象クリップのインデックスが不正です。"));
        return false;
    }

    switch (type) {
    case TrimType::RippleIn: {
        // clip[i] の左端。inPoint を delta*speed 分 source 空間で動かす。clip[i]
        // のタイムライン開始秒 (leadInSec) は不変のまま実尺が delta 分縮む/伸び、
        // その差分だけ clip[i] 以降がギャップ無しで自動的に deltaSec シフトする
        // (下流の leadInSec は一切触らない — 実尺変化が ripple を生む)。正 delta は
        // inPoint↑ で実尺↓ → 下流が左へ、負 delta は inPoint↓ で実尺↑ → 下流が右へ。
        ClipInfo &c = clips[index];
        const double srcDelta = deltaSec * safeSpeed(c);
        const double newIn = c.inPoint + srcDelta;
        const double oEff = outEff(c);
        if (newIn < -kEps) {
            setErr(QObject::tr("リップル(左)が素材の先頭を超えています。"));
            return false;
        }
        if (newIn >= oEff - kEps) {
            setErr(QObject::tr("リップル(左)でクリップ長が 0 以下になります。"));
            return false;
        }
        c.inPoint = (newIn < 0.0) ? 0.0 : newIn;
        return true;
    }

    case TrimType::RippleOut: {
        // clip[i] の右端。outPoint を delta*speed 分動かす。実尺が変わり、
        // clip[i+1] 以降を deltaSec シフトする (clip[i+1].leadInSec を動かす)。
        ClipInfo &c = clips[index];
        const double srcDelta = deltaSec * safeSpeed(c);
        const double newOut = outEff(c) + srcDelta;
        if (newOut <= c.inPoint + kEps) {
            setErr(QObject::tr("リップル(右)でクリップ長が 0 以下になります。"));
            return false;
        }
        if (newOut > c.duration + kEps) {
            setErr(QObject::tr("リップル(右)が素材の末尾を超えています。"));
            return false;
        }
        // 下流が存在する場合、clip[i+1].leadInSec を deltaSec 動かして ripple。
        if (index + 1 < clips.size()) {
            const double newLeadNext = clips[index + 1].leadInSec + deltaSec;
            if (newLeadNext < -kEps) {
                setErr(QObject::tr("リップル(右)で後続クリップが重なります。"));
                return false;
            }
            clips[index + 1].leadInSec = (newLeadNext < 0.0) ? 0.0 : newLeadNext;
        }
        c.outPoint = (newOut > c.duration) ? c.duration : newOut;
        return true;
    }

    case TrimType::Roll: {
        // clip[i] と clip[i+1] の編集点。clip[i].outPoint と clip[i+1].inPoint を
        // それぞれ delta*speed 分動かす。両クリップの実尺が逆向きに変化し、編集点
        // (clip[i] 終端 = clip[i+1] 始端) が deltaSec 動く。総尺・下流位置は不変。
        if (index + 1 >= clips.size()) {
            setErr(QObject::tr("ロールには次のクリップが必要です。"));
            return false;
        }
        ClipInfo &a = clips[index];
        ClipInfo &b = clips[index + 1];
        const double newOutA = outEff(a) + deltaSec * safeSpeed(a);
        const double newInB  = b.inPoint + deltaSec * safeSpeed(b);
        if (newOutA <= a.inPoint + kEps || newOutA > a.duration + kEps) {
            setErr(QObject::tr("ロールが左クリップの素材範囲を超えています。"));
            return false;
        }
        const double oEffB = outEff(b);
        if (newInB < -kEps || newInB >= oEffB - kEps) {
            setErr(QObject::tr("ロールが右クリップの素材範囲を超えています。"));
            return false;
        }
        a.outPoint = (newOutA > a.duration) ? a.duration : newOutA;
        b.inPoint  = (newInB < 0.0) ? 0.0 : newInB;
        // clip[i+1].leadInSec は不変 (編集点は両クリップの実尺で吸収されるため、
        // a の実尺が +deltaSec、b の実尺が -deltaSec 変わり、b の開始秒は不変)。
        return true;
    }

    case TrimType::Slip: {
        // clip[i] の見せる source 窓を delta 分シフト。inPoint/outPoint を同じ
        // source 量だけ動かす。タイムライン位置・実尺は不変。
        ClipInfo &c = clips[index];
        const double srcDelta = deltaSec * safeSpeed(c);
        const double newIn  = c.inPoint + srcDelta;
        const double newOut = outEff(c) + srcDelta;
        if (newIn < -kEps) {
            setErr(QObject::tr("スリップが素材の先頭を超えています。"));
            return false;
        }
        if (newOut > c.duration + kEps) {
            setErr(QObject::tr("スリップが素材の末尾を超えています。"));
            return false;
        }
        c.inPoint  = (newIn < 0.0) ? 0.0 : newIn;
        c.outPoint = (newOut > c.duration) ? c.duration : newOut;
        return true;
    }

    case TrimType::Slide: {
        // clip[i] をタイムライン上で delta 分移動。source 区間は不変。前の
        // クリップが実尺で吸収 (outPoint を delta*speed(prev) 変更)、次のクリップが
        // 逆に吸収 (inPoint を delta*speed(next) 変更)。端のクリップは leadInSec で
        // 表現する。
        ClipInfo &c = clips[index];
        const bool hasPrev = index - 1 >= 0;
        const bool hasNext = index + 1 < clips.size();

        // --- 前側の吸収を試算 ---------------------------------------------
        // 前クリップがあれば実尺で吸収。無ければ clip[i].leadInSec で吸収。
        double newPrevOut = 0.0;
        double newLeadSelf = c.leadInSec;
        if (hasPrev) {
            ClipInfo &p = clips[index - 1];
            newPrevOut = outEff(p) + deltaSec * safeSpeed(p);
            if (newPrevOut <= p.inPoint + kEps || newPrevOut > p.duration + kEps) {
                setErr(QObject::tr("スライドが前クリップの素材範囲を超えています。"));
                return false;
            }
        } else {
            newLeadSelf = c.leadInSec + deltaSec;
            if (newLeadSelf < -kEps) {
                setErr(QObject::tr("スライドでクリップがタイムライン原点より前に出ます。"));
                return false;
            }
        }

        // --- 次側の吸収を試算 ---------------------------------------------
        // 次クリップがあれば実尺で吸収。無ければ何もしない (末尾は自由に伸びる)。
        double newNextIn = 0.0;
        if (hasNext) {
            ClipInfo &nx = clips[index + 1];
            const double oEffN = outEff(nx);
            newNextIn = nx.inPoint + deltaSec * safeSpeed(nx);
            if (newNextIn < -kEps || newNextIn >= oEffN - kEps) {
                setErr(QObject::tr("スライドが次クリップの素材範囲を超えています。"));
                return false;
            }
        }

        // --- 全境界 OK。確定適用 ------------------------------------------
        if (hasPrev) {
            ClipInfo &p = clips[index - 1];
            p.outPoint = (newPrevOut > p.duration) ? p.duration : newPrevOut;
        } else {
            c.leadInSec = (newLeadSelf < 0.0) ? 0.0 : newLeadSelf;
        }
        if (hasNext) {
            ClipInfo &nx = clips[index + 1];
            nx.inPoint = (newNextIn < 0.0) ? 0.0 : newNextIn;
        }
        return true;
    }
    }

    setErr(QObject::tr("未知のトリム種別です。"));
    return false;
}

double maxTrimDelta(const QVector<ClipInfo> &clips, int index, TrimType type)
{
    if (!validIndex(clips, index))
        return 0.0;

    switch (type) {
    case TrimType::RippleIn: {
        // 正 delta: inPoint↑。上限 = (outEff - inPoint)/speed だが、実尺 0 は
        // applyTrim() が拒否するので source 境界から epsilon 分だけ手前に戻す。
        const ClipInfo &c = clips[index];
        const double headroom = (outEff(c) - c.inPoint) / safeSpeed(c);
        return positiveSourceExclusive(headroom, c);
    }
    case TrimType::RippleOut: {
        // outPoint を増やせる上限 = (duration - outEff)/speed。下流 leadIn は
        // 正方向で増えるので制約にならない。
        const ClipInfo &c = clips[index];
        const double headroom = (c.duration - outEff(c)) / safeSpeed(c);
        return headroom > 0.0 ? headroom : 0.0;
    }
    case TrimType::Roll: {
        if (index + 1 >= clips.size()) return 0.0;
        const ClipInfo &a = clips[index];
        const ClipInfo &b = clips[index + 1];
        // 正 delta: a.outPoint↑ (limit (duration_a - outEff_a)/speed_a)、
        //           b.inPoint↑  (limit (outEff_b - inPoint_b)/speed_b)。
        const double limA = (a.duration - outEff(a)) / safeSpeed(a);
        const double limB = positiveSourceExclusive((outEff(b) - b.inPoint) / safeSpeed(b), b);
        const double lim = (limA < limB) ? limA : limB;
        return lim > 0.0 ? lim : 0.0;
    }
    case TrimType::Slip: {
        // 正 delta: in/out を右へ。limit = (duration - outEff)/speed。
        const ClipInfo &c = clips[index];
        const double lim = (c.duration - outEff(c)) / safeSpeed(c);
        return lim > 0.0 ? lim : 0.0;
    }
    case TrimType::Slide: {
        // 正 delta: prev.outPoint↑ (limit (duration_p - outEff_p)/speed_p、prev
        //           無ければ leadIn↑ で無制限→prev 不在側は INF 扱い)、
        //           next.inPoint↑  (limit (outEff_n - inPoint_n)/speed_n)。
        double lim = std::numeric_limits<double>::infinity();
        if (index - 1 >= 0) {
            const ClipInfo &p = clips[index - 1];
            const double limP = (p.duration - outEff(p)) / safeSpeed(p);
            if (limP < lim) lim = limP;
        }
        if (index + 1 < clips.size()) {
            const ClipInfo &nx = clips[index + 1];
            const double limN = positiveSourceExclusive((outEff(nx) - nx.inPoint) / safeSpeed(nx), nx);
            if (limN < lim) lim = limN;
        }
        // 単独クリップは正方向なら leadInSec を伸ばして動かせるので、上限無し。
        return lim > 0.0 ? lim : 0.0;
    }
    }
    return 0.0;
}

double minTrimDelta(const QVector<ClipInfo> &clips, int index, TrimType type)
{
    if (!validIndex(clips, index))
        return 0.0;

    switch (type) {
    case TrimType::RippleIn: {
        // 負 delta: inPoint↓。下限 = -inPoint/speed (素材の先頭が 0 になる手前)。
        // 下流は右へ伸びるだけなので下流制約は無い。
        const ClipInfo &c = clips[index];
        const double lim = -c.inPoint / safeSpeed(c); // <=0
        return lim < 0.0 ? lim : 0.0;
    }
    case TrimType::RippleOut: {
        // 負 delta: outPoint↓ (limit -(outEff - inPoint)/speed)、かつ下流 leadIn↓
        //           (limit -leadInNext)。
        const ClipInfo &c = clips[index];
        double lim = negativeSourceExclusive(-(outEff(c) - c.inPoint) / safeSpeed(c), c); // <=0
        if (index + 1 < clips.size()) {
            const double limLeadNext = -clips[index + 1].leadInSec; // <=0
            if (limLeadNext > lim) lim = limLeadNext;               // 0 に近い方
        }
        return lim < 0.0 ? lim : 0.0;
    }
    case TrimType::Roll: {
        if (index + 1 >= clips.size()) return 0.0;
        const ClipInfo &a = clips[index];
        const ClipInfo &b = clips[index + 1];
        // 負 delta: a.outPoint↓ (limit -(outEff_a - inPoint_a)/speed_a)、
        //           b.inPoint↓  (limit -inPoint_b/speed_b)。
        const double limA = negativeSourceExclusive(-(outEff(a) - a.inPoint) / safeSpeed(a), a);
        const double limB = -b.inPoint / safeSpeed(b);
        const double lim = (limA > limB) ? limA : limB; // 0 に近い方
        return lim < 0.0 ? lim : 0.0;
    }
    case TrimType::Slip: {
        // 負 delta: in/out を左へ。limit = -inPoint/speed。
        const ClipInfo &c = clips[index];
        const double lim = -c.inPoint / safeSpeed(c);
        return lim < 0.0 ? lim : 0.0;
    }
    case TrimType::Slide: {
        // 負 delta: prev.outPoint↓ (limit -(outEff_p - inPoint_p)/speed_p、prev
        //           無ければ leadIn↓ limit -leadIn)、
        //           next.inPoint↓  (limit -inPoint_n/speed_n)。
        double lim = -std::numeric_limits<double>::infinity();
        if (index - 1 >= 0) {
            const ClipInfo &p = clips[index - 1];
            const double limP = negativeSourceExclusive(-(outEff(p) - p.inPoint) / safeSpeed(p), p);
            if (limP > lim) lim = limP;
        } else {
            const ClipInfo &c = clips[index];
            const double limLead = -c.leadInSec;
            if (limLead > lim) lim = limLead;
        }
        if (index + 1 < clips.size()) {
            const ClipInfo &nx = clips[index + 1];
            const double limN = -nx.inPoint / safeSpeed(nx);
            if (limN > lim) lim = limN;
        }
        if (!std::isfinite(lim)) lim = 0.0;
        return lim < 0.0 ? lim : 0.0;
    }
    }
    return 0.0;
}

} // namespace trimops
