#include "../RenderQueue.h"

#include <cstdio>

namespace {
bool check(bool condition,
           const char *gate,
           const char *message,
           int &passed,
           int &failed)
{
    std::printf("[loudness-export-wire] %s %s %s\n",
                condition ? "PASS" : "FAIL",
                gate,
                message);
    condition ? ++passed : ++failed;
    return condition;
}
} // namespace

int runLoudnessExportWireSelftest()
{
    int passed = 0;
    int failed = 0;

    {
        const QStringList args = RenderQueue::buildLoudnessAudioFilterArgs(0.0);
        check(args.isEmpty(), "G1", "0 dB emits no audio filter",
              passed, failed);
    }

    {
        const QStringList args = RenderQueue::buildLoudnessAudioFilterArgs(2.25);
        check(args.size() == 2
                  && args[0] == QStringLiteral("-af")
                  && args[1] == QStringLiteral(
                         "volume=2.25dB,alimiter=limit=0.98"),
              "G2", "+gain emits volume dB plus limiter",
              passed, failed);
    }

    {
        const QStringList args = RenderQueue::buildLoudnessAudioFilterArgs(-6.0);
        check(args.size() == 2
                  && args[0] == QStringLiteral("-af")
                  && args[1] == QStringLiteral(
                         "volume=-6dB,alimiter=limit=0.98"),
              "G3", "-gain emits volume dB plus limiter",
              passed, failed);
    }

    {
        const QStringList args = RenderQueue::buildLoudnessAudioFilterArgs(0.0001);
        check(args.isEmpty(), "G4", "sub-threshold gain preserves byte identity",
              passed, failed);
    }

    std::printf("[loudness-export-wire] summary: gates=4 passed=%d failed=%d\n",
                passed,
                failed);
    return failed == 0 ? 0 : 1;
}
