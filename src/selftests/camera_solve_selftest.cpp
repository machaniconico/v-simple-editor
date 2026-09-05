#include "../CameraSolver.h"
#include "../Camera3D.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>

namespace {
constexpr double pi = 3.14159265358979323846;
using Mat = std::array<double, 9>;
const Mat identity = {1,0,0, 0,1,0, 0,0,1};
const camsolve::Intrinsics intrinsics{935, 960, 540};
const QPolygonF reference{QPointF(200,150), QPointF(1720,150),
                          QPointF(1720,930), QPointF(200,930)};
Mat multiply(const Mat& a, const Mat& b)
{
    Mat c{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) c[3*i+j] += a[3*i+k]*b[3*k+j];
    return c;
}
Mat rotation(double yaw, double pitch)
{
    yaw *= pi/180; pitch *= pi/180;
    const Mat y = {std::cos(yaw),0,std::sin(yaw), 0,1,0, -std::sin(yaw),0,std::cos(yaw)};
    const Mat x = {1,0,0, 0,std::cos(pitch),-std::sin(pitch), 0,std::sin(pitch),std::cos(pitch)};
    return multiply(y, x);
}
Mat homography(Mat r, const QVector3D& t = {})
{
    // Synthetic plane: n=(0,0,-1), d=1. This is fixture generation, not a solver.
    for (int i = 0; i < 3; ++i) r[3*i+2] -= t[i];
    const auto& k = intrinsics;
    return multiply(multiply(Mat{k.f,0,k.cx, 0,k.f,k.cy, 0,0,1}, r),
                    Mat{1/k.f,0,-k.cx/k.f, 0,1/k.f,-k.cy/k.f, 0,0,1});
}
QPolygonF project(const Mat& h)
{
    QPolygonF polygon;
    for (const auto& p : reference) {
        const double w = h[6]*p.x()+h[7]*p.y()+h[8];
        polygon.append(QPointF((h[0]*p.x()+h[1]*p.y()+h[2])/w,
                              (h[3]*p.x()+h[4]*p.y()+h[5])/w));
    }
    return polygon;
}
double error(const camsolve::Pose& p, const Mat& r)
{
    // atan2 is accurate at zero, unlike acos(trace) for the identity gate.
    Mat rt{r[0],r[3],r[6], r[1],r[4],r[7], r[2],r[5],r[8]}, pr;
    std::copy(p.R, p.R+9, pr.begin());
    const Mat d = multiply(pr, rt);
    const double sine = 0.5*std::hypot(std::hypot(d[7]-d[5], d[2]-d[6]), d[3]-d[1]);
    return std::atan2(sine, (d[0]+d[4]+d[8]-1)/2)*180/pi;
}
camsolve::Pose solve(const Mat& h)
{
    return camsolve::solveSequence({reference, project(h)}, {1,1}, 0, intrinsics)[1];
}
}

int runCameraSolveSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    const Mat r = rotation(10, 5);
    const auto pan = solve(homography(r));
    Camera3DState base;
    base.fov = 123; base.nearPlane = 0.25; base.farPlane = 500; base.trueProjection = true;
    const auto camera = camsolve::poseToCameraState(pan, base);
    const auto k = camsolve::makeIntrinsics(90, QSize(1920,1080));
    const double angle = 23*pi/180;
    const Mat rollRotation{std::cos(angle),-std::sin(angle),0,
                           std::sin(angle),std::cos(angle),0, 0,0,1};
    const auto rolled = solve(homography(rollRotation));
    const auto rollCamera = camsolve::poseToCameraState(rolled, base);
    gate(1, pan.valid && error(pan, r) < 0.5 && pan.t.length() < 1e-6
         && std::abs(k.f-540) < 1e-9 && k.cx == 960 && k.cy == 540
         && camera.fov == base.fov && camera.nearPlane == base.nearPlane
         && camera.farPlane == base.farPlane && camera.trueProjection
         && rolled.valid && std::abs(rollCamera.roll+23) < 1e-4
         && (camera.target-camera.position-QVector3D(-float(r[6]),-float(r[7]),-float(r[8]))).length() < 1e-5);
    const auto dolly = solve(homography(identity, QVector3D(0,0,0.2f)));
    const auto dollyCamera = camsolve::poseToCameraState(dolly, base);
    const auto dollyH = homography(identity, QVector3D(0,0,0.2f));
    const auto unseeded = camsolve::decomposeHomography(dollyH.data(), intrinsics);
    // Exercise four-candidate continuity with both rotation and translation.
    const QVector3D mixedT(0.03f,-0.02f,0.15f);
    const auto mixedH = homography(r, mixedT);
    const auto mixed = camsolve::decomposeHomography(mixedH.data(), intrinsics, &pan);
    gate(2, dolly.valid && error(dolly, identity) < 0.5 && dolly.t.length() > 0
         && dolly.t.z()/dolly.t.length() > std::cos(2*pi/180) && dolly.n.z() < 0
         && std::abs(dolly.t.z()-0.2) < 1e-5 && dolly.residual < 1e-6
         && unseeded.valid && error(unseeded, identity) < 0.5 && unseeded.t.z() > 0
         && mixed.valid && error(mixed, r) < 0.5 && (mixed.t-mixedT).length() < 1e-5
         && (dollyCamera.position-QVector3D(0,0,-0.2f)).length() < 1e-5);
    const auto zero = camsolve::decomposeHomography(identity.data(), intrinsics);
    const auto zeroSequence = solve(identity);
    Mat negativeIdentity = identity;
    for (double& x : negativeIdentity) x *= -7;
    const auto negative = camsolve::decomposeHomography(negativeIdentity.data(), intrinsics);
    const Mat singular{};
    const auto invalid = camsolve::decomposeHomography(singular.data(), intrinsics);
    gate(3, zero.valid && zeroSequence.valid && error(zero, identity) < 1e-9
         && zero.t.length() == 0 && zero.n == QVector3D(0,0,-1)
         && negative.valid && error(negative, identity) < 1e-9 && !invalid.valid
         && camsolve::poseToCameraState(invalid, base).fov == base.fov);
    std::mt19937 rng(205);
    bool noiseOk = true;
    for (int trial = 0; trial < 32; ++trial) {
        auto noisy = project(homography(r));
        // Explicit engine-to-uniform mapping is identical on MSVC and libstdc++.
        for (auto& p : noisy) {
            p.setX(p.x()+double(rng())/double(std::mt19937::max())-0.5);
            p.setY(p.y()+double(rng())/double(std::mt19937::max())-0.5);
        }
        const auto pose = camsolve::solveSequence({reference,noisy}, {1,1}, 0, intrinsics)[1];
        noiseOk = noiseOk && pose.valid && error(pose, r) < 2;
    }
    gate(4, noiseOk);
    QVector<QPolygonF> frames;
    QVector<double> confidence(30, 1.0);
    for (int i = 0; i < 30; ++i) frames.append(project(homography(rotation(15.0*i/29, 0))));
    const auto sequence = camsolve::solveSequence(frames, confidence, 0, intrinsics);
    bool continuous = true;
    double lastYaw = -1;
    for (int i = 0; i < 30; ++i) {
        const auto& p = sequence[i];
        const double yaw = std::atan2(p.R[2], p.R[0])*180/pi;
        continuous = continuous && p.valid && yaw > lastYaw && p.n.z() < 0
            && error(p, rotation(15.0*i/29,0)) < 0.5;
        lastYaw = yaw;
    }
    // A nonzero reference verifies relative homographies, rather than adjacent ones.
    const auto relative = camsolve::solveSequence(frames, confidence, 12, intrinsics);
    for (int i = 0; i < 30; ++i)
        continuous = continuous && relative[i].valid && error(relative[i], rotation(15.0*(i-12)/29,0)) < 0.5;
    gate(5, continuous);
    confidence[4] = confidence[15] = confidence[16] = 0.2;
    const auto filtered = camsolve::solveSequence(frames, confidence, 0, intrinsics);
    int valid = 0;
    bool exact = true;
    for (int i = 0; i < 30; ++i) {
        valid += filtered[i].valid ? 1 : 0;
        exact = exact && (filtered[i].valid == (i != 4 && i != 15 && i != 16));
        if (filtered[i].valid) exact = exact && error(filtered[i], rotation(15.0*i/29,0)) < 0.5;
    }
    gate(6, exact && valid == 27);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
