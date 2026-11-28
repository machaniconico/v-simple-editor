#pragma once

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QVector>

class Timeline;
struct ClipInfo;

namespace creatortools {

enum class CollagePreset {
    TwoUpHorizontal = 0, // Left / Right (V1 = left, V2 = right)
    TwoUpVertical   = 1, // Top / Bottom (V1 = top, V2 = bottom)
    ThreeUpMainLeft = 2, // Left half main (V1) + Top-right (V2) + Bottom-right (V3)
    FourUpGrid      = 3  // 2x2 Grid (V1..V4)
};

struct CollageResult {
    bool ok = false;
    int clipsUpdated = 0;
    QString summary;
};

CollageResult applySplitScreenCollage(Timeline *timeline,
                                      CollagePreset preset,
                                      double timelineSec = -1.0);

enum class VisualizerStyle {
    SpectrumBars = 0,
    MirrorBars   = 1,
    CircularRing = 2,
    WaveformLine = 3
};

VisualizerStyle visualizerStyleFromString(const QString &name);
QString visualizerStyleToString(VisualizerStyle style);
QVector<double> computeSpectrumBands(const QVector<float> &monoPcm, int bandCount = 32);

QImage renderAudioVisualizerFrame(const QJsonObject &params,
                                  double clipLocalSec,
                                  QSize size,
                                  const Timeline *timeline = nullptr);

QString buildVisualizerGeneratorUri(VisualizerStyle style,
                                    const QColor &primaryColor = QColor(76, 201, 240),
                                    const QColor &secondaryColor = QColor(247, 37, 133),
                                    int barCount = 32,
                                    double fps = 30.0,
                                    bool transparentBg = true);

struct BeatCutResult {
    bool ok = false;
    double detectedBpm = 0.0;
    int beatCount = 0;
    int cutsApplied = 0;
    QString summary;
};

BeatCutResult autoCutClipToBeats(Timeline *timeline,
                                 int videoTrackIdx,
                                 int videoClipIdx,
                                 const QVector<double> &beatTimesSec,
                                 int everyNBeats = 1,
                                 double minSegmentSec = 0.15);

BeatCutResult detectBeatsAndAutoCut(Timeline *timeline,
                                    int videoTrackIdx,
                                    int videoClipIdx,
                                    int audioTrackIdx = 0,
                                    int audioClipIdx = 0,
                                    int everyNBeats = 1,
                                    double sensitivity = 1.3);

} // namespace creatortools
