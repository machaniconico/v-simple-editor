#pragma once

#include <QString>
#include <QColor>
#include <QPointF>
#include <QImage>
#include <QSize>
#include <QVariant>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>

#include "MaskSystem.h"

// --- Blend Modes ---

enum class BlendMode {
    Normal,
    Add,
    Multiply,
    Screen,
    Overlay,
    SoftLight,
    HardLight,
    Difference,
    Exclusion,
    ColorDodge,
    ColorBurn,
    Darken,
    Lighten
};

// --- Layer Source Type ---

enum class LayerSourceType {
    Video,
    Image,
    Solid,
    Shape,
    Text,
    Adjustment
};

// --- Composite Layer ---

struct CompositeLayer {
    QString name;
    bool visible  = true;
    bool locked   = false;

    double opacity    = 1.0;       // 0.0 - 1.0
    BlendMode blendMode = BlendMode::Normal;

    QPointF position    = QPointF(0.0, 0.0);
    QPointF scale       = QPointF(1.0, 1.0);
    double  rotation    = 0.0;     // degrees
    QPointF anchorPoint = QPointF(0.0, 0.0);
    int     zOrder      = 0;

    LayerSourceType sourceType = LayerSourceType::Video;
    QString sourcePath;
    QColor  solidColor = Qt::white;

    double inPoint  = 0.0;         // seconds — time range start
    double outPoint = 0.0;         // seconds — time range end (0 = until end)

    // Track matte wiring (AE/Premiere semantics)
    TrackMatteType matteType            = TrackMatteType::None;
    int            matteSourceLayerIndex = -1;  // index into layers vector that provides the matte

    QJsonObject toJson() const;
    static CompositeLayer fromJson(const QJsonObject &obj);

    static QString blendModeName(BlendMode mode);
    static BlendMode blendModeFromName(const QString &name);

    static QString sourceTypeName(LayerSourceType type);
    static LayerSourceType sourceTypeFromName(const QString &name);
};

// --- Layer Compositor ---

class LayerCompositor
{
public:
    LayerCompositor() = default;

    // Layer management
    void addLayer(const CompositeLayer &layer);
    bool removeLayer(int index);
    bool moveLayer(int fromIndex, int toIndex);

    // Access
    QVector<CompositeLayer> layers() const;          // sorted by zOrder
    int layerCount() const { return m_layers.size(); }

    // Generic property setter
    bool setLayerProperty(int index, const QString &property, const QVariant &value);

    // Compositing
    static QImage compositeFrame(const QVector<CompositeLayer> &layers,
                                 const QSize &canvasSize, double time);

    // Pixel-level blending (ARGB)
    static QRgb blendPixel(QRgb base, QRgb top, BlendMode mode, double opacity);

    // Image-level blending
    static QImage blendImages(const QImage &base, const QImage &top,
                              BlendMode mode, double opacity);

    // Serialisation
    QJsonObject toJson() const;
    static LayerCompositor fromJson(const QJsonObject &obj);

private:
    QVector<CompositeLayer> m_layers;

    // Blend math helpers (operate on normalised 0-1 floats)
    static double blendChannel(double base, double top, BlendMode mode);

    // Transform a layer image according to position/scale/rotation/anchor
    static QImage transformLayer(const QImage &source, const CompositeLayer &layer,
                                 const QSize &canvasSize);
};
