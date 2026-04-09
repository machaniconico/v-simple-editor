#include "EffectPlugin.h"
#include <QPainter>
#include <QtMath>
#include <algorithm>
#include <cmath>

static inline int clamp255(int v) { return qBound(0, v, 255); }

// --- Plugin Registry ---

PluginRegistry &PluginRegistry::instance()
{
    static PluginRegistry reg;
    return reg;
}

PluginRegistry::PluginRegistry()
{
    registerBuiltins();
}

void PluginRegistry::registerBuiltins()
{
    registerPlugin(std::make_shared<GlowPlugin>());
    registerPlugin(std::make_shared<EmbossPlugin>());
    registerPlugin(std::make_shared<PosterizePlugin>());
    registerPlugin(std::make_shared<EdgeDetectPlugin>());
    registerPlugin(std::make_shared<ColorShiftPlugin>());
}

void PluginRegistry::registerPlugin(std::shared_ptr<EffectPlugin> plugin)
{
    // Replace if exists
    for (auto &p : m_plugins) {
        if (p->name() == plugin->name()) {
            p = plugin;
            return;
        }
    }
    m_plugins.append(plugin);
}

void PluginRegistry::unregisterPlugin(const QString &name)
{
    m_plugins.erase(
        std::remove_if(m_plugins.begin(), m_plugins.end(),
            [&](const std::shared_ptr<EffectPlugin> &p) { return p->name() == name; }),
        m_plugins.end());
}

QVector<std::shared_ptr<EffectPlugin>> PluginRegistry::allPlugins() const
{
    return m_plugins;
}

QVector<std::shared_ptr<EffectPlugin>> PluginRegistry::pluginsByCategory(const QString &category) const
{
    QVector<std::shared_ptr<EffectPlugin>> result;
    for (const auto &p : m_plugins)
        if (p->category() == category) result.append(p);
    return result;
}

std::shared_ptr<EffectPlugin> PluginRegistry::findByName(const QString &name) const
{
    for (const auto &p : m_plugins)
        if (p->name() == name) return p;
    return nullptr;
}

QStringList PluginRegistry::categories() const
{
    QStringList cats;
    for (const auto &p : m_plugins)
        if (!cats.contains(p->category())) cats.append(p->category());
    return cats;
}

// ===== Built-in Plugin Implementations =====

// --- Glow ---
QImage GlowPlugin::process(const QImage &input, const QVector<double> &params) const
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    int radius = (params.size() > 0) ? qMax(1, static_cast<int>(params[0])) : 10;
    double intensity = (params.size() > 1) ? params[1] : 0.5;

    int w = img.width(), h = img.height();
    QImage blurred = img.copy();

    // Simple box blur for glow base
    for (int pass = 0; pass < 2; ++pass) {
        QImage tmp = blurred.copy();
        for (int y = 0; y < h; ++y) {
            const uint8_t *src = tmp.constScanLine(y);
            uint8_t *dst = blurred.scanLine(y);
            for (int x = 0; x < w; ++x) {
                int rS = 0, gS = 0, bS = 0, cnt = 0;
                for (int kx = -radius; kx <= radius; ++kx) {
                    int sx = qBound(0, x + kx, w - 1);
                    rS += src[sx * 3]; gS += src[sx * 3 + 1]; bS += src[sx * 3 + 2]; ++cnt;
                }
                dst[x*3] = rS/cnt; dst[x*3+1] = gS/cnt; dst[x*3+2] = bS/cnt;
            }
        }
    }

    // Screen blend: result = 1 - (1 - a) * (1 - b)
    for (int y = 0; y < h; ++y) {
        uint8_t *orig = img.scanLine(y);
        const uint8_t *glow = blurred.constScanLine(y);
        for (int x = 0; x < w * 3; ++x) {
            double a = orig[x] / 255.0;
            double b = glow[x] / 255.0 * intensity;
            double screen = 1.0 - (1.0 - a) * (1.0 - b);
            orig[x] = clamp255(static_cast<int>(screen * 255.0));
        }
    }
    return img;
}

// --- Emboss ---
QImage EmbossPlugin::process(const QImage &input, const QVector<double> &params) const
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    QImage result = img.copy();
    double strength = (params.size() > 0) ? params[0] : 1.0;
    int w = img.width(), h = img.height();

    for (int y = 1; y < h - 1; ++y) {
        const uint8_t *prev = img.constScanLine(y - 1);
        const uint8_t *next = img.constScanLine(y + 1);
        uint8_t *dst = result.scanLine(y);
        for (int x = 1; x < w - 1; ++x) {
            for (int c = 0; c < 3; ++c) {
                // Emboss kernel: top-left minus bottom-right + 128
                int val = static_cast<int>(
                    (next[(x+1)*3+c] - prev[(x-1)*3+c]) * strength + 128);
                dst[x*3+c] = clamp255(val);
            }
        }
    }
    return result;
}

// --- Posterize ---
QImage PosterizePlugin::process(const QImage &input, const QVector<double> &params) const
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    int levels = (params.size() > 0) ? qMax(2, static_cast<int>(params[0])) : 4;
    double step = 255.0 / (levels - 1);

    uint8_t lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = clamp255(static_cast<int>(std::round(i / step) * step));

    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width() * 3; ++x)
            line[x] = lut[line[x]];
    }
    return img;
}

// --- Edge Detect (Sobel) ---
QImage EdgeDetectPlugin::process(const QImage &input, const QVector<double> &params) const
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    QImage result(img.width(), img.height(), QImage::Format_RGB888);
    result.fill(Qt::black);
    double threshold = (params.size() > 0) ? params[0] : 30.0;
    int w = img.width(), h = img.height();

    for (int y = 1; y < h - 1; ++y) {
        const uint8_t *p = img.constScanLine(y - 1);
        const uint8_t *c = img.constScanLine(y);
        const uint8_t *n = img.constScanLine(y + 1);
        uint8_t *dst = result.scanLine(y);

        for (int x = 1; x < w - 1; ++x) {
            for (int ch = 0; ch < 3; ++ch) {
                // Sobel X
                int gx = -p[(x-1)*3+ch] + p[(x+1)*3+ch]
                       - 2*c[(x-1)*3+ch] + 2*c[(x+1)*3+ch]
                       - n[(x-1)*3+ch] + n[(x+1)*3+ch];
                // Sobel Y
                int gy = -p[(x-1)*3+ch] - 2*p[x*3+ch] - p[(x+1)*3+ch]
                       + n[(x-1)*3+ch] + 2*n[x*3+ch] + n[(x+1)*3+ch];
                double mag = std::sqrt(gx * gx + gy * gy);
                dst[x*3+ch] = (mag > threshold) ? clamp255(static_cast<int>(mag)) : 0;
            }
        }
    }
    return result;
}

// --- Color Shift ---
QImage ColorShiftPlugin::process(const QImage &input, const QVector<double> &params) const
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    int rShift = (params.size() > 0) ? static_cast<int>(params[0]) : 0;
    int gShift = (params.size() > 1) ? static_cast<int>(params[1]) : 0;
    int bShift = (params.size() > 2) ? static_cast<int>(params[2]) : 0;

    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            line[x*3]     = clamp255(line[x*3]     + rShift);
            line[x*3 + 1] = clamp255(line[x*3 + 1] + gShift);
            line[x*3 + 2] = clamp255(line[x*3 + 2] + bShift);
        }
    }
    return img;
}
