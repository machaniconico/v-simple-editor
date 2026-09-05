#pragma once
#include <QWidget>
#include "VideoEffect.h"

class HueSatWarpWidget : public QWidget
{
    Q_OBJECT
public:
    explicit HueSatWarpWidget(QWidget *parent = nullptr);
    void setValue(const HueSatWarp &value);
    HueSatWarp value() const { return m_value; }
signals:
    void valueChanged();
    void editingFinished();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
private:
    QPointF nodePosition(int ring, int hue) const;
    double baseRadius(int ring) const;
    int hitNode(const QPointF &point) const;
    void drag(const QPointF &point);
    HueSatWarp m_value;
    int m_dragNode = -1;
};
