#include "PuppetPinTool.h"
#include "Timeline.h"
#include <QPainter>
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <algorithm>

PuppetPinTool::PuppetPinTool(Timeline *timeline, int trackIndex, int clipIndex, QObject *parent)
    : SurfaceTool(nullptr, parent), m_timeline(timeline),
      m_trackIndex(trackIndex), m_clipIndex(clipIndex)
{
    auto *track = timeline ? timeline->videoTracks().value(trackIndex, nullptr) : nullptr;
    const bool validTarget = track && clipIndex >= 0 && clipIndex < track->clips().size();
    if (validTarget) {
        const auto &clip = track->clips()[clipIndex];
        m_targetFilePath = clip.filePath;
        m_targetInPoint = clip.inPoint;
    }
    setEnabled(validTarget);
}

bool PuppetPinTool::isEnabled() const
{
    if (!SurfaceTool::isEnabled() || !m_timeline) return false;
    auto *track = m_timeline->videoTracks().value(m_trackIndex, nullptr);
    return track && !track->isLocked() && m_clipIndex >= 0
        && m_clipIndex < track->clips().size()
        && track->clips()[m_clipIndex].filePath == m_targetFilePath
        && track->clips()[m_clipIndex].inPoint == m_targetInPoint;
}

QVector<WarpPin> PuppetPinTool::currentPins() const
{
    return isEnabled() ? m_timeline->videoTracks()[m_trackIndex]->clips()[m_clipIndex].puppetPins
                       : QVector<WarpPin>{};
}

QPointF PuppetPinTool::toWidget(const QPointF &uv) const
{
    return m_viewRect.topLeft() + QPointF(uv.x() * m_viewRect.width(), uv.y() * m_viewRect.height());
}

QPointF PuppetPinTool::toUv(const QPoint &pos) const
{
    return QPointF(std::clamp((pos.x() - m_viewRect.left()) / m_viewRect.width(), 0.0, 1.0),
                   std::clamp((pos.y() - m_viewRect.top()) / m_viewRect.height(), 0.0, 1.0));
}

int PuppetPinTool::hitTestCorner(const QPoint &pos, const QRectF &rect) const
{
    if (!isEnabled() || rect.isEmpty()) return -1;
    const auto pins = currentPins();
    for (int i = pins.size() - 1; i >= 0; --i) {
        const auto uv = pins[i].deformedPos;
        const QPointF d = rect.topLeft() + QPointF(uv.x() * rect.width(), uv.y() * rect.height()) - pos;
        if (d.x() * d.x() + d.y() * d.y() <= 64.0) return i;
    }
    return -1;
}

void PuppetPinTool::paintOverlay(QPainter &painter, const QRectF &rect)
{
    m_viewRect = rect;
    if (!isEnabled() || rect.isEmpty()) return;
    painter.save();
    const auto pins = currentPins();
    for (const auto &pin : pins) {
        painter.setPen(QPen(QColor(255, 200, 50, 150), 1));
        painter.setBrush(Qt::NoBrush);
        const double radius = pin.radius * qMin(rect.width(), rect.height());
        painter.drawEllipse(toWidget(pin.originalPos), radius, radius);
        painter.drawLine(toWidget(pin.originalPos), toWidget(pin.deformedPos));
        painter.setBrush(QColor(255, 190, 30));
        painter.drawEllipse(toWidget(pin.deformedPos), 6.0, 6.0);
    }
    painter.setPen(Qt::white);
    painter.drawText(rect.adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft,
                    QStringLiteral("クリック: 追加 / ドラッグ: 移動 / 右クリック: 削除 / Shift+クリック: 半径・硬さ"));
    painter.restore();
}

bool PuppetPinTool::handleMousePress(const QPoint &pos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    if (!isEnabled() || m_handle >= 0 || m_viewRect.isEmpty() || !m_viewRect.contains(pos)) return false;
    const int hit = hitTestCorner(pos, m_viewRect);
    auto pins = currentPins();
    if (button == Qt::RightButton) {
        if (hit < 0) return false;
        pins.removeAt(hit);
        m_timeline->setClipPuppetPins(m_trackIndex, m_clipIndex, pins);
        emit previewChanged();
        return true;
    }
    if (button != Qt::LeftButton) return false;
    if (hit >= 0 && modifiers.testFlag(Qt::ShiftModifier)) {
        const auto before = pins;
        QDialog dialog;
        dialog.setWindowTitle(QStringLiteral("パペットピン"));
        QFormLayout layout(&dialog);
        QDoubleSpinBox radius, stiffness;
        radius.setRange(0.001, 2.0);
        radius.setDecimals(3);
        radius.setSingleStep(0.05);
        radius.setValue(pins[hit].radius);
        stiffness.setRange(0.0, 1.0);
        stiffness.setSingleStep(0.05);
        stiffness.setValue(pins[hit].stiffness);
        layout.addRow(QStringLiteral("半径（フレーム短辺比）"), &radius);
        layout.addRow(QStringLiteral("硬さ"), &stiffness);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons.button(QDialogButtonBox::Ok)->setText(QStringLiteral("適用"));
        buttons.button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
        layout.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted && isEnabled() && puppetpin::equal(currentPins(), before)) {
            pins[hit].radius = radius.value();
            pins[hit].stiffness = stiffness.value();
            m_timeline->setClipPuppetPins(m_trackIndex, m_clipIndex, pins);
            emit previewChanged();
        }
        return true;
    }
    m_before = pins;
    m_handle = hit;
    if (hit < 0) {
        WarpPin pin;
        pin.originalPos = pin.deformedPos = toUv(pos);
        pin.radius = 0.25;
        pins.append(pin);
        m_handle = pins.size() - 1;
    }
    m_dragPins = pins;
    m_pressPos = pos;
    m_previewPins = pins;
    m_previewApplied = true;
    m_timeline->previewClipPuppetPins(m_trackIndex, m_clipIndex, pins);
    emit previewChanged();
    return true;
}

bool PuppetPinTool::handleMouseMove(const QPoint &pos, Qt::KeyboardModifiers)
{
    if (m_handle < 0 || !isEnabled() || m_viewRect.isEmpty()) return false;
    // A restore/undo can replace the value while keeping the same clip identity.
    if (!puppetpin::equal(currentPins(), m_previewPins)) { m_handle = -1; return false; }
    auto pins = m_dragPins;
    const QPointF p = pins[m_handle].deformedPos + QPointF(
        (pos.x() - m_pressPos.x()) / m_viewRect.width(), (pos.y() - m_pressPos.y()) / m_viewRect.height());
    pins[m_handle].deformedPos = QPointF(std::clamp(p.x(), 0.0, 1.0), std::clamp(p.y(), 0.0, 1.0));
    m_previewPins = pins;
    m_timeline->previewClipPuppetPins(m_trackIndex, m_clipIndex, pins);
    emit previewChanged();
    return true;
}

bool PuppetPinTool::handleMouseRelease(const QPoint &pos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    if (button != Qt::LeftButton || m_handle < 0) return false;
    if (!handleMouseMove(pos, modifiers)) { m_handle = -1; return true; }
    const auto finalPins = currentPins();
    m_handle = -1;
    m_previewApplied = false;
    m_timeline->previewClipPuppetPins(m_trackIndex, m_clipIndex, m_before);
    m_timeline->setClipPuppetPins(m_trackIndex, m_clipIndex, finalPins);
    emit previewChanged();
    return true;
}

void PuppetPinTool::cancelDrag()
{
    if (m_handle < 0) return;
    m_handle = -1;
    if (isEnabled() && m_previewApplied && puppetpin::equal(currentPins(), m_previewPins))
        m_timeline->previewClipPuppetPins(m_trackIndex, m_clipIndex, m_before);
    m_previewApplied = false;
    emit previewChanged();
}
