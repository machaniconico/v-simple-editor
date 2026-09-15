#include "FeatureTracker.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace feattrack {
namespace {
struct Image {
    int w, h;
    std::vector<float> pixels;
    Image(int width, int height)
        : w(width), h(height), pixels(size_t(width) * size_t(height)) {}
    float& at(int x, int y) { return pixels[size_t(y) * w + x]; }
    float at(int x, int y) const { return pixels[size_t(y) * w + x]; }
    double sample(double x, double y) const {
        const int ix = int(x), iy = int(y);
        const double fx = x - ix, fy = y - iy;
        return (1 - fy) * ((1 - fx) * at(ix, iy) + fx * at(ix + 1, iy))
             + fy * ((1 - fx) * at(ix, iy + 1) + fx * at(ix + 1, iy + 1));
    }
};

Image fromQImage(const QImage& input)
{
    const QImage gray = input.convertToFormat(QImage::Format_Grayscale8);
    Image out(gray.width(), gray.height());
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            out.at(x, y) = gray.constScanLine(y)[x];
    return out;
}

Image blur(const Image& in)
{
    // Separable binomial Gaussian, replicated edges.
    constexpr int weights[] = {1, 4, 6, 4, 1};
    Image temp(in.w, in.h), out(in.w, in.h);
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x) {
            double sum = 0;
            for (int k = -2; k <= 2; ++k)
                sum += weights[k + 2] * in.at(std::clamp(x + k, 0, in.w - 1), y);
            temp.at(x, y) = float(sum / 16);
        }
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x) {
            double sum = 0;
            for (int k = -2; k <= 2; ++k)
                sum += weights[k + 2] * temp.at(x, std::clamp(y + k, 0, in.h - 1));
            out.at(x, y) = float(sum / 16);
        }
    return out;
}

std::vector<Image> pyramid(const QImage& gray, int levels)
{
    std::vector<Image> out;
    out.push_back(blur(fromQImage(gray)));
    while (int(out.size()) < levels && out.back().w >= 8 && out.back().h >= 8) {
        const Image filtered = blur(out.back());
        Image down((filtered.w + 1) / 2, (filtered.h + 1) / 2);
        for (int y = 0; y < down.h; ++y)
            for (int x = 0; x < down.w; ++x)
                down.at(x, y) = filtered.at(2 * x, 2 * y);
        out.push_back(std::move(down));
    }
    return out;
}

bool inside(const Image& image, const QPointF& p, int radius)
{
    return std::isfinite(p.x()) && std::isfinite(p.y())
        && p.x() >= radius + 1 && p.y() >= radius + 1
        && p.x() < image.w - radius - 2 && p.y() < image.h - radius - 2;
}

bool follow(const std::vector<Image>& prev, const std::vector<Image>& next,
            const QPointF& origin, QPointF& result, float& error,
            int radius, int maxIter, double eps)
{
    int top = int(prev.size()) - 1;
    while (top > 0 && !inside(prev[top], origin / std::ldexp(1.0, top), radius))
        --top;
    QPointF displacement;
    const int side = 2 * radius + 1;
    const size_t count = size_t(side) * side;
    std::vector<double> reference(count), gx(count), gy(count), weights(count);
    size_t wi = 0;
    // Emphasize the feature center so local rotation does not bias its
    // translation toward the edges of the window.
    const double sigma = std::max(1.0, radius / 3.0);
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x, ++wi)
            weights[wi] = std::exp(-(x*x + y*y) / (2*sigma*sigma));
    for (int level = top; level >= 0; --level) {
        const Image& a = prev[level];
        const Image& b = next[level];
        const QPointF p = origin / std::ldexp(1.0, level);
        if (!inside(a, p, radius)) return false;
        if (level != top) displacement *= 2;
        QPointF q = p + displacement;
        double xx = 0, xy = 0, yy = 0;
        size_t i = 0;
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x, ++i) {
                const double px = p.x() + x, py = p.y() + y;
                reference[i] = a.sample(px, py);
                gx[i] = (a.sample(px + 1, py) - a.sample(px - 1, py)) * 0.5;
                gy[i] = (a.sample(px, py + 1) - a.sample(px, py - 1)) * 0.5;
                xx += weights[i] * gx[i] * gx[i];
                xy += weights[i] * gx[i] * gy[i]; yy += weights[i] * gy[i] * gy[i];
            }
        const double det = xx * yy - xy * xy;
        if (det <= 1e-6 || det <= 1e-5 * (xx + yy) * (xx + yy)) return false;
        if (level == top) {
            // Seed LK inside its convergence basin for larger motion. Only the
            // coarsest level searches; every level still uses the LK normal equations.
            const auto cost = [&](const QPointF& center) {
                if (!inside(b, center, radius)) return std::numeric_limits<double>::infinity();
                double sum = 0;
                size_t index = 0;
                for (int y = -radius; y <= radius; ++y)
                    for (int x = -radius; x <= radius; ++x, ++index) {
                        const double d = reference[index] - b.sample(center.x()+x, center.y()+y);
                        sum += d*d;
                    }
                return sum;
            };
            QPointF best = q;
            double bestCost = cost(q);
            for (int y = -16; y <= 16; y += 2)
                for (int x = -16; x <= 16; x += 2) {
                    const QPointF trial = p + QPointF(x, y);
                    const double value = cost(trial);
                    if (value < bestCost) { bestCost = value; best = trial; }
                }
            const QPointF seed = best;
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) {
                    const QPointF trial = seed + QPointF(x, y);
                    const double value = cost(trial);
                    if (value < bestCost) { bestCost = value; best = trial; }
                }
            q = best;
        }
        bool converged = false;
        QPointF lastStep;
        for (int iter = 0; iter < maxIter; ++iter) {
            if (!inside(b, q, radius)) return false;
            double bx = 0, by = 0;
            i = 0;
            for (int y = -radius; y <= radius; ++y)
                for (int x = -radius; x <= radius; ++x, ++i) {
                    const double residual = reference[i] - b.sample(q.x() + x, q.y() + y);
                    bx += weights[i] * gx[i] * residual; by += weights[i] * gy[i] * residual;
                }
            const QPointF step((yy * bx - xy * by) / det, (xx * by - xy * bx) / det);
            q += step;
            if (std::hypot(step.x(), step.y()) <= eps) { converged = true; break; }
            // Bilinear sampling can alternate across the optimum; use its midpoint.
            if (iter > 0 && std::hypot(step.x() + lastStep.x(), step.y() + lastStep.y()) <= eps) {
                q -= step * 0.5; converged = true; break;
            }
            lastStep = step;
        }
        if (!inside(b, q, radius)) return false;
        displacement = q - p;
        if (level == 0) {
            double ssd = 0, sum = 0, squares = 0;
            i = 0;
            for (int y = -radius; y <= radius; ++y)
                for (int x = -radius; x <= radius; ++x, ++i) {
                    const double d = reference[i] - b.sample(q.x() + x, q.y() + y);
                    ssd += d * d;
                    sum += reference[i]; squares += reference[i]*reference[i];
                }
            error = float(ssd / double(count));
            result = q;
            // Reject both large absolute residuals and low-contrast mismatches.
            const double variance = std::max(0.0, squares / double(count)
                - (sum / double(count)) * (sum / double(count)));
            return converged && std::isfinite(error) && error <= 900.0f
                && error <= 0.2 * variance;
        }
    }
    return false;
}
} // namespace

QVector<QPointF> detectShiTomasi(const QImage& gray, int maxCorners,
                               double qualityLevel, double minDistance)
{
    QVector<QPointF> result;
    if (gray.isNull() || gray.width() < 7 || gray.height() < 7 || maxCorners <= 0
        || !std::isfinite(qualityLevel) || qualityLevel <= 0 || qualityLevel > 1
        || !std::isfinite(minDistance) || minDistance < 0) return result;
    const Image in = fromQImage(gray);
    Image xx(in.w, in.h), xy(in.w, in.h), yy(in.w, in.h), score(in.w, in.h);
    for (int y = 1; y < in.h - 1; ++y)
        for (int x = 1; x < in.w - 1; ++x) {
            const double gx = (in.at(x+1,y-1) + 2*in.at(x+1,y) + in.at(x+1,y+1)
                             - in.at(x-1,y-1) - 2*in.at(x-1,y) - in.at(x-1,y+1)) / 8.0;
            const double gy = (in.at(x-1,y+1) + 2*in.at(x,y+1) + in.at(x+1,y+1)
                             - in.at(x-1,y-1) - 2*in.at(x,y-1) - in.at(x+1,y-1)) / 8.0;
            xx.at(x,y) = float(gx*gx); xy.at(x,y) = float(gx*gy); yy.at(x,y) = float(gy*gy);
        }
    double peak = 0;
    constexpr int weight[] = {1, 2, 1};
    for (int y = 2; y < in.h - 2; ++y)
        for (int x = 2; x < in.w - 2; ++x) {
            double a = 0, b = 0, c = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const double w = weight[dx+1] * weight[dy+1] / 16.0;
                    a += w * xx.at(x+dx,y+dy); b += w * xy.at(x+dx,y+dy);
                    c += w * yy.at(x+dx,y+dy);
                }
            score.at(x,y) = float(std::max(0.0, (a+c-std::hypot(a-c, 2*b))*0.5));
            peak = std::max(peak, double(score.at(x,y)));
        }
    if (peak <= 0) return result;
    struct Candidate { int x, y; float score; };
    std::vector<Candidate> candidates;
    for (int y = 2; y < in.h - 2; ++y)
        for (int x = 2; x < in.w - 2; ++x) {
            const float s = score.at(x,y);
            if (s <= 0 || s < peak * qualityLevel) continue;
            bool maximum = true;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (score.at(x+dx,y+dy) > s) maximum = false;
            if (maximum) candidates.push_back({x,y,s});
        }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    for (const auto& c : candidates) {
        bool separated = true;
        for (const QPointF& p : result)
            if (std::hypot(p.x()-c.x, p.y()-c.y) < minDistance) { separated = false; break; }
        if (separated) result.push_back(QPointF(c.x,c.y));
        if (result.size() >= maxCorners) break;
    }
    return result;
}

void trackPyramidalLK(const QImage& prevGray, const QImage& nextGray,
                      QVector<Track>& tracks, int levels, int window, int maxIter,
                      double eps, bool forwardBackwardCheck, double forwardBackwardThreshold)
{
    const bool valid = !prevGray.isNull() && prevGray.size() == nextGray.size()
        && levels > 0 && window >= 3 && window <= std::min(prevGray.width(), prevGray.height())
        && maxIter > 0 && std::isfinite(eps) && eps > 0
        && std::isfinite(forwardBackwardThreshold) && forwardBackwardThreshold >= 0;
    if (!valid) {
        for (Track& t : tracks) if (t.alive) {
            t.alive = false; t.error = std::numeric_limits<float>::infinity();
        }
        return;
    }
    if (tracks.isEmpty()) return;
    const auto prev = pyramid(prevGray, levels), next = pyramid(nextGray, levels);
    for (Track& t : tracks) {
        if (!t.alive) continue;
        t.error = std::numeric_limits<float>::infinity();
        QPointF candidate;
        t.alive = follow(prev, next, t.pt, candidate, t.error, window/2, maxIter, eps);
        if (t.alive && forwardBackwardCheck) {
            QPointF back;
            float reverseError = std::numeric_limits<float>::infinity();
            t.alive = follow(next, prev, candidate, back, reverseError, window/2, maxIter, eps)
                && std::hypot(back.x()-t.pt.x(), back.y()-t.pt.y()) <= forwardBackwardThreshold;
        }
        if (t.alive) t.pt = candidate;
    }
}
} // namespace feattrack
