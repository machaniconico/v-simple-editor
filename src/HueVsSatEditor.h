#pragma once

#include <QWidget>
#include <QVector>
#include <QPointF>

class QPaintEvent;
class QMouseEvent;
class QResizeEvent;
class QPushButton;

// US-CG-4: Hue vs Saturation curve (DaVinci Resolve color page parity).
// X axis = hue degrees [0..360]. Y axis = saturation multiplier [0..2]
// (1.0 = identity flat line). Drag control points to selectively desaturate
// or supersaturate specific hue ranges (skin tones, sky, foliage, etc.).
//
// Output: 256-entry float LUT keyed on the hue normalized to [0..1]; the
// host (ColorGradingPanel) re-emits hueVsSatChanged() so MainWindow can
// forward the LUT to GLPreview::setHueVsSatLut.
class HueVsSatEditor : public QWidget
{
    Q_OBJECT

public:
    explicit HueVsSatEditor(QWidget *parent = nullptr);

    // 256 floats, sat-multiplier per hue bin (0..255 maps to 0°..360°).
    QVector<float> currentLut() const;

    // True when every control point sits exactly on the 1.0 identity line.
    bool isIdentity() const;

signals:
    void hueVsSatChanged(const QVector<float> &lut);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onResetClicked();

private:
    // Control points stored in graph space:
    //   x in [0..360] (hue degrees), y in [0..2] (sat multiplier)
    // Sorted ascending by x; first point fixed at x=0, last at x=360.
    QVector<QPointF> m_points;
    int m_draggingPoint = -1;
    bool m_pointWasMoved = false;

    QPushButton *m_resetButton = nullptr;

    // Inner plot rectangle (rainbow strip area) in widget pixels.
    QRect plotRect() const;

    QPointF pointToPixel(const QPointF &p, const QRect &plot) const;
    QPointF pixelToPoint(const QPointF &px, const QRect &plot) const;

    int hitTestPoint(const QPoint &widgetPos) const;
    void sanitizePoints();
    void rebuildLut();
    static QVector<QPointF> identityPoints();

    QVector<float> m_lutCache; // 256 floats, sat multiplier per hue bin
};
