#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>

// --- Mask shape type ---

enum class MaskShape {
    Rectangle,
    Ellipse,
    Polygon,
    Path
};

// --- Mask combination mode ---

enum class MaskMode {
    Add,        // max(base, mask)
    Subtract,   // base - mask
    Intersect,  // min(base, mask)
    Difference  // abs(base - mask)
};

// --- Feather direction ---

enum class FeatherDirection {
    Both,
    InOnly,
    OutOnly
};

// --- Feather settings ---

struct MaskFeather {
    double amount = 0.0;   // 0-100 pixels
    FeatherDirection direction = FeatherDirection::Both;
};

// --- Single mask definition ---

struct Mask {
    MaskShape shape = MaskShape::Rectangle;
    MaskMode mode = MaskMode::Add;
    MaskFeather feather;

    QVector<QPointF> points;  // control points for Polygon / Path
    QRectF rect;              // bounding rect for Rectangle / Ellipse

    bool inverted = false;
    double opacity = 1.0;     // 0.0 - 1.0
    double expansion = 0.0;   // -100 to 100
    QString name;

    QJsonObject toJson() const;
    static Mask fromJson(const QJsonObject &obj);
};

// --- Track matte type ---

enum class TrackMatteType {
    None,
    AlphaMatte,
    AlphaInvertedMatte,
    LumaMatte,
    LumaInvertedMatte
};

// --- Track matte configuration ---

struct TrackMatte {
    TrackMatteType type = TrackMatteType::None;
    int sourceLayerIndex = -1;

    QJsonObject toJson() const;
    static TrackMatte fromJson(const QJsonObject &obj);
};

// --- Mask System ---

class MaskSystem
{
public:
    MaskSystem() = default;
    bool isDefault() const { return m_masks.isEmpty(); }

    // --- Mask management ---

    void addMask(const Mask &mask);
    void removeMask(int index);
    const QVector<Mask> &masks() const { return m_masks; }

    // --- Mask rendering ---

    // Render all masks into a single grayscale alpha image
    // White (255) = fully visible, Black (0) = fully masked
    static QImage generateMaskImage(const QVector<Mask> &masks, const QSize &canvasSize);

    // Multiply source image alpha by mask (grayscale)
    static QImage applyMask(const QImage &sourceImage, const QImage &maskImage);

    // Use another layer as a track matte
    static QImage applyTrackMatte(const QImage &sourceImage,
                                  const QImage &matteImage,
                                  TrackMatteType matteType);

    // --- Individual shape renderers ---

    static QImage renderRectMask(const QRectF &rect, const QSize &canvasSize,
                                 const MaskFeather &feather);
    static QImage renderEllipseMask(const QRectF &rect, const QSize &canvasSize,
                                    const MaskFeather &feather);
    static QImage renderPolygonMask(const QVector<QPointF> &points, const QSize &canvasSize,
                                    const MaskFeather &feather);

    // --- Post-processing ---

    // Apply Gaussian-like feathering (iterative box blur) to a mask image
    static QImage featherMask(const QImage &mask, double amount);

    // --- Serialisation ---

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    // Combine a new mask layer onto the base using the given mode
    static void combineMasks(QImage &base, const QImage &layer, MaskMode mode);

    // Iterative box blur (fast Gaussian approximation), single channel
    static void boxBlur(QImage &img, int radius);

    QVector<Mask> m_masks;
};
