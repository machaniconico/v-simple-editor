#include "OpticalFlow.h"

#include <QColor>
#include <QPainter>

#include <algorithm>  // std::min, std::max, std::clamp
#include <cmath>      // std::sqrt, std::atan2, std::floor, std::round

namespace opticalflow {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Convert any QImage format to 8-bit grayscale (Format_Grayscale8).
QImage toGray(const QImage& img)
{
    if (img.isNull()) return {};
    if (img.format() == QImage::Format_Grayscale8) return img;
    return img.convertToFormat(QImage::Format_Grayscale8);
}

// Downsample image by factor 2 (simple 2×2 box average) → grayscale.
QImage downsample2(const QImage& src)
{
    if (src.isNull() || src.width() < 2 || src.height() < 2) return src;
    int w2 = src.width()  / 2;
    int h2 = src.height() / 2;
    QImage dst(w2, h2, QImage::Format_Grayscale8);
    for (int y = 0; y < h2; ++y) {
        const uchar* r0 = src.scanLine(y * 2);
        const uchar* r1 = src.scanLine(y * 2 + 1);
        uchar* d = dst.scanLine(y);
        for (int x = 0; x < w2; ++x) {
            int v = (static_cast<int>(r0[x*2])   + static_cast<int>(r0[x*2+1])
                   + static_cast<int>(r1[x*2])   + static_cast<int>(r1[x*2+1])) / 4;
            d[x] = static_cast<uchar>(v);
        }
    }
    return dst;
}

// Sum of absolute differences between two blocks of size bs×bs.
// (ax,ay) top-left in image a; (bx,by) top-left in image b.
// Returns large value if any block pixel is out of bounds.
int sad(const QImage& a, const QImage& b,
        int ax, int ay, int bx, int by, int bs)
{
    int aw = a.width(),  ah = a.height();
    int bw = b.width(),  bh = b.height();
    if (ax < 0 || ay < 0 || ax + bs > aw || ay + bs > ah) return 0x7FFFFFFF;
    if (bx < 0 || by < 0 || bx + bs > bw || by + bs > bh) return 0x7FFFFFFF;
    int acc = 0;
    for (int dy = 0; dy < bs; ++dy) {
        const uchar* ra = a.scanLine(ay + dy) + ax;
        const uchar* rb = b.scanLine(by + dy) + bx;
        for (int dx = 0; dx < bs; ++dx) {
            acc += std::abs(static_cast<int>(ra[dx]) - static_cast<int>(rb[dx]));
        }
    }
    return acc;
}

// Build Gaussian pyramid (grayscale).  Level 0 = full res.
QVector<QImage> buildPyramid(const QImage& img, int levels)
{
    QVector<QImage> pyr;
    pyr.reserve(levels);
    QImage cur = toGray(img);
    pyr.push_back(cur);
    for (int l = 1; l < levels; ++l) {
        cur = downsample2(cur);
        pyr.push_back(cur);
    }
    return pyr;
}

// One 3×3 box-average smoothing pass over a flow field.
FlowField smoothFlow(const FlowField& f)
{
    FlowField out;
    out.width  = f.width;
    out.height = f.height;
    out.v.resize(f.v.size());

    for (int y = 0; y < f.height; ++y) {
        for (int x = 0; x < f.width; ++x) {
            double sx = 0.0, sy = 0.0;
            int cnt = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < f.width && ny >= 0 && ny < f.height) {
                        const QPointF& q = f.v[ny * f.width + nx];
                        sx += q.x();
                        sy += q.y();
                        ++cnt;
                    }
                }
            }
            if (cnt > 0) {
                out.v[y * f.width + x] = QPointF(sx / cnt, sy / cnt);
            } else {
                out.v[y * f.width + x] = QPointF(0.0, 0.0);
            }
        }
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// FlowField::at
// ---------------------------------------------------------------------------
QPointF FlowField::at(int x, int y) const
{
    if (v.isEmpty() || width <= 0 || height <= 0) return QPointF(0.0, 0.0);
    x = std::clamp(x, 0, width  - 1);
    y = std::clamp(y, 0, height - 1);
    return v[y * width + x];
}

// ---------------------------------------------------------------------------
// estimateFlow
// ---------------------------------------------------------------------------
FlowField estimateFlow(const QImage& a, const QImage& b, const FlowParams& p)
{
    if (a.isNull() || b.isNull()) return {};
    if (a.width() <= 0 || a.height() <= 0) return {};

    // Normalise b to same size as a if needed.
    QImage bScaled = (b.size() != a.size()) ? b.scaled(a.size(), Qt::IgnoreAspectRatio,
                                                         Qt::SmoothTransformation) : b;

    int levels     = std::max(1, p.levels);
    int blockSize  = std::max(1, p.blockSize);
    int searchRange = std::max(1, p.searchRange);

    // Build pyramids (index 0 = full res, index levels-1 = coarsest).
    QVector<QImage> pyrA = buildPyramid(a,       levels);
    QVector<QImage> pyrB = buildPyramid(bScaled, levels);

    int fullW = a.width();
    int fullH = a.height();

    // Flow at each pyramid level (in that level's coordinate space).
    // We start at the coarsest level with zero initial flow and refine.

    // Coarsest level dimensions.
    int coarseW = pyrA[levels - 1].width();
    int coarseH = pyrA[levels - 1].height();

    // Grid of flow vectors at the current working level.
    // We store one vector per *block* at coarse levels but expand to per-pixel
    // at the end.  For simplicity we store per-pixel throughout (wastes a bit
    // of memory on coarse levels but is clearer).

    // Current flow (per-pixel, in the coordinate space of the current level).
    FlowField cur;
    cur.width  = coarseW;
    cur.height = coarseH;
    cur.v.fill(QPointF(0.0, 0.0), coarseW * coarseH);

    // Coarsest-level block matching.
    {
        const QImage& la = pyrA[levels - 1];
        const QImage& lb = pyrB[levels - 1];
        int lw = la.width(), lh = la.height();
        int bs = std::max(1, blockSize >> (levels - 1));  // scale block size
        int sr = searchRange;  // search range is per-level (not scaled further)

        for (int by = 0; by < lh; by += bs) {
            for (int bx = 0; bx < lw; bx += bs) {
                int bestDx = 0, bestDy = 0, bestSad = 0x7FFFFFFF;

                for (int dy = -sr; dy <= sr; ++dy) {
                    for (int dx = -sr; dx <= sr; ++dx) {
                        int s = sad(la, lb, bx, by, bx + dx, by + dy, bs);
                        if (s < bestSad) {
                            bestSad = s;
                            bestDx  = dx;
                            bestDy  = dy;
                        }
                    }
                }
                // Assign to all pixels in this block.
                for (int py = by; py < std::min(by + bs, lh); ++py) {
                    for (int px = bx; px < std::min(bx + bs, lw); ++px) {
                        cur.v[py * lw + px] = QPointF(
                            static_cast<double>(bestDx),
                            static_cast<double>(bestDy));
                    }
                }
            }
        }
    }

    // Refine from coarse to fine (levels-2 down to 0).
    for (int l = levels - 2; l >= 0; --l) {
        const QImage& la = pyrA[l];
        const QImage& lb = pyrB[l];
        int lw = la.width(), lh = la.height();
        int bs = std::max(1, blockSize >> l);
        int sr = searchRange;  // refinement search window (small)

        // Upsample cur flow ×2 into the new level's coordinate space.
        FlowField next;
        next.width  = lw;
        next.height = lh;
        next.v.resize(lw * lh);

        for (int py = 0; py < lh; ++py) {
            for (int px = 0; px < lw; ++px) {
                // Map this fine-level pixel to the coarser level.
                int cx = std::min(px / 2, cur.width  - 1);
                int cy = std::min(py / 2, cur.height - 1);
                QPointF q = cur.v[cy * cur.width + cx];
                // Scale the vector up by 2 (coarse → fine).
                next.v[py * lw + px] = QPointF(q.x() * 2.0, q.y() * 2.0);
            }
        }

        // Refine on a block grid with a small search around the propagated estimate.
        for (int by = 0; by < lh; by += bs) {
            for (int bx = 0; bx < lw; bx += bs) {
                // Use the centre pixel's propagated flow as the initial estimate.
                int cx = std::min(bx + bs / 2, lw - 1);
                int cy = std::min(by + bs / 2, lh - 1);
                QPointF init = next.v[cy * lw + cx];
                int initDx = static_cast<int>(std::round(init.x()));
                int initDy = static_cast<int>(std::round(init.y()));

                int bestDx = initDx, bestDy = initDy, bestSad = 0x7FFFFFFF;

                // Small refinement window ±sr around the propagated estimate.
                for (int dy = -sr; dy <= sr; ++dy) {
                    for (int dx = -sr; dx <= sr; ++dx) {
                        int tdx = initDx + dx;
                        int tdy = initDy + dy;
                        int s = sad(la, lb, bx, by, bx + tdx, by + tdy, bs);
                        if (s < bestSad) {
                            bestSad = s;
                            bestDx  = tdx;
                            bestDy  = tdy;
                        }
                    }
                }

                // Write refined flow to all pixels in this block.
                for (int py = by; py < std::min(by + bs, lh); ++py) {
                    for (int px = bx; px < std::min(bx + bs, lw); ++px) {
                        next.v[py * lw + px] = QPointF(
                            static_cast<double>(bestDx),
                            static_cast<double>(bestDy));
                    }
                }
            }
        }

        cur = std::move(next);
    }

    // At this point cur is at full resolution (level 0).
    // But if levels==1, the coarse pass already worked at level 0 with the
    // per-block blockSize; cur.width/height may still equal fullW/fullH since
    // pyrA[0] == a.  Ensure the output dimensions match a exactly.
    if (cur.width != fullW || cur.height != fullH) {
        // Rescale by nearest-neighbour (should rarely happen).
        FlowField scaled;
        scaled.width  = fullW;
        scaled.height = fullH;
        scaled.v.resize(fullW * fullH);
        double sx = static_cast<double>(cur.width)  / fullW;
        double sy = static_cast<double>(cur.height) / fullH;
        double scaleVx = (cur.width  > 0) ? (static_cast<double>(fullW)  / cur.width)  : 1.0;
        double scaleVy = (cur.height > 0) ? (static_cast<double>(fullH)  / cur.height) : 1.0;
        for (int y = 0; y < fullH; ++y) {
            for (int x = 0; x < fullW; ++x) {
                int cx2 = std::clamp(static_cast<int>(x * sx), 0, cur.width  - 1);
                int cy2 = std::clamp(static_cast<int>(y * sy), 0, cur.height - 1);
                QPointF q = cur.v[cy2 * cur.width + cx2];
                scaled.v[y * fullW + x] = QPointF(q.x() * scaleVx, q.y() * scaleVy);
            }
        }
        cur = std::move(scaled);
    }

    // Optional smoothing.
    if (p.smooth) {
        cur = smoothFlow(cur);
    }

    return cur;
}

// ---------------------------------------------------------------------------
// warpImage
// ---------------------------------------------------------------------------
QImage warpImage(const QImage& src, const FlowField& flow, double t)
{
    if (src.isNull()) return {};
    if (flow.v.isEmpty()) return src;
    // For t==0, return a copy of src (no warp).
    if (t == 0.0) return src.copy();

    int w = src.width();
    int h = src.height();

    QImage out(w, h, QImage::Format_ARGB32);
    // Convert src to ARGB32 for uniform pixel access.
    QImage srcARGB = src.convertToFormat(QImage::Format_ARGB32);

    for (int y = 0; y < h; ++y) {
        QRgb* dstLine = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            QPointF fv = flow.at(x, y);
            double sx = x + t * fv.x();
            double sy = y + t * fv.y();

            // Edge-clamp.
            sx = std::clamp(sx, 0.0, static_cast<double>(w - 1));
            sy = std::clamp(sy, 0.0, static_cast<double>(h - 1));

            // Bilinear interpolation.
            int x0 = static_cast<int>(std::floor(sx));
            int y0 = static_cast<int>(std::floor(sy));
            int x1 = std::min(x0 + 1, w - 1);
            int y1 = std::min(y0 + 1, h - 1);
            double fx = sx - x0;
            double fy = sy - y0;

            QRgb c00 = srcARGB.pixel(x0, y0);
            QRgb c10 = srcARGB.pixel(x1, y0);
            QRgb c01 = srcARGB.pixel(x0, y1);
            QRgb c11 = srcARGB.pixel(x1, y1);

            double w00 = (1.0 - fx) * (1.0 - fy);
            double w10 = fx         * (1.0 - fy);
            double w01 = (1.0 - fx) * fy;
            double w11 = fx         * fy;

            int r = static_cast<int>(w00 * qRed(c00)   + w10 * qRed(c10)
                                   + w01 * qRed(c01)   + w11 * qRed(c11));
            int g = static_cast<int>(w00 * qGreen(c00) + w10 * qGreen(c10)
                                   + w01 * qGreen(c01) + w11 * qGreen(c11));
            int b2 = static_cast<int>(w00 * qBlue(c00)  + w10 * qBlue(c10)
                                    + w01 * qBlue(c01)  + w11 * qBlue(c11));
            int a  = static_cast<int>(w00 * qAlpha(c00) + w10 * qAlpha(c10)
                                    + w01 * qAlpha(c01) + w11 * qAlpha(c11));

            r  = std::clamp(r,  0, 255);
            g  = std::clamp(g,  0, 255);
            b2 = std::clamp(b2, 0, 255);
            a  = std::clamp(a,  0, 255);

            dstLine[x] = qRgba(r, g, b2, a);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// flowToColor
// ---------------------------------------------------------------------------
QImage flowToColor(const FlowField& flow)
{
    if (flow.v.isEmpty() || flow.width <= 0 || flow.height <= 0) return {};

    // Find max magnitude for normalisation.
    double maxMag = 1.0;  // avoid division by zero; at least 1 px
    for (const QPointF& q : flow.v) {
        double mag = std::sqrt(q.x() * q.x() + q.y() * q.y());
        if (mag > maxMag) maxMag = mag;
    }

    QImage out(flow.width, flow.height, QImage::Format_RGB32);

    for (int y = 0; y < flow.height; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < flow.width; ++x) {
            const QPointF& q = flow.v[y * flow.width + x];
            double mag = std::sqrt(q.x() * q.x() + q.y() * q.y());

            // Hue: atan2 result in [-π, π] → [0, 360).
            double angle = std::atan2(q.y(), q.x());  // radians
            double hue = angle * (180.0 / M_PI);       // degrees
            if (hue < 0.0) hue += 360.0;

            double val = std::min(1.0, mag / maxMag);

            // Convert HSV(hue, 1, val) → RGB.
            QColor c = QColor::fromHsvF(hue / 360.0, 1.0, val);
            line[x] = c.rgb();
        }
    }
    return out;
}

} // namespace opticalflow
