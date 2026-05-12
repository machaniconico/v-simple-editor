#include "SmartReframe.h"

#include <QJsonArray>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

struct SaliencyResult
{
    QVector<double> luma;
    QRectF subjectBounds;
    QPointF subjectCentroid;
    bool hasSubject = false;
};

double clampValue(double value, double minValue, double maxValue)
{
    return std::max(minValue, std::min(value, maxValue));
}

QVector<double> buildLumaMap(const QImage &image)
{
    QVector<double> luma;
    luma.resize(image.width() * image.height());

    for (int y = 0; y < image.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = row[x];
            luma[(y * image.width()) + x] =
                (0.299 * qRed(pixel)) +
                (0.587 * qGreen(pixel)) +
                (0.114 * qBlue(pixel));
        }
    }

    return luma;
}

SaliencyResult analyzeSaliency(const QImage &frame,
                               const QSize &sourceSize,
                               const QVector<double> &previousLuma,
                               const QSize &previousMapSize,
                               double motionWeight,
                               double massFraction)
{
    SaliencyResult result;
    if (frame.isNull() || !sourceSize.isValid()) {
        return result;
    }

    const int mapWidth = std::max(1, std::min(frame.width(), 160));
    const int mapHeight = std::max(1, static_cast<int>(std::lround(
        static_cast<double>(frame.height()) * static_cast<double>(mapWidth) /
        std::max(1, frame.width()))));
    const QSize mapSize(mapWidth, mapHeight);

    const QImage scaled = frame
        .scaled(mapSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_ARGB32);
    result.luma = buildLumaMap(scaled);

    if (result.luma.isEmpty()) {
        return result;
    }

    QVector<double> saliency(result.luma.size(), 0.0);
    const bool hasPrevious = (previousMapSize == mapSize) &&
                             (previousLuma.size() == result.luma.size());
    const double motion = clampValue(motionWeight, 0.0, 1.0);

    double totalMass = 0.0;
    double weightedX = 0.0;
    double weightedY = 0.0;

    for (int y = 0; y < mapHeight; ++y) {
        for (int x = 0; x < mapWidth; ++x) {
            const int index = (y * mapWidth) + x;
            const double center = result.luma[index];

            double neighborSum = 0.0;
            int neighborCount = 0;
            for (int ny = std::max(0, y - 1); ny <= std::min(mapHeight - 1, y + 1); ++ny) {
                for (int nx = std::max(0, x - 1); nx <= std::min(mapWidth - 1, x + 1); ++nx) {
                    if (nx == x && ny == y) {
                        continue;
                    }
                    neighborSum += result.luma[(ny * mapWidth) + nx];
                    ++neighborCount;
                }
            }

            const double laplacian = std::abs((center * neighborCount) - neighborSum);
            const double frameDiff = hasPrevious
                ? std::abs(center - previousLuma[index])
                : 0.0;
            const double energy = laplacian + (motion * frameDiff * 4.0);

            saliency[index] = energy;
            totalMass += energy;
            weightedX += energy * (x + 0.5);
            weightedY += energy * (y + 0.5);
        }
    }

    if (totalMass <= std::numeric_limits<double>::epsilon()) {
        return result;
    }

    result.hasSubject = true;

    const double scaleX = static_cast<double>(sourceSize.width()) / mapWidth;
    const double scaleY = static_cast<double>(sourceSize.height()) / mapHeight;
    result.subjectCentroid = QPointF(
        (weightedX / totalMass) * scaleX,
        (weightedY / totalMass) * scaleY);

    QVector<double> sortedSaliency = saliency;
    std::sort(sortedSaliency.begin(), sortedSaliency.end(), std::greater<double>());

    const double targetMass = totalMass * clampValue(massFraction, 0.0, 1.0);
    double cumulative = 0.0;
    double threshold = 0.0;
    for (double value : sortedSaliency) {
        cumulative += value;
        threshold = value;
        if (cumulative >= targetMass) {
            break;
        }
    }

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();

    for (int y = 0; y < mapHeight; ++y) {
        for (int x = 0; x < mapWidth; ++x) {
            const double value = saliency[(y * mapWidth) + x];
            if (value <= 0.0 || value < threshold) {
                continue;
            }

            minX = std::min(minX, x * scaleX);
            minY = std::min(minY, y * scaleY);
            maxX = std::max(maxX, (x + 1.0) * scaleX);
            maxY = std::max(maxY, (y + 1.0) * scaleY);
        }
    }

    if (minX == std::numeric_limits<double>::max()) {
        result.subjectBounds = QRectF(result.subjectCentroid, QSizeF(1.0, 1.0));
    } else {
        result.subjectBounds = QRectF(QPointF(minX, minY), QPointF(maxX, maxY)).normalized();
    }

    return result;
}

} // namespace

SmartReframe::SmartReframe(QObject *parent)
    : QObject(parent)
{
}

double SmartReframe::clamp01(double value)
{
    return clampValue(value, 0.0, 1.0);
}

QRectF SmartReframe::interpolateRect(const QRectF &a, const QRectF &b, double t)
{
    const double amount = clampValue(t, 0.0, 1.0);
    return QRectF(
        a.x() + ((b.x() - a.x()) * amount),
        a.y() + ((b.y() - a.y()) * amount),
        a.width() + ((b.width() - a.width()) * amount),
        a.height() + ((b.height() - a.height()) * amount));
}

void SmartReframe::setTargetAspect(double w, double h)
{
    if (w > 0.0) {
        m_targetAspectW = w;
    }
    if (h > 0.0) {
        m_targetAspectH = h;
    }
}

void SmartReframe::setSourceSize(QSize size)
{
    if (size.isValid()) {
        m_sourceSize = size;
    }
}

void SmartReframe::setSmoothness(double smoothness)
{
    m_smoothness = clamp01(smoothness);
}

void SmartReframe::setMotionWeight(double motionWeight)
{
    m_motionWeight = clamp01(motionWeight);
}

void SmartReframe::setSaliencyMassFraction(double saliencyMassFraction)
{
    m_saliencyMassFraction = clamp01(saliencyMassFraction);
}

QRectF SmartReframe::defaultCropRect() const
{
    if (!m_sourceSize.isValid()) {
        return QRectF();
    }

    const double sourceWidth = m_sourceSize.width();
    const double sourceHeight = m_sourceSize.height();
    const double targetAspect = (m_targetAspectW > 0.0 && m_targetAspectH > 0.0)
        ? (m_targetAspectW / m_targetAspectH)
        : (sourceWidth / std::max(1.0, sourceHeight));
    const double sourceAspect = sourceWidth / std::max(1.0, sourceHeight);

    double cropWidth = sourceWidth;
    double cropHeight = sourceHeight;
    if (sourceAspect > targetAspect) {
        cropHeight = sourceHeight;
        cropWidth = cropHeight * targetAspect;
    } else {
        cropWidth = sourceWidth;
        cropHeight = cropWidth / std::max(targetAspect, std::numeric_limits<double>::epsilon());
    }

    const double x = (sourceWidth - cropWidth) * 0.5;
    const double y = (sourceHeight - cropHeight) * 0.5;
    return QRectF(x, y, cropWidth, cropHeight);
}

QRectF SmartReframe::solveCropRect(const QRectF &subjectBounds, const QPointF &subjectCentroid) const
{
    QRectF crop = defaultCropRect();
    if (!crop.isValid() || crop.isEmpty()) {
        return crop;
    }

    const double halfWidth = crop.width() * 0.5;
    const double halfHeight = crop.height() * 0.5;

    double centerX = subjectCentroid.x();
    double centerY = subjectCentroid.y();

    if (subjectBounds.isValid() && !subjectBounds.isEmpty()) {
        const double minCenterX = subjectBounds.right() - halfWidth;
        const double maxCenterX = subjectBounds.left() + halfWidth;
        const double minCenterY = subjectBounds.bottom() - halfHeight;
        const double maxCenterY = subjectBounds.top() + halfHeight;

        if (minCenterX <= maxCenterX) {
            centerX = clampValue(centerX, minCenterX, maxCenterX);
        } else {
            centerX = subjectBounds.center().x();
        }

        if (minCenterY <= maxCenterY) {
            centerY = clampValue(centerY, minCenterY, maxCenterY);
        } else {
            centerY = subjectBounds.center().y();
        }
    }

    centerX = clampValue(centerX, halfWidth, m_sourceSize.width() - halfWidth);
    centerY = clampValue(centerY, halfHeight, m_sourceSize.height() - halfHeight);

    crop.moveCenter(QPointF(centerX, centerY));
    crop.moveLeft(clampValue(crop.left(), 0.0, m_sourceSize.width() - crop.width()));
    crop.moveTop(clampValue(crop.top(), 0.0, m_sourceSize.height() - crop.height()));
    return crop;
}

void SmartReframe::analyzeFrame(double timeSec, const QImage &frame)
{
    if (frame.isNull()) {
        return;
    }

    if (!m_sourceSize.isValid()) {
        m_sourceSize = frame.size();
    }

    const SaliencyResult result = analyzeSaliency(
        frame,
        m_sourceSize,
        m_previousLuma,
        m_previousMapSize,
        m_motionWeight,
        m_saliencyMassFraction);

    TrackKey key;
    key.timeSec = timeSec;
    key.rect = result.hasSubject
        ? solveCropRect(result.subjectBounds, result.subjectCentroid)
        : defaultCropRect();

    m_rawTrack.append(key);
    m_analysisDirty = true;

    m_previousLuma = result.luma;
    if (!frame.isNull()) {
        const int mapWidth = std::max(1, std::min(frame.width(), 160));
        const int mapHeight = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(frame.height()) * static_cast<double>(mapWidth) /
            std::max(1, frame.width()))));
        m_previousMapSize = QSize(mapWidth, mapHeight);
    }
}

void SmartReframe::rebuildSmoothedTrack()
{
    m_smoothedTrack.clear();
    if (m_rawTrack.isEmpty()) {
        return;
    }

    m_smoothedTrack.reserve(m_rawTrack.size());
    if (m_smoothness <= 0.0) {
        m_smoothedTrack = m_rawTrack;
        return;
    }

    const double alpha = 1.0 - (0.85 * m_smoothness);

    TrackKey current = m_rawTrack.first();
    m_smoothedTrack.append(current);

    for (int i = 1; i < m_rawTrack.size(); ++i) {
        const QRectF &target = m_rawTrack[i].rect;
        QRectF blended(
            current.rect.x() + ((target.x() - current.rect.x()) * alpha),
            current.rect.y() + ((target.y() - current.rect.y()) * alpha),
            current.rect.width() + ((target.width() - current.rect.width()) * alpha),
            current.rect.height() + ((target.height() - current.rect.height()) * alpha));

        current.timeSec = m_rawTrack[i].timeSec;
        current.rect = blended;
        m_smoothedTrack.append(current);
    }
}

void SmartReframe::finalizeAnalysis()
{
    if (!m_analysisDirty) {
        return;
    }

    std::sort(m_rawTrack.begin(), m_rawTrack.end(), [](const TrackKey &a, const TrackKey &b) {
        return a.timeSec < b.timeSec;
    });

    rebuildSmoothedTrack();
    m_analysisDirty = false;
}

QRectF SmartReframe::interpolateTrackRect(const QVector<TrackKey> &track, double timeSec) const
{
    if (track.isEmpty()) {
        return defaultCropRect();
    }

    if (timeSec <= track.first().timeSec) {
        return track.first().rect;
    }
    if (timeSec >= track.last().timeSec) {
        return track.last().rect;
    }

    for (int i = 1; i < track.size(); ++i) {
        if (timeSec > track[i].timeSec) {
            continue;
        }

        const TrackKey &left = track[i - 1];
        const TrackKey &right = track[i];
        const double span = right.timeSec - left.timeSec;
        if (span <= std::numeric_limits<double>::epsilon()) {
            return right.rect;
        }
        const double amount = (timeSec - left.timeSec) / span;
        return interpolateRect(left.rect, right.rect, amount);
    }

    return track.last().rect;
}

QRectF SmartReframe::cropRectAt(double timeSec) const
{
    if (!m_smoothedTrack.isEmpty()) {
        return interpolateTrackRect(m_smoothedTrack, timeSec);
    }
    if (!m_rawTrack.isEmpty()) {
        return interpolateTrackRect(m_rawTrack, timeSec);
    }
    return defaultCropRect();
}

QImage SmartReframe::applyReframe(const QImage &frame, double timeSec, QSize outputSize) const
{
    if (frame.isNull() || !outputSize.isValid()) {
        return QImage();
    }

    QRectF sourceCrop = cropRectAt(timeSec);
    const QSize referenceSize = m_sourceSize.isValid() ? m_sourceSize : frame.size();
    if (referenceSize.isValid() && referenceSize != frame.size()) {
        const double scaleX = static_cast<double>(frame.width()) / referenceSize.width();
        const double scaleY = static_cast<double>(frame.height()) / referenceSize.height();
        sourceCrop = QRectF(
            sourceCrop.x() * scaleX,
            sourceCrop.y() * scaleY,
            sourceCrop.width() * scaleX,
            sourceCrop.height() * scaleY);
    }

    sourceCrop = sourceCrop.intersected(QRectF(QPointF(0.0, 0.0), QSizeF(frame.size())));
    if (!sourceCrop.isValid() || sourceCrop.isEmpty()) {
        sourceCrop = QRectF(QPointF(0.0, 0.0), QSizeF(frame.size()));
    }

    const QImage::Format outputFormat =
        frame.format() == QImage::Format_Invalid
            ? QImage::Format_ARGB32_Premultiplied
            : frame.format();
    QImage output(outputSize, outputFormat);
    output.fill(Qt::transparent);

    QPainter painter(&output);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRectF(QPointF(0.0, 0.0), QSizeF(outputSize)), frame, sourceCrop);
    painter.end();

    return output;
}

QJsonObject SmartReframe::toJson() const
{
    const QVector<TrackKey> &track = m_smoothedTrack.isEmpty() ? m_rawTrack : m_smoothedTrack;

    QJsonArray trackArray;
    for (const TrackKey &key : track) {
        QJsonObject entry;
        entry["t"] = key.timeSec;
        entry["x"] = key.rect.x();
        entry["y"] = key.rect.y();
        entry["w"] = key.rect.width();
        entry["h"] = key.rect.height();
        trackArray.append(entry);
    }

    QJsonObject targetAspect;
    targetAspect["w"] = m_targetAspectW;
    targetAspect["h"] = m_targetAspectH;

    QJsonObject sourceSize;
    sourceSize["w"] = m_sourceSize.width();
    sourceSize["h"] = m_sourceSize.height();

    QJsonObject json;
    json["targetAspect"] = targetAspect;
    json["smoothness"] = m_smoothness;
    json["motionWeight"] = m_motionWeight;
    json["saliencyMassFraction"] = m_saliencyMassFraction;
    json["sourceSize"] = sourceSize;
    json["cropTrack"] = trackArray;
    return json;
}

void SmartReframe::fromJson(const QJsonObject &json)
{
    const QJsonObject targetAspect = json["targetAspect"].toObject();
    const double targetW = targetAspect["w"].toDouble(m_targetAspectW);
    const double targetH = targetAspect["h"].toDouble(m_targetAspectH);
    if (targetW > 0.0 && targetH > 0.0) {
        m_targetAspectW = targetW;
        m_targetAspectH = targetH;
    }

    m_smoothness = clamp01(json["smoothness"].toDouble(m_smoothness));
    m_motionWeight = clamp01(json["motionWeight"].toDouble(m_motionWeight));
    m_saliencyMassFraction = clamp01(
        json["saliencyMassFraction"].toDouble(m_saliencyMassFraction));

    const QJsonObject sourceSize = json["sourceSize"].toObject();
    const QSize size(sourceSize["w"].toInt(m_sourceSize.width()),
                     sourceSize["h"].toInt(m_sourceSize.height()));
    if (size.isValid()) {
        m_sourceSize = size;
    }

    m_rawTrack.clear();
    m_smoothedTrack.clear();
    m_previousLuma.clear();
    m_previousMapSize = QSize();

    const QJsonArray trackArray = json["cropTrack"].toArray();
    m_smoothedTrack.reserve(trackArray.size());
    for (const QJsonValue &value : trackArray) {
        const QJsonObject entry = value.toObject();
        TrackKey key;
        key.timeSec = entry["t"].toDouble();
        key.rect = QRectF(
            entry["x"].toDouble(),
            entry["y"].toDouble(),
            entry["w"].toDouble(),
            entry["h"].toDouble());
        m_smoothedTrack.append(key);
    }

    m_analysisDirty = false;
}
