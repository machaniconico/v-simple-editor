#include "../ExportRange.h"

#include <cstdio>
#include <cstdint>

namespace {

void checkRange(const char *gate,
                const char *desc,
                const ExportFrameRange &got,
                int64_t expectedStart,
                int64_t expectedEnd,
                int &passed,
                int &failed)
{
    const bool ok = got.startFrame == expectedStart
        && got.endFrame == expectedEnd
        && got.frameCount() == expectedEnd - expectedStart;
    std::printf("[export-range] %s %s %s got=[%lld,%lld) frames=%lld expected=[%lld,%lld) frames=%lld\n",
                ok ? "PASS" : "FAIL",
                gate,
                desc,
                static_cast<long long>(got.startFrame),
                static_cast<long long>(got.endFrame),
                static_cast<long long>(got.frameCount()),
                static_cast<long long>(expectedStart),
                static_cast<long long>(expectedEnd),
                static_cast<long long>(expectedEnd - expectedStart));
    ok ? ++passed : ++failed;
}

} // namespace

int runExportRangeSelftest()
{
    int passed = 0;
    int failed = 0;

    checkRange("G1",
               "disabled returns whole timeline",
               computeExportRange(2.0, 5.0, 30.0, 300, false, true),
               0,
               300,
               passed,
               failed);

    checkRange("G2",
               "2s to 5s at 30fps",
               computeExportRange(2.0, 5.0, 30.0, 300, true, true),
               60,
               150,
               passed,
               failed);

    checkRange("G3",
               "enabled without valid marked range falls back",
               computeExportRange(-1.0, 5.0, 30.0, 300, true, false),
               0,
               300,
               passed,
               failed);

    checkRange("G4",
               "mark out beyond duration clamps to total frames",
               computeExportRange(8.0, 20.0, 10.0, 100, true, true),
               80,
               100,
               passed,
               failed);

    checkRange("G5",
               "empty marked range falls back",
               computeExportRange(4.0, 4.0, 30.0, 300, true, true),
               0,
               300,
               passed,
               failed);

    std::printf("[export-range] summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
