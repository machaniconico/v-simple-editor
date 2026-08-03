#include "../ObjectRemoval.h"

#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int kWidth = 128;
constexpr int kHeight = 96;
constexpr int kFrameCount = 20;

QImage makeBackground()
{
    QImage image(kWidth, kHeight, QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int r = (24 + x * 2 + y) & 255;
            const int g = (18 + x + y * 2) & 255;
            const int b = (42 + x * 3 + y * 2) & 255;
            image.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    return image;
}

QImage makeHighFrequencyBackground()
{
    QImage image(kWidth, kHeight, QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int r = (x * 73 + y * 151 + (x ^ (y * 3))) & 255;
            const int g = (x * 191 + y * 47 + ((x * y) & 31)) & 255;
            const int b = ((x * 29) ^ (y * 113) ^ (x * y * 7)) & 255;
            image.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    return image;
}

QImage makeFlatBackground()
{
    QImage image(kWidth, kHeight, QImage::Format_RGB888);
    image.fill(QColor(36, 112, 184));
    return image;
}

QRect movingRect(int frameIndex)
{
    return QRect(5 + frameIndex * 7, 34, 7, 20);
}

QVector<QImage> makeSequence(const QImage &background)
{
    QVector<QImage> frames;
    frames.reserve(kFrameCount);
    for (int frameIndex = 0; frameIndex < kFrameCount; ++frameIndex) {
        QImage frame = background.copy();
        const QRect rect = movingRect(frameIndex);
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            for (int x = rect.left(); x <= rect.right(); ++x) {
                if (x >= 0 && x < frame.width() && y >= 0 && y < frame.height())
                    frame.setPixelColor(x, y, QColor(232, 35, 28));
            }
        }
        frames.append(frame);
    }
    return frames;
}

QImage rectMask(const QRect &rect)
{
    QImage mask(kWidth, kHeight, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = rect.left(); x <= rect.right(); ++x) {
            if (x >= 0 && x < mask.width() && y >= 0 && y < mask.height())
                row[x] = 255;
        }
    }
    return mask;
}

QVector<QImage> makeNeighbors(const QVector<QImage> &frames, int targetIndex,
                              int radius)
{
    QVector<QImage> neighbors;
    const int frameCount = static_cast<int>(frames.size());
    for (int delta = -radius; delta <= radius; ++delta) {
        if (delta == 0)
            continue;
        const int index = targetIndex + delta;
        if (index >= 0 && index < frameCount)
            neighbors.append(frames[index]);
    }
    return neighbors;
}

double maskedMae(const QImage &actual, const QImage &expected, const QImage &mask)
{
    if (actual.isNull() || expected.isNull() || mask.isNull()
        || actual.size() != expected.size() || actual.size() != mask.size()) {
        return 255.0;
    }

    const QImage aa = actual.convertToFormat(QImage::Format_RGB888);
    const QImage ee = expected.convertToFormat(QImage::Format_RGB888);
    const QImage mm = mask.convertToFormat(QImage::Format_Grayscale8);
    double error = 0.0;
    qint64 count = 0;
    for (int y = 0; y < mm.height(); ++y) {
        for (int x = 0; x < mm.width(); ++x) {
            if (mm.constScanLine(y)[x] == 0)
                continue;
            const QColor a = aa.pixelColor(x, y);
            const QColor e = ee.pixelColor(x, y);
            error += (std::abs(a.red() - e.red())
                      + std::abs(a.green() - e.green())
                      + std::abs(a.blue() - e.blue())) / 3.0;
            ++count;
        }
    }
    return count > 0 ? error / static_cast<double>(count) : 255.0;
}

bool outsideBitIdentical(const QImage &before, const QImage &after,
                         const QImage &mask)
{
    if (before.size() != after.size() || before.format() != after.format()
        || mask.isNull() || mask.size() != before.size())
        return false;
    for (int y = 0; y < before.height(); ++y) {
        const uchar *maskRow = mask.constScanLine(y);
        for (int x = 0; x < before.width(); ++x) {
            if (maskRow[x] != 0)
                continue;
            if (before.pixel(x, y) != after.pixel(x, y))
                return false;
        }
    }
    return true;
}

QImage shiftImage(const QImage &source, int dx, int dy)
{
    QImage shifted(source.size(), source.format());
    shifted.fill(Qt::black);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            const int sx = x - dx;
            const int sy = y - dy;
            if (sx >= 0 && sx < source.width() && sy >= 0 && sy < source.height())
                shifted.setPixelColor(x, y, source.pixelColor(sx, sy));
        }
    }
    return shifted;
}

double edgeJump(const QImage &filled, const QImage &original,
                const QImage &mask)
{
    const QImage a = filled.convertToFormat(QImage::Format_RGB888);
    const QImage b = original.convertToFormat(QImage::Format_RGB888);
    const QImage m = mask.convertToFormat(QImage::Format_Grayscale8);
    double sum = 0.0;
    int count = 0;
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};
    for (int y = 0; y < m.height(); ++y) {
        for (int x = 0; x < m.width(); ++x) {
            const bool inside = m.constScanLine(y)[x] > 0;
            if (!inside)
                continue;
            const QColor in = a.pixelColor(x, y);
            const double inLuma = 0.2126 * in.red() + 0.7152 * in.green()
                + 0.0722 * in.blue();
            for (int direction = 0; direction < 4; ++direction) {
                const int nx = x + dx[direction];
                const int ny = y + dy[direction];
                if (nx < 0 || nx >= m.width() || ny < 0 || ny >= m.height()
                    || m.constScanLine(ny)[nx] != 0) {
                    continue;
                }
                const QColor out = b.pixelColor(nx, ny);
                const double outLuma = 0.2126 * out.red()
                    + 0.7152 * out.green() + 0.0722 * out.blue();
                sum += std::abs(inLuma - outLuma);
                ++count;
            }
        }
    }
    return count > 0 ? sum / count : 255.0;
}

} // namespace

int runObjectRemovalSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[object-removal] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[object-removal] FAIL G%1 %2%3")
                .arg(gate).arg(QString::fromLatin1(name))
                .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
        }
    };

    const QImage background = makeBackground();
    const QVector<QImage> frames = makeSequence(background);
    const int targetIndex = 10;
    const QRect targetRect = movingRect(targetIndex);
    const QImage mask = rectMask(targetRect);

    objremoval::ObjectRemovalParams params;
    params.temporalRadius = 8;
    params.temporalStride = 1;
    params.featherPx = 0;
    params.maskDilatePx = 0;
    params.spatialRadius = 24;

    const QVector<QImage> neighbors = makeNeighbors(frames, targetIndex, 8);
    const int neighborCount = static_cast<int>(neighbors.size());
    const QVector<QPointF> zeroOffsets(neighborCount, QPointF());

    objremoval::ObjectRemovalTemporalSources uiSources;
    uiSources.frameFetcher = [frames](int index) { return frames.value(index); };
    uiSources.maskTrackingOffsetFetcher = [](int index) {
        return QPointF(index * 7.0, 0.0);
    };
    const objremoval::ObjectRemovalNeighborSet uiNeighborSet =
        objremoval::collectTemporalNeighbors(
            targetIndex, kFrameCount, params.temporalRadius, uiSources, false);
    const QImage uiRestored = objremoval::removeObject(
        frames[targetIndex], mask, uiNeighborSet.neighbors,
        uiNeighborSet.backgroundOffsets, params);
    const double uiMae = maskedMae(uiRestored, background, mask);
    bool uiOffsetsAreZero = static_cast<int>(uiNeighborSet.neighbors.size())
        == static_cast<int>(uiNeighborSet.backgroundOffsets.size())
        && !uiNeighborSet.neighbors.isEmpty();
    for (const QPointF &offset : uiNeighborSet.backgroundOffsets) {
        if (!qFuzzyIsNull(offset.x()) || !qFuzzyIsNull(offset.y()))
            uiOffsetsAreZero = false;
    }
    check(1, "dialog neighbor path ignores object tracking offsets",
          uiMae < 2.0 && uiOffsetsAreZero,
          QStringLiteral("MAE=%1 offsetsZero=%2")
              .arg(uiMae, 0, 'f', 3).arg(uiOffsetsAreZero));

    const QImage restored = objremoval::removeObject(
        frames[targetIndex], mask, neighbors, zeroOffsets, params);
    const double mae = maskedMae(restored, background, mask);
    check(2, "moving rectangle temporal reconstruction", mae < 2.0,
          QStringLiteral("MAE=%1").arg(mae, 0, 'f', 3));

    check(3, "outside mask bit preservation",
          outsideBitIdentical(frames[targetIndex], restored, mask));

    QVector<QImage> contaminated = neighbors;
    const int contaminatedCount = static_cast<int>(contaminated.size());
    for (int i = 0; i < 3 && i < contaminatedCount; ++i)
        contaminated[i].fill(Qt::white);
    const QImage medianRestored = objremoval::removeObject(
        frames[targetIndex], mask, contaminated,
        QVector<QPointF>(static_cast<int>(contaminated.size()), QPointF()), params);
    const double contaminatedMae = maskedMae(medianRestored, background, mask);
    check(4, "median rejects three contaminated frames",
          contaminatedMae < 2.0,
          QStringLiteral("MAE=%1").arg(contaminatedMae, 0, 'f', 3));

    QVector<QImage> shiftedNeighbors;
    shiftedNeighbors.reserve(neighbors.size());
    for (const QImage &neighbor : neighbors)
        shiftedNeighbors.append(shiftImage(neighbor, 5, 3));
    const QVector<QPointF> trackingOffsets(
        static_cast<int>(shiftedNeighbors.size()), QPointF(5, 3));
    objremoval::ObjectRemovalParams trackedParams = params;
    trackedParams.useBackgroundAlignment = true;
    const QImage trackedRestored = objremoval::removeObject(
        frames[targetIndex], mask, shiftedNeighbors, trackingOffsets,
        trackedParams);
    const double trackedMae = maskedMae(trackedRestored, background, mask);
    check(5, "background alignment offset reconstruction",
          std::abs(trackedMae - mae) <= 1.0,
          QStringLiteral("untracked=%1 tracked=%2")
              .arg(mae, 0, 'f', 3).arg(trackedMae, 0, 'f', 3));

    QImage hole = background.copy();
    const QRect holeRect(44, 28, 36, 30);
    const QImage holeMask = rectMask(holeRect);
    for (int y = holeRect.top(); y <= holeRect.bottom(); ++y) {
        for (int x = holeRect.left(); x <= holeRect.right(); ++x)
            hole.setPixelColor(x, y, Qt::black);
    }
    const QImage inpainted = objremoval::inpaintSpatial(hole, holeMask, params);
    const double spatialMae = maskedMae(inpainted, background, holeMask);
    const bool spatialChanged = spatialMae < 80.0
        && inpainted.pixelColor(holeRect.center())
            != hole.pixelColor(holeRect.center());
    const double jump = edgeJump(inpainted, background, holeMask);
    check(6, "spatial fast-marching fallback", spatialChanged && jump < 12.0,
          QStringLiteral("MAE=%1 edgeJump=%2")
              .arg(spatialMae, 0, 'f', 3).arg(jump, 0, 'f', 3));

    QVector<QImage> halfValidNeighbors = neighbors;
    for (int i = 0; i < static_cast<int>(halfValidNeighbors.size()); i += 2)
        halfValidNeighbors[i] = QImage();
    const QVector<QPointF> halfValidOffsets(
        static_cast<int>(halfValidNeighbors.size()), QPointF());
    const QImage halfCoverage = objremoval::temporalCoverageMap(
        mask, halfValidNeighbors, halfValidOffsets, params);
    const int coverageAtCenter = halfCoverage.constScanLine(targetRect.center().y())
        [targetRect.center().x()];

    const QImage noisyBackground = makeHighFrequencyBackground();
    const QVector<QImage> noisyFrames = makeSequence(noisyBackground);
    const QVector<QImage> noisyNeighbors = makeNeighbors(
        noisyFrames, targetIndex, 8);
    const QVector<QPointF> noisyOffsets(
        static_cast<int>(noisyNeighbors.size()), QPointF());
    QVector<QImage> noisyHalfValid = noisyNeighbors;
    for (int i = 0; i < static_cast<int>(noisyHalfValid.size()); i += 2)
        noisyHalfValid[i] = QImage();

    objremoval::ObjectRemovalParams strictThresholdParams = params;
    strictThresholdParams.temporalTrustThreshold = 0.65;
    const QImage strictFallback = objremoval::removeObject(
        noisyFrames[targetIndex], mask, noisyHalfValid, noisyOffsets,
        strictThresholdParams);
    const double strictMae = maskedMae(
        strictFallback, noisyBackground, mask);

    objremoval::ObjectRemovalParams relaxedThresholdParams = params;
    relaxedThresholdParams.temporalTrustThreshold = 0.4;
    const QImage relaxedTemporal = objremoval::removeObject(
        noisyFrames[targetIndex], mask, noisyHalfValid, noisyOffsets,
        relaxedThresholdParams);
    const double relaxedMae = maskedMae(
        relaxedTemporal, noisyBackground, mask);
    check(7, "temporal trust threshold selects fallback path",
          coverageAtCenter >= 120 && coverageAtCenter <= 135
              && strictMae > relaxedMae + 4.0 && relaxedMae < 2.0,
          QStringLiteral("coverage=%1 strictMAE=%2 relaxedMAE=%3")
              .arg(coverageAtCenter)
              .arg(strictMae, 0, 'f', 3)
              .arg(relaxedMae, 0, 'f', 3));

    const QVector<QImage> stableNeighbors(
        static_cast<int>(neighbors.size()), noisyBackground);
    const QImage targetMatchingMedian = objremoval::removeObject(
        noisyBackground, mask, stableNeighbors, zeroOffsets, params);
    const double targetMatchingMae = maskedMae(
        targetMatchingMedian, noisyBackground, mask);
    check(8, "target-matching temporal median remains trusted",
          targetMatchingMae < 0.01,
          QStringLiteral("MAE=%1").arg(targetMatchingMae, 0, 'f', 3));

    QImage smallNeighbor(4, 4, QImage::Format_RGB888);
    smallNeighbor.fill(Qt::green);
    QVector<QImage> mismatchedNeighbors;
    mismatchedNeighbors.append(smallNeighbor);
    const QVector<QPointF> mismatchedOffsets(1, QPointF());
    const QImage mismatchedCoverage = objremoval::temporalCoverageMap(
        mask, mismatchedNeighbors, mismatchedOffsets, params);
    bool mismatchCoverageIsZero = mismatchedCoverage.size() == mask.size();
    for (int y = 0; y < mask.height() && mismatchCoverageIsZero; ++y) {
        const uchar *maskRow = mask.constScanLine(y);
        const uchar *coverageRow = mismatchedCoverage.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (maskRow[x] > 0 && coverageRow[x] != 0) {
                mismatchCoverageIsZero = false;
                break;
            }
        }
    }
    check(9, "mismatched temporal neighbor is rejected safely",
          mismatchCoverageIsZero);

    const QImage flatBackground = makeFlatBackground();
    const QVector<QImage> flatFrames = makeSequence(flatBackground);
    const QVector<QImage> flatNeighbors = makeNeighbors(
        flatFrames, targetIndex, 8);
    objremoval::ObjectRemovalParams compositeParams = params;
    compositeParams.maskDilatePx = 2;
    compositeParams.featherPx = 3;
    const QImage composited = objremoval::removeObject(
        flatFrames[targetIndex], mask, flatNeighbors,
        QVector<QPointF>(static_cast<int>(flatNeighbors.size()), QPointF()),
        compositeParams);
    const QColor expectedFlat = flatBackground.pixelColor(targetRect.center());
    const QColor targetColor = flatFrames[targetIndex].pixelColor(
        targetRect.center());
    const auto rgbDistance = [](const QColor &a, const QColor &b) {
        return (std::abs(a.red() - b.red())
                + std::abs(a.green() - b.green())
                + std::abs(a.blue() - b.blue())) / 3.0;
    };
    const double fullBoundaryDistance = rgbDistance(targetColor, expectedFlat);
    double previousBackgroundDistance = rgbDistance(
        composited.pixelColor(targetRect.left(), targetRect.center().y()),
        expectedFlat);
    bool monotonicBoundary = true;
    for (int x = targetRect.left() + 1; x <= targetRect.center().x(); ++x) {
        const double currentDistance = rgbDistance(
            composited.pixelColor(x, targetRect.center().y()), expectedFlat);
        if (currentDistance > previousBackgroundDistance + 1.0) {
            monotonicBoundary = false;
            break;
        }
        previousBackgroundDistance = currentDistance;
    }
    const double edgeDistance = rgbDistance(
        composited.pixelColor(targetRect.left(), targetRect.center().y()),
        expectedFlat);
    const bool centerReplaced = composited.pixelColor(targetRect.center())
        == expectedFlat;
    const bool boundaryHasFeather = edgeDistance > 0.0
        && edgeDistance < fullBoundaryDistance;
    check(10, "dilation and feather composite path",
          outsideBitIdentical(flatFrames[targetIndex], composited, mask)
              && centerReplaced && boundaryHasFeather && monotonicBoundary,
          QStringLiteral("center=%1 edge=%2 full=%3 monotonic=%4")
              .arg(centerReplaced).arg(edgeDistance, 0, 'f', 3)
              .arg(fullBoundaryDistance, 0, 'f', 3)
              .arg(monotonicBoundary));

    QImage emptyMask(mask.size(), QImage::Format_Grayscale8);
    emptyMask.fill(0);
    check(11, "maskedMae fails closed for an empty mask",
          maskedMae(restored, background, emptyMask) == 255.0);

    QImage performanceTarget(160, 120, QImage::Format_RGB888);
    performanceTarget.fill(QColor(72, 82, 92));
    QImage performanceMask(performanceTarget.size(), QImage::Format_Grayscale8);
    performanceMask.fill(0);
    const QRect performanceRect(70, 45, 20, 20);
    for (int y = performanceRect.top(); y <= performanceRect.bottom(); ++y) {
        uchar *row = performanceMask.scanLine(y);
        for (int x = performanceRect.left(); x <= performanceRect.right(); ++x)
            row[x] = 255;
    }
    objremoval::ObjectRemovalParams performanceParams = params;
    performanceParams.temporalRadius = 0;
    performanceParams.maskDilatePx = 64;
    performanceParams.featherPx = 0;
    performanceParams.spatialRadius = 1;
    QElapsedTimer dilationTimer;
    dilationTimer.start();
    const QImage performanceResult = objremoval::removeObject(
        performanceTarget, performanceMask, QVector<QImage>(),
        QVector<QPointF>(), performanceParams);
    const qint64 dilationElapsedMs = dilationTimer.elapsed();
    check(12, "large mask dilation stays linear-time",
          !performanceResult.isNull() && dilationElapsedMs < 1500,
          QStringLiteral("elapsedMs=%1").arg(dilationElapsedMs));

    QHash<int, QImage> cache;
    for (int i = 0; i < 100; ++i)
        cache.insert(i, QImage(1, 1, QImage::Format_RGB32));
    objremoval::trimObjectRemovalFrameCache(cache, 50, 100, 2);
    bool cacheIsWindowed = cache.size() == 5;
    for (int i = 48; i <= 52; ++i)
        cacheIsWindowed = cacheIsWindowed && cache.contains(i);
    check(13, "frame cache is bounded to temporal window", cacheIsWindowed,
          QStringLiteral("entries=%1").arg(cache.size()));

    const QImage repeatedA = objremoval::removeObject(
        frames[targetIndex], mask, neighbors, zeroOffsets, params);
    const QImage repeatedB = objremoval::removeObject(
        frames[targetIndex], mask, neighbors, zeroOffsets, params);
    const bool deterministic = repeatedA.format() == repeatedB.format()
        && repeatedA.size() == repeatedB.size()
        && std::memcmp(repeatedA.constBits(), repeatedB.constBits(),
                       static_cast<std::size_t>(repeatedA.sizeInBytes())) == 0;
    check(14, "deterministic output", deterministic);

    qInfo().noquote() << QStringLiteral("[object-removal] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
