#pragma once
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <functional>

class Timeline;
namespace sequenceexport {
struct Settings {
    QString folder;
    QString pattern = QStringLiteral("name_%06d.png");
    int digits = 6;
    int start = 0;
    bool keepAlpha = false;
    bool exrLinear = false; // EXR only: scene-linear instead of display-referred values
    static Settings fromJson(const QJsonObject &json, const QString &fallbackFolder);
};
QString validate(const Settings &settings);
QString fileName(const Settings &settings, qint64 index);
// Returns an error on failure/cancellation. Completed files remain on disk.
QString write(Timeline *timeline, const Settings &settings, QSize size,
              double fps, qint64 startUs, qint64 frames,
              const std::function<bool()> &cancelled = {},
              const std::function<void(int)> &progress = {},
              const std::function<void(QImage &, qint64)> &prepare = {});
// Disables the new queue branch for baseline-render regression tests only.
void setEnabledForTest(bool enabled);
bool enabledForTest();
void resetFrameCountForTest();
int writeCallCountForTest();
int frameCountForTest();
}
