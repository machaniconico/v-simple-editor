#include "SelftestGate.h"

#include <sstream>

int runGateTallySelftest()
{
    selftest::GateTally tally;

    std::ostringstream firstOutput;
    auto *previous = std::cerr.rdbuf(firstOutput.rdbuf());
    selftest::GateTally first;
    first.gate(1, true);
    first.gate(2, false);
    first.skip(3, "unavailable");
    const int firstResult = first.finish();
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
    const int duplicateResult = duplicate.finish();
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
    const int resultCode = result.finish();
    std::cerr.rdbuf(previous);
    std::cerr << "gate-tally G4: return=" << resultCode << " failed=" << result.failed << '\n';
    tally.gate(4, resultCode == result.failed && resultCode == 1);

    return tally.finish();
}
