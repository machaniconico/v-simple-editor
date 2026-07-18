#include <QDebug>

#include "../AudioMixer.h"

namespace {

struct Case {
    bool envForce;
    bool perClipFlag;
    bool expected;
    const char *name;
};

} // namespace

int runAtempoResolveSelftest()
{
    qInfo().noquote() << "[atempo-resolve] selftest start";
    int passed = 0;
    int failed = 0;
    auto pass = [&](const char *name) {
        ++passed;
        qInfo().noquote() << "[atempo-resolve] PASS" << name;
    };
    auto fail = [&](const char *name, bool actual, bool expected) {
        ++failed;
        qWarning().noquote() << "[atempo-resolve] FAIL" << name
                             << ": actual=" << actual
                             << "expected=" << expected;
    };

    const Case cases[] = {
        {false, false, false, "env off + clip off"},
        {false, true,  true,  "env off + clip on"},
        {true,  false, true,  "env on + clip off"},
        {true,  true,  true,  "env on + clip on"},
    };

    for (const Case &c : cases) {
        const bool actual = resolveAudioAtempoEnabled(c.envForce, c.perClipFlag);
        if (actual == c.expected)
            pass(c.name);
        else
            fail(c.name, actual, c.expected);
    }

    qInfo().noquote() << "[atempo-resolve] summary passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
