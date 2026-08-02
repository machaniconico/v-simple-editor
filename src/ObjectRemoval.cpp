#include "ObjectRemoval.h"

#include <QColor>
#include <QList>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <queue>
#include <vector>

namespace objremoval {
namespace {

struct RgbSample {
    int r = 0;
    int g = 0;
    int b = 0;
};

struct TemporalPixel {
    bool hasSample = false;
    bool trusted = false;
    int validCount = 0;
    int totalCount = 0;
    RgbSample median;
};

struct ImagePoint {
    int x = 0;
    int y = 0;
};

QImage normalizeMask(const QImage &mask, const QSize &size)
{
    if (mask.isNull() || size.isEmpty())
        return {};

    QImage result = mask.convertToFormat(QImage::Format_Grayscale8);
    if (result.size() != size)
        result = result.scaled(size, Qt::IgnoreAspectRatio,
                               Qt::FastTransformation);
    return result;
}

QImage normalizeImage(const QImage &image)
{
    if (image.isNull())
        return {};
    return image.convertToFormat(QImage::Format_ARGB32);
}

RgbSample rgbAt(const QImage &image, int x, int y)
{
    const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    const QRgb value = row[x];
    return {qRed(value), qGreen(value), qBlue(value)};
}

QRgb toRgb(const RgbSample &sample)
{
    return qRgb(qBound(0, sample.r, 255),
                qBound(0, sample.g, 255),
                qBound(0, sample.b, 255));
}

int roundedAverage(int a, int b)
{
    return static_cast<int>(std::lround((static_cast<double>(a) + b) / 2.0));
}

int medianChannel(QVector<int> values)
{
    if (values.isEmpty())
        return 0;

    std::sort(values.begin(), values.end());
    const int count = static_cast<int>(values.size());
    const int middle = count / 2;
    if ((count & 1) != 0)
        return values[middle];
    return roundedAverage(values[middle - 1], values[middle]);
}

bool finitePoint(const QPointF &point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool sampleBilinear(const QImage &image, const QPointF &point,
                    RgbSample *out)
{
    if (!out || image.isNull() || !finitePoint(point))
        return false;
    if (point.x() < 0.0 || point.y() < 0.0
        || point.x() > image.width() - 1.0
        || point.y() > image.height() - 1.0) {
        return false;
    }

    const int x0 = qBound(0, static_cast<int>(std::floor(point.x())),
                          image.width() - 1);
    const int y0 = qBound(0, static_cast<int>(std::floor(point.y())),
                          image.height() - 1);
    const int x1 = qMin(image.width() - 1, x0 + 1);
    const int y1 = qMin(image.height() - 1, y0 + 1);
    const double fx = point.x() - x0;
    const double fy = point.y() - y0;

    const RgbSample a = rgbAt(image, x0, y0);
    const RgbSample b = rgbAt(image, x1, y0);
    const RgbSample c = rgbAt(image, x0, y1);
    const RgbSample d = rgbAt(image, x1, y1);
    const auto interpolate = [fx, fy](int av, int bv, int cv, int dv) {
        const double top = av + (bv - av) * fx;
        const double bottom = cv + (dv - cv) * fx;
        return static_cast<int>(std::lround(top + (bottom - top) * fy));
    };

    out->r = interpolate(a.r, b.r, c.r, d.r);
    out->g = interpolate(a.g, b.g, c.g, d.g);
    out->b = interpolate(a.b, b.b, c.b, d.b);
    return true;
}

QPointF offsetForNeighbor(const QVector<QPointF> &offsets, int index)
{
    const int offsetCount = static_cast<int>(offsets.size());
    if (index < 0 || index >= offsetCount || !finitePoint(offsets[index]))
        return {};
    return offsets[index];
}

TemporalPixel temporalPixel(const QImage &target,
                            int x,
                            int y,
                            const QVector<QImage> &neighbors,
                            const QVector<QPointF> &neighborOffsets,
                            const ObjectRemovalParams &params)
{
    TemporalPixel result;
    const int stride = qMax(1, params.temporalStride);
    const int neighborCount = static_cast<int>(neighbors.size());
    result.totalCount = (neighborCount + stride - 1) / stride;
    if (target.isNull() || neighborCount <= 0
        || x < 0 || y < 0 || x >= target.width() || y >= target.height()) {
        return result;
    }

    QVector<int> rs;
    QVector<int> gs;
    QVector<int> bs;
    rs.reserve(neighborCount);
    gs.reserve(neighborCount);
    bs.reserve(neighborCount);

    for (int i = 0; i < neighborCount; i += stride) {
        const QImage &neighbor = neighbors[i];
        const QPointF offset = params.useBackgroundAlignment
            ? offsetForNeighbor(neighborOffsets, i) : QPointF();

        // The offset is the background/camera displacement needed to map
        // target p to the neighbor's source pixel p + offset. An object-mask
        // displacement is not valid here.
        RgbSample sample;
        if (!sampleBilinear(neighbor,
                            QPointF(static_cast<double>(x) + offset.x(),
                                    static_cast<double>(y) + offset.y()),
                            &sample)) {
            continue;
        }
        ++result.validCount;
        rs.append(sample.r);
        gs.append(sample.g);
        bs.append(sample.b);
    }

    if (result.validCount <= 0)
        return result;

    result.hasSample = true;
    result.median = {
        medianChannel(rs),
        medianChannel(gs),
        medianChannel(bs)
    };

    const double ratio = static_cast<double>(result.validCount)
        / static_cast<double>(qMax(1, result.totalCount));
    result.trusted = ratio >= qBound(0.0, params.temporalTrustThreshold, 1.0);
    return result;
}

void writeRgb(QImage &image, int x, int y, const RgbSample &sample)
{
    const QRgb old = image.pixel(x, y);
    const int r = qBound(0, sample.r, 255);
    const int g = qBound(0, sample.g, 255);
    const int b = qBound(0, sample.b, 255);

    if (image.format() == QImage::Format_ARGB32) {
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        row[x] = qRgba(r, g, b, qAlpha(old));
        return;
    }
    if (image.format() == QImage::Format_RGB888) {
        uchar *row = image.scanLine(y);
        row[x * 3 + 0] = static_cast<uchar>(r);
        row[x * 3 + 1] = static_cast<uchar>(g);
        row[x * 3 + 2] = static_cast<uchar>(b);
        return;
    }

    QColor color = image.pixelColor(x, y);
    color.setRgb(r, g, b, color.alpha());
    image.setPixelColor(x, y, color);
}

RgbSample readRgb(const QImage &image, int x, int y)
{
    if (image.format() == QImage::Format_ARGB32)
        return rgbAt(image, x, y);
    const QColor color = image.pixelColor(x, y);
    return {color.red(), color.green(), color.blue()};
}

QVector<int> makeDistanceField(const QImage &mask,
                               QVector<ImagePoint> *nearestBoundary)
{
    const int w = mask.width();
    const int h = mask.height();
    QVector<int> distances(w * h, -1);
    nearestBoundary->resize(w * h);
    std::queue<int> queue;

    const auto masked = [&mask](int x, int y) {
        return mask.constScanLine(y)[x] > 0;
    };
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!masked(x, y))
                continue;

            bool boundary = false;
            for (int direction = 0; direction < 4; ++direction) {
                const int nx = x + dx[direction];
                const int ny = y + dy[direction];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h
                    || !masked(nx, ny)) {
                    boundary = true;
                    break;
                }
            }
            if (!boundary)
                continue;

            const int index = y * w + x;
            distances[index] = 0;
            (*nearestBoundary)[index] = {x, y};
            queue.push(index);
        }
    }

    while (!queue.empty()) {
        const int index = queue.front();
        queue.pop();
        const int x = index % w;
        const int y = index / w;
        for (int direction = 0; direction < 4; ++direction) {
            const int nx = x + dx[direction];
            const int ny = y + dy[direction];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h || !masked(nx, ny))
                continue;
            const int next = ny * w + nx;
            if (distances[next] >= 0)
                continue;
            distances[next] = distances[index] + 1;
            (*nearestBoundary)[next] = (*nearestBoundary)[index];
            queue.push(next);
        }
    }
    return distances;
}

QPointF outwardNormal(const QImage &mask, int x, int y,
                      const ImagePoint &boundary)
{
    QPointF normal(static_cast<double>(boundary.x - x),
                   static_cast<double>(boundary.y - y));
    if (!qFuzzyIsNull(normal.x()) || !qFuzzyIsNull(normal.y())) {
        const double length = std::hypot(normal.x(), normal.y());
        return normal / length;
    }

    const int w = mask.width();
    const int h = mask.height();
    QPointF sum;
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};
    for (int direction = 0; direction < 4; ++direction) {
        const int nx = x + dx[direction];
        const int ny = y + dy[direction];
        if (nx < 0 || nx >= w || ny < 0 || ny >= h
            || mask.constScanLine(ny)[nx] == 0) {
            sum += QPointF(static_cast<double>(dx[direction]),
                           static_cast<double>(dy[direction]));
        }
    }
    const double length = std::hypot(sum.x(), sum.y());
    return length > 0.0 ? sum / length : QPointF();
}

RgbSample averageResolved(const QVector<RgbSample> &colors,
                          const QVector<bool> &resolved,
                          const RgbSample &fallback)
{
    qint64 r = 0;
    qint64 g = 0;
    qint64 b = 0;
    qint64 count = 0;
    const int colorCount = static_cast<int>(colors.size());
    for (int i = 0; i < colorCount; ++i) {
        if (!resolved[i])
            continue;
        r += colors[i].r;
        g += colors[i].g;
        b += colors[i].b;
        ++count;
    }
    if (count <= 0)
        return fallback;
    return {
        static_cast<int>(std::lround(static_cast<double>(r) / count)),
        static_cast<int>(std::lround(static_cast<double>(g) / count)),
        static_cast<int>(std::lround(static_cast<double>(b) / count))
    };
}

QImage dilatedAndFeatheredMask(const QImage &mask,
                               int dilatePx,
                               int featherPx)
{
    if (mask.isNull())
        return {};

    const QImage sourceMask = mask.convertToFormat(QImage::Format_Grayscale8);
    const int w = sourceMask.width();
    const int h = sourceMask.height();
    const int radius = qMax(0, dilatePx);

    // A separable max filter gives the same bounded square dilation in O(w*h)
    // time. The sliding deque keeps each pixel out of the inner max scan.
    QImage dilationHorizontal(sourceMask.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        const uchar *source = sourceMask.constScanLine(y);
        uchar *destination = dilationHorizontal.scanLine(y);
        std::deque<int> candidates;
        for (int x = 0; x < w; ++x) {
            while (!candidates.empty()
                   && source[candidates.back()] <= source[x]) {
                candidates.pop_back();
            }
            candidates.push_back(x);

            const int center = x - radius;
            if (center < 0)
                continue;
            const int left = center - radius;
            while (!candidates.empty() && candidates.front() < left)
                candidates.pop_front();
            destination[center] = candidates.empty()
                ? 0 : source[candidates.front()];
        }

        for (int center = qMax(0, w - radius); center < w; ++center) {
            const int left = center - radius;
            while (!candidates.empty() && candidates.front() < left)
                candidates.pop_front();
            destination[center] = candidates.empty()
                ? 0 : source[candidates.front()];
        }
    }

    QImage dilated(sourceMask.size(), QImage::Format_Grayscale8);
    for (int x = 0; x < w; ++x) {
        std::deque<int> candidates;
        for (int y = 0; y < h; ++y) {
            while (!candidates.empty()
                   && dilationHorizontal.constScanLine(candidates.back())[x]
                       <= dilationHorizontal.constScanLine(y)[x]) {
                candidates.pop_back();
            }
            candidates.push_back(y);

            const int center = y - radius;
            if (center < 0)
                continue;
            const int top = center - radius;
            while (!candidates.empty() && candidates.front() < top)
                candidates.pop_front();
            dilated.scanLine(center)[x] = candidates.empty()
                ? 0 : dilationHorizontal.constScanLine(candidates.front())[x];
        }

        for (int center = qMax(0, h - radius); center < h; ++center) {
            const int top = center - radius;
            while (!candidates.empty() && candidates.front() < top)
                candidates.pop_front();
            dilated.scanLine(center)[x] = candidates.empty()
                ? 0 : dilationHorizontal.constScanLine(candidates.front())[x];
        }
    }

    if (featherPx <= 0)
        return dilated;

    const double sigma = qMax(0.5, static_cast<double>(featherPx) * 0.5);
    const int blurRadius = qMax(1, static_cast<int>(std::ceil(sigma * 2.5)));
    QVector<double> kernel(2 * blurRadius + 1);
    for (int offset = -blurRadius; offset <= blurRadius; ++offset) {
        const double distance = static_cast<double>(offset);
        kernel[offset + blurRadius] = std::exp(
            -(distance * distance) / (2.0 * sigma * sigma));
    }

    QImage horizontal(dilated.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        const uchar *source = dilated.constScanLine(y);
        uchar *destination = horizontal.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double sum = 0.0;
            double weightSum = 0.0;
            for (int ox = -blurRadius; ox <= blurRadius; ++ox) {
                const int sx = x + ox;
                if (sx < 0 || sx >= w)
                    continue;
                const double weight = kernel[ox + blurRadius];
                sum += source[sx] * weight;
                weightSum += weight;
            }
            destination[x] = static_cast<uchar>(weightSum > 0.0
                ? qBound(0, static_cast<int>(std::lround(sum / weightSum)), 255)
                : 0);
        }
    }

    QImage result(dilated.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar *destination = result.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double sum = 0.0;
            double weightSum = 0.0;
            for (int oy = -blurRadius; oy <= blurRadius; ++oy) {
                const int sy = y + oy;
                if (sy < 0 || sy >= h)
                    continue;
                const double weight = kernel[oy + blurRadius];
                sum += horizontal.constScanLine(sy)[x] * weight;
                weightSum += weight;
            }
            destination[x] = static_cast<uchar>(weightSum > 0.0
                ? qBound(0, static_cast<int>(std::lround(sum / weightSum)), 255)
                : 0);
        }
    }
    return result;
}

} // namespace

ObjectRemovalNeighborSet collectTemporalNeighbors(
    int frameIndex,
    int frameCount,
    int temporalRadius,
    const ObjectRemovalTemporalSources &sources,
    bool useBackgroundAlignment)
{
    ObjectRemovalNeighborSet result;
    const int boundedFrameCount = qMax(1, frameCount);
    const int radius = qMax(0, temporalRadius);
    if (!sources.frameFetcher || frameIndex < 0
        || frameIndex >= boundedFrameCount) {
        return result;
    }

    // Mask/object tracking is intentionally not consulted here. It changes
    // the mask shape, but it does not describe the background sampling grid.
    (void)sources.maskTrackingOffsetFetcher;

    const bool hasBackgroundAlignment = useBackgroundAlignment
        && static_cast<bool>(sources.backgroundAlignmentOffsetFetcher);
    const QPointF targetOffset = hasBackgroundAlignment
        ? sources.backgroundAlignmentOffsetFetcher(frameIndex) : QPointF();
    const QPointF safeTargetOffset = finitePoint(targetOffset)
        ? targetOffset : QPointF();

    for (int delta = -radius; delta <= radius; ++delta) {
        if (delta == 0)
            continue;
        const int neighborIndex = frameIndex + delta;
        if (neighborIndex < 0 || neighborIndex >= boundedFrameCount)
            continue;

        result.neighbors.append(sources.frameFetcher(neighborIndex));
        if (!hasBackgroundAlignment) {
            result.backgroundOffsets.append(QPointF());
            continue;
        }

        const QPointF neighborOffset =
            sources.backgroundAlignmentOffsetFetcher(neighborIndex);
        result.backgroundOffsets.append(finitePoint(neighborOffset)
                                             ? neighborOffset - safeTargetOffset
                                             : QPointF());
    }
    return result;
}

void trimObjectRemovalFrameCache(QHash<int, QImage> &cache,
                                 int centerFrame,
                                 int frameCount,
                                 int temporalRadius)
{
    const int boundedFrameCount = qMax(1, frameCount);
    const int lastFrame = boundedFrameCount - 1;
    const int center = qBound(0, centerFrame, lastFrame);
    const int radius = qMax(0, temporalRadius);
    const int firstNeeded = qMax(0, center - radius);
    const int lastNeeded = qMin(lastFrame, center + radius);

    const QList<int> keys = cache.keys();
    for (const int key : keys) {
        if (key < firstNeeded || key > lastNeeded)
            cache.remove(key);
    }
}

QImage inpaintSpatial(const QImage &image, const QImage &mask,
                      const ObjectRemovalParams &params)
{
    if (image.isNull() || mask.isNull() || image.size() != mask.size())
        return image.copy();

    const QImage imageArgb = normalizeImage(image);
    const QImage maskGray = mask.convertToFormat(QImage::Format_Grayscale8);
    const int w = image.width();
    const int h = image.height();
    const int pixelCount = w * h;

    QVector<RgbSample> colors(pixelCount);
    QVector<bool> resolved(pixelCount, false);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int index = y * w + x;
            colors[index] = rgbAt(imageArgb, x, y);
            resolved[index] = maskGray.constScanLine(y)[x] == 0;
        }
    }

    QVector<ImagePoint> nearestBoundary;
    QVector<int> distances = makeDistanceField(maskGray, &nearestBoundary);
    QVector<int> maskedPixels;
    maskedPixels.reserve(pixelCount);
    for (int index = 0; index < pixelCount; ++index) {
        if (!resolved[index])
            maskedPixels.append(index);
    }
    if (maskedPixels.isEmpty())
        return image.copy();

    std::sort(maskedPixels.begin(), maskedPixels.end(),
              [&distances, w](int a, int b) {
                  const int da = distances[a] >= 0
                      ? distances[a] : std::numeric_limits<int>::max();
                  const int db = distances[b] >= 0
                      ? distances[b] : std::numeric_limits<int>::max();
                  if (da != db)
                      return da < db;
                  const int ay = a / w;
                  const int by = b / w;
                  if (ay != by)
                      return ay < by;
                  return (a % w) < (b % w);
              });

    const int radius = qMax(1, params.spatialRadius);
    const RgbSample sourceAverage = averageResolved(
        colors, resolved, colors[maskedPixels.first()]);

    for (const int index : maskedPixels) {
        const int x = index % w;
        const int y = index / w;
        const QPointF normal = outwardNormal(maskGray, x, y,
                                              nearestBoundary[index]);
        double sumWeight = 0.0;
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;

        const int x0 = qMax(0, x - radius);
        const int x1 = qMin(w - 1, x + radius);
        const int y0 = qMax(0, y - radius);
        const int y1 = qMin(h - 1, y + radius);
        for (int sy = y0; sy <= y1; ++sy) {
            for (int sx = x0; sx <= x1; ++sx) {
                const int sourceIndex = sy * w + sx;
                if (!resolved[sourceIndex])
                    continue;
                const double distance = std::hypot(
                    static_cast<double>(sx - x),
                    static_cast<double>(sy - y));
                if (distance <= 0.0 || distance > radius)
                    continue;

                double directionWeight = 1.0;
                if (!qFuzzyIsNull(normal.x()) || !qFuzzyIsNull(normal.y())) {
                    const double dot =
                        ((sx - x) * normal.x() + (sy - y) * normal.y())
                        / distance;
                    directionWeight = 0.25 + 0.75 * qMax(0.0, dot);
                }
                const double weight = directionWeight / (distance + 1.0e-6);
                sumWeight += weight;
                r += colors[sourceIndex].r * weight;
                g += colors[sourceIndex].g * weight;
                b += colors[sourceIndex].b * weight;
            }
        }

        if (sumWeight <= 0.0) {
            colors[index] = sourceAverage;
        } else {
            colors[index] = {
                qBound(0, static_cast<int>(std::lround(r / sumWeight)), 255),
                qBound(0, static_cast<int>(std::lround(g / sumWeight)), 255),
                qBound(0, static_cast<int>(std::lround(b / sumWeight)), 255)
            };
        }
        resolved[index] = true;
    }

    QImage result = image.copy();
    for (const int index : maskedPixels)
        writeRgb(result, index % w, index / w, colors[index]);
    return result;
}

QImage removeObject(const QImage &target,
                    const QImage &targetMask,
                    const QVector<QImage> &neighbors,
                    const QVector<QPointF> &neighborOffsets,
                    const ObjectRemovalParams &params)
{
    if (target.isNull())
        return {};
    if (targetMask.isNull())
        return target.copy();

    const QImage mask = normalizeMask(targetMask, target.size());
    if (mask.isNull())
        return target.copy();

    QImage restored = target.copy();
    const QImage targetArgb = normalizeImage(target);
    QImage restoredArgb = targetArgb.copy();
    QVector<QImage> neighborArgb;
    neighborArgb.reserve(static_cast<int>(neighbors.size()));
    for (const QImage &neighbor : neighbors)
        neighborArgb.append(normalizeImage(neighbor));
    QImage fallbackMask(target.size(), QImage::Format_Grayscale8);
    fallbackMask.fill(0);

    for (int y = 0; y < target.height(); ++y) {
        uchar *fallbackRow = fallbackMask.scanLine(y);
        for (int x = 0; x < target.width(); ++x) {
            if (mask.constScanLine(y)[x] == 0)
                continue;
            const TemporalPixel temporal = temporalPixel(
                targetArgb, x, y, neighborArgb, neighborOffsets, params);
            if (temporal.trusted) {
                writeRgb(restoredArgb, x, y, temporal.median);
            } else {
                fallbackRow[x] = 255;
            }
        }
    }

    // The temporal pixels live in the same format as the target here, but
    // the helper may need to write to RGB888 or a future supported format.
    // Copy the resolved ARGB values back before the spatial pass.
    if (restored.format() == QImage::Format_ARGB32) {
        restored = restoredArgb;
    } else {
        for (int y = 0; y < target.height(); ++y) {
            for (int x = 0; x < target.width(); ++x) {
                if (fallbackMask.constScanLine(y)[x] == 0
                    && mask.constScanLine(y)[x] > 0) {
                    writeRgb(restored, x, y, rgbAt(restoredArgb, x, y));
                }
            }
        }
    }

    if (!fallbackMask.isNull())
        restored = inpaintSpatial(restored, fallbackMask, params);

    const QImage softMask = dilatedAndFeatheredMask(
        mask, qMax(0, params.maskDilatePx), qMax(0, params.featherPx));
    QImage result = target.copy();
    for (int y = 0; y < target.height(); ++y) {
        const uchar *sourceMask = mask.constScanLine(y);
        const uchar *blendMask = softMask.constScanLine(y);
        for (int x = 0; x < target.width(); ++x) {
            if (sourceMask[x] == 0)
                continue; // strict outside-mask byte preservation
            const int alpha = qMin<int>(sourceMask[x], blendMask[x]);
            if (alpha <= 0)
                continue;

            const RgbSample before = readRgb(target, x, y);
            const RgbSample after = readRgb(restored, x, y);
            const RgbSample blended = {
                (before.r * (255 - alpha) + after.r * alpha + 127) / 255,
                (before.g * (255 - alpha) + after.g * alpha + 127) / 255,
                (before.b * (255 - alpha) + after.b * alpha + 127) / 255
            };
            writeRgb(result, x, y, blended);
        }
    }
    return result;
}

QImage temporalCoverageMap(const QImage &targetMask,
                           const QVector<QImage> &neighbors,
                           const QVector<QPointF> &neighborOffsets,
                           const ObjectRemovalParams &params)
{
    if (targetMask.isNull())
        return {};

    const QImage mask = targetMask.convertToFormat(QImage::Format_Grayscale8);
    QImage result(mask.size(), QImage::Format_Grayscale8);
    result.fill(0);
    if (neighbors.isEmpty())
        return result;

    bool hasUnmaskedPixel = false;
    for (int y = 0; y < mask.height() && !hasUnmaskedPixel; ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (row[x] == 0) {
                hasUnmaskedPixel = true;
                break;
            }
        }
    }
    if (!hasUnmaskedPixel)
        return result;

    // The map reports both geometric coverage and a conservative content
    // guard. When every temporal sample at a masked pixel disagrees strongly
    // with the nearest unmasked temporal sample, the references are likely
    // still showing the object at that position, so the pixel is reported as
    // spatial-fallback territory.
    QVector<QImage> neighborArgb;
    const int neighborCount = static_cast<int>(neighbors.size());
    neighborArgb.reserve(neighborCount);
    for (const QImage &neighbor : neighbors) {
        const QImage normalized = normalizeImage(neighbor);
        // Coverage is defined on the target-mask grid. A mismatched neighbor
        // is retained as an invalid sample so it cannot be read out of bounds
        // or inflate the valid ratio through implicit coordinate reuse.
        neighborArgb.append(normalized.isNull()
                                || normalized.size() != mask.size()
                            ? QImage() : normalized);
    }
    QImage coverageTarget(mask.size(), QImage::Format_ARGB32);
    coverageTarget.fill(Qt::black);
    const auto colorDistance = [](const RgbSample &a, const RgbSample &b) {
        return (std::abs(a.r - b.r) + std::abs(a.g - b.g)
                + std::abs(a.b - b.b)) / 3.0;
    };
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = result.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (mask.constScanLine(y)[x] == 0)
                continue;
            const TemporalPixel temporal = temporalPixel(
                coverageTarget, x, y, neighborArgb, neighborOffsets, params);
            const double ratio = static_cast<double>(temporal.validCount)
                / static_cast<double>(qMax(1, temporal.totalCount));
            bool likelyCovered = false;
            const int dx[4] = {1, -1, 0, 0};
            const int dy[4] = {0, 0, 1, -1};
            for (int direction = 0; direction < 4 && !likelyCovered; ++direction) {
                for (int step = 1; step <= qMax(mask.width(), mask.height()); ++step) {
                    const int sx = x + dx[direction] * step;
                    const int sy = y + dy[direction] * step;
                    if (sx < 0 || sx >= mask.width()
                        || sy < 0 || sy >= mask.height()) {
                        break;
                    }
                    if (mask.constScanLine(sy)[sx] != 0)
                        continue;
                    const TemporalPixel outside = temporalPixel(
                        coverageTarget, sx, sy, neighborArgb, neighborOffsets,
                        params);
                    if (temporal.hasSample && outside.hasSample
                        && colorDistance(temporal.median, outside.median) >= 30.0) {
                        likelyCovered = true;
                    }
                    break;
                }
            }
            row[x] = likelyCovered
                ? 0
                : static_cast<uchar>(qBound(
                    0, static_cast<int>(std::lround(ratio * 255.0)), 255));
        }
    }
    return result;
}

} // namespace objremoval
