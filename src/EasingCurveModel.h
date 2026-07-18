#pragma once
// Sprint 22 — US-EASE-1: easing curve model + cubic-bezier solver.
#include <QObject>
#include <QVector>
#include <QString>
#include <cmath>

namespace easing {

enum class EasingType {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    CubicBezier,
    Step
};

// Cubic-bezier control points. Endpoints are fixed at (0,0) and (1,1) per
// standard CSS cubic-bezier semantics; only the two inner control points
// are stored. Defaults match the CSS "ease" curve.
struct CubicBezier {
    double x1 = 0.25;
    double y1 = 0.1;
    double x2 = 0.25;
    double y2 = 1.0;
};

// Evaluate an easing curve. t is clamped to [0,1]; return value is the eased
// progress (~[0,1], may slightly overshoot for back/elastic style beziers).
// For CubicBezier we solve the parametric bezier: given x = t, find the
// curve parameter u with bezierX(u) == t (Newton-Raphson + bisection
// fallback), then return bezierY(u).
double evaluate(EasingType type, double t, const CubicBezier &bez = {});

double elasticOut(double t);
double bounceOut(double t);
double backOut(double t);

// A named cubic-bezier preset (CSS-style control points).
struct NamedCurve {
    QString     name;
    CubicBezier bez;
};

// Well-known easing presets (>= 8 entries).
QVector<NamedCurve> presets();

} // namespace easing
