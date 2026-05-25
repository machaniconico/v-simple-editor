#include "util/CrashHandler.h"

#include "util/Logger.h"

#include <QDebug>
#include <QString>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

// ──────────────────────────────────────────────────────────────────────────
// Lightweight file-backed logger + unhandled-exception reporter.
//
// Goal: when the app crashes, we want a log file we can read to understand
// what was happening just before the crash. Everything here is best-effort —
// it should never throw or hold locks that could deadlock on shutdown.
// ──────────────────────────────────────────────────────────────────────────

namespace {

static void qtMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    const char *level = "INFO";
    switch (type) {
        case QtDebugMsg:    level = "DEBUG"; break;
        case QtInfoMsg:     level = "INFO";  break;
        case QtWarningMsg:  level = "WARN";  break;
        case QtCriticalMsg: level = "CRIT";  break;
        case QtFatalMsg:    level = "FATAL"; break;
    }
    QString full = msg;
    if (ctx.file && *ctx.file) {
        full += QString(" (%1:%2)").arg(ctx.file).arg(ctx.line);
    }
    writeLogLine(level, full);

    // Also emit to stderr so console users see it.
    QTextStream(stderr) << "[" << level << "] " << msg << "\n";
}

#ifdef Q_OS_WIN
static LONG WINAPI crashHandler(EXCEPTION_POINTERS *info)
{
    if (!info) return EXCEPTION_EXECUTE_HANDLER;

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const void *addr = info->ExceptionRecord->ExceptionAddress;

    QString msg = QString("UNHANDLED EXCEPTION code=0x%1 addr=0x%2")
        .arg(QString::number(code, 16))
        .arg(quint64(addr), 0, 16);
    writeLogLine("CRASH", msg);

    // Capture stack frames.
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);

    void *frames[64];
    USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);

    char symbolBuf[sizeof(SYMBOL_INFO) + 256] = {0};
    SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuf);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    QString stack;
    for (USHORT i = 0; i < count; ++i) {
        DWORD64 disp = 0;
        if (SymFromAddr(process, DWORD64(frames[i]), &disp, symbol)) {
            stack += QString("  #%1 %2 + 0x%3 @ 0x%4\n")
                .arg(i)
                .arg(QString::fromLocal8Bit(symbol->Name))
                .arg(disp, 0, 16)
                .arg(DWORD64(frames[i]), 0, 16);
        } else {
            stack += QString("  #%1 ?? @ 0x%2\n")
                .arg(i).arg(DWORD64(frames[i]), 0, 16);
        }
    }
    writeLogLine("CRASH", "STACK:\n" + stack);
    SymCleanup(process);

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif // Q_OS_WIN

} // anonymous namespace (qtMessageHandler + crashHandler only)

void installCrashHandling()
{
    qInstallMessageHandler(qtMessageHandler);
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(crashHandler);
#endif
}
