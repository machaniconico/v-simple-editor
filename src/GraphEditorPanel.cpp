#include "GraphEditorPanel.h"
#include "Timeline.h"
#include "VideoEffect.h"
#include "EffectParamSchema.h"
#include "TransformAnimator.h"
#include "clipanim/ClipAnim.h"

#include <QAbstractItemView>
#include <QAction>
#include <QBrush>
#include <QComboBox>
#include <QFontMetrics>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QMenu>
#include <QToolButton>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

QVector<GraphEditorCurveTrack> graphEditorEditableCurves(
    const QVector<GraphEditorCurveTrack> &curves)
{
    QVector<GraphEditorCurveTrack> editable;
    for (const auto &curve : curves) {
        if (!curve.isPastePlaceholder)
            editable.append(curve);
    }
    return editable;
}

namespace {

bool parseEffectTrackName(const QString &trackName, int *effectIndex, QString *paramName)
{
    static const QString prefix = QStringLiteral("effect.");
    if (!trackName.startsWith(prefix))
        return false;

    const int indexStart = prefix.size();
    const int dotPos = trackName.indexOf(QLatin1Char('.'), indexStart);
    if (dotPos <= indexStart)
        return false;

    bool ok = false;
    const int parsedIndex = trackName.mid(indexStart, dotPos - indexStart).toInt(&ok);
    if (!ok || parsedIndex < 0)
        return false;

    if (effectIndex)
        *effectIndex = parsedIndex;
    if (paramName)
        *paramName = trackName.mid(dotPos + 1);
    return true;
}

bool splitColorChannelParam(const QString &paramName, QString *baseParamName, QString *channel)
{
    const int dotPos = paramName.lastIndexOf(QLatin1Char('.'));
    if (dotPos <= 0 || dotPos == paramName.size() - 1)
        return false;

    const QString suffix = paramName.mid(dotPos + 1);
    if (suffix != QStringLiteral("r")
        && suffix != QStringLiteral("g")
        && suffix != QStringLiteral("b")) {
        return false;
    }

    if (baseParamName)
        *baseParamName = paramName.left(dotPos);
    if (channel)
        *channel = suffix;
    return true;
}

// Resolve the destination's static value, as the existing keyframe-creation
// controls do. Merely offering a destination must not mutate the clip.
bool pasteTrackDefault(const ClipInfo &clip, const QString &property, double &value)
{
    const QHash<QString, double> values = {
        {QStringLiteral("motion.position.x"), clip.videoDx},
        {QStringLiteral("motion.position.y"), clip.videoDy},
        {QStringLiteral("motion.scale"), clip.videoScale},
        {QStringLiteral("motion.rotation"), clip.rotation2DDegrees},
        {QStringLiteral("motion.opacity"), clip.opacity},
        {QStringLiteral("motion.position.z"), clip.layer3D.positionZ},
        {QStringLiteral("motion.rotation.x"), clip.layer3D.rotationX},
        {QStringLiteral("motion.rotation.y"), clip.layer3D.rotationY},
        {QStringLiteral("motion.rotation.z"), clip.layer3D.rotationZ},
        {QStringLiteral("grade.brightness"), clip.colorCorrection.brightness},
        {QStringLiteral("grade.contrast"), clip.colorCorrection.contrast},
        {QStringLiteral("grade.saturation"), clip.colorCorrection.saturation},
        {QStringLiteral("grade.exposure"), clip.colorCorrection.exposure},
        {QStringLiteral("grade.temperature"), clip.colorCorrection.temperature},
        {QStringLiteral("grade.hue"), clip.colorCorrection.hue},
        {QStringLiteral("grade.gamma"), clip.colorCorrection.gamma},
        {QStringLiteral("grade.highlights"), clip.colorCorrection.highlights},
        {QStringLiteral("grade.shadows"), clip.colorCorrection.shadows},
        {QStringLiteral("brush_progress"), 0.0}
    };
    const auto it = values.constFind(property);
    if (it != values.cend()) {
        value = it.value();
        return true;
    }
    for (bool log : {false, true}) {
        for (const auto &field : clipanim::sectionGradeTracks(log)) {
            if (property == field.name) {
                value = clip.colorCorrection.*(field.member);
                return true;
            }
        }
    }
    for (const auto &field : clipanim::hslGradeTracks()) {
        if (property == field.name) {
            value = clip.hslSecondary.*(field.member);
            return true;
        }
    }
    for (const auto &field : clipanim::warpGradeTracks()) {
        if (property == field.name) {
            value = field.shift
                ? clip.colorCorrection.hueSatWarp.hueShiftDeg[field.ring][field.hue]
                : clip.colorCorrection.hueSatWarp.satScale[field.ring][field.hue];
            return true;
        }
    }
    const auto transformProperty = TransformAnimator::propertyFromName(property);
    // propertyFromName falls back to PositionX for unknown names.
    if (TransformAnimator::propertyName(transformProperty) == property) {
        value = TransformAnimator::propertyDefaultValue(transformProperty);
        return true;
    }
    int effectIndex = -1;
    QString param;
    if (!parseEffectTrackName(property, &effectIndex, &param)
        || effectIndex >= clip.effects.size())
        return false;
    const auto &effect = clip.effects[effectIndex];
    QString base, channel;
    const bool colorChannel = splitColorChannelParam(param, &base, &channel);
    for (const auto &def : effectctrl::paramSchemaFor(effect.type)) {
        if (colorChannel && def.name == base && def.type == effectctrl::ParamType::Color) {
            QColor color = effectctrl::colorParamValue(effect, base);
            if (!color.isValid())
                color = QColor(0, 0, 0);
            value = channel == QStringLiteral("r") ? color.red()
                : channel == QStringLiteral("g") ? color.green() : color.blue();
            return true;
        }
        if (def.name == param && def.type != effectctrl::ParamType::Color) {
            value = effectctrl::paramValue(effect, param);
            return true;
        }
    }
    return false;
}

QString motionTrackLabel(const QString &propertyName)
{
    static const QHash<QString, QString> labels = [] {
        QHash<QString, QString> map;
        map.insert(QStringLiteral("motion.position.x"), QStringLiteral("Motion: Position X"));
        map.insert(QStringLiteral("motion.position.y"), QStringLiteral("Motion: Position Y"));
        map.insert(QStringLiteral("motion.scale"), QStringLiteral("Motion: Scale"));
        map.insert(QStringLiteral("motion.rotation"), QStringLiteral("Motion: Rotation 2D"));
        map.insert(QStringLiteral("motion.opacity"), QStringLiteral("Motion: Opacity"));
        map.insert(QStringLiteral("motion.position.z"), QStringLiteral("Motion: Position Z"));
        map.insert(QStringLiteral("motion.rotation.x"), QStringLiteral("Motion: Rotation X"));
        map.insert(QStringLiteral("motion.rotation.y"), QStringLiteral("Motion: Rotation Y"));
        map.insert(QStringLiteral("motion.rotation.z"), QStringLiteral("Motion: Rotation Z"));
        return map;
    }();
    return labels.value(propertyName, propertyName);
}

QString displayNameForTrack(const KeyframeTrack &track, const QVector<VideoEffect> &effects)
{
    int effectIndex = -1;
    QString paramName;
    if (parseEffectTrackName(track.propertyName(), &effectIndex, &paramName)) {
        const QString effectName = (effectIndex >= 0 && effectIndex < effects.size())
            ? VideoEffect::typeName(effects[effectIndex].type)
            : QStringLiteral("Effect %1").arg(effectIndex + 1);
        QString baseParamName;
        QString channel;
        if (splitColorChannelParam(paramName, &baseParamName, &channel)) {
            return QStringLiteral("%1: %2 %3")
                .arg(effectName, baseParamName, channel.toUpper());
        }
        return QStringLiteral("%1: %2").arg(effectName, paramName);
    }
    return motionTrackLabel(track.propertyName());
}

double clipTimelineStartSeconds(const Timeline *timeline, int trackIdx, int clipIdx)
{
    if (!timeline || trackIdx < 0 || trackIdx >= timeline->videoTracks().size())
        return 0.0;

    const auto *track = timeline->videoTracks().value(trackIdx, nullptr);
    if (!track)
        return 0.0;

    const auto &clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return 0.0;

    double cursor = 0.0;
    for (int i = 0; i < clipIdx; ++i) {
        cursor += qMax(0.0, clips[i].leadInSec);
        cursor += qMax(0.0, clips[i].effectiveDuration());
    }
    return cursor + qMax(0.0, clips[clipIdx].leadInSec);
}

QColor curveColor(int index)
{
    static const QVector<QColor> palette = {
        QColor(QStringLiteral("#7dd3fc")),
        QColor(QStringLiteral("#fda4af")),
        QColor(QStringLiteral("#86efac")),
        QColor(QStringLiteral("#fde047")),
        QColor(QStringLiteral("#c4b5fd")),
        QColor(QStringLiteral("#fb923c")),
        QColor(QStringLiteral("#67e8f9")),
        QColor(QStringLiteral("#f0abfc")),
    };
    return palette[index % palette.size()];
}

QColor curveColorForTrack(const QString &propertyName, int index)
{
    int effectIndex = -1;
    QString paramName;
    QString baseParamName;
    QString channel;
    if (parseEffectTrackName(propertyName, &effectIndex, &paramName)
        && splitColorChannelParam(paramName, &baseParamName, &channel)) {
        if (channel == QStringLiteral("r"))
            return QColor(QStringLiteral("#ef4444"));
        if (channel == QStringLiteral("g"))
            return QColor(QStringLiteral("#22c55e"));
        if (channel == QStringLiteral("b"))
            return QColor(QStringLiteral("#3b82f6"));
    }
    return curveColor(index);
}

double curveDurationFor(const QVector<GraphEditorCurveTrack> &curves, double clipDuration)
{
    double duration = qMax(clipDuration, 0.0);
    for (const auto &curve : curves) {
        for (const auto &kf : curve.track.keyframes())
            duration = qMax(duration, kf.time);
    }
    return qMax(1.0, duration);
}

QString interpolationLabel(KeyframePoint::Interpolation interpolation)
{
    switch (interpolation) {
    case KeyframePoint::Linear: return QStringLiteral("Linear");
    case KeyframePoint::EaseIn: return QStringLiteral("Ease In");
    case KeyframePoint::EaseOut: return QStringLiteral("Ease Out");
    case KeyframePoint::EaseInOut: return QStringLiteral("Ease In/Out");
    case KeyframePoint::Hold: return QStringLiteral("Hold");
    case KeyframePoint::Bezier: return QStringLiteral("Bezier");
    case KeyframePoint::ElasticOut: return QStringLiteral("Elastic Out");
    case KeyframePoint::BounceOut: return QStringLiteral("Bounce Out");
    case KeyframePoint::BackOut: return QStringLiteral("Back Out");
    }
    return QStringLiteral("Keyframe");
}

constexpr int kGraphLeft = 118;
constexpr int kGraphRight = 18;
constexpr int kGraphTop = 28;
constexpr int kLaneHeight = 78;
constexpr int kLaneGap = 16;
constexpr double kMinKeyframeGapSeconds = 0.001;

double niceGridStep(double rawStep)
{
    if (!std::isfinite(rawStep) || rawStep <= 0.0)
        return 1.0;

    const double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    const double fraction = rawStep / magnitude;
    if (fraction <= 1.0)
        return magnitude;
    if (fraction <= 2.0)
        return 2.0 * magnitude;
    if (fraction <= 5.0)
        return 5.0 * magnitude;
    return 10.0 * magnitude;
}

double snapToStep(double value, double step)
{
    if (!std::isfinite(value) || !std::isfinite(step) || step <= 0.0)
        return value;
    return std::round(value / step) * step;
}

bool nearlyEqual(double a, double b)
{
    return qAbs(a - b) <= 1e-9;
}

bool sameEditableKeyframe(const KeyframePoint &a, const KeyframePoint &b)
{
    return nearlyEqual(a.time, b.time)
        && nearlyEqual(a.value, b.value)
        && a.interpolation == b.interpolation
        && nearlyEqual(a.bezX1, b.bezX1)
        && nearlyEqual(a.bezY1, b.bezY1)
        && nearlyEqual(a.bezX2, b.bezX2)
        && nearlyEqual(a.bezY2, b.bezY2)
        && a.hasSpatialTangent == b.hasSpatialTangent
        && nearlyEqual(a.spatialOutX, b.spatialOutX)
        && nearlyEqual(a.spatialOutY, b.spatialOutY)
        && nearlyEqual(a.spatialInX, b.spatialInX)
        && nearlyEqual(a.spatialInY, b.spatialInY);
}

bool sameTrackKeyframes(const KeyframeTrack &a, const KeyframeTrack &b)
{
    const auto &ak = a.keyframes();
    const auto &bk = b.keyframes();
    if (ak.size() != bk.size())
        return false;
    for (int i = 0; i < ak.size(); ++i) {
        if (!sameEditableKeyframe(ak[i], bk[i]))
            return false;
    }
    return true;
}

} // namespace

class GraphEditorCurveView : public QWidget
{
public:
    using EditCallback = std::function<void(const QVector<GraphEditorCurveTrack> &curves, bool commitUndo)>;
    enum class GraphMode {
        Value,
        Speed,
    };

    explicit GraphEditorCurveView(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumWidth(420);
        setMouseTracking(true);
    }

    void setEditCallback(EditCallback callback)
    {
        m_editCallback = std::move(callback);
    }

    void setGraphMode(GraphMode mode)
    {
        if (m_graphMode == mode)
            return;
        m_graphMode = mode;
        clearDragState();
        update();
    }

    GraphEditorPanel *panel() const
    {
        for (QWidget *widget = parentWidget(); widget; widget = widget->parentWidget()) {
            if (auto *owner = qobject_cast<GraphEditorPanel *>(widget))
                return owner;
        }
        return nullptr;
    }

    bool isCopySelected(int curve, int key) const
    {
        const auto *owner = panel();
        return owner && owner->m_keyframeSelectionProperty == m_curves[curve].propertyName
            && owner->m_selectedKeyframeIndices.contains(key);
    }

    bool hasSelectedKeyframe() const
    {
        return m_selectedCurveIndex >= 0
            && m_selectedCurveIndex < m_curves.size()
            && !m_curves[m_selectedCurveIndex].isPastePlaceholder
            && m_selectedKeyframeIndex >= 0
            && m_selectedKeyframeIndex
                < m_curves[m_selectedCurveIndex].track.keyframes().size();
    }

    bool applyInterpolationToSelected(KeyframePoint::Interpolation interpolation)
    {
        if (!hasSelectedKeyframe())
            return false;

        auto &curve = m_curves[m_selectedCurveIndex];
        const QVector<KeyframePoint> &keyframes = curve.track.keyframes();
        KeyframePoint edited = keyframes[m_selectedKeyframeIndex];
        if (edited.interpolation == interpolation) {
            update();
            return true;
        }

        edited.interpolation = interpolation;
        KeyframeTrack editedTrack = curve.track;
        editedTrack.setKeyframePoint(m_selectedKeyframeIndex, edited);
        curve.track = editedTrack;
        if (m_editCallback)
            m_editCallback(m_curves, true);
        update();
        return true;
    }

    void setCurves(const QVector<GraphEditorCurveTrack> &curves,
                   double clipDurationSeconds,
                   double localPlayheadSeconds)
    {
        m_curves = curves;
        m_durationSeconds = curveDurationFor(m_curves, clipDurationSeconds);
        m_localPlayheadSeconds = localPlayheadSeconds;
        if (m_selectedCurveIndex < 0 || m_selectedCurveIndex >= m_curves.size()
            || m_selectedKeyframeIndex < 0
            || m_selectedKeyframeIndex >= m_curves[m_selectedCurveIndex].track.keyframes().size()) {
            m_selectedCurveIndex = -1;
            m_selectedKeyframeIndex = -1;
        }
        setMinimumHeight(qMax(180, 38 + static_cast<int>(m_curves.size()) * 94));
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        return {720, qMax(220, 38 + static_cast<int>(m_curves.size()) * 94)};
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), palette().color(QPalette::Base));

        if (m_curves.isEmpty()) {
            painter.setPen(palette().color(QPalette::Mid));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No keyframe tracks"));
            return;
        }

        const int graphWidth = qMax(1, width() - kGraphLeft - kGraphRight);
        const double duration = qMax(1.0, m_durationSeconds);

        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.drawText(QRect(kGraphLeft, 4, graphWidth, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("0.00s"));
        painter.drawText(QRect(kGraphLeft, 4, graphWidth, 18),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1s").arg(duration, 0, 'f', 2));
        painter.drawText(QRect(kGraphLeft, 4, graphWidth, 18),
                         Qt::AlignHCenter | Qt::AlignVCenter,
                         m_graphMode == GraphMode::Speed
                             ? QStringLiteral("Speed Graph")
                             : QStringLiteral("Value Graph"));

        const int playheadX = kGraphLeft + qRound(qBound(0.0, m_localPlayheadSeconds, duration)
                                           / duration * graphWidth);

        for (int i = 0; i < m_curves.size(); ++i) {
            const auto &curve = m_curves[i];
            const LaneMetrics metrics = laneMetricsForCurve(i);
            if (!metrics.valid)
                continue;
            const QRect laneRect = metrics.laneRect;
            const int y = laneRect.top();

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 12));
            painter.drawRoundedRect(laneRect, 3, 3);

            painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
            for (int grid = 1; grid < 4; ++grid) {
                const int gx = laneRect.left() + grid * laneRect.width() / 4;
                painter.drawLine(gx, laneRect.top(), gx, laneRect.bottom());
            }
            painter.drawLine(laneRect.left(), laneRect.center().y(),
                             laneRect.right(), laneRect.center().y());

            QPainterPath path;
            for (int x = 0; x <= graphWidth; ++x) {
                const double t = duration * static_cast<double>(x) / graphWidth;
                const QPointF point(laneRect.left() + x,
                                    yForValue(metrics, graphValueAt(curve.track, t)));
                if (x == 0)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            painter.setPen(QPen(curve.color, 2.0));
            painter.drawPath(path);

            const auto &keyframes = curve.track.keyframes();
            painter.setBrush(curve.color.lighter(130));
            if (m_graphMode == GraphMode::Value) {
                for (int k = 0; k + 1 < keyframes.size(); ++k) {
                    if (keyframes[k].interpolation != KeyframePoint::Bezier)
                        continue;

                    QPointF outHandle;
                    QPointF inHandle;
                    if (!bezierHandlePoint(metrics, keyframes, k, true, &outHandle)
                        || !bezierHandlePoint(metrics, keyframes, k, false, &inHandle)) {
                        continue;
                    }

                    const QPointF start = keyframePoint(metrics, keyframes[k]);
                    const QPointF end = keyframePoint(metrics, keyframes[k + 1]);
                    const bool selectedSegment = i == m_selectedCurveIndex && k == m_selectedKeyframeIndex;
                    const bool outSelected = selectedSegment;
                    const bool inSelected = selectedSegment
                        || (i == m_selectedCurveIndex && k + 1 == m_selectedKeyframeIndex);
                    outHandle = visibleBezierHandlePoint(metrics, keyframes, k, true, outSelected);
                    inHandle = visibleBezierHandlePoint(metrics, keyframes, k, false, inSelected);

                    painter.setPen(QPen(outSelected ? palette().color(QPalette::HighlightedText)
                                                    : curve.color.lighter(135),
                                        outSelected ? 1.8 : 1.0));
                    painter.drawLine(start, outHandle);
                    painter.setPen(QPen(inSelected ? palette().color(QPalette::HighlightedText)
                                                   : curve.color.lighter(135),
                                        inSelected ? 1.8 : 1.0));
                    painter.drawLine(end, inHandle);

                    auto drawHandle = [&](const QPointF &point, bool selected) {
                        painter.setBrush(selected ? palette().color(QPalette::Highlight)
                                                  : curve.color.lighter(130));
                        painter.setPen(QPen(palette().color(QPalette::Window), selected ? 1.4 : 1.0));
                        painter.drawEllipse(point, selected ? 5.0 : 4.0, selected ? 5.0 : 4.0);
                    };
                    drawHandle(outHandle, outSelected);
                    drawHandle(inHandle, inSelected);
                }
            }

            painter.setBrush(curve.color);
            painter.setPen(QPen(palette().color(QPalette::Window), 1));
            for (int k = 0; k < keyframes.size(); ++k) {
                const auto &kf = keyframes[k];
                const QPointF center = graphKeyframePoint(metrics, curve.track, k);
                const int kx = qRound(center.x());
                const int ky = qRound(center.y());
                QPolygonF diamond;
                diamond << QPointF(kx, ky - 5)
                        << QPointF(kx + 5, ky)
                        << QPointF(kx, ky + 5)
                        << QPointF(kx - 5, ky);
                painter.drawPolygon(diamond);

                if (m_graphMode == GraphMode::Value
                    && kf.interpolation == KeyframePoint::Bezier) {
                    painter.setPen(QPen(curve.color.lighter(135), 1));
                    painter.drawEllipse(QPointF(kx, ky), 7, 7);
                    painter.setPen(QPen(palette().color(QPalette::Window), 1));
                }

                if (isCopySelected(i, k) || (i == m_selectedCurveIndex && k == m_selectedKeyframeIndex)) {
                    painter.setBrush(Qt::NoBrush);
                    painter.setPen(QPen(palette().color(QPalette::HighlightedText), 1.3));
                    painter.drawEllipse(QPointF(kx, ky), 9, 9);
                    painter.setBrush(curve.color);
                    painter.setPen(QPen(palette().color(QPalette::Window), 1));
                }
            }

            painter.setPen(palette().color(QPalette::Text));
            const QString elided = QFontMetrics(painter.font())
                .elidedText(curve.displayName, Qt::ElideRight, kGraphLeft - 18);
            painter.drawText(QRect(8, y, kGraphLeft - 18, 20),
                             Qt::AlignLeft | Qt::AlignVCenter, elided);

            painter.setPen(palette().color(QPalette::Mid));
            painter.drawText(QRect(8, y + 24, kGraphLeft - 18, 18),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1 kf").arg(curve.track.count()));
            painter.drawText(QRect(8, y + 44, kGraphLeft - 18, 18),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             graphRangeLabel(metrics));
        }

        painter.setPen(QPen(QColor(QStringLiteral("#ff4d4f")), 1.5));
        painter.drawLine(playheadX, kGraphTop - 10, playheadX,
                         kGraphTop + m_curves.size() * (kLaneHeight + kLaneGap) - kLaneGap + 8);
        painter.setBrush(QColor(QStringLiteral("#ff4d4f")));
        painter.setPen(Qt::NoPen);
        QPolygonF playheadMarker;
        playheadMarker << QPointF(playheadX - 5, kGraphTop - 12)
                       << QPointF(playheadX + 5, kGraphTop - 12)
                       << QPointF(playheadX, kGraphTop - 4);
        painter.drawPolygon(playheadMarker);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }

        const bool extend = event->modifiers().testFlag(Qt::ControlModifier)
            || event->modifiers().testFlag(Qt::ShiftModifier);
        const HitResult hit = hitTest(event->position().toPoint(), extend);
        if (hit.kind == DragKind::None) {
            QWidget::mousePressEvent(event);
            return;
        }

        if (hit.kind == DragKind::Keyframe) {
            if (auto *owner = panel()) {
                const QString property = m_curves[hit.curveIndex].propertyName;
                QVector<int> indices;
                if (extend && owner->m_keyframeSelectionProperty == property)
                    indices = owner->m_selectedKeyframeIndices;
                if (indices.contains(hit.keyframeIndex))
                    indices.removeOne(hit.keyframeIndex);
                else
                    indices.append(hit.keyframeIndex);
                owner->selectKeyframes(property, indices);
                if (extend) {
                    m_selectedCurveIndex = -1;
                    m_selectedKeyframeIndex = -1;
                    event->accept();
                    update();
                    return;
                }
            }
        }

        if (m_graphMode == GraphMode::Speed) {
            if (hit.kind == DragKind::Keyframe) {
                m_selectedCurveIndex = hit.curveIndex;
                m_selectedKeyframeIndex = hit.keyframeIndex;
                event->accept();
                update();
                return;
            }
            QWidget::mousePressEvent(event);
            return;
        }

        m_dragKind = hit.kind;
        m_dragCurveIndex = hit.curveIndex;
        m_dragKeyframeIndex = hit.keyframeIndex;
        m_selectedCurveIndex = hit.curveIndex;
        m_selectedKeyframeIndex = hit.keyframeIndex;
        m_dragStartTrack = m_curves[m_dragCurveIndex].track;
        m_dragMetrics = laneMetricsForCurve(m_dragCurveIndex);
        m_dragChanged = false;
        grabMouse();
        setCursor(hit.kind == DragKind::Keyframe ? Qt::SizeAllCursor : Qt::CrossCursor);
        event->accept();
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPoint pos = event->position().toPoint();
        if (m_dragKind != DragKind::None) {
            applyDrag(pos, event->modifiers());
            event->accept();
            return;
        }

        const HitResult hit = hitTest(pos);
        if (hit.kind == DragKind::Keyframe)
            setCursor(m_graphMode == GraphMode::Speed ? Qt::PointingHandCursor : Qt::SizeAllCursor);
        else if (hit.kind == DragKind::BezierOut || hit.kind == DragKind::BezierIn)
            setCursor(Qt::CrossCursor);
        else
            setCursor(Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_dragKind == DragKind::None) {
            QWidget::mouseReleaseEvent(event);
            return;
        }

        if (event->button() == Qt::LeftButton) {
            if (m_dragChanged && m_editCallback)
                m_editCallback(m_curves, true);
            clearDragState();
            event->accept();
            update();
            return;
        }

        QWidget::mouseReleaseEvent(event);
    }

private:
    enum class DragKind {
        None,
        Keyframe,
        BezierOut,
        BezierIn,
    };

    struct LaneMetrics {
        bool valid = false;
        QRect laneRect;
        double duration = 1.0;
        double minValue = 0.0;
        double maxValue = 1.0;
        double valueSpan = 1.0;
    };

    struct HitResult {
        DragKind kind = DragKind::None;
        int curveIndex = -1;
        int keyframeIndex = -1;
    };

    LaneMetrics laneMetricsForCurve(int curveIndex) const
    {
        LaneMetrics metrics;
        if (curveIndex < 0 || curveIndex >= m_curves.size())
            return metrics;

        const auto &curve = m_curves[curveIndex];
        const int graphWidth = qMax(1, width() - kGraphLeft - kGraphRight);
        const int y = kGraphTop + curveIndex * (kLaneHeight + kLaneGap);
        metrics.valid = true;
        metrics.laneRect = QRect(kGraphLeft, y, graphWidth, kLaneHeight);
        metrics.duration = qMax(1.0, m_durationSeconds);
        metrics.minValue = graphValueAt(curve.track, 0.0);
        metrics.maxValue = metrics.minValue;

        auto includeValue = [&metrics](double value) {
            metrics.minValue = qMin(metrics.minValue, value);
            metrics.maxValue = qMax(metrics.maxValue, value);
        };
        if (m_graphMode == GraphMode::Speed)
            includeValue(0.0);

        const int sampleCount = qMax(24, qMin(240, graphWidth));
        for (int s = 0; s <= sampleCount; ++s) {
            const double t = metrics.duration * static_cast<double>(s) / sampleCount;
            includeValue(graphValueAt(curve.track, t));
        }

        const auto &keyframes = curve.track.keyframes();
        for (int i = 0; i < keyframes.size(); ++i)
            includeValue(graphKeyframeValue(curve.track, i));
        if (m_graphMode == GraphMode::Value) {
            for (int i = 0; i + 1 < keyframes.size(); ++i) {
                if (keyframes[i].interpolation != KeyframePoint::Bezier)
                    continue;
                const double deltaValue = keyframes[i + 1].value - keyframes[i].value;
                includeValue(keyframes[i].value + keyframes[i].bezY1 * deltaValue);
                includeValue(keyframes[i].value + keyframes[i].bezY2 * deltaValue);
            }
        }

        if (qFuzzyCompare(metrics.minValue, metrics.maxValue)) {
            metrics.minValue -= 1.0;
            metrics.maxValue += 1.0;
        }
        metrics.valueSpan = qMax(0.000001, metrics.maxValue - metrics.minValue);
        return metrics;
    }

    double timeForX(const LaneMetrics &metrics, int x) const
    {
        const double normalized = static_cast<double>(x - metrics.laneRect.left())
            / qMax(1, metrics.laneRect.width());
        return normalized * metrics.duration;
    }

    double valueForY(const LaneMetrics &metrics, int y) const
    {
        const double normalized = static_cast<double>(metrics.laneRect.bottom() - y)
            / qMax(1, metrics.laneRect.height());
        return metrics.minValue + normalized * metrics.valueSpan;
    }

    int xForTime(const LaneMetrics &metrics, double time) const
    {
        return metrics.laneRect.left()
            + qRound(qBound(0.0, time, metrics.duration) / metrics.duration * metrics.laneRect.width());
    }

    int yForValue(const LaneMetrics &metrics, double value) const
    {
        const double normalized = (value - metrics.minValue) / metrics.valueSpan;
        return metrics.laneRect.bottom() - qRound(normalized * metrics.laneRect.height());
    }

    QPointF keyframePoint(const LaneMetrics &metrics, const KeyframePoint &keyframe) const
    {
        return QPointF(xForTime(metrics, keyframe.time), yForValue(metrics, keyframe.value));
    }

    QString graphRangeLabel(const LaneMetrics &metrics) const
    {
        const QString suffix = m_graphMode == GraphMode::Speed
            ? QStringLiteral("/s")
            : QString();
        return QStringLiteral("%1..%2%3")
            .arg(metrics.minValue, 0, 'g', 4)
            .arg(metrics.maxValue, 0, 'g', 4)
            .arg(suffix);
    }

    int segmentIndexForTime(const QVector<KeyframePoint> &keyframes, double time) const
    {
        if (keyframes.size() < 2)
            return -1;
        if (time < keyframes.first().time || time > keyframes.last().time)
            return -1;

        for (int i = 0; i + 1 < keyframes.size(); ++i) {
            if (time >= keyframes[i].time && time <= keyframes[i + 1].time)
                return i;
        }
        return -1;
    }

    double speedAtTime(const KeyframeTrack &track, double time) const
    {
        const QVector<KeyframePoint> &keyframes = track.keyframes();
        const int segmentIndex = segmentIndexForTime(keyframes, time);
        if (segmentIndex < 0)
            return 0.0;

        const KeyframePoint &from = keyframes[segmentIndex];
        const KeyframePoint &to = keyframes[segmentIndex + 1];
        const double segmentDuration = to.time - from.time;
        if (!std::isfinite(segmentDuration) || segmentDuration <= 1e-9
            || from.interpolation == KeyframePoint::Hold) {
            return 0.0;
        }

        const double step = qMax(0.00001, segmentDuration / 1000.0);
        double lo = qMax(from.time, time - step);
        double hi = qMin(to.time, time + step);
        if (hi - lo <= 1e-9) {
            if (time <= from.time + 1e-9) {
                lo = from.time;
                hi = qMin(to.time, from.time + step);
            } else {
                lo = qMax(from.time, to.time - step);
                hi = to.time;
            }
        }
        if (hi - lo <= 1e-9)
            return 0.0;
        return (track.valueAt(hi) - track.valueAt(lo)) / (hi - lo);
    }

    double graphValueAt(const KeyframeTrack &track, double time) const
    {
        return m_graphMode == GraphMode::Speed
            ? speedAtTime(track, time)
            : track.valueAt(time);
    }

    double graphKeyframeValue(const KeyframeTrack &track, int keyframeIndex) const
    {
        const QVector<KeyframePoint> &keyframes = track.keyframes();
        if (keyframeIndex < 0 || keyframeIndex >= keyframes.size())
            return 0.0;
        return m_graphMode == GraphMode::Speed
            ? speedAtTime(track, keyframes[keyframeIndex].time)
            : keyframes[keyframeIndex].value;
    }

    QPointF graphKeyframePoint(const LaneMetrics &metrics,
                               const KeyframeTrack &track,
                               int keyframeIndex) const
    {
        const QVector<KeyframePoint> &keyframes = track.keyframes();
        if (keyframeIndex < 0 || keyframeIndex >= keyframes.size())
            return {};
        return QPointF(xForTime(metrics, keyframes[keyframeIndex].time),
                       yForValue(metrics, graphKeyframeValue(track, keyframeIndex)));
    }

    bool bezierHandlePoint(const LaneMetrics &metrics,
                           const QVector<KeyframePoint> &keyframes,
                           int keyframeIndex,
                           bool outgoing,
                           QPointF *point) const
    {
        if (keyframeIndex < 0 || keyframeIndex + 1 >= keyframes.size())
            return false;

        const KeyframePoint &from = keyframes[keyframeIndex];
        const KeyframePoint &to = keyframes[keyframeIndex + 1];
        const double deltaTime = to.time - from.time;
        if (!std::isfinite(deltaTime) || deltaTime <= 1e-9)
            return false;

        const double bezX = outgoing ? from.bezX1 : from.bezX2;
        const double bezY = outgoing ? from.bezY1 : from.bezY2;
        const double handleTime = from.time + bezX * deltaTime;
        const double handleValue = from.value + bezY * (to.value - from.value);
        if (point)
            *point = QPointF(xForTime(metrics, handleTime), yForValue(metrics, handleValue));
        return true;
    }

    QPointF bezierHandleAnchorPoint(const LaneMetrics &metrics,
                                    const QVector<KeyframePoint> &keyframes,
                                    int keyframeIndex,
                                    bool outgoing) const
    {
        return keyframePoint(metrics, keyframes[outgoing ? keyframeIndex : keyframeIndex + 1]);
    }

    QPointF visibleBezierHandlePoint(const LaneMetrics &metrics,
                                     const QVector<KeyframePoint> &keyframes,
                                     int keyframeIndex,
                                     bool outgoing,
                                     bool selected) const
    {
        QPointF point;
        if (!bezierHandlePoint(metrics, keyframes, keyframeIndex, outgoing, &point))
            return point;

        if (!selected)
            return point;

        const QPointF anchor = bezierHandleAnchorPoint(metrics, keyframes, keyframeIndex, outgoing);
        if (std::hypot(point.x() - anchor.x(), point.y() - anchor.y()) > 0.5)
            return point;

        QPointF offset(outgoing ? 14.0 : -14.0, -14.0);
        QPointF visible = anchor + offset;
        const QRectF innerLane = QRectF(metrics.laneRect).adjusted(5.0, 5.0, -5.0, -5.0);
        if (!innerLane.contains(visible)) {
            offset.setY(14.0);
            visible = anchor + offset;
        }
        if (!innerLane.contains(visible)) {
            visible.setX(qBound(innerLane.left(), visible.x(), innerLane.right()));
            visible.setY(qBound(innerLane.top(), visible.y(), innerLane.bottom()));
        }
        return visible;
    }

    bool pointHitsHandle(const QPoint &pos, const QPointF &handlePoint, double radius) const
    {
        return std::hypot(pos.x() - handlePoint.x(), pos.y() - handlePoint.y()) <= radius;
    }

    bool pointHitsSelectedHandle(const QPoint &pos,
                                 const LaneMetrics &metrics,
                                 const QVector<KeyframePoint> &keyframes,
                                 int keyframeIndex,
                                 bool outgoing,
                                 double radius) const
    {
        QPointF handlePoint;
        if (!bezierHandlePoint(metrics, keyframes, keyframeIndex, outgoing, &handlePoint))
            return false;

        if (pointHitsHandle(pos, handlePoint, radius))
            return true;

        const QPointF visiblePoint = visibleBezierHandlePoint(metrics, keyframes, keyframeIndex, outgoing, true);
        return pointHitsHandle(pos, visiblePoint, radius);
    }

    HitResult selectedBezierHandleHit(const QPoint &pos) const
    {
        if (m_graphMode != GraphMode::Value)
            return {};

        constexpr double kHandleRadius = 8.0;
        if (m_selectedCurveIndex < 0 || m_selectedCurveIndex >= m_curves.size()
            || m_curves[m_selectedCurveIndex].isPastePlaceholder
            || m_selectedKeyframeIndex < 0) {
            return {};
        }

        const LaneMetrics metrics = laneMetricsForCurve(m_selectedCurveIndex);
        if (!metrics.valid)
            return {};

        const auto &keyframes = m_curves[m_selectedCurveIndex].track.keyframes();
        if (m_selectedKeyframeIndex >= keyframes.size())
            return {};

        if (m_selectedKeyframeIndex + 1 < keyframes.size()
            && keyframes[m_selectedKeyframeIndex].interpolation == KeyframePoint::Bezier
            && pointHitsSelectedHandle(pos, metrics, keyframes, m_selectedKeyframeIndex,
                                       true, kHandleRadius)) {
            return {DragKind::BezierOut, m_selectedCurveIndex, m_selectedKeyframeIndex};
        }

        if (m_selectedKeyframeIndex + 1 < keyframes.size()
            && keyframes[m_selectedKeyframeIndex].interpolation == KeyframePoint::Bezier
            && pointHitsSelectedHandle(pos, metrics, keyframes, m_selectedKeyframeIndex,
                                       false, kHandleRadius)) {
            return {DragKind::BezierIn, m_selectedCurveIndex, m_selectedKeyframeIndex};
        }

        if (m_selectedKeyframeIndex > 0
            && keyframes[m_selectedKeyframeIndex - 1].interpolation == KeyframePoint::Bezier
            && pointHitsSelectedHandle(pos, metrics, keyframes, m_selectedKeyframeIndex - 1,
                                       false, kHandleRadius)) {
            return {DragKind::BezierIn, m_selectedCurveIndex, m_selectedKeyframeIndex - 1};
        }

        return {};
    }

    HitResult hitTest(const QPoint &pos, bool preferKeyframes = false) const
    {
        constexpr double kHandleRadius = 8.0;
        constexpr double kKeyframeRadius = 8.0;

        // Ctrl/Shift selection must reach keys underneath a selected handle.
        // Unmodified clicks retain the existing handle-first drag behavior.
        if (!preferKeyframes) {
            const HitResult selectedHandleHit = selectedBezierHandleHit(pos);
            if (selectedHandleHit.kind != DragKind::None)
                return selectedHandleHit;
        }

        for (int curveIndex = 0; curveIndex < m_curves.size(); ++curveIndex) {
            if (m_curves[curveIndex].isPastePlaceholder)
                continue;
            const LaneMetrics metrics = laneMetricsForCurve(curveIndex);
            const auto &keyframes = m_curves[curveIndex].track.keyframes();
            for (int keyframeIndex = 0; keyframeIndex < keyframes.size(); ++keyframeIndex) {
                const QPointF center = graphKeyframePoint(metrics, m_curves[curveIndex].track, keyframeIndex);
                if (pointHitsHandle(pos, center, kKeyframeRadius))
                    return {DragKind::Keyframe, curveIndex, keyframeIndex};
            }
        }

        if (m_graphMode != GraphMode::Value)
            return {};

        for (int curveIndex = 0; curveIndex < m_curves.size(); ++curveIndex) {
            if (m_curves[curveIndex].isPastePlaceholder)
                continue;
            const LaneMetrics metrics = laneMetricsForCurve(curveIndex);
            const auto &keyframes = m_curves[curveIndex].track.keyframes();
            for (int keyframeIndex = 0; keyframeIndex + 1 < keyframes.size(); ++keyframeIndex) {
                if (keyframes[keyframeIndex].interpolation != KeyframePoint::Bezier)
                    continue;

                QPointF handlePoint;
                if (bezierHandlePoint(metrics, keyframes, keyframeIndex, true, &handlePoint)
                    && pointHitsHandle(pos, handlePoint, kHandleRadius)) {
                    return {DragKind::BezierOut, curveIndex, keyframeIndex};
                }
                if (bezierHandlePoint(metrics, keyframes, keyframeIndex, false, &handlePoint)
                    && pointHitsHandle(pos, handlePoint, kHandleRadius)) {
                    return {DragKind::BezierIn, curveIndex, keyframeIndex};
                }
            }
        }

        return {};
    }

    void applyDrag(const QPoint &pos, Qt::KeyboardModifiers modifiers)
    {
        if (m_graphMode != GraphMode::Value)
            return;

        if (m_dragCurveIndex < 0 || m_dragCurveIndex >= m_curves.size()
            || m_curves[m_dragCurveIndex].isPastePlaceholder
            || !m_dragMetrics.valid) {
            return;
        }

        const auto &startKeyframes = m_dragStartTrack.keyframes();
        if (m_dragKeyframeIndex < 0 || m_dragKeyframeIndex >= startKeyframes.size())
            return;

        KeyframePoint edited = startKeyframes[m_dragKeyframeIndex];
        const bool snapEnabled = !(modifiers & Qt::AltModifier);

        if (m_dragKind == DragKind::Keyframe) {
            double newTime = timeForX(m_dragMetrics, pos.x());
            double newValue = valueForY(m_dragMetrics, pos.y());
            if (snapEnabled) {
                newTime = snapToStep(newTime, niceGridStep(m_dragMetrics.duration / 24.0));
                newValue = snapToStep(newValue, niceGridStep(m_dragMetrics.valueSpan / 8.0));
            }

            double minTime = 0.0;
            double maxTime = m_dragMetrics.duration;
            if (m_dragKeyframeIndex > 0)
                minTime = qMax(minTime, startKeyframes[m_dragKeyframeIndex - 1].time + kMinKeyframeGapSeconds);
            if (m_dragKeyframeIndex + 1 < startKeyframes.size())
                maxTime = qMin(maxTime, startKeyframes[m_dragKeyframeIndex + 1].time - kMinKeyframeGapSeconds);
            edited.time = (minTime <= maxTime)
                ? qBound(minTime, newTime, maxTime)
                : startKeyframes[m_dragKeyframeIndex].time;
            edited.value = newValue;
        } else if (m_dragKind == DragKind::BezierOut || m_dragKind == DragKind::BezierIn) {
            if (m_dragKeyframeIndex + 1 >= startKeyframes.size())
                return;

            const KeyframePoint &next = startKeyframes[m_dragKeyframeIndex + 1];
            const double deltaTime = next.time - edited.time;
            if (!std::isfinite(deltaTime) || deltaTime <= 1e-9)
                return;

            double bezX = (timeForX(m_dragMetrics, pos.x()) - edited.time) / deltaTime;
            const double deltaValue = next.value - edited.value;
            double bezY = (m_dragKind == DragKind::BezierOut) ? edited.bezY1 : edited.bezY2;
            if (std::isfinite(deltaValue) && qAbs(deltaValue) > 1e-9)
                bezY = (valueForY(m_dragMetrics, pos.y()) - edited.value) / deltaValue;

            if (snapEnabled) {
                bezX = snapToStep(bezX, 0.05);
                bezY = snapToStep(bezY, 0.05);
            }

            edited.interpolation = KeyframePoint::Bezier;
            if (m_dragKind == DragKind::BezierOut) {
                edited.bezX1 = qBound(0.0, bezX, 1.0);
                edited.bezY1 = qBound(-2.0, bezY, 3.0);
            } else {
                edited.bezX2 = qBound(0.0, bezX, 1.0);
                edited.bezY2 = qBound(-2.0, bezY, 3.0);
            }
        }

        KeyframeTrack editedTrack = m_dragStartTrack;
        editedTrack.setKeyframePoint(m_dragKeyframeIndex, edited);
        const bool differsFromCurrent = !sameTrackKeyframes(editedTrack, m_curves[m_dragCurveIndex].track);
        m_dragChanged = !sameEditableKeyframe(edited, startKeyframes[m_dragKeyframeIndex]);

        if (differsFromCurrent) {
            m_curves[m_dragCurveIndex].track = editedTrack;
            if (m_editCallback)
                m_editCallback(m_curves, false);
            update();
        }
    }

    void clearDragState()
    {
        if (mouseGrabber() == this)
            releaseMouse();
        m_dragKind = DragKind::None;
        m_dragCurveIndex = -1;
        m_dragKeyframeIndex = -1;
        m_dragStartTrack = KeyframeTrack();
        m_dragMetrics = {};
        m_dragChanged = false;
        setCursor(Qt::ArrowCursor);
    }

    EditCallback m_editCallback;
    QVector<GraphEditorCurveTrack> m_curves;
    double m_durationSeconds = 1.0;
    double m_localPlayheadSeconds = 0.0;
    GraphMode m_graphMode = GraphMode::Value;
    DragKind m_dragKind = DragKind::None;
    int m_dragCurveIndex = -1;
    int m_dragKeyframeIndex = -1;
    int m_selectedCurveIndex = -1;
    int m_selectedKeyframeIndex = -1;
    KeyframeTrack m_dragStartTrack;
    LaneMetrics m_dragMetrics;
    bool m_dragChanged = false;
};

bool GraphEditorPanel::pasteTrackDefaultForTest(const ClipInfo &clip,
                                                const QString &property, double &value)
{
    return pasteTrackDefault(clip, property, value);
}

GraphEditorPanel::GraphEditorPanel(QWidget *parent)
    : QDockWidget(QStringLiteral("Graph Editor"), parent)
{
    setObjectName(QStringLiteral("GraphEditorDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                    | Qt::BottomDockWidgetArea);

    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_statusLabel = new QLabel(QStringLiteral("No clip selected"), root);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    auto *controlsLayout = new QHBoxLayout;
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);

    auto *graphModeLabel = new QLabel(QStringLiteral("Graph"), root);
    auto *graphModeCombo = new QComboBox(root);
    graphModeCombo->addItem(QStringLiteral("Value Graph"),
                            static_cast<int>(GraphEditorCurveView::GraphMode::Value));
    graphModeCombo->addItem(QStringLiteral("Speed Graph"),
                            static_cast<int>(GraphEditorCurveView::GraphMode::Speed));

    auto *presetLabel = new QLabel(QStringLiteral("Preset"), root);
    auto *presetCombo = new QComboBox(root);
    auto addPreset = [presetCombo](const QString &label, KeyframePoint::Interpolation interpolation) {
        presetCombo->addItem(label, static_cast<int>(interpolation));
    };
    addPreset(QStringLiteral("Linear"), KeyframePoint::Linear);
    addPreset(QStringLiteral("Ease In"), KeyframePoint::EaseIn);
    addPreset(QStringLiteral("Ease Out"), KeyframePoint::EaseOut);
    addPreset(QStringLiteral("Ease In/Out"), KeyframePoint::EaseInOut);
    addPreset(QStringLiteral("Hold"), KeyframePoint::Hold);
    addPreset(QStringLiteral("Elastic Out"), KeyframePoint::ElasticOut);
    addPreset(QStringLiteral("Bounce Out"), KeyframePoint::BounceOut);
    addPreset(QStringLiteral("Back Out"), KeyframePoint::BackOut);

    auto *applyPresetButton = new QPushButton(QStringLiteral("Apply"), root);

    auto *loopOutLabel = new QLabel(QStringLiteral("Loop Out"), root);
    m_loopOutCombo = new QComboBox(root);
    m_loopOutCombo->setObjectName(QStringLiteral("GraphEditorLoopOutCombo"));
    m_loopOutCombo->setAccessibleName(
        QStringLiteral("Loop Out mode for selected keyframe track"));
    m_loopOutCombo->addItem(QStringLiteral("None"), static_cast<int>(LoopMode::None));
    m_loopOutCombo->addItem(QStringLiteral("Cycle"), static_cast<int>(LoopMode::Cycle));
    m_loopOutCombo->addItem(QStringLiteral("PingPong"), static_cast<int>(LoopMode::PingPong));
    m_loopOutCombo->addItem(QStringLiteral("Continue"), static_cast<int>(LoopMode::Continue));
    m_loopOutCombo->setEnabled(false);
    m_loopOutCombo->setAccessibleDescription(
        QStringLiteral("Continue uses linear continuation after the last keyframe."));
    m_loopOutCombo->setItemData(
        m_loopOutCombo->findData(static_cast<int>(LoopMode::Continue)),
        QStringLiteral("Linear continuation after the last keyframe."),
        Qt::ToolTipRole);
    loopOutLabel->setBuddy(m_loopOutCombo);

    controlsLayout->addWidget(graphModeLabel);
    controlsLayout->addWidget(graphModeCombo);
    controlsLayout->addSpacing(8);
    controlsLayout->addWidget(presetLabel);
    controlsLayout->addWidget(presetCombo);
    controlsLayout->addWidget(applyPresetButton);
    controlsLayout->addSpacing(8);
    controlsLayout->addWidget(loopOutLabel);
    controlsLayout->addWidget(m_loopOutCombo);
    controlsLayout->addStretch(1);
    layout->addLayout(controlsLayout);

    auto *splitter = new QSplitter(Qt::Horizontal, root);
    m_trackList = new QListWidget(splitter);
    m_trackList->setObjectName(QStringLiteral("GraphEditorTrackList"));
    m_trackList->setAccessibleName(QStringLiteral("Keyframe tracks"));
    m_trackList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_trackList->setMinimumWidth(190);

    auto *scrollArea = new QScrollArea(splitter);
    scrollArea->setWidgetResizable(true);
    m_curveView = new GraphEditorCurveView(scrollArea);
    m_curveView->setEditCallback([this](const QVector<GraphEditorCurveTrack> &curves, bool commitUndo) {
        if (!m_timeline || m_trackIdx < 0 || m_clipIdx < 0)
            return;

        auto *track = m_timeline->videoTracks().value(m_trackIdx, nullptr);
        if (!track)
            return;

        auto clips = track->clips();
        if (m_clipIdx < 0 || m_clipIdx >= clips.size())
            return;

        KeyframeManager keyframes = clips[m_clipIdx].keyframes;
        for (const auto &curve : graphEditorEditableCurves(curves)) {
            KeyframeTrack editedTrack = curve.track;
            editedTrack.setPropertyName(curve.propertyName);
            keyframes.addTrack(editedTrack);
        }

        clips[m_clipIdx].keyframes = keyframes;
        track->setClips(clips);
        m_tracks = curves;

        if (commitUndo) {
            m_timeline->setClipEffectsAndKeyframes(m_trackIdx, m_clipIdx,
                                                   clips[m_clipIdx].effects,
                                                   keyframes);
            rebuildForSelection();
        }
    });

    connect(graphModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, graphModeCombo](int) {
                if (!m_curveView)
                    return;
                const auto mode = static_cast<GraphEditorCurveView::GraphMode>(
                    graphModeCombo->currentData().toInt());
                m_curveView->setGraphMode(mode);
            });
    connect(applyPresetButton, &QPushButton::clicked, this, [this, presetCombo]() {
        if (!m_curveView || !m_statusLabel)
            return;
        const auto interpolation = static_cast<KeyframePoint::Interpolation>(
            presetCombo->currentData().toInt());
        if (!m_curveView->applyInterpolationToSelected(interpolation)) {
            m_statusLabel->setText(QStringLiteral("Select a keyframe before applying an easing preset."));
            return;
        }
        m_statusLabel->setText(QStringLiteral("Applied %1 to the selected keyframe.")
                                   .arg(presetCombo->currentText()));
    });
    connect(m_trackList, &QListWidget::currentRowChanged,
            this, [this](int row) {
                if (row >= 0 && row < m_tracks.size())
                    m_selectedProperty = m_tracks[row].propertyName;
                syncLoopOutControl();
            });
    connect(m_loopOutCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (!m_timeline || !m_trackList || !m_loopOutCombo)
                    return;

                const int row = m_trackList->currentRow();
                if (row < 0 || row >= m_tracks.size())
                    return;
                if (m_tracks[row].isPastePlaceholder)
                    return;
                if (m_trackIdx < 0 || m_trackIdx >= m_timeline->videoTracks().size())
                    return;

                auto *timelineTrack = m_timeline->videoTracks().value(m_trackIdx, nullptr);
                if (!timelineTrack)
                    return;
                const auto clips = timelineTrack->clips();
                if (m_clipIdx < 0 || m_clipIdx >= clips.size())
                    return;

                const QString propertyName = m_tracks[row].propertyName;
                KeyframeManager keyframes = clips[m_clipIdx].keyframes;
                const auto mode = static_cast<LoopMode>(m_loopOutCombo->currentData().toInt());
                if (keyframes.loopOutMode(propertyName) == mode)
                    return;

                keyframes.setLoopOutMode(propertyName, mode);
                m_timeline->setClipEffectsAndKeyframes(m_trackIdx, m_clipIdx,
                                                       clips[m_clipIdx].effects,
                                                       keyframes);
                rebuildForSelection();
            });
    scrollArea->setWidget(m_curveView);

    splitter->addWidget(m_trackList);
    splitter->addWidget(scrollArea);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    auto *clipboardButton = new QToolButton(root);
    clipboardButton->setText(QStringLiteral("キーフレーム編集"));
    clipboardButton->setPopupMode(QToolButton::InstantPopup);
    auto *clipboardMenu = new QMenu(clipboardButton);
    auto *copyAction = clipboardMenu->addAction(QStringLiteral("選択キーフレームをコピー"));
    auto *pasteAction = clipboardMenu->addAction(QStringLiteral("再生ヘッド位置にキーフレームを貼り付け"));
    connect(copyAction, &QAction::triggered, this, [this]() { copySelectedKeyframes(); });
    connect(pasteAction, &QAction::triggered, this, [this]() {
        if (!pasteKeyframes())
            m_statusLabel->setText(QStringLiteral("貼り付けできません。クリップと対応する貼り付け先を選択してください。"));
    });
    connect(clipboardMenu, &QMenu::aboutToShow, this, [this, copyAction, pasteAction]() {
        // Another graph panel may have updated the shared clipboard.
        rebuildForSelection();
        copyAction->setEnabled(!m_selectedKeyframeIndices.isEmpty());
        pasteAction->setEnabled(!s_keyframeClipboard.isEmpty() && !m_tracks.isEmpty());
    });
    clipboardButton->setMenu(clipboardMenu);
    clipboardButton->setToolTip(QStringLiteral("Ctrl／Shift クリックで同じパラメータのキーを複数選択。貼り付け先はトラック一覧で選択します。"));
    layout->addWidget(clipboardButton);
    setWidget(root);
    showEmptyState(QStringLiteral("No clip selected"));
}

void GraphEditorPanel::setTimeline(Timeline *timeline)
{
    if (m_timeline != timeline) {
        m_selectedKeyframeIndices.clear();
        m_keyframeSelectionProperty.clear();
    }
    m_timeline = timeline;
    m_playheadSeconds = m_timeline ? m_timeline->playheadPosition() : 0.0;
    rebuildForSelection();
}

void GraphEditorPanel::setSelectedClip(int trackIdx, int clipIdx)
{
    if (m_trackIdx != trackIdx || m_clipIdx != clipIdx) {
        m_selectedKeyframeIndices.clear();
        m_keyframeSelectionProperty.clear();
        // On a different clip, prefer its existing tracks over the virtual
        // clipboard destination. Keep the property across undo's (-1, -1).
        if (m_trackIdx >= 0 && m_clipIdx >= 0 && trackIdx >= 0 && clipIdx >= 0) {
            m_selectedProperty.clear();
            const QSignalBlocker blocker(m_trackList);
            m_trackList->setCurrentRow(-1);
        }
    }
    m_trackIdx = trackIdx;
    m_clipIdx = clipIdx;
    rebuildForSelection();
}

void GraphEditorPanel::setPlayheadSeconds(double seconds)
{
    m_playheadSeconds = qMax(0.0, seconds);
    const double local = qBound(0.0,
                                m_playheadSeconds - m_clipStartSeconds,
                                qMax(0.0, m_clipDurationSeconds));
    if (m_curveView)
        m_curveView->setCurves(m_tracks, m_clipDurationSeconds, local);
}

void GraphEditorPanel::refreshFromTimeline()
{
    rebuildForSelection();
}

void GraphEditorPanel::rebuildForSelection()
{
    if (m_trackList && m_trackList->currentItem()) {
        const QString currentProperty =
            m_trackList->currentItem()->data(Qt::UserRole).toString();
        if (!currentProperty.isEmpty())
            m_selectedProperty = currentProperty;
    }
    const QString selectedProperty = m_selectedProperty;

    if (!m_timeline) {
        showEmptyState(QStringLiteral("No timeline"));
        return;
    }

    if (m_trackIdx < 0 || m_clipIdx < 0) {
        const auto &tracks = m_timeline->videoTracks();
        for (int i = 0; i < tracks.size(); ++i) {
            const auto *track = tracks.value(i, nullptr);
            if (track && track->selectedClip() >= 0) {
                m_trackIdx = i;
                m_clipIdx = track->selectedClip();
                break;
            }
        }
    }

    if (m_trackIdx < 0 || m_trackIdx >= m_timeline->videoTracks().size()) {
        showEmptyState(QStringLiteral("No clip selected"));
        return;
    }

    const auto *track = m_timeline->videoTracks().value(m_trackIdx, nullptr);
    if (!track || m_clipIdx < 0 || m_clipIdx >= track->clips().size()) {
        showEmptyState(QStringLiteral("No clip selected"));
        return;
    }

    const ClipInfo &clip = track->clips()[m_clipIdx];
    m_clipStartSeconds = clipTimelineStartSeconds(m_timeline, m_trackIdx, m_clipIdx);
    m_clipDurationSeconds = qMax(0.0, clip.effectiveDuration());
    m_tracks.clear();
    m_tracks.reserve(clip.keyframes.tracks().size());

    int colorIndex = 0;
    for (const auto &kfTrack : clip.keyframes.tracks()) {
        GraphEditorCurveTrack curve;
        curve.propertyName = kfTrack.propertyName();
        curve.displayName = displayNameForTrack(kfTrack, clip.effects);
        curve.track = kfTrack;
        curve.color = curveColorForTrack(kfTrack.propertyName(), colorIndex++);
        m_tracks.append(curve);
    }

    // The clipboard source is also a selectable destination on unanimated
    // clips. This is a view-only placeholder until the explicit paste action.
    double pasteDefault = 0.0;
    if (!s_keyframeClipboard.isEmpty()
        && !clip.keyframes.hasTrack(s_clipboardSourceProperty)
        && pasteTrackDefault(clip, s_clipboardSourceProperty, pasteDefault)) {
        GraphEditorCurveTrack curve;
        curve.propertyName = s_clipboardSourceProperty;
        curve.track = KeyframeTrack(curve.propertyName, pasteDefault);
        curve.displayName = QStringLiteral("貼り付け先: %1").arg(curve.propertyName);
        curve.isPastePlaceholder = true;
        curve.color = curveColorForTrack(curve.propertyName, colorIndex++);
        m_tracks.append(curve);
    }

    int selectedRow = -1;
    {
        const QSignalBlocker blocker(m_trackList);
        m_trackList->clear();
        for (int row = 0; row < m_tracks.size(); ++row) {
            const auto &curve = m_tracks[row];
            auto *item = new QListWidgetItem(
                QStringLiteral("%1\n%2 keyframes")
                    .arg(curve.displayName)
                    .arg(curve.track.count()),
                m_trackList);
        item->setData(Qt::UserRole, curve.propertyName);
            item->setForeground(QBrush(curve.color));
            item->setToolTip(QStringLiteral("%1\n%2")
                                 .arg(curve.propertyName)
                                 .arg(curve.track.keyframes().isEmpty()
                                      ? QStringLiteral("No keyframes")
                                      : interpolationLabel(curve.track.keyframes().first().interpolation)));
            if (curve.propertyName == selectedProperty)
                selectedRow = row;
        }
        if (!m_tracks.isEmpty()) {
            const int restoredRow = selectedRow >= 0 ? selectedRow : 0;
            m_trackList->setCurrentRow(restoredRow);
            m_selectedProperty = m_tracks[restoredRow].propertyName;
        }
    }

    if (m_tracks.isEmpty()) {
        showEmptyState(QStringLiteral("Selected clip has no keyframe tracks"));
        return;
    }

    syncLoopOutControl();

    m_statusLabel->setText(QStringLiteral("V%1 Clip %2  |  %3 tracks  |  %4s")
                               .arg(m_trackIdx + 1)
                               .arg(m_clipIdx + 1)
                               .arg(m_tracks.size())
                               .arg(m_clipDurationSeconds, 0, 'f', 2));
    setPlayheadSeconds(m_playheadSeconds);
}

void GraphEditorPanel::showEmptyState(const QString &message)
{
    m_tracks.clear();
    m_clipStartSeconds = 0.0;
    m_clipDurationSeconds = 0.0;
    if (m_statusLabel)
        m_statusLabel->setText(message);
    if (m_trackList)
        m_trackList->clear();
    if (m_curveView)
        m_curveView->setCurves({}, 1.0, 0.0);
    syncLoopOutControl();
}

void GraphEditorPanel::syncLoopOutControl()
{
    if (!m_loopOutCombo)
        return;

    bool hasSelectedTrack = false;
    LoopMode mode = LoopMode::None;
    const int row = m_trackList ? m_trackList->currentRow() : -1;
    if (m_timeline
        && row >= 0
        && row < m_tracks.size()
        && m_trackIdx >= 0
        && m_trackIdx < m_timeline->videoTracks().size()) {
        const auto *timelineTrack = m_timeline->videoTracks().value(m_trackIdx, nullptr);
        if (timelineTrack) {
            const auto &clips = timelineTrack->clips();
            if (m_clipIdx >= 0 && m_clipIdx < clips.size()) {
                hasSelectedTrack = clips[m_clipIdx].keyframes.hasTrack(m_tracks[row].propertyName);
                mode = clips[m_clipIdx].keyframes.loopOutMode(m_tracks[row].propertyName);
            }
        }
    }

    const QSignalBlocker blocker(m_loopOutCombo);
    const int index = m_loopOutCombo->findData(static_cast<int>(mode));
    m_loopOutCombo->setCurrentIndex(index >= 0 ? index : 0);
    m_loopOutCombo->setEnabled(hasSelectedTrack);
}

QVector<KeyframePoint> GraphEditorPanel::s_keyframeClipboard;
QString GraphEditorPanel::s_clipboardSourceProperty;

bool GraphEditorPanel::selectKeyframes(const QString &property, const QVector<int> &indices)
{
    for (int row = 0; row < m_tracks.size(); ++row) {
        if (m_tracks[row].propertyName != property)
            continue;
        for (int index : indices) {
            if (index < 0 || index >= m_tracks[row].track.count())
                return false;
        }
        m_keyframeSelectionProperty = property;
        m_selectedKeyframeIndices = indices;
        std::sort(m_selectedKeyframeIndices.begin(), m_selectedKeyframeIndices.end());
        m_selectedKeyframeIndices.erase(std::unique(m_selectedKeyframeIndices.begin(),
            m_selectedKeyframeIndices.end()), m_selectedKeyframeIndices.end());
        m_trackList->setCurrentRow(row);
        m_curveView->update();
        return true;
    }
    return false;
}

bool GraphEditorPanel::copySelectedKeyframes()
{
    for (const auto &curve : m_tracks) {
        if (curve.propertyName != m_keyframeSelectionProperty)
            continue;
        QVector<KeyframePoint> points;
        for (int index : m_selectedKeyframeIndices) {
            if (index < 0 || index >= curve.track.count())
                return false;
            points.append(curve.track.keyframes()[index]);
        }
        if (points.isEmpty())
            return false;
        s_keyframeClipboard = points;
        s_clipboardSourceProperty = curve.propertyName;
        return true;
    }
    return false;
}

bool GraphEditorPanel::pasteKeyframes()
{
    if (!m_timeline || s_keyframeClipboard.isEmpty())
        return false;
    rebuildForSelection();
    auto *track = m_timeline->videoTracks().value(m_trackIdx, nullptr);
    if (!track || m_clipIdx < 0 || m_clipIdx >= track->clips().size())
        return false;
    const auto clip = track->clips()[m_clipIdx];
    auto manager = clip.keyframes;
    auto *destination = manager.track(m_selectedProperty);
    if (!destination) {
        double defaultValue = 0.0;
        if (!pasteTrackDefault(clip, m_selectedProperty, defaultValue))
            return false;
        manager.addTrack(KeyframeTrack(m_selectedProperty, defaultValue));
        destination = manager.track(m_selectedProperty);
    }
    // Preserve relative times, including beyond the clip end. Only the anchor
    // is bounded at the clip start; values retain the existing evaluation rules.
    const double anchor = qMax(0.0, m_playheadSeconds - m_clipStartSeconds);
    const double shift = anchor - s_keyframeClipboard.first().time;
    for (auto point : s_keyframeClipboard) {
        point.time += shift;
        destination->addKeyframe(point.time, point.value, point.interpolation,
            point.bezX1, point.bezY1, point.bezX2, point.bezY2,
            point.hasSpatialTangent, point.spatialOutX, point.spatialOutY,
            point.spatialInX, point.spatialInY);
    }
    m_timeline->setClipEffectsAndKeyframes(m_trackIdx, m_clipIdx, clip.effects, manager);
    rebuildForSelection();
    return true;
}
