#include "SequenceExport.h"
#include "TimelineFrameRenderer.h"
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <atomic>
#include <cmath>
#include <limits>

namespace sequenceexport {
namespace {
std::atomic<int> frameCount{0};
std::atomic<int> writeCallCount{0};
std::atomic<bool> enabled{true};
const QRegularExpression token(QStringLiteral("%d|%0[1-9][0-9]*d"));
}
Settings Settings::fromJson(const QJsonObject &json, const QString &fallbackFolder)
{
    Settings s;
    s.folder = json.value("sequenceFolder").toString(fallbackFolder);
    s.pattern = json.value("sequencePattern").toString(s.pattern);
    s.digits = json.value("sequenceDigits").toInt(s.digits);
    s.start = json.value("sequenceStart").toInt(s.start);
    s.keepAlpha = json.value("keepAlpha").toBool();
    return s;
}
QString validate(const Settings &s)
{
    const auto match = token.match(s.pattern);
    QString remainder = s.pattern;
    if (match.hasMatch()) remainder.remove(match.capturedStart(), match.capturedLength());
    if (s.folder.trimmed().isEmpty() || s.digits < 1 || s.digits > 12 || s.start < 0
        || !match.hasMatch() || remainder.contains('%') || s.pattern.contains('/')
        || s.pattern.contains('\\') || s.pattern.contains(':'))
        return QStringLiteral("出力フォルダ・番号の桁数・開始番号・パターン (%d または %06d) を確認してください");
    const auto suffix = QFileInfo(s.pattern).suffix().toLower().toLatin1();
    if (suffix != "png" && suffix != "tiff")
        return QStringLiteral("連番の形式は PNG または TIFF を指定してください");
    if (suffix == "tiff" && !QImageWriter::supportedImageFormats().contains("tiff"))
        return QStringLiteral("この環境では TIFF の書き出しに対応していません");
    return {};
}
QString fileName(const Settings &s, qint64 index)
{
    QString name = s.pattern;
    const auto match = token.match(name);
    if (!match.hasMatch()) return {};
    // The explicit digits setting is authoritative; the UI keeps the token in sync.
    name.replace(match.capturedStart(), match.capturedLength(),
                 QString::number(qint64(s.start) + index).rightJustified(s.digits, QLatin1Char('0')));
    return QDir(s.folder).filePath(name);
}
QString write(Timeline *timeline, const Settings &s, QSize size, double fps,
              qint64 startUs, qint64 frames, const std::function<bool()> &cancelled,
              const std::function<void(int)> &progress,
              const std::function<void(QImage &, qint64)> &prepare)
{
    ++writeCallCount;
    const QString error = validate(s);
    if (!error.isEmpty()) return error;
    if (!timeline || size.isEmpty() || !std::isfinite(fps) || fps <= 0 || frames <= 0
        || frames > std::numeric_limits<qint64>::max() - s.start)
        return QStringLiteral("連番の書き出し範囲または解像度が無効です");
    if (!QDir().mkpath(s.folder)) return QStringLiteral("出力フォルダを作成できません");
    const double step = 1000000.0 / fps;
    for (qint64 f = 0; f < frames; ++f) {
        if (cancelled && cancelled()) return QStringLiteral("連番の書き出しを中断しました");
        const qint64 usec = startUs + static_cast<qint64>(f * step);
        QImage frame = tlrender::renderFrameAt(timeline, usec, size, step);
        if (frame.isNull()) return QStringLiteral("フレーム %1 の描画に失敗しました").arg(f);
        if (prepare) prepare(frame, usec);
        if (!s.keepAlpha) {
            QImage opaque(size, QImage::Format_RGB888);
            opaque.fill(Qt::black);
            QPainter painter(&opaque);
            painter.drawImage(0, 0, frame);
            painter.end();
            frame = opaque;
        }
        const QString path = fileName(s, f);
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly)) return output.errorString();
        QImageWriter writer(&output, QFileInfo(s.pattern).suffix().toLower().toLatin1());
        if (!writer.write(frame)) return QStringLiteral("%1: %2").arg(path, writer.errorString());
        if (!output.commit()) return output.errorString();
        ++frameCount;
        if (progress) progress(static_cast<int>(100.0 * (f + 1) / frames));
    }
    return {};
}
void setEnabledForTest(bool value) { enabled = value; }
bool enabledForTest() { return enabled.load(); }
void resetFrameCountForTest() { frameCount = 0; writeCallCount = 0; }
int writeCallCountForTest() { return writeCallCount.load(); }
int frameCountForTest() { return frameCount.load(); }
}
