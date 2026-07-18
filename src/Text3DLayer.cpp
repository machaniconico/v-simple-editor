#include "Text3DLayer.h"

#include "Camera3D.h"
#include "ExtrudedMesh.h"
#include "SoftRaster3D.h"

#include <QFontMetricsF>
#include <QMatrix4x4>
#include <QPainter>
#include <QPolygonF>
#include <QStringList>
#include <QTransform>
#include <algorithm>
#include <cmath>

namespace {

constexpr double kMinScaleComponent = 0.05;
constexpr int kGlyphPadding = 6;

// Degrees-per-second of time-based spin applied to the extruded mesh, scaled
// by the rotation-anim axis components (see Text3DLayer.h).
constexpr double kExtrudeSpinDegPerSec = 90.0;

// Glyph-contour flattening tolerance passed to mesh3d::glyphContours().
constexpr double kExtrudeFlatness = 0.5;
constexpr int kPreviewProxyLongEdge = 640;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

struct ProjectedVertex {
    QPointF screen;
    double depth = 0.0;
    bool valid = false;
};

struct GlyphQuad {
    QImage image;
    QPolygonF screenQuad;
    double depth = 0.0;
};

QJsonObject vectorToJson(const QVector3D &value)
{
    QJsonObject obj;
    obj[QStringLiteral("x")] = static_cast<double>(value.x());
    obj[QStringLiteral("y")] = static_cast<double>(value.y());
    obj[QStringLiteral("z")] = static_cast<double>(value.z());
    return obj;
}

QVector3D vectorFromJson(const QJsonObject &obj, const QVector3D &fallback = QVector3D())
{
    return QVector3D(
        static_cast<float>(obj[QStringLiteral("x")].toDouble(fallback.x())),
        static_cast<float>(obj[QStringLiteral("y")].toDouble(fallback.y())),
        static_cast<float>(obj[QStringLiteral("z")].toDouble(fallback.z())));
}

bool cameraEqualsDefault(const Camera3D &camera)
{
    const Camera3D defaultCamera;
    return camera.toJson() == defaultCamera.toJson();
}

Camera3DState cameraStateAtTime(const Camera3D &cam, double time)
{
    return cam.hasAnimation() ? cam.getCameraAt(time) : cam.camera();
}

QVector3D safeForwardVector(const Camera3DState &state)
{
    QVector3D forward = state.target - state.position;
    if (forward.lengthSquared() < 1.0e-6f)
        forward = QVector3D(0.0f, 0.0f, -1.0f);
    return forward.normalized();
}

QMatrix4x4 buildViewMatrix(const Camera3DState &state)
{
    const QVector3D forward = safeForwardVector(state);

    QVector3D up(0.0f, 1.0f, 0.0f);
    if (std::abs(QVector3D::dotProduct(forward, up)) > 0.98f)
        up = QVector3D(1.0f, 0.0f, 0.0f);

    if (std::abs(state.roll) > 0.001)
        up = QQuaternion::fromAxisAndAngle(forward, static_cast<float>(state.roll)).rotatedVector(up);

    QMatrix4x4 view;
    view.lookAt(state.position, state.position + forward, up);
    return view;
}

QMatrix4x4 buildProjectionMatrix(const Camera3DState &state, const QSize &size)
{
    QMatrix4x4 projection;
    const float aspect = size.height() > 0
        ? static_cast<float>(size.width()) / static_cast<float>(size.height())
        : 1.0f;

    const float fov = static_cast<float>(state.fov > 0.001 ? state.fov : 60.0);
    const float nearPlane = static_cast<float>(std::max(0.001, state.nearPlane));
    const float farPlane = static_cast<float>(std::max(nearPlane + 1.0, state.farPlane));
    projection.perspective(fov, aspect, nearPlane, farPlane);
    return projection;
}

void foldCameraIntoExtrudeOrbit(const Camera3D &camera, double time,
                                double &yaw, double &pitch, double &cameraDistance)
{
    if (cameraEqualsDefault(camera))
        return;

    const Camera3DState state = cameraStateAtTime(camera, time);
    const QVector3D forward = safeForwardVector(state);
    const double forwardX = static_cast<double>(forward.x());
    const double forwardY = std::clamp(static_cast<double>(forward.y()), -1.0, 1.0);
    const double forwardZ = static_cast<double>(forward.z());

    const double cameraYaw = std::atan2(-forwardX, -forwardZ) * kRadToDeg;
    const double cameraPitch = std::asin(forwardY) * kRadToDeg;
    if (std::isfinite(cameraYaw))
        yaw += cameraYaw;
    if (std::isfinite(cameraPitch))
        pitch += cameraPitch;

    const QVector3D cameraSpan = state.target - state.position;
    const double spanLength = std::sqrt(static_cast<double>(cameraSpan.lengthSquared()));
    if (std::isfinite(spanLength) && spanLength > 1.0e-6)
        cameraDistance *= spanLength;

    if (std::isfinite(state.fov) && state.fov > 0.001) {
        const double fov = std::clamp(state.fov, 1.0, 179.0);
        const double fovScale = std::tan(fov * kDegToRad * 0.5)
            / std::tan(60.0 * kDegToRad * 0.5);
        if (std::isfinite(fovScale) && fovScale > 0.0)
            cameraDistance *= fovScale;
    }
}

ProjectedVertex projectVertex(const QVector3D &worldPoint,
                              const QMatrix4x4 &view,
                              const QMatrix4x4 &projection,
                              const QSize &size)
{
    const QVector4D viewPoint = view * QVector4D(worldPoint, 1.0f);
    if (viewPoint.z() >= -0.001f)
        return {};

    const QVector4D clipPoint = projection * viewPoint;
    if (std::abs(clipPoint.w()) < 1.0e-6f)
        return {};

    const double invW = 1.0 / static_cast<double>(clipPoint.w());
    const double ndcX = static_cast<double>(clipPoint.x()) * invW;
    const double ndcY = static_cast<double>(clipPoint.y()) * invW;

    ProjectedVertex projected;
    projected.screen = QPointF(
        (ndcX + 1.0) * 0.5 * static_cast<double>(size.width()),
        (1.0 - ndcY) * 0.5 * static_cast<double>(size.height()));
    projected.depth = static_cast<double>(-viewPoint.z());
    projected.valid = std::isfinite(projected.screen.x())
        && std::isfinite(projected.screen.y())
        && std::isfinite(projected.depth);
    return projected;
}

double polygonArea(const QPolygonF &polygon)
{
    if (polygon.size() < 3)
        return 0.0;

    double area = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &a = polygon[i];
        const QPointF &b = polygon[(i + 1) % polygon.size()];
        area += (a.x() * b.y()) - (b.x() * a.y());
    }
    return std::abs(area) * 0.5;
}

QImage rasterizeGlyph(const QString &glyph, const QFont &font, const QRectF &tightBounds)
{
    const int width = std::max(1, static_cast<int>(std::ceil(tightBounds.width())) + kGlyphPadding * 2);
    const int height = std::max(1, static_cast<int>(std::ceil(tightBounds.height())) + kGlyphPadding * 2);

    QImage image(QSize(width, height), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(Qt::white);
    painter.setFont(font);

    const QPointF baseline(
        static_cast<double>(kGlyphPadding) - tightBounds.left(),
        static_cast<double>(kGlyphPadding) - tightBounds.top());
    painter.drawText(baseline, glyph);
    painter.end();

    return image;
}

} // namespace

Text3DLayer::Text3DLayer(QObject *parent)
    : QObject(parent)
{
}

void Text3DLayer::setText(const QString &text, const QFont &font)
{
    m_text = text;
    m_font = font;
    m_cacheValid = false;
}

void Text3DLayer::setPerCharRotation(QVector3D perAxisAmount)
{
    m_perCharRotation = perAxisAmount;
}

void Text3DLayer::setPerCharPosition(QVector3D offset)
{
    m_perCharPosition = offset;
}

void Text3DLayer::setPerCharScale(QVector3D scaleAmount)
{
    m_perCharScale = scaleAmount;
}

void Text3DLayer::setCameraDistance(double distance)
{
    m_cameraDistance = distance;
}

void Text3DLayer::setRotationAnimAxis(QVector3D tumbleAxis)
{
    m_rotationAnimAxis = tumbleAxis;
}

void Text3DLayer::setCamera(const Camera3D &camera)
{
    m_camera = camera;
}

const Camera3D &Text3DLayer::camera() const
{
    return m_camera;
}

void Text3DLayer::setExtrudeEnabled(bool on)
{
    m_extrudeEnabled = on;
}

bool Text3DLayer::extrudeEnabled() const
{
    return m_extrudeEnabled;
}

void Text3DLayer::setExtrude(double depth, double bevelDepth, double bevelWidth, int bevelSegments)
{
    m_extrudeDepth = depth;
    m_extrudeBevelDepth = bevelDepth;
    m_extrudeBevelWidth = bevelWidth;
    m_extrudeBevelSegments = bevelSegments;
    m_cacheValid = false;
}

double Text3DLayer::extrudeDepth() const
{
    return m_extrudeDepth;
}

double Text3DLayer::extrudeBevelDepth() const
{
    return m_extrudeBevelDepth;
}

double Text3DLayer::extrudeBevelWidth() const
{
    return m_extrudeBevelWidth;
}

int Text3DLayer::extrudeBevelSegments() const
{
    return m_extrudeBevelSegments;
}

void Text3DLayer::setMaterial(QColor frontColor, QColor sideColor, QColor ambient, QVector3D lightDir)
{
    m_frontColor = frontColor;
    m_sideColor = sideColor;
    m_ambient = ambient;
    m_lightDir = lightDir;
}

QColor Text3DLayer::materialFrontColor() const
{
    return m_frontColor;
}

QColor Text3DLayer::materialSideColor() const
{
    return m_sideColor;
}

QColor Text3DLayer::materialAmbient() const
{
    return m_ambient;
}

QVector3D Text3DLayer::materialLightDir() const
{
    return m_lightDir;
}

void Text3DLayer::setExtrudeYawPitch(double yawDeg, double pitchDeg)
{
    m_extrudeBaseYaw = yawDeg;
    m_extrudeBasePitch = pitchDeg;
}

double Text3DLayer::extrudeBaseYaw() const
{
    return m_extrudeBaseYaw;
}

double Text3DLayer::extrudeBasePitch() const
{
    return m_extrudeBasePitch;
}

int Text3DLayer::meshBuildCount() const
{
    return m_meshBuildCount;
}

double Text3DLayer::characterProgress(int characterIndex, double time) const
{
    if (m_staggerDuration <= 0.0)
        return time >= static_cast<double>(characterIndex) * m_staggerDelay ? 1.0 : 0.0;

    const double startTime = static_cast<double>(characterIndex) * m_staggerDelay;
    const double rawProgress = (time - startTime) / m_staggerDuration;
    return std::clamp(rawProgress, 0.0, 1.0);
}

QImage Text3DLayer::renderFrame(QSize size, double time, const Camera3D &cam) const
{
    if (m_extrudeEnabled)
        return renderFrameExtruded(size, time, cam);
    return renderFrameQuads(size, time, cam);
}

QSize text3DPreviewProxySize(QSize fullSize)
{
    if (!fullSize.isValid() || fullSize.width() <= 0 || fullSize.height() <= 0)
        return {};

    const int longDim = std::max(fullSize.width(), fullSize.height());
    if (longDim <= 0)
        return {};

    const double scale = std::min(0.5, static_cast<double>(kPreviewProxyLongEdge) / static_cast<double>(longDim));
    return QSize(
        std::max(1, static_cast<int>(std::round(static_cast<double>(fullSize.width()) * scale))),
        std::max(1, static_cast<int>(std::round(static_cast<double>(fullSize.height()) * scale))));
}

bool shouldUseText3DPreviewProxy(const Text3DLayer &layer, QSize frameSize, int threshold)
{
    if (threshold <= 0 || !layer.extrudeEnabled())
        return false;
    if (!frameSize.isValid() || frameSize.width() <= 0 || frameSize.height() <= 0)
        return false;
    return std::max(frameSize.width(), frameSize.height()) > threshold;
}

QImage renderText3DProxy(const Text3DLayer &layer, QSize fullSize, double time, const Camera3D &cam)
{
    if (!fullSize.isValid() || fullSize.width() <= 0 || fullSize.height() <= 0)
        return layer.renderFrame(fullSize, time, cam);

    const QSize proxySize = text3DPreviewProxySize(fullSize);
    if (!proxySize.isValid() || proxySize.width() <= 0 || proxySize.height() <= 0)
        return layer.renderFrame(fullSize, time, cam);

    const QImage proxy = layer.renderFrame(proxySize, time, cam);
    if (proxy.isNull() || proxy.size() == fullSize)
        return proxy;

    return proxy.scaled(fullSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QImage Text3DLayer::renderFrameQuads(QSize size, double time, const Camera3D &cam) const
{
    QImage frame(size, QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::transparent);

    if (!size.isValid() || size.width() <= 0 || size.height() <= 0)
        return frame;
    if (m_text.isEmpty() || m_cameraDistance <= 0.0)
        return frame;

    QFont font = m_font;
    if (font.family().isEmpty())
        font = QFont(QStringLiteral("Sans Serif"), 32);
    if (font.pointSizeF() <= 0.0 && font.pixelSize() <= 0)
        font.setPointSize(32);

    const Camera3DState cameraState = cameraStateAtTime(cam, time);
    const QMatrix4x4 view = buildViewMatrix(cameraState);
    const QMatrix4x4 projection = buildProjectionMatrix(cameraState, size);

    const QFontMetricsF metrics(font);
    const QStringList lines = m_text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    const double lineHeight = std::max(1.0, metrics.height());
    const double totalHeight = lineHeight * static_cast<double>(std::max<int>(1, static_cast<int>(lines.size())));

    QVector<GlyphQuad> glyphs;
    glyphs.reserve(m_text.size());

    int animatedCharacterIndex = 0;
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const QString &line = lines[lineIndex];
        const double lineWidth = metrics.horizontalAdvance(line);
        const double lineLeft = -lineWidth * 0.5;
        const double baselineY = -totalHeight * 0.5
            + static_cast<double>(lineIndex) * lineHeight
            + metrics.ascent();

        double cursorX = 0.0;
        for (int i = 0; i < line.size(); ++i, ++animatedCharacterIndex) {
            const QString glyphText = line.mid(i, 1);
            const QChar ch = glyphText.at(0);
            const QRectF tightBounds = metrics.tightBoundingRect(glyphText);
            const double advance = metrics.horizontalAdvance(glyphText);

            if (tightBounds.isEmpty() || ch.isSpace()) {
                cursorX += advance;
                continue;
            }

            const QImage glyphImage = rasterizeGlyph(glyphText, font, tightBounds);

            const double baseLeft = lineLeft + cursorX + tightBounds.left() - static_cast<double>(kGlyphPadding);
            const double baseTop = baselineY + tightBounds.top() - static_cast<double>(kGlyphPadding);
            const QVector3D baseCenter(
                static_cast<float>(baseLeft + glyphImage.width() * 0.5),
                static_cast<float>(baseTop + glyphImage.height() * 0.5),
                static_cast<float>(-m_cameraDistance));

            const double progress = characterProgress(animatedCharacterIndex, time);
            const float characterFactor = static_cast<float>(animatedCharacterIndex + 1);

            const QVector3D animatedPosition = m_perCharPosition * characterFactor * static_cast<float>(progress);
            const QVector3D scaleAmount = m_perCharScale * characterFactor * static_cast<float>(progress);
            const QVector3D scale(
                std::max(kMinScaleComponent, 1.0 + static_cast<double>(scaleAmount.x())),
                std::max(kMinScaleComponent, 1.0 + static_cast<double>(scaleAmount.y())),
                std::max(kMinScaleComponent, 1.0 + static_cast<double>(scaleAmount.z())));

            const QVector3D eulerRotation = m_perCharRotation * characterFactor * static_cast<float>(progress);
            QQuaternion rotation = QQuaternion::fromEulerAngles(
                eulerRotation.x(),
                eulerRotation.y(),
                eulerRotation.z());

            if (m_rotationAnimAxis.lengthSquared() > 1.0e-6f) {
                const QVector3D axis = m_rotationAnimAxis.normalized();
                const float tumbleAngle = static_cast<float>(progress * 180.0 * characterFactor);
                rotation = QQuaternion::fromAxisAndAngle(axis, tumbleAngle) * rotation;
            }

            const QVector3D center = baseCenter + animatedPosition;
            const QVector3D halfExtents(
                static_cast<float>(glyphImage.width()) * 0.5f,
                static_cast<float>(glyphImage.height()) * 0.5f,
                0.0f);

            const QVector<QVector3D> localCorners{
                QVector3D(-halfExtents.x(), -halfExtents.y(), 0.0f),
                QVector3D(halfExtents.x(), -halfExtents.y(), 0.0f),
                QVector3D(halfExtents.x(), halfExtents.y(), 0.0f),
                QVector3D(-halfExtents.x(), halfExtents.y(), 0.0f)
            };

            QPolygonF screenQuad;
            screenQuad.reserve(4);
            double depthSum = 0.0;
            bool valid = true;
            for (const QVector3D &localCorner : localCorners) {
                QVector3D scaledCorner(
                    localCorner.x() * scale.x(),
                    localCorner.y() * scale.y(),
                    localCorner.z() * scale.z());
                const QVector3D worldCorner = center + rotation.rotatedVector(scaledCorner);
                const ProjectedVertex projected = projectVertex(worldCorner, view, projection, size);
                if (!projected.valid) {
                    valid = false;
                    break;
                }

                screenQuad << projected.screen;
                depthSum += projected.depth;
            }

            if (valid && polygonArea(screenQuad) > 0.25) {
                GlyphQuad quad;
                quad.image = glyphImage;
                quad.screenQuad = screenQuad;
                quad.depth = depthSum / 4.0;
                glyphs.push_back(quad);
            }

            cursorX += advance;
        }
    }

    std::sort(glyphs.begin(), glyphs.end(),
              [](const GlyphQuad &lhs, const GlyphQuad &rhs) {
                  return lhs.depth > rhs.depth;
              });

    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    for (const GlyphQuad &glyph : glyphs) {
        const QPolygonF sourceQuad{
            QPointF(0.0, 0.0),
            QPointF(static_cast<double>(glyph.image.width()), 0.0),
            QPointF(static_cast<double>(glyph.image.width()), static_cast<double>(glyph.image.height())),
            QPointF(0.0, static_cast<double>(glyph.image.height()))
        };

        QTransform transform;
        if (!QTransform::quadToQuad(sourceQuad, glyph.screenQuad, transform))
            continue;

        painter.save();
        painter.setWorldTransform(transform, false);
        painter.drawImage(QPointF(0.0, 0.0), glyph.image);
        painter.restore();
    }

    painter.end();
    return frame;
}

void Text3DLayer::ensureExtrudedMesh() const
{
    QFont font = m_font;
    if (font.family().isEmpty())
        font = QFont(QStringLiteral("Sans Serif"), 32);
    if (font.pointSizeF() <= 0.0 && font.pixelSize() <= 0)
        font.setPointSize(32);

    const QString fontKey = font.toString();
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6")
                            .arg(m_text)
                            .arg(fontKey)
                            .arg(m_extrudeDepth, 0, 'g', 12)
                            .arg(m_extrudeBevelDepth, 0, 'g', 12)
                            .arg(m_extrudeBevelWidth, 0, 'g', 12)
                            .arg(m_extrudeBevelSegments);

    if (m_cacheValid && m_cacheKey == key)
        return;

    const QVector<QPolygonF> contours = mesh3d::glyphContours(m_text, font, kExtrudeFlatness);
    const mesh3d::ExtrudeParams params{
        m_extrudeDepth,
        m_extrudeBevelDepth,
        m_extrudeBevelWidth,
        m_extrudeBevelSegments,
        true
    };
    m_cachedMesh = mesh3d::buildExtrudedMesh(contours, params);
    m_cacheKey = key;
    m_cacheValid = true;
    ++m_meshBuildCount;
}

QImage Text3DLayer::renderFrameExtruded(QSize size, double time, const Camera3D &cam) const
{
    QSize outSize = size;
    if (!outSize.isValid() || outSize.width() <= 0)
        outSize.setWidth(1);
    if (outSize.height() <= 0)
        outSize.setHeight(1);

    if (m_text.isEmpty()) {
        QImage frame(outSize, QImage::Format_ARGB32);
        frame.fill(QColor(0, 0, 0, 0));
        return frame;
    }

    ensureExtrudedMesh();

    double yaw = m_extrudeBaseYaw
        + static_cast<double>(m_rotationAnimAxis.y()) * time * kExtrudeSpinDegPerSec;
    double pitch = m_extrudeBasePitch
        + static_cast<double>(m_rotationAnimAxis.x()) * time * kExtrudeSpinDegPerSec;

    softras::RenderParams rp;
    rp.lightDir = m_lightDir;
    rp.frontColor = m_frontColor;
    rp.sideColor = m_sideColor;
    rp.ambient = m_ambient;
    rp.background = QColor(0, 0, 0, 0);
    rp.wireframe = false;

    double camDist = m_cameraDistance;
    if (!std::isfinite(camDist) || camDist <= 0.0)
        camDist = 3.0;

    const Camera3D &activeCamera = cameraEqualsDefault(cam) ? m_camera : cam;
    foldCameraIntoExtrudeOrbit(activeCamera, time, yaw, pitch, camDist);
    if (!std::isfinite(camDist) || camDist <= 0.0)
        camDist = 3.0;

    return softras::renderMeshAuto(m_cachedMesh, outSize, yaw, pitch, camDist, rp);
}

QJsonObject Text3DLayer::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("text")] = m_text;
    obj[QStringLiteral("fontFamily")] = m_font.family();
    obj[QStringLiteral("fontPointSize")] = m_font.pointSizeF() > 0.0
        ? m_font.pointSizeF()
        : static_cast<double>(m_font.pointSize());
    obj[QStringLiteral("perCharRotation")] = vectorToJson(m_perCharRotation);
    obj[QStringLiteral("perCharPosition")] = vectorToJson(m_perCharPosition);
    obj[QStringLiteral("perCharScale")] = vectorToJson(m_perCharScale);
    obj[QStringLiteral("cameraDistance")] = m_cameraDistance;
    obj[QStringLiteral("rotationAnimAxis")] = vectorToJson(m_rotationAnimAxis);

    // --- Extrusion fields (US-3D-3) ---
    obj[QStringLiteral("extrudeEnabled")] = m_extrudeEnabled;
    obj[QStringLiteral("extrudeDepth")] = m_extrudeDepth;
    obj[QStringLiteral("extrudeBevelDepth")] = m_extrudeBevelDepth;
    obj[QStringLiteral("extrudeBevelWidth")] = m_extrudeBevelWidth;
    obj[QStringLiteral("extrudeBevelSegments")] = m_extrudeBevelSegments;
    obj[QStringLiteral("frontColor")] = m_frontColor.name(QColor::HexArgb);
    obj[QStringLiteral("sideColor")] = m_sideColor.name(QColor::HexArgb);
    obj[QStringLiteral("ambient")] = m_ambient.name(QColor::HexArgb);
    obj[QStringLiteral("lightDir")] = vectorToJson(m_lightDir);
    obj[QStringLiteral("extrudeBaseYaw")] = m_extrudeBaseYaw;
    obj[QStringLiteral("extrudeBasePitch")] = m_extrudeBasePitch;
    if (!cameraEqualsDefault(m_camera))
        obj[QStringLiteral("camera")] = m_camera.toJson();
    return obj;
}

void Text3DLayer::fromJson(const QJsonObject &obj)
{
    m_text = obj[QStringLiteral("text")].toString();

    QFont font = m_font;
    const QString family = obj[QStringLiteral("fontFamily")].toString();
    if (!family.isEmpty())
        font.setFamily(family);

    const double pointSize = obj[QStringLiteral("fontPointSize")].toDouble(font.pointSizeF());
    if (pointSize > 0.0)
        font.setPointSizeF(pointSize);
    else if (font.pointSizeF() <= 0.0 && font.pixelSize() <= 0)
        font.setPointSize(32);
    m_font = font;

    m_perCharRotation = vectorFromJson(
        obj[QStringLiteral("perCharRotation")].toObject(),
        m_perCharRotation);
    m_perCharPosition = vectorFromJson(
        obj[QStringLiteral("perCharPosition")].toObject(),
        m_perCharPosition);
    m_perCharScale = vectorFromJson(
        obj[QStringLiteral("perCharScale")].toObject(),
        m_perCharScale);
    m_cameraDistance = obj[QStringLiteral("cameraDistance")].toDouble(m_cameraDistance);
    m_camera = Camera3D{};
    if (obj.value(QStringLiteral("camera")).isObject())
        m_camera.fromJson(obj[QStringLiteral("camera")].toObject());
    m_rotationAnimAxis = vectorFromJson(
        obj[QStringLiteral("rotationAnimAxis")].toObject(),
        m_rotationAnimAxis);

    // --- Extrusion fields (US-3D-3); absent keys keep extrude OFF + defaults ---
    m_extrudeEnabled = obj[QStringLiteral("extrudeEnabled")].toBool(m_extrudeEnabled);
    m_extrudeDepth = obj[QStringLiteral("extrudeDepth")].toDouble(m_extrudeDepth);
    m_extrudeBevelDepth = obj[QStringLiteral("extrudeBevelDepth")].toDouble(m_extrudeBevelDepth);
    m_extrudeBevelWidth = obj[QStringLiteral("extrudeBevelWidth")].toDouble(m_extrudeBevelWidth);
    m_extrudeBevelSegments = obj[QStringLiteral("extrudeBevelSegments")].toInt(m_extrudeBevelSegments);

    const QString frontStr = obj[QStringLiteral("frontColor")].toString();
    if (!frontStr.isEmpty()) {
        const QColor c(frontStr);
        if (c.isValid())
            m_frontColor = c;
    }
    const QString sideStr = obj[QStringLiteral("sideColor")].toString();
    if (!sideStr.isEmpty()) {
        const QColor c(sideStr);
        if (c.isValid())
            m_sideColor = c;
    }
    const QString ambientStr = obj[QStringLiteral("ambient")].toString();
    if (!ambientStr.isEmpty()) {
        const QColor c(ambientStr);
        if (c.isValid())
            m_ambient = c;
    }
    if (obj.contains(QStringLiteral("lightDir")))
        m_lightDir = vectorFromJson(obj[QStringLiteral("lightDir")].toObject(), m_lightDir);

    m_extrudeBaseYaw = obj[QStringLiteral("extrudeBaseYaw")].toDouble(m_extrudeBaseYaw);
    m_extrudeBasePitch = obj[QStringLiteral("extrudeBasePitch")].toDouble(m_extrudeBasePitch);

    m_cacheValid = false;
}
