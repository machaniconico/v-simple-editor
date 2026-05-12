// RotoTracking.cpp — shape propagation for rotoscoped paths
//
// Algorithm overview:
//  1. Sample ~8 points around the bounding box of the seed path.
//  2. For each consecutive frame pair, extract a 15×15 grayscale template
//     patch around each sample point from the previous frame, then search
//     inside a (2*searchMargin+15)×(2*searchMargin+15) window in the current
//     frame via normalised cross-correlation (NCC).
//  3. Collect correspondences (prevPt → currPt) for points with NCC ≥ minConfidence.
//  4. Least-squares-fit a 2-D similarity transform [s·cosθ, -s·sinθ, tx;
//     s·sinθ,  s·cosθ, ty] from the correspondences (minimum 2 required).
//  5. If the fitted scale deviates from 1.0 by more than maxScaleDelta, or
//     fewer than 2 good correspondences exist, hold (reuse previous transform).
//  6. Compose the per-frame delta into a cumulative transform from the seed.
//  7. Apply the cumulative transform to the seed path at keyframe emit points.

#include "RotoTracking.h"

#include <QRectF>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace rototrack {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Convert a QImage region to an 8-bit grayscale buffer (row-major).
// Returns an empty vector if the image is null or the rect is out of bounds.
struct GrayPatch {
    std::vector<uint8_t> data;
    int width  = 0;
    int height = 0;
};

static GrayPatch extractGray(const QImage &img, int x, int y, int w, int h)
{
    GrayPatch patch;
    if (img.isNull() || w <= 0 || h <= 0) return patch;

    const int imgW = img.width();
    const int imgH = img.height();

    // Clamp rect to image bounds.
    int x0 = std::max(x, 0);
    int y0 = std::max(y, 0);
    int x1 = std::min(x + w, imgW);
    int y1 = std::min(y + h, imgH);
    if (x1 <= x0 || y1 <= y0) return patch;

    int pw = x1 - x0;
    int ph = y1 - y0;
    patch.width  = pw;
    patch.height = ph;
    patch.data.resize(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph));

    // Use a converted grayscale image to avoid per-pixel format branching.
    QImage gray = img.convertToFormat(QImage::Format_Grayscale8);

    for (int row = 0; row < ph; ++row) {
        const uchar *src = gray.constScanLine(y0 + row) + x0;
        uint8_t     *dst = patch.data.data() + static_cast<std::size_t>(row) * pw;
        std::copy(src, src + pw, dst);
    }
    return patch;
}

// Compute NCC between a template (tw×th) and a patch at offset (ox, oy) inside
// a search image (sw×sh).  Returns −1.0 if stddev is near zero (flat region).
static double computeNCC(const GrayPatch &search,
                         const GrayPatch &templ,
                         int ox, int oy)
{
    const int tw = templ.width;
    const int th = templ.height;
    const int sw = search.width;
    const int sh = search.height;

    if (tw <= 0 || th <= 0 || sw <= 0 || sh <= 0) return -1.0;
    if (ox < 0 || oy < 0 || ox + tw > sw || oy + th > sh) return -1.0;

    double tSum  = 0.0;
    double sSum  = 0.0;
    const int N  = tw * th;

    for (int r = 0; r < th; ++r) {
        const uint8_t *tRow = templ.data.data()  + static_cast<std::size_t>(r) * tw;
        const uint8_t *sRow = search.data.data() + static_cast<std::size_t>(oy + r) * sw + ox;
        for (int c = 0; c < tw; ++c) {
            tSum += tRow[c];
            sSum += sRow[c];
        }
    }
    const double tMean = tSum / N;
    const double sMean = sSum / N;

    double num   = 0.0;
    double tVar  = 0.0;
    double sVar  = 0.0;

    for (int r = 0; r < th; ++r) {
        const uint8_t *tRow = templ.data.data()  + static_cast<std::size_t>(r) * tw;
        const uint8_t *sRow = search.data.data() + static_cast<std::size_t>(oy + r) * sw + ox;
        for (int c = 0; c < tw; ++c) {
            const double td = tRow[c] - tMean;
            const double sd = sRow[c] - sMean;
            num  += td * sd;
            tVar += td * td;
            sVar += sd * sd;
        }
    }

    const double denom = std::sqrt(tVar * sVar);
    if (denom < 1e-9) return -1.0;
    return num / denom;
}

// Find the best NCC match for a template inside a search image.
// searchImg covers [prevCx - searchMargin - half, prevCx + searchMargin + half] etc.
// Returns the sub-pixel displacement (dx, dy) from prevPt to the matched currPt,
// and the NCC score.  score = -1 on failure.
struct MatchResult {
    double dx    = 0.0;
    double dy    = 0.0;
    double score = -1.0;
};

static MatchResult matchTemplate(const QImage &prevFrame,
                                 const QImage &currFrame,
                                 double cx, double cy,
                                 int half,        // template half-size → template is (2*half+1)^2
                                 int margin)       // search margin in pixels
{
    MatchResult result;
    if (prevFrame.isNull() || currFrame.isNull()) return result;

    const int tw = 2 * half + 1;
    const int th = tw;

    // Template from prev frame centred on (cx, cy).
    const int tx = static_cast<int>(std::round(cx)) - half;
    const int ty = static_cast<int>(std::round(cy)) - half;
    GrayPatch templ = extractGray(prevFrame, tx, ty, tw, th);
    if (templ.data.empty()) return result;

    // Search window in curr frame: template position ± margin.
    const int sxReq = tx - margin;
    const int syReq = ty - margin;
    const int sw    = tw + 2 * margin;
    const int sh    = th + 2 * margin;
    // Clamp to image so we know the actual top-left pixel coordinate.
    const int sxAct = std::max(sxReq, 0);
    const int syAct = std::max(syReq, 0);
    GrayPatch search = extractGray(currFrame, sxReq, syReq, sw, sh);
    if (search.data.empty()) return result;

    // Actual template size may be smaller if templ was clamped.
    const int etw = templ.width;
    const int eth = templ.height;

    double bestScore = -2.0;
    int    bestOx    = 0;
    int    bestOy    = 0;

    const int maxOx = search.width  - etw;
    const int maxOy = search.height - eth;

    for (int oy = 0; oy <= maxOy; ++oy) {
        for (int ox = 0; ox <= maxOx; ++ox) {
            double ncc = computeNCC(search, templ, ox, oy);
            if (ncc > bestScore) {
                bestScore = ncc;
                bestOx    = ox;
                bestOy    = oy;
            }
        }
    }

    if (bestScore < -1.5) return result; // no valid candidate found

    // Convert best offset back to world displacement.
    // patch[0,0] = frame pixel (sxAct, syAct).
    // The matched template top-left in the frame is (sxAct + bestOx, syAct + bestOy).
    // The centre of the matched template patch:
    const double matchCx = sxAct + bestOx + half;
    const double matchCy = syAct + bestOy + half;

    result.dx    = matchCx - cx;
    result.dy    = matchCy - cy;
    result.score = bestScore;
    return result;
}

// Compute the bounding box of a RotoPath.
static QRectF pathBounds(const RotoPath &path)
{
    if (path.points.isEmpty()) return {};
    double xMin =  std::numeric_limits<double>::max();
    double yMin =  std::numeric_limits<double>::max();
    double xMax = -std::numeric_limits<double>::max();
    double yMax = -std::numeric_limits<double>::max();
    for (const RotoPoint &rp : path.points) {
        xMin = std::min(xMin, rp.position.x());
        yMin = std::min(yMin, rp.position.y());
        xMax = std::max(xMax, rp.position.x());
        yMax = std::max(yMax, rp.position.y());
    }
    return {xMin, yMin, xMax - xMin, yMax - yMin};
}

// Generate ~8 sample points distributed around the path bounding box.
// We use the path anchor points themselves plus midpoints of the bounding box
// edges, capped to a maximum of 8 to keep the inner loop fast.
static std::vector<QPointF> samplePoints(const RotoPath &path)
{
    std::vector<QPointF> pts;

    // Add all anchor positions (up to 8).
    for (const RotoPoint &rp : path.points) {
        pts.push_back(rp.position);
        if (static_cast<int>(pts.size()) >= 8) break;
    }

    // If fewer than 8, pad with bounding-box midpoints.
    if (static_cast<int>(pts.size()) < 8) {
        QRectF bb = pathBounds(path);
        if (!bb.isEmpty()) {
            const double cx = bb.center().x();
            const double cy = bb.center().y();
            const double l  = bb.left();
            const double r  = bb.right();
            const double t  = bb.top();
            const double b  = bb.bottom();
            std::vector<QPointF> extra = {
                {cx, t}, {cx, b}, {l, cy}, {r, cy},
                {l, t},  {r, t},  {l, b},  {r, b}
            };
            for (const QPointF &e : extra) {
                pts.push_back(e);
                if (static_cast<int>(pts.size()) >= 8) break;
            }
        }
    }

    return pts;
}

// Least-squares fit of a 2-D similarity transform from N≥2 correspondences.
// Model: [x'] = s*[cosθ  -sinθ] [x] + [tx]
//        [y']     [sinθ   cosθ] [y]   [ty]
// Reformulated as: for each pair (src, dst):
//   [src.x  -src.y  1  0] [a]   [dst.x]
//   [src.y   src.x  0  1] [b] = [dst.y]
//                         [tx]
//                         [ty]
// where a = s*cosθ, b = s*sinθ.
// Solved via closed-form least squares (normal equations for 4 unknowns).
// Returns false if the system is degenerate.
struct SimilarityResult {
    double a  = 1.0; // s*cosθ
    double b  = 0.0; // s*sinθ
    double tx = 0.0;
    double ty = 0.0;
    bool   ok = false;

    double scale() const { return std::sqrt(a * a + b * b); }

    QTransform toQTransform() const
    {
        // QTransform(m11, m12, m21, m22, dx, dy)
        // Maps (x,y) → (m11*x + m21*y + dx, m12*x + m22*y + dy)
        // Our model: x' = a*x - b*y + tx,  y' = b*x + a*y + ty
        // → m11=a, m12=b, m21=-b, m22=a, dx=tx, dy=ty
        return QTransform(a, b, -b, a, tx, ty);
    }
};

static SimilarityResult fitSimilarity(const std::vector<QPointF> &src,
                                      const std::vector<QPointF> &dst)
{
    SimilarityResult res;
    const int N = static_cast<int>(src.size());
    if (N < 2) return res;

    // Accumulate normal-equation sums for ATA x = ATb,  A is (2N)×4.
    // Unknowns: [a, b, tx, ty]
    // Row for each point:
    //   [xi, -yi, 1, 0]  →  dst.x
    //   [yi,  xi, 0, 1]  →  dst.y
    //
    // ATA is 4×4 symmetric; ATb is 4×1.
    double ATA[4][4] = {{0}};
    double ATb[4]    = {0};

    for (int i = 0; i < N; ++i) {
        const double xi = src[i].x();
        const double yi = src[i].y();
        // Row 1: [xi, -yi, 1, 0]
        {
            double row[4] = {xi, -yi, 1.0, 0.0};
            const double rhs = dst[i].x();
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) ATA[r][c] += row[r] * row[c];
                ATb[r] += row[r] * rhs;
            }
        }
        // Row 2: [yi,  xi, 0, 1]
        {
            double row[4] = {yi, xi, 0.0, 1.0};
            const double rhs = dst[i].y();
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) ATA[r][c] += row[r] * row[c];
                ATb[r] += row[r] * rhs;
            }
        }
    }

    // Solve 4×4 system via Gaussian elimination with partial pivoting.
    // Augmented matrix [ATA | ATb].
    double M[4][5];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) M[r][c] = ATA[r][c];
        M[r][4] = ATb[r];
    }

    for (int col = 0; col < 4; ++col) {
        // Find pivot.
        int    pivot = col;
        double best  = std::abs(M[col][col]);
        for (int r = col + 1; r < 4; ++r) {
            if (std::abs(M[r][col]) > best) {
                best  = std::abs(M[r][col]);
                pivot = r;
            }
        }
        if (best < 1e-12) return res; // degenerate

        if (pivot != col) {
            for (int c = 0; c <= 4; ++c) std::swap(M[col][c], M[pivot][c]);
        }

        const double diag = M[col][col];
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double factor = M[r][col] / diag;
            for (int c = col; c <= 4; ++c) M[r][c] -= factor * M[col][c];
        }
    }

    res.a  = M[0][4] / M[0][0];
    res.b  = M[1][4] / M[1][1];
    res.tx = M[2][4] / M[2][2];
    res.ty = M[3][4] / M[3][3];
    res.ok = true;
    return res;
}

// Identity QTransform as a similarity result.
static QTransform identityTransform()
{
    return QTransform(); // Qt default is identity
}

// Compose two QTransforms (apply lhs then rhs).
// QTransform::operator* applies lhs then rhs when used as lhs * rhs.
// (Qt docs: "result = this * matrix", i.e. lhs is applied first.)
static QTransform compose(const QTransform &first, const QTransform &second)
{
    return first * second;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// applyTransformToPath
// ---------------------------------------------------------------------------

RotoPath applyTransformToPath(const RotoPath &path, const QTransform &xf)
{
    RotoPath result = path; // copy closed / feather

    // RotoPoint handleIn/handleOut are ABSOLUTE bezier control-point
    // coordinates (see Rotoscope.cpp: cubicTo(handleOut, handleIn, position)).
    // Therefore they are mapped through the full xf, just like position.
    for (qsizetype i = 0; i < path.points.size(); ++i) {
        const RotoPoint &src = path.points[i];
        RotoPoint       &dst = result.points[i];

        dst.position  = xf.map(src.position);
        dst.handleIn  = xf.map(src.handleIn);
        dst.handleOut = xf.map(src.handleOut);
    }

    return result;
}

// ---------------------------------------------------------------------------
// propagateRotoShape
// ---------------------------------------------------------------------------

QVector<RotoKeyframe> propagateRotoShape(const RotoPath    &seedPath,
                                          int                seedFrame,
                                          const QVector<QImage> &frames,
                                          int                firstFrameIndex,
                                          const RotoTrackParams &p)
{
    QVector<RotoKeyframe> keyframes;

    // Guard: nothing to do.
    if (frames.isEmpty() || seedPath.points.isEmpty()) {
        RotoKeyframe kf;
        kf.frameNumber = seedFrame;
        kf.path        = seedPath;
        keyframes.append(kf);
        return keyframes;
    }

    const int nFrames     = frames.size();
    const int lastAbsFrame = firstFrameIndex + nFrames - 1;

    // Seed frame relative index within the frames array.
    const int seedRelIndex = seedFrame - firstFrameIndex;
    // Clamp to valid range.
    const int seedRel = std::max(0, std::min(seedRelIndex, nFrames - 1));

    // Template half-size (patch = 15×15).
    const int half = 7;

    // Sample points from the seed path (anchor positions).
    std::vector<QPointF> samplePts = samplePoints(seedPath);

    // We only propagate forward from the seed frame.
    // (Backward propagation would require going from seedRel down to 0;
    //  the story spec says "for each consecutive frame pair" without specifying
    //  direction; we propagate forward only — from seedRel to nFrames-1.)

    // Cumulative transform from the seed frame (starts as identity).
    QTransform cumXf = identityTransform();

    // Emit the seed keyframe.
    {
        RotoKeyframe kf;
        kf.frameNumber = firstFrameIndex + seedRel;
        kf.path        = seedPath;
        keyframes.append(kf);
    }

    // Determine which absolute frame numbers need a keyframe.
    // Rule: seed + multiples of keyframeInterval + last frame.
    const int interval = std::max(1, p.keyframeInterval);

    // Helper: should we emit at this absolute frame?
    auto needsKeyframe = [&](int absFrame) -> bool {
        if (absFrame == lastAbsFrame) return true;
        const int delta = absFrame - (firstFrameIndex + seedRel);
        return (delta > 0) && (delta % interval == 0);
    };

    // Propagate forward from seedRel+1 to nFrames-1.
    for (int rel = seedRel + 1; rel < nFrames; ++rel) {
        const int prevRel = rel - 1;
        const QImage &prevFrame = frames[prevRel];
        const QImage &currFrame = frames[rel];

        // Per-frame delta transform (identity = hold).
        QTransform deltaXf = identityTransform();

        if (!prevFrame.isNull() && !currFrame.isNull()
            && prevFrame.width() > 0 && currFrame.width() > 0) {

            // For each sample point, transform by current cumXf to get the
            // position in the previous frame's coordinate system, then match.
            std::vector<QPointF> srcPts; // positions in prev frame
            std::vector<QPointF> dstPts; // matched positions in curr frame

            for (const QPointF &seedPt : samplePts) {
                // Position of this sample in the previous frame.
                QPointF prevPt = cumXf.map(seedPt);

                MatchResult m = matchTemplate(prevFrame, currFrame,
                                              prevPt.x(), prevPt.y(),
                                              half, p.searchMargin);

                if (m.score >= p.minConfidence) {
                    srcPts.push_back(prevPt);
                    dstPts.push_back(QPointF(prevPt.x() + m.dx,
                                             prevPt.y() + m.dy));
                }
            }

            if (static_cast<int>(srcPts.size()) >= 2) {
                SimilarityResult sim = fitSimilarity(srcPts, dstPts);
                if (sim.ok) {
                    const double s = sim.scale();
                    // Accept only if scale is within [1-maxScaleDelta, 1+maxScaleDelta].
                    if (std::abs(s - 1.0) <= p.maxScaleDelta) {
                        deltaXf = sim.toQTransform();
                    }
                }
            }
            // else: hold — deltaXf stays identity.
        }

        // Compose: cumulative from seed to curr = cumulative to prev * delta.
        cumXf = compose(cumXf, deltaXf);

        // Emit keyframe if needed.
        const int absFrame = firstFrameIndex + rel;
        if (needsKeyframe(absFrame)) {
            RotoKeyframe kf;
            kf.frameNumber = absFrame;
            kf.path        = applyTransformToPath(seedPath, cumXf);
            keyframes.append(kf);
        }
    }

    // Ensure frame numbers are strictly increasing (they are by construction
    // since rel is strictly increasing and we only emit each absFrame once,
    // but guard against the edge case where seedFrame == lastAbsFrame).
    // Remove any duplicate frame numbers (keep first occurrence).
    QVector<RotoKeyframe> deduped;
    deduped.reserve(keyframes.size());
    int lastEmitted = -1;
    for (const RotoKeyframe &kf : keyframes) {
        if (kf.frameNumber > lastEmitted) {
            deduped.append(kf);
            lastEmitted = kf.frameNumber;
        }
    }

    return deduped;
}

} // namespace rototrack
