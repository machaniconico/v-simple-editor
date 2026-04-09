#pragma once

#include <QObject>
#include <QVector>
#include <QString>

// Type of detected highlight
enum class HighlightType {
    LoudMoment,
    SceneChange,
    MotionPeak,
    SpeechSegment,
    Combined
};

// A single highlight segment
struct Highlight {
    double startTime = 0.0;   // seconds
    double endTime = 0.0;     // seconds
    double score = 0.0;       // 0.0-1.0 (combined relevance)
    HighlightType type = HighlightType::Combined;
    QString description;

    double duration() const { return endTime - startTime; }
    bool overlaps(const Highlight &other) const {
        return startTime < other.endTime && endTime > other.startTime;
    }
};

// Time-indexed score for a single analysis metric
struct ScoredSegment {
    double time = 0.0;        // seconds (center of window)
    double score = 0.0;       // raw score (normalized later)
};

// Configuration for highlight detection
struct HighlightConfig {
    double minHighlightDuration = 3.0;    // minimum highlight length (seconds)
    double maxHighlightDuration = 30.0;   // maximum highlight length (seconds)
    int targetCount = 10;                  // how many highlights to extract

    // Weights for combining metrics (should sum to ~1.0)
    double audioWeight = 0.4;
    double motionWeight = 0.3;
    double sceneWeight = 0.3;

    // Audio analysis
    double loudnessThreshold = 0.7;       // relative threshold for "loud" moments (0-1)
    double audioWindowSize = 0.5;         // RMS window size in seconds

    // Motion analysis
    int motionSampleFps = 1;              // decode at this fps for motion analysis
};

class AIHighlight : public QObject
{
    Q_OBJECT

public:
    explicit AIHighlight(QObject *parent = nullptr);

    // Full analysis pipeline (async — results via analysisComplete signal)
    void analyze(const QString &filePath, const HighlightConfig &config = {});

    // Individual analysis passes (synchronous, for background thread use)
    static QVector<ScoredSegment> analyzeAudioEnergy(const QString &filePath,
                                                      const HighlightConfig &config = {});
    static QVector<ScoredSegment> analyzeMotionActivity(const QString &filePath,
                                                         const HighlightConfig &config = {});
    static QVector<ScoredSegment> analyzeSceneChanges(const QString &filePath,
                                                       const HighlightConfig &config = {});

    // Combine individual metric scores into unified highlights
    static QVector<Highlight> combineScores(const QVector<ScoredSegment> &audioScores,
                                             const QVector<ScoredSegment> &motionScores,
                                             const QVector<ScoredSegment> &sceneScores,
                                             const HighlightConfig &config = {});

    // Pick best non-overlapping highlights
    static QVector<Highlight> selectTopHighlights(const QVector<Highlight> &allHighlights,
                                                   const HighlightConfig &config = {});

    // Export highlights as a concatenated video reel (async — result via exportComplete)
    void exportHighlightReel(const QString &inputPath, const QString &outputPath,
                             const QVector<Highlight> &highlights);

    // Format highlights as human-readable timestamp list
    static QString exportTimestamps(const QVector<Highlight> &highlights);

signals:
    void progressChanged(int percent);
    void analysisComplete(const QVector<Highlight> &highlights);
    void exportComplete(bool success, const QString &message);

private:
    // Normalize scores in a segment list to 0.0-1.0 range
    static void normalizeScores(QVector<ScoredSegment> &segments);

    // Build highlights from a time-aligned combined score array
    static QVector<Highlight> buildHighlightsFromScores(
        const QVector<ScoredSegment> &combinedScores,
        const HighlightConfig &config);
};
