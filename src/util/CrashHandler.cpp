#include "util/CrashHandler.h"

#include "util/Logger.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTextStream>
#include <QVector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
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
struct LoadedModule
{
    DWORD64 base = 0;
    DWORD64 size = 0;
    QString name;
    QString path;
};

static QString executableDirectory()
{
    wchar_t path[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0)
        return {};

    const QString executablePath = QString::fromWCharArray(path, int(length));
    const int slash = qMax(executablePath.lastIndexOf('/'), executablePath.lastIndexOf('\\'));
    return slash >= 0 ? executablePath.left(slash) : QString();
}

static QString moduleNameFromPath(const QString &path)
{
    const int slash = qMax(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return slash >= 0 ? path.mid(slash + 1) : path;
}

static QVector<LoadedModule> enumerateLoadedModules(HANDLE process)
{
    QVector<LoadedModule> result;
    QVector<HMODULE> handles(256);
    DWORD bytesNeeded = 0;
    for (;;) {
        DWORD bytesReturned = 0;
        const DWORD bufferSize = DWORD(handles.size() * sizeof(HMODULE));
        if (!EnumProcessModules(process, handles.data(),
                                bufferSize, &bytesReturned)) {
            if (bytesReturned > bufferSize) {
                handles.resize(int(bytesReturned / sizeof(HMODULE)) + 16);
                continue;
            }
            return result;
        }
        if (bytesReturned <= bufferSize) {
            bytesNeeded = bytesReturned;
            break;
        }
        handles.resize(int(bytesReturned / sizeof(HMODULE)) + 16);
    }

    const int moduleCount = int(bytesNeeded / sizeof(HMODULE));
    result.reserve(moduleCount);
    for (int i = 0; i < moduleCount; ++i) {
        MODULEINFO moduleInfo = {};
        if (!GetModuleInformation(process, handles[i], &moduleInfo, sizeof(moduleInfo)))
            continue;

        wchar_t path[32768] = {};
        const DWORD pathLength = GetModuleFileNameExW(
            process, handles[i], path, ARRAYSIZE(path));
        const QString modulePath = pathLength > 0
            ? QString::fromWCharArray(path, int(pathLength))
            : QString();

        QString moduleName = moduleNameFromPath(modulePath);
        if (moduleName.isEmpty()) {
            wchar_t baseName[MAX_PATH] = {};
            const DWORD baseNameLength = GetModuleBaseNameW(
                process, handles[i], baseName, ARRAYSIZE(baseName));
            if (baseNameLength > 0)
                moduleName = QString::fromWCharArray(baseName, int(baseNameLength));
        }

        LoadedModule loaded;
        loaded.base = DWORD64(reinterpret_cast<quintptr>(moduleInfo.lpBaseOfDll));
        loaded.size = DWORD64(moduleInfo.SizeOfImage);
        loaded.name = moduleName.isEmpty() ? QStringLiteral("??") : moduleName;
        loaded.path = modulePath;
        result.push_back(loaded);
    }
    return result;
}

static const LoadedModule *moduleContainingAddress(const QVector<LoadedModule> &modules,
                                                   DWORD64 address)
{
    for (const LoadedModule &module : modules) {
        if (address >= module.base && address < module.base + module.size)
            return &module;
    }
    return nullptr;
}

static QString formatLoadedModules(const QVector<LoadedModule> &modules)
{
    QString text;
    for (const LoadedModule &module : modules) {
        text += QString("  %1 base=0x%2 size=0x%3 path=%4\n")
            .arg(module.name)
            .arg(QString::number(module.base, 16))
            .arg(QString::number(module.size, 16))
            .arg(module.path.isEmpty() ? QStringLiteral("??") : module.path);
    }
    if (text.isEmpty())
        text = QStringLiteral("  <unavailable>\n");
    return text;
}

static QString formatStackFrame(HANDLE process,
                                DWORD64 address,
                                DWORD frameIndex,
                                bool symbolsInitialized,
                                const QVector<LoadedModule> &modules)
{
    QString moduleName;
    DWORD64 moduleBase = 0;

    if (symbolsInitialized) {
        IMAGEHLP_MODULE64 moduleInfo = {};
        moduleInfo.SizeOfStruct = sizeof(moduleInfo);
        if (SymGetModuleInfo64(process, address, &moduleInfo)) {
            moduleBase = moduleInfo.BaseOfImage;
            if (moduleInfo.ModuleName[0] != '\0')
                moduleName = QString::fromLocal8Bit(moduleInfo.ModuleName);
        }
    }

    const LoadedModule *loaded = moduleContainingAddress(modules, address);
    if (loaded != nullptr) {
        if (moduleName.isEmpty())
            moduleName = loaded->name;
        if (moduleBase == 0)
            moduleBase = loaded->base;
    }
    if (moduleName.isEmpty())
        moduleName = QStringLiteral("??");

    const DWORD64 rva = address >= moduleBase ? address - moduleBase : address;
    QString functionName = QStringLiteral("??");
    DWORD64 symbolDisplacement = 0;
    if (symbolsInitialized) {
        constexpr ULONG maxSymbolName = 1024;
        alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + maxSymbolName] = {};
        SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(symbolStorage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = maxSymbolName;
        if (SymFromAddr(process, address, &symbolDisplacement, symbol)) {
            // SYMBOL_INFO::Name is declared CHAR[1] but is a variable-length
            // array. Passing it directly picks the fixed-size array overload of
            // fromLocal8Bit, which truncates every symbol to a single character
            // (this is why the stacks read "Q + 0x1e"). Decay to a pointer and
            // use the length the API reports.
            const char *namePtr = static_cast<const char *>(symbol->Name);
            functionName = symbol->NameLen > 0
                ? QString::fromLocal8Bit(namePtr, int(symbol->NameLen))
                : QString::fromLocal8Bit(namePtr);
        }
    }

    QString sourceLocation;
    if (symbolsInitialized) {
        IMAGEHLP_LINE64 lineInfo = {};
        lineInfo.SizeOfStruct = sizeof(lineInfo);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, address, &lineDisplacement, &lineInfo)
            && lineInfo.FileName != nullptr) {
            sourceLocation = QString("  [%1:%2]")
                .arg(QString::fromLocal8Bit(lineInfo.FileName))
                .arg(lineInfo.LineNumber);
        }
    }

    return QString("  #%1 %2+0x%3  %4 + 0x%5 @ 0x%6%7\n")
        .arg(frameIndex, 2, 10, QLatin1Char('0'))
        .arg(moduleName)
        .arg(QString::number(rva, 16).rightJustified(8, QLatin1Char('0')))
        .arg(functionName)
        .arg(QString::number(symbolDisplacement, 16))
        .arg(QString::number(address, 16))
        .arg(sourceLocation);
}

static bool writeCrashDump(const QString &dumpPath, EXCEPTION_POINTERS *info)
{
    const HANDLE dumpFile = CreateFileW(
        reinterpret_cast<LPCWSTR>(dumpPath.utf16()),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (dumpFile == INVALID_HANDLE_VALUE)
        return false;

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = info;
    exceptionInfo.ClientPointers = FALSE;

    const MINIDUMP_TYPE dumpType = MINIDUMP_TYPE(
        MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo);
    const BOOL written = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        dumpFile,
        dumpType,
        &exceptionInfo,
        nullptr,
        nullptr);
    CloseHandle(dumpFile);
    return written == TRUE;
}

static LONG WINAPI crashHandler(EXCEPTION_POINTERS *info)
{
    try {
        if (!info)
            return EXCEPTION_EXECUTE_HANDLER;

        const DWORD code = info->ExceptionRecord != nullptr
            ? info->ExceptionRecord->ExceptionCode
            : 0;
        const void *addr = info->ExceptionRecord != nullptr
            ? info->ExceptionRecord->ExceptionAddress
            : nullptr;

        QString msg = QString("UNHANDLED EXCEPTION code=0x%1 addr=0x%2")
            .arg(QString::number(code, 16))
            .arg(quint64(reinterpret_cast<quintptr>(addr)), 0, 16);
        // For access violations the ExceptionInformation pair tells us whether
        // the faulting instruction read, wrote or executed, and which address it
        // touched. That distinguishes a null dereference from a use-after-free
        // (freed heap is stamped 0xdddddddd / 0xfeeefeee by the debug heap).
        if (info->ExceptionRecord != nullptr
            && code == EXCEPTION_ACCESS_VIOLATION
            && info->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR operation = info->ExceptionRecord->ExceptionInformation[0];
            const ULONG_PTR target    = info->ExceptionRecord->ExceptionInformation[1];
            const char *operationName = operation == 0 ? "READ"
                                      : operation == 1 ? "WRITE"
                                      : operation == 8 ? "EXECUTE(DEP)"
                                                       : "UNKNOWN";
            msg += QString(" access=%1 target=0x%2")
                .arg(QLatin1String(operationName))
                .arg(quint64(target), 0, 16);
        }
        writeLogLine("CRASH", msg);

        HANDLE process = GetCurrentProcess();
        const QVector<LoadedModule> modules = enumerateLoadedModules(process);
        writeLogLine("CRASH", "MODULES:\n" + formatLoadedModules(modules));

        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        const QString exeDir = executableDirectory();
        const QByteArray searchPath = exeDir.toLocal8Bit();
        const char *searchPathPtr = searchPath.isEmpty() ? "." : searchPath.constData();
        const bool symbolsInitialized = SymInitialize(process, searchPathPtr, TRUE) == TRUE;

        QString stack;
        if (info->ContextRecord != nullptr) {
            CONTEXT context = *info->ContextRecord;
            STACKFRAME64 frame = {};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;

            for (DWORD frameIndex = 0; frameIndex < 64; ++frameIndex) {
                if (!StackWalk64(
                        IMAGE_FILE_MACHINE_AMD64,
                        process,
                        GetCurrentThread(),
                        &frame,
                        &context,
                        nullptr,
                        SymFunctionTableAccess64,
                        SymGetModuleBase64,
                        nullptr)) {
                    break;
                }
                if (frame.AddrPC.Offset == 0)
                    break;
                stack += formatStackFrame(
                    process, frame.AddrPC.Offset, frameIndex, symbolsInitialized, modules);
            }
        }
        if (stack.isEmpty())
            stack = QStringLiteral("  <unavailable>\n");
        writeLogLine("CRASH", "STACK:\n" + stack);

        const QString logDirectory = QFileInfo(g_logFilePath).absolutePath();
        const QString dumpName = QString("veditor_crash_%1_%2.dmp")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"))
            .arg(GetCurrentProcessId());
        const QString dumpPath = QFileInfo(
            QDir(logDirectory).filePath(dumpName)).absoluteFilePath();
        if (writeCrashDump(dumpPath, info))
            writeLogLine("CRASH", QString("DUMP: %1").arg(dumpPath));

        if (symbolsInitialized)
            SymCleanup(process);
    } catch (...) {
        // The process is already terminating; all crash reporting is best-effort.
    }
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
