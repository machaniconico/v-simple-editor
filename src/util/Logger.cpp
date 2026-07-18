#include "util/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

// PRD-SPLIT-MAIN-1: g_logFilePath, g_logMutex, defaultLogPath(), and
// writeLogLine() are placed outside the anonymous namespace so that
// src/selftests/SelftestRegistry.cpp can call writeLogLine() via an
// extern declaration without requiring it to be in a named namespace.
QString g_logFilePath;
QMutex  g_logMutex;

QString defaultLogPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                        + "/.veditor/logs";
    QDir().mkpath(dir);
    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    return dir + "/veditor_" + ts + ".log";
}

void writeLogLine(const QString &level, const QString &msg)
{
    QMutexLocker lock(&g_logMutex);
    QFile f(g_logFilePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
       << " [" << level << "] " << msg << "\n";
    ts.flush();
    f.close();
}
