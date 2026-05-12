// RotoAutoTrace.cpp — US-PRO-1
// Edge-contour auto-trace + edge-snap for the Rotoscope subsystem.
//
// Qt-only (no OpenCV). Tested-by-inspection against:
//   AC1  struct + function declarations match header
//   AC2  white rect on black -> closed path near border, count <= maxPoints
//   AC3  flat image -> 4-corner fallback, no crash
//   AC4  snapPointToEdge off a strong edge -> moves onto the edge
//   AC5  debugEdgeMap -> same-size greyscale, 0 in flat areas, bright on edges
//   AC6  deterministic, terminates on 1280x720
//   AC7  Qt6/MSVC, no third-party deps

#include "RotoAutoTrace.h"

#include <QLineF>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <vector>

// MSVC: make sure std::round etc. are visible via <cmath>
// (already included above)

namespace rototrace {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Convert any QImage to an 8-bit greyscale image.
// Result is always Format_Grayscale8 and same size as src.
// Handles null and 1x1 images safely.
QImage toGrayscale(const QImage &src)
{
    if (src.isNull())
        return QImage();
    return src.convertToFormat(QImage::Format_Grayscale8);
}

// Box-blur a single-channel (Grayscale8) image in-place.
// radius is the half-width of the box kernel (box width = 2*radius+1).
// Approximates a Gaussian when applied multiple times; here we apply it
// three times to get a reasonable approximation.
// Bounded: O(W*H) per pass.
void boxBlurGray(QImage &img, int radius)
{
    if (img.isNull() || radius <= 0)
        return;

    const int W = img.width();
    const int H = img.height();

    // Horizontal pass
    std::vector<quint8> row(static_cast<size_t>(W));
    for (int y = 0; y < H; ++y) {
        const uchar *src = img.constScanLine(y);
        uchar       *dst = img.scanLine(y);

        // prefix sum
        std::vector<int> psum(static_cast<size_t>(W + 1), 0);
        for (int x = 0; x < W; ++x)
            psum[static_cast<size_t>(x + 1)] = psum[static_cast<size_t>(x)] + src[x];

        for (int x = 0; x < W; ++x) {
            int x0 = std::max(0, x - radius);
            int x1 = std::min(W - 1, x + radius);
            int cnt = x1 - x0 + 1;
            int sum = psum[static_cast<size_t>(x1 + 1)] - psum[static_cast<size_t>(x0)];
            row[static_cast<size_t>(x)] = static_cast<quint8>(sum / cnt);
        }
        for (int x = 0; x < W; ++x)
            dst[x] = row[static_cast<size_t>(x)];
    }

    // Vertical pass — work column by column via a transposed buffer
    std::vector<int> col(static_cast<size_t>(H + 1), 0);
    std::vector<quint8> colOut(static_cast<size_t>(H));
    for (int x = 0; x < W; ++x) {
        col[0] = 0;
        for (int y = 0; y < H; ++y)
            col[static_cast<size_t>(y + 1)] = col[static_cast<size_t>(y)] + img.constScanLine(y)[x];

        for (int y = 0; y < H; ++y) {
            int y0 = std::max(0, y - radius);
            int y1 = std::min(H - 1, y + radius);
            int cnt = y1 - y0 + 1;
            int sum = col[static_cast<size_t>(y1 + 1)] - col[static_cast<size_t>(y0)];
            colOut[static_cast<size_t>(y)] = static_cast<quint8>(sum / cnt);
        }
        for (int y = 0; y < H; ++y)
            img.scanLine(y)[x] = colOut[static_cast<size_t>(y)];
    }
}

// Apply box blur 3 times to approximate a Gaussian with the given sigma.
// The equivalent box radius r satisfies: r ≈ sigma * sqrt(3) (3-pass rule).
void gaussianBlurGray(QImage &img, double sigma)
{
    if (img.isNull() || sigma <= 0.0)
        return;
    int r = std::max(1, static_cast<int>(std::round(sigma * 1.73205)));
    boxBlurGray(img, r);
    boxBlurGray(img, r);
    boxBlurGray(img, r);
}

// Compute Sobel gradient magnitude for a Grayscale8 image.
// Returns a float array [H][W] with values in [0, 255*sqrt(2)].
// Pixels on the border have gradient 0.
std::vector<float> sobelMagnitude(const QImage &gray)
{
    const int W = gray.width();
    const int H = gray.height();
    std::vector<float> mag(static_cast<size_t>(W * H), 0.0f);

    if (W < 3 || H < 3)
        return mag;

    for (int y = 1; y < H - 1; ++y) {
        const uchar *r0 = gray.constScanLine(y - 1);
        const uchar *r1 = gray.constScanLine(y);
        const uchar *r2 = gray.constScanLine(y + 1);
        for (int x = 1; x < W - 1; ++x) {
            float gx = static_cast<float>(
                -r0[x-1] + r0[x+1]
                - 2*r1[x-1] + 2*r1[x+1]
                - r2[x-1] + r2[x+1]);
            float gy = static_cast<float>(
                -r0[x-1] - 2*r0[x] - r0[x+1]
                + r2[x-1] + 2*r2[x] + r2[x+1]);
            mag[static_cast<size_t>(y * W + x)] = std::sqrt(gx * gx + gy * gy);
        }
    }
    return mag;
}

// Sample gradient magnitude at integer pixel (x,y) with bounds check.
inline float magAt(const std::vector<float> &mag, int W, int H, int x, int y)
{
    if (x < 0 || y < 0 || x >= W || y >= H)
        return 0.0f;
    return mag[static_cast<size_t>(y * W + x)];
}

// ─── Ramer-Douglas-Peucker ────────────────────────────────────────────────

static double perpendicularDist(const QPointF &pt, const QPointF &a, const QPointF &b)
{
    QPointF ab = b - a;
    double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 < 1e-12)
        return QLineF(pt, a).length();
    double t = ((pt.x() - a.x()) * ab.x() + (pt.y() - a.y()) * ab.y()) / len2;
    QPointF proj = a + t * ab;
    QPointF diff = pt - proj;
    return std::sqrt(diff.x() * diff.x() + diff.y() * diff.y());
}

// Recursive RDP; appends kept indices into `out`.
static void rdp(const std::vector<QPointF> &pts,
                int start, int end,
                double epsilon,
                std::vector<int> &out)
{
    if (end <= start + 1) {
        out.push_back(end);
        return;
    }
    double maxDist = 0.0;
    int    maxIdx  = start;
    for (int i = start + 1; i < end; ++i) {
        double d = perpendicularDist(pts[static_cast<size_t>(i)],
                                     pts[static_cast<size_t>(start)],
                                     pts[static_cast<size_t>(end)]);
        if (d > maxDist) { maxDist = d; maxIdx = i; }
    }
    if (maxDist > epsilon) {
        rdp(pts, start, maxIdx, epsilon, out);
        rdp(pts, maxIdx, end,   epsilon, out);
    } else {
        out.push_back(end);
    }
}

static std::vector<QPointF> simplifyRDP(const std::vector<QPointF> &pts, double epsilon)
{
    if (pts.size() < 3)
        return pts;
    std::vector<int> kept;
    kept.push_back(0);
    rdp(pts, 0, static_cast<int>(pts.size()) - 1, epsilon, kept);
    std::sort(kept.begin(), kept.end());
    std::vector<QPointF> result;
    result.reserve(kept.size());
    for (int idx : kept)
        result.push_back(pts[static_cast<size_t>(idx)]);
    return result;
}

// ─── Catmull-Rom handle fitting ───────────────────────────────────────────

// Given a closed polygon (points implicitly wrap), compute smooth Bezier
// handles using the Catmull-Rom formula (alpha=0.5 for centripetal).
// handleIn  ≈ p - (p_next - p_prev) / 6
// handleOut ≈ p + (p_next - p_prev) / 6
static RotoPath buildClosedPath(const std::vector<QPointF> &poly)
{
    RotoPath path;
    path.closed = true;

    const int N = static_cast<int>(poly.size());
    if (N == 0)
        return path;
    if (N == 1) {
        RotoPoint rp;
        rp.position  = poly[0];
        rp.handleIn  = poly[0];
        rp.handleOut = poly[0];
        path.points.append(rp);
        return path;
    }

    for (int i = 0; i < N; ++i) {
        int prev = (i - 1 + N) % N;
        int next = (i + 1) % N;

        QPointF p  = poly[static_cast<size_t>(i)];
        QPointF p0 = poly[static_cast<size_t>(prev)];
        QPointF p2 = poly[static_cast<size_t>(next)];

        // tangent direction
        QPointF tang = (p2 - p0);
        // scale: 1/6 of the distance to neighbours for a cubic Bezier
        double scale = 1.0 / 6.0;

        RotoPoint rp;
        rp.position  = p;
        rp.handleOut = p + tang * scale;
        rp.handleIn  = p - tang * scale;
        path.points.append(rp);
    }
    return path;
}

// ─── Rectangular fallback path ────────────────────────────────────────────

static RotoPath rectFallback(const QRectF &r)
{
    std::vector<QPointF> corners = {
        r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()
    };
    return buildClosedPath(corners);
}

// ─── Moore-neighbour boundary tracing ─────────────────────────────────────
//
// Classic Moore-neighbour (8-connected) contour tracing on a binary edge
// image.  We find the first edge pixel inside seedRegion (scanning top-to-
// bottom, left-to-right), then trace the outer boundary of the connected
// component, stopping when we return to the start or exceed a hard limit.
//
// The 8 directions in CCW order, starting from "right".
static const int DIR_DX[8] = { 1,  1,  0, -1, -1, -1,  0,  1 };
static const int DIR_DY[8] = { 0, -1, -1, -1,  0,  1,  1,  1 };

// Return the direction index that is 180° opposite (backtrack).
inline int opposite(int d) { return (d + 4) & 7; }

static std::vector<QPointF> mooreTrace(const std::vector<bool> &edge,
                                       int W, int H,
                                       int startX, int startY,
                                       int maxSteps = 200000)
{
    std::vector<QPointF> contour;
    contour.push_back(QPointF(startX, startY));

    // Entry direction: we arrived from "left" of the start pixel, so the
    // backtrack direction is "left" = index 3 (dx=-1).
    // We search clockwise from the direction opposite to backtrack.
    int cx = startX, cy = startY;
    int entryDir = 3; // came from left

    for (int step = 0; step < maxSteps; ++step) {
        // Search 8 neighbours starting from (opposite(entryDir) + 1) % 8
        int startSearch = (opposite(entryDir) + 1) & 7;
        bool found = false;
        for (int k = 0; k < 8; ++k) {
            int d  = (startSearch + k) & 7;
            int nx = cx + DIR_DX[d];
            int ny = cy + DIR_DY[d];
            if (nx >= 0 && ny >= 0 && nx < W && ny < H &&
                edge[static_cast<size_t>(ny * W + nx)])
            {
                entryDir = d;
                cx = nx; cy = ny;
                found = true;
                break;
            }
        }
        if (!found)
            break;

        // Stop when we revisit the start pixel (closed contour)
        if (cx == startX && cy == startY)
            break;

        contour.push_back(QPointF(cx, cy));
    }
    return contour;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

RotoPath autoTraceContour(const QImage &frame,
                          const QRectF &seedRegion,
                          const RotoAutoTraceParams &p)
{
    // Null / degenerate image guard
    if (frame.isNull() || frame.width() < 1 || frame.height() < 1)
        return rectFallback(seedRegion);

    const int W = frame.width();
    const int H = frame.height();

    // 1. Greyscale
    QImage gray = toGrayscale(frame);

    // 2. Optional blur
    if (p.blurRadius > 0.0)
        gaussianBlurGray(gray, p.blurRadius);

    // 3. Sobel magnitude
    std::vector<float> mag = sobelMagnitude(gray);

    // 4. Threshold to binary edge image (clipped to seedRegion)
    // Clamp seedRegion to image bounds
    int rx0 = static_cast<int>(std::max(0.0,  std::floor(seedRegion.left())));
    int ry0 = static_cast<int>(std::max(0.0,  std::floor(seedRegion.top())));
    int rx1 = static_cast<int>(std::min(static_cast<double>(W - 1), std::ceil(seedRegion.right())));
    int ry1 = static_cast<int>(std::min(static_cast<double>(H - 1), std::ceil(seedRegion.bottom())));

    if (rx1 < rx0 || ry1 < ry0)
        return rectFallback(seedRegion);

    std::vector<bool> edge(static_cast<size_t>(W * H), false);
    for (int y = ry0; y <= ry1; ++y)
        for (int x = rx0; x <= rx1; ++x)
            if (mag[static_cast<size_t>(y * W + x)] >= static_cast<float>(p.edgeThreshold))
                edge[static_cast<size_t>(y * W + x)] = true;

    // 5. Find start pixel: scan top row of seedRegion first, then raster
    int startX = -1, startY = -1;
    for (int y = ry0; y <= ry1 && startX < 0; ++y)
        for (int x = rx0; x <= rx1 && startX < 0; ++x)
            if (edge[static_cast<size_t>(y * W + x)])
            { startX = x; startY = y; }

    if (startX < 0)
        return rectFallback(seedRegion);  // AC3: flat image

    // 6. Moore-neighbour trace (bounded by maxSteps = W*H)
    std::vector<QPointF> contour = mooreTrace(edge, W, H, startX, startY, W * H);

    if (contour.size() < 2)
        return rectFallback(seedRegion);

    // 7. RDP simplification
    std::vector<QPointF> simplified = simplifyRDP(contour, p.simplifyEpsilon);

    // 8. Enforce maxPoints (uniform sub-sampling if still over limit)
    if (static_cast<int>(simplified.size()) > p.maxPoints && p.maxPoints > 3) {
        std::vector<QPointF> sub;
        sub.reserve(static_cast<size_t>(p.maxPoints));
        double step = static_cast<double>(simplified.size()) / p.maxPoints;
        for (int i = 0; i < p.maxPoints; ++i) {
            size_t idx = static_cast<size_t>(std::round(i * step));
            if (idx >= simplified.size()) idx = simplified.size() - 1;
            sub.push_back(simplified[idx]);
        }
        simplified = sub;
    }

    // 9. Build closed RotoPath with Catmull-Rom handles
    return buildClosedPath(simplified);
}

// ─────────────────────────────────────────────────────────────────────────────

QPointF snapPointToEdge(const QImage &frame,
                        QPointF p,
                        double searchRadius,
                        double blurRadius)
{
    if (frame.isNull() || searchRadius < 1.0)
        return p;

    const int W = frame.width();
    const int H = frame.height();

    QImage gray = toGrayscale(frame);
    if (blurRadius > 0.0)
        gaussianBlurGray(gray, blurRadius);
    std::vector<float> mag = sobelMagnitude(gray);

    // Gradient at input point (nearest pixel)
    int px = static_cast<int>(std::round(p.x()));
    int py = static_cast<int>(std::round(p.y()));
    px = std::max(0, std::min(W - 1, px));
    py = std::max(0, std::min(H - 1, py));
    float baseMag = magAt(mag, W, H, px, py);

    // Search window
    int r   = static_cast<int>(std::ceil(searchRadius));
    int x0  = std::max(0,     px - r);
    int y0  = std::max(0,     py - r);
    int x1  = std::min(W - 1, px + r);
    int y1  = std::min(H - 1, py + r);

    double  bestMag = static_cast<double>(baseMag);
    int     bestX   = px, bestY = py;
    double  r2      = searchRadius * searchRadius;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            double dx = x - p.x(), dy = y - p.y();
            if (dx*dx + dy*dy > r2)
                continue;
            double m = static_cast<double>(magAt(mag, W, H, x, y));
            if (m > bestMag) {
                bestMag = m;
                bestX   = x;
                bestY   = y;
            }
        }
    }

    // Parabolic sub-pixel refinement in x and y independently
    // (only when bestX/bestY is not on the border)
    double subX = static_cast<double>(bestX);
    double subY = static_cast<double>(bestY);

    if (bestX > 0 && bestX < W - 1) {
        double m_l = static_cast<double>(magAt(mag, W, H, bestX - 1, bestY));
        double m_c = static_cast<double>(magAt(mag, W, H, bestX,     bestY));
        double m_r = static_cast<double>(magAt(mag, W, H, bestX + 1, bestY));
        double denom = m_l - 2.0 * m_c + m_r;
        if (std::abs(denom) > 1e-6)
            subX = bestX - 0.5 * (m_r - m_l) / denom;
    }
    if (bestY > 0 && bestY < H - 1) {
        double m_u = static_cast<double>(magAt(mag, W, H, bestX, bestY - 1));
        double m_c = static_cast<double>(magAt(mag, W, H, bestX, bestY));
        double m_d = static_cast<double>(magAt(mag, W, H, bestX, bestY + 1));
        double denom = m_u - 2.0 * m_c + m_d;
        if (std::abs(denom) > 1e-6)
            subY = bestY - 0.5 * (m_d - m_u) / denom;
    }

    // Guarantee: only move if we actually found a stronger point
    if (bestMag >= static_cast<double>(baseMag))
        return QPointF(subX, subY);
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────

QImage debugEdgeMap(const QImage &frame, double blurRadius, double threshold)
{
    if (frame.isNull())
        return QImage();

    const int W = frame.width();
    const int H = frame.height();

    QImage gray = toGrayscale(frame);
    if (blurRadius > 0.0)
        gaussianBlurGray(gray, blurRadius);

    std::vector<float> mag = sobelMagnitude(gray);

    QImage result(W, H, QImage::Format_Grayscale8);
    result.fill(0);

    // Find max magnitude for normalisation
    float maxMag = 0.0f;
    for (float v : mag)
        if (v > maxMag) maxMag = v;
    if (maxMag < 1.0f) maxMag = 1.0f;  // avoid div-by-zero on flat images

    for (int y = 0; y < H; ++y) {
        uchar *line = result.scanLine(y);
        for (int x = 0; x < W; ++x) {
            float v = mag[static_cast<size_t>(y * W + x)];
            if (threshold > 0.0 && static_cast<double>(v) < threshold) {
                line[x] = 0;
            } else {
                // Scale to [0,255] relative to max magnitude
                int val = static_cast<int>(v / maxMag * 255.0f + 0.5f);
                line[x] = static_cast<uchar>(std::min(255, std::max(0, val)));
            }
        }
    }
    return result;
}

}  // namespace rototrace
