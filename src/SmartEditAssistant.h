#pragma once

#include <QObject>
#include <QVector>
#include <QString>
#include <QtGlobal>

namespace smartedit {

struct AnalysisConfig {
    double silenceThreshold = 0.02;
    qint64 minSilenceMs = 500;
    double sceneChangeThreshold = 0.3;
    qint64 mergeAdjacentMs = 200;
    qint64 minClipDurationMs = 1000;
};

struct CutSuggestion {
    enum Reason {
        Silence,
        SceneChange,
        Combined
    };

    qint64 startMs = 0;
    qint64 endMs = 0;
    Reason reason = Silence;
    double confidence = 0.0;
};

class Assistant : public QObject
{
    Q_OBJECT

public:
    explicit Assistant(QObject *parent = nullptr);

    QVector<CutSuggestion> analyze(const QString &videoPath,
                                   const AnalysisConfig &config = AnalysisConfig());

signals:
    void analysisProgress(int percent);
    void analysisFinished(const QVector<smartedit::CutSuggestion> &suggestions);
    void analysisFailed(const QString &error);
};

} // namespace smartedit

Q_DECLARE_METATYPE(smartedit::CutSuggestion)
Q_DECLARE_METATYPE(QVector<smartedit::CutSuggestion>)
