#pragma once
#include <QString>
#include <QList>
#include <QSharedPointer>
#include <QStringList>

namespace speech {

struct Segment {
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString text;
    double confidence = 1.0;
};

struct RecognizeParams {
    QString audioPath;        // WAV/MP3/AAC 等のパス
    QString language;         // "ja", "en", "auto" 等 (ISO 639-1 風)
    QString modelId;          // "whisper-base", "whisper-small", "stub" 等
    qint64 maxDurationMs = 0; // 0 = 制限なし
};

struct RecognizeResult {
    QList<Segment> segments;
    bool success = false;
    QString error;
    QString detectedLanguage;   // recognizer が判定した言語 (auto モードで参照)
    qint64 processingTimeMs = 0;
};

// Recognizer 抽象 (plugin インターフェース)
class Recognizer {
public:
    virtual ~Recognizer() = default;
    virtual QString name() const = 0;                          // "Stub" / "Whisper.cpp CLI" / etc
    virtual bool isAvailable() const = 0;                      // 実行時利用可能か (CLI 存在 / ライブラリロード)
    virtual bool supportsLanguage(const QString& lang) const = 0;
    virtual QStringList supportedLanguages() const = 0;        // ISO 639-1 リスト ("ja", "en", "auto" 等)
    virtual RecognizeResult recognize(const RecognizeParams& params) = 0;
};

// 組み込み実装 1: Stub (テスト + デフォルト)。
// 与えられた audioPath を読まず、決定論的な dummy segments を返す。
class StubRecognizer : public Recognizer {
public:
    QString name() const override;
    bool isAvailable() const override { return true; }
    bool supportsLanguage(const QString& lang) const override;
    QStringList supportedLanguages() const override;
    RecognizeResult recognize(const RecognizeParams& params) override;
};

// 組み込み実装 2: Whisper.cpp CLI ラッパー。
// PATH 上の `whisper-cli` (Whisper.cpp 公式 binary) を QProcess で呼び出し JSON parse。
// バイナリが存在しなければ isAvailable=false / recognize success=false。
class WhisperCliRecognizer : public Recognizer {
public:
    QString name() const override { return QStringLiteral("Whisper.cpp CLI"); }
    bool isAvailable() const override;
    bool supportsLanguage(const QString&) const override { return true; }
    QStringList supportedLanguages() const override;
    RecognizeResult recognize(const RecognizeParams& params) override;

    // CLI バイナリパス上書き (テスト用)
    void setCliPath(const QString& path) { m_cliPath = path; }
private:
    QString m_cliPath = QStringLiteral("whisper-cli");
};

// 利用可能な recognizer のリストを返す (Stub + CLI が available なら両方)
QList<QSharedPointer<Recognizer>> availableRecognizers();

// 名前で recognizer を取得 (find or default to Stub)
QSharedPointer<Recognizer> recognizerByName(const QString& name);

} // namespace speech
