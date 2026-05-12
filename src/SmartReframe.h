#pragma once

#include <QObject>

#include <QImage>
#include <QJsonObject>
#include <QRectF>
#include <QSize>
#include <QVector>

class SmartReframe : public QObject
{
    Q_OBJECT

public:
    explicit SmartReframe(QObject *parent = nullptr);

    void setTargetAspect(double w, double h);
    void setSourceSize(QSize size);
    void setSmoothness(double smoothness);
    void setMotionWeight(double motionWeight);
    void setSaliencyMassFraction(double saliencyMassFraction);

    void analyzeFrame(double timeSec, const QImage &frame);
    void finalizeAnalysis();

    QRectF cropRectAt(double timeSec) const;
    QImage applyReframe(const QImage &frame, double timeSec, QSize outputSize) const;

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &json);

private:
    struct TrackKey
    {
        double timeSec = 0.0;
        QRectF rect;
    };

    static double clamp01(double value);
    static QRectF interpolateRect(const QRectF &a, const QRectF &b, double t);

    QRectF defaultCropRect() const;
    QRectF solveCropRect(const QRectF &subjectBounds, const QPointF &subjectCentroid) const;
    QRectF interpolateTrackRect(const QVector<TrackKey> &track, double timeSec) const;
    void rebuildSmoothedTrack();

    double m_targetAspectW = 1.0;
    double m_targetAspectH = 1.0;
    QSize m_sourceSize;
    double m_smoothness = 0.7;
    double m_motionWeight = 0.0;
    double m_saliencyMassFraction = 0.6;

    QVector<double> m_previousLuma;
    QSize m_previousMapSize;

    QVector<TrackKey> m_rawTrack;
    QVector<TrackKey> m_smoothedTrack;
    bool m_analysisDirty = false;
};
