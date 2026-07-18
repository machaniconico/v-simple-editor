// preview16_policy_selftest.cpp
// Headless selftest for HDR Stage3 preview16 policy gates.
// Run via: --selftest=preview16-policy

#include <cstdio>

#include <QImage>

#include "../playback/TlrCompose16.h"
#include "../playback/hdrexport16_flag.h"

int runPreview16PolicySelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[preview16-policy] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    check(1, "flag=false rejects otherwise-applicable preview16 path",
          !hdrexport16::preview16Applicable(false, false, 2, true));

    check(2, "hasMatte=true rejects preview16 path",
          !hdrexport16::preview16Applicable(true, true, 2, true));

    check(3, "layerCount=1 rejects preview16 path",
          !hdrexport16::preview16Applicable(true, false, 1, true));

    check(4, "allRgba64=false rejects preview16 path",
          !hdrexport16::preview16Applicable(true, false, 2, false));

    check(5, "flag ON matte-free 2 RGBA64 layers accepts preview16 path",
          hdrexport16::preview16Applicable(true, false, 2, true));

    check(6, "flag ON matte-free 3 RGBA64 layers accepts preview16 path",
          hdrexport16::preview16Applicable(true, false, 3, true));

    check(7, "RGBA64 formats are accepted",
          tlrcompose16::isRgba64Format(QImage::Format_RGBA64)
          && tlrcompose16::isRgba64Format(QImage::Format_RGBA64_Premultiplied));

    check(8, "8-bit formats are rejected",
          !tlrcompose16::isRgba64Format(QImage::Format_ARGB32)
          && !tlrcompose16::isRgba64Format(QImage::Format_ARGB32_Premultiplied)
          && !tlrcompose16::isRgba64Format(QImage::Format_RGBA8888));

    std::printf("[preview16-policy] summary: gates=8 passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
