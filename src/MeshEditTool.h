#pragma once

#include "SurfaceTool.h"
#include "WarpDistortion.h"
#include <QPointer>
#include <QString>

class Timeline;

class MeshEditTool : public SurfaceTool
{
    Q_OBJECT
public:
    MeshEditTool(Timeline *timeline, int trackIndex, int clipIndex,
                 QObject *parent = nullptr);
    void setViewRect(const QRectF &rect) { m_viewRect = rect; }
    int hitTest(const QPoint &pos) const;
    void cancelDrag();
    bool isEnabled() const override;
    void paintOverlay(QPainter &painter, const QRectF &rect) override;
    bool handleMousePress(const QPoint &, Qt::MouseButton, Qt::KeyboardModifiers) override;
    bool handleMouseMove(const QPoint &, Qt::KeyboardModifiers) override;
    bool handleMouseRelease(const QPoint &, Qt::MouseButton, Qt::KeyboardModifiers) override;

signals:
    void previewChanged();

private:
    MeshGrid currentGrid() const;
    QPointF toWidget(const QPointF &uv) const;
    QPointer<Timeline> m_timeline;
    int m_trackIndex;
    int m_clipIndex;
    QString m_targetFilePath;
    double m_targetInPoint = 0.0;
    QRectF m_viewRect;
    MeshGrid m_before;
    MeshGrid m_dragGrid;
    MeshGrid m_previewGrid;
    bool m_previewApplied = false;
    QPoint m_pressPos;
    int m_handle = -1;
};
