// AE-ANIM-1: Elastic/Bounce/Back keyframe easing preset regression gates.

#include "../EasingCurveModel.h"
#include "../Keyframe.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kEps = 1e-6;

bool near(double a, double b, double eps = kEps)
{
    return std::abs(a - b) <= eps;
}

double trackValue(KeyframePoint::Interpolation interpolation, double t)
{
    KeyframeTrack track(QStringLiteral("opacity"), 0.0);
    track.addKeyframe(0.0, 0.0, interpolation);
    track.addKeyframe(1.0, 100.0, KeyframePoint::Linear);
    return track.valueAt(t);
}

bool interpolationRoundTrips(KeyframePoint::Interpolation interpolation)
{
    KeyframePoint kf;
    kf.time = 0.25;
    kf.value = 42.0;
    kf.interpolation = interpolation;

    const QJsonObject json = keyframePointToJson(kf);
    const KeyframePoint loaded = keyframePointFromJson(json);
    return loaded.interpolation == interpolation
        && json[QStringLiteral("interpolation")].toInt(-1) == static_cast<int>(interpolation);
}

} // namespace

int runEasingPresetsSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[easing-presets] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    // G1: backOut endpoints and overshoot.
    {
        bool overshoots = false;
        double maxValue = easing::backOut(0.0);
        for (int i = 501; i < 1000; ++i) {
            const double t = i / 1000.0;
            const double y = easing::backOut(t);
            maxValue = std::max(maxValue, y);
            if (y > 1.0) {
                overshoots = true;
            }
        }
        check(1, "backOut endpoints and overshoot",
              near(easing::backOut(0.0), 0.0)
              && near(easing::backOut(1.0), 1.0)
              && overshoots,
              QStringLiteral("max=%1").arg(maxValue, 0, 'g', 12));
    }

    // G2: bounceOut endpoints, bounded output, and local dip.
    {
        bool bounded = true;
        bool hasDip = false;
        double previous = easing::bounceOut(0.0);
        for (int i = 1; i <= 1000; ++i) {
            const double y = easing::bounceOut(i / 1000.0);
            bounded = bounded && y >= -kEps && y <= 1.0 + kEps;
            if (y < previous - kEps) {
                hasDip = true;
            }
            previous = y;
        }
        check(2, "bounceOut endpoints, bounds, and bounce dip",
              near(easing::bounceOut(0.0), 0.0)
              && near(easing::bounceOut(1.0), 1.0)
              && bounded
              && hasDip);
    }

    // G3: elasticOut endpoints and oscillation around 1.0.
    {
        bool exceedsOne = false;
        bool dipsBelowAfterExceed = false;
        for (int i = 1; i < 1000; ++i) {
            const double y = easing::elasticOut(i / 1000.0);
            if (y > 1.0 + kEps) {
                exceedsOne = true;
            }
            if (exceedsOne && y < 1.0 - kEps) {
                dipsBelowAfterExceed = true;
            }
        }
        check(3, "elasticOut endpoints and oscillation",
              near(easing::elasticOut(0.0), 0.0)
              && near(easing::elasticOut(1.0), 1.0)
              && exceedsOne
              && dipsBelowAfterExceed);
    }

    // G4: BackOut interpolates beyond B when the easing value overshoots; all
    // new modes retain exact endpoints through KeyframeTrack::valueAt.
    {
        bool backOvershootsValue = false;
        for (int i = 1; i < 1000; ++i) {
            if (trackValue(KeyframePoint::BackOut, i / 1000.0) > 100.0) {
                backOvershootsValue = true;
                break;
            }
        }

        const KeyframePoint::Interpolation modes[] = {
            KeyframePoint::ElasticOut,
            KeyframePoint::BounceOut,
            KeyframePoint::BackOut
        };
        bool endpoints = true;
        for (KeyframePoint::Interpolation mode : modes) {
            endpoints = endpoints
                && near(trackValue(mode, 0.0), 0.0)
                && near(trackValue(mode, 1.0), 100.0);
        }
        check(4, "new preset interpolation overshoot and endpoints",
              backOvershootsValue && endpoints);
    }

    // G5: representative legacy interpolation values remain unchanged.
    {
        const bool linear = near(trackValue(KeyframePoint::Linear, 0.5), 50.0);
        const bool easeIn = near(trackValue(KeyframePoint::EaseIn, 0.5), 25.0);
        const bool easeOut = near(trackValue(KeyframePoint::EaseOut, 0.5), 75.0);
        const bool easeInOutSymmetric =
            near(trackValue(KeyframePoint::EaseInOut, 0.25), 12.5)
            && near(trackValue(KeyframePoint::EaseInOut, 0.75), 87.5)
            && near(trackValue(KeyframePoint::EaseInOut, 0.25)
                    + trackValue(KeyframePoint::EaseInOut, 0.75), 100.0);
        const bool hold =
            near(trackValue(KeyframePoint::Hold, 0.999), 0.0)
            && near(trackValue(KeyframePoint::Hold, 1.0), 100.0);

        check(5, "existing interpolation values unchanged",
              linear && easeIn && easeOut && easeInOutSymmetric && hold);
    }

    // G6: JSON round-trip preserves appended preset enum values and an
    // existing value.
    {
        const bool ok =
            interpolationRoundTrips(KeyframePoint::ElasticOut)
            && interpolationRoundTrips(KeyframePoint::BounceOut)
            && interpolationRoundTrips(KeyframePoint::BackOut)
            && interpolationRoundTrips(KeyframePoint::Linear);
        check(6, "interpolation JSON round-trip", ok);
    }

    std::printf("[easing-presets] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
