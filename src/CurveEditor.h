#pragma once

#include <QWidget>
#include <QVector>
#include <QPoint>
#include <QPointF>

class QComboBox;
class QPushButton;
class QPaintEvent;
class QMouseEvent;
class QResizeEvent;

// US-CG-1: RGB Curves editor (R / G / B / Luma master).
// Per-channel control points are stored in image space (x,y in [0,255])
// and interpolated with a Catmull-Rom spline to produce 256-entry LUTs.
// The host (ColorGradingPanel) re-emits curvesChanged() so MainWindow
// can forward the LUTs to GLPreview::setRgbCurves.
class CurveEditor : public QWidget
{
    Q_OBJECT

public:
    enum Channel {
        ChannelR = 0,
        ChannelG = 1,
        ChannelB = 2,
        ChannelLuma = 3,
        ChannelCount = 4
    };

    explicit CurveEditor(QWidget *parent = nullptr);

    // Returns 4 curves (R, G, B, Luma), each 256 samples in [0,255].
    QVector<QVector<int>> currentCurves() const;

    // Returns true if every channel is the default identity (only the
    // two endpoint control points). Useful for callers that want to
    // skip uploading an identity LUT to the GPU.
    bool isIdentity() const;

    // Persistence accessors for ColorGradingPanel JSON round-trip.
    // Points are in image-space (x,y in [0,255]).
    QVector<QVector<QPointF>> allPoints() const;
    void setAllPoints(const QVector<QVector<QPointF>> &pts);

signals:
    void curvesChanged(const QVector<QVector<int>> &curves);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onChannelChanged(int index);
    void onResetClicked();

private:
    // Per-channel control points (x,y in [0,255], sorted ascending by x).
    QVector<QPointF> m_points[ChannelCount];
    int m_activeChannel = ChannelR;
    int m_draggingPoint = -1;       // index into m_points[m_activeChannel]
    bool m_pointWasMoved = false;   // track whether drag actually moved
    QPointF m_dragStart;
    QComboBox *m_channelCombo = nullptr;
    QPushButton *m_resetButton = nullptr;

    // The drawing area — the inner 256x256 grid in widget pixels.
    QRect plotRect() const;

    // Convert between image-space (0..255) and widget-pixel coordinates.
    QPointF pointToPixel(const QPointF &p, const QRect &plot) const;
    QPointF pixelToPoint(const QPointF &px, const QRect &plot) const;

    // Hit-test a control point near a widget-pixel position; returns the
    // index in m_points[m_activeChannel] or -1.
    int hitTestPoint(const QPoint &widgetPos) const;

    // Sort the active channel's control points by x ascending and clamp
    // every coordinate to [0,255]. Endpoints (x=0 and x=255) keep their
    // x but their y can move freely.
    void sanitizeActivePoints();

    // Catmull-Rom evaluate over the active channel's control points,
    // sampling every integer x in [0,255] and writing results into
    // m_lutCache[m_activeChannel]. Endpoints are extended by reflecting
    // through the first/last point (standard "uniform Catmull-Rom" trick).
    void rebuildLutFor(int channel);
    void rebuildAllLuts();

    QVector<int> m_lutCache[ChannelCount];   // each = 256 ints in [0,255]

    // Default identity = two control points (0,0)..(255,255).
    static QVector<QPointF> identityPoints();
};
