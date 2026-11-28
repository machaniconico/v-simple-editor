#pragma once

namespace hangdump {

struct Result {
    char dumpPath[512] = {};
    bool dumpWritten = false;
    bool stacksWritten = false;
    long long elapsedMs = 0;
};

// Start the independent dump deadline thread before the watchdog can time out.
// Safe to call for each armed selftest entry; the thread is created once.
void prepare();

// Called only by the selftest watchdog after its TIMEOUT line. No Qt or
// selftest locks are used; the caller exits the process after this returns.
Result writeHangArtifacts(const char* selftestName);

} // namespace hangdump
