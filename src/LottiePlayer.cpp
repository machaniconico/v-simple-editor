#include "LottiePlayer.h"
#include "ExternalGenerator.h"
#include "Timeline.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QTransform>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace lottie {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct CachedDoc {
    qint64 mtimeMs = 0;
    LottieDocument doc;
};

QMutex g_cacheMutex;
QHash<QString, CachedDoc> g_docCache;

double evalCubicBezier(double u, double p1, double p2)
{
    const double inv = 1.0 - u;
    return 3.0 * inv * inv * u * p1 + 3.0 * inv * u * u * p2 + u * u * u;
}

double solveBezierEase(double t, double x1, double y1, double x2, double y2)
{
    t = qBound(0.0, t, 1.0);
    if (std::abs(x1 - y1) < 1e-4 && std::abs(x2 - y2) < 1e-4)
        return t;
    double lo = 0.0;
    double hi = 1.0;
    for (int iter = 0; iter < 14; ++iter) {
        const double mid = 0.5 * (lo + hi);
        const double x = evalCubicBezier(mid, x1, x2);
        if (x < t)
            lo = mid;
        else
            hi = mid;
    }
    return evalCubicBezier(0.5 * (lo + hi), y1, y2);
}

QVector<double> jsonToVec(const QJsonValue &v)
{
    QVector<double> out;
    if (v.isArray()) {
        const QJsonArray arr = v.toArray();
        out.reserve(arr.size());
        for (const QJsonValue &el : arr)
            out.append(el.toDouble(0.0));
    } else if (v.isDouble()) {
        out.append(v.toDouble(0.0));
    }
    return out;
}

double readTangentCoord(const QJsonValue &v, double fallback)
{
    if (v.isArray() && !v.toArray().isEmpty())
        return v.toArray().first().toDouble(fallback);
    if (v.isDouble())
        return v.toDouble(fallback);
    return fallback;
}

AnimatedProp parseAnimatedProp(const QJsonValue &val, const QVector<double> &fallback)
{
    AnimatedProp prop;
    prop.staticVal = fallback;
    if (!val.isObject())
        return prop;

    const QJsonObject obj = val.toObject();
    const bool isAnim = (obj.value(QStringLiteral("a")).toInt(0) == 1);
    const QJsonValue kVal = obj.value(QStringLiteral("k"));

    if (!isAnim) {
        const QVector<double> parsed = jsonToVec(kVal);
        if (!parsed.isEmpty())
            prop.staticVal = parsed;
        return prop;
    }

    if (!kVal.isArray())
        return prop;

    const QJsonArray kArr = kVal.toArray();
    for (int i = 0; i < kArr.size(); ++i) {
        if (!kArr[i].isObject())
            continue;
        const QJsonObject ko = kArr[i].toObject();
        Keyframe kf;
        kf.frame = ko.value(QStringLiteral("t")).toDouble(0.0);
        kf.startVal = jsonToVec(ko.value(QStringLiteral("s")));
        kf.endVal = jsonToVec(ko.value(QStringLiteral("e")));
        kf.hold = (ko.value(QStringLiteral("h")).toInt(0) == 1);

        if (ko.contains(QStringLiteral("o")) && ko.value(QStringLiteral("o")).isObject()) {
            const QJsonObject oo = ko.value(QStringLiteral("o")).toObject();
            kf.outTangent = QPointF(
                qBound(0.0, readTangentCoord(oo.value(QStringLiteral("x")), 0.33), 1.0),
                readTangentCoord(oo.value(QStringLiteral("y")), 0.33));
        }
        if (ko.contains(QStringLiteral("i")) && ko.value(QStringLiteral("i")).isObject()) {
            const QJsonObject io = ko.value(QStringLiteral("i")).toObject();
            kf.inTangent = QPointF(
                qBound(0.0, readTangentCoord(io.value(QStringLiteral("x")), 0.67), 1.0),
                readTangentCoord(io.value(QStringLiteral("y")), 0.67));
        }
        prop.keyframes.append(kf);
    }

    // Lottie v5 often omits "e" and uses the next keyframe's "s".
    for (int i = 0; i + 1 < prop.keyframes.size(); ++i) {
        if (prop.keyframes[i].endVal.isEmpty())
            prop.keyframes[i].endVal = prop.keyframes[i + 1].startVal;
    }
    if (!prop.keyframes.isEmpty()) {
        prop.animated = true;
        if (prop.keyframes.first().startVal.isEmpty())
            prop.keyframes.first().startVal = fallback;
    }
    return prop;
}

QVector<QPointF> parsePointArray(const QJsonArray &arr)
{
    QVector<QPointF> pts;
    pts.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        if (v.isArray()) {
            const QJsonArray pair = v.toArray();
            if (pair.size() >= 2)
                pts.append(QPointF(pair[0].toDouble(), pair[1].toDouble()));
        }
    }
    return pts;
}

BezierData parseBezier(const QJsonValue &ksVal)
{
    BezierData bd;
    if (!ksVal.isObject())
        return bd;
    const QJsonObject kso = ksVal.toObject();
    QJsonObject shapeObj = kso.value(QStringLiteral("k")).toObject();
    if (kso.value(QStringLiteral("a")).toInt(0) == 1 && kso.value(QStringLiteral("k")).isArray()) {
        const QJsonArray arr = kso.value(QStringLiteral("k")).toArray();
        if (!arr.isEmpty() && arr[0].isObject()) {
            const QJsonValue sVal = arr[0].toObject().value(QStringLiteral("s"));
            if (sVal.isArray() && !sVal.toArray().isEmpty())
                shapeObj = sVal.toArray()[0].toObject();
            else if (sVal.isObject())
                shapeObj = sVal.toObject();
        }
    }
    bd.vertices = parsePointArray(shapeObj.value(QStringLiteral("v")).toArray());
    bd.inTangents = parsePointArray(shapeObj.value(QStringLiteral("i")).toArray());
    bd.outTangents = parsePointArray(shapeObj.value(QStringLiteral("o")).toArray());
    bd.closed = shapeObj.value(QStringLiteral("c")).toBool(false);
    return bd;
}

ShapeItem parseShapeItem(const QJsonObject &obj);

QVector<ShapeItem> parseShapeArray(const QJsonArray &arr)
{
    QVector<ShapeItem> out;
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        if (v.isObject())
            out.append(parseShapeItem(v.toObject()));
    }
    return out;
}

ShapeItem parseShapeItem(const QJsonObject &obj)
{
    ShapeItem item;
    item.name = obj.value(QStringLiteral("nm")).toString();
    const QString ty = obj.value(QStringLiteral("ty")).toString();

    if (ty == QStringLiteral("gr")) {
        item.kind = ShapeItem::Kind::Group;
        item.children = parseShapeArray(obj.value(QStringLiteral("it")).toArray());
    } else if (ty == QStringLiteral("rc")) {
        item.kind = ShapeItem::Kind::Rect;
        item.position = parseAnimatedProp(obj.value(QStringLiteral("p")), {0.0, 0.0});
        item.size = parseAnimatedProp(obj.value(QStringLiteral("s")), {100.0, 100.0});
        item.roundness = parseAnimatedProp(obj.value(QStringLiteral("r")), {0.0});
    } else if (ty == QStringLiteral("el")) {
        item.kind = ShapeItem::Kind::Ellipse;
        item.position = parseAnimatedProp(obj.value(QStringLiteral("p")), {0.0, 0.0});
        item.size = parseAnimatedProp(obj.value(QStringLiteral("s")), {100.0, 100.0});
    } else if (ty == QStringLiteral("sr")) {
        item.kind = ShapeItem::Kind::Polystar;
        item.starType = obj.value(QStringLiteral("sy")).toInt(1);
        item.position = parseAnimatedProp(obj.value(QStringLiteral("p")), {0.0, 0.0});
        item.points = parseAnimatedProp(obj.value(QStringLiteral("pt")), {5.0});
        item.outerRadius = parseAnimatedProp(obj.value(QStringLiteral("or")), {50.0});
        item.innerRadius = parseAnimatedProp(obj.value(QStringLiteral("ir")), {25.0});
        item.rotation = parseAnimatedProp(obj.value(QStringLiteral("r")), {0.0});
    } else if (ty == QStringLiteral("sh")) {
        item.kind = ShapeItem::Kind::Path;
        item.path = parseBezier(obj.value(QStringLiteral("ks")));
    } else if (ty == QStringLiteral("fl")) {
        item.kind = ShapeItem::Kind::Fill;
        item.color = parseAnimatedProp(obj.value(QStringLiteral("c")), {1.0, 1.0, 1.0});
        item.opacity = parseAnimatedProp(obj.value(QStringLiteral("o")), {100.0});
    } else if (ty == QStringLiteral("st")) {
        item.kind = ShapeItem::Kind::Stroke;
        item.color = parseAnimatedProp(obj.value(QStringLiteral("c")), {1.0, 1.0, 1.0});
        item.opacity = parseAnimatedProp(obj.value(QStringLiteral("o")), {100.0});
        item.strokeWidth = parseAnimatedProp(obj.value(QStringLiteral("w")), {2.0});
        item.lineCap = obj.value(QStringLiteral("lc")).toInt(2);
    } else if (ty == QStringLiteral("tr")) {
        item.kind = ShapeItem::Kind::Transform;
        item.anchor = parseAnimatedProp(obj.value(QStringLiteral("a")), {0.0, 0.0});
        item.position = parseAnimatedProp(obj.value(QStringLiteral("p")), {0.0, 0.0});
        item.scale = parseAnimatedProp(obj.value(QStringLiteral("s")), {100.0, 100.0});
        item.rotation = parseAnimatedProp(obj.value(QStringLiteral("r")), {0.0});
        item.opacity = parseAnimatedProp(obj.value(QStringLiteral("o")), {100.0});
    }
    return item;
}

QColor colorFromProp(const AnimatedProp &colorProp, double frame, double opacityFrac)
{
    const QVector<double> c = colorProp.evaluate(frame);
    double r = c.size() > 0 ? c[0] : 1.0;
    double g = c.size() > 1 ? c[1] : 1.0;
    double b = c.size() > 2 ? c[2] : 1.0;
    // Some Lottie exporters write 0..255 instead of 0..1.
    if (r > 1.0 || g > 1.0 || b > 1.0) {
        r /= 255.0;
        g /= 255.0;
        b /= 255.0;
    }
    return QColor::fromRgbF(
        qBound(0.0, r, 1.0),
        qBound(0.0, g, 1.0),
        qBound(0.0, b, 1.0),
        qBound(0.0, opacityFrac, 1.0));
}

QTransform evalTransform(const AnimatedProp &anchorProp,
                         const AnimatedProp &posProp,
                         const AnimatedProp &scaleProp,
                         const AnimatedProp &rotProp,
                         double frame)
{
    const QVector<double> a = anchorProp.evaluate(frame);
    const QVector<double> p = posProp.evaluate(frame);
    const QVector<double> s = scaleProp.evaluate(frame);
    const double r = rotProp.evaluateScalar(frame, 0.0);

    const double ax = a.size() > 0 ? a[0] : 0.0;
    const double ay = a.size() > 1 ? a[1] : 0.0;
    const double px = p.size() > 0 ? p[0] : 0.0;
    const double py = p.size() > 1 ? p[1] : 0.0;
    const double sx = (s.size() > 0 ? s[0] : 100.0) * 0.01;
    const double sy = (s.size() > 1 ? s[1] : 100.0) * 0.01;

    QTransform xf;
    xf.translate(px, py);
    xf.rotate(r);
    xf.scale(sx, sy);
    xf.translate(-ax, -ay);
    return xf;
}

QTransform computeWorldTransform(const LottieDocument &doc, int layerIdx, double frame, int depth = 0)
{
    if (depth > 16)
        return {};
    const Layer *target = nullptr;
    for (const Layer &l : doc.layers) {
        if (l.index == layerIdx) {
            target = &l;
            break;
        }
    }
    if (!target)
        return {};
    const QTransform local = evalTransform(
        target->anchor, target->position, target->scale, target->rotation, frame);
    if (target->parentIndex >= 0 && target->parentIndex != target->index) {
        return local * computeWorldTransform(doc, target->parentIndex, frame, depth + 1);
    }
    return local;
}

QPainterPath buildPathForItem(const ShapeItem &item, double frame)
{
    QPainterPath path;
    if (item.kind == ShapeItem::Kind::Rect) {
        const QVector<double> p = item.position.evaluate(frame);
        const QVector<double> s = item.size.evaluate(frame);
        const double px = p.size() > 0 ? p[0] : 0.0;
        const double py = p.size() > 1 ? p[1] : 0.0;
        const double w = qMax(0.0, s.size() > 0 ? s[0] : 0.0);
        const double h = qMax(0.0, s.size() > 1 ? s[1] : 0.0);
        const double r = qBound(0.0, item.roundness.evaluateScalar(frame, 0.0), qMin(w, h) * 0.5);
        const QRectF rect(px - w * 0.5, py - h * 0.5, w, h);
        if (r > 0.01)
            path.addRoundedRect(rect, r, r);
        else
            path.addRect(rect);
    } else if (item.kind == ShapeItem::Kind::Ellipse) {
        const QVector<double> p = item.position.evaluate(frame);
        const QVector<double> s = item.size.evaluate(frame);
        const double px = p.size() > 0 ? p[0] : 0.0;
        const double py = p.size() > 1 ? p[1] : 0.0;
        const double w = qMax(0.0, s.size() > 0 ? s[0] : 0.0);
        const double h = qMax(0.0, s.size() > 1 ? s[1] : 0.0);
        path.addEllipse(QRectF(px - w * 0.5, py - h * 0.5, w, h));
    } else if (item.kind == ShapeItem::Kind::Polystar) {
        const QVector<double> p = item.position.evaluate(frame);
        const double cx = p.size() > 0 ? p[0] : 0.0;
        const double cy = p.size() > 1 ? p[1] : 0.0;
        const int numPts = qBound(3, static_cast<int>(std::lround(item.points.evaluateScalar(frame, 5.0))), 64);
        const double outerR = qMax(0.1, item.outerRadius.evaluateScalar(frame, 50.0));
        const double innerR = qMax(0.1, item.innerRadius.evaluateScalar(frame, outerR * 0.5));
        const double rotRad = (item.rotation.evaluateScalar(frame, 0.0) - 90.0) * kPi / 180.0;
        const bool isStar = (item.starType == 1);
        const int totalVerts = isStar ? numPts * 2 : numPts;
        for (int i = 0; i < totalVerts; ++i) {
            const double ang = rotRad + (2.0 * kPi * i) / totalVerts;
            const double rad = (isStar && (i % 2 == 1)) ? innerR : outerR;
            const QPointF pt(cx + std::cos(ang) * rad, cy + std::sin(ang) * rad);
            if (i == 0) path.moveTo(pt);
            else path.lineTo(pt);
        }
        path.closeSubpath();
    } else if (item.kind == ShapeItem::Kind::Path) {
        const auto &v = item.path.vertices;
        if (!v.isEmpty()) {
            path.moveTo(v[0]);
            const int n = v.size();
            const int segs = item.path.closed ? n : (n - 1);
            for (int i = 0; i < segs; ++i) {
                const int nxt = (i + 1) % n;
                const QPointF p0 = v[i];
                const QPointF p1 = v[nxt];
                const QPointF outT = (i < item.path.outTangents.size()) ? item.path.outTangents[i] : QPointF();
                const QPointF inT = (nxt < item.path.inTangents.size()) ? item.path.inTangents[nxt] : QPointF();
                if (outT.isNull() && inT.isNull())
                    path.lineTo(p1);
                else
                    path.cubicTo(p0 + outT, p1 + inT, p1);
            }
            if (item.path.closed)
                path.closeSubpath();
        }
    }
    return path;
}

void paintShapeList(QPainter &painter,
                    const QVector<ShapeItem> &items,
                    double frame,
                    double inheritedOpacity)
{
    // First apply any Transform item in this group
    double groupOpacity = inheritedOpacity;
    painter.save();
    for (const ShapeItem &it : items) {
        if (it.kind == ShapeItem::Kind::Transform) {
            const QTransform xf = evalTransform(it.anchor, it.position, it.scale, it.rotation, frame);
            painter.setTransform(xf, true);
            groupOpacity *= qBound(0.0, it.opacity.evaluateScalar(frame, 100.0) * 0.01, 1.0);
        }
    }

    // Collect geometry paths in this group, paint nested groups recursively
    QPainterPath combined;
    for (const ShapeItem &it : items) {
        if (it.kind == ShapeItem::Kind::Group) {
            paintShapeList(painter, it.children, frame, groupOpacity);
        } else if (it.kind == ShapeItem::Kind::Rect
                   || it.kind == ShapeItem::Kind::Ellipse
                   || it.kind == ShapeItem::Kind::Polystar
                   || it.kind == ShapeItem::Kind::Path) {
            combined.addPath(buildPathForItem(it, frame));
        }
    }

    // Apply Fill / Stroke in reverse Lottie order (bottom-up paint stack)
    if (!combined.isEmpty()) {
        for (int i = items.size() - 1; i >= 0; --i) {
            const ShapeItem &it = items[i];
            if (it.kind == ShapeItem::Kind::Fill) {
                const double op = groupOpacity * qBound(0.0, it.opacity.evaluateScalar(frame, 100.0) * 0.01, 1.0);
                painter.setPen(Qt::NoPen);
                painter.setBrush(colorFromProp(it.color, frame, op));
                painter.drawPath(combined);
            } else if (it.kind == ShapeItem::Kind::Stroke) {
                const double op = groupOpacity * qBound(0.0, it.opacity.evaluateScalar(frame, 100.0) * 0.01, 1.0);
                const double w = qMax(0.0, it.strokeWidth.evaluateScalar(frame, 2.0));
                if (w > 0.01) {
                    QPen pen(colorFromProp(it.color, frame, op), w);
                    pen.setCapStyle(it.lineCap == 1 ? Qt::FlatCap : (it.lineCap == 3 ? Qt::SquareCap : Qt::RoundCap));
                    pen.setJoinStyle(Qt::RoundJoin);
                    painter.setBrush(Qt::NoBrush);
                    painter.setPen(pen);
                    painter.drawPath(combined);
                }
            }
        }
    }

    painter.restore();
}

} // namespace

QVector<double> AnimatedProp::evaluate(double frame) const
{
    if (!animated || keyframes.isEmpty())
        return staticVal;
    if (frame <= keyframes.first().frame)
        return keyframes.first().startVal;
    if (frame >= keyframes.last().frame) {
        const auto &last = keyframes.last();
        return !last.startVal.isEmpty() ? last.startVal : last.endVal;
    }

    for (int i = 0; i + 1 < keyframes.size(); ++i) {
        const Keyframe &k0 = keyframes[i];
        const Keyframe &k1 = keyframes[i + 1];
        if (frame >= k0.frame && frame <= k1.frame) {
            if (k0.hold || k1.frame <= k0.frame)
                return k0.startVal;
            const double rawT = (frame - k0.frame) / (k1.frame - k0.frame);
            const double easeT = solveBezierEase(
                rawT, k0.outTangent.x(), k0.outTangent.y(), k0.inTangent.x(), k0.inTangent.y());
            const QVector<double> &a = k0.startVal;
            const QVector<double> &b = !k0.endVal.isEmpty() ? k0.endVal : k1.startVal;
            const int dims = qMin(a.size(), b.size());
            QVector<double> res(dims);
            for (int d = 0; d < dims; ++d)
                res[d] = a[d] + (b[d] - a[d]) * easeT;
            return res;
        }
    }
    return staticVal;
}

double AnimatedProp::evaluateScalar(double frame, double fallback) const
{
    const QVector<double> v = evaluate(frame);
    return v.isEmpty() ? fallback : v.first();
}

LottieDocument parseLottieJson(const QByteArray &jsonBytes, const QString &sourceLabel)
{
    LottieDocument doc;
    doc.sourcePath = sourceLabel;
    QJsonParseError perr;
    const QJsonDocument jdoc = QJsonDocument::fromJson(jsonBytes, &perr);
    if (perr.error != QJsonParseError::NoError || !jdoc.isObject()) {
        doc.error = QStringLiteral("Lottie JSON の解析に失敗しました: %1").arg(perr.errorString());
        return doc;
    }
    const QJsonObject root = jdoc.object();
    if (!root.contains(QStringLiteral("layers")) || !root.value(QStringLiteral("layers")).isArray()) {
        doc.error = QStringLiteral("Lottie JSON に layers 配列がありません。");
        return doc;
    }

    doc.name = root.value(QStringLiteral("nm")).toString(QFileInfo(sourceLabel).baseName());
    if (doc.name.isEmpty())
        doc.name = QStringLiteral("Lottie Animation");
    doc.fps = qBound(1.0, root.value(QStringLiteral("fr")).toDouble(30.0), 240.0);
    doc.inFrame = root.value(QStringLiteral("ip")).toDouble(0.0);
    doc.outFrame = root.value(QStringLiteral("op")).toDouble(doc.inFrame + doc.fps * 2.0);
    if (doc.outFrame <= doc.inFrame)
        doc.outFrame = doc.inFrame + doc.fps;
    doc.width = qMax(1.0, root.value(QStringLiteral("w")).toDouble(512.0));
    doc.height = qMax(1.0, root.value(QStringLiteral("h")).toDouble(512.0));

    const QJsonArray layerArr = root.value(QStringLiteral("layers")).toArray();
    for (int i = 0; i < layerArr.size(); ++i) {
        if (!layerArr[i].isObject())
            continue;
        const QJsonObject lo = layerArr[i].toObject();
        Layer layer;
        layer.index = lo.value(QStringLiteral("ind")).toInt(i + 1);
        layer.parentIndex = lo.value(QStringLiteral("parent")).toInt(-1);
        layer.type = lo.value(QStringLiteral("ty")).toInt(4);
        layer.name = lo.value(QStringLiteral("nm")).toString();
        layer.inFrame = lo.value(QStringLiteral("ip")).toDouble(doc.inFrame);
        layer.outFrame = lo.value(QStringLiteral("op")).toDouble(doc.outFrame);

        if (layer.type == 1) {
            layer.solidColor = QColor(lo.value(QStringLiteral("sc")).toString(QStringLiteral("#ffffff")));
            layer.solidWidth = lo.value(QStringLiteral("sw")).toDouble(doc.width);
            layer.solidHeight = lo.value(QStringLiteral("sh")).toDouble(doc.height);
        }

        const QJsonObject ks = lo.value(QStringLiteral("ks")).toObject();
        layer.anchor = parseAnimatedProp(ks.value(QStringLiteral("a")), {0.0, 0.0});
        layer.position = parseAnimatedProp(ks.value(QStringLiteral("p")), {0.0, 0.0});
        layer.scale = parseAnimatedProp(ks.value(QStringLiteral("s")), {100.0, 100.0});
        layer.rotation = parseAnimatedProp(ks.value(QStringLiteral("r")), {0.0});
        layer.opacity = parseAnimatedProp(ks.value(QStringLiteral("o")), {100.0});

        if (layer.type == 4 && lo.value(QStringLiteral("shapes")).isArray())
            layer.shapes = parseShapeArray(lo.value(QStringLiteral("shapes")).toArray());

        doc.layers.append(layer);
    }

    doc.valid = true;
    return doc;
}

LottieDocument loadLottieFile(const QString &filePath)
{
    const QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile()) {
        LottieDocument d;
        d.error = QStringLiteral("ファイルが見つかりません: %1").arg(filePath);
        return d;
    }
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    {
        QMutexLocker lock(&g_cacheMutex);
        const auto it = g_docCache.constFind(fi.absoluteFilePath());
        if (it != g_docCache.constEnd() && it.value().mtimeMs == mtime)
            return it.value().doc;
    }

    QFile f(fi.absoluteFilePath());
    if (!f.open(QIODevice::ReadOnly)) {
        LottieDocument d;
        d.error = QStringLiteral("ファイルを開けません: %1").arg(filePath);
        return d;
    }
    const LottieDocument parsed = parseLottieJson(f.readAll(), fi.absoluteFilePath());
    if (parsed.valid) {
        QMutexLocker lock(&g_cacheMutex);
        if (g_docCache.size() > 32)
            g_docCache.clear();
        g_docCache.insert(fi.absoluteFilePath(), {mtime, parsed});
    }
    return parsed;
}

QImage renderLottieFrame(const LottieDocument &doc, double timeSec, QSize outSize)
{
    const QSize sz = (outSize.isValid() && !outSize.isEmpty()) ? outSize : QSize(512, 512);
    QImage img(sz, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    if (!doc.valid)
        return img;

    const double spanFrames = qMax(1e-3, doc.outFrame - doc.inFrame);
    const double localFrame = doc.inFrame + std::fmod(qMax(0.0, timeSec) * doc.fps, spanFrames);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Aspect-preserving letterbox fit into outSize
    const double sx = static_cast<double>(sz.width()) / qMax(1.0, doc.width);
    const double sy = static_cast<double>(sz.height()) / qMax(1.0, doc.height);
    const double fitScale = qMin(sx, sy);
    const double ox = (static_cast<double>(sz.width()) - doc.width * fitScale) * 0.5;
    const double oy = (static_cast<double>(sz.height()) - doc.height * fitScale) * 0.5;

    QTransform baseXf;
    baseXf.translate(ox, oy);
    baseXf.scale(fitScale, fitScale);

    // Lottie renders layers from bottom (last in array) to top (index 0)
    for (int i = doc.layers.size() - 1; i >= 0; --i) {
        const Layer &layer = doc.layers[i];
        if (localFrame < layer.inFrame || localFrame >= layer.outFrame)
            continue;
        if (layer.type == 3) // Null layer
            continue;

        const double op = qBound(0.0, layer.opacity.evaluateScalar(localFrame, 100.0) * 0.01, 1.0);
        if (op <= 1e-4)
            continue;

        painter.save();
        const QTransform worldXf = computeWorldTransform(doc, layer.index, localFrame);
        painter.setTransform(worldXf * baseXf, false);

        if (layer.type == 1) {
            QColor col = layer.solidColor;
            col.setAlphaF(qBound(0.0, col.alphaF() * op, 1.0));
            painter.fillRect(QRectF(0.0, 0.0, layer.solidWidth, layer.solidHeight), col);
        } else if (layer.type == 4) {
            paintShapeList(painter, layer.shapes, localFrame, op);
        }
        painter.restore();
    }

    return img;
}

QImage renderLottieFileAtTime(const QString &filePath, double timeSec, QSize outSize)
{
    const LottieDocument doc = loadLottieFile(filePath);
    if (!doc.valid) {
        extgen::GeneratorSpec spec;
        spec.renderer = QStringLiteral("lottie");
        spec.entryPath = filePath;
        return extgen::renderMissingPlaceholder(spec, outSize, doc.error);
    }
    return renderLottieFrame(doc, timeSec, outSize);
}

QString buildLottieGeneratorUri(const QString &filePath, double fps)
{
    extgen::GeneratorSpec spec;
    spec.renderer = QStringLiteral("lottie");
    spec.entryPath = filePath;
    spec.fps = qBound(1.0, fps, 240.0);
    return extgen::buildGeneratorUri(spec);
}

bool insertLottieClip(Timeline *timeline,
                      const QString &filePath,
                      int videoTrackIdx,
                      QString *errorOut)
{
    if (!timeline) {
        if (errorOut) *errorOut = QStringLiteral("タイムラインが無効です。");
        return false;
    }
    const LottieDocument doc = loadLottieFile(filePath);
    if (!doc.valid) {
        if (errorOut) *errorOut = doc.error;
        return false;
    }
    const auto tracks = timeline->videoTracks();
    if (videoTrackIdx < 0 || videoTrackIdx >= tracks.size() || !tracks[videoTrackIdx]) {
        if (errorOut) *errorOut = QStringLiteral("挿入先ビデオトラックがありません。");
        return false;
    }
    TimelineTrack *tr = tracks[videoTrackIdx];
    if (tr->isLocked()) {
        if (errorOut) *errorOut = QStringLiteral("対象トラックがロックされています。");
        return false;
    }

    const double dur = qMax(0.5, doc.durationSec());
    ClipInfo clip;
    clip.filePath = buildLottieGeneratorUri(QFileInfo(filePath).absoluteFilePath(), doc.fps);
    clip.displayName = QStringLiteral("Lottie: %1").arg(doc.name);
    clip.duration = dur;
    clip.inPoint = 0.0;
    clip.outPoint = dur;
    tr->addClip(clip);
    timeline->saveUndoState(QStringLiteral("Lottie アニメーションを挿入 (%1)").arg(doc.name));
    return true;
}

void clearDocumentCache()
{
    QMutexLocker lock(&g_cacheMutex);
    g_docCache.clear();
}

// ============================================================================
// LottieImportDialog
// ============================================================================

LottieImportDialog::LottieImportDialog(Timeline *timeline,
                                       QWidget *parent,
                                       const QString &initialFilePath)
    : QDialog(parent)
    , m_timeline(timeline)
{
    setWindowTitle(QStringLiteral("Lottie ベクタアニメーションを挿入 (.json)"));
    resize(540, 440);

    auto *root = new QVBoxLayout(this);
    auto *pathRow = new QHBoxLayout();
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(QStringLiteral("Lottie JSON (.json) ファイルを選択…"));
    auto *browseBtn = new QPushButton(QStringLiteral("参照…"), this);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(browseBtn);
    root->addLayout(pathRow);

    m_infoLabel = new QLabel(QStringLiteral("Lottie JSON ファイルを選択してください。"), this);
    root->addWidget(m_infoLabel);

    m_previewLabel = new QLabel(this);
    m_previewLabel->setMinimumSize(320, 220);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setStyleSheet(QStringLiteral("background: #181a20; border: 1px solid #353945;"));
    root->addWidget(m_previewLabel, 1);

    m_scrubSlider = new QSlider(Qt::Horizontal, this);
    m_scrubSlider->setRange(0, 100);
    root->addWidget(m_scrubSlider);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    if (QPushButton *okBtn = buttons->button(QDialogButtonBox::Ok))
        okBtn->setText(QStringLiteral("タイムラインへ挿入"));
    root->addWidget(buttons);

    connect(browseBtn, &QPushButton::clicked, this, &LottieImportDialog::browseFile);
    connect(m_scrubSlider, &QSlider::valueChanged, this, &LottieImportDialog::updatePreview);
    connect(buttons, &QDialogButtonBox::accepted, this, &LottieImportDialog::onInsertAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (!initialFilePath.isEmpty())
        loadPath(initialFilePath);
}

QString LottieImportDialog::selectedFilePath() const
{
    return m_pathEdit ? m_pathEdit->text().trimmed() : QString();
}

void LottieImportDialog::browseFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Lottie JSON を選択"), QString(),
        QStringLiteral("Lottie JSON (*.json);;All Files (*)"));
    if (!path.isEmpty())
        loadPath(path);
}

void LottieImportDialog::loadPath(const QString &path)
{
    m_pathEdit->setText(path);
    m_doc = loadLottieFile(path);
    if (!m_doc.valid) {
        m_infoLabel->setText(m_doc.error);
        m_previewLabel->clear();
        return;
    }
    m_infoLabel->setText(QStringLiteral("%1 — %2x%3 @ %4 fps / 長さ %5s / レイヤー %6")
                             .arg(m_doc.name)
                             .arg(static_cast<int>(m_doc.width))
                             .arg(static_cast<int>(m_doc.height))
                             .arg(m_doc.fps, 0, 'f', 1)
                             .arg(m_doc.durationSec(), 0, 'f', 2)
                             .arg(m_doc.layers.size()));
    updatePreview();
}

void LottieImportDialog::updatePreview()
{
    if (!m_doc.valid)
        return;
    const double t = m_doc.durationSec() * (static_cast<double>(m_scrubSlider->value()) / 100.0);
    const QImage img = renderLottieFrame(m_doc, t, QSize(320, 220));
    m_previewLabel->setPixmap(QPixmap::fromImage(img));
}

void LottieImportDialog::onInsertAndAccept()
{
    if (!m_doc.valid) {
        QMessageBox::warning(this, QStringLiteral("Lottie 挿入"),
                             QStringLiteral("有効な Lottie JSON ファイルを選択してください。"));
        return;
    }
    QString err;
    if (!insertLottieClip(m_timeline, selectedFilePath(), 0, &err)) {
        QMessageBox::warning(this, QStringLiteral("Lottie 挿入失敗"), err);
        return;
    }
    accept();
}

} // namespace lottie
