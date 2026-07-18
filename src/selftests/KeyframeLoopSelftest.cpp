// src/selftests/KeyframeLoopSelftest.cpp
// AE-ANIM-3: loopOut sampling and serialization regression gates.

#include "../Keyframe.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cmath>
#include <cstdio>

namespace {

const QString kProp = QStringLiteral("motion.scale");
constexpr double kEps = 1e-9;

bool near(double a, double b, double eps = kEps)
{
    return std::abs(a - b) <= eps;
}

KeyframeTrack makeTrack()
{
    KeyframeTrack track(kProp, 1.0);
    track.addKeyframe(1.0, 10.0, KeyframePoint::Linear);
    track.addKeyframe(5.0, 50.0, KeyframePoint::Linear);
    return track;
}

QJsonObject firstTrackObject(const KeyframeManager &manager)
{
    const QJsonArray tracks = manager.toJson()[QStringLiteral("tracks")].toArray();
    if (tracks.isEmpty())
        return {};
    return tracks.at(0).toObject();
}

} // namespace

int runKeyframeLoopSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[keyframe-loop] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    // G1: default None preserves the legacy post-last hold.
    {
        KeyframeManager manager;
        manager.addTrack(makeTrack());
        const double afterLast = manager.valueAt(kProp, 7.0, 0.0);
        check(1, "loopOut=None holds after last keyframe",
              manager.loopOutMode(kProp) == LoopMode::None
                  && near(afterLast, 50.0),
              QStringLiteral("value=%1").arg(afterLast, 0, 'g', 12));
    }

    // G2: Cycle folds post-last time back into the original range.
    {
        KeyframeManager manager;
        manager.addTrack(makeTrack());
        manager.setLoopOutMode(kProp, LoopMode::Cycle);

        const double firstT = 1.0;
        const double lastT = 5.0;
        const double range = lastT - firstT;
        const double looped = manager.valueAt(kProp, lastT + 0.5 * range, 0.0);
        const double expected = manager.valueAt(kProp, firstT + 0.5 * range, 0.0);
        check(2, "Cycle remaps to matching in-range sample",
              near(looped, expected),
              QStringLiteral("looped=%1 expected=%2")
                  .arg(looped, 0, 'g', 12)
                  .arg(expected, 0, 'g', 12));
    }

    // G3: PingPong folds post-last time through the reversed phase.
    {
        KeyframeManager manager;
        manager.addTrack(makeTrack());
        manager.setLoopOutMode(kProp, LoopMode::PingPong);

        const double firstT = 1.0;
        const double lastT = 5.0;
        const double range = lastT - firstT;
        const double query = lastT + 0.5 * range;
        const double folded = lastT - 0.5 * range;
        const double looped = manager.valueAt(kProp, query, 0.0);
        const double expected = manager.valueAt(kProp, folded, 0.0);
        check(3, "PingPong remaps to reversed folded sample",
              near(looped, expected),
              QStringLiteral("looped=%1 expected=%2")
                  .arg(looped, 0, 'g', 12)
                  .arg(expected, 0, 'g', 12));
    }

    // G4: Continue extrapolates along the slope of the final segment.
    {
        KeyframeManager manager;
        KeyframeTrack track(kProp, 0.0);
        track.addKeyframe(0.0, 0.0);
        track.addKeyframe(2.0, 10.0);
        track.addKeyframe(5.0, 40.0);
        manager.addTrack(track);
        manager.setLoopOutMode(kProp, LoopMode::Continue);

        const double value = manager.valueAt(kProp, 6.5, 0.0);
        check(4, "Continue extrapolates from last segment slope",
              near(value, 55.0),
              QStringLiteral("value=%1").arg(value, 0, 'g', 12));
    }

    // G5: in-range sampling is identical for every loopOut mode.
    {
        KeyframeManager baseline;
        baseline.addTrack(makeTrack());

        const LoopMode modes[] = {
            LoopMode::None,
            LoopMode::Cycle,
            LoopMode::PingPong,
            LoopMode::Continue
        };
        const double samples[] = {1.0, 2.0, 3.5, 5.0};

        bool ok = true;
        QString detail;
        for (LoopMode mode : modes) {
            KeyframeManager manager;
            manager.addTrack(makeTrack());
            manager.setLoopOutMode(kProp, mode);
            for (double t : samples) {
                const double expected = baseline.valueAt(kProp, t, 0.0);
                const double actual = manager.valueAt(kProp, t, 0.0);
                if (actual != expected) {
                    ok = false;
                    detail = QStringLiteral("mode=%1 t=%2 actual=%3 expected=%4")
                        .arg(static_cast<int>(mode))
                        .arg(t, 0, 'g', 12)
                        .arg(actual, 0, 'g', 17)
                        .arg(expected, 0, 'g', 17);
                    break;
                }
            }
            if (!ok)
                break;
        }
        check(5, "in-range values unchanged for all loop modes", ok, detail);
    }

    // G6: non-None loopOut round-trips; None remains omitted from JSON.
    {
        KeyframeManager looped;
        looped.addTrack(makeTrack());
        looped.setLoopOutMode(kProp, LoopMode::PingPong);

        const QJsonObject loopTrack = firstTrackObject(looped);
        KeyframeManager loaded;
        loaded.fromJson(looped.toJson());

        KeyframeManager plain;
        plain.addTrack(makeTrack());
        const QJsonObject plainTrack = firstTrackObject(plain);

        looped.setLoopOutMode(kProp, LoopMode::None);
        const QJsonObject clearedTrack = firstTrackObject(looped);

        const bool ok =
            loopTrack[QStringLiteral("loopOut")].toString() == QStringLiteral("PingPong")
            && loaded.loopOutMode(kProp) == LoopMode::PingPong
            && !plainTrack.contains(QStringLiteral("loopOut"))
            && !clearedTrack.contains(QStringLiteral("loopOut"));
        check(6, "loopOut JSON round-trip and None omission", ok);
    }

    std::printf("[keyframe-loop] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
