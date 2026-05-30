#include "AutoMatte.h"

#include <algorithm>
#include <cmath>

namespace automatte {

namespace {

// 2 画素 (各 RGB 0..255) の正規化 RGB ユークリッド色距離 (0..1)。
// 最大距離 sqrt(3*255^2) で割って 0..1 に正規化する。
inline double colorDistance(const uint8_t* a, const uint8_t* b)
{
    const double dr = static_cast<double>(a[0]) - static_cast<double>(b[0]);
    const double dg = static_cast<double>(a[1]) - static_cast<double>(b[1]);
    const double db = static_cast<double>(a[2]) - static_cast<double>(b[2]);
    const double d  = std::sqrt(dr * dr + dg * dg + db * db);
    static const double kMax = std::sqrt(3.0) * 255.0;
    return d / kMax;
}

inline double clampThreshold(double threshold)
{
    if (!std::isfinite(threshold))
        return 0.25;
    return std::clamp(threshold, 0.0, 1.0);
}

// w*h*4 の RGBA バッファとして妥当か。
inline bool validRgba(const std::vector<uint8_t>& buf, int w, int h)
{
    if (w <= 0 || h <= 0) return false;
    return buf.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
}

// w*h のマットバッファとして妥当か。
inline bool validMatte(const std::vector<uint8_t>& buf, int w, int h)
{
    if (w <= 0 || h <= 0) return false;
    return buf.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
}

// 1D box-blur 1 パス (radius、edge clamp)。in/out は同じ長さの 0..255 マット。
void boxBlur1D(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
               int w, int h, int radius, bool horizontal)
{
    out.resize(in.size());
    const int win = 2 * radius + 1;
    if (horizontal) {
        for (int y = 0; y < h; ++y) {
            const int row = y * w;
            for (int x = 0; x < w; ++x) {
                int sum = 0;
                for (int k = -radius; k <= radius; ++k) {
                    int xx = x + k;
                    if (xx < 0) xx = 0;
                    else if (xx >= w) xx = w - 1;
                    sum += in[row + xx];
                }
                out[row + x] = static_cast<uint8_t>(sum / win);
            }
        }
    } else {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int sum = 0;
                for (int k = -radius; k <= radius; ++k) {
                    int yy = y + k;
                    if (yy < 0) yy = 0;
                    else if (yy >= h) yy = h - 1;
                    sum += in[yy * w + x];
                }
                out[y * w + x] = static_cast<uint8_t>(sum / win);
            }
        }
    }
}

} // namespace

std::vector<uint8_t> differenceMatte(const std::vector<uint8_t>& fgRgba,
                                     const std::vector<uint8_t>& plateRgba,
                                     int w, int h, double threshold)
{
    if (!validRgba(fgRgba, w, h) || fgRgba.size() != plateRgba.size())
        return {};

    const double t = clampThreshold(threshold);
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<uint8_t> matte(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const uint8_t* a = &fgRgba[i * 4];
        const uint8_t* b = &plateRgba[i * 4];
        matte[i] = (colorDistance(a, b) > t) ? 255 : 0;
    }
    return matte;
}

std::vector<uint8_t> autoSegment(const std::vector<uint8_t>& rgba,
                                 int w, int h, double threshold)
{
    if (!validRgba(rgba, w, h))
        return {};

    const double t = clampThreshold(threshold);
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<uint8_t> matte(n, 0);

    // 四隅の色を背景シードに採用する。
    const int corners[4] = {
        0,                       // top-left
        (w - 1),                 // top-right
        (h - 1) * w,             // bottom-left
        (h - 1) * w + (w - 1),   // bottom-right
    };
    uint8_t seeds[4][3];
    for (int c = 0; c < 4; ++c) {
        const uint8_t* p = &rgba[static_cast<std::size_t>(corners[c]) * 4];
        seeds[c][0] = p[0];
        seeds[c][1] = p[1];
        seeds[c][2] = p[2];
    }

    // 中央被写体プライア: 中心からの正規化距離 (0=中心, 1=隅) を弱い前景バイアスに使う。
    const double cx = (w - 1) * 0.5;
    const double cy = (h - 1) * 0.5;
    const double maxR = std::sqrt(cx * cx + cy * cy);
    // プライアの寄与上限 (しきい値を最大このぶん引き下げる)。
    const double kPriorWeight = 0.10;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const uint8_t* px = &rgba[i * 4];

            // 最寄り背景シードとの距離。
            double minDist = 1.0;
            for (int c = 0; c < 4; ++c) {
                const double d = colorDistance(px, seeds[c]);
                if (d < minDist) minDist = d;
            }

            // 中心に近いほどしきい値を下げ、前景判定されやすくする。
            double prior = 0.0;
            if (maxR > 0.0) {
                const double dx = x - cx;
                const double dy = y - cy;
                const double r = std::sqrt(dx * dx + dy * dy) / maxR; // 0..1
                prior = (1.0 - r) * kPriorWeight;                     // 中心で最大
            }
            const double effThreshold = std::clamp(t - prior, 0.0, 1.0);

            matte[i] = (minDist > effThreshold) ? 255 : 0;
        }
    }
    return matte;
}

std::vector<uint8_t> erodeMatte(const std::vector<uint8_t>& matte, int w, int h, int radius)
{
    if (!validMatte(matte, w, h)) return {};
    if (radius <= 0) return matte;

    std::vector<uint8_t> out(matte.size(), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t minV = 255;
            for (int dy = -radius; dy <= radius && minV != 0; ++dy) {
                int yy = y + dy;
                if (yy < 0) yy = 0;
                else if (yy >= h) yy = h - 1;
                for (int dx = -radius; dx <= radius; ++dx) {
                    int xx = x + dx;
                    if (xx < 0) xx = 0;
                    else if (xx >= w) xx = w - 1;
                    const uint8_t v = matte[static_cast<std::size_t>(yy) * w + xx];
                    if (v < minV) { minV = v; if (minV == 0) break; }
                }
            }
            out[static_cast<std::size_t>(y) * w + x] = minV;
        }
    }
    return out;
}

std::vector<uint8_t> dilateMatte(const std::vector<uint8_t>& matte, int w, int h, int radius)
{
    if (!validMatte(matte, w, h)) return {};
    if (radius <= 0) return matte;

    std::vector<uint8_t> out(matte.size(), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t maxV = 0;
            for (int dy = -radius; dy <= radius && maxV != 255; ++dy) {
                int yy = y + dy;
                if (yy < 0) yy = 0;
                else if (yy >= h) yy = h - 1;
                for (int dx = -radius; dx <= radius; ++dx) {
                    int xx = x + dx;
                    if (xx < 0) xx = 0;
                    else if (xx >= w) xx = w - 1;
                    const uint8_t v = matte[static_cast<std::size_t>(yy) * w + xx];
                    if (v > maxV) { maxV = v; if (maxV == 255) break; }
                }
            }
            out[static_cast<std::size_t>(y) * w + x] = maxV;
        }
    }
    return out;
}

std::vector<uint8_t> featherMatte(const std::vector<uint8_t>& matte, int w, int h, int radius)
{
    if (!validMatte(matte, w, h)) return {};
    if (radius <= 0) return matte;

    // box-blur 2 パス (H→V を 2 回) で近似ガウシアン。
    std::vector<uint8_t> a = matte;
    std::vector<uint8_t> b;
    boxBlur1D(a, b, w, h, radius, true);
    boxBlur1D(b, a, w, h, radius, false);
    boxBlur1D(a, b, w, h, radius, true);
    boxBlur1D(b, a, w, h, radius, false);
    return a;
}

std::vector<uint8_t> refineMatte(const std::vector<uint8_t>& matte, int w, int h,
                                 const MatteParams& params)
{
    if (!validMatte(matte, w, h)) return {};
    std::vector<uint8_t> m = matte;
    m = erodeMatte(m, w, h, params.erode);
    m = dilateMatte(m, w, h, params.dilate);
    m = featherMatte(m, w, h, params.featherRadius);
    return m;
}

std::vector<uint8_t> suppressSpill(const std::vector<uint8_t>& fgRgba,
                                   const std::vector<uint8_t>& matte,
                                   int w, int h, double amount)
{
    if (!validRgba(fgRgba, w, h) || !validMatte(matte, w, h))
        return fgRgba;
    if (amount <= 0.0)
        return fgRgba;
    const double amt = std::min(1.0, amount);

    std::vector<uint8_t> out = fgRgba;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < n; ++i) {
        // 境界 (中間アルファ) ほど強く抑制する。
        const double a = matte[i] / 255.0;
        const double edgeness = 1.0 - std::abs(2.0 * a - 1.0); // 0(端) .. 1(中間)
        if (edgeness <= 0.0) continue;

        uint8_t* p = &out[i * 4];
        const int r = p[0], g = p[1], bch = p[2];
        // 最大チャンネルが突出していれば、その分を他 2 チャンネル平均へ寄せる。
        const int maxc = std::max({r, g, bch});
        const double other = (maxc == r)   ? (g + bch) * 0.5
                           : (maxc == g)   ? (r + bch) * 0.5
                                           : (r + g)   * 0.5;
        if (maxc > other) {
            const double reduce = (maxc - other) * amt * edgeness;
            const int newMax = static_cast<int>(std::lround(maxc - reduce));
            if (maxc == r)        p[0] = static_cast<uint8_t>(std::clamp(newMax, 0, 255));
            else if (maxc == g)   p[1] = static_cast<uint8_t>(std::clamp(newMax, 0, 255));
            else                  p[2] = static_cast<uint8_t>(std::clamp(newMax, 0, 255));
        }
        // alpha (p[3]) は不変。
    }
    return out;
}

std::vector<uint8_t> composite(const std::vector<uint8_t>& fgRgba,
                               const std::vector<uint8_t>& bgRgba,
                               const std::vector<uint8_t>& matte,
                               int w, int h)
{
    if (!validRgba(fgRgba, w, h) || fgRgba.size() != bgRgba.size()
        || !validMatte(matte, w, h))
        return {};

    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<uint8_t> out(fgRgba.size(), 0);
    for (std::size_t i = 0; i < n; ++i) {
        const double a = matte[i] / 255.0;
        const uint8_t* f = &fgRgba[i * 4];
        const uint8_t* b = &bgRgba[i * 4];
        uint8_t* o = &out[i * 4];
        for (int c = 0; c < 3; ++c) {
            const double v = f[c] * a + b[c] * (1.0 - a);
            o[c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
        }
        o[3] = 255;
    }
    return out;
}

std::vector<uint8_t> applyMatteAsAlpha(const std::vector<uint8_t>& rgba,
                                       const std::vector<uint8_t>& matte,
                                       int w, int h)
{
    if (!validRgba(rgba, w, h) || !validMatte(matte, w, h))
        return rgba;

    std::vector<uint8_t> out = rgba;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < n; ++i)
        out[i * 4 + 3] = matte[i];
    return out;
}

QImage rgbaToQImage(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (!validRgba(rgba, w, h))
        return QImage();
    // バッファから構築した QImage は元データを参照するため、copy() で深いコピーする。
    QImage img(rgba.data(), w, h, w * 4, QImage::Format_RGBA8888);
    return img.copy();
}

std::vector<uint8_t> qimageToRgba(const QImage& image)
{
    if (image.isNull())
        return {};
    const QImage conv = (image.format() == QImage::Format_RGBA8888)
                            ? image
                            : image.convertToFormat(QImage::Format_RGBA8888);
    const int w = conv.width();
    const int h = conv.height();
    std::vector<uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    for (int y = 0; y < h; ++y) {
        const uchar* line = conv.constScanLine(y);
        std::copy(line, line + static_cast<std::size_t>(w) * 4u,
                  out.begin() + static_cast<std::size_t>(y) * w * 4u);
    }
    return out;
}

} // namespace automatte
