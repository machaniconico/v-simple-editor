#pragma once

#include "../CodecDetector.h"
#include "SelftestRegistry.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <iostream>
#include <map>

namespace selftest {

struct GateTally {
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    bool gate(int n, bool ok, const char *detail = nullptr)
    {
        if (seen_.count(n)) {
            undoPrevious(n);
            ++failed;
            seen_[n] = Result::Fail;
            std::cerr << "FAIL G" << n << " duplicate gate\n";
            return false;
        }
        seen_[n] = ok ? Result::Pass : Result::Fail;
        if (ok) ++passed;
        else ++failed;
        std::cerr << (ok ? "PASS G" : "FAIL G") << n;
        if (!ok && detail && *detail) std::cerr << ' ' << detail;
        std::cerr << '\n';
        return ok;
    }

    void skip(int n, const char *reason)
    {
        if (seen_.count(n)) {
            undoPrevious(n);
            ++failed;
            seen_[n] = Result::Fail;
            std::cerr << "FAIL G" << n << " duplicate gate\n";
            return;
        }
        seen_[n] = Result::Skip;
        ++skipped;
        std::cerr << "SKIP G" << n;
        if (reason && *reason) std::cerr << ' ' << reason;
        std::cerr << '\n';
    }

    int finish(const char *name = nullptr, bool report = true)
    {
        if (name && *name) std::cerr << name << ' ';
        std::cerr << "summary: " << passed << " PASS, " << failed
                  << " FAIL, " << skipped << " SKIP\n";
        // The tally selftest checks finish() with nested sample tallies. Those
        // samples opt out so only its own G1..G13 count toward the entry.
        if (report) selftests::reportTally(passed, failed, skipped);
        return failed;
    }

private:
    enum class Result { Pass, Fail, Skip };
    std::map<int, Result> seen_;

    void undoPrevious(int n)
    {
        switch (seen_.at(n)) {
        case Result::Pass: --passed; break;
        case Result::Fail: --failed; break;
        case Result::Skip: --skipped; break;
        }
    }
};

inline bool requireEncoder(const QString &name, QString *why = nullptr)
{
    const QByteArray forcedNames = qgetenv("VEDITOR_SELFTEST_FORCE_MISSING_ENCODER");
    const QStringList forced = QString::fromUtf8(forcedNames.constData())
        .split(',', Qt::SkipEmptyParts);
    for (const QString &entry : forced) {
        if (entry.trimmed() == name) {
            if (why) *why = name + QStringLiteral(" forced unavailable");
            return false;
        }
    }
    if (!CodecDetector::isEncoderAvailable(name)) {
        if (why) *why = name + QStringLiteral(" encoder unavailable");
        return false;
    }
    if (why) why->clear();
    return true;
}

} // namespace selftest
