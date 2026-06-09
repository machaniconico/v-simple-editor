#include "../CaptionCps.h"

#include <QString>

#include <cmath>
#include <cstdio>

namespace {

bool closeTo(double got, double expected)
{
    return std::fabs(got - expected) < 1.0e-9;
}

void printGate(const char* gate, bool ok, int& passed, int& failed)
{
    std::printf("%s: %s\n", gate, ok ? "PASS" : "FAIL");
    ok ? ++passed : ++failed;
}

} // namespace

int runSubtitleCpsSelftest()
{
    int passed = 0;
    int failed = 0;

    const QString hello = QStringLiteral("Hello");
    printGate("G1",
              closeTo(captioncps::cps(hello, 1.0), 5.0)
                  && !captioncps::exceeds(hello, 1.0),
              passed,
              failed);

    printGate("G2",
              closeTo(captioncps::cps(hello, 0.2), 25.0)
                  && captioncps::exceeds(hello, 0.2),
              passed,
              failed);

    const QString spaced = QStringLiteral("ab cd\nef");
    printGate("G3",
              closeTo(captioncps::cps(spaced, 1.0), 6.0),
              passed,
              failed);

    printGate("G4",
              captioncps::cps(QStringLiteral("anything"), 0.0) >= 1.0e8
                  && captioncps::exceeds(QStringLiteral("anything"), 0.0),
              passed,
              failed);

    const QString boundary = QStringLiteral("abcdefghijklmnopqrst");
    printGate("G5",
              closeTo(captioncps::cps(boundary, 1.0), 20.0)
                  && !captioncps::exceeds(boundary, 1.0),
              passed,
              failed);

    printGate("G6",
              closeTo(captioncps::cps(QString::fromUtf8("あいう"), 1.0), 3.0),
              passed,
              failed);

    std::printf("[subtitle-cps] summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
