#pragma once

#include <QString>
#include <QMutex>

extern QString g_logFilePath;
extern QMutex g_logMutex;

QString defaultLogPath();
void writeLogLine(const QString &level, const QString &msg);
