// Sprint 22 — US-EASE-1: easing curve model + cubic-bezier solver impl.
#include "EasingCurveModel.h"

namespace easing {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

// Parametric cubic bezier with fixed endpoints P0=(0,0), P3=(1,1) and the
// two inner control points c1, c2 (one axis at a time).
//   B(u) = 3(1-u)^2 u c1 + 3(1-u) u^2 c2 + u^3
inline double bezierAxis(double u, double c1, double c2) {
    const double mu = 1.0 - u;
    return 3.0 * mu * mu * u * c1
         + 3.0 * mu * u * u * c2
         + u * u * u;
}

// d/du of bezierAxis.
inline double bezierAxisDeriv(double u, double c1, double c2) {
    const double mu = 1.0 - u;
    return 3.0 * mu * mu * c1
         + 6.0 * mu * u * (c2 - c1)
         + 3.0 * u * u * (1.0 - c2);
}

// Solve bezierX(u) == x for u in [0,1]. Newton-Raphson (8 iters) seeded with
// u = x, with a robust bisection fallback when the derivative is degenerate
// or Newton steps out of range.
double solveBezierParam(double x, double x1, double x2) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;

    double u = x; // good initial guess for monotone-ish curves
    for (int i = 0; i < 8; ++i) {
        const double fx = bezierAxis(u, x1, x2) - x;
        if (std::fabs(fx) < 1e-7)
            return u;
        const double d = bezierAxisDeriv(u, x1, x2);
        if (std::fabs(d) < 1e-7)
            break; // derivative too small — fall back to bisection
        const double next = u - fx / d;
        if (next <= 0.0 || next >= 1.0 || !std::isfinite(next))
            break; // stepped out of range — fall back to bisection
        u = next;
    }

    // Bisection fallback on [lo, hi]. bezierX is monotone non-decreasing for
    // valid CSS control points (x1, x2 in [0,1]), so this always converges.
    double lo = 0.0;
    double hi = 1.0;
    u = x;
    for (int i = 0; i < 60; ++i) {
        const double fx = bezierAxis(u, x1, x2);
        if (std::fabs(fx - x) < 1e-7)
            return u;
        if (fx < x)
            lo = u;
        else
            hi = u;
        u = 0.5 * (lo + hi);
    }
    return u;
}

} // namespace

double elasticOut(double t) {
    if (t == 0.0)
        return 0.0;
    if (t == 1.0)
        return 1.0;
    return std::pow(2.0, -10.0 * t) * std::sin((t * 10.0 - 0.75) * (2.0 * kPi / 3.0)) + 1.0;
}

double bounceOut(double t) {
    constexpr double n1 = 7.5625;
    constexpr double d1 = 2.75;

    if (t < 1.0 / d1) {
        return n1 * t * t;
    } else if (t < 2.0 / d1) {
        t -= 1.5 / d1;
        return n1 * t * t + 0.75;
    } else if (t < 2.5 / d1) {
        t -= 2.25 / d1;
        return n1 * t * t + 0.9375;
    }

    t -= 2.625 / d1;
    return n1 * t * t + 0.984375;
}

double backOut(double t) {
    constexpr double c1 = 1.70158;
    constexpr double c3 = c1 + 1.0;
    const double u = t - 1.0;
    return 1.0 + c3 * std::pow(u, 3.0) + c1 * std::pow(u, 2.0);
}

double evaluate(EasingType type, double t, const CubicBezier &bez) {
    switch (type) {
    case EasingType::Linear:
        // MUST be exact identity.
        return t;
    case EasingType::EaseIn: {
        const double c = clamp01(t);
        return c * c;
    }
    case EasingType::EaseOut: {
        const double c = clamp01(t);
        return 1.0 - (1.0 - c) * (1.0 - c);
    }
    case EasingType::EaseInOut: {
        const double c = clamp01(t);
        if (c < 0.5)
            return 2.0 * c * c;
        const double k = -2.0 * c + 2.0;
        return 1.0 - (k * k) / 2.0;
    }
    case EasingType::Step:
        return (clamp01(t) < 0.5) ? 0.0 : 1.0;
    case EasingType::CubicBezier: {
        const double c = clamp01(t);
        const double u = solveBezierParam(c, bez.x1, bez.x2);
        return bezierAxis(u, bez.y1, bez.y2);
    }
    }
    return t;
}

QVector<NamedCurve> presets() {
    QVector<NamedCurve> v;
    // Standard CSS / easings.net control-point values.
    v.push_back({ QStringLiteral("easeInSine"),    { 0.12, 0.0,  0.39, 0.0  } });
    v.push_back({ QStringLiteral("easeOutSine"),   { 0.61, 1.0,  0.88, 1.0  } });
    v.push_back({ QStringLiteral("easeInOutSine"), { 0.37, 0.0,  0.63, 1.0  } });
    v.push_back({ QStringLiteral("easeInQuad"),    { 0.11, 0.0,  0.5,  0.0  } });
    v.push_back({ QStringLiteral("easeOutQuad"),   { 0.5,  1.0,  0.89, 1.0  } });
    v.push_back({ QStringLiteral("easeInOutCubic"),{ 0.65, 0.0,  0.35, 1.0  } });
    v.push_back({ QStringLiteral("easeInBack"),    { 0.36, 0.0,  0.66, -0.56 } });
    v.push_back({ QStringLiteral("easeOutBack"),   { 0.34, 1.56, 0.64, 1.0  } });
    return v;
}

} // namespace easing
