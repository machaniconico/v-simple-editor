// src/selftests/SelftestRegistry.h
// PRD-SPLIT-MAIN-1: Centralized selftest dispatch — table + 3 helper functions.
// The run<Foo>Selftest() implementations remain in src/main.cpp; only the
// routing logic (struct, table, loops) lives here.
#pragma once

#include <cstddef>
#include <optional>
#include <QString>
#include <QStringList>

namespace selftests {

// Central dispatch entry: one row per selftest in kArgvSelftests[].
struct ArgvSelftestEntry {
    const char* name;        // --selftest=<name> kebab-case identifier
    const char* envVar;      // VEDITOR_*_SELFTEST env var, or nullptr
    int (*fn)();             // selftest function pointer
    bool needsQApplication;  // true = dispatch after QApplication construction
    const char* description; // one-line summary shown by --selftest=help
};

// The canonical selftest table (defined in SelftestRegistry.cpp).
extern const ArgvSelftestEntry kArgvSelftests[];
extern const std::size_t kArgvSelftestsCount;

bool requireSelftest(bool condition, const QString &message, QString *error);

// Pre-QApplication argv parse: handles --selftest=list / --selftest=help /
// QApp-free entries. Returns the selftest exit code if matched, std::nullopt
// otherwise (caller proceeds with QApplication construction).
std::optional<int> dispatchPreQApplication(int argc, char* argv[]);

// Post-QApplication argv parse: handles --selftest=all sweep, QApp-required
// entries, and env-gate (VEDITOR_*_SELFTEST integer env vars).
// Returns the selftest exit code if matched, std::nullopt otherwise.
std::optional<int> dispatchPostQApplication(const QStringList& args);

// --selftest=<unknown> guard: if any --selftest=<name> in args did not match
// list / help / all / a kArgvSelftests entry, emit stderr error + return 2.
// Returns std::nullopt if no unknown selftest argv found.
std::optional<int> validateUnknown(const QStringList& args);

} // namespace selftests
