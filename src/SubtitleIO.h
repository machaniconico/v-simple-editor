#pragma once
#include "CaptionTrack.h"
#include <QString>
#include <QList>

namespace subtitle {

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
