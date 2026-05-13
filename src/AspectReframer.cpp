#include "AspectReframer.h"

#include <algorithm>
#include <cmath>
#include <QColor>
#include <QPainter>

namespace reframe {

// ---------------------------------------------------------------------------
// modeDisplayName / modeFromString / availableModes
// ---------------------------------------------------------------------------
QString modeDisplayName(Mode m)
{
    switch (m) {
    case Mode::CenterCrop:        return QStringLiteral("中央クロップ");
    case Mode::LetterBox:         return QStringLiteral("レターボックス");
    case Mode::SmartCenterFollow: return QStringLiteral("スマート中央追跡");
    case Mode::SkinToneFocus:     return QStringLiteral("肌色フォーカス");
    case Mode::Manual:            return QStringLiteral("手動");
    }
    return QStringLiteral("中央クロップ");
}

Mode modeFromString(const QString& s)
{
    if (s == QStringLiteral("中央クロップ")       || s == QStringLiteral("CenterCrop"))        return Mode::CenterCrop;
    if (s == QStringLiteral("レターボックス")      || s == QStringLiteral("LetterBox"))         return Mode::LetterBox;
    if (s == QStringLiteral("スマート中央追跡")    || s == QStringLiteral("SmartCenterFollow")) return Mode::SmartCenterFollow;
    if (s == QStringLiteral("肌色フォーカス")      || s == QStringLiteral("SkinToneFocus"))     return Mode::SkinToneFocus;
    if (s == QStringLiteral("手動")               || s == QStringLiteral("Manual"))             return Mode::Manual;
    return Mode::CenterCrop;
}

QStringList availableModes()
{
    return {
        modeDisplayName(Mode::CenterCrop),
        modeDisplayName(Mode::LetterBox),
        modeDisplayName(Mode::SmartCenterFollow),
        modeDisplayName(Mode::SkinToneFocus),
        modeDisplayName(Mode::Manual)
    };
}

// ---------------------------------------------------------------------------
// Helper: compute center-crop rect (normalized) given source/target aspect
// Returns QRectF(x, y, w, h) all in [0..1]
// ---------------------------------------------------------------------------
static QRectF centerCropRect(double srcAspect, double tgtAspect)
{
    if (srcAspect > tgtAspect) {
        // Source is wider — crop left/right
        const double cropW = tgtAspect / srcAspect;
        const double cropX = (1.0 - cropW) / 2.0;
        return QRectF(cropX, 0.0, cropW, 1.0);
    } else {
        // Source is taller — crop top/bottom
        const double cropH = srcAspect / tgtAspect;
        const double cropY = (1.0 - cropH) / 2.0;
        return QRectF(0.0, cropY, 1.0, cropH);
    }
}

// ---------------------------------------------------------------------------
// Helper: clamp a crop rect so it stays within [0,0,1,1]
// ---------------------------------------------------------------------------
static QRectF clampRect(QRectF r)
{
    double x = std::max(0.0, std::min(1.0 - r.width(),  r.x()));
    double y = std::max(0.0, std::min(1.0 - r.height(), r.y()));
    return QRectF(x, y, r.width(), r.height());
}

// ---------------------------------------------------------------------------
// SmartCenterFollow: Sobel edge density on 8x8 grid, pick densest cell center
// ---------------------------------------------------------------------------
static QRectF smartCropRect(const QImage& src, double srcAspect, double tgtAspect)
{
    const QRectF base = centerCropRect(srcAspect, tgtAspect);

    const QImage gray = src.convertToFormat(QImage::Format_Grayscale8);
    const int    w    = gray.width();
    const int    h    = gray.height();
    if (w < 3 || h < 3)
        return base;

    constexpr int kGrid = 8;
    double density[kGrid][kGrid] = {};

    // Sobel gradient magnitude, accumulated per grid cell
    for (int y = 1; y < h - 1; ++y) {
        const uchar* row0 = gray.constScanLine(y - 1);
        const uchar* row1 = gray.constScanLine(y);
        const uchar* row2 = gray.constScanLine(y + 1);
        const int    gy   = y * kGrid / h;

        for (int x = 1; x < w - 1; ++x) {
            const int gx = x * kGrid / w;
            const double gX = -row0[x-1] + row0[x+1]
                              -2.0*row1[x-1] + 2.0*row1[x+1]
                              -row2[x-1] + row2[x+1];
            const double gY = -row0[x-1] - 2.0*row0[x] - row0[x+1]
                              + row2[x-1] + 2.0*row2[x] + row2[x+1];
            density[gy][gx] += std::sqrt(gX*gX + gY*gY);
        }
    }

    // Find densest cell
    int bestRow = 0, bestCol = 0;
    double best = -1.0;
    for (int r = 0; r < kGrid; ++r) {
        for (int c = 0; c < kGrid; ++c) {
            if (density[r][c] > best) {
                best    = density[r][c];
                bestRow = r;
                bestCol = c;
            }
        }
    }

    // Center of that cell in normalized coords
    const double cx = (bestCol + 0.5) / kGrid;
    const double cy = (bestRow + 0.5) / kGrid;

    QRectF r(cx - base.width()  / 2.0,
             cy - base.height() / 2.0,
             base.width(),
             base.height());
    return clampRect(r);
}

// ---------------------------------------------------------------------------
// SkinToneFocus: HSV hue centroid of skin-tone pixels
// ---------------------------------------------------------------------------
static QRectF skinToneCropRect(const QImage& src, double srcAspect, double tgtAspect)
{
    const QRectF base = centerCropRect(srcAspect, tgtAspect);

    const QImage argb = src.convertToFormat(QImage::Format_ARGB32);
    const int    w    = argb.width();
    const int    h    = argb.height();

    double sumX = 0.0, sumY = 0.0;
    int    count = 0;

    // Skin tone: hue in [0..0.083] or [0.917..1.0], S>0.2, V>0.3
    for (int y = 0; y < h; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            float hueF, satF, valF;
            QColor::fromRgb(qRed(line[x]), qGreen(line[x]), qBlue(line[x]))
                .getHsvF(&hueF, &satF, &valF);
            const double hue = static_cast<double>(hueF);
            const double sat = static_cast<double>(satF);
            const double val = static_cast<double>(valF);
            if (hue < 0.0) continue; // achromatic
            const bool isSkin = (hue <= 0.083 || hue >= 0.917) && sat > 0.2 && val > 0.3;
            if (isSkin) {
                sumX += static_cast<double>(x) / w;
                sumY += static_cast<double>(y) / h;
                ++count;
            }
        }
    }

    if (count < 100)
        return base; // fallback to center crop

    const double cx = sumX / count;
    const double cy = sumY / count;

    QRectF r(cx - base.width()  / 2.0,
             cy - base.height() / 2.0,
             base.width(),
             base.height());
    return clampRect(r);
}

// ---------------------------------------------------------------------------
// computeCropRect
// ---------------------------------------------------------------------------
QRectF computeCropRect(const QImage& source, const ReframeParams& params)
{
    if (source.isNull())
        return QRectF(0, 0, 1, 1);

    const double srcAspect = static_cast<double>(source.width())  / source.height();
    const double tgtAspect = static_cast<double>(params.targetSize.width()) / params.targetSize.height();

    switch (params.mode) {
    case Mode::LetterBox:
        // No crop — entire source shown with letterbox bars
        return QRectF(0.0, 0.0, 1.0, 1.0);

    case Mode::CenterCrop:
        return centerCropRect(srcAspect, tgtAspect);

    case Mode::Manual: {
        // Start from base center-crop size, move center to manualOffset
        const QRectF base = centerCropRect(srcAspect, tgtAspect);
        // Apply zoom: crop dims shrink as zoom increases (zoom=1 → base size)
        const double zoomedW = base.width()  / std::max(params.zoom, 0.1);
        const double zoomedH = base.height() / std::max(params.zoom, 0.1);
        const double cx = params.manualOffsetNormalized.x();
        const double cy = params.manualOffsetNormalized.y();
        QRectF r(cx - zoomedW / 2.0, cy - zoomedH / 2.0, zoomedW, zoomedH);
        return clampRect(r);
    }

    case Mode::SmartCenterFollow:
        return smartCropRect(source, srcAspect, tgtAspect);

    case Mode::SkinToneFocus:
        return skinToneCropRect(source, srcAspect, tgtAspect);
    }

    return centerCropRect(srcAspect, tgtAspect);
}

// ---------------------------------------------------------------------------
// applyReframe
// ---------------------------------------------------------------------------
ReframeResult applyReframe(const QImage& source, const ReframeParams& params)
{
    const QSize tgtSize = params.targetSize;

    // Null source guard — return black target-sized image
    if (source.isNull()) {
        QImage black(tgtSize, QImage::Format_ARGB32);
        black.fill(Qt::black);
        return ReframeResult{ QRectF(), black, false, QStringLiteral("source is null") };
    }

    const QRectF cropRect = computeCropRect(source, params);

    QImage out(tgtSize, QImage::Format_ARGB32);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (params.mode == Mode::LetterBox) {
        // Fill background black, then draw source aspect-preserved centered
        out.fill(Qt::black);

        const double srcAspect = static_cast<double>(source.width())  / source.height();
        const double tgtAspect = static_cast<double>(tgtSize.width()) / tgtSize.height();

        QRectF dstRect;
        if (srcAspect > tgtAspect) {
            // Pillarbox: fit width
            const double drawH = tgtSize.width() / srcAspect;
            dstRect = QRectF(0, (tgtSize.height() - drawH) / 2.0, tgtSize.width(), drawH);
        } else {
            // Letterbox: fit height
            const double drawW = tgtSize.height() * srcAspect;
            dstRect = QRectF((tgtSize.width() - drawW) / 2.0, 0, drawW, tgtSize.height());
        }

        painter.drawImage(dstRect, source, QRectF(source.rect()));
    } else {
        // Crop from source and scale to target
        const QRectF srcPixelRect(
            cropRect.x()      * source.width(),
            cropRect.y()      * source.height(),
            cropRect.width()  * source.width(),
            cropRect.height() * source.height()
        );
        painter.drawImage(QRectF(QPointF(0, 0), tgtSize), source, srcPixelRect);
    }

    painter.end();

    return ReframeResult{ cropRect, out, true, QString() };
}

} // namespace reframe
