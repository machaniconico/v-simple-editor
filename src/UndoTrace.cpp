#include "UndoTrace.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>
#include <QtGlobal>

namespace {

bool traceEnabled()
{
    static const bool cached = qEnvironmentVariableIntValue("VEDITOR_UNDO_TRACE") != 0;
    return cached;
}

QElapsedTimer& traceTimer()
{
    static QElapsedTimer timer;
    static const bool started = [] {
        timer.start();
        return true;
    }();
    Q_UNUSED(started);
    return timer;
}

QString tracePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (dir.isEmpty())
        dir = qEnvironmentVariable("TEMP");
    return dir + QStringLiteral("/veditor_undo_trace.log");
}

QMutex& traceMutex()
{
    static QMutex mutex;
    return mutex;
}

} // namespace

namespace undotrace {

void log(const char* node)
{
    if (!traceEnabled())
        return;

    QMutexLocker locker(&traceMutex());
    QFile file(tracePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    const quintptr tid = reinterpret_cast<quintptr>(QThread::currentThreadId());
    QByteArray line = QByteArray::number(traceTimer().elapsed());
    line += ' ';
    line += QByteArray::number(tid);
    line += ' ';
    line += node ? node : "";
    line += '\n';
    file.write(line);
    file.flush();
}

} // namespace undotrace
