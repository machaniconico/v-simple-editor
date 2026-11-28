// src/selftests/watchdog_selftest.cpp
// US-700 SELFTEST-WATCHDOG: --selftest=watchdog (G1..G7) and the internal
// --selftest=watchdog-probe child entry.
//
// The watchdog itself lives in SelftestRegistry.cpp (selftests::armWatchdog /
// disarmWatchdog). These gates exercise it through child processes of this
// executable so that std::_Exit(124) is observed from the outside.

#include "SelftestRegistry.h"
#include "SelftestGate.h"
#include "../mcp/McpHttpServer.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTest>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {

std::mutex g_watchdogProbeMutex;
std::mutex g_watchdogProbeReadyMutex;
std::condition_variable g_watchdogProbeReadyCv;
bool g_watchdogProbeReady = false;

#ifdef _WIN32
#define WATCHDOG_NOINLINE __declspec(noinline)
#else
#define WATCHDOG_NOINLINE __attribute__((noinline))
#endif

extern "C" WATCHDOG_NOINLINE void watchdogProbeHangWorker()
{
    std::lock_guard<std::mutex> held(g_watchdogProbeMutex);
    {
        std::lock_guard<std::mutex> ready(g_watchdogProbeReadyMutex);
        g_watchdogProbeReady = true;
    }
    g_watchdogProbeReadyCv.notify_one();
    for (;;)
        std::this_thread::sleep_for(std::chrono::hours(1));
}

extern "C" WATCHDOG_NOINLINE void watchdogProbeHangMain()
{
    {
        std::unique_lock<std::mutex> ready(g_watchdogProbeReadyMutex);
        g_watchdogProbeReadyCv.wait(ready, [] { return g_watchdogProbeReady; });
    }
    std::lock_guard<std::mutex> blocked(g_watchdogProbeMutex);
}

QSet<QString> dumpFiles()
{
    const QDir dir(QStringLiteral(".omc/artifacts/hang-dumps"));
    QSet<QString> files;
    for (const QString& name : dir.entryList(QDir::Files))
        files.insert(name);
    return files;
}

bool isWslDirectExec()
{
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsSet("WSL_INTEROP") ||
        qEnvironmentVariableIsSet("VEDITOR_WSL_DIRECT_EXEC"))
        return true;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    DWORD parentPid = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                parentPid = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    bool direct = false;
    if (parentPid && Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == parentPid) {
                direct = lstrcmpiW(entry.szExeFile, L"wsl.exe") == 0 ||
                         lstrcmpiW(entry.szExeFile, L"wslhost.exe") == 0;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return direct;
#else
    return false;
#endif
}

struct ChildRun {
    bool finished = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    qint64 elapsedMs = 0;
    QString stdErr;
};

QProcessEnvironment cleanChildEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("VEDITOR_SELFTEST_TIMEOUT_SEC"));
    env.remove(QStringLiteral("VEDITOR_WATCHDOG_PROBE_MODE"));
    env.remove(QStringLiteral("VEDITOR_ALL_SELFTEST"));
    env.remove(QStringLiteral("VEDITOR_ALL_SELFTEST_FILTER"));
    env.remove(QStringLiteral("VEDITOR_WATCHDOG_SELFTEST"));
    return env;
}

// Starts the child and waits (condition wait) until it exits or waitMs
// elapses. When killAfterWait is true and the child is still running, it is
// killed after the wait so no process is left behind.
ChildRun runChild(const QProcessEnvironment& env, const QStringList& args,
                  int waitMs, bool killAfterWait)
{
    ChildRun r;
    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.setWorkingDirectory(QDir::currentPath());
    QElapsedTimer timer;
    timer.start();
    proc.start(QCoreApplication::applicationFilePath(), args);
    if (!proc.waitForStarted(10000)) {
        r.stdErr = QStringLiteral("<failed to start: %1>").arg(proc.errorString());
        return r;
    }
    QByteArray err;
    r.finished = QTest::qWaitFor([&]() {
        err += proc.readAllStandardError();
        proc.readAllStandardOutput();
        return proc.state() == QProcess::NotRunning;
    }, waitMs);
    r.elapsedMs = timer.elapsed();
    if (!r.finished && killAfterWait) {
        proc.kill();
        (void)QTest::qWaitFor([&]() { return proc.state() == QProcess::NotRunning; }, 5000);
    }
    err += proc.readAllStandardError();
    r.stdErr = QString::fromUtf8(err);
    if (proc.state() == QProcess::NotRunning) {
        r.exitCode = proc.exitCode();
        r.exitStatus = proc.exitStatus();
    }
    return r;
}

QString excerpt(const QString& s)
{
    QString t = s.trimmed();
    t.replace(QLatin1Char('\r'), QString());
    if (t.size() > 600)
        t = QStringLiteral("...") + t.right(600);
    return t;
}

void logChild(const char* gate, const ChildRun& r)
{
    std::fprintf(stderr,
                 "[watchdog] %s finished=%d exitCode=%d exitStatus=%s elapsedMs=%lld\n"
                 "[watchdog] %s stderr excerpt:\n%s\n",
                 gate, r.finished ? 1 : 0, r.exitCode,
                 r.exitStatus == QProcess::NormalExit ? "normal" : "crash",
                 static_cast<long long>(r.elapsedMs),
                 gate, excerpt(r.stdErr).toUtf8().constData());
    std::fflush(stderr);
}

QStringList* g_capturedWarnings = nullptr;

void captureMessages(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    if (g_capturedWarnings && type == QtWarningMsg)
        g_capturedWarnings->append(msg);
    std::fprintf(stderr, "[watchdog] captured qt message: %s\n", msg.toUtf8().constData());
    std::fflush(stderr);
}

} // namespace

int runWatchdogProbeSelftest()
{
    const QByteArray mode = qgetenv("VEDITOR_WATCHDOG_PROBE_MODE");
    if (mode == "hang") {
        std::thread(watchdogProbeHangWorker).detach();
        watchdogProbeHangMain();
    }
    if (mode == "slow")
        std::this_thread::sleep_for(std::chrono::seconds(2));
    return 0;
}

int runWatchdogSelftest()
{
    std::unique_ptr<QCoreApplication> localApp;
    static int s_argc = 1;
    static char s_arg0[] = "v-simple-editor";
    static char* s_argv[] = { s_arg0, nullptr };
    if (!QCoreApplication::instance())
        localApp = std::make_unique<QCoreApplication>(s_argc, s_argv);

    selftest::GateTally tally;
    auto gate = [&](const char* id, bool ok) {
        tally.gate(std::atoi(id + 1), ok);
    };
    const QStringList probeArgs{ QStringLiteral("--selftest=watchdog-probe") };

    // G1: TIMEOUT_SEC=2 + hang -> exit 124 within 15 s with the TIMEOUT line.
    {
        QProcessEnvironment env = cleanChildEnvironment();
        env.insert(QStringLiteral("VEDITOR_SELFTEST_TIMEOUT_SEC"), QStringLiteral("2"));
        env.insert(QStringLiteral("VEDITOR_WATCHDOG_PROBE_MODE"), QStringLiteral("hang"));
        const ChildRun r = runChild(env, probeArgs, 15000, true);
        logChild("G1", r);
        const bool hasLine = r.stdErr.contains(
            QStringLiteral("summary: TIMEOUT after 2s (selftest=watchdog-probe)"));
        std::fprintf(stderr, "[watchdog] G1 terms: finished=%d exit124=%d within15s=%d timeoutLine=%d\n",
                     r.finished, r.exitCode == 124, r.elapsedMs < 15000, hasLine);
        gate("G1", r.finished && r.exitCode == 124 && r.elapsedMs < 15000 && hasLine);
    }

    // G2: default timeout -> armed 900s, exit 0, no TIMEOUT. Checked on the
    // table path (watchdog-probe) and on a legacy argv entry
    // (--selftest-trackmatte-rm6-duplicate, dispatchLegacyTrackmatte).
    {
        const ChildRun r = runChild(cleanChildEnvironment(), probeArgs, 30000, true);
        logChild("G2", r);
        const bool armed = r.stdErr.contains(QStringLiteral("[watchdog] armed 900s"));
        const bool timeout = r.stdErr.contains(QStringLiteral("TIMEOUT"));
        const bool ok = r.finished && r.exitCode == 0 && r.exitStatus == QProcess::NormalExit;

        const ChildRun legacy = runChild(
            cleanChildEnvironment(),
            QStringList{ QStringLiteral("--selftest-trackmatte-rm6-duplicate") }, 60000, true);
        logChild("G2-legacy", legacy);
        const bool legacyArmed = legacy.stdErr.contains(
            QStringLiteral("[watchdog] armed 900s (selftest-trackmatte-rm6-duplicate)"));
        const bool legacyTimeout = legacy.stdErr.contains(QStringLiteral("TIMEOUT"));
        const bool legacyOk = legacy.finished && legacy.exitCode == 0
            && legacy.exitStatus == QProcess::NormalExit;
        std::fprintf(stderr,
                     "[watchdog] G2 terms: finished=%d exit0=%d armed900=%d noTimeout=%d "
                     "legacyExit0=%d legacyArmed900=%d legacyNoTimeout=%d\n",
                     r.finished, ok, armed, !timeout, legacyOk, legacyArmed, !legacyTimeout);
        gate("G2", ok && armed && !timeout && legacyOk && legacyArmed && !legacyTimeout);
    }

    // G3: TIMEOUT_SEC=0 + hang -> still running after 3 s, `[watchdog] disabled`.
    {
        QProcessEnvironment env = cleanChildEnvironment();
        env.insert(QStringLiteral("VEDITOR_SELFTEST_TIMEOUT_SEC"), QStringLiteral("0"));
        env.insert(QStringLiteral("VEDITOR_WATCHDOG_PROBE_MODE"), QStringLiteral("hang"));
        const ChildRun r = runChild(env, probeArgs, 3000, true);
        logChild("G3", r);
        const bool disabled = r.stdErr.contains(QStringLiteral("[watchdog] disabled"));
        const bool timeout = r.stdErr.contains(QStringLiteral("TIMEOUT"));
        std::fprintf(stderr, "[watchdog] G3 terms: runningAt3s=%d disabledLine=%d noTimeout=%d\n",
                     !r.finished, disabled, !timeout);
        gate("G3", !r.finished && disabled && !timeout);
    }

    // G4: 20 consecutive ports occupied -> start() false within 2 s + warning.
    {
        std::vector<std::unique_ptr<QTcpServer>> blockers;
        int base = 0;
        for (int candidate = 47100; candidate + 19 <= 65535 && base == 0; candidate += 20) {
            blockers.clear();
            bool all = true;
            for (int i = 0; i < 20; ++i) {
                auto s = std::make_unique<QTcpServer>();
                if (!s->listen(QHostAddress::LocalHost, static_cast<quint16>(candidate + i))) {
                    all = false;
                    break;
                }
                blockers.push_back(std::move(s));
            }
            if (all)
                base = candidate;
        }
        const QByteArray oldPort = qgetenv("VEDITOR_MCP_PORT");
        const bool hadPort = qEnvironmentVariableIsSet("VEDITOR_MCP_PORT");
        const QByteArray oldToken = qgetenv("VEDITOR_MCP_TOKEN");
        const bool hadToken = qEnvironmentVariableIsSet("VEDITOR_MCP_TOKEN");
        qputenv("VEDITOR_MCP_PORT", QByteArray::number(base));
        qputenv("VEDITOR_MCP_TOKEN", "watchdog-selftest-token");

        QStringList warnings;
        g_capturedWarnings = &warnings;
        const QtMessageHandler previous = qInstallMessageHandler(captureMessages);
        bool started = true;
        qint64 elapsedMs = -1;
        if (base != 0) {
            mcp::McpHttpServer server(nullptr);
            QElapsedTimer timer;
            timer.start();
            started = server.start();
            elapsedMs = timer.elapsed();
        }
        qInstallMessageHandler(previous);
        g_capturedWarnings = nullptr;

        if (hadPort) qputenv("VEDITOR_MCP_PORT", oldPort); else qunsetenv("VEDITOR_MCP_PORT");
        if (hadToken) qputenv("VEDITOR_MCP_TOKEN", oldToken); else qunsetenv("VEDITOR_MCP_TOKEN");

        const QString expected = QStringLiteral("MCP サーバを起動できませんでした: ポート %1-%2")
                                     .arg(base).arg(base + 19);
        bool warned = false;
        for (const QString& w : warnings)
            warned = warned || w.contains(expected);
        std::fprintf(stderr,
                     "[watchdog] G4 terms: occupiedBase=%d blockers=%d startReturnedFalse=%d "
                     "elapsedMs=%lld under2s=%d warningMatched=%d warnings=%d\n",
                     base, static_cast<int>(blockers.size()), !started,
                     static_cast<long long>(elapsedMs), elapsedMs >= 0 && elapsedMs < 2000,
                     warned, static_cast<int>(warnings.size()));
        gate("G4", base != 0 && !started && elapsedMs >= 0 && elapsedMs < 2000 && warned);
    }

    // G5: sweep re-arms per entry: 2 s + 2 s + proxy under a 3 s limit.
    {
        QProcessEnvironment env = cleanChildEnvironment();
        env.insert(QStringLiteral("VEDITOR_SELFTEST_TIMEOUT_SEC"), QStringLiteral("3"));
        env.insert(QStringLiteral("VEDITOR_WATCHDOG_PROBE_MODE"), QStringLiteral("slow"));
        env.insert(QStringLiteral("VEDITOR_ALL_SELFTEST_FILTER"),
                   QStringLiteral("watchdog-probe,watchdog-probe,proxy"));
        const ChildRun r = runChild(env, QStringList{ QStringLiteral("--selftest=all") },
                                    120000, true);
        logChild("G5", r);
        const int armed = r.stdErr.count(QStringLiteral("[watchdog] armed 3s"));
        const int timeouts = r.stdErr.count(QStringLiteral("TIMEOUT"));
        std::fprintf(stderr,
                     "[watchdog] G5 terms: finished=%d exit0=%d armed3sCount=%d timeoutCount=%d "
                     "elapsedMs=%lld\n",
                     r.finished, r.exitCode == 0 && r.exitStatus == QProcess::NormalExit,
                     armed, timeouts, static_cast<long long>(r.elapsedMs));
        gate("G5", r.finished && r.exitCode == 0 && r.exitStatus == QProcess::NormalExit
                   && armed == 3 && timeouts == 0);
    }

    // G6 detects a timeout path that exits without producing its required
    // minidump. The disabled and normal paths in G7 are its negative control.
#ifdef Q_OS_WIN
    if (isWslDirectExec()) {
        tally.skip(6, "WSL direct exec cannot validate Windows child dump behavior");
        tally.skip(7, "WSL direct exec cannot validate Windows child dump behavior");
    } else {
        QProcessEnvironment env = cleanChildEnvironment();
        env.insert(QStringLiteral("VEDITOR_SELFTEST_TIMEOUT_SEC"), QStringLiteral("2"));
        env.insert(QStringLiteral("VEDITOR_WATCHDOG_PROBE_MODE"), QStringLiteral("hang"));
        const ChildRun r = runChild(env, probeArgs, 15000, true);
        const QRegularExpression dumpLine(QStringLiteral(
            R"(watchdog: dump written to (\S+\.dmp) stacks=(yes|no) elapsedMs=(\d+))"));
        const QRegularExpressionMatch match = dumpLine.match(r.stdErr);
        const QString dumpPath = match.captured(1);
        const QFileInfo dumpInfo(QDir::current().filePath(dumpPath));
        const bool timeoutLine = r.stdErr.contains(
            QStringLiteral("summary: TIMEOUT after 2s (selftest=watchdog-probe)"));
        const bool dumpExists = dumpInfo.isFile() && dumpInfo.size() > 0;
        const bool stackReported = match.captured(2) == QStringLiteral("yes");
        const qint64 dumpElapsedMs = match.hasMatch() ? match.captured(3).toLongLong() : -1;

        QFile stackFile(dumpPath.left(dumpPath.size() - 4) + QStringLiteral(".txt"));
        const bool textExists = stackFile.open(QIODevice::ReadOnly);
        const QString stackText = textExists ? QString::fromUtf8(stackFile.readAll()) : QString();
        const QStringList sections = stackText.split(
            QRegularExpression(QStringLiteral(R"((?=thread \d+))")), Qt::SkipEmptyParts);
        int threadCount = 0;
        int mainTid = -1;
        int workerTid = -1;
        const QRegularExpression threadId(QStringLiteral(R"(^thread (\d+))"));
        for (const QString& section : sections) {
            const QRegularExpressionMatch idMatch = threadId.match(section);
            if (!idMatch.hasMatch()) continue;
            ++threadCount;
            const int tid = idMatch.captured(1).toInt();
            if (section.contains(QStringLiteral("watchdogProbeHangMain"))) mainTid = tid;
            if (section.contains(QStringLiteral("watchdogProbeHangWorker"))) workerTid = tid;
        }
        std::fprintf(stderr,
            "[watchdog] G6 terms: finished=%d exit124=%d timeoutLine=%d "
            "dumpPath=%s dumpExists=%d dumpBytes=%lld dumpElapsedMs=%lld "
            "txt=%d stackReported=%d threads=%d mainTid=%d workerTid=%d distinct=%d\n",
            r.finished, r.exitCode == 124, timeoutLine,
            dumpPath.toUtf8().constData(), dumpExists,
            static_cast<long long>(dumpInfo.size()), static_cast<long long>(dumpElapsedMs),
            textExists, stackReported, threadCount, mainTid, workerTid,
            mainTid >= 0 && workerTid >= 0 && mainTid != workerTid);
        gate("G6", r.finished && r.exitCode == 124 && timeoutLine &&
                    match.hasMatch() && dumpExists && dumpElapsedMs >= 0 && dumpElapsedMs < 10000);

        // G7 detects a regression where disabled or normally completed entries
        // still leave dump artifacts. The hang child is deliberately killed.
        const QSet<QString> before = dumpFiles();
        env.insert(QStringLiteral("VEDITOR_SELFTEST_TIMEOUT_SEC"), QStringLiteral("0"));
        const ChildRun disabled = runChild(env, probeArgs, 3000, true);
        env.insert(QStringLiteral("VEDITOR_SELFTEST_TIMEOUT_SEC"), QStringLiteral("60"));
        env.remove(QStringLiteral("VEDITOR_WATCHDOG_PROBE_MODE"));
        const ChildRun normal = runChild(env,
            QStringList{ QStringLiteral("--selftest=gate-count-probe") }, 30000, true);
        const QSet<QString> added = dumpFiles() - before;
        const bool disabledLine = disabled.stdErr.contains(QStringLiteral("[watchdog] disabled"));
        const bool normalArmed = normal.stdErr.contains(QStringLiteral("[watchdog] armed"));
        const bool normalExit = normal.finished && normal.exitCode == 0 &&
                                normal.exitStatus == QProcess::NormalExit;
        std::fprintf(stderr,
            "[watchdog] G7 terms: disabledFinished=%d disabledExit=%d "
            "disabledLine=%d normalArmed=%d normalFinished=%d normalExit=%d "
            "normalExitCode=%d newDumpFiles=%d\n",
            disabled.finished, disabled.exitCode, disabledLine, normalArmed, normal.finished,
            normalExit, normal.exitCode, static_cast<int>(added.size()));
        gate("G7", !disabled.finished && disabled.exitCode != 124 && disabledLine &&
                    normalArmed && normalExit && added.isEmpty());
    }
#else
    tally.skip(6, "Windows minidump requires Windows");
    tally.skip(7, "Windows minidump requires Windows");
#endif

    return tally.finish();
}
