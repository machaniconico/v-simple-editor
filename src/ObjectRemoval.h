#pragma once

#include <QHash>
#include <QImage>
#include <QPointF>
#include <QSize>
#include <QVector>

#include <functional>

namespace objremoval {

struct ObjectRemovalParams {
    int temporalRadius = 12;
    int temporalStride = 1;
    // Background alignment is opt-in. Object/mask tracking displacement is
    // never a valid substitute for this value.
    bool useBackgroundAlignment = false;
    int featherPx = 3;
    int maskDilatePx = 2;
    double temporalTrustThreshold = 0.65;
    int spatialRadius = 24;
};

struct ObjectRemovalTemporalSources {
    std::function<QImage(int)> frameFetcher;
    // This displacement follows the object mask shape only. It is deliberately
    // kept separate from background alignment and is never used for neighbor
    // pixel sampling.
    std::function<QPointF(int)> maskTrackingOffsetFetcher;
    // Returns the background displacement relative to a common reference.
    // The collected neighbor offset is neighborValue - targetValue.
    std::function<QPointF(int)> backgroundAlignmentOffsetFetcher;
};

struct ObjectRemovalNeighborSet {
    QVector<QImage> neighbors;
    // For each neighbor, the translation that aligns the background to the
    // target sampling position. This is not the displacement of the object
    // being removed.
    QVector<QPointF> backgroundOffsets;
};

// GUI-independent part of ObjectRemovalDialog::processFrame. Mask tracking
// may be present in sources, but only an explicitly enabled background
// alignment source can contribute pixel-sampling offsets.
ObjectRemovalNeighborSet collectTemporalNeighbors(
    int frameIndex,
    int frameCount,
    int temporalRadius,
    const ObjectRemovalTemporalSources &sources,
    bool useBackgroundAlignment);

// Keep only the temporal window needed by the current frame. This makes the
// dialog cache bounded while preserving every frame needed by one operation.
void trimObjectRemovalFrameCache(QHash<int, QImage> &cache,
                                 int centerFrame,
                                 int frameCount,
                                 int temporalRadius);

// neighborOffsets[i] is a background-alignment sampling translation: the
// engine samples neighbor i at target point p + neighborOffsets[i]. It must
// describe background/camera displacement, never object or mask displacement.
QImage removeObject(const QImage &target,
                    const QImage &targetMask,
                    const QVector<QImage> &neighbors,
                    const QVector<QPointF> &neighborOffsets,
                    const ObjectRemovalParams &params);

QImage inpaintSpatial(const QImage &image, const QImage &mask,
                      const ObjectRemovalParams &params);

// The same background-alignment offset semantics as removeObject apply here.
QImage temporalCoverageMap(const QImage &targetMask,
                           const QVector<QImage> &neighbors,
                           const QVector<QPointF> &neighborOffsets,
                           const ObjectRemovalParams &params);

} // namespace objremoval
