#include "EffectKeyframeNavBar.h"

#include "EasingCurveEditorDialog.h"
#include "Keyframe.h"

#include <QActionGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygon>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kDiamondSize = 8;
constexpr int kHitTolerancePx = 5;
constexpr int kHorizontalPadding = 6;
constexpr double kSnapFps = 30.0;

QVector<KeyframePoint> &mutableKeyframes(KeyframeTrack *track)
{
    return const_cast<QVector<KeyframePoint> &>(track->keyframes());
}

bool sameKeyframe(const KeyframePoint &a, const KeyframePoint &b)
{
    return std::abs(a.time - b.time) < 1e-6
        && std::abs(a.value - b.value) < 1e-6
        && a.interpolation == b.interpolation;
}

QPolygon diamondPolygon(const QPoint &center)
{
    const int half = kDiamondSize / 2;
    return QPolygon({
        QPoint(center.x(), center.y() - half),
        QPoint(center.x() + half, center.y()),
        QPoint(center.x(), center.y() + half),
        QPoint(center.x() - half, center.y())
    });
}

QVector<int> currentSegmentKeyframes(const QVector<KeyframePoint> &keyframes, double playheadSeconds)
{
    QVector<int> indices;
    if (keyframes.isEmpty()) {
        return indices;
    }

    if (playheadSeconds <= keyframes.first().time) {
        indices.append(0);
        return indices;
    }

    if (playheadSeconds >= keyframes.last().time) {
        indices.append(keyframes.size() - 1);
        return indices;
    }

    for (int i = 0; i + 1 < keyframes.size(); ++i) {
        if (playheadSeconds >= keyframes[i].time && playheadSeconds <= keyframes[i + 1].time) {
            indices.append(i);
            indices.append(i + 1);
            return indices;
        }
    }

    return indices;
}

} // namespace

namespace effectctrl {

EffectKeyframeNavBar::EffectKeyframeNavBar(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void EffectKeyframeNavBar::setTrack(KeyframeTrack *track, double clipDurationSeconds, double playheadSeconds)
{
    m_track = track;
    m_clipDurationSeconds = qMax(0.0, clipDurationSeconds);
    m_playheadSeconds = qBound(0.0, playheadSeconds, m_clipDurationSeconds);

    const int count = m_track ? m_track->count() : 0;
    if (m_selectedKeyframe >= count) {
        setSelectedIndex(-1);
    } else {
        update();
    }
}

void EffectKeyframeNavBar::setPlayhead(double seconds)
{
    const double clamped = qBound(0.0, seconds, m_clipDurationSeconds);
    if (std::abs(m_playheadSeconds - clamped) < 1e-6) {
        return;
    }

    m_playheadSeconds = clamped;
    update();
}

void EffectKeyframeNavBar::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int centerY = height() / 2;
    const int left = kHorizontalPadding;
    const int right = width() - kHorizontalPadding;

    painter.setPen(QPen(palette().mid().color(), 1.0));
    painter.drawLine(left, centerY, qMax(left, right), centerY);

    const int playheadX = timeToPixel(m_playheadSeconds);
    painter.setPen(QPen(palette().highlight().color(), 1.0));
    painter.drawLine(playheadX, 2, playheadX, height() - 2);

    if (!m_track) {
        return;
    }

    const auto &keyframes = m_track->keyframes();
    const QVector<int> filledIndices = currentSegmentKeyframes(keyframes, m_playheadSeconds);
    const QColor fillColor = palette().highlight().color();
    const QColor outlineColor = palette().windowText().color();
    const QColor selectedColor = palette().highlight().color().lighter(140);

    for (int i = 0; i < keyframes.size(); ++i) {
        const QPoint center(timeToPixel(keyframes[i].time), centerY);
        const QPolygon diamond = diamondPolygon(center);
        const bool filled = filledIndices.contains(i);
        const bool selected = i == m_selectedKeyframe;

        painter.setPen(QPen(selected ? selectedColor : outlineColor, selected ? 2.0 : 1.0));
        painter.setBrush(filled ? fillColor : Qt::NoBrush);
        painter.drawPolygon(diamond);
    }
}

void EffectKeyframeNavBar::mousePressEvent(QMouseEvent *event)
{
    if (!m_track) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int hitIndex = hitTestKeyframe(event->pos());

    if (event->button() == Qt::RightButton) {
        if (hitIndex >= 0) {
            showDiamondContextMenu(hitIndex, event->globalPos());
        } else {
            showEmptyContextMenu(pixelToTime(event->pos().x()), event->globalPos());
        }
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        if (hitIndex >= 0) {
            setSelectedIndex(hitIndex);
            m_dragging = true;
            m_dragChanged = false;
            event->accept();
            return;
        }

        setSelectedIndex(-1);
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void EffectKeyframeNavBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !m_track || m_selectedKeyframe < 0 || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    auto &keyframes = mutableKeyframes(m_track);
    if (m_selectedKeyframe >= keyframes.size()) {
        return;
    }

    double newTime = pixelToTime(event->pos().x());
    if (event->modifiers() & Qt::ControlModifier) {
        newTime = std::round(newTime * kSnapFps) / kSnapFps;
    }
    newTime = qBound(0.0, newTime, m_clipDurationSeconds);

    if (std::abs(keyframes[m_selectedKeyframe].time - newTime) < 1e-6) {
        return;
    }

    keyframes[m_selectedKeyframe].time = newTime;
    m_dragChanged = true;
    update();
    event->accept();
}

void EffectKeyframeNavBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;

        if (m_track) {
            auto &keyframes = mutableKeyframes(m_track);
            if (m_selectedKeyframe >= 0 && m_selectedKeyframe < keyframes.size()) {
                const KeyframePoint moved = keyframes[m_selectedKeyframe];
                std::sort(keyframes.begin(), keyframes.end(),
                          [](const KeyframePoint &a, const KeyframePoint &b) {
                              return a.time < b.time;
                          });

                int newIndex = -1;
                for (int i = 0; i < keyframes.size(); ++i) {
                    if (sameKeyframe(keyframes[i], moved)) {
                        newIndex = i;
                        break;
                    }
                }
                setSelectedIndex(newIndex);
            }
        }

        update();
        if (m_dragChanged) {
            emit trackChanged();
        }
        m_dragChanged = false;
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

int EffectKeyframeNavBar::hitTestKeyframe(const QPoint &pos) const
{
    if (!m_track) {
        return -1;
    }

    const int centerY = height() / 2;
    const auto &keyframes = m_track->keyframes();
    for (int i = 0; i < keyframes.size(); ++i) {
        const int centerX = timeToPixel(keyframes[i].time);
        if (std::abs(pos.x() - centerX) <= kHitTolerancePx
            && std::abs(pos.y() - centerY) <= kHitTolerancePx) {
            return i;
        }
    }

    return -1;
}

int EffectKeyframeNavBar::timeToPixel(double seconds) const
{
    const int usableWidth = qMax(1, width() - (2 * kHorizontalPadding));
    if (m_clipDurationSeconds <= 0.0) {
        return kHorizontalPadding;
    }

    const double clamped = qBound(0.0, seconds, m_clipDurationSeconds);
    const double normalized = clamped / m_clipDurationSeconds;
    return kHorizontalPadding + qRound(normalized * usableWidth);
}

double EffectKeyframeNavBar::pixelToTime(int x) const
{
    if (m_clipDurationSeconds <= 0.0) {
        return 0.0;
    }

    const int usableWidth = qMax(1, width() - (2 * kHorizontalPadding));
    const int clampedX = qBound(kHorizontalPadding, x, kHorizontalPadding + usableWidth);
    const double normalized = static_cast<double>(clampedX - kHorizontalPadding) / usableWidth;
    return qBound(0.0, normalized * m_clipDurationSeconds, m_clipDurationSeconds);
}

void EffectKeyframeNavBar::setSelectedIndex(int index)
{
    if (m_selectedKeyframe == index) {
        update();
        return;
    }

    m_selectedKeyframe = index;
    emit selectedKeyframeChanged(index);
    update();
}

void EffectKeyframeNavBar::showDiamondContextMenu(int index, const QPoint &globalPos)
{
    if (!m_track) {
        return;
    }

    auto &keyframes = mutableKeyframes(m_track);
    if (index < 0 || index >= keyframes.size()) {
        return;
    }

    setSelectedIndex(index);

    QMenu menu(this);
    QAction *deleteAction = menu.addAction(QStringLiteral("Delete keyframe"));
    QMenu *interpolationMenu = menu.addMenu(QStringLiteral("Interpolation"));
    auto *group = new QActionGroup(interpolationMenu);
    group->setExclusive(true);

    auto addInterpolationAction = [&](const QString &text, KeyframePoint::Interpolation interpolation) {
        QAction *action = interpolationMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(keyframes[index].interpolation == interpolation);
        action->setActionGroup(group);
        connect(action, &QAction::triggered, this, [this, index, interpolation]() {
            if (!m_track) {
                return;
            }
            auto &kfs = mutableKeyframes(m_track);
            if (index < 0 || index >= kfs.size()) {
                return;
            }
            kfs[index].interpolation = interpolation;
            update();
            emit trackChanged();
        });
    };

    addInterpolationAction(QStringLiteral("Linear"), KeyframePoint::Linear);
    addInterpolationAction(QStringLiteral("EaseIn"), KeyframePoint::EaseIn);
    addInterpolationAction(QStringLiteral("EaseOut"), KeyframePoint::EaseOut);
    addInterpolationAction(QStringLiteral("EaseInOut"), KeyframePoint::EaseInOut);
    addInterpolationAction(QStringLiteral("Hold"), KeyframePoint::Hold);

    interpolationMenu->addSeparator();
    QAction *editCurveAction = interpolationMenu->addAction(QStringLiteral("イージングカーブを編集…"));
    connect(editCurveAction, &QAction::triggered, this, [this, index]() {
        if (!m_track) {
            return;
        }

        auto &kfs = mutableKeyframes(m_track);
        if (index < 0 || index >= kfs.size()) {
            return;
        }

        EasingCurveEditorDialog dialog(this);
        if (kfs[index].interpolation == KeyframePoint::Bezier) {
            dialog.setInitialCurve(kfs[index].bezX1, kfs[index].bezY1,
                                   kfs[index].bezX2, kfs[index].bezY2);
        } else {
            dialog.setInitialCurve(0.0, 0.0, 1.0, 1.0);
        }

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 1.0;
        double y2 = 1.0;
        dialog.getCurve(x1, y1, x2, y2);

        auto &updatedKfs = mutableKeyframes(m_track);
        if (index < 0 || index >= updatedKfs.size()) {
            return;
        }
        updatedKfs[index].interpolation = KeyframePoint::Bezier;
        updatedKfs[index].bezX1 = x1;
        updatedKfs[index].bezY1 = y1;
        updatedKfs[index].bezX2 = x2;
        updatedKfs[index].bezY2 = y2;
        update();
        emit trackChanged();
    });

    connect(deleteAction, &QAction::triggered, this, [this, index]() {
        if (!m_track) {
            return;
        }
        m_track->removeKeyframe(index);
        setSelectedIndex(-1);
        update();
        emit trackChanged();
    });

    menu.exec(globalPos);
}

void EffectKeyframeNavBar::showEmptyContextMenu(double timeSeconds, const QPoint &globalPos)
{
    if (!m_track) {
        return;
    }

    const double clampedTime = qBound(0.0, timeSeconds, m_clipDurationSeconds);
    QMenu menu(this);
    QAction *addAction = menu.addAction(
        QStringLiteral("Add keyframe at %1s").arg(clampedTime, 0, 'f', 2));

    connect(addAction, &QAction::triggered, this, [this, clampedTime]() {
        if (!m_track) {
            return;
        }

        const double value = m_track->valueAt(clampedTime);
        m_track->addKeyframe(clampedTime, value, KeyframePoint::Linear);

        const auto &keyframes = m_track->keyframes();
        int insertedIndex = -1;
        for (int i = 0; i < keyframes.size(); ++i) {
            if (std::abs(keyframes[i].time - clampedTime) < 1e-6) {
                insertedIndex = i;
                break;
            }
        }
        setSelectedIndex(insertedIndex);
        update();
        emit trackChanged();
    });

    menu.exec(globalPos);
}

} // namespace effectctrl
