#pragma once

#include "SurfaceTool.h"
#include "WarpDistortion.h"
#include <QPointer>
#include <QString>

class Timeline;

class PuppetPinTool : public SurfaceTool
{
    Q_OBJECT
public:
    PuppetPinTool(Timeline *timeline, int trackIndex, int clipIndex,
                 QObject *parent = nullptr);
    void setViewRect(const QRectF &rect) { m_viewRect = rect; }
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
    QVector<WarpPin> currentPins() const;
    QPointF toUv(const QPoint &pos) const;
    QPointF toWidget(const QPointF &uv) const;
    QPointer<Timeline> m_timeline;
    int m_trackIndex;
    int m_clipIndex;
    QString m_targetFilePath;
    double m_targetInPoint = 0.0;
    QRectF m_viewRect;
    QVector<WarpPin> m_before;
    QVector<WarpPin> m_dragPins;
    QVector<WarpPin> m_previewPins;
    bool m_previewApplied = false;
    QPoint m_pressPos;
    int m_handle = -1;
};
