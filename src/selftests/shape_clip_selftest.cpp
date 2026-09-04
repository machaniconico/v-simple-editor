#include "../ProjectFile.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"

#include <QColor>
#include <QFile>
#include <QJsonDocument>
#include <QRect>
#include <QTemporaryDir>

#include <cstdio>

namespace {

void reportGate(int gate, bool ok, const char *detail, int &passed, int &failed)
{
    if (ok) {
        ++passed;
        std::fprintf(stderr, "[shape-clip] PASS G%d\n", gate);
    } else {
        ++failed;
        std::fprintf(stderr, "[shape-clip] FAIL G%d: %s\n", gate, detail);
    }
}

QByteArray shapeBytes(const Shape &shape)
{
    return QJsonDocument(shape.toJson()).toJson(QJsonDocument::Compact);
}

Shape fullyPopulatedShape(ShapeType type)
{
    Shape shape;
    shape.type = type;
    shape.properties.size = QSizeF(123.5, 67.25);
    shape.properties.cornerRadius = 11.5;
    shape.properties.radius = 42.25;
    shape.properties.sides = 7;
    shape.properties.outerRadius = 51.5;
    shape.properties.innerRadius = 19.75;
    shape.properties.points = 9;
    shape.properties.startPoint = QPointF(-13.25, 7.5);
    shape.properties.endPoint = QPointF(88.75, -22.5);
    shape.properties.headSize = 17.25;
    shape.properties.controlPoints = {
        QPointF(1.25, 2.5), QPointF(10.75, 20.125),
        QPointF(30.5, -4.25), QPointF(44.0, 55.5)
    };
    shape.fill.color = QColor(12, 34, 56, 78);
    shape.fill.opacity = 0.625;
    shape.fill.enabled = false;
    shape.fill.gradient = true;
    shape.fill.gradientStart = QColor(90, 80, 70, 60);
    shape.fill.gradientEnd = QColor(50, 40, 30, 20);
    shape.fill.gradientAngle = 37.5;
    shape.stroke.color = QColor(101, 102, 103, 104);
    shape.stroke.width = 6.75;
    shape.stroke.opacity = 0.375;
    shape.stroke.enabled = true;
    shape.stroke.dashPattern = {2.5, 4.75, 1.25};
    shape.stroke.cap = StrokeCap::Square;
    shape.stroke.join = StrokeJoin::Bevel;
    shape.position = QPointF(321.25, 123.75);
    shape.rotation = -27.5;
    shape.scale = 1.875;
    shape.name = QStringLiteral("全フィールド");
    return shape;
}

Shape solidRectangle(const QPointF &position, const QSizeF &size,
                     const QColor &color)
{
    Shape shape;
    shape.type = ShapeType::Rectangle;
    shape.properties.size = size;
    shape.position = position;
    shape.fill.color = color;
    shape.fill.opacity = 1.0;
    shape.fill.enabled = true;
    shape.fill.gradient = false;
    shape.stroke.enabled = false;
    return shape;
}

QRect opaqueBounds(const QImage &image, int minimumAlpha = 200)
{
    QRect bounds;
    bool found = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() < minimumAlpha)
                continue;
            const QRect pixel(x, y, 1, 1);
            bounds = found ? bounds.united(pixel) : pixel;
            found = true;
        }
    }
    return bounds;
}

ProjectData projectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = {{clip}};
    data.audioTracks = {};
    return data;
}

ClipInfo makeShapeClip(const Shape &shape)
{
    ClipInfo clip;
    clip.displayName = QStringLiteral("シェイプクリップ");
    clip.duration = 5.0;
    clip.inPoint = 0.0;
    clip.outPoint = 5.0;
    clip.shapes = {shape};
    return clip;
}

} // namespace

int runShapeClipSelftest()
{
    int passed = 0;
    int failed = 0;

    bool allTypesRoundTrip = true;
    for (int ordinal = static_cast<int>(ShapeType::Rectangle);
         ordinal <= static_cast<int>(ShapeType::Bezier); ++ordinal) {
        const Shape original = fullyPopulatedShape(
            static_cast<ShapeType>(ordinal));
        const Shape restored = Shape::fromJson(original.toJson());
        if (shapeBytes(restored) != shapeBytes(original)) {
            allTypesRoundTrip = false;
            break;
        }
    }
    reportGate(1, allTypesRoundTrip,
               "Shape JSON round-trip lost a field or shape type",
               passed, failed);

    const Shape rectangle = solidRectangle(
        QPointF(32.0, 24.0), QSizeF(20.0, 10.0), QColor(17, 91, 203, 255));
    const QImage direct = ShapeLayer::renderShapesToImage(
        QVector<Shape>{rectangle}, QSize(64, 48));
    const bool directOk = !direct.isNull()
        && direct.size() == QSize(64, 48)
        && opaqueBounds(direct) == QRect(22, 19, 20, 10)
        && direct.pixelColor(32, 24) == QColor(17, 91, 203, 255)
        && direct.pixelColor(0, 0).alpha() == 0;
    reportGate(2, directOk,
               "renderShapesToImage rectangle pixels/bounds differ",
               passed, failed);

    const ClipInfo persistedClip = makeShapeClip(
        fullyPopulatedShape(ShapeType::Bezier));
    const ProjectData persistedProject = projectWithClip(persistedClip);
    QTemporaryDir temporary;
    ProjectData loadedProject;
    const QString projectPath = temporary.filePath(
        QStringLiteral("shape_clip.veditor"));
    const bool persisted = temporary.isValid()
        && ProjectFile::save(projectPath, persistedProject)
        && ProjectFile::load(projectPath, loadedProject)
        && loadedProject.videoTracks.size() == 1
        && loadedProject.videoTracks.first().size() == 1
        && loadedProject.videoTracks.first().first().filePath.isEmpty()
        && loadedProject.videoTracks.first().first().shapes.size() == 1
        && shapeBytes(loadedProject.videoTracks.first().first().shapes.first())
            == shapeBytes(persistedClip.shapes.first());
    reportGate(3, persisted,
               "project save/load did not preserve shape clip fields",
               passed, failed);

    ClipInfo legacyClip;
    legacyClip.filePath = QStringLiteral("legacy-source.mp4");
    legacyClip.displayName = QStringLiteral("legacy");
    legacyClip.duration = 3.0;
    legacyClip.outPoint = 3.0;
    const QString legacyJson = ProjectFile::toJsonString(
        projectWithClip(legacyClip));
    ProjectData loadedLegacy;
    const bool legacyLoaded = ProjectFile::fromJsonString(
        legacyJson, loadedLegacy);
    const QString legacyResaved = legacyLoaded
        ? ProjectFile::toJsonString(loadedLegacy) : QString();
    const bool legacyIdentical = legacyLoaded
        && !legacyJson.contains(QStringLiteral("\"shapes\""))
        && legacyJson.toUtf8() == legacyResaved.toUtf8();
    reportGate(4, legacyIdentical,
               "empty shapes changed legacy project JSON bytes",
               passed, failed);

    Timeline timeline;
    const Shape renderedRectangle = solidRectangle(
        QPointF(30.0, 20.0), QSizeF(16.0, 8.0), QColor(225, 41, 73, 255));
    timeline.videoTracks().first()->setClips(
        QVector<ClipInfo>{makeShapeClip(renderedRectangle)});
    timeline.refreshPlaybackSequence();
    const QImage frame = tlrender::renderFrameAt(
        &timeline, 500000, QSize(60, 40));
    const QColor renderedCenter = frame.isNull()
        ? QColor() : frame.pixelColor(30, 20);
    const bool rendered = !frame.isNull()
        && frame.size() == QSize(60, 40)
        && opaqueBounds(frame) == QRect(22, 16, 16, 8)
        && renderedCenter.red() >= 220
        && renderedCenter.green() <= 45
        && renderedCenter.blue() >= 70
        && frame.pixelColor(0, 0).alpha() == 0;
    reportGate(5, rendered,
               "renderFrameAt did not composite the media-less shape clip",
               passed, failed);

    std::fprintf(stderr, "[shape-clip] summary: %d PASS, %d FAIL\n",
                 passed, failed);
    return failed;
}
