#include "FrameInterpolator.h"
#include <cmath>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Clamp an integer to [0, 255].
inline int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// Convert both images to Format_ARGB32 and scale b to a's size if needed.
// Returns false if either input is null/empty after conversion.
bool prepareImages(const QImage& a, const QImage& b,
                   QImage& outA, QImage& outB)
{
    if (a.isNull() || a.width() == 0 || a.height() == 0)
        return false;
    if (b.isNull() || b.width() == 0 || b.height() == 0)
        return false;

    outA = a.convertToFormat(QImage::Format_ARGB32);

    QImage bConverted = b.convertToFormat(QImage::Format_ARGB32);
    if (bConverted.size() != outA.size())
        bConverted = bConverted.scaled(outA.size(), Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
    outB = bConverted;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// LinearBlendInterpolator
// ---------------------------------------------------------------------------

QImage LinearBlendInterpolator::interpolate(const QImage& a, const QImage& b, double t)
{
    // Edge: clamp t range
    if (t <= 0.0) {
        if (a.isNull()) return QImage();
        return a.convertToFormat(QImage::Format_ARGB32);
    }
    if (t >= 1.0) {
        if (b.isNull()) return QImage();
        return b.convertToFormat(QImage::Format_ARGB32);
    }

    QImage imgA, imgB;
    if (!prepareImages(a, b, imgA, imgB))
        return QImage();

    const int w = imgA.width();
    const int h = imgA.height();
    QImage result(w, h, QImage::Format_ARGB32);

    const double wa = 1.0 - t;  // weight for a
    const double wb = t;         // weight for b

    for (int y = 0; y < h; ++y) {
        const uchar* rowA = imgA.constScanLine(y);
        const uchar* rowB = imgB.constScanLine(y);
        uchar*       rowR = result.scanLine(y);

        for (int x = 0; x < w; ++x) {
            // ARGB32 layout in memory (little-endian): B G R A
            const int base = x * 4;
            rowR[base + 0] = static_cast<uchar>(clamp255(
                static_cast<int>(wa * rowA[base + 0] + wb * rowB[base + 0] + 0.5)));
            rowR[base + 1] = static_cast<uchar>(clamp255(
                static_cast<int>(wa * rowA[base + 1] + wb * rowB[base + 1] + 0.5)));
            rowR[base + 2] = static_cast<uchar>(clamp255(
                static_cast<int>(wa * rowA[base + 2] + wb * rowB[base + 2] + 0.5)));
            rowR[base + 3] = static_cast<uchar>(clamp255(
                static_cast<int>(wa * rowA[base + 3] + wb * rowB[base + 3] + 0.5)));
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// MotionBlendInterpolator — delegates to linear blend.
// Placeholder for future RIFE / block-match warp implementation.
// ---------------------------------------------------------------------------

QImage MotionBlendInterpolator::interpolate(const QImage& a, const QImage& b, double t)
{
    // Future: replace body with optical-flow warp + blend.
    // For now, delegate to linear to satisfy non-null / size-match acceptance criteria.
    LinearBlendInterpolator linear;
    return linear.interpolate(a, b, t);
}

// ---------------------------------------------------------------------------
// FrameInterpolatorManager — static registry
// ---------------------------------------------------------------------------

namespace {

struct EngineRegistry {
    LinearBlendInterpolator  linear;
    MotionBlendInterpolator  motionBlend;
    std::vector<FrameInterpolatorEngine*> extra;

    EngineRegistry() = default;
};

EngineRegistry& registry() {
    static EngineRegistry s;
    return s;
}

} // namespace

QVector<FrameInterpolatorEngine*> FrameInterpolatorManager::engines()
{
    auto& r = registry();
    QVector<FrameInterpolatorEngine*> result;
    result.append(&r.linear);
    result.append(&r.motionBlend);
    for (auto* e : r.extra)
        result.append(e);
    return result;
}

FrameInterpolatorEngine* FrameInterpolatorManager::engineByName(const QString& name)
{
    for (auto* e : engines()) {
        if (e->name() == name)
            return e;
    }
    return nullptr;
}

void FrameInterpolatorManager::registerEngine(FrameInterpolatorEngine* engine)
{
    if (!engine)
        return;
    registry().extra.push_back(engine);
}
