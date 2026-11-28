#include "SelftestGate.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTest>

#include <memory>
#include <sstream>

namespace {

struct ChildRun {
    bool finished = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QString stderrText;
};

QProcessEnvironment cleanChildEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("VEDITOR_ALL_SELFTEST"));
    env.remove(QStringLiteral("VEDITOR_ALL_SELFTEST_FILTER"));
    env.remove(QStringLiteral("VEDITOR_GATE_COUNT_PROBE_MODE"));
    for (std::size_t i = 0; i < selftests::kArgvSelftestsCount; ++i) {
        if (selftests::kArgvSelftests[i].envVar)
            env.remove(QString::fromLatin1(selftests::kArgvSelftests[i].envVar));
    }
    env.insert(QStringLiteral("VEDITOR_SELFTEST_TIMEOUT_SEC"), QStringLiteral("60"));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    return env;
}

ChildRun runChild(const QStringList &args, const QProcessEnvironment &env)
{
    ChildRun result;
    QProcess child;
    child.setProcessEnvironment(env);
    child.setProcessChannelMode(QProcess::SeparateChannels);
    child.setWorkingDirectory(QDir::currentPath());
    child.start(QCoreApplication::applicationFilePath(), args);
    if (!child.waitForStarted(10000)) {
        result.stderrText = QStringLiteral("failed to start: ") + child.errorString();
        return result;
    }
    QByteArray stderrBytes;
    result.finished = QTest::qWaitFor([&]() {
        stderrBytes += child.readAllStandardError();
        child.readAllStandardOutput();
        return child.state() == QProcess::NotRunning;
    }, 65000);
    if (!result.finished) {
        child.kill();
        (void)QTest::qWaitFor([&]() { return child.state() == QProcess::NotRunning; }, 5000);
    }
    stderrBytes += child.readAllStandardError();
    result.stderrText = QString::fromUtf8(stderrBytes);
    if (child.state() == QProcess::NotRunning) {
        result.exitCode = child.exitCode();
        result.exitStatus = child.exitStatus();
    }
    return result;
}

void logChild(int gate, const ChildRun &run)
{
    std::cerr << "gate-tally G" << gate << " child finished=" << run.finished
              << " exit=" << run.exitCode
              << " normal=" << (run.exitStatus == QProcess::NormalExit) << '\n';
    if (!run.finished || run.exitStatus != QProcess::NormalExit)
        std::cerr << "gate-tally G" << gate << " child stderr: "
                  << run.stderrText.right(800).toStdString() << '\n';
}

bool normalExit(const ChildRun &run, int code)
{
    return run.finished && run.exitStatus == QProcess::NormalExit
        && run.exitCode == code;
}

} // namespace

int runGateCountProbeSelftest()
{
    const QByteArray mode = qgetenv("VEDITOR_GATE_COUNT_PROBE_MODE");
    selftest::GateTally tally;
    tally.gate(1, true);
    if (mode == "split") {
        const int firstResult = tally.finish("gate-count-probe-part-1");
        selftest::GateTally second;
        second.gate(2, firstResult == 0);
        return firstResult + second.finish("gate-count-probe-part-2");
    }
    tally.gate(2, true);
    if (mode == "no-finish") return 0;
    if (mode == "extra") tally.gate(3, true);
    return tally.finish("gate-count-probe");
}

int runGateTallySelftest()
{
    selftest::GateTally tally;

    std::ostringstream firstOutput;
    auto *previous = std::cerr.rdbuf(firstOutput.rdbuf());
    selftest::GateTally first;
    first.gate(1, true);
    first.gate(2, false);
    first.skip(3, "unavailable");
    const int firstResult = first.finish(nullptr, false);
    std::cerr.rdbuf(previous);
    const bool summaryFound = firstOutput.str().find("summary: 1 PASS, 1 FAIL, 1 SKIP")
        != std::string::npos;
    std::cerr << "gate-tally G1: return=" << firstResult << " passed=" << first.passed
              << " failed=" << first.failed << " skipped=" << first.skipped
              << " summaryFound=" << summaryFound << '\n';
    tally.gate(1, firstResult == 1 && first.passed == 1 && first.failed == 1
        && first.skipped == 1 && summaryFound);

    const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_SELFTEST_FORCE_MISSING_ENCODER");
    const QByteArray oldEnv = qgetenv("VEDITOR_SELFTEST_FORCE_MISSING_ENCODER");
    qputenv("VEDITOR_SELFTEST_FORCE_MISSING_ENCODER", "libvpx-vp9, unused");
    QString reason;
    const bool forcedMissing = !selftest::requireEncoder("libvpx-vp9", &reason)
        && reason.contains("forced unavailable");
    const bool unrelated = selftest::requireEncoder("libx264")
        == CodecDetector::isEncoderAvailable("libx264");
    if (hadEnv) qputenv("VEDITOR_SELFTEST_FORCE_MISSING_ENCODER", oldEnv);
    else qunsetenv("VEDITOR_SELFTEST_FORCE_MISSING_ENCODER");
    std::cerr << "gate-tally G2: forcedMissing=" << forcedMissing
              << " unrelatedMatchesDetector=" << unrelated << '\n';
    tally.gate(2, forcedMissing && unrelated);

    std::ostringstream duplicateOutput;
    previous = std::cerr.rdbuf(duplicateOutput.rdbuf());
    selftest::GateTally duplicate;
    duplicate.skip(7, "unavailable");
    const bool duplicateAccepted = duplicate.gate(7, true);
    const int duplicateResult = duplicate.finish(nullptr, false);
    std::cerr.rdbuf(previous);
    const bool duplicateLogged = duplicateOutput.str().find("FAIL G7 duplicate gate")
        != std::string::npos;
    std::cerr << "gate-tally G3: accepted=" << duplicateAccepted
              << " return=" << duplicateResult << " passed=" << duplicate.passed
              << " failed=" << duplicate.failed << " skipped=" << duplicate.skipped
              << " duplicateLogged=" << duplicateLogged << '\n';
    tally.gate(3, !duplicateAccepted && duplicateResult == 1
        && duplicate.passed == 0 && duplicate.failed == 1 && duplicate.skipped == 0
        && duplicateLogged);

    std::ostringstream resultOutput;
    previous = std::cerr.rdbuf(resultOutput.rdbuf());
    selftest::GateTally result;
    result.gate(1, false);
    result.skip(2, "unavailable");
    const int resultCode = result.finish(nullptr, false);
    std::cerr.rdbuf(previous);
    std::cerr << "gate-tally G4: return=" << resultCode << " failed=" << result.failed << '\n';
    tally.gate(4, resultCode == result.failed && resultCode == 1);

    // G5 catches a regression where a repeated PASS leaves the old pass in
    // the total while also counting the duplicate as a failure.
    std::ostringstream passDuplicateOutput;
    previous = std::cerr.rdbuf(passDuplicateOutput.rdbuf());
    selftest::GateTally passDuplicate;
    passDuplicate.gate(1, true);
    const bool repeatedAccepted = passDuplicate.gate(1, true);
    std::cerr.rdbuf(previous);
    const bool repeatedLogged = passDuplicateOutput.str().find("FAIL G1 duplicate gate")
        != std::string::npos;
    std::cerr << "gate-tally G5 accepted=" << repeatedAccepted
              << " passed=" << passDuplicate.passed
              << " failed=" << passDuplicate.failed
              << " skipped=" << passDuplicate.skipped
              << " repeatedLogged=" << repeatedLogged << '\n';
    tally.gate(5, !repeatedAccepted && passDuplicate.passed == 0
        && passDuplicate.failed == 1 && passDuplicate.skipped == 0
        && repeatedLogged);

    std::unique_ptr<QCoreApplication> localApp;
    static int argc = 1;
    static char arg0[] = "v-simple-editor";
    static char *argv[] = {arg0, nullptr};
    if (!QCoreApplication::instance())
        localApp = std::make_unique<QCoreApplication>(argc, argv);
    const QStringList probeArg{QStringLiteral("--selftest=gate-count-probe")};
    const auto baseEnv = cleanChildEnvironment();

    // G6 verifies that two finish() calls within an entry sum to two gates.
    auto splitEnv = baseEnv;
    splitEnv.insert(QStringLiteral("VEDITOR_GATE_COUNT_PROBE_MODE"), QStringLiteral("split"));
    const ChildRun split = runChild(probeArg, splitEnv);
    logChild(6, split);
    const bool splitCounted = split.stderrText.contains(
        QStringLiteral("gate-count: declared 2, ran 2"));
    const bool splitSummaries = split.stderrText.count(QStringLiteral("summary: 1 PASS, 0 FAIL, 0 SKIP")) == 2;
    std::cerr << "gate-tally G6 splitCounted=" << splitCounted
              << " splitSummaries=" << splitSummaries << '\n';
    tally.gate(6, normalExit(split, 0) && splitCounted && splitSummaries);

    // G7 verifies a path that returns before finish() stays unverified.
    auto noFinishEnv = baseEnv;
    noFinishEnv.insert(QStringLiteral("VEDITOR_GATE_COUNT_PROBE_MODE"), QStringLiteral("no-finish"));
    const ChildRun noFinish = runChild(probeArg, noFinishEnv);
    logChild(7, noFinish);
    const bool noFinishUnverified = noFinish.stderrText.contains(
        QStringLiteral("gate-count: unverified (no GateTally) (selftest=gate-count-probe)"));
    std::cerr << "gate-tally G7 noFinishUnverified=" << noFinishUnverified << '\n';
    tally.gate(7, normalExit(noFinish, 0) && noFinishUnverified);

    auto extraEnv = baseEnv;
    extraEnv.insert(QStringLiteral("VEDITOR_GATE_COUNT_PROBE_MODE"), QStringLiteral("extra"));
    const QString mismatchLine = QStringLiteral(
        "FAIL gate-count: declared 2, ran 3 (selftest=gate-count-probe)");

    // G8 is the negative control for an extra gate on the argv path.
    const ChildRun extra = runChild(probeArg, extraEnv);
    logChild(8, extra);
    const bool extraMismatch = extra.stderrText.contains(mismatchLine);
    std::cerr << "gate-tally G8 extraMismatch=" << extraMismatch << '\n';
    tally.gate(8, extra.finished && extra.exitStatus == QProcess::NormalExit
        && extra.exitCode != 0 && extraMismatch);

    // G9 verifies a matching declaration has one success line and exit zero.
    const ChildRun matching = runChild(probeArg, baseEnv);
    logChild(9, matching);
    const int matchingLines = matching.stderrText.count(
        QStringLiteral("gate-count: declared 2, ran 2"));
    std::cerr << "gate-tally G9 matchingLines=" << matchingLines << '\n';
    tally.gate(9, normalExit(matching, 0) && matchingLines == 1);

    // G10 verifies that a legacy, non-GateTally entry remains runnable.
    const ChildRun legacy = runChild({QStringLiteral("--selftest=fcpxml")}, baseEnv);
    logChild(10, legacy);
    const bool legacyUnverified = legacy.stderrText.contains(
        QStringLiteral("gate-count: unverified (no GateTally) (selftest=fcpxml)"));
    std::cerr << "gate-tally G10 legacyUnverified=" << legacyUnverified << '\n';
    tally.gate(10, normalExit(legacy, 0) && legacyUnverified);

    // G11 catches malformed declarations without opening source files.
    static const QRegularExpression declaration(QStringLiteral("\\(([0-9]+) gates\\)"));
    int malformed = 0;
    for (std::size_t i = 0; i < selftests::kArgvSelftestsCount; ++i) {
        const auto &entry = selftests::kArgvSelftests[i];
        const QString description = QString::fromUtf8(entry.description ? entry.description : "");
        if (description.contains(QStringLiteral("gates"))
            && !declaration.match(description).hasMatch()) {
            ++malformed;
            std::cerr << "gate-tally G11 malformed registry row " << (i + 1)
                      << " (selftest=" << entry.name << "): "
                      << description.toStdString() << '\n';
        }
    }
    std::cerr << "gate-tally G11 malformed=" << malformed << '\n';
    tally.gate(11, malformed == 0);

    // G12 checks the sweep's shared wrapper and its mismatch summary.
    auto sweepEnv = extraEnv;
    sweepEnv.insert(QStringLiteral("VEDITOR_ALL_SELFTEST_FILTER"),
        QStringLiteral("gate-count-probe"));
    const ChildRun sweep = runChild({QStringLiteral("--selftest=all")}, sweepEnv);
    logChild(12, sweep);
    const bool sweepMismatch = sweep.stderrText.contains(mismatchLine);
    const bool sweepCount = sweep.stderrText.contains(QStringLiteral(
        "gate-count summary: verified 0, mismatch 1, unverified 0, no-declaration 0"));
    std::cerr << "gate-tally G12 sweepMismatch=" << sweepMismatch
              << " sweepCount=" << sweepCount << '\n';
    tally.gate(12, sweep.finished && sweep.exitStatus == QProcess::NormalExit
        && sweep.exitCode != 0 && sweepMismatch && sweepCount);

    // G13 checks the environment-variable entry path with the same mismatch.
    auto envEntry = extraEnv;
    envEntry.insert(QStringLiteral("VEDITOR_GATE_COUNT_PROBE_SELFTEST"), QStringLiteral("1"));
    const ChildRun envRun = runChild({}, envEntry);
    logChild(13, envRun);
    const bool envMismatch = envRun.stderrText.contains(mismatchLine);
    std::cerr << "gate-tally G13 envMismatch=" << envMismatch << '\n';
    tally.gate(13, envRun.finished && envRun.exitStatus == QProcess::NormalExit
        && envRun.exitCode != 0 && envMismatch);

    // G14 catches a regression where the std::string detail disappears from
    // a successful line while the failed line and summary still look valid.
    std::ostringstream operandOutput;
    previous = std::cerr.rdbuf(operandOutput.rdbuf());
    selftest::GateTally operands;
    operands.gate(1, true, std::string("operand=17"));
    operands.gate(2, false, std::string("operand=23"));
    const int operandResult = operands.finish(nullptr, false);
    std::cerr.rdbuf(previous);
    const std::string operandText = operandOutput.str();
    const bool passDetail = operandText.find("PASS G1 operand=17\n") != std::string::npos;
    const bool failDetail = operandText.find("FAIL G2 operand=23\n") != std::string::npos;
    const bool operandSummary = operandText.find("summary: 1 PASS, 1 FAIL, 0 SKIP\n")
        != std::string::npos;
    std::ostringstream literalOutput;
    previous = std::cerr.rdbuf(literalOutput.rdbuf());
    selftest::GateTally literal;
    literal.gate(3, true, "hidden");
    const int literalResult = literal.finish(nullptr, false);
    std::cerr.rdbuf(previous);
    const std::string literalText = literalOutput.str();
    const bool literalHidden = literalText.find("PASS G3\n") != std::string::npos
        && literalText.find("hidden") == std::string::npos && literalResult == 0;
    std::cerr << "gate-tally G14 passDetail=" << passDetail
              << " failDetail=" << failDetail << " result=" << operandResult
              << " passed=" << operands.passed << " failed=" << operands.failed
              << " skipped=" << operands.skipped << " summary=" << operandSummary
              << " literalHidden=" << literalHidden << '\n';
    tally.gate(14, passDetail && failDetail && operandSummary && operandResult == 1
        && operands.passed == 1 && operands.failed == 1 && operands.skipped == 0
        && literalHidden);

    return tally.finish();
}
