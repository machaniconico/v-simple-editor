#include "PuppetPinTool.h"
#include "Timeline.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <algorithm>
#include <cmath>

namespace {

QColor colorForPinType(puppet::PinType type)
{
    switch (type) {
    case puppet::PinType::Position: return QColor(255, 195, 40);  // Amber
    case puppet::PinType::Bend:     return QColor(50, 215, 255);  // Cyan
    case puppet::PinType::Advanced: return QColor(235, 95, 255);  // Magenta
    case puppet::PinType::Starch:   return QColor(120, 175, 255); // Steel blue
    case puppet::PinType::Overlap:  return QColor(75, 230, 145);  // Emerald
    }
    return QColor(255, 195, 40);
}

} // namespace

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

double PuppetPinTool::currentClipLocalSec() const
{
    return m_timeProvider ? std::max(0.0, m_timeProvider()) : 0.0;
}

QString PuppetPinTool::currentClipUid() const
{
    if (!isEnabled()) return {};
    return m_timeline->videoTracks()[m_trackIndex]->clips()[m_clipIndex].uid;
}

bool PuppetPinTool::arapActive() const
{
    if (!isEnabled()) return false;
    const QString uid = currentClipUid();
    return m_useArapMode || m_toolMode != puppet::PinType::Position
        || (!uid.isEmpty() && puppet::hasRig(uid));
}

puppet::PuppetRig PuppetPinTool::currentRig() const
{
    if (!isEnabled()) return {};
    const auto &clip = m_timeline->videoTracks()[m_trackIndex]->clips()[m_clipIndex];
    if (!clip.uid.isEmpty() && puppet::hasRig(clip.uid))
        return puppet::rigForClip(clip.uid);
    if (!clip.puppetPins.isEmpty())
        return puppet::rigFromLegacyPins(clip.puppetPins);
    return {};
}

QVector<WarpPin> PuppetPinTool::currentPins() const
{
    return isEnabled() ? m_timeline->videoTracks()[m_trackIndex]->clips()[m_clipIndex].puppetPins
                       : QVector<WarpPin>{};
}

QPointF PuppetPinTool::toWidget(const QPointF &uv) const
{
    return SurfaceTool::toWidget(uv, m_viewRect);
}

QPointF PuppetPinTool::toUv(const QPoint &pos) const
{
    const QPointF uv = widgetToUv(pos, m_viewRect);
    return QPointF(std::clamp(uv.x(), 0.0, 1.0), std::clamp(uv.y(), 0.0, 1.0));
}

int PuppetPinTool::hitTestCorner(const QPoint &pos, const QRectF &rect) const
{
    if (!isEnabled() || rect.isEmpty() || !clipMapValid()) return -1;
    if (arapActive()) {
        const auto rig = currentRig();
        const auto eval = puppet::evaluateRigAt(rig, currentClipLocalSec());
        for (int i = eval.size() - 1; i >= 0; --i) {
            const QPointF d = SurfaceTool::toWidget(eval[i].position, rect) - pos;
            if (d.x() * d.x() + d.y() * d.y() <= 81.0) return i;
        }
        return -1;
    }
    const auto pins = currentPins();
    for (int i = pins.size() - 1; i >= 0; --i) {
        const QPointF d = SurfaceTool::toWidget(pins[i].deformedPos, rect) - pos;
        if (d.x() * d.x() + d.y() * d.y() <= 64.0) return i;
    }
    return -1;
}

void PuppetPinTool::paintOverlay(QPainter &painter, const QRectF &rect)
{
    m_viewRect = rect;
    if (!isEnabled() || rect.isEmpty()) return;
    syncClipTransform(*m_timeline, m_trackIndex, m_clipIndex);
    if (!clipMapValid()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (arapActive()) {
        const puppet::PuppetRig rig = currentRig();
        const double tSec = currentClipLocalSec();
        const auto eval = puppet::evaluateRigAt(rig, tSec);

        // Draw lightweight ARAP wireframe mesh preview when enabled.
        if (rig.mesh.showMesh && !eval.isEmpty()) {
            QImage dummy(160, 90, QImage::Format_ARGB32);
            dummy.fill(Qt::white);
            puppet::MeshSettings previewSettings = rig.mesh;
            previewSettings.density = std::clamp(rig.mesh.density, 6, 16);
            const puppet::PuppetMesh baseMesh = puppet::buildMeshForImage(dummy, previewSettings);
            const puppet::DeformedMesh deformed = puppet::solveArapMesh(baseMesh, eval);
            painter.setPen(QPen(QColor(80, 220, 255, 70), 1.0));
            const double invW = 1.0 / std::max(1, baseMesh.width);
            const double invH = 1.0 / std::max(1, baseMesh.height);
            for (const auto &tri : deformed.base.triangles) {
                const QPointF p0 = toWidget(QPointF(deformed.deformedVertices[tri[0]].x() * invW,
                                                    deformed.deformedVertices[tri[0]].y() * invH));
                const QPointF p1 = toWidget(QPointF(deformed.deformedVertices[tri[1]].x() * invW,
                                                    deformed.deformedVertices[tri[1]].y() * invH));
                const QPointF p2 = toWidget(QPointF(deformed.deformedVertices[tri[2]].x() * invW,
                                                    deformed.deformedVertices[tri[2]].y() * invH));
                painter.drawLine(p0, p1);
                painter.drawLine(p1, p2);
                painter.drawLine(p2, p0);
            }
        }

        // Draw color-coded pins & influence circles.
        for (int i = 0; i < eval.size(); ++i) {
            const auto &ep = eval[i];
            const QColor col = colorForPinType(ep.type);
            const QPointF wpRest = toWidget(ep.restPos);
            const QPointF wpCur = toWidget(ep.position);

            if (ep.type == puppet::PinType::Starch || ep.type == puppet::PinType::Overlap
                || ep.type == puppet::PinType::Bend) {
                QPen ringPen(QColor(col.red(), col.green(), col.blue(), 150), 1.0,
                             ep.type == puppet::PinType::Starch ? Qt::DashLine : Qt::SolidLine);
                painter.setPen(ringPen);
                painter.setBrush(Qt::NoBrush);
                painter.drawPolygon(sourceCircleToWidget(ep.restPos, ep.radius, rect));
            }
            if (ep.type == puppet::PinType::Position || ep.type == puppet::PinType::Advanced) {
                painter.setPen(QPen(QColor(col.red(), col.green(), col.blue(), 180), 1.2));
                painter.drawLine(wpRest, wpCur);
            }
            if (ep.type == puppet::PinType::Bend || ep.type == puppet::PinType::Advanced) {
                const double rad = ep.rotationDeg * 3.14159265358979323846 / 180.0;
                const double arm = 18.0 * std::clamp(ep.scale, 0.3, 2.5);
                const QPointF tip = wpCur + QPointF(std::cos(rad) * arm, std::sin(rad) * arm);
                painter.setPen(QPen(col, 1.5));
                painter.drawLine(wpCur, tip);
                painter.setBrush(col);
                painter.drawEllipse(tip, 3.0, 3.0);
            }

            painter.setPen(QPen(Qt::black, 1.2));
            painter.setBrush(col);
            switch (ep.type) {
            case puppet::PinType::Position:
                painter.drawEllipse(wpCur, 6.5, 6.5);
                break;
            case puppet::PinType::Bend: {
                QPolygonF dia;
                dia << wpCur + QPointF(0, -7) << wpCur + QPointF(7, 0)
                    << wpCur + QPointF(0, 7) << wpCur + QPointF(-7, 0);
                painter.drawPolygon(dia);
                break;
            }
            case puppet::PinType::Advanced: {
                QPolygonF hex;
                for (int k = 0; k < 6; ++k) {
                    const double a = k * 3.14159265358979323846 / 3.0;
                    hex << wpCur + QPointF(std::cos(a) * 7.5, std::sin(a) * 7.5);
                }
                painter.drawPolygon(hex);
                break;
            }
            case puppet::PinType::Starch:
                painter.drawRect(QRectF(wpCur.x() - 6.0, wpCur.y() - 6.0, 12.0, 12.0));
                break;
            case puppet::PinType::Overlap: {
                QPolygonF tri;
                tri << wpCur + QPointF(0, -7) << wpCur + QPointF(7, 6) << wpCur + QPointF(-7, 6);
                painter.drawPolygon(tri);
                painter.setPen(Qt::white);
                painter.drawText(wpCur + QPointF(9, 4), QStringLiteral("Z:%1").arg(ep.inFront, 0, 'f', 0));
                break;
            }
            }
        }

        // Wave 32: Draw live Puppet Sketch motion trail & REC badge while recording.
        if (m_sketchRecording && m_sketchSamples.size() >= 2) {
            QPolygonF trail;
            trail.reserve(m_sketchSamples.size());
            for (const auto &s : m_sketchSamples)
                trail << toWidget(s.posUv);
            painter.setPen(QPen(QColor(255, 70, 175, 220), 2.2));
            painter.drawPolyline(trail);
            const double dur = m_sketchSamples.last().timeSec - m_sketchSamples.first().timeSec;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(220, 35, 50, 210));
            painter.drawRoundedRect(QRectF(rect.right() - 175, rect.top() + 8, 165, 24), 4, 4);
            painter.setPen(Qt::white);
            painter.drawText(QRectF(rect.right() - 175, rect.top() + 8, 165, 24), Qt::AlignCenter,
                             QStringLiteral("REC %1s (%2 pts)")
                                 .arg(dur, 0, 'f', 2)
                                 .arg(m_sketchSamples.size()));
        }

        painter.setPen(Qt::white);
        painter.drawText(
            rect.adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft,
            QStringLiteral("ARAP パペット [%1 / t=%2s]  クリック:追加 / ドラッグ:移動 / Ctrl+ドラッグ:スケッチ録画 / Shift+クリック:設定 / 右クリック:削除")
                .arg(puppet::pinTypeName(m_toolMode))
                .arg(tSec, 0, 'f', 2));
        painter.restore();
        return;
    }

    // Legacy static overlay path (used when PuppetPinTool is instantiated directly in legacy tests).
    const auto pins = currentPins();
    for (const auto &pin : pins) {
        painter.setPen(QPen(QColor(255, 200, 50, 150), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(sourceCircleToWidget(pin.originalPos, pin.radius, rect));
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
    if (!isEnabled() || m_handle >= 0 || m_viewRect.isEmpty() || !m_viewRect.contains(pos)
        || !clipMapValid()) return false;
    const int hit = hitTestCorner(pos, m_viewRect);

    if (arapActive()) {
        auto rig = currentRig();
        const double tSec = currentClipLocalSec();
        if (button == Qt::RightButton) {
            if (hit < 0 || hit >= rig.pins.size()) return false;
            rig.pins.removeAt(hit);
            puppet::commitRigToTimeline(m_timeline, m_trackIndex, m_clipIndex, rig,
                                        QStringLiteral("パペットピンを削除"));
            emit previewChanged();
            return true;
        }
        if (button != Qt::LeftButton) return false;

        if (hit >= 0 && hit < rig.pins.size() && modifiers.testFlag(Qt::ShiftModifier)) {
            const auto beforeRig = rig;
            const auto ep = puppet::evaluatePinAt(rig.pins[hit], tSec);
            QDialog dialog;
            dialog.setWindowTitle(QStringLiteral("パペットピン設定 (ARAP)"));
            QFormLayout layout(&dialog);

            QComboBox typeCombo;
            for (int t = 0; t <= 4; ++t)
                typeCombo.addItem(puppet::pinTypeDisplayName(static_cast<puppet::PinType>(t)), t);
            typeCombo.setCurrentIndex(static_cast<int>(rig.pins[hit].type));

            QDoubleSpinBox radius, stiffness, rotSpin, scaleSpin, inFrontSpin;
            radius.setRange(0.01, 2.0);
            radius.setDecimals(3);
            radius.setSingleStep(0.05);
            radius.setValue(rig.pins[hit].radius);

            stiffness.setRange(0.0, 1.0);
            stiffness.setSingleStep(0.05);
            stiffness.setValue(rig.pins[hit].stiffness);

            rotSpin.setRange(-360.0, 360.0);
            rotSpin.setSuffix(QStringLiteral(" °"));
            rotSpin.setSingleStep(5.0);
            rotSpin.setValue(ep.rotationDeg);

            scaleSpin.setRange(0.2, 3.0);
            scaleSpin.setDecimals(2);
            scaleSpin.setSingleStep(0.05);
            scaleSpin.setValue(ep.scale);

            inFrontSpin.setRange(-100.0, 100.0);
            inFrontSpin.setSingleStep(10.0);
            inFrontSpin.setValue(ep.inFront);

            QComboBox easingCombo;
            easingCombo.addItem(QStringLiteral("イーズインアウト"), static_cast<int>(puppet::EasingKind::EaseInOut));
            easingCombo.addItem(QStringLiteral("リニア"), static_cast<int>(puppet::EasingKind::Linear));
            easingCombo.addItem(QStringLiteral("イーズイン"), static_cast<int>(puppet::EasingKind::EaseIn));
            easingCombo.addItem(QStringLiteral("イーズアウト"), static_cast<int>(puppet::EasingKind::EaseOut));
            easingCombo.addItem(QStringLiteral("ホールド"), static_cast<int>(puppet::EasingKind::Hold));

            QCheckBox delKfCheck(QStringLiteral("現在時刻のキーフレームを削除"));

            layout.addRow(QStringLiteral("ピン種別"), &typeCombo);
            layout.addRow(QStringLiteral("影響半径（短辺比）"), &radius);
            layout.addRow(QStringLiteral("硬さ (Starch)"), &stiffness);
            layout.addRow(QStringLiteral("回転 (Bend / Adv)"), &rotSpin);
            layout.addRow(QStringLiteral("スケール (Bend / Adv)"), &scaleSpin);
            layout.addRow(QStringLiteral("前後深度 (Overlap)"), &inFrontSpin);
            layout.addRow(QStringLiteral("イージング"), &easingCombo);
            layout.addRow(&delKfCheck);

            QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            buttons.button(QDialogButtonBox::Ok)->setText(QStringLiteral("適用"));
            buttons.button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
            layout.addRow(&buttons);
            connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

            if (dialog.exec() == QDialog::Accepted && isEnabled() && currentRig() == beforeRig) {
                auto &pin = rig.pins[hit];
                pin.type      = static_cast<puppet::PinType>(typeCombo.currentData().toInt());
                pin.radius    = radius.value();
                pin.stiffness = stiffness.value();
                if (delKfCheck.isChecked()) {
                    puppet::removePinKeyframeNear(pin, tSec, 0.05);
                } else {
                    puppet::PinKeyframe kf;
                    kf.timeSec     = tSec;
                    kf.position    = ep.position;
                    kf.rotationDeg = rotSpin.value();
                    kf.scale       = scaleSpin.value();
                    kf.inFront     = inFrontSpin.value();
                    kf.easing      = static_cast<puppet::EasingKind>(easingCombo.currentData().toInt());
                    puppet::upsertPinKeyframe(pin, kf);
                }
                puppet::commitRigToTimeline(m_timeline, m_trackIndex, m_clipIndex, rig,
                                            QStringLiteral("パペットピン設定を変更"));
                emit previewChanged();
            }
            return true;
        }

        m_beforeRig = rig;
        m_handle = hit;
        if (hit < 0) {
            const QPointF raw = widgetToUv(pos, m_viewRect);
            if (raw.x() < 0.0 || raw.x() > 1.0 || raw.y() < 0.0 || raw.y() > 1.0)
                return false;
            puppet::PuppetPin pin;
            pin.id = rig.nextPinId++;
            pin.name = QStringLiteral("ピン %1").arg(pin.id);
            pin.type = m_toolMode;
            pin.restPos = toUv(pos);
            pin.radius = 0.25;
            pin.stiffness = (m_toolMode == puppet::PinType::Starch) ? 0.8 : 0.5;
            puppet::PinKeyframe kf;
            kf.timeSec = rig.mesh.autoKey ? tSec : 0.0;
            kf.position = pin.restPos;
            kf.inFront = (m_toolMode == puppet::PinType::Overlap) ? 20.0 : 0.0;
            pin.keyframes.append(kf);
            rig.pins.append(pin);
            m_handle = rig.pins.size() - 1;
        }
        m_dragRig = rig;
        m_pressPos = pos;
        m_dragStartRotDeg = puppet::evaluatePinAt(rig.pins[m_handle], tSec).rotationDeg;
        m_previewRig = rig;
        m_previewApplied = true;

        // Wave 32: Ctrl + Left-drag on an existing Position/Advanced pin starts Puppet Sketch recording.
        m_sketchRecording = false;
        m_sketchSamples.clear();
        if (hit >= 0 && modifiers.testFlag(Qt::ControlModifier)) {
            const auto pType = rig.pins[m_handle].type;
            if (pType == puppet::PinType::Position || pType == puppet::PinType::Advanced) {
                m_sketchRecording = true;
                m_sketchStartSec = tSec;
                m_sketchTimer.start();
                const auto curEval = puppet::evaluatePinAt(rig.pins[m_handle], tSec);
                m_sketchSamples.append({tSec, curEval.position});
            }
        }

        puppet::previewRigOnTimeline(m_timeline, m_trackIndex, m_clipIndex, rig, tSec);
        emit previewChanged();
        return true;
    }

    // Legacy static path
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
        const QPointF raw = widgetToUv(pos, m_viewRect);
        if (raw.x() < 0.0 || raw.x() > 1.0 || raw.y() < 0.0 || raw.y() > 1.0) return false;
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

bool PuppetPinTool::recordSketchSampleAt(const QPoint &pos, double clipLocalSec)
{
    if (!m_sketchRecording || m_handle < 0 || !isEnabled() || m_viewRect.isEmpty())
        return false;
    if (m_handle >= m_dragRig.pins.size()) return false;
    const auto baseEval = puppet::evaluatePinAt(m_dragRig.pins[m_handle], m_sketchStartSec);
    const QPointF uv = baseEval.position + widgetDeltaToUv(QPointF(pos - m_pressPos), m_viewRect);
    const QPointF clamped(std::clamp(uv.x(), -0.25, 1.25), std::clamp(uv.y(), -0.25, 1.25));
    const double t = std::max(m_sketchStartSec, clipLocalSec);
    m_sketchSamples.append({t, clamped});

    auto rig = m_dragRig;
    puppet::PinKeyframe kf;
    kf.timeSec     = t;
    kf.position    = clamped;
    kf.rotationDeg = baseEval.rotationDeg;
    kf.scale       = baseEval.scale;
    kf.inFront     = baseEval.inFront;
    puppet::upsertPinKeyframe(rig.pins[m_handle], kf);
    m_previewRig = rig;
    puppet::previewRigOnTimeline(m_timeline, m_trackIndex, m_clipIndex, rig, t);
    emit previewChanged();
    return true;
}

bool PuppetPinTool::handleMouseMove(const QPoint &pos, Qt::KeyboardModifiers)
{
    if (m_handle < 0 || !isEnabled() || m_viewRect.isEmpty()) return false;
    if (arapActive()) {
        if (m_sketchRecording) {
            const double elapsed = m_sketchTimer.isValid()
                ? (static_cast<double>(m_sketchTimer.elapsed()) / 1000.0) : 0.0;
            return recordSketchSampleAt(pos, m_sketchStartSec + elapsed * m_sketchSpeed);
        }
        if (!(currentRig() == m_previewRig)) { m_handle = -1; return false; }
        auto rig = m_dragRig;
        if (m_handle >= rig.pins.size()) { m_handle = -1; return false; }
        const double tSec = currentClipLocalSec();
        auto &pin = rig.pins[m_handle];
        const auto baseEval = puppet::evaluatePinAt(pin, tSec);
        const double keyTime = rig.mesh.autoKey ? tSec
            : (pin.keyframes.isEmpty() ? 0.0 : pin.keyframes.first().timeSec);

        puppet::PinKeyframe kf;
        kf.timeSec     = keyTime;
        kf.position    = baseEval.position;
        kf.rotationDeg = baseEval.rotationDeg;
        kf.scale       = baseEval.scale;
        kf.inFront     = baseEval.inFront;

        if (pin.type == puppet::PinType::Bend) {
            // Horizontal drag adjusts Bend rotation angle intuitively (1.5 deg / px).
            kf.rotationDeg = std::clamp(m_dragStartRotDeg + (pos.x() - m_pressPos.x()) * 1.2,
                                        -180.0, 180.0);
            puppet::upsertPinKeyframe(pin, kf);
        } else if (pin.type == puppet::PinType::Starch || pin.type == puppet::PinType::Overlap) {
            const QPointF p = pin.restPos + widgetDeltaToUv(QPointF(pos - m_pressPos), m_viewRect);
            pin.restPos = QPointF(std::clamp(p.x(), 0.0, 1.0), std::clamp(p.y(), 0.0, 1.0));
            kf.position = pin.restPos;
            puppet::upsertPinKeyframe(pin, kf);
        } else {
            const QPointF p = baseEval.position + widgetDeltaToUv(QPointF(pos - m_pressPos), m_viewRect);
            kf.position = QPointF(std::clamp(p.x(), -0.25, 1.25), std::clamp(p.y(), -0.25, 1.25));
            puppet::upsertPinKeyframe(pin, kf);
        }

        m_previewRig = rig;
        puppet::previewRigOnTimeline(m_timeline, m_trackIndex, m_clipIndex, rig, tSec);
        emit previewChanged();
        return true;
    }

    if (!puppetpin::equal(currentPins(), m_previewPins)) { m_handle = -1; return false; }
    auto pins = m_dragPins;
    const QPointF p = pins[m_handle].deformedPos + widgetDeltaToUv(QPointF(pos - m_pressPos), m_viewRect);
    pins[m_handle].deformedPos = QPointF(std::clamp(p.x(), 0.0, 1.0), std::clamp(p.y(), 0.0, 1.0));
    m_previewPins = pins;
    m_timeline->previewClipPuppetPins(m_trackIndex, m_clipIndex, pins);
    emit previewChanged();
    return true;
}

bool PuppetPinTool::handleMouseRelease(const QPoint &pos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    if (button != Qt::LeftButton || m_handle < 0) return false;
    if (arapActive() && m_sketchRecording) {
        const int pinIdx = m_handle;
        const auto samples = m_sketchSamples;
        const double tSec = m_sketchStartSec;
        m_sketchRecording = false;
        m_sketchSamples.clear();
        m_handle = -1;
        m_previewApplied = false;

        auto finalRig = m_beforeRig;
        if (pinIdx >= 0 && pinIdx < finalRig.pins.size() && !samples.isEmpty()) {
            puppet::applySketchToPin(finalRig.pins[pinIdx], samples, m_sketchEpsilonUv,
                                     puppet::EasingKind::Linear, /*replaceInRange=*/true);
        }
        puppet::previewRigOnTimeline(m_timeline, m_trackIndex, m_clipIndex, m_beforeRig, tSec);
        puppet::commitRigToTimeline(m_timeline, m_trackIndex, m_clipIndex, finalRig,
                                    QStringLiteral("パペットスケッチ録画"));
        emit previewChanged();
        return true;
    }

    if (!handleMouseMove(pos, modifiers)) { m_handle = -1; return true; }

    if (arapActive()) {
        const double tSec = currentClipLocalSec();
        const auto finalRig = currentRig();
        m_handle = -1;
        m_previewApplied = false;
        puppet::previewRigOnTimeline(m_timeline, m_trackIndex, m_clipIndex, m_beforeRig, tSec);
        puppet::commitRigToTimeline(m_timeline, m_trackIndex, m_clipIndex, finalRig,
                                    QStringLiteral("パペットピン (ARAP)"));
        emit previewChanged();
        return true;
    }

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
    m_sketchRecording = false;
    m_sketchSamples.clear();
    if (arapActive()) {
        if (isEnabled() && m_previewApplied && currentRig() == m_previewRig)
            puppet::previewRigOnTimeline(m_timeline, m_trackIndex, m_clipIndex, m_beforeRig,
                                         currentClipLocalSec());
        m_previewApplied = false;
        emit previewChanged();
        return;
    }
    if (isEnabled() && m_previewApplied && puppetpin::equal(currentPins(), m_previewPins))
        m_timeline->previewClipPuppetPins(m_trackIndex, m_clipIndex, m_before);
    m_previewApplied = false;
    emit previewChanged();
}
