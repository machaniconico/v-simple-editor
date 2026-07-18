#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QProcess>
#include "TextManager.h"

struct SubtitleWord {
    QString text;
    double startTime = 0.0; // seconds
    double endTime = 0.0;
};

// Single subtitle segment from speech recognition
struct SubtitleSegment {
    double startTime = 0.0;   // seconds
    double endTime = 0.0;
    QString text;
    QString language;
    double confidence = 0.0;  // 0.0-1.0
    QVector<SubtitleWord> words; // optional per-word timing (empty = segment-level only)
};

// Whisper configuration
struct WhisperConfig {
    QString modelPath;                 // path to whisper model file
    QString language = "auto";         // language code or "auto" for detection
    bool translateToEnglish = false;   // translate output to English
    int maxSegmentLength = 0;          // max chars per segment (0 = no limit)
    bool wordTimestamps = false;       // per-word timing
};

class SubtitleGenerator : public QObject
{
    Q_OBJECT

public:
    explicit SubtitleGenerator(QObject *parent = nullptr);

    // Generate subtitles from video (extracts audio first, then runs whisper)
    void generate(const QString &videoFilePath, const WhisperConfig &config);

    // Generate subtitles directly from audio file
    void generateFromAudio(const QString &audioFilePath, const WhisperConfig &config);

    // Extract audio from video to 16kHz mono WAV (whisper format)
    static bool extractAudio(const QString &videoPath, const QString &outputWavPath);

    // Parse whisper JSON output into segments
    static QVector<SubtitleSegment> parseWhisperOutput(const QString &jsonPath);

    // Export to SRT subtitle file
    static bool exportSRT(const QVector<SubtitleSegment> &segments, const QString &outputPath);

    // Export to WebVTT subtitle file
    static bool exportVTT(const QVector<SubtitleSegment> &segments, const QString &outputPath);

    // Convert segments to text overlays for the video editor
    static QVector<EnhancedTextOverlay> toTextOverlays(const QVector<SubtitleSegment> &segments);

    // Check if whisper CLI is available on the system
    static bool isWhisperAvailable();

    // List available whisper models
    static QStringList availableModels();

signals:
    void progressChanged(int percent);
    void generationComplete(const QVector<SubtitleSegment> &segments);
    void errorOccurred(const QString &message);

private:
    void runWhisper(const QString &audioPath, const WhisperConfig &config);
    static QString findWhisperBinary();
    static QString formatTimeSRT(double seconds);
    static QString formatTimeVTT(double seconds);

    QProcess *m_process = nullptr;
    QString m_tempDir;
};
