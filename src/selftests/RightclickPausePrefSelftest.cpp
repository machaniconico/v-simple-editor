#include "../util/RcPause.h"

#include <cstdio>

int runRightclickPausePrefSelftest()
{
    int passed = 0;
    int failed = 0;
    constexpr bool kDefault = rcpause::kDefaultPauseOnRightClick;

    auto check = [&](const char *gate, bool ok) {
        std::printf("[rightclick-pause-pref] %s: %s\n", gate, ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    };

    check("G1 unset uses default",
          rcpause::resolvePauseOnRightClick(QString(), kDefault) == kDefault);

    check("G2 true/false strings",
          rcpause::resolvePauseOnRightClick(QStringLiteral("true"), false)
          && !rcpause::resolvePauseOnRightClick(QStringLiteral("false"), true));

    check("G3 garbage uses default",
          rcpause::resolvePauseOnRightClick(QStringLiteral("garbage_value_xyz"), kDefault)
          == kDefault);

    check("G4 1/0 strings",
          rcpause::resolvePauseOnRightClick(QStringLiteral("1"), false)
          && !rcpause::resolvePauseOnRightClick(QStringLiteral("0"), true));

    std::printf("[rightclick-pause-pref] summary: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
