#pragma once

#include <QWidget>

class KeyframeTrack;
class QMouseEvent;
class QPaintEvent;
class QPoint;

namespace effectctrl {

class EffectKeyframeNavBar : public QWidget
{
    Q_OBJECT

public:
    explicit EffectKeyframeNavBar(QWidget *parent = nullptr);

    void setTrack(KeyframeTrack *track, double clipDurationSeconds, double playheadSeconds);
    void setPlayhead(double seconds);

signals:
    void trackChanged();
    void selectedKeyframeChanged(int kfIndex);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    int hitTestKeyframe(const QPoint &pos) const;
    int timeToPixel(double seconds) const;
    double pixelToTime(int x) const;
    void setSelectedIndex(int index);
    void showDiamondContextMenu(int index, const QPoint &globalPos);
    void showEmptyContextMenu(double timeSeconds, const QPoint &globalPos);

    KeyframeTrack *m_track = nullptr;
    double m_clipDurationSeconds = 0.0;
    double m_playheadSeconds = 0.0;
    int m_selectedKeyframe = -1;
    bool m_dragging = false;
    bool m_dragChanged = false;
};

} // namespace effectctrl
