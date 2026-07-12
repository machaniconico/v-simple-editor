#include "Rotoscope.h"

#include <QDir>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

// ============================================================
//  RotoPoint serialisation
// ============================================================

QJsonObject RotoPoint::toJson() const
{
    QJsonObject obj;

    QJsonObject pos;
    pos["x"] = position.x();
    pos["y"] = position.y();
    obj["position"] = pos;

    QJsonObject hIn;
    hIn["x"] = handleIn.x();
    hIn["y"] = handleIn.y();
    obj["handleIn"] = hIn;

    QJsonObject hOut;
    hOut["x"] = handleOut.x();
    hOut["y"] = handleOut.y();
    obj["handleOut"] = hOut;

    return obj;
}

RotoPoint RotoPoint::fromJson(const QJsonObject &obj)
{
    RotoPoint rp;

    QJsonObject pos = obj["position"].toObject();
    rp.position = QPointF(pos["x"].toDouble(), pos["y"].toDouble());

    QJsonObject hIn = obj["handleIn"].toObject();
    rp.handleIn = QPointF(hIn["x"].toDouble(), hIn["y"].toDouble());

    QJsonObject hOut = obj["handleOut"].toObject();
    rp.handleOut = QPointF(hOut["x"].toDouble(), hOut["y"].toDouble());

    return rp;
}

// ============================================================
//  RotoPath serialisation
// ============================================================

QJsonObject RotoPath::toJson() const
{
    QJsonObject obj;
    obj["closed"]  = closed;
    obj["feather"] = feather;

    QJsonArray pts;
    for (const auto &p : points)
        pts.append(p.toJson());
    obj["points"] = pts;

    return obj;
}

RotoPath RotoPath::fromJson(const QJsonObject &obj)
{
    RotoPath rp;
    rp.closed  = obj["closed"].toBool(true);
    rp.feather = obj["feather"].toDouble(0.0);

    QJsonArray pts = obj["points"].toArray();
    rp.points.reserve(pts.size());
    for (const auto &val : pts)
        rp.points.append(RotoPoint::fromJson(val.toObject()));

    return rp;
}

// ============================================================
//  RotoKeyframe serialisation
// ============================================================

QJsonObject RotoKeyframe::toJson() const
{
    QJsonObject obj;
    obj["frameNumber"] = frameNumber;
    obj["path"]        = path.toJson();
    return obj;
}

RotoKeyframe RotoKeyframe::fromJson(const QJsonObject &obj)
{
    RotoKeyframe kf;
    kf.frameNumber = obj["frameNumber"].toInt(0);
    kf.path        = RotoPath::fromJson(obj["path"].toObject());
    return kf;
}

// ============================================================
//  Rotoscope — keyframe management
// ============================================================

void Rotoscope::addKeyframe(int frameNumber, const RotoPath &path)
{
    // Remove existing keyframe at the same frame, if any
    removeKeyframe(frameNumber);

    RotoKeyframe kf;
    kf.frameNumber = frameNumber;
    kf.path = path;

    // Insert sorted by frameNumber
    auto it = std::lower_bound(m_keyframes.begin(), m_keyframes.end(), kf,
                               [](const RotoKeyframe &a, const RotoKeyframe &b) {
                                   return a.frameNumber < b.frameNumber;
                               });
    m_keyframes.insert(it, kf);
}

void Rotoscope::removeKeyframe(int frameNumber)
{
    auto it = std::remove_if(m_keyframes.begin(), m_keyframes.end(),
                             [frameNumber](const RotoKeyframe &kf) {
                                 return kf.frameNumber == frameNumber;
                             });
    m_keyframes.erase(it, m_keyframes.end());
}

// ============================================================
//  Rotoscope — bracketing keyframe lookup
// ============================================================

bool Rotoscope::findBracketingKeyframes(int frameNumber,
                                        const RotoKeyframe *&prev,
                                        const RotoKeyframe *&next) const
{
    if (m_keyframes.isEmpty()) {
        prev = nullptr;
        next = nullptr;
        return false;
    }

    // Find first keyframe with frameNumber >= target
    auto it = std::lower_bound(m_keyframes.cbegin(), m_keyframes.cend(), frameNumber,
                               [](const RotoKeyframe &kf, int frame) {
                                   return kf.frameNumber < frame;
                               });

    // Exact match
    if (it != m_keyframes.cend() && it->frameNumber == frameNumber) {
        prev = &(*it);
        next = &(*it);
        return true;
    }

    // Before all keyframes — clamp to first
    if (it == m_keyframes.cbegin()) {
        prev = &m_keyframes.first();
        next = &m_keyframes.first();
        return true;
    }

    // After all keyframes — clamp to last
    if (it == m_keyframes.cend()) {
        prev = &m_keyframes.last();
        next = &m_keyframes.last();
        return true;
    }

    // Between two keyframes
    next = &(*it);
    prev = &(*(it - 1));
    return true;
}

// ============================================================
//  Rotoscope — path interpolation
// ============================================================

static QPointF lerpPoint(const QPointF &a, const QPointF &b, double t)
{
    return a + (b - a) * t;
}

RotoPath Rotoscope::interpolatePaths(const RotoPath &pathA, const RotoPath &pathB, double t)
{
    RotoPath result;
    result.closed  = pathA.closed;
    result.feather = pathA.feather + (pathB.feather - pathA.feather) * t;

    int countA = pathA.points.size();
    int countB = pathB.points.size();
    int maxCount = std::max(countA, countB);

    if (maxCount == 0)
        return result;

    if (countA == 0)
        return pathB;
    if (countB == 0)
        return pathA;

    result.points.reserve(maxCount);

    for (int i = 0; i < maxCount; ++i) {
        // Pad shorter path by duplicating its last point
        const RotoPoint &ptA = (i < countA) ? pathA.points[i] : pathA.points.last();
        const RotoPoint &ptB = (i < countB) ? pathB.points[i] : pathB.points.last();

        RotoPoint rp;
        rp.position  = lerpPoint(ptA.position,  ptB.position,  t);
        rp.handleIn  = lerpPoint(ptA.handleIn,  ptB.handleIn,  t);
        rp.handleOut = lerpPoint(ptA.handleOut, ptB.handleOut, t);
        result.points.append(rp);
    }

    return result;
}

RotoPath Rotoscope::getPathAtFrame(int frameNumber) const
{
    const RotoKeyframe *prev = nullptr;
    const RotoKeyframe *next = nullptr;

    if (!findBracketingKeyframes(frameNumber, prev, next))
        return RotoPath();

    // Exact keyframe or clamped to a single keyframe
    if (prev == next || prev->frameNumber == next->frameNumber)
        return prev->path;

    // Interpolation factor
    double t = static_cast<double>(frameNumber - prev->frameNumber)
             / static_cast<double>(next->frameNumber - prev->frameNumber);

    return interpolatePaths(prev->path, next->path, t);
}

// ============================================================
//  Rotoscope — QPainterPath conversion
// ============================================================

QPainterPath Rotoscope::pathToQPainterPath(const RotoPath &rotoPath)
{
    QPainterPath qpath;

    if (rotoPath.points.isEmpty())
        return qpath;

    const auto &pts = rotoPath.points;
    qpath.moveTo(pts[0].position);

    for (int i = 1; i < pts.size(); ++i) {
        const RotoPoint &prev = pts[i - 1];
        const RotoPoint &curr = pts[i];

        // Cubic bezier: control1 = prev.handleOut, control2 = curr.handleIn
        qpath.cubicTo(prev.handleOut, curr.handleIn, curr.position);
    }

    // Close the path: last point -> first point
    if (rotoPath.closed && pts.size() > 1) {
        const RotoPoint &last  = pts.last();
        const RotoPoint &first = pts.first();
        qpath.cubicTo(last.handleOut, first.handleIn, first.position);
        qpath.closeSubpath();
    }

    return qpath;
}

// ============================================================
//  Rotoscope — feathering (iterative box blur)
// ============================================================

void Rotoscope::boxBlur(QImage &img, int radius)
{
    if (radius <= 0)
        return;

    const int w = img.width();
    const int h = img.height();
    if (w == 0 || h == 0)
        return;

    QImage tmp(w, h, QImage::Format_Grayscale8);

    // Horizontal pass
    for (int y = 0; y < h; ++y) {
        const uchar *srcRow = img.constScanLine(y);
        uchar *dstRow = tmp.scanLine(y);

        int sum = 0;
        int count = 0;
        for (int kx = 0; kx <= radius; ++kx) {
            if (kx < w) { sum += srcRow[kx]; ++count; }
        }
        dstRow[0] = static_cast<uchar>(sum / count);

        for (int x = 1; x < w; ++x) {
            int addIdx = x + radius;
            int remIdx = x - radius - 1;
            if (addIdx < w) { sum += srcRow[addIdx]; ++count; }
            if (remIdx >= 0) { sum -= srcRow[remIdx]; --count; }
            dstRow[x] = static_cast<uchar>(count > 0 ? sum / count : 0);
        }
    }

    // Vertical pass
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        int count = 0;
        for (int ky = 0; ky <= radius; ++ky) {
            if (ky < h) { sum += tmp.constScanLine(ky)[x]; ++count; }
        }
        img.scanLine(0)[x] = static_cast<uchar>(sum / count);

        for (int y = 1; y < h; ++y) {
            int addIdx = y + radius;
            int remIdx = y - radius - 1;
            if (addIdx < h) { sum += tmp.constScanLine(addIdx)[x]; ++count; }
            if (remIdx >= 0) { sum -= tmp.constScanLine(remIdx)[x]; --count; }
            img.scanLine(y)[x] = static_cast<uchar>(count > 0 ? sum / count : 0);
        }
    }
}

QImage Rotoscope::estimateFeather(const RotoPath &path, const QSize &canvasSize)
{
    // Render the binary mask first
    QPainterPath qpath = pathToQPainterPath(path);

    QImage mask(canvasSize, QImage::Format_Grayscale8);
    mask.fill(0);

    if (qpath.isEmpty())
        return mask;

    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255));
    painter.drawPath(qpath);
    painter.end();

    // Apply feathering if requested
    if (path.feather > 0.0) {
        int radius = static_cast<int>(std::ceil(path.feather));

        // Three iterations of box blur approximate a Gaussian blur
        boxBlur(mask, radius);
        boxBlur(mask, radius);
        boxBlur(mask, radius);
    }

    return mask;
}

// ============================================================
//  Rotoscope — mask rendering
// ============================================================

QImage Rotoscope::renderMask(int frameNumber, const QSize &canvasSize) const
{
    if (m_keyframes.isEmpty() || canvasSize.isEmpty())
        return QImage();

    RotoPath path = getPathAtFrame(frameNumber);
    return estimateFeather(path, canvasSize);
}

QVector<QImage> Rotoscope::renderMaskSequence(int startFrame, int endFrame,
                                               const QSize &canvasSize) const
{
    QVector<QImage> masks;
    if (startFrame > endFrame)
        return masks;

    masks.reserve(endFrame - startFrame + 1);
    for (int f = startFrame; f <= endFrame; ++f)
        masks.append(renderMask(f, canvasSize));

    return masks;
}

// ============================================================
//  Rotoscope — apply mask to video frame
// ============================================================

QImage Rotoscope::applyToFrame(const QImage &sourceFrame, const QImage &maskFrame)
{
    QImage src = sourceFrame;
    if (src.format() != QImage::Format_ARGB32_Premultiplied)
        src = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage mask = maskFrame;
    if (mask.format() != QImage::Format_Grayscale8)
        mask = mask.convertToFormat(QImage::Format_Grayscale8);

    // Scale mask to match source if dimensions differ
    if (mask.size() != src.size())
        mask = mask.scaled(src.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    const int w = src.width();
    const int h = src.height();

    QImage result(w, h, QImage::Format_ARGB32_Premultiplied);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcRow  = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const uchar *maskRow = mask.constScanLine(y);
        QRgb *dstRow = reinterpret_cast<QRgb *>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            int maskVal = maskRow[x];  // 0-255
            int a = qAlpha(srcRow[x]) * maskVal / 255;
            int r = qRed(srcRow[x])   * maskVal / 255;
            int g = qGreen(srcRow[x]) * maskVal / 255;
            int b = qBlue(srcRow[x])  * maskVal / 255;
            dstRow[x] = qRgba(r, g, b, a);
        }
    }

    return result;
}

// ============================================================
//  Rotoscope — export mask sequence as PNG files
// ============================================================

bool Rotoscope::exportMaskSequence(int startFrame, int endFrame,
                                   const QSize &canvasSize,
                                   const QString &outputDir) const
{
    QDir dir(outputDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return false;

    for (int f = startFrame; f <= endFrame; ++f) {
        QImage mask = renderMask(f, canvasSize);
        if (mask.isNull())
            continue;

        QString filename = dir.filePath(
            QStringLiteral("roto_mask_%1.png").arg(f, 6, 10, QLatin1Char('0')));

        if (!mask.save(filename, "PNG"))
            return false;
    }

    return true;
}

// ============================================================
//  Rotoscope — serialisation
// ============================================================

QJsonObject Rotoscope::toJson() const
{
    QJsonObject obj;

    QJsonArray kfArr;
    for (const auto &kf : m_keyframes)
        kfArr.append(kf.toJson());
    obj["keyframes"] = kfArr;

    return obj;
}

void Rotoscope::fromJson(const QJsonObject &obj)
{
    m_keyframes.clear();

    QJsonArray kfArr = obj["keyframes"].toArray();
    m_keyframes.reserve(kfArr.size());
    for (const auto &val : kfArr)
        m_keyframes.append(RotoKeyframe::fromJson(val.toObject()));

    // Ensure sorted order after deserialisation
    std::sort(m_keyframes.begin(), m_keyframes.end(),
              [](const RotoKeyframe &a, const RotoKeyframe &b) {
                  return a.frameNumber < b.frameNumber;
              });
}
