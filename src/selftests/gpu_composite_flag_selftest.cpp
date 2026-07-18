// gpu_composite_flag_selftest.cpp
// Headless selftest for the pure VEDITOR_GPU_COMPOSITE/settings resolver.
// Run via: --selftest=gpu-composite-flag

#include <cstdio>

#include <QByteArray>

#include "../gpucomposite_flag.h"

int runGpuCompositeFlagSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[gpu-composite-flag] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    check(1, "env absent + settings=false -> false",
          !resolveGpuCompositeEnabled(false, QByteArray(), false));

    check(2, "env absent + settings=true -> true",
          resolveGpuCompositeEnabled(false, QByteArray(), true));

    check(3, "env=1 overrides settings=false -> true",
          resolveGpuCompositeEnabled(true, QByteArray("1"), false));

    check(4, "env=0 overrides settings=true -> false",
          !resolveGpuCompositeEnabled(true, QByteArray("0"), true));

    {
        const QByteArray emptyEnv;
        check(5, "empty env is treated as absent by caller -> settings",
              resolveGpuCompositeEnabled(!emptyEnv.isEmpty(), emptyEnv, true));
    }

    check(6, "env=yes is not 1 and overrides settings=true -> false",
          !resolveGpuCompositeEnabled(true, QByteArray("yes"), true));

    std::printf("[gpu-composite-flag] summary: passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
