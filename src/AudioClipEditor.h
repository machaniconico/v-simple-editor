#pragma once

#include <QWidget>
#include <QList>

struct VolumeEnvelopePoint {
    qint64 timeMs = 0;  // クリップ内相対時刻
    double dB = 0.0;    // -60 .. +12 dB
};

class AudioClipEditor : public QWidget {
    Q_OBJECT
public:
    explicit AudioClipEditor(QWidget* parent = nullptr);
    ~AudioClipEditor() override = default;

    void setClipDuration(qint64 durationMs);  // 1ms 以上
    qint64 clipDuration() const;

    void setEnvelope(const QList<VolumeEnvelopePoint>& points);
    QList<VolumeEnvelopePoint> envelope() const;

    void clearEnvelope();  // 既定の 2 点 (0ms, 0dB) と (duration, 0dB) に戻す

    // 評価: 任意時刻 t での補間 dB (linear)。範囲外は端点を返す
    double evaluateAt(qint64 timeMs) const;

signals:
    void envelopeChanged(const QList<VolumeEnvelopePoint>& points);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;  // 空白ダブルクリックで点追加
    void contextMenuEvent(QContextMenuEvent* e) override; // 右クリックで「点を削除」

private:
    qint64 m_durationMs = 10000;
    QList<VolumeEnvelopePoint> m_points;
    int m_dragIndex = -1;

    // 座標変換
    QPointF pointToPixel(const VolumeEnvelopePoint& p) const;
    VolumeEnvelopePoint pixelToPoint(const QPointF& px) const;
    int hitTest(const QPointF& px, double hitRadiusPx = 8.0) const;  // 既存点インデックス or -1
    void sortPoints();
    void emitChanged();

    // 描画余白
    static constexpr int kMarginLeft   = 30;
    static constexpr int kMarginRight  = 10;
    static constexpr int kMarginTop    = 10;
    static constexpr int kMarginBottom = 10;

    static constexpr double kDbMin = -60.0;
    static constexpr double kDbMax =  12.0;
};
