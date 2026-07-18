#include "ClipMask.h"

#include "../MaskSystem.h"
#include "../PlanarTracker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QImage>
#include <QList>
#include <QMap>
#include <QRgba64>
#include <QtGlobal>

#include <cmath>

namespace {

constexpr double kEpsilon = 1e-9;
constexpr double kEllipseKappa = 0.5522847498307936;
constexpr int kBezierSegments = 16;
constexpr const char *kBezierMetadataMarker = "\n#VEDITOR_MASK_BEZIER:";

bool nearlyEqual(double a, double b)
{
    return std::abs(a - b) <= kEpsilon;
}

bool pointNearlyEqual(const QPointF &a, const QPointF &b)
{
    return nearlyEqual(a.x(), b.x()) && nearlyEqual(a.y(), b.y());
}

bool hasTangent(const clipmask::BezierPoint &point)
{
    return !pointNearlyEqual(point.inTangent, QPointF())
        || !pointNearlyEqual(point.outTangent, QPointF());
}

QPointF cubicAt(const QPointF &p0, const QPointF &p1,
                const QPointF &p2, const QPointF &p3,
                double t)
{
    const double u = 1.0 - t;
    return p0 * (u * u * u)
        + p1 * (3.0 * u * u * t)
        + p2 * (3.0 * u * t * t)
        + p3 * (t * t * t);
}

clipmask::CombineMode combineModeFromMaskMode(MaskMode mode)
{
    switch (mode) {
    case MaskMode::Add:
        return clipmask::CombineMode::Add;
    case MaskMode::Subtract:
        return clipmask::CombineMode::Subtract;
    case MaskMode::Intersect:
        return clipmask::CombineMode::Intersect;
    case MaskMode::Difference:
        return clipmask::CombineMode::Difference;
    }
    return clipmask::CombineMode::Add;
}

MaskMode maskModeFromCombineMode(clipmask::CombineMode mode)
{
    switch (mode) {
    case clipmask::CombineMode::Add:
        return MaskMode::Add;
    case clipmask::CombineMode::Subtract:
        return MaskMode::Subtract;
    case clipmask::CombineMode::Intersect:
        return MaskMode::Intersect;
    case clipmask::CombineMode::Difference:
        return MaskMode::Difference;
    }
    return MaskMode::Add;
}

clipmask::BezierPoint makePoint(const QPointF &vertex,
                                const QPointF &inTangent = QPointF(),
                                const QPointF &outTangent = QPointF())
{
    clipmask::BezierPoint point;
    point.vertex = vertex;
    point.inTangent = inTangent;
    point.outTangent = outTangent;
    return point;
}

bool decodeBezierMetadata(const QString &name, QVector<clipmask::BezierPoint> *points)
{
    if (!points)
        return false;
    const QString marker = QString::fromLatin1(kBezierMetadataMarker);
    const int markerAt = name.indexOf(marker);
    if (markerAt < 0)
        return false;

    const QByteArray payload = name.mid(markerAt + marker.size()).trimmed().toLatin1();
    const QByteArray json = QByteArray::fromBase64(payload);
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray())
        return false;

    QVector<clipmask::BezierPoint> decoded;
    const QJsonArray arr = doc.array();
    decoded.reserve(arr.size());
    for (const QJsonValue &value : arr) {
        const QJsonObject obj = value.toObject();
        decoded.append(makePoint(
            QPointF(obj.value(QStringLiteral("x")).toDouble(),
                    obj.value(QStringLiteral("y")).toDouble()),
            QPointF(obj.value(QStringLiteral("inX")).toDouble(0.0),
                    obj.value(QStringLiteral("inY")).toDouble(0.0)),
            QPointF(obj.value(QStringLiteral("outX")).toDouble(0.0),
                    obj.value(QStringLiteral("outY")).toDouble(0.0))));
    }
    if (decoded.size() < 3)
        return false;

    *points = decoded;
    return true;
}

QVector<clipmask::BezierPoint> rectToBezier(const QRectF &rect)
{
    QVector<clipmask::BezierPoint> points;
    if (rect.isNull() || !rect.isValid())
        return points;

    points.reserve(4);
    points.append(makePoint(rect.topLeft()));
    points.append(makePoint(rect.topRight()));
    points.append(makePoint(rect.bottomRight()));
    points.append(makePoint(rect.bottomLeft()));
    return points;
}

QVector<clipmask::BezierPoint> ellipseToBezier(const QRectF &rect)
{
    QVector<clipmask::BezierPoint> points;
    if (rect.isNull() || !rect.isValid())
        return points;

    const QPointF center = rect.center();
    const double rx = rect.width() * 0.5;
    const double ry = rect.height() * 0.5;
    const double hx = rx * kEllipseKappa;
    const double hy = ry * kEllipseKappa;

    points.reserve(4);
    points.append(makePoint(QPointF(center.x() + rx, center.y()),
                            QPointF(0.0, -hy), QPointF(0.0, hy)));
    points.append(makePoint(QPointF(center.x(), center.y() + ry),
                            QPointF(hx, 0.0), QPointF(-hx, 0.0)));
    points.append(makePoint(QPointF(center.x() - rx, center.y()),
                            QPointF(0.0, hy), QPointF(0.0, -hy)));
    points.append(makePoint(QPointF(center.x(), center.y() - ry),
                            QPointF(-hx, 0.0), QPointF(hx, 0.0)));
    return points;
}

QVector<QPointF> flattenBezier(const clipmask::BezierMask &mask)
{
    QVector<QPointF> flattened;
    if (mask.points.isEmpty())
        return flattened;

    flattened.reserve(mask.points.size() * 4);
    flattened.append(mask.points.first().vertex);

    const int segmentCount = mask.closed ? mask.points.size() : mask.points.size() - 1;
    for (int i = 0; i < segmentCount; ++i) {
        const int next = (i + 1) % mask.points.size();
        const clipmask::BezierPoint &a = mask.points[i];
        const clipmask::BezierPoint &b = mask.points[next];
        const bool curved = hasTangent(a) || hasTangent(b);
        if (!curved) {
            if (!mask.closed || next != 0)
                flattened.append(b.vertex);
            continue;
        }

        const QPointF p0 = a.vertex;
        const QPointF p1 = a.vertex + a.outTangent;
        const QPointF p2 = b.vertex + b.inTangent;
        const QPointF p3 = b.vertex;
        for (int step = 1; step <= kBezierSegments; ++step) {
            if (mask.closed && next == 0 && step == kBezierSegments)
                continue;
            flattened.append(cubicAt(p0, p1, p2, p3,
                                     static_cast<double>(step) / kBezierSegments));
        }
    }

    return flattened;
}

QPointF transformPoint(const planartrack::Homography &homography,
                       const QPointF &point)
{
    const double x = point.x();
    const double y = point.y();
    const double w = homography[6] * x + homography[7] * y + homography[8];
    if (std::abs(w) < kEpsilon)
        return point;

    return QPointF((homography[0] * x + homography[1] * y + homography[2]) / w,
                   (homography[3] * x + homography[4] * y + homography[5]) / w);
}

clipmask::BezierPoint transformBezierPoint(
    const clipmask::BezierPoint &source,
    const planar::Homography &homography)
{
    clipmask::BezierPoint point;
    point.vertex = planar::transformPoint(source.vertex, homography);
    if (!pointNearlyEqual(source.inTangent, QPointF()))
        point.inTangent = planar::transformPoint(source.vertex + source.inTangent,
                                                 homography) - point.vertex;
    if (!pointNearlyEqual(source.outTangent, QPointF()))
        point.outTangent = planar::transformPoint(source.vertex + source.outTangent,
                                                  homography) - point.vertex;
    return point;
}

clipmask::BezierPoint transformBezierPoint(
    const clipmask::BezierPoint &source,
    const planartrack::Homography &homography)
{
    clipmask::BezierPoint point;
    point.vertex = transformPoint(homography, source.vertex);
    if (!pointNearlyEqual(source.inTangent, QPointF()))
        point.inTangent = transformPoint(homography,
                                         source.vertex + source.inTangent) - point.vertex;
    if (!pointNearlyEqual(source.outTangent, QPointF()))
        point.outTangent = transformPoint(homography,
                                          source.vertex + source.outTangent) - point.vertex;
    return point;
}

clipmask::BezierMask transformMask(const clipmask::BezierMask &source,
                                   const planar::Homography &homography)
{
    clipmask::BezierMask mask = source;
    mask.points.clear();
    mask.points.reserve(source.points.size());
    for (const clipmask::BezierPoint &point : source.points)
        mask.points.append(transformBezierPoint(point, homography));
    return mask;
}

clipmask::BezierMask transformMask(const clipmask::BezierMask &source,
                                   const planartrack::Homography &homography)
{
    clipmask::BezierMask mask = source;
    mask.points.clear();
    mask.points.reserve(source.points.size());
    for (const clipmask::BezierPoint &point : source.points)
        mask.points.append(transformBezierPoint(point, homography));
    return mask;
}

clipmask::ClipMaskStack transformStack(const clipmask::ClipMaskStack &source,
                                       const planar::Homography &homography)
{
    clipmask::ClipMaskStack stack;
    stack.masks.reserve(source.masks.size());
    for (const clipmask::BezierMask &mask : source.masks)
        stack.masks.append(transformMask(mask, homography));
    return stack;
}

clipmask::ClipMaskStack transformStack(const clipmask::ClipMaskStack &source,
                                       const planartrack::Homography &homography)
{
    clipmask::ClipMaskStack stack;
    stack.masks.reserve(source.masks.size());
    for (const clipmask::BezierMask &mask : source.masks)
        stack.masks.append(transformMask(mask, homography));
    return stack;
}

bool containsKey(const QMap<int, clipmask::ClipMaskStack> &keyframes, int frameIndex)
{
    return keyframes.constFind(frameIndex) != keyframes.constEnd();
}

} // namespace

namespace clipmask {

bool BezierPoint::operator==(const BezierPoint &other) const
{
    return pointNearlyEqual(vertex, other.vertex)
        && pointNearlyEqual(inTangent, other.inTangent)
        && pointNearlyEqual(outTangent, other.outTangent);
}

bool BezierMask::operator==(const BezierMask &other) const
{
    return name == other.name
        && points == other.points
        && mode == other.mode
        && closed == other.closed
        && nearlyEqual(feather, other.feather)
        && nearlyEqual(expansion, other.expansion)
        && inverted == other.inverted
        && nearlyEqual(opacity, other.opacity);
}

bool ClipMaskStack::operator==(const ClipMaskStack &other) const
{
    return masks == other.masks;
}

QString combineModeToken(CombineMode mode)
{
    switch (mode) {
    case CombineMode::Add:
        return QStringLiteral("add");
    case CombineMode::Subtract:
        return QStringLiteral("subtract");
    case CombineMode::Intersect:
        return QStringLiteral("intersect");
    case CombineMode::Difference:
        return QStringLiteral("difference");
    }
    return QStringLiteral("add");
}

CombineMode combineModeFromToken(const QString &token, CombineMode fallback)
{
    const QString s = token.trimmed().toLower();
    if (s == QLatin1String("add"))
        return CombineMode::Add;
    if (s == QLatin1String("subtract") || s == QLatin1String("sub"))
        return CombineMode::Subtract;
    if (s == QLatin1String("intersect") || s == QLatin1String("intersection"))
        return CombineMode::Intersect;
    if (s == QLatin1String("difference") || s == QLatin1String("xor"))
        return CombineMode::Difference;
    return fallback;
}

QJsonObject pointToJson(const BezierPoint &point)
{
    QJsonObject obj;
    obj[QStringLiteral("x")] = point.vertex.x();
    obj[QStringLiteral("y")] = point.vertex.y();
    if (!nearlyEqual(point.inTangent.x(), 0.0))
        obj[QStringLiteral("inX")] = point.inTangent.x();
    if (!nearlyEqual(point.inTangent.y(), 0.0))
        obj[QStringLiteral("inY")] = point.inTangent.y();
    if (!nearlyEqual(point.outTangent.x(), 0.0))
        obj[QStringLiteral("outX")] = point.outTangent.x();
    if (!nearlyEqual(point.outTangent.y(), 0.0))
        obj[QStringLiteral("outY")] = point.outTangent.y();
    return obj;
}

BezierPoint pointFromJson(const QJsonObject &obj)
{
    BezierPoint point;
    point.vertex = QPointF(obj.value(QStringLiteral("x")).toDouble(),
                           obj.value(QStringLiteral("y")).toDouble());
    point.inTangent = QPointF(obj.value(QStringLiteral("inX")).toDouble(0.0),
                              obj.value(QStringLiteral("inY")).toDouble(0.0));
    point.outTangent = QPointF(obj.value(QStringLiteral("outX")).toDouble(0.0),
                               obj.value(QStringLiteral("outY")).toDouble(0.0));
    return point;
}

QJsonObject maskToJson(const BezierMask &mask)
{
    QJsonObject obj;
    obj[QStringLiteral("mode")] = combineModeToken(mask.mode);
    obj[QStringLiteral("closed")] = mask.closed;
    obj[QStringLiteral("feather")] = mask.feather;
    obj[QStringLiteral("expansion")] = mask.expansion;
    obj[QStringLiteral("invert")] = mask.inverted;
    obj[QStringLiteral("opacity")] = mask.opacity;
    if (!mask.name.isEmpty())
        obj[QStringLiteral("name")] = mask.name;

    QJsonArray points;
    for (const BezierPoint &point : mask.points)
        points.append(pointToJson(point));
    obj[QStringLiteral("points")] = points;
    return obj;
}

BezierMask maskFromJson(const QJsonObject &obj)
{
    BezierMask mask;
    mask.name = obj.value(QStringLiteral("name")).toString();
    mask.mode = combineModeFromToken(obj.value(QStringLiteral("mode")).toString(),
                                     CombineMode::Add);
    mask.closed = obj.value(QStringLiteral("closed")).toBool(true);
    mask.expansion = obj.value(QStringLiteral("expansion")).toDouble(0.0);
    mask.inverted = obj.contains(QStringLiteral("invert"))
        ? obj.value(QStringLiteral("invert")).toBool(false)
        : obj.value(QStringLiteral("inverted")).toBool(false);
    mask.opacity = qBound(0.0, obj.value(QStringLiteral("opacity")).toDouble(1.0), 1.0);

    const QJsonValue featherValue = obj.value(QStringLiteral("feather"));
    mask.feather = featherValue.isObject()
        ? featherValue.toObject().value(QStringLiteral("amount")).toDouble(0.0)
        : featherValue.toDouble(0.0);

    const QJsonArray points = obj.value(QStringLiteral("points")).toArray();
    mask.points.reserve(points.size());
    for (const QJsonValue &pointValue : points)
        mask.points.append(pointFromJson(pointValue.toObject()));

    return mask;
}

QJsonObject toJson(const ClipMaskStack &stack)
{
    QJsonObject obj;
    if (stack.masks.isEmpty())
        return obj;

    QJsonArray masks;
    for (const BezierMask &mask : stack.masks)
        masks.append(maskToJson(mask));
    obj[QStringLiteral("masks")] = masks;
    return obj;
}

ClipMaskStack fromJson(const QJsonObject &obj)
{
    ClipMaskStack stack;
    const QJsonArray masks = obj.value(QStringLiteral("masks")).toArray();
    stack.masks.reserve(masks.size());
    for (const QJsonValue &maskValue : masks)
        stack.masks.append(maskFromJson(maskValue.toObject()));
    return stack;
}

ClipMaskStack fromMaskSystem(const MaskSystem &system)
{
    ClipMaskStack stack;
    const QVector<Mask> &masks = system.masks();
    stack.masks.reserve(masks.size());

    for (const Mask &source : masks) {
        BezierMask mask;
        mask.name = source.name;
        mask.mode = combineModeFromMaskMode(source.mode);
        mask.closed = true;
        mask.feather = source.feather.amount;
        mask.expansion = source.expansion;
        mask.inverted = source.inverted;
        mask.opacity = source.opacity;

        QVector<BezierPoint> storedBezier;
        if (decodeBezierMetadata(source.name, &storedBezier)) {
            mask.points = storedBezier;
            stack.masks.append(mask);
            continue;
        }

        switch (source.shape) {
        case MaskShape::Rectangle:
            mask.points = rectToBezier(source.rect);
            break;
        case MaskShape::Ellipse:
            mask.points = ellipseToBezier(source.rect);
            break;
        case MaskShape::Polygon:
        case MaskShape::Path:
            mask.points.reserve(source.points.size());
            for (const QPointF &point : source.points)
                mask.points.append(makePoint(point));
            break;
        }

        stack.masks.append(mask);
    }

    return stack;
}

MaskSystem toMaskSystem(const ClipMaskStack &stack)
{
    MaskSystem system;
    for (const BezierMask &source : stack.masks) {
        Mask mask;
        mask.shape = MaskShape::Path;
        mask.mode = maskModeFromCombineMode(source.mode);
        mask.feather.amount = source.feather;
        mask.inverted = source.inverted;
        mask.opacity = qBound(0.0, source.opacity, 1.0);
        mask.expansion = source.expansion;
        mask.name = source.name;
        mask.points = flattenBezier(source);
        system.addMask(mask);
    }
    return system;
}

bool isDefault(const MaskSystem &system)
{
    return system.masks().isEmpty();
}

QJsonObject maskSystemToJson(const MaskSystem &system)
{
    return toJson(fromMaskSystem(system));
}

MaskSystem maskSystemFromJson(const QJsonObject &obj)
{
    return toMaskSystem(fromJson(obj));
}

ClipMaskStack applyPlanarCornerPinToMaskPath(const ClipMaskStack &stack,
                                             const planar::CornerSet &referenceCorners,
                                             const planar::CornerSet &trackedCorners)
{
    if (stack.isDefault() || !referenceCorners.isValid() || !trackedCorners.isValid())
        return stack;

    return transformStack(stack,
                          planar::homographyFromCorners(referenceCorners,
                                                        trackedCorners));
}

QMap<int, ClipMaskStack> bakePlanarTrackToMaskPathKeyframes(
    const ClipMaskStack &stack,
    const planar::CornerSet &referenceCorners,
    const QList<planar::Frame> &frames)
{
    QMap<int, ClipMaskStack> keyframes;
    if (stack.isDefault() || !referenceCorners.isValid())
        return keyframes;

    for (const planar::Frame &frame : frames) {
        if (!frame.corners.isValid())
            continue;
        keyframes.insert(frame.frameIndex,
                         applyPlanarCornerPinToMaskPath(stack,
                                                        referenceCorners,
                                                        frame.corners));
    }

    if (!containsKey(keyframes, 0))
        keyframes.insert(0, stack);
    return keyframes;
}

QMap<int, ClipMaskStack> bakePlanarTrackToMaskPathKeyframes(
    const ClipMaskStack &stack,
    const planartrack::PlanarTrack &track)
{
    QMap<int, ClipMaskStack> keyframes;
    if (stack.isDefault())
        return keyframes;

    keyframes.insert(track.refFrameIndex, stack);
    for (const planartrack::FrameResult &frame : track.frames)
        keyframes.insert(frame.frameIndex, transformStack(stack, frame.H));
    return keyframes;
}

QImage applyRasterAlphaMask(const QImage &sourceImage, const QVector<Mask> &masks)
{
    if (sourceImage.isNull() || masks.isEmpty())
        return sourceImage;

    const QImage matte = MaskSystem::generateMaskImage(masks, sourceImage.size());
    if (matte.isNull())
        return sourceImage;

    const bool rgba64 = sourceImage.format() == QImage::Format_RGBA64
        || sourceImage.format() == QImage::Format_RGBA64_Premultiplied;
    if (!rgba64)
        return MaskSystem::applyMask(sourceImage, matte);

    QImage src = sourceImage.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    QImage mask = matte;
    if (mask.format() != QImage::Format_Grayscale8)
        mask = mask.convertToFormat(QImage::Format_Grayscale8);
    if (mask.size() != src.size())
        mask = mask.scaled(src.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                   .convertToFormat(QImage::Format_Grayscale8);

    QImage result(src.size(), QImage::Format_RGBA64_Premultiplied);
    const int w = src.width();
    const int h = src.height();
    auto scale16 = [](quint16 value, quint16 alpha) -> quint16 {
        return static_cast<quint16>(
            (static_cast<quint32>(value) * static_cast<quint32>(alpha)) / 65535u);
    };

    for (int y = 0; y < h; ++y) {
        const QRgba64 *srcRow =
            reinterpret_cast<const QRgba64 *>(src.constScanLine(y));
        const uchar *maskRow = mask.constScanLine(y);
        QRgba64 *dstRow = reinterpret_cast<QRgba64 *>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const quint16 a = static_cast<quint16>(
                static_cast<quint16>(maskRow[x]) * 257u);
            const QRgba64 px = srcRow[x];
            dstRow[x] = qRgba64(scale16(px.red(), a),
                                scale16(px.green(), a),
                                scale16(px.blue(), a),
                                scale16(px.alpha(), a));
        }
    }

    return result;
}

} // namespace clipmask
