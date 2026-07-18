// src/selftests/bezier_easing_selftest.cpp
// S3: per-keyframe cubic-bezier easing regression gates.

#include "../EasingCurveModel.h"
#include "../Keyframe.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cmath>
#include <cstdio>

namespace {

constexpr double kEps = 1e-6;

bool near(double a, double b, double eps = kEps)
{
    return std::abs(a - b) <= eps;
}

double expectedLegacyValue(KeyframePoint::Interpolation interpolation, double t)
{
    switch (interpolation) {
    case KeyframePoint::Linear:
        return 10.0 * t;
    case KeyframePoint::EaseIn:
        return 10.0 * t * t;
    case KeyframePoint::EaseOut:
        return 10.0 * t * (2.0 - t);
    case KeyframePoint::EaseInOut:
        return 10.0 * (t < 0.5 ? 2.0 * t * t : -1.0 + (4.0 - 2.0 * t) * t);
    case KeyframePoint::Hold:
        return 0.0;
    case KeyframePoint::Bezier:
        break;
    }
    return 10.0 * t;
}

bool checkTrackAt(KeyframePoint::Interpolation interpolation, double t)
{
    KeyframeTrack track(QStringLiteral("opacity"), 0.0);
    track.addKeyframe(0.0, 0.0, interpolation);
    track.addKeyframe(1.0, 10.0, KeyframePoint::Linear);
    return near(track.valueAt(t), expectedLegacyValue(interpolation, t));
}

QJsonObject firstKeyframeObject(const KeyframeManager &manager)
{
    const QJsonObject root = manager.toJson();
    const QJsonArray tracks = root[QStringLiteral("tracks")].toArray();
    if (tracks.isEmpty()) {
        return {};
    }
    const QJsonArray keyframes =
        tracks.at(0).toObject()[QStringLiteral("keyframes")].toArray();
    if (keyframes.isEmpty()) {
        return {};
    }
    return keyframes.at(0).toObject();
}

bool hasAnyBezierControlKey(const QJsonObject &obj)
{
    return obj.contains(QStringLiteral("bezX1"))
        || obj.contains(QStringLiteral("bezY1"))
        || obj.contains(QStringLiteral("bezX2"))
        || obj.contains(QStringLiteral("bezY2"));
}

} // namespace

int runBezierEasingSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[bezier-easing] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    // G1: legacy interpolation math is unchanged for every non-Bezier mode.
    {
        bool ok = true;
        const double samples[] = {0.25, 0.5, 0.75};
        const KeyframePoint::Interpolation modes[] = {
            KeyframePoint::Linear,
            KeyframePoint::EaseIn,
            KeyframePoint::EaseOut,
            KeyframePoint::EaseInOut,
            KeyframePoint::Hold
        };
        for (KeyframePoint::Interpolation mode : modes) {
            for (double t : samples) {
                ok = ok && checkTrackAt(mode, t);
            }
        }
        check(1, "non-Bezier interpolation values unchanged", ok);
    }

    // G2: cubic-bezier(0,0,1,1) is identity and matches Linear.
    {
        bool ok = true;
        KeyframeTrack track(QStringLiteral("opacity"), 0.0);
        track.addKeyframe(0.0, 0.0, KeyframePoint::Bezier, 0.0, 0.0, 1.0, 1.0);
        track.addKeyframe(1.0, 10.0, KeyframePoint::Linear);
        const double samples[] = {0.1, 0.25, 0.5, 0.75, 0.9};
        for (double t : samples) {
            ok = ok && near(track.valueAt(t), 10.0 * t);
        }
        check(2, "identity Bezier matches Linear", ok);
    }

    // G3: CSS ease-in accelerates, so the halfway value is below Linear.
    {
        KeyframeTrack track(QStringLiteral("opacity"), 0.0);
        track.addKeyframe(0.0, 0.0, KeyframePoint::Bezier, 0.42, 0.0, 1.0, 1.0);
        track.addKeyframe(1.0, 10.0, KeyframePoint::Linear);
        const double value = track.valueAt(0.5);
        check(3, "ease-in Bezier midpoint below Linear",
              value < 5.0,
              QStringLiteral("value=%1").arg(value, 0, 'g', 12));
    }

    // G4: Bezier metadata round-trips; non-Bezier JSON keeps the legacy shape.
    {
        KeyframeManager manager;
        KeyframeTrack bezierTrack(QStringLiteral("opacity"), 1.0);
        bezierTrack.addKeyframe(0.0, 0.0, KeyframePoint::Bezier, 0.42, 0.0, 1.0, 1.0);
        bezierTrack.addKeyframe(1.0, 10.0, KeyframePoint::Linear);
        manager.addTrack(bezierTrack);

        const QJsonObject bezierJson = manager.toJson();
        const QJsonObject bezierKf = firstKeyframeObject(manager);
        KeyframeManager loaded;
        loaded.fromJson(bezierJson);
        const KeyframeTrack *loadedTrack = loaded.track(QStringLiteral("opacity"));
        bool ok = loadedTrack && loadedTrack->count() == 2;
        if (ok) {
            const KeyframePoint &kf = loadedTrack->keyframes().first();
            ok = kf.interpolation == KeyframePoint::Bezier
              && near(kf.bezX1, 0.42)
              && near(kf.bezY1, 0.0)
              && near(kf.bezX2, 1.0)
              && near(kf.bezY2, 1.0)
              && bezierKf[QStringLiteral("interp")].toString() == QStringLiteral("Bezier")
              && bezierKf.contains(QStringLiteral("bezX1"))
              && bezierKf.contains(QStringLiteral("bezY1"))
              && bezierKf.contains(QStringLiteral("bezX2"))
              && bezierKf.contains(QStringLiteral("bezY2"));
        }

        KeyframeManager legacyManager;
        KeyframeTrack legacyTrack(QStringLiteral("opacity"), 1.0);
        legacyTrack.addKeyframe(0.0, 0.0, KeyframePoint::Linear);
        legacyTrack.addKeyframe(1.0, 10.0, KeyframePoint::EaseInOut);
        legacyManager.addTrack(legacyTrack);
        const QJsonObject legacyKf = firstKeyframeObject(legacyManager);
        ok = ok
          && !legacyKf.contains(QStringLiteral("interp"))
          && !hasAnyBezierControlKey(legacyKf);

        KeyframeManager identityManager;
        KeyframeTrack identityTrack(QStringLiteral("opacity"), 1.0);
        identityTrack.addKeyframe(0.0, 0.0, KeyframePoint::Bezier, 0.0, 0.0, 1.0, 1.0);
        identityTrack.addKeyframe(1.0, 10.0, KeyframePoint::Linear);
        identityManager.addTrack(identityTrack);
        const QJsonObject identityKf = firstKeyframeObject(identityManager);
        ok = ok
          && identityKf[QStringLiteral("interp")].toString() == QStringLiteral("Bezier")
          && !hasAnyBezierControlKey(identityKf);

        check(4, "Bezier JSON round-trip and legacy non-Bezier JSON shape", ok);
    }

    std::printf("[bezier-easing] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
