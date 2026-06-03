// hdr_overlay_policy_selftest.cpp
// Headless selftest for HDR Stage4 overlay RGBA64 policy gates.
// Run via: --selftest=hdr-overlay-policy

#include <cstdio>

#include <QByteArray>
#include <QtGlobal>

#include "../playback/hdrexport16_flag.h"
#include "../playback/hdroverlay_flag.h"

namespace {

struct EnvGuard {
    EnvGuard()
        : had(qEnvironmentVariableIsSet("VEDITOR_HDR_OVERLAY")),
          value(qgetenv("VEDITOR_HDR_OVERLAY"))
    {
    }

    ~EnvGuard()
    {
        if (had)
            qputenv("VEDITOR_HDR_OVERLAY", value);
        else
            qunsetenv("VEDITOR_HDR_OVERLAY");
    }

    bool had = false;
    QByteArray value;
};

} // namespace

int runHdrOverlayPolicySelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[hdr-overlay-policy] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    EnvGuard envGuard;

    check(1, "flag=false isHdr=false routes RGB888",
          !hdroverlay::wantRgba64(false, false));

    check(2, "flag=false isHdr=true routes RGB888",
          !hdroverlay::wantRgba64(false, true));

    check(3, "flag=true isHdr=false routes RGB888",
          !hdroverlay::wantRgba64(true, false));

    check(4, "flag=true isHdr=true routes RGBA64",
          hdroverlay::wantRgba64(true, true));

    qputenv("VEDITOR_HDR_OVERLAY", QByteArray("1"));
    check(5, "env == 1 enables overlay RGBA64 policy",
          hdroverlay::enabledFromEnv()
          && hdroverlay::enabledFromEnvValue() == QByteArrayLiteral("1"));

    qunsetenv("VEDITOR_HDR_OVERLAY");
    check(6, "unset env disables overlay RGBA64 policy",
          !hdroverlay::enabledFromEnv()
          && hdroverlay::enabledFromEnvValue().isEmpty());

    check(7, "mixed SDR V1 plus HDR overlay stays on 8-bit preview route",
          !hdrexport16::preview16Applicable(true, false, 2, false));

    qputenv("VEDITOR_HDR_OVERLAY", QByteArray("0"));
    const bool rejectsZero =
        !hdroverlay::enabledFromEnv()
        && hdroverlay::enabledFromEnvValue() == QByteArrayLiteral("0");
    qputenv("VEDITOR_HDR_OVERLAY", QByteArray("2"));
    check(8, "env values other than 1 stay disabled",
          rejectsZero
          && !hdroverlay::enabledFromEnv()
          && hdroverlay::enabledFromEnvValue() == QByteArrayLiteral("2"));

    std::printf("[hdr-overlay-policy] summary: gates=8 passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
