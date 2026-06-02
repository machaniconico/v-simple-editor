#pragma once

#include <QObject>
#include <QVector>
#include <QString>
#include <cmath>

#include "WaveformGenerator.h"

namespace multicam {

struct CamSource {
    QString filePath;
    QString label;
    double  offsetMs = 0.0;
};

struct AngleCut {
    double timeMs   = 0.0;
    int    camIndex = 0;
};

class MultiCamSync : public QObject
{
    Q_OBJECT

public:
    explicit MultiCamSync(QObject *parent = nullptr);

    void setSources(const QVector<CamSource> &sources);
    QVector<CamSource> sources() const;

    // Estimate each camera's offsetMs by audio cross-correlation against
    // source[0] (the reference). source[0].offsetMs is forced to 0.
    void syncByAudio();

    // Pure, testable normalized cross-correlation core.
    // Returns the lag (in ms, can be negative) that best aligns
    // otherEnv onto refEnv. Returns 0 if either envelope is empty.
    static double estimateOffsetMs(const QVector<float> &refEnv,
                                   const QVector<float> &otherEnv,
                                   double envHopMs);

    // Pure helper for dialog/selftest use. Angle 0 is the reference and always
    // returns 0; empty/unusable envelopes fall back to 0 for that angle.
    static QVector<qint64> computeAngleOffsetsUs(
        const QVector<QVector<float>> &angleEnvelopes,
        double envHopMs);

    void addAngleCut(double timeMs, int camIndex);

    // Simple text EDL, one line per cut sorted by time:
    //   {index}  CAM{camIndex}  {timeMs}ms  {filePath}
    QString exportSwitchedEdl() const;

signals:
    void syncProgress(int percent);
    void syncFinished();

private:
    QVector<CamSource> m_sources;
    QVector<AngleCut>  m_cuts;
};

} // namespace multicam
