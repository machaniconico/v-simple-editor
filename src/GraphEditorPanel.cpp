#include "GraphEditorPanel.h"
#include "Timeline.h"
#include "VideoEffect.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QFontMetrics>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

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

} // namespace

class GraphEditorCurveView : public QWidget
{
public:
    explicit GraphEditorCurveView(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumWidth(420);
        setMouseTracking(true);
    }

    void setCurves(const QVector<GraphEditorCurveTrack> &curves,
                   double clipDurationSeconds,
                   double localPlayheadSeconds)
    {
        m_curves = curves;
        m_durationSeconds = curveDurationFor(m_curves, clipDurationSeconds);
        m_localPlayheadSeconds = localPlayheadSeconds;
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

        const int left = 118;
        const int right = 18;
        const int top = 28;
        const int laneHeight = 78;
        const int laneGap = 16;
        const int graphWidth = qMax(1, width() - left - right);
        const double duration = qMax(1.0, m_durationSeconds);

        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.drawText(QRect(left, 4, graphWidth, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("0.00s"));
        painter.drawText(QRect(left, 4, graphWidth, 18),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1s").arg(duration, 0, 'f', 2));

        const int playheadX = left + qRound(qBound(0.0, m_localPlayheadSeconds, duration)
                                           / duration * graphWidth);

        for (int i = 0; i < m_curves.size(); ++i) {
            const auto &curve = m_curves[i];
            const int y = top + i * (laneHeight + laneGap);
            const QRect laneRect(left, y, graphWidth, laneHeight);

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

            double minValue = curve.track.defaultValue();
            double maxValue = curve.track.defaultValue();
            const int sampleCount = qMax(24, qMin(240, graphWidth));
            for (int s = 0; s <= sampleCount; ++s) {
                const double t = duration * static_cast<double>(s) / sampleCount;
                const double v = curve.track.valueAt(t);
                minValue = qMin(minValue, v);
                maxValue = qMax(maxValue, v);
            }
            for (const auto &kf : curve.track.keyframes()) {
                minValue = qMin(minValue, kf.value);
                maxValue = qMax(maxValue, kf.value);
            }
            if (qFuzzyCompare(minValue, maxValue)) {
                minValue -= 1.0;
                maxValue += 1.0;
            }
            const double valueSpan = qMax(0.000001, maxValue - minValue);
            auto mapY = [&](double value) {
                const double normalized = (value - minValue) / valueSpan;
                return laneRect.bottom() - qRound(normalized * laneRect.height());
            };

            QPainterPath path;
            for (int x = 0; x <= graphWidth; ++x) {
                const double t = duration * static_cast<double>(x) / graphWidth;
                const QPointF point(laneRect.left() + x, mapY(curve.track.valueAt(t)));
                if (x == 0)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            painter.setPen(QPen(curve.color, 2.0));
            painter.drawPath(path);

            painter.setBrush(curve.color);
            painter.setPen(QPen(palette().color(QPalette::Window), 1));
            for (const auto &kf : curve.track.keyframes()) {
                const int kx = laneRect.left() + qRound(qBound(0.0, kf.time, duration)
                                                        / duration * laneRect.width());
                const int ky = mapY(kf.value);
                QPolygonF diamond;
                diamond << QPointF(kx, ky - 5)
                        << QPointF(kx + 5, ky)
                        << QPointF(kx, ky + 5)
                        << QPointF(kx - 5, ky);
                painter.drawPolygon(diamond);

                if (kf.interpolation == KeyframePoint::Bezier) {
                    painter.setPen(QPen(curve.color.lighter(135), 1));
                    painter.drawEllipse(QPointF(kx, ky), 7, 7);
                    painter.setPen(QPen(palette().color(QPalette::Window), 1));
                }
            }

            painter.setPen(palette().color(QPalette::Text));
            const QString elided = QFontMetrics(painter.font())
                .elidedText(curve.displayName, Qt::ElideRight, left - 18);
            painter.drawText(QRect(8, y, left - 18, 20),
                             Qt::AlignLeft | Qt::AlignVCenter, elided);

            painter.setPen(palette().color(QPalette::Mid));
            painter.drawText(QRect(8, y + 24, left - 18, 18),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1 kf").arg(curve.track.count()));
            painter.drawText(QRect(8, y + 44, left - 18, 18),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1..%2")
                                 .arg(minValue, 0, 'g', 4)
                                 .arg(maxValue, 0, 'g', 4));
        }

        painter.setPen(QPen(QColor(QStringLiteral("#ff4d4f")), 1.5));
        painter.drawLine(playheadX, top - 10, playheadX,
                         top + m_curves.size() * (laneHeight + laneGap) - laneGap + 8);
        painter.setBrush(QColor(QStringLiteral("#ff4d4f")));
        painter.setPen(Qt::NoPen);
        QPolygonF playheadMarker;
        playheadMarker << QPointF(playheadX - 5, top - 12)
                       << QPointF(playheadX + 5, top - 12)
                       << QPointF(playheadX, top - 4);
        painter.drawPolygon(playheadMarker);
    }

private:
    QVector<GraphEditorCurveTrack> m_curves;
    double m_durationSeconds = 1.0;
    double m_localPlayheadSeconds = 0.0;
};

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

    auto *splitter = new QSplitter(Qt::Horizontal, root);
    m_trackList = new QListWidget(splitter);
    m_trackList->setSelectionMode(QAbstractItemView::NoSelection);
    m_trackList->setMinimumWidth(190);

    auto *scrollArea = new QScrollArea(splitter);
    scrollArea->setWidgetResizable(true);
    m_curveView = new GraphEditorCurveView(scrollArea);
    scrollArea->setWidget(m_curveView);

    splitter->addWidget(m_trackList);
    splitter->addWidget(scrollArea);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    setWidget(root);
    showEmptyState(QStringLiteral("No clip selected"));
}

void GraphEditorPanel::setTimeline(Timeline *timeline)
{
    m_timeline = timeline;
    m_playheadSeconds = m_timeline ? m_timeline->playheadPosition() : 0.0;
    rebuildForSelection();
}

void GraphEditorPanel::setSelectedClip(int trackIdx, int clipIdx)
{
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
        curve.color = curveColor(colorIndex++);
        m_tracks.append(curve);
    }

    m_trackList->clear();
    for (const auto &curve : m_tracks) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n%2 keyframes")
                .arg(curve.displayName)
                .arg(curve.track.count()),
            m_trackList);
        item->setForeground(QBrush(curve.color));
        item->setToolTip(QStringLiteral("%1\n%2")
                             .arg(curve.propertyName)
                             .arg(curve.track.keyframes().isEmpty()
                                  ? QStringLiteral("No keyframes")
                                  : interpolationLabel(curve.track.keyframes().first().interpolation)));
    }

    if (m_tracks.isEmpty()) {
        showEmptyState(QStringLiteral("Selected clip has no keyframe tracks"));
        return;
    }

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
}
