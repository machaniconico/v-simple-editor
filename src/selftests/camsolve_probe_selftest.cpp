// Measures how a camera solve (R, t in the CV convention, +z in front) maps onto
// Camera3DState + the projectLayerQuad projection formula (Camera3D.cpp). Print
// only apart from the G1 sanity condition; the numbers feed the Wave 18 design.
#include "../CameraSolver.h"
#include "../Camera3D.h"
#include "../TwoViewGeometry.h"
#include "SelftestGate.h"

#include <QMatrix4x4>
#include <QPointF>
#include <QSize>
#include <QVector3D>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <random>

namespace {
constexpr double pi = 3.14159265358979323846;
const camsolve::Intrinsics k{1000, 960, 540}; // 1920x1080 analysis image (as sfm_two_view)
const double translation[3] = {0.3, 0.05, 0.1};
const double baseline = std::sqrt(0.3 * 0.3 + 0.05 * 0.05 + 0.1 * 0.1);
const QSize analysisSize(1920, 1080);

void rotation(double* r)
{
    const double y = 8 * pi / 180, p = 3 * pi / 180;
    const double m[9] = {std::cos(y), std::sin(y) * std::sin(p), std::sin(y) * std::cos(p),
        0, std::cos(p), -std::sin(p), -std::sin(y), std::cos(y) * std::sin(p), std::cos(y) * std::cos(p)};
    for (int i = 0; i < 9; ++i) r[i] = m[i];
}

QPointF projectCv(const QVector3D& p, const double* r, const double* t)
{
    double b[3];
    for (int i = 0; i < 3; ++i) b[i] = r[3 * i] * p.x() + r[3 * i + 1] * p.y() + r[3 * i + 2] * p.z() + t[i];
    return {k.f * b[0] / b[2] + k.cx, k.f * b[1] / b[2] + k.cy};
}

// Same cube as sfm_two_view_selftest.cpp scene(): x,y in [-1,1], z in [4,6].
QVector<QVector3D> cube(int count)
{
    std::mt19937 rng(48213);
    const auto uniform = [&]() { return (double(rng()) + 0.5) / 4294967296.0; };
    QVector<QVector3D> pts;
    for (int i = 0; i < count; ++i) {
        const float x = float(2 * uniform() - 1), y = float(2 * uniform() - 1), z = float(4 + 2 * uniform());
        pts.append(QVector3D(x, y, z));
    }
    return pts;
}

// The projectLayerQuad formula (Camera3D.cpp) for a world point already in layer
// pixels (a 3D layer corner at layer-space (x, y) with positionZ = z, no rotation).
QPointF projectWorld(const QVector3D& world, const Camera3DState& cam, const QSize& canvas)
{
    QVector3D forward = cam.target - cam.position;
    if (forward.lengthSquared() < 1e-12f) forward = QVector3D(0, 0, -1);
    forward.normalize();
    const QVector3D up = std::abs(QVector3D::dotProduct(forward, QVector3D(0, 1, 0))) > 0.9999f
        ? QVector3D(0, 0, 1) : QVector3D(0, 1, 0);
    QMatrix4x4 view;
    view.lookAt(cam.position, cam.position + forward, up);
    const QVector3D v = view.map(world);
    const double angle = cam.roll * pi / 180.0;
    const double c = std::cos(angle), s = std::sin(angle);
    const double x = c * v.x() + s * v.y();
    const double y = -s * v.x() + c * v.y();
    const double scale = cam.fov / std::max(0.001, cam.fov + v.z());
    return {canvas.width() / 2.0 + x * scale, canvas.height() / 2.0 + y * scale};
}

struct Stats { double median = 0, maximum = 0, radius = 0; };

enum class Mapping { Head, Proposed, ProposedWrongSign };

// Projects every cube point through the mapped camera and compares with the
// observation (analysis px scaled by kScale into canvas px).
Stats measure(const QVector<QVector3D>& pts, const camsolve::Pose& pose, const double* rObs,
              const double* tObs, Mapping mapping, double S, const QSize& canvas)
{
    const double kScale = double(canvas.width()) / analysisSize.width();
    Camera3DState cam = camsolve::poseToCameraState(pose, Camera3DState{});
    cam.trueProjection = true;
    cam.fov = k.f * kScale; // f_analysis x canvas / analysis
    QVector3D position = cam.position * float(S);
    if (mapping != Mapping::Head) {
        const QVector3D r3(float(pose.R[6]), float(pose.R[7]), float(pose.R[8])); // CV optical axis
        const float sign = mapping == Mapping::Proposed ? 1.0f : -1.0f;
        position += sign * float(cam.fov) * r3;
    }
    const QVector3D forward = cam.target - cam.position;
    cam.position = position;
    cam.target = position + forward;
    QVector<double> errors;
    double radius = 0;
    for (const QVector3D& p : pts) {
        const QPointF observed = projectCv(p, rObs, tObs) * kScale;
        const QPointF got = projectWorld(p * float(S), cam, canvas);
        errors.append(std::hypot(got.x() - observed.x(), got.y() - observed.y()));
        radius = std::max(radius, std::hypot(got.x() - canvas.width() / 2.0, got.y() - canvas.height() / 2.0));
    }
    std::sort(errors.begin(), errors.end());
    return {(errors[errors.size() / 2 - 1] + errors[errors.size() / 2]) / 2, errors.back(), radius};
}

// Proposed mapping evaluated in double precision with the same lookAt basis and
// roll as poseToCameraState / projectLayerQuad: separates a convention error from
// the float32 quantisation of Camera3DState.position (eye = S*c + f*R3).
double measureDouble(const QVector<QVector3D>& pts, const camsolve::Pose& pose, const double* rObs,
                     const double* tObs, double S, const QSize& canvas)
{
    using V = std::array<double, 3>;
    const auto dot = [](const V& a, const V& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
    const auto cross = [](const V& a, const V& b) { return V{a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; };
    const auto norm = [&](V a) { const double l = std::sqrt(dot(a, a)); return V{a[0]/l, a[1]/l, a[2]/l}; };
    const double kScale = double(canvas.width()) / analysisSize.width();
    const double fov = k.f * kScale;
    const double R[9] = {pose.R[0], pose.R[1], pose.R[2], pose.R[3], pose.R[4], pose.R[5], pose.R[6], pose.R[7], pose.R[8]};
    const double t[3] = {pose.t.x(), pose.t.y(), pose.t.z()};
    V c, forward{-R[6], -R[7], -R[8]};
    for (int i = 0; i < 3; ++i) c[i] = -(R[i]*t[0] + R[3+i]*t[1] + R[6+i]*t[2]);
    const V eye{S*c[0] + fov*R[6], S*c[1] + fov*R[7], S*c[2] + fov*R[8]};
    const V up = std::abs(forward[1]) > 0.9999 ? V{0, 0, 1} : V{0, 1, 0};
    const V right = norm(cross(forward, up));
    const V viewUp = norm(cross(right, forward));
    const V poseRight{R[0], R[1], R[2]};
    const double roll = std::atan2(dot(poseRight, viewUp), dot(poseRight, right));
    const double cr = std::cos(roll), sr = std::sin(roll);
    double worst = 0;
    for (const QVector3D& p : pts) {
        const V d{S*p.x() - eye[0], S*p.y() - eye[1], S*p.z() - eye[2]};
        const double vx = dot(right, d), vy = dot(viewUp, d), vz = -dot(forward, d);
        const double x = cr*vx + sr*vy, y = -sr*vx + cr*vy;
        const double scale = fov / std::max(0.001, fov + vz);
        const QPointF got(canvas.width() / 2.0 + x*scale, canvas.height() / 2.0 + y*scale);
        const QPointF observed = projectCv(p, rObs, tObs) * kScale;
        worst = std::max(worst, std::hypot(got.x() - observed.x(), got.y() - observed.y()));
    }
    return worst;
}

camsolve::Pose poseFrom(const double* r, const double* t)
{
    camsolve::Pose p;
    for (int i = 0; i < 9; ++i) p.R[i] = r[i];
    p.t = QVector3D(float(t[0]), float(t[1]), float(t[2]));
    p.valid = true;
    return p;
}

void line(const char* label, const Stats& s)
{
    std::cerr << "camsolve-probe " << label << ": median=" << s.median << " px, max=" << s.maximum
              << " px, spread radius=" << s.radius << " px\n";
}
} // namespace

int runCamsolveProbeSelftest()
{
    selftest::GateTally tally;
    const auto pts = cube(200);
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, zero[3] = {};
    double r[9]; rotation(r);
    const QSize canvases[] = {analysisSize, QSize(1280, 720)};

    std::cerr << "camsolve-probe intrinsics: f=" << k.f << " cx=" << k.cx << " cy=" << k.cy
              << " (sfm_two_view_selftest.cpp k, analysis 1920x1080); k=canvas/analysis;"
              << " proposed: eye=S*c+f*(+R3), fov=f*k\n";

    double worstProposed = 0, worstDouble = 0, worstS1 = 0, bestHead = 1e30, bestWrongSign = 1e30;
    for (const QSize& canvas : canvases) {
        const double kScale = double(canvas.width()) / analysisSize.width();
        std::cerr << "camsolve-probe canvas " << canvas.width() << "x" << canvas.height()
                  << " k=" << kScale << "\n";
        const camsolve::Pose views[2] = {poseFrom(identity, zero), poseFrom(r, translation)};
        const double* rs[2] = {identity, r};
        const double* ts[2] = {zero, translation};
        for (int v = 0; v < 2; ++v) {
            const std::string tag = std::string("view") + (v ? "B" : "A");
            const Stats head = measure(pts, views[v], rs[v], ts[v], Mapping::Head, 1, canvas);
            line((tag + " HEAD mapping (S=1, no f offset)").c_str(), head);
            bestHead = std::min(bestHead, head.maximum);
            for (const double S : {1.0, 100.0, 1000.0}) {
                const Stats prop = measure(pts, views[v], rs[v], ts[v], Mapping::Proposed, S, canvas);
                line((tag + " proposed mapping, true pose, S=" + std::to_string(int(S))).c_str(), prop);
                // Float32 path is gated at S = 100 / 1000; S = 1 is printed (eye ~ f*R3 is
                // quantised by float32 position, see the note at the end).
                if (S == 1.0) worstS1 = std::max(worstS1, prop.maximum);
                else worstProposed = std::max(worstProposed, prop.maximum);
                const double dbl = measureDouble(pts, views[v], rs[v], ts[v], S, canvas);
                worstDouble = std::max(worstDouble, dbl);
                std::cerr << "camsolve-probe " << tag << " proposed mapping in double precision, S="
                          << int(S) << ": max=" << dbl << " px\n";
            }
            const Stats wrong = measure(pts, views[v], rs[v], ts[v], Mapping::ProposedWrongSign, 1, canvas);
            line((tag + " control: f offset along -R3 (wrong sign), S=1").c_str(), wrong);
            bestWrongSign = std::min(bestWrongSign, wrong.maximum);
        }
    }

    // Solver pose (view B): estimate from the observations, restore the baseline
    // length from the known truth (solver t has |t| = 1). Error = solver + mapping.
    QVector<sfm::Correspondence> pairs;
    for (const QVector3D& p : pts) pairs.append({projectCv(p, identity, zero), projectCv(p, r, translation)});
    const auto est = sfm::estimateRelativePose(pairs, k);
    double solverMax = -1;
    if (est.valid) {
        const double tScaled[3] = {est.t[0] * baseline, est.t[1] * baseline, est.t[2] * baseline};
        const camsolve::Pose solved = poseFrom(est.R, tScaled);
        for (const double S : {1.0, 100.0, 1000.0}) {
            const Stats s = measure(pts, solved, r, translation, Mapping::Proposed, S, analysisSize);
            line(("viewB proposed mapping, solver pose (|t| rescaled to truth baseline), S="
                  + std::to_string(int(S))).c_str(), s);
            if (S == 1.0) solverMax = s.maximum;
        }
    } else {
        std::cerr << "camsolve-probe solver pose: invalid\n";
    }

    // Depth range of the cloud seen from a positionZ = 0 layer (px plane) and the
    // share inside the +-10000 positionZ clamp (Timeline.cpp:9261-9262).
    for (const double S : {1.0, 100.0, 1000.0}) {
        double lo = 1e30, hi = -1e30; int inside = 0;
        for (const QVector3D& p : pts) {
            const double z = S * p.z();
            lo = std::min(lo, z); hi = std::max(hi, z);
            if (std::abs(z) <= 10000.0) ++inside;
        }
        std::cerr << "camsolve-probe S=" << int(S) << ": depth range vs positionZ=0 plane = ["
                  << lo << ", " << hi << "] px (width " << hi - lo << "), inside +-10000 clamp = "
                  << 100.0 * inside / pts.size() << " %\n";
    }
    std::cerr << "camsolve-probe float32 note: S=1 state-path max error " << worstS1
              << " px vs double-precision " << worstDouble << " px (all S): the f offset puts the eye at ~f px"
              << " from the points, so float32 Camera3DState.position loses the sub-px detail at S=1\n";
    std::cerr << "camsolve-probe note: screen position is f*x_cv/z_cv for any S (S cancels),"
              << " so reprojection error cannot observe S; it is fixed only by the layer-world relation\n";

    // G1 (sanity of the convention): the double-precision proposed mapping with the true
    // pose reprojects within 1e-4 px at S = 1/100/1000, both views, both canvases (Pose.t
    // is float, residual ~3e-6). The float32 Camera3DState path is gated at S = 100/1000
    // only (<= 0.02 px); S = 1 is printed (float32 position quantisation). The HEAD
    // mapping and the wrong-sign control must exceed 1 px, so a convention error breaks it.
    const bool ok = worstProposed <= 0.02 && worstDouble <= 1e-4 && bestHead > 1.0 && bestWrongSign > 1.0 && solverMax >= 0;
    tally.gate(1, ok, std::string("proposed (S=100,1000) max=") + std::to_string(worstProposed)
        + " px, double max=" + std::to_string(worstDouble)
        + " px, HEAD min-max=" + std::to_string(bestHead) + " px, wrong-sign min-max="
        + std::to_string(bestWrongSign) + " px, solver max=" + std::to_string(solverMax) + " px");
    return tally.finish("camsolve-probe");
}
