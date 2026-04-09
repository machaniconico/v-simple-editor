#pragma once

#include <QWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QCursor>
#include "TextManager.h"

// Interactive overlay widget for drag/resize/rotate text on preview
class TextInteractive : public QWidget
{
    Q_OBJECT

public:
    explicit TextInteractive(QWidget *parent = nullptr);

    void setOverlays(const QVector<EnhancedTextOverlay> &overlays);
    void setSelectedIndex(int index);
    int selectedIndex() const { return m_selectedIndex; }

    void setVideoRect(const QRect &rect) { m_videoRect = rect; update(); }

signals:
    void overlayMoved(int index, double x, double y);
    void overlayResized(int index, double scale);
    void overlayRotated(int index, double rotation);
    void overlaySelected(int index);
    void overlayDoubleClicked(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    enum Handle { None, Move, TopLeft, TopRight, BottomLeft, BottomRight, Rotate };

    Handle hitTest(const QPoint &pos, int overlayIndex) const;
    QRect overlayScreenRect(int index) const;
    QPoint rotateHandlePos(int index) const;

    QVector<EnhancedTextOverlay> m_overlays;
    int m_selectedIndex = -1;
    Handle m_activeHandle = None;
    QPoint m_dragStart;
    double m_dragStartX = 0, m_dragStartY = 0;
    double m_dragStartScale = 1.0;
    double m_dragStartRotation = 0.0;
    QRect m_videoRect;

    static constexpr int HANDLE_SIZE = 8;
    static constexpr int ROTATE_HANDLE_DISTANCE = 25;
};
