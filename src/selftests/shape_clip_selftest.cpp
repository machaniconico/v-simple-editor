#include "../ProjectFile.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoPlayer.h"
#include "../GLPreview.h"
#include "../UndoManager.h"
#include "../BrushAnimation.h"
#include "../ShapeModifierDialog.h"

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QRect>
#include <QTemporaryDir>
#include <QDoubleSpinBox>
#include <QLineF>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <QTransform>

#include <algorithm>
#include <cmath>

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

// Frozen BrushAnimation trim helpers from before US-110. These deliberately
// retain the legacy flattening/tolerance and never call ShapeLayer's new trim.
namespace legacyBrush {
constexpr double kLengthEpsilon = 1.0e-6;
QPointF lerpPoint(const QPointF &a, const QPointF &b, double t)
{
    return QPointF(a.x() + (b.x() - a.x()) * t,
                   a.y() + (b.y() - a.y()) * t);
}

double polygonLength(const QPolygonF &polygon)
{
    if (polygon.size() < 2) {
        return 0.0;
    }

    double length = 0.0;
    for (int i = 1; i < polygon.size(); ++i) {
        length += QLineF(polygon.at(i - 1), polygon.at(i)).length();
    }
    return length;
}

QPainterPath polygonToPath(const QPolygonF &polygon)
{
    QPainterPath path;
    if (polygon.isEmpty()) {
        return path;
    }

    path.moveTo(polygon.first());
    for (int i = 1; i < polygon.size(); ++i) {
        path.lineTo(polygon.at(i));
    }
    return path;
}

QPainterPath trimPolygon(const QPolygonF &polygon, double targetLength)
{
    QPainterPath path;
    if (polygon.size() < 2 || targetLength <= kLengthEpsilon) {
        return path;
    }

    path.moveTo(polygon.first());
    double remaining = targetLength;

    for (int i = 1; i < polygon.size(); ++i) {
        const QPointF start = polygon.at(i - 1);
        const QPointF end = polygon.at(i);
        const double segmentLength = QLineF(start, end).length();
        if (segmentLength <= kLengthEpsilon) {
            continue;
        }

        if (remaining >= segmentLength - kLengthEpsilon) {
            path.lineTo(end);
            remaining -= segmentLength;
            continue;
        }

        const double t = std::clamp(remaining / segmentLength, 0.0, 1.0);
        path.lineTo(lerpPoint(start, end, t));
        break;
    }

    return path;
}

double pathLength(const QPainterPath &path)
{
    double length = 0.0;
    const QList<QPolygonF> polygons = path.toSubpathPolygons();
    for (const QPolygonF &polygon : polygons) {
        length += polygonLength(polygon);
    }
    return length;
}

QPainterPath trimPath(const QPainterPath &path, double targetLength)
{
    QPainterPath trimmed;
    if (path.isEmpty() || targetLength <= kLengthEpsilon) {
        return trimmed;
    }

    double remaining = targetLength;
    const QList<QPolygonF> polygons = path.toSubpathPolygons();
    for (const QPolygonF &polygon : polygons) {
        const double subpathLength = polygonLength(polygon);
        if (subpathLength <= kLengthEpsilon) {
            continue;
        }

        if (remaining >= subpathLength - kLengthEpsilon) {
            trimmed.addPath(polygonToPath(polygon));
            remaining -= subpathLength;
            continue;
        }

        trimmed.addPath(trimPolygon(polygon, remaining));
        break;
    }

    return trimmed;
}

} // namespace legacyBrush

QImage legacyBrushFrame(const QPainterPath &fullPath, const QSize &size, double progress)
{
    const QPainterPath path = progress >= 1.0 ? fullPath
        : legacyBrush::trimPath(fullPath, legacyBrush::pathLength(fullPath) * progress);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainterPathStroker stroker;
    stroker.setWidth(8.0);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setMiterLimit(4.0);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawPath(stroker.createStroke(path));
    return image;
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

    Timeline playbackTimeline;
    playbackTimeline.videoTracks().first()->setClips(
        QVector<ClipInfo>{makeShapeClip(renderedRectangle)});
    playbackTimeline.refreshPlaybackSequence();
    int tickFrames = 0;
    QImage lastTickFrame;
    VideoPlayer player;
    player.setCanvasSize(60, 40);
    player.setProjectOutputSize(QSize(60, 40));
    player.glPreview()->setTimeline(&playbackTimeline);
    player.setSequence(playbackTimeline.computePlaybackSequence());

    QObject::connect(&player, &VideoPlayer::frameComposited,
                     [&tickFrames, &lastTickFrame](const QImage &image) {
        ++tickFrames;
        lastTickFrame = image;
    });
    const qint64 beforePlayback = player.timelinePositionUs();
    player.play();
    // Let the production playback timer deliver ticks, with a bounded wait.
    QElapsedTimer guard;
    guard.start();
    while (guard.elapsed() < 2000 && tickFrames < 2)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    player.pause();
    const QColor playbackCenter = lastTickFrame.isNull()
        ? QColor() : lastTickFrame.pixelColor(30, 20);
    const bool playbackRendered = player.timelinePositionUs() > beforePlayback
        && tickFrames >= 1
        && !lastTickFrame.isNull()
        && lastTickFrame.size() == QSize(60, 40)
        && playbackCenter.red() >= 220
        && playbackCenter.green() <= 45
        && playbackCenter.blue() >= 70;
    reportGate(5, rendered && playbackRendered,
               "Shape export or VideoPlayer tick did not render via SSOT",
               passed, failed);

    QPainterPath line;
    line.moveTo(0, 0);
    line.lineTo(200, 0);
    QPainterPath curve;
    curve.moveTo(0, 0);
    curve.cubicTo(0, 150, 200, -70, 220, 30);
    const QPainterPath half = ShapeLayer::trimPathRange(line, 0, 50, 0);
    const QPainterPath halfCurve = ShapeLayer::trimPathRange(curve, 0, 50, 0);
    QPainterPath separated;
    separated.moveTo(0, 0);
    separated.lineTo(100, 0);
    separated.moveTo(500, 0);
    separated.lineTo(600, 0);
    const QPainterPath middle = ShapeLayer::trimPathRange(separated, 25, 75, 0);
    reportGate(6, std::abs(half.length() / line.length() - 0.5) < 0.02
                   && std::abs(halfCurve.length() / curve.length() - 0.5) < 0.02
                   && std::abs(middle.length() - 100.0) < 0.01
                   && ShapeLayer::trimPathRange(curve, 0, 100, 75) == curve
                   && ShapeLayer::trimPathRange(curve, 20, 20, 0).isEmpty(),
               "Length-based trim lost half length, subpath gaps, or full/empty range",
               passed, failed);

    const QPainterPath wrapped = ShapeLayer::trimPathRange(line, 0, 50, 75);
    const QPainterPath negative = ShapeLayer::trimPathRange(line, 0, 50, -25);
    reportGate(7, wrapped == negative
                   && wrapped == ShapeLayer::trimPathRange(line, 0, 50, 175)
                   && wrapped == ShapeLayer::trimPathRange(line, 50, 0, 75)
                   && wrapped.elementCount() == 4
                   && std::abs(wrapped.elementAt(0).x - 150.0) < 0.01
                   && std::abs(wrapped.elementAt(3).x - 50.0) < 0.01
                   && std::abs(wrapped.length() - 100.0) < 0.01,
               "Offset did not wrap periodically without a connecting segment",
               passed, failed);

    Shape repeated = solidRectangle(QPointF(20, 20), QSizeF(10, 10), Qt::red);
    repeated.modifiers.repeater.enabled = true;
    const QSize repeatSize(140, 100);
    const QImage repeatImage = ShapeLayer::renderShapesToImage({repeated}, repeatSize);
    int opaquePixels = 0;
    for (int y = 0; y < repeatImage.height(); ++y)
        for (int x = 0; x < repeatImage.width(); ++x)
            if (repeatImage.pixelColor(x, y).alpha() > 200) ++opaquePixels;
    bool repeatOk = opaqueBounds(repeatImage) == QRect(15, 15, 90, 10)
        && opaquePixels == 300
        && repeatImage.pixelColor(20, 20).alpha() == 255
        && repeatImage.pixelColor(60, 20).alpha() == 255
        && repeatImage.pixelColor(100, 20).alpha() == 255
        && repeatImage.pixelColor(40, 20).alpha() == 0;
    repeated.modifiers.repeater.opacityEnd = 0.25;
    const QImage faded = ShapeLayer::renderShapesToImage({repeated}, repeatSize);
    repeatOk = repeatOk && faded.pixelColor(20, 20).alpha() == 255
        && std::abs(faded.pixelColor(60, 20).alpha() - 159) <= 2
        && std::abs(faded.pixelColor(100, 20).alpha() - 64) <= 2;

    // Verify repeated transform composition with distinguishable centers and sizes.
    repeated.modifiers.repeater.rotationDeg = 90;
    repeated.modifiers.repeater.scale = 0.5;
    const QImage transformed = ShapeLayer::renderShapesToImage({repeated}, repeatSize);
    repeatOk = repeatOk && transformed.pixelColor(20, 20).alpha() == 255
        && transformed.pixelColor(60, 20).alpha() > 150
        && transformed.pixelColor(60, 40).alpha() > 50
        && transformed.pixelColor(100, 20).alpha() == 0
        && transformed.pixelColor(64, 20).alpha() == 0;

    // Trim precedes repetition; verify the same result through the export SSOT.
    Shape trimmedLine;
    trimmedLine.type = ShapeType::Line;
    trimmedLine.position = QPointF(10, 10);
    trimmedLine.properties.endPoint = QPointF(20, 0);
    trimmedLine.stroke.width = 2;
    trimmedLine.stroke.cap = StrokeCap::Flat;
    trimmedLine.modifiers.trim.enabled = true;
    trimmedLine.modifiers.trim.endPct = 50;
    trimmedLine.modifiers.repeater.enabled = true;
    const QImage trimRepeat = ShapeLayer::renderShapesToImage({trimmedLine}, repeatSize);
    timeline.videoTracks().first()->setClips({makeShapeClip(trimmedLine)});
    timeline.refreshPlaybackSequence();
    const QImage trimExport = tlrender::renderFrameAt(&timeline, 500000, repeatSize);
    repeatOk = repeatOk && trimRepeat.pixelColor(15, 10).alpha() == 255
        && trimRepeat.pixelColor(25, 10).alpha() == 0
        && trimRepeat.pixelColor(55, 10).alpha() == 255
        && trimRepeat.pixelColor(95, 10).alpha() == 255
        && trimExport == trimRepeat;
    playbackTimeline.videoTracks().first()->setClips({makeShapeClip(trimmedLine)});
    playbackTimeline.refreshPlaybackSequence();
    VideoPlayer modifierPlayer;
    modifierPlayer.setCanvasSize(repeatSize.width(), repeatSize.height());
    modifierPlayer.setProjectOutputSize(repeatSize);
    modifierPlayer.glPreview()->setTimeline(&playbackTimeline);
    modifierPlayer.setSequence(playbackTimeline.computePlaybackSequence());
    tickFrames = 0;
    lastTickFrame = QImage();
    QObject::connect(&modifierPlayer, &VideoPlayer::frameComposited,
                     [&tickFrames, &lastTickFrame](const QImage &image) {
        ++tickFrames;
        lastTickFrame = image;
    });
    modifierPlayer.play();
    guard.restart();
    while (guard.elapsed() < 2000 && tickFrames < 2)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    modifierPlayer.pause();
    repeatOk = repeatOk && tickFrames >= 1 && lastTickFrame == trimExport;
    reportGate(8, repeatOk, "Repeater count/bounds/opacity/transform or trim/export order differs",
               passed, failed);

    Shape configured = fullyPopulatedShape(ShapeType::Star);
    configured.modifiers.repeater.enabled = true;
    configured.modifiers.repeater.copies = 7;
    configured.modifiers.repeater.offset = QPointF(-12.5, 33.25);
    configured.modifiers.repeater.rotationDeg = -25.5;
    configured.modifiers.repeater.scale = 0.875;
    configured.modifiers.repeater.opacityEnd = 0.375;
    configured.modifiers.trim.enabled = true;
    configured.modifiers.trim.startPct = 12.5;
    configured.modifiers.trim.endPct = 78.75;
    configured.modifiers.trim.offsetPct = -125.5;
    Shape disabledConfigured = configured;
    disabledConfigured.modifiers.repeater.enabled = false;
    disabledConfigured.modifiers.trim.enabled = false;
    const QString modifiersPath = temporary.filePath(QStringLiteral("modifiers.veditor"));
    ProjectData loadedModifiers;
    bool modifiersOk = ShapeModifiers().isDefault()
        && ShapeModifiers().toJson().isEmpty()
        && !Shape().toJson().contains(QStringLiteral("modifiers"))
        && Shape::fromJson(Shape().toJson()).modifiers.isDefault()
        && shapeBytes(Shape::fromJson(configured.toJson())) == shapeBytes(configured)
        && shapeBytes(Shape::fromJson(disabledConfigured.toJson())) == shapeBytes(disabledConfigured)
        && ProjectFile::save(modifiersPath, projectWithClip(makeShapeClip(configured)))
        && ProjectFile::load(modifiersPath, loadedModifiers)
        && loadedModifiers.videoTracks.size() == 1
        && loadedModifiers.videoTracks[0].size() == 1
        && loadedModifiers.videoTracks[0][0].shapes.size() == 1
        && shapeBytes(loadedModifiers.videoTracks[0][0].shapes[0]) == shapeBytes(configured);
    Timeline editTimeline;
    ClipInfo editClip = makeShapeClip(rectangle);
    editClip.shapes.append(renderedRectangle);
    editTimeline.videoTracks().first()->setClips({editClip});
    editTimeline.undoManager()->clear();
    editTimeline.undoManager()->saveState(editTimeline.currentState(), QStringLiteral("baseline"));
    const quint64 serial = editTimeline.undoManager()->saveSerial();
    ShapeModifierDialog modifierDialog(configured.modifiers);
    int changes = 0;
    QObject::connect(&modifierDialog, &ShapeModifierDialog::modifiersChanged, [&]() {
        ++changes;
        editTimeline.setClipShapeModifiers(0, 0, modifierDialog.modifiers(), false);
    });
    auto *endControl = modifierDialog.findChild<QDoubleSpinBox *>(QStringLiteral("endPct"));
    modifiersOk = modifiersOk && modifierDialog.modifiers().toJson() == configured.modifiers.toJson()
        && endControl;
    if (endControl) endControl->setValue(60.0);
    modifiersOk = modifiersOk && changes == 1
        && editTimeline.videoTracks()[0]->clips()[0].shapes[0].modifiers.trim.endPct == 60.0
        && editTimeline.undoManager()->saveSerial() == serial;
    editTimeline.setClipShapeModifiers(0, 0, rectangle.modifiers, false); // cancel
    modifiersOk = modifiersOk && editTimeline.videoTracks()[0]->clips()[0].shapes[0].modifiers.isDefault()
        && editTimeline.undoManager()->saveSerial() == serial;
    editTimeline.setClipShapeModifiers(0, 0, configured.modifiers, true);
    modifiersOk = modifiersOk && editTimeline.undoManager()->saveSerial() == serial + 1
        && shapeBytes(editTimeline.videoTracks()[0]->clips()[0].shapes[1]) == shapeBytes(renderedRectangle);
    editTimeline.undo();
    modifiersOk = modifiersOk && editTimeline.videoTracks()[0]->clips()[0].shapes[0].modifiers.isDefault()
        && !editTimeline.undoManager()->canUndo();
    editTimeline.redo();
    modifiersOk = modifiersOk
        && editTimeline.videoTracks()[0]->clips()[0].shapes[0].modifiers.toJson() == configured.modifiers.toJson();
    reportGate(9, modifiersOk, "Modifier JSON/default omission/project/UI preview/cancel/one undo failed",
               passed, failed);

    bool legacyPixels = true;
    for (int ordinal = 0; ordinal <= static_cast<int>(ShapeType::Bezier); ++ordinal) {
        Shape plain = fullyPopulatedShape(static_cast<ShapeType>(ordinal));
        plain.position = QPointF(90, 70);
        const QImage original = ShapeLayer::renderShapesToImage({plain}, QSize(200, 160));
        plain.modifiers = disabledConfigured.modifiers;
        legacyPixels = legacyPixels
            && original == ShapeLayer::renderShapesToImage({plain}, QSize(200, 160));
    }
    const QFont brushFont(QStringLiteral("Arial"), 32);
    const QPointF brushBase(10, 60);
    const QSize brushSize(160, 90);
    BrushAnimation brush;
    brush.setText(QStringLiteral("B"), brushFont, brushBase);
    QPainterPath glyph;
    glyph.addText(brushBase, brushFont, QStringLiteral("B"));
    legacyPixels = legacyPixels && brush.totalLength() > 0.0;
    for (BrushAnimationMode mode : {PerStroke, PerCharacter}) {
        brush.setMode(mode);
        for (double progress : {0.0, 0.125, 0.5, 0.875, 1.0}) {
            const double target = mode == PerCharacter
                ? progress * progress * (3.0 - 2.0 * progress) : progress;
            legacyPixels = legacyPixels
                && brush.renderFrame(brushSize, progress) == legacyBrushFrame(glyph, brushSize, target);
        }
    }
    reportGate(10, legacyPixels, "Disabled modifier pixels or legacy brush trim bytes changed",
               passed, failed);

    std::fprintf(stderr, "[shape-clip] summary: %d PASS, %d FAIL\n",
                 passed, failed);
    return failed;
}
