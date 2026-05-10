#include "AdjustmentLayer.h"

#include <QJsonValue>
#include <algorithm>
#include <cmath>

namespace {

// Identity AdjustmentLayer used to detect "this slider was never touched"
// during composition. Kept here (not as a constexpr field) because qint64
// comparisons against a static instance work for value semantics without
// needing constexpr support for QString / QVector.
const AdjustmentLayer kDefaultLayer{};

inline bool nearlyEqual(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) <= eps;
}

QJsonArray doubleArr4ToJson(const double v[4])
{
    QJsonArray a;
    a.append(v[0]);
    a.append(v[1]);
    a.append(v[2]);
    a.append(v[3]);
    return a;
}

void doubleArr4FromJson(const QJsonArray &arr, double v[4])
{
    for (int i = 0; i < 4; ++i)
        v[i] = (i < arr.size()) ? arr[i].toDouble(0.0) : 0.0;
}

QJsonArray curvesToJson(const QVector<QVector<int>> &curves)
{
    QJsonArray outer;
    for (const auto &chan : curves) {
        QJsonArray inner;
        for (int v : chan)
            inner.append(v);
        outer.append(inner);
    }
    return outer;
}

QVector<QVector<int>> curvesFromJson(const QJsonArray &arr)
{
    QVector<QVector<int>> out;
    out.reserve(arr.size());
    for (const auto &val : arr) {
        QJsonArray chan = val.toArray();
        QVector<int> c;
        c.reserve(chan.size());
        for (const auto &v : chan)
            c.append(v.toInt(0));
        out.append(c);
    }
    return out;
}

} // namespace

QJsonObject AdjustmentLayer::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("id")]              = id;
    obj[QStringLiteral("timelineStartUs")] = static_cast<double>(timelineStartUs);
    obj[QStringLiteral("timelineEndUs")]   = static_cast<double>(timelineEndUs);
    obj[QStringLiteral("trackIndex")]      = trackIndex;
    obj[QStringLiteral("name")]            = name;
    obj[QStringLiteral("gradingEnabled")]  = gradingEnabled;
    obj[QStringLiteral("lift")]            = doubleArr4ToJson(lift);
    obj[QStringLiteral("gamma")]           = doubleArr4ToJson(gamma);
    obj[QStringLiteral("gain")]            = doubleArr4ToJson(gain);
    obj[QStringLiteral("rgbCurves")]       = curvesToJson(rgbCurves);
    obj[QStringLiteral("wbTempSlider")]    = wbTempSlider;
    obj[QStringLiteral("wbTintSlider")]    = wbTintSlider;
    obj[QStringLiteral("vigAmount")]       = vigAmount;
    obj[QStringLiteral("vigMidpoint")]     = vigMidpoint;
    obj[QStringLiteral("vigRoundness")]    = vigRoundness;
    obj[QStringLiteral("vigFeather")]      = vigFeather;
    return obj;
}

AdjustmentLayer AdjustmentLayer::fromJson(const QJsonObject &obj)
{
    AdjustmentLayer a;
    a.id              = obj.value(QStringLiteral("id")).toInt(-1);
    a.timelineStartUs = static_cast<qint64>(
        obj.value(QStringLiteral("timelineStartUs")).toDouble(0.0));
    a.timelineEndUs   = static_cast<qint64>(
        obj.value(QStringLiteral("timelineEndUs")).toDouble(0.0));
    a.trackIndex      = obj.value(QStringLiteral("trackIndex")).toInt(0);
    a.name            = obj.value(QStringLiteral("name")).toString();
    a.gradingEnabled  = obj.value(QStringLiteral("gradingEnabled")).toBool(false);
    doubleArr4FromJson(obj.value(QStringLiteral("lift")).toArray(),  a.lift);
    doubleArr4FromJson(obj.value(QStringLiteral("gamma")).toArray(), a.gamma);
    doubleArr4FromJson(obj.value(QStringLiteral("gain")).toArray(),  a.gain);
    a.rgbCurves       = curvesFromJson(
        obj.value(QStringLiteral("rgbCurves")).toArray());
    a.wbTempSlider    = obj.value(QStringLiteral("wbTempSlider")).toDouble(0.0);
    a.wbTintSlider    = obj.value(QStringLiteral("wbTintSlider")).toDouble(0.0);
    a.vigAmount       = obj.value(QStringLiteral("vigAmount")).toDouble(0.0);
    a.vigMidpoint     = obj.value(QStringLiteral("vigMidpoint")).toDouble(0.7);
    a.vigRoundness    = obj.value(QStringLiteral("vigRoundness")).toDouble(0.0);
    a.vigFeather      = obj.value(QStringLiteral("vigFeather")).toDouble(0.3);
    return a;
}

QJsonArray adjustmentLayersToJsonArray(const QVector<AdjustmentLayer> &layers)
{
    QJsonArray arr;
    for (const auto &l : layers)
        arr.append(l.toJson());
    return arr;
}

QVector<AdjustmentLayer> adjustmentLayersFromJsonArray(const QJsonArray &arr)
{
    QVector<AdjustmentLayer> out;
    out.reserve(arr.size());
    for (const auto &v : arr)
        out.append(AdjustmentLayer::fromJson(v.toObject()));
    return out;
}

AdjustmentLayerComposite composeAdjustmentLayersAt(
    const QVector<AdjustmentLayer> &layers,
    qint64 timelineUs)
{
    AdjustmentLayerComposite comp;
    if (layers.isEmpty())
        return comp;

    // Collect layers that cover `timelineUs`.
    QVector<const AdjustmentLayer *> active;
    active.reserve(layers.size());
    for (const auto &l : layers) {
        if (!l.gradingEnabled)
            continue;
        if (timelineUs < l.timelineStartUs || timelineUs >= l.timelineEndUs)
            continue;
        active.append(&l);
    }
    if (active.isEmpty())
        return comp;

    // Stack order: lower trackIndex evaluated first, ties preserved by the
    // input vector (stable_sort). The "topmost" entry is therefore the LAST
    // element after sorting, which is the one whose override-mode values win.
    std::stable_sort(active.begin(), active.end(),
        [](const AdjustmentLayer *a, const AdjustmentLayer *b) {
            return a->trackIndex < b->trackIndex;
        });

    comp.gradingEnabled = true;

    // Additive sliders — sum across the stack.
    for (const auto *l : active) {
        for (int i = 0; i < 4; ++i)
            comp.lift[i] += l->lift[i];
        comp.wbTempSlider += l->wbTempSlider;
        comp.wbTintSlider += l->wbTintSlider;
    }

    // Override sliders — walk top-to-bottom and pick the first non-default
    // value (i.e. the highest-priority layer wins). This mirrors how grading
    // panels expose "the layer that last touched gamma owns gamma".
    for (int i = static_cast<int>(active.size()) - 1; i >= 0; --i) {
        const AdjustmentLayer *l = active[i];

        for (int c = 0; c < 4; ++c) {
            if (!nearlyEqual(l->gamma[c], kDefaultLayer.gamma[c])
                && nearlyEqual(comp.gamma[c], kDefaultLayer.gamma[c])) {
                comp.gamma[c] = l->gamma[c];
            }
            if (!nearlyEqual(l->gain[c], kDefaultLayer.gain[c])
                && nearlyEqual(comp.gain[c], kDefaultLayer.gain[c])) {
                comp.gain[c] = l->gain[c];
            }
        }

        if (!l->rgbCurves.isEmpty() && comp.rgbCurves.isEmpty())
            comp.rgbCurves = l->rgbCurves;

        if (!nearlyEqual(l->vigAmount, kDefaultLayer.vigAmount)
            && nearlyEqual(comp.vigAmount, kDefaultLayer.vigAmount)) {
            comp.vigAmount    = l->vigAmount;
            comp.vigMidpoint  = l->vigMidpoint;
            comp.vigRoundness = l->vigRoundness;
            comp.vigFeather   = l->vigFeather;
        }
    }

    return comp;
}
