#include "MeshEditTool.h"
#include "Timeline.h"
#include <QPainter>
#include <cmath>

MeshEditTool::MeshEditTool(Timeline *timeline, int trackIndex, int clipIndex, QObject *parent)
    : SurfaceTool(nullptr, parent), m_timeline(timeline),
      m_trackIndex(trackIndex), m_clipIndex(clipIndex)
{
    setEnabled(true);
}

bool MeshEditTool::isEnabled() const
{
    if (!SurfaceTool::isEnabled() || !m_timeline) return false;
    auto *track = m_timeline->videoTracks().value(m_trackIndex, nullptr);
    return track && !track->isLocked() && m_clipIndex >= 0
        && m_clipIndex < track->clips().size();
}

MeshGrid MeshEditTool::currentGrid() const
{
    if (!isEnabled()) return {};
    const MeshGrid grid = m_timeline->videoTracks()[m_trackIndex]->clips()[m_clipIndex].meshWarp;
    bool valid = grid.rows >= 2 && grid.cols >= 2 && grid.controlPoints.size() == grid.rows;
    for (const auto &row : grid.controlPoints) {
        valid &= row.size() == grid.cols;
        for (const auto &p : row) valid &= std::isfinite(p.x()) && std::isfinite(p.y());
    }
    return valid ? grid : WarpDistortion::createDefaultMesh(QSize(1, 1), 4, 4);
}

QPointF MeshEditTool::toWidget(const QPointF &uv) const
{
    return m_viewRect.topLeft() + QPointF(uv.x() * m_viewRect.width(), uv.y() * m_viewRect.height());
}

int MeshEditTool::hitTest(const QPoint &pos) const
{
    if (!isEnabled() || m_viewRect.isEmpty()) return -1;
    const MeshGrid grid = currentGrid();
    for (int r = 0; r < grid.rows; ++r)
        for (int c = 0; c < grid.cols; ++c) {
            const QPointF d = toWidget(grid.controlPoints[r][c]) - pos;
            if (d.x() * d.x() + d.y() * d.y() <= 36.0) return r * grid.cols + c;
        }
    return -1;
}

void MeshEditTool::paintOverlay(QPainter &painter, const QRectF &rect)
{
    setViewRect(rect);
    if (!isEnabled() || rect.isEmpty()) return;
    const MeshGrid grid = currentGrid();
    painter.save();
    painter.setPen(QPen(QColor(0, 200, 255, 220), 1));
    for (int r = 0; r < grid.rows; ++r)
        for (int c = 0; c < grid.cols; ++c) {
            const QPointF p = toWidget(grid.controlPoints[r][c]);
            if (c + 1 < grid.cols) painter.drawLine(p, toWidget(grid.controlPoints[r][c + 1]));
            if (r + 1 < grid.rows) painter.drawLine(p, toWidget(grid.controlPoints[r + 1][c]));
        }
    painter.setPen(QPen(Qt::white, 1));
    painter.setBrush(QColor(0, 150, 255, 220));
    for (const auto &row : grid.controlPoints)
        for (const auto &p : row) painter.drawEllipse(toWidget(p), 6.0, 6.0);
    painter.restore();
}

bool MeshEditTool::handleMousePress(const QPoint &pos, Qt::MouseButton button, Qt::KeyboardModifiers)
{
    if (button != Qt::LeftButton || m_handle >= 0) return false;
    const int handle = hitTest(pos);
    if (handle < 0) return false;
    m_before = m_timeline->videoTracks()[m_trackIndex]->clips()[m_clipIndex].meshWarp;
    m_dragGrid = currentGrid();
    m_previewApplied = false;
    m_pressPos = pos;
    m_handle = handle;
    return true;
}

bool MeshEditTool::handleMouseMove(const QPoint &pos, Qt::KeyboardModifiers)
{
    if (m_handle < 0 || !isEnabled() || m_viewRect.isEmpty()) return false;
    MeshGrid grid = m_dragGrid;
    grid.controlPoints[m_handle / grid.cols][m_handle % grid.cols] +=
        QPointF((pos.x() - m_pressPos.x()) / m_viewRect.width(),
                (pos.y() - m_pressPos.y()) / m_viewRect.height());
    m_previewGrid = grid;
    m_previewApplied = true;
    m_timeline->previewClipMeshWarp(m_trackIndex, m_clipIndex, grid);
    emit previewChanged();
    return true;
}

bool MeshEditTool::handleMouseRelease(const QPoint &pos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    if (button != Qt::LeftButton || m_handle < 0) return false;
    if (!isEnabled()) { m_handle = -1; return true; }
    handleMouseMove(pos, modifiers);
    const MeshGrid finalGrid = currentGrid();
    m_handle = -1;
    m_timeline->previewClipMeshWarp(m_trackIndex, m_clipIndex, m_before);
    // A click without displacement must not materialize the implicit identity mesh.
    if (finalGrid.controlPoints != m_dragGrid.controlPoints)
        m_timeline->setClipMeshWarp(m_trackIndex, m_clipIndex, finalGrid);
    emit previewChanged();
    return true;
}

void MeshEditTool::cancelDrag()
{
    if (m_handle < 0) return;
    m_handle = -1;
    // Undo/restore may already have replaced the clip before selection changes.
    // Only roll back the temporary value this gesture actually wrote.
    if (isEnabled() && m_previewApplied) {
        const MeshGrid grid = m_timeline->videoTracks()[m_trackIndex]->clips()[m_clipIndex].meshWarp;
        if (grid.rows == m_previewGrid.rows && grid.cols == m_previewGrid.cols
            && grid.controlPoints == m_previewGrid.controlPoints)
            m_timeline->previewClipMeshWarp(m_trackIndex, m_clipIndex, m_before);
    }
    m_previewApplied = false;
    emit previewChanged();
}
