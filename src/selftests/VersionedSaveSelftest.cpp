#include "../VersionedSave.h"

#include <QSet>

#include <cstdio>

namespace {

bool checkPath(const char *gate,
               const QString &current,
               const QSet<QString> &existing,
               const QString &expected,
               int &passed,
               int &failed)
{
    const QString actual = versionedsave::nextVersionedPath(
        current,
        [&existing](const QString &path) { return existing.contains(path); });
    const bool ok = actual == expected;
    std::printf("[versioned-save] %s %s current=%s actual=%s expected=%s\n",
                ok ? "PASS" : "FAIL",
                gate,
                current.toUtf8().constData(),
                actual.toUtf8().constData(),
                expected.toUtf8().constData());
    ok ? ++passed : ++failed;
    return ok;
}

} // namespace

int runVersionedSaveSelftest()
{
    int passed = 0;
    int failed = 0;

    checkPath("G1 numbered suffix increments",
              QStringLiteral("/tmp/project_v001.json"),
              QSet<QString>{},
              QStringLiteral("/tmp/project_v002.json"),
              passed,
              failed);

    checkPath("G2 unnumbered appends v002",
              QStringLiteral("/tmp/project.json"),
              QSet<QString>{},
              QStringLiteral("/tmp/project_v002.json"),
              passed,
              failed);

    checkPath("G3 collisions are skipped",
              QStringLiteral("/tmp/project_v001.json"),
              QSet<QString>{QStringLiteral("/tmp/project_v002.json"),
                            QStringLiteral("/tmp/project_v003.json")},
              QStringLiteral("/tmp/project_v004.json"),
              passed,
              failed);

    checkPath("G4 digit width is preserved",
              QStringLiteral("/tmp/project_0099.veditor"),
              QSet<QString>{},
              QStringLiteral("/tmp/project_0100.veditor"),
              passed,
              failed);

    checkPath("G5 width expands when needed",
              QStringLiteral("shot_v999.json"),
              QSet<QString>{},
              QStringLiteral("shot_v1000.json"),
              passed,
              failed);

    std::printf("[versioned-save] summary: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
