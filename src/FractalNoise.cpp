#include "FractalNoise.h"

#include <QVector>
#include <cmath>
#include <cstdint>

namespace FractalNoise
{

// ---------------------------------------------------------------------------
//  Internal: 3-D gradient noise (Perlin-style)
// ---------------------------------------------------------------------------

namespace Detail
{

// SplitMix64 — fast, good-enough PRNG for shuffling the permutation table.
static uint64_t splitMix64(uint64_t &state)
{
    state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// 12 gradient directions (cube edges) used by classic Perlin noise.
static const int GRAD3[12][3] = {
    { 1, 1, 0}, {-1, 1, 0}, { 1,-1, 0}, {-1,-1, 0},
    { 1, 0, 1}, {-1, 0, 1}, { 1, 0,-1}, {-1, 0,-1},
    { 0, 1, 1}, { 0,-1, 1}, { 0, 1,-1}, { 0,-1,-1}
};

// Permutation table (256 entries, duplicated to 512 for overflow safety).
struct PermTable {
    int p[512];
};

static PermTable buildPermTable(unsigned seed)
{
    PermTable tbl;
    uint64_t state = seed;

    // Identity
    for (int i = 0; i < 256; ++i)
        tbl.p[i] = i;

    // Fisher-Yates shuffle
    for (int i = 255; i > 0; --i) {
        int j = static_cast<int>(splitMix64(state) % (i + 1));
        int tmp = tbl.p[i];
        tbl.p[i] = tbl.p[j];
        tbl.p[j] = tmp;
    }
    // Duplicate for overflow
    for (int i = 0; i < 256; ++i)
        tbl.p[256 + i] = tbl.p[i];

    return tbl;
}

static double dot3(const int g[3], double x, double y, double z)
{
    return g[0] * x + g[1] * y + g[2] * z;
}

// Quintic fade curve: 6t^5 - 15t^4 + 10t^3
static double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

static double lerp(double a, double b, double t) { return a + t * (b - a); }

// 3-D Perlin gradient noise, returns value in roughly [-1, 1].
static double noise3D(double x, double y, double z, const PermTable &perm)
{
    // Find unit cube containing the point
    int xi = static_cast<int>(std::floor(x)) & 255;
    int yi = static_cast<int>(std::floor(y)) & 255;
    int zi = static_cast<int>(std::floor(z)) & 255;

    // Relative position within cube
    double xf = x - std::floor(x);
    double yf = y - std::floor(y);
    double zf = z - std::floor(z);

    // Compute fade curves
    double u = fade(xf);
    double v = fade(yf);
    double w = fade(zf);

    const int *p = perm.p;

    // Hash coordinates of cube corners
    int aaa = p[p[p[xi]     + yi]     + zi];
    int aba = p[p[p[xi]     + yi + 1] + zi];
    int aab = p[p[p[xi]     + yi]     + zi + 1];
    int abb = p[p[p[xi]     + yi + 1] + zi + 1];
    int baa = p[p[p[xi + 1] + yi]     + zi];
    int bba = p[p[p[xi + 1] + yi + 1] + zi];
    int bab = p[p[p[xi + 1] + yi]     + zi + 1];
    int bbb = p[p[p[xi + 1] + yi + 1] + zi + 1];

    // Dot product of gradient and distance vector
    double x1 = lerp(dot3(GRAD3[aaa % 12], xf,     yf,     zf),
                     dot3(GRAD3[baa % 12], xf - 1.0, yf,     zf), u);
    double x2 = lerp(dot3(GRAD3[aba % 12], xf,     yf - 1.0, zf),
                     dot3(GRAD3[bba % 12], xf - 1.0, yf - 1.0, zf), u);
    double y1 = lerp(x1, x2, v);

    x1 = lerp(dot3(GRAD3[aab % 12], xf,     yf,     zf - 1.0),
              dot3(GRAD3[bab % 12], xf - 1.0, yf,     zf - 1.0), u);
    x2 = lerp(dot3(GRAD3[abb % 12], xf,     yf - 1.0, zf - 1.0),
              dot3(GRAD3[bbb % 12], xf - 1.0, yf - 1.0, zf - 1.0), u);
    double y2 = lerp(x1, x2, v);

    return lerp(y1, y2, w);  // result in [-1, 1]
}

} // namespace Detail

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

static double sampleWithPerm(double x, double y, double evolution, const Params &p,
                             const Detail::PermTable &perm)
{
    double freq = p.frequency;
    double amp  = 1.0;
    double maxVal = 0.0;
    double value = 0.0;

    for (int i = 0; i < p.octaves; ++i) {
        double nx = x * freq;
        double ny = y * freq;
        double nz = evolution * freq * 0.25;  // slower evolution axis

        double n = Detail::noise3D(nx, ny, nz, perm);

        switch (p.kind) {
        case FractalKind::FBm:
            value += n * amp;
            maxVal += amp;
            break;
        case FractalKind::Turbulence:
            value += std::abs(n) * amp;
            maxVal += amp;
            break;
        case FractalKind::Ridged:
            n = 1.0 - std::abs(n);
            value += n * n * amp;
            maxVal += amp;
            break;
        }

        freq *= p.lacunarity;
        amp  *= p.gain;
    }

    // Normalise to [0, 1]
    if (maxVal > 0.0)
        value /= maxVal;

    return value;
}

double sample(double x, double y, double evolution, const Params &p)
{
    const Detail::PermTable perm = Detail::buildPermTable(p.seed);
    return sampleWithPerm(x, y, evolution, p, perm);
}

QImage render(const QSize &size, double evolution, const Params &p, bool grayscale)
{
    if (size.isEmpty())
        return QImage();

    QImage::Format fmt = grayscale ? QImage::Format_Grayscale8 : QImage::Format_ARGB32;
    QImage img(size, fmt);

    const int w = size.width();
    const int h = size.height();
    const Detail::PermTable perm = Detail::buildPermTable(p.seed);

    if (grayscale) {
        for (int y = 0; y < h; ++y) {
            quint8 *scan = img.scanLine(y);
            for (int x = 0; x < w; ++x) {
                double nx = static_cast<double>(x) / static_cast<double>(w);
                double ny = static_cast<double>(y) / static_cast<double>(h);
                double v = sampleWithPerm(nx, ny, evolution, p, perm);
                // Clamp to [0,1] then quantise — matches sample()'s pre-quantization value
                if (v < 0.0) v = 0.0;
                if (v > 1.0) v = 1.0;
                scan[x] = static_cast<quint8>(std::round(v * 255.0));
            }
        }
    } else {
        int lr = p.lowColor.red();
        int lg = p.lowColor.green();
        int lb = p.lowColor.blue();
        int hr = p.highColor.red();
        int hg = p.highColor.green();
        int hb = p.highColor.blue();

        for (int y = 0; y < h; ++y) {
            auto *scan = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                double nx = static_cast<double>(x) / static_cast<double>(w);
                double ny = static_cast<double>(y) / static_cast<double>(h);
                double v = sampleWithPerm(nx, ny, evolution, p, perm);
                if (v < 0.0) v = 0.0;
                if (v > 1.0) v = 1.0;

                int r = static_cast<int>(std::round(lr + (hr - lr) * v));
                int g = static_cast<int>(std::round(lg + (hg - lg) * v));
                int b = static_cast<int>(std::round(lb + (hb - lb) * v));
                scan[x] = qRgba(r, g, b, 255);
            }
        }
    }

    return img;
}

} // namespace FractalNoise
