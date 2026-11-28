#pragma once
#include "CaptionTrack.h"
#include <QString>
#include <QList>

namespace subtitle {

// Missing keys preserve the existing burn-in selection.
enum class ExportMode { None = 0, BurnIn = 1, MovText = 2, Both = 3 };
inline bool embeds(ExportMode mode) { return mode == ExportMode::MovText || mode == ExportMode::Both; }
inline bool burns(ExportMode mode) { return mode == ExportMode::BurnIn || mode == ExportMode::Both; }
inline bool supportsMovText(const QString &container) {
    return container.compare("mp4", Qt::CaseInsensitive) == 0
        || container.compare("mov", Qt::CaseInsensitive) == 0;
}

struct ImportResult {
    QList<caption::Clip> clips;
    bool    success = false;
    QString error;
};

ImportResult importSrt(const QString& path);
ImportResult importVtt(const QString& path);

bool exportSrt(const QString& path, const QList<caption::Clip>& clips);
bool exportVtt(const QString& path, const QList<caption::Clip>& clips);

// "HH:MM:SS,mmm" (SRT) または "HH:MM:SS.mmm" (VTT)
QString formatSrtTimestamp(qint64 ms);
QString formatVttTimestamp(qint64 ms);
qint64  parseSrtTimestamp(const QString& s);  // 失敗時 -1
qint64  parseVttTimestamp(const QString& s);  // 失敗時 -1

} // namespace subtitle
