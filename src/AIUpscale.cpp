#include "AIUpscale.h"

#ifndef _USE_MATH_DEFINES
#  define _USE_MATH_DEFINES
#endif
#include <cmath>
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif
#include <vector>
#include <algorithm>

// ─────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────

static inline double sinc(double x)
{
    if (x == 0.0) return 1.0;
    const double pi_x = M_PI * x;
    return std::sin(pi_x) / pi_x;
}

// Lanczos kernel: a = 3 lobes
static inline double lanczos3(double x)
{
    const double a = 3.0;
    const double ax = std::abs(x);
    if (ax < 1e-9) return 1.0;
    if (ax >= a)   return 0.0;
    return sinc(ax) * sinc(ax / a);
}

// Cubic kernel (Catmull-Rom, a = -0.5)
static inline double cubic(double t)
{
    const double a  = -0.5;
    const double at = std::abs(t);
    if (at < 1.0)
        return (a + 2.0) * at * at * at - (a + 3.0) * at * at + 1.0;
    if (at < 2.0)
        return a * at * at * at - 5.0 * a * at * at + 8.0 * a * at - 4.0 * a;
    return 0.0;
}

// Clamp an integer to [0, max]
static inline int clampI(int v, int lo, int hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

// Clamp a double to [0, 255] and round to int
static inline int clamp255(double v)
{
    return (v < 0.0) ? 0 : (v > 255.0) ? 255 : static_cast<int>(v + 0.5);
}

// ─────────────────────────────────────────────────────────────
// LanczosUpscaler
// ─────────────────────────────────────────────────────────────

QImage LanczosUpscaler::upscale(const QImage& src, int scale)
{
    if (scale == 1)
        return src.copy();
    if (scale <= 0 || scale > 4)
        return src.copy();

    const QImage in = src.convertToFormat(QImage::Format_ARGB32);
    const int srcW  = in.width();
    const int srcH  = in.height();
    const int dstW  = srcW * scale;
    const int dstH  = srcH * scale;

    QImage out(dstW, dstH, QImage::Format_ARGB32);

    // Pre-compute per-column horizontal weights (reused across all rows)
    // For each destination x: source centre sx, 6 taps (a=3)
    const int taps = 6; // 2*a
    struct Tap { int idx; double w; };
    std::vector<std::vector<Tap>> hTaps(static_cast<size_t>(dstW));
    for (int dx = 0; dx < dstW; ++dx) {
        const double sx = (dx + 0.5) / scale - 0.5;
        const int    x0 = static_cast<int>(std::floor(sx)) - 2; // first of 6
        std::vector<Tap>& row = hTaps[static_cast<size_t>(dx)];
        row.reserve(taps);
        double sum = 0.0;
        for (int k = 0; k < taps; ++k) {
            const int xi = x0 + k;
            const double w = lanczos3(sx - xi);
            row.push_back({ clampI(xi, 0, srcW - 1), w });
            sum += w;
        }
        if (sum != 0.0)
            for (auto& t : row) t.w /= sum;
    }

    std::vector<std::vector<Tap>> vTaps(static_cast<size_t>(dstH));
    for (int dy = 0; dy < dstH; ++dy) {
        const double sy = (dy + 0.5) / scale - 0.5;
        const int    y0 = static_cast<int>(std::floor(sy)) - 2;
        std::vector<Tap>& col = vTaps[static_cast<size_t>(dy)];
        col.reserve(taps);
        double sum = 0.0;
        for (int k = 0; k < taps; ++k) {
            const int yi = y0 + k;
            const double w = lanczos3(sy - yi);
            col.push_back({ clampI(yi, 0, srcH - 1), w });
            sum += w;
        }
        if (sum != 0.0)
            for (auto& t : col) t.w /= sum;
    }

    // Separable two-pass: horizontal into temp, then vertical into out
    // temp: srcH rows × dstW cols, stored as ARGB doubles (4 channels)
    const int tempSize = srcH * dstW * 4;
    std::vector<double> temp(static_cast<size_t>(tempSize), 0.0);

    for (int sy = 0; sy < srcH; ++sy) {
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(in.constScanLine(sy));
        double* dstLine = temp.data() + sy * dstW * 4;
        for (int dx = 0; dx < dstW; ++dx) {
            double r = 0, g = 0, b = 0, a = 0;
            for (const auto& t : hTaps[static_cast<size_t>(dx)]) {
                const QRgb px = srcLine[t.idx];
                r += qRed(px)   * t.w;
                g += qGreen(px) * t.w;
                b += qBlue(px)  * t.w;
                a += qAlpha(px) * t.w;
            }
            dstLine[dx * 4 + 0] = r;
            dstLine[dx * 4 + 1] = g;
            dstLine[dx * 4 + 2] = b;
            dstLine[dx * 4 + 3] = a;
        }
    }

    for (int dy = 0; dy < dstH; ++dy) {
        QRgb* outLine = reinterpret_cast<QRgb*>(out.scanLine(dy));
        for (int dx = 0; dx < dstW; ++dx) {
            double r = 0, g = 0, b = 0, a = 0;
            for (const auto& t : vTaps[static_cast<size_t>(dy)]) {
                const double* px = temp.data() + t.idx * dstW * 4 + dx * 4;
                r += px[0] * t.w;
                g += px[1] * t.w;
                b += px[2] * t.w;
                a += px[3] * t.w;
            }
            outLine[dx] = qRgba(clamp255(r), clamp255(g), clamp255(b), clamp255(a));
        }
    }

    return out;
}

// ─────────────────────────────────────────────────────────────
// BicubicUpscaler
// ─────────────────────────────────────────────────────────────

QImage BicubicUpscaler::upscale(const QImage& src, int scale)
{
    if (scale == 1)
        return src.copy();
    if (scale <= 0 || scale > 4)
        return src.copy();

    const QImage in = src.convertToFormat(QImage::Format_ARGB32);
    const int srcW  = in.width();
    const int srcH  = in.height();
    const int dstW  = srcW * scale;
    const int dstH  = srcH * scale;

    QImage out(dstW, dstH, QImage::Format_ARGB32);

    // Catmull-Rom: 4-tap separable
    struct Tap { int idx; double w; };

    auto makeTaps = [](double s, int maxIdx) -> std::array<Tap, 4> {
        const int    x0 = static_cast<int>(std::floor(s)) - 1;
        std::array<Tap, 4> t{};
        double sum = 0.0;
        for (int k = 0; k < 4; ++k) {
            const int xi = x0 + k;
            const double w = cubic(s - xi);
            t[static_cast<size_t>(k)] = { clampI(xi, 0, maxIdx), w };
            sum += w;
        }
        if (sum != 0.0)
            for (auto& tap : t) tap.w /= sum;
        return t;
    };

    // Separable two-pass via temp buffer
    const int tempSize = srcH * dstW * 4;
    std::vector<double> temp(static_cast<size_t>(tempSize), 0.0);

    for (int sy = 0; sy < srcH; ++sy) {
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(in.constScanLine(sy));
        double* dstLine = temp.data() + sy * dstW * 4;
        for (int dx = 0; dx < dstW; ++dx) {
            const double sx = (dx + 0.5) / scale - 0.5;
            auto taps = makeTaps(sx, srcW - 1);
            double r = 0, g = 0, b = 0, a = 0;
            for (const auto& t : taps) {
                const QRgb px = srcLine[t.idx];
                r += qRed(px)   * t.w;
                g += qGreen(px) * t.w;
                b += qBlue(px)  * t.w;
                a += qAlpha(px) * t.w;
            }
            dstLine[dx * 4 + 0] = r;
            dstLine[dx * 4 + 1] = g;
            dstLine[dx * 4 + 2] = b;
            dstLine[dx * 4 + 3] = a;
        }
    }

    for (int dy = 0; dy < dstH; ++dy) {
        const double sy = (dy + 0.5) / scale - 0.5;
        auto vTaps = makeTaps(sy, srcH - 1);
        QRgb* outLine = reinterpret_cast<QRgb*>(out.scanLine(dy));
        for (int dx = 0; dx < dstW; ++dx) {
            double r = 0, g = 0, b = 0, a = 0;
            for (const auto& t : vTaps) {
                const double* px = temp.data() + t.idx * dstW * 4 + dx * 4;
                r += px[0] * t.w;
                g += px[1] * t.w;
                b += px[2] * t.w;
                a += px[3] * t.w;
            }
            outLine[dx] = qRgba(clamp255(r), clamp255(g), clamp255(b), clamp255(a));
        }
    }

    return out;
}

// ─────────────────────────────────────────────────────────────
// AIUpscaleManager
// ─────────────────────────────────────────────────────────────

namespace {
    LanczosUpscaler g_lanczos;
    BicubicUpscaler g_bicubic;
    std::vector<AIUpscaleEngine*> g_registered;
} // namespace

QVector<AIUpscaleEngine*> AIUpscaleManager::engines()
{
    QVector<AIUpscaleEngine*> all;
    all.append(&g_lanczos);
    all.append(&g_bicubic);
    for (AIUpscaleEngine* e : g_registered)
        all.append(e);
    return all;
}

AIUpscaleEngine* AIUpscaleManager::engineByName(const QString& name)
{
    for (AIUpscaleEngine* e : engines()) {
        if (e->name() == name)
            return e;
    }
    return nullptr;
}

void AIUpscaleManager::registerEngine(AIUpscaleEngine* engine)
{
    if (engine)
        g_registered.push_back(engine);
}
