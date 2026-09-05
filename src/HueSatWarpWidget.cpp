#include "HueSatWarpWidget.h"
#include <QPainter>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QLineF>
#include <algorithm>
#include <cmath>

namespace { constexpr double pi = 3.14159265358979323846; }
HueSatWarpWidget::HueSatWarpWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(240, 240);
    setToolTip(tr("ノードをドラッグして色相・彩度を調整。ダブルクリックでノードをリセット。"));
}
void HueSatWarpWidget::setValue(const HueSatWarp &value) { m_value = value; update(); }
double HueSatWarpWidget::baseRadius(int ring) const
{
    // Display the achromatic ring with a small radius so its 12 nodes remain selectable.
    return (std::min(width(), height()) - 24.0) * 0.25 * (ring + 1) / 3.0;
}
QPointF HueSatWarpWidget::nodePosition(int ring, int hue) const
{
    const double angle = (hue * 30.0 + m_value.hueShiftDeg[ring][hue]) * pi / 180.0;
    const double radius = baseRadius(ring) * m_value.satScale[ring][hue];
    return QPointF(width()/2.0 + std::cos(angle)*radius, height()/2.0 - std::sin(angle)*radius);
}
void HueSatWarpWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF center(width()/2.0, height()/2.0);
    const double radius = baseRadius(2)*2.0;
    for (int h=0; h<360; ++h) {
        p.setPen(QPen(QColor::fromHsv(h, 220, 220), 4));
        p.drawArc(QRectF(center.x()-radius, center.y()-radius, radius*2, radius*2), h*16, 16);
    }
    p.setPen(QPen(palette().mid().color(), 1));
    for (int r=0; r<3; ++r)
        for (int h=0; h<12; ++h) {
            p.drawLine(nodePosition(r,h), nodePosition(r,(h+1)%12));
            if (r<2) p.drawLine(nodePosition(r,h),nodePosition(r+1,h));
        }
    for (int r=0; r<3; ++r)
        for (int h=0; h<12; ++h) {
            p.setBrush(QColor::fromHsv(h*30, 100+r*70, 240));
            p.setPen(QPen(palette().text().color(), m_dragNode==r*12+h ? 2 : 1));
            p.drawEllipse(nodePosition(r,h), 5, 5);
        }
}
int HueSatWarpWidget::hitNode(const QPointF &point) const
{
    int result=-1; double distance=10;
    for (int r=0; r<3; ++r)
        for (int h=0; h<12; ++h) {
            const double d=QLineF(point,nodePosition(r,h)).length();
            if (d<distance) { distance=d; result=r*12+h; }
        }
    return result;
}
void HueSatWarpWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button()==Qt::LeftButton) m_dragNode=hitNode(event->position());
}
void HueSatWarpWidget::drag(const QPointF &point)
{
    if (m_dragNode<0) return;
    const int r=m_dragNode/12, h=m_dragNode%12;
    const QPointF d=point-QPointF(width()/2.0,height()/2.0);
    double shift=std::atan2(-d.y(),d.x())*180.0/pi-h*30.0;
    shift=std::fmod(shift+540.0,360.0)-180.0;
    m_value.hueShiftDeg[r][h]=static_cast<float>(std::clamp(shift,-60.0,60.0));
    m_value.satScale[r][h]=static_cast<float>(std::clamp(std::hypot(d.x(),d.y())/baseRadius(r),0.0,2.0));
    update(); emit valueChanged();
}
void HueSatWarpWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) drag(event->position());
}
void HueSatWarpWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button()!=Qt::LeftButton || m_dragNode<0) return;
    m_dragNode=-1; update(); emit editingFinished();
}
void HueSatWarpWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button()!=Qt::LeftButton) return;
    const int node=hitNode(event->position());
    if (node<0) return;
    m_dragNode=-1;
    m_value.hueShiftDeg[node/12][node%12]=0;
    m_value.satScale[node/12][node%12]=1;
    update(); emit valueChanged(); emit editingFinished();
}
void HueSatWarpWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *reset=menu.addAction(tr("すべてリセット"));
    if (menu.exec(event->globalPos())==reset) {
        m_value=HueSatWarp{}; update(); emit valueChanged(); emit editingFinished();
    }
}
