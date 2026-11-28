#pragma once

#include "PuppetEngine.h"
#include "SurfaceTool.h"
#include "WarpDistortion.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QString>
#include <functional>

class Timeline;

class PuppetPinTool : public SurfaceTool
{
    Q_OBJECT
public:
    PuppetPinTool(Timeline *timeline, int trackIndex, int clipIndex,
                 QObject *parent = nullptr);
    void setViewRect(const QRectF &rect) { m_viewRect = rect; }
    void setUseArapMode(bool enabled) { m_useArapMode = enabled; }
    bool useArapMode() const { return m_useArapMode; }
    void setToolMode(puppet::PinType mode) { m_toolMode = mode; }
    puppet::PinType toolMode() const { return m_toolMode; }
    void setTimeProvider(std::function<double()> provider) { m_timeProvider = std::move(provider); }

    // Wave 32: Puppet Sketch (Ctrl+Drag real-time trajectory recording + RDP simplification)
    void setSketchSpeed(double speed) { m_sketchSpeed = std::clamp(speed, 0.1, 4.0); }
    void setSketchEpsilonUv(double eps) { m_sketchEpsilonUv = std::clamp(eps, 0.0005, 0.05); }
    bool isSketchRecording() const { return m_sketchRecording; }
    int sketchSampleCount() const { return m_sketchSamples.size(); }
    bool recordSketchSampleAt(const QPoint &pos, double clipLocalSec);

    int hitTestCorner(const QPoint &pos, const QRectF &rect) const override;
    void cancelDrag();
    bool isEnabled() const override;
    void paintOverlay(QPainter &painter, const QRectF &rect) override;
    bool handleMousePress(const QPoint &, Qt::MouseButton, Qt::KeyboardModifiers) override;
    bool handleMouseMove(const QPoint &, Qt::KeyboardModifiers) override;
    bool handleMouseRelease(const QPoint &, Qt::MouseButton, Qt::KeyboardModifiers) override;

signals:
    void previewChanged();

private:
    bool arapActive() const;
    double currentClipLocalSec() const;
    QString currentClipUid() const;
    puppet::PuppetRig currentRig() const;
    QVector<WarpPin> currentPins() const;
    QPointF toUv(const QPoint &pos) const;
    QPointF toWidget(const QPointF &uv) const;

    QPointer<Timeline> m_timeline;
    int m_trackIndex;
    int m_clipIndex;
    QString m_targetFilePath;
    double m_targetInPoint = 0.0;
    QRectF m_viewRect;

    // Legacy static pin drag state (kept for backward-compatible unit tests)
    QVector<WarpPin> m_before;
    QVector<WarpPin> m_dragPins;
    QVector<WarpPin> m_previewPins;

    // Wave 31 ARAP animated rig state
    bool m_useArapMode = false;
    puppet::PinType m_toolMode = puppet::PinType::Position;
    std::function<double()> m_timeProvider;
    puppet::PuppetRig m_beforeRig;
    puppet::PuppetRig m_dragRig;
    puppet::PuppetRig m_previewRig;
    double m_dragStartRotDeg = 0.0;

    // Wave 32: Puppet Sketch recording state
    bool m_sketchRecording = false;
    double m_sketchStartSec = 0.0;
    double m_sketchSpeed = 1.0;
    double m_sketchEpsilonUv = 0.004;
    QElapsedTimer m_sketchTimer;
    QVector<puppet::SketchSample> m_sketchSamples;

    bool m_previewApplied = false;
    QPoint m_pressPos;
    int m_handle = -1;
};
