// ---------------------------------------------------------------------------
// TimeRemap.cpp — implementation of timeremap::TimeRemapCurve + resolveFrame
// ---------------------------------------------------------------------------

#include "TimeRemap.h"
#include "OpticalFlow.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QImage>

#include <algorithm>
#include <cmath>

namespace timeremap {

// ---------------------------------------------------------------------------
// TimeRemapCurve::addKey
// Inserts keeping keys sorted ascending by outTime.
// ---------------------------------------------------------------------------
void TimeRemapCurve::addKey(double outT, double srcT)
{
    TimeRemapKey key;
    key.outTime = outT;
    key.srcTime = srcT;

    // Find insertion point (lower_bound on outTime)
    auto it = std::lower_bound(keys.begin(), keys.end(), key,
        [](const TimeRemapKey& a, const TimeRemapKey& b) {
            return a.outTime < b.outTime;
        });
    keys.insert(it, key);
}

// ---------------------------------------------------------------------------
// TimeRemapCurve::srcTimeAt
//
// 0 keys  → identity (return outTime unchanged)
// 1 key   → constant (return that key's srcTime regardless of outTime)
// ≥2 keys → piecewise-linear; clamps to first/last srcTime outside key range
// ---------------------------------------------------------------------------
double TimeRemapCurve::srcTimeAt(double outTime) const
{
    if (keys.isEmpty()) {
        return outTime; // identity
    }
    if (keys.size() == 1) {
        return keys.first().srcTime; // constant
    }

    // Clamp before first key
    if (outTime <= keys.first().outTime) {
        return keys.first().srcTime;
    }
    // Clamp after last key
    if (outTime >= keys.last().outTime) {
        return keys.last().srcTime;
    }

    // Binary search for the bracketing segment [lo, hi)
    // We want the largest index i such that keys[i].outTime <= outTime
    int lo = 0;
    int hi = static_cast<int>(keys.size()) - 1;
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if (keys[mid].outTime <= outTime) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    const TimeRemapKey& k0 = keys[lo];
    const TimeRemapKey& k1 = keys[hi];

    double span = k1.outTime - k0.outTime;
    if (span <= 0.0) {
        return k0.srcTime;
    }
    double t = (outTime - k0.outTime) / span;
    return k0.srcTime + t * (k1.srcTime - k0.srcTime);
}

// ---------------------------------------------------------------------------
// TimeRemapCurve::toJson / fromJson
// ---------------------------------------------------------------------------
QJsonObject TimeRemapCurve::toJson() const
{
    QJsonArray arr;
    for (const TimeRemapKey& k : keys) {
        QJsonObject kobj;
        kobj[QStringLiteral("outTime")] = k.outTime;
        kobj[QStringLiteral("srcTime")] = k.srcTime;
        arr.append(kobj);
    }

    QJsonObject obj;
    obj[QStringLiteral("keys")]      = arr;
    obj[QStringLiteral("blendMode")] = static_cast<int>(blendMode);
    obj[QStringLiteral("sourceFps")] = sourceFps;
    return obj;
}

TimeRemapCurve TimeRemapCurve::fromJson(const QJsonObject& obj)
{
    TimeRemapCurve curve;

    curve.sourceFps = obj.value(QStringLiteral("sourceFps")).toDouble(30.0);
    curve.blendMode = static_cast<FrameBlendMode>(
        obj.value(QStringLiteral("blendMode")).toInt(0));

    QJsonArray arr = obj.value(QStringLiteral("keys")).toArray();
    curve.keys.reserve(static_cast<int>(arr.size()));
    for (const auto& val : arr) {
        QJsonObject kobj = val.toObject();
        TimeRemapKey k;
        k.outTime = kobj.value(QStringLiteral("outTime")).toDouble(0.0);
        k.srcTime = kobj.value(QStringLiteral("srcTime")).toDouble(0.0);
        curve.keys.append(k);
    }
    return curve;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Returns a 1×1 fully transparent image used as a safe fallback.
static QImage transparentFallback()
{
    QImage img(1, 1, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

// Per-pixel linear blend: out = a*(1-frac) + b*frac, ARGB32.
static QImage blendImages(const QImage& a, const QImage& b, double frac)
{
    if (frac <= 0.0) return a;
    if (frac >= 1.0) return b;

    QImage imgA = a.convertToFormat(QImage::Format_ARGB32);
    QImage imgB = b.convertToFormat(QImage::Format_ARGB32);

    // If sizes differ, scale b to a's size for a simple blend
    if (imgA.size() != imgB.size()) {
        imgB = imgB.scaled(imgA.size(), Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation);
    }

    const int w = imgA.width();
    const int h = imgA.height();

    QImage out(w, h, QImage::Format_ARGB32);

    const int fracI = static_cast<int>(frac * 256.0 + 0.5); // 0..256
    const int invI  = 256 - fracI;

    for (int y = 0; y < h; ++y) {
        const QRgb* rowA = reinterpret_cast<const QRgb*>(imgA.constScanLine(y));
        const QRgb* rowB = reinterpret_cast<const QRgb*>(imgB.constScanLine(y));
        QRgb*       rowO = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            QRgb pa = rowA[x];
            QRgb pb = rowB[x];
            int r = (qRed(pa)   * invI + qRed(pb)   * fracI) >> 8;
            int g = (qGreen(pa) * invI + qGreen(pb) * fracI) >> 8;
            int bl= (qBlue(pa)  * invI + qBlue(pb)  * fracI) >> 8;
            int al= (qAlpha(pa) * invI + qAlpha(pb) * fracI) >> 8;
            rowO[x] = qRgba(r, g, bl, al);
        }
    }
    return out;
}

// Clamp fractional frame index to a non-negative value and return floor/ceil.
static int safeFloor(double f)
{
    double cf = std::max(0.0, f);
    return static_cast<int>(std::floor(cf));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// resolveFrame
// ---------------------------------------------------------------------------
QImage resolveFrame(const TimeRemapCurve&                      curve,
                    double                                     outTime,
                    const std::function<QImage(int srcFrameIndex)>& fetchFrame)
{
    const double st = curve.srcTimeAt(outTime);
    const double f  = st * curve.sourceFps;

    switch (curve.blendMode) {
    case FrameBlendMode::NearestFrame: {
        int idx = static_cast<int>(std::round(std::max(0.0, f)));
        QImage img = fetchFrame(idx);
        if (img.isNull()) {
            // Try nearest neighbours
            if (idx > 0) {
                img = fetchFrame(idx - 1);
            }
            if (img.isNull() && idx >= 0) {
                img = fetchFrame(idx + 1);
            }
        }
        return img.isNull() ? transparentFallback() : img;
    }

    case FrameBlendMode::Blend: {
        int    f0   = safeFloor(f);
        int    f1   = f0 + 1;
        double frac = f - static_cast<double>(f0);

        if (frac <= 0.0) {
            QImage img = fetchFrame(f0);
            return img.isNull() ? transparentFallback() : img;
        }

        QImage a = fetchFrame(f0);
        QImage b = fetchFrame(f1);

        if (a.isNull() && b.isNull()) return transparentFallback();
        if (a.isNull()) return b;
        if (b.isNull()) return a;

        return blendImages(a, b, frac);
    }

    case FrameBlendMode::OpticalFlow: {
        int    f0   = safeFloor(f);
        int    f1   = f0 + 1;
        double frac = f - static_cast<double>(f0);

        if (frac <= 0.0) {
            QImage img = fetchFrame(f0);
            return img.isNull() ? transparentFallback() : img;
        }

        QImage a = fetchFrame(f0);
        QImage b = fetchFrame(f1);

        if (a.isNull() && b.isNull()) return transparentFallback();
        if (a.isNull()) return b.isNull() ? transparentFallback() : b;
        if (b.isNull()) return a;

        opticalflow::FlowField flow =
            opticalflow::estimateFlow(a, b, opticalflow::FlowParams{});

        QImage warped = opticalflow::warpImage(a, flow, frac);
        // warpImage returns null on empty src or mismatched flow — fall back
        return warped.isNull() ? blendImages(a, b, frac) : warped;
    }
    } // switch

    // Should be unreachable; defensive fallback
    return transparentFallback();
}

} // namespace timeremap
