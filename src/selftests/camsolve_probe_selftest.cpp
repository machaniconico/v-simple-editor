// Measures how a camera solve (R, t in the CV convention, +z in front) maps onto
// Camera3DState + the projectLayerQuad projection formula (Camera3D.cpp). G1 is the
// convention sanity check (the numbers fed the Wave 18 design); G2-G5 (US-1810) gate
// the reference-centred, scaled, f-offset mapping (camsolve::applySolvedPosesToCamera).
#include "../BundleAdjustment.h"
#include "../CameraSolver.h"
#include "../Camera3D.h"
#include "../TwoViewGeometry.h"
#include "SelftestGate.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QVector3D>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
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

constexpr double kInf = std::numeric_limits<double>::infinity();

// camera3d::projectWorldPoint (the projectLayerQuad core) for a world point in layer pixels;
// a point at / behind the pinhole is infinitely far from any observation.
QPointF projectWorld(const QVector3D& world, const Camera3DState& cam, const QSize& canvas)
{
    return camera3d::projectWorldPoint(world, cam, canvas).value_or(QPointF(kInf, kInf));
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

// --- US-1810 (G2-G5): analysis = canvas = 1920x1080 unless stated, principal point at the centre.
const QSize kCanvas(1920, 1080);

// CV observation of a world point through pose at canvas focal f.
QPointF observe(const QVector3D& p, const camsolve::Pose& pose, double f, const QSize& canvas)
{
    double b[3];
    for (int i = 0; i < 3; ++i)
        b[i] = pose.R[3*i] * p.x() + pose.R[3*i+1] * p.y() + pose.R[3*i+2] * p.z() + pose.t[i];
    return {f * b[0] / b[2] + canvas.width() / 2.0, f * b[1] / b[2] + canvas.height() / 2.0};
}

camsolve::SolveMapping mappingFor(double focalPx, double S, QSize analysis = kCanvas, QSize canvas = kCanvas)
{
    camsolve::SolveMapping m;
    m.unitScalePx = S;
    m.focalPx = focalPx;
    m.analysisSize = analysis;
    m.canvasSize = canvas;
    return m;
}

camsolve::CameraSolveApplied appliedFor(const QVector<camsolve::Pose>& poses, const QVector<QVector3D>& points,
                                        const camsolve::SolveMapping& m, double fps = 1)
{
    camsolve::CameraSolveApplied a;
    a.poses = poses;
    a.points = points;
    a.fps = fps;
    a.startSec = 0;
    a.mapping = m;
    return a;
}

// S = fit: defaultUnitScale over the reference-centred copy of the points.
double fitScale(QVector<camsolve::Pose> poses, QVector<QVector3D> points, double f)
{
    camsolve::normaliseToReference(poses, points, camsolve::referencePoseIndex(poses));
    return camsolve::defaultUnitScale(points, f);
}

double distance(const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

// Largest corner distance between a Z = 0 layer's projected quad and the canvas rectangle.
double cornerError(const Camera3DState& cam, const QSize& canvas)
{
    const QPolygonF quad = Camera3D::projectLayerQuad(Layer3DTransform{}, cam, canvas);
    const QPointF corners[4] = {QPointF(0, 0), QPointF(canvas.width(), 0),
                                QPointF(canvas.width(), canvas.height()), QPointF(0, canvas.height())};
    double worst = quad.size() == 4 ? 0 : kInf;
    for (int i = 0; i < quad.size() && i < 4; ++i)
        worst = std::max(worst, std::isfinite(quad[i].x()) && std::isfinite(quad[i].y()) ? distance(quad[i], corners[i]) : kInf);
    return worst;
}

// Every corner within ten canvas sizes of the canvas centre.
bool cornersBounded(const Camera3DState& cam, const QSize& canvas)
{
    const QPolygonF quad = Camera3D::projectLayerQuad(Layer3DTransform{}, cam, canvas);
    bool ok = quad.size() == 4;
    for (const QPointF& p : quad)
        ok = ok && std::abs(p.x() - canvas.width() / 2.0) <= 10.0 * canvas.width()
            && std::abs(p.y() - canvas.height() / 2.0) <= 10.0 * canvas.height();
    return ok;
}

// G2: true poses, f in {60, 1000} x S in {fit, 100, 1000}, every view and point.
bool gate2(const QVector<camsolve::Pose>& views, const QVector<QVector3D>& pts, std::string& detail)
{
    bool ok = true;
    double legacyAt1000 = 0;
    for (const double f : {60.0, 1000.0}) {
        const double fit = fitScale(views, pts, f);
        for (const double S : {fit, 100.0, 1000.0}) {
            const camsolve::SolveMapping m = mappingFor(f, S);
            Camera3D camera;
            const bool applied = camsolve::applySolvedPosesToCamera(camera, appliedFor(views, pts, m));
            QVector<camsolve::Pose> np = views;
            QVector<QVector3D> npts = pts;
            camsolve::normaliseToReference(np, npts, 0);
            double worst = applied ? 0 : kInf;
            for (int v = 0; applied && v < views.size(); ++v) {
                const Camera3DState state = camera.getCameraAt(v);
                for (int j = 0; j < pts.size(); ++j)
                    worst = std::max(worst, distance(projectWorld(camsolve::solvePointToWorldPx(npts[j], m), state, kCanvas),
                                                     observe(pts[j], views[v], f, kCanvas)));
            }
            std::cerr << "camsolve-probe G2 f=" << f << " S=" << S << (S == fit ? " (fit)" : "")
                      << ": applied=" << applied << " max reprojection=" << worst << " px\n";
            ok = ok && applied && worst <= 2.0;
        }
        // Control: the legacy pose mapping (poseToCameraState, S = 1, no f offset).
        Camera3DState base;
        base.fov = f;
        base.trueProjection = true;
        double legacy = 0;
        for (int v = 0; v < views.size(); ++v) {
            const Camera3DState state = camsolve::poseToCameraState(views[v], base);
            for (int j = 0; j < pts.size(); ++j)
                legacy = std::max(legacy, distance(projectWorld(pts[j], state, kCanvas), observe(pts[j], views[v], f, kCanvas)));
        }
        std::cerr << "camsolve-probe G2 control f=" << f << ": legacy poseToCameraState max=" << legacy << " px\n";
        if (f == 1000.0) legacyAt1000 = legacy;
    }
    ok = ok && legacyAt1000 >= 100.0;
    detail = "legacy control (f=1000) max=" + std::to_string(legacyAt1000) + " px";
    return ok;
}

// G3: synthetic 20-frame orbit (bundle_adjust_selftest.cpp bundleScene, noise-free) through
// sfm::solveCameraTracks -> toCameraPoses -> applySolvedPosesToCamera, f = 1000.
bool gate3(std::string& detail)
{
    const camsolve::Intrinsics k3{1000, 960, 540};
    const int frames = 20;
    QVector<camsolve::Pose> truth;
    for (int frame = 0; frame < frames; ++frame) {
        const double angle = 0.025 * frame, c = std::cos(angle), s = std::sin(angle);
        camsolve::Pose pose;
        const double r[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
        std::copy(r, r + 9, pose.R);
        const double center[3] = {6 * s, 0.2 * std::sin(2 * angle), 6 * (1 - c)};
        for (int i = 0; i < 3; ++i) {
            double t = 0;
            for (int j = 0; j < 3; ++j) t -= r[3*i+j] * center[j];
            pose.t[i] = float(t);
        }
        pose.valid = true;
        truth.append(pose);
    }
    std::mt19937 rng(518);
    const auto uniform = [&]() { return (double(rng()) + 0.5) / 4294967296.0; };
    sfm::MultiViewTracks tracks;
    tracks.frameCount = frames;
    for (int point = 0; point < 100; ++point) {
        const QVector3D p(float(3 * uniform() - 1.5), float(2 * uniform() - 1), float(5 + 3 * uniform()));
        sfm::Track2D track;
        track.pointId = point;
        for (int frame = 0; frame < frames; ++frame)
            track.observations.append(qMakePair(frame, observe(p, truth[frame], k3.f, kCanvas)));
        tracks.tracks.append(track);
    }
    const sfm::CameraTrackSolve result = sfm::solveCameraTracks(tracks, k3);
    const auto solved = sfm::toCameraPoses(result.bundle);
    const double fps = 30;
    QVector<camsolve::Pose> timed(frames);
    for (int i = 0; i < solved.size() && i < result.sourceFrames.size(); ++i) timed[result.sourceFrames[i]] = solved[i];
    const QVector<QVector3D> points = result.bundle.points;
    const double S = fitScale(timed, points, k3.f);
    const camsolve::SolveMapping m = mappingFor(k3.f, S);
    Camera3D camera;
    const bool applied = result.bundle.valid && points.size() == result.input.tracks.size()
        && camsolve::applySolvedPosesToCamera(camera, appliedFor(timed, points, m, fps));
    QVector<camsolve::Pose> np = timed;
    QVector<QVector3D> npts = points;
    camsolve::normaliseToReference(np, npts, camsolve::referencePoseIndex(np));
    double worst = applied ? 0 : kInf;
    int observations = 0;
    for (int j = 0; applied && j < result.input.tracks.size(); ++j) {
        for (const auto& obs : result.input.tracks[j].observations) {
            const int frame = result.sourceFrames.value(obs.first, -1);
            if (frame < 0 || !timed[frame].valid) continue;
            const Camera3DState state = camera.getCameraAt(frame / fps);
            worst = std::max(worst, distance(projectWorld(camsolve::solvePointToWorldPx(npts[j], m), state, kCanvas), obs.second));
            ++observations;
        }
    }
    detail = "bundle valid=" + std::to_string(result.bundle.valid) + " rms=" + std::to_string(result.bundle.rms)
        + " px, poses=" + std::to_string(solved.size()) + ", points=" + std::to_string(points.size())
        + ", S=" + std::to_string(S) + ", observations=" + std::to_string(observations)
        + ", max reprojection=" + std::to_string(worst) + " px";
    return applied && observations >= frames * 50 && worst <= 2.0;
}

// G4: f from the analysis / canvas sizes, solveMapping JSON, projectWorldPoint's pinhole
// cut-off, defaultUnitScale's fallback and the rejected inputs.
bool gate4(const QVector<camsolve::Pose>& views, const QVector<QVector3D>& pts, std::string& detail)
{
    bool ok = true;
    const auto fovAfter = [&](QSize analysis, QSize canvas, double focalPx, double& trackFov) {
        Camera3D camera;
        Camera3DState animated;
        animated.fov = 30;
        camera.setCameraKeyframe(0, animated);
        animated.fov = 90;
        camera.setCameraKeyframe(10, animated);
        if (!camsolve::applySolvedPosesToCamera(camera, appliedFor(views, pts, mappingFor(focalPx, 100, analysis, canvas))))
            return -1.0;
        trackFov = camera.getCameraAt(5).fov; // between the old Fov keys: the track is now constant
        return camera.camera().fov;
    };
    const double focal = 500;
    double track1 = 0, track2 = 0, track3 = 0;
    const double same = fovAfter(QSize(1280, 720), QSize(1280, 720), focal, track1);
    const double wide = fovAfter(QSize(1440, 1080), kCanvas, focal, track2);
    const double half = fovAfter(QSize(960, 540), kCanvas, focal, track3);
    const double r1 = focal * 1920.0 / 1440.0; // the r1 formula focalPx * cw / aw
    const bool fovOk = same == focal && track1 == focal && wide == focal && track2 == focal
        && half == 2 * focal && track3 == 2 * focal && std::abs(r1 - wide) > 0.3 * focal;
    std::cerr << "camsolve-probe G4 fov: 1280x720->1280x720 " << same << " (track " << track1
              << "), 1440x1080->1920x1080 " << wide << " (track " << track2 << ", r1 control " << r1
              << "), 960x540->1920x1080 " << half << " (track " << track3 << "), focalPx " << focal << "\n";
    ok = ok && fovOk;

    // JSON: no key without a mapping; the mapping round-trips; a JSON without it clears it.
    Camera3D plain;
    Camera3D solved;
    const camsolve::SolveMapping m = mappingFor(1000, 199.25, QSize(1440, 1080), kCanvas);
    QVector<camsolve::Pose> refLater = views;
    refLater.prepend(camsolve::Pose{}); // invalid frame 0: the reference is index 1
    const bool appliedOk = camsolve::applySolvedPosesToCamera(solved, appliedFor(refLater, pts, m));
    const QJsonObject json = solved.toJson();
    Camera3D loaded;
    loaded.fromJson(json);
    const auto back = loaded.solveMapping();
    const QJsonObject mapJson = json.value("solveMapping").toObject();
    const bool jsonOk = !plain.toJson().contains("solveMapping") && appliedOk && back
        && mapJson.value("version").toInt() == 2 && back->unitScalePx == 199.25 && back->focalPx == 1000
        && back->analysisSize == QSize(1440, 1080) && back->canvasSize == kCanvas && back->referenceIndex == 1
        && loaded.toJson() == json;
    loaded.fromJson(plain.toJson());
    const bool clearedOk = !loaded.solveMapping() && !loaded.toJson().contains("solveMapping");
    std::cerr << "camsolve-probe G4 json: plain has key=" << plain.toJson().contains("solveMapping")
              << ", solveMapping=" << QJsonDocument(mapJson).toJson(QJsonDocument::Compact).toStdString()
              << ", round-trip=" << jsonOk << ", cleared by a JSON without the key=" << clearedOk << "\n";
    ok = ok && jsonOk && clearedOk;

    // projectWorldPoint: nullopt at fov + v.z <= 1e-3 (default camera, fov 60, pinhole z = -60).
    const Camera3DState def;
    const auto at = [&](double z) { return camera3d::projectWorldPoint(QVector3D(10, 0, float(z)), def, kCanvas); };
    const auto onPlane = at(0);
    const bool cutOk = !at(-60) && !at(-59.9995) && !at(-70) && at(-59.99)
        && onPlane && distance(*onPlane, QPointF(970, 540)) < 1e-9
        && !camera3d::projectWorldPoint(QVector3D(), def, QSize());
    std::cerr << "camsolve-probe G4 projectWorldPoint: z=-60 " << bool(at(-60)) << ", z=-59.9995 " << bool(at(-59.9995))
              << ", z=-59.99 " << bool(at(-59.99)) << ", z=0 -> (" << (onPlane ? onPlane->x() : -1) << ", "
              << (onPlane ? onPlane->y() : -1) << ")\n";
    ok = ok && cutOk;

    // defaultUnitScale: no point (or none in front) falls back to f.
    const double fallback = camsolve::defaultUnitScale({}, 60);
    const double behind = camsolve::defaultUnitScale({QVector3D(0, 0, -5), QVector3D(1, 1, 0)}, 1000);
    std::cerr << "camsolve-probe G4 defaultUnitScale fallback: no points " << fallback << ", none in front " << behind << "\n";
    ok = ok && fallback == 60 && behind == 1000;

    // Rejected inputs leave the camera JSON unchanged.
    Camera3D target;
    Camera3DState start;
    start.position = QVector3D(5, -3, 2);
    target.setCamera(start);
    target.setCameraKeyframe(0.5, start);
    const QJsonObject before = target.toJson();
    QVector<camsolve::Pose> invalid = views;
    for (auto& pose : invalid) pose.valid = false;
    const bool zeroFocal = camsolve::applySolvedPosesToCamera(target, appliedFor(views, pts, mappingFor(0, 100)));
    const bool noValid = camsolve::applySolvedPosesToCamera(target, appliedFor(invalid, pts, mappingFor(1000, 100)));
    const bool nanScale = camsolve::applySolvedPosesToCamera(target, appliedFor(views, pts, mappingFor(1000, std::nan(""))));
    QVector<camsolve::Pose> invalidCopy = invalid;
    QVector<QVector3D> ptsCopy = pts;
    const bool normalised = camsolve::normaliseToReference(invalidCopy, ptsCopy, camsolve::referencePoseIndex(invalidCopy));
    const bool rejectOk = !zeroFocal && !noValid && !nanScale && target.toJson() == before
        && !normalised && ptsCopy == pts && camsolve::referencePoseIndex(invalid) == -1;
    std::cerr << "camsolve-probe G4 rejects: focalPx=0 -> " << zeroFocal << ", all poses invalid -> " << noValid
              << ", S=NaN -> " << nanScale << ", camera JSON unchanged=" << (target.toJson() == before)
              << ", normaliseToReference(no valid pose) -> " << normalised << "\n";
    ok = ok && rejectOk;
    detail = std::string("fov=") + (fovOk ? "ok" : "bad") + " json=" + (jsonOk && clearedOk ? "ok" : "bad")
        + " cut-off=" + (cutOk ? "ok" : "bad") + " rejects=" + (rejectOk ? "ok" : "bad");
    return ok;
}

// G5: the Z = 0 layer fills the canvas at the reference frame, and S = fit puts the
// median-depth point on Z = 0.
bool gate5(const QVector<camsolve::Pose>& views, const QVector<QVector3D>& pts, std::string& detail)
{
    bool ok = true;
    double worstRef = 0, uncentred = kInf;
    for (const double f : {60.0, 1000.0}) {
        const double S = fitScale(views, pts, f);
        // [A, B] (reference = identity) and [invalid, B, A] (reference = B, non-identity).
        QVector<camsolve::Pose> later{camsolve::Pose{}, views[1], views[0]};
        for (const auto& order : {views, later}) {
            const int ref = camsolve::referencePoseIndex(order);
            Camera3D camera;
            const bool applied = camsolve::applySolvedPosesToCamera(camera, appliedFor(order, pts, mappingFor(f, S)));
            const double refErr = applied ? cornerError(camera.getCameraAt(ref), kCanvas) : kInf;
            bool bounded = applied;
            for (int i = ref + 1; applied && i < order.size(); ++i)
                bounded = bounded && cornersBounded(camera.getCameraAt(i), kCanvas);
            std::cerr << "camsolve-probe G5a f=" << f << " S=" << S << " reference=" << ref
                      << ": reference-frame corner error=" << refErr << " px, other view within 10x canvas=" << bounded << "\n";
            worstRef = std::max(worstRef, refErr);
            // The bounded check runs at f = 1000. At f = 60 the canvas-wide layer spans about
            // +-86 degrees from the pinhole, so the 8 degree yaw of view B puts a corner behind
            // it whatever the mapping (printed only).
            ok = ok && applied && refErr <= 1e-3 && (f != 1000.0 || bounded);
        }
        // Control: the uncentred position S*c + f*R3 at the identity reference (c = 0).
        Camera3DState control;
        control.trueProjection = true;
        control.fov = f;
        control.position = QVector3D(0, 0, float(f));
        control.target = control.position + QVector3D(0, 0, -1);
        const double controlErr = cornerError(control, kCanvas);
        std::cerr << "camsolve-probe G5a control f=" << f << ": uncentred S*c+f*R3 corner error=" << controlErr << " px\n";
        uncentred = std::min(uncentred, controlErr);

        // (b) the median-depth point (mean of the two middle points for an even count).
        QVector<camsolve::Pose> np = views;
        QVector<QVector3D> npts = pts;
        camsolve::normaliseToReference(np, npts, 0);
        QVector<QVector3D> sorted = npts;
        std::sort(sorted.begin(), sorted.end(), [](const QVector3D& a, const QVector3D& b) { return a.z() < b.z(); });
        const auto medianZ = [&](double scale) {
            const camsolve::SolveMapping m = mappingFor(f, scale);
            const qsizetype n = sorted.size();
            const double hi = camsolve::solvePointToWorldPx(sorted[n / 2], m).z();
            return n % 2 ? hi : (camsolve::solvePointToWorldPx(sorted[n / 2 - 1], m).z() + hi) / 2;
        };
        const double z = medianZ(S), rounded = medianZ(std::clamp(S, 100.0, 1000.0));
        std::cerr << "camsolve-probe G5b f=" << f << ": S(fit)=" << S << " median-point Z=" << z
                  << " px; control S clamped to [100, 1000] = " << std::clamp(S, 100.0, 1000.0) << " gives Z=" << rounded << " px\n";
        ok = ok && std::abs(z) < 1e-3;
        if (f == 60.0) ok = ok && std::abs(rounded) > 100.0;
    }
    ok = ok && uncentred >= 1e3;
    detail = "worst reference corner error=" + std::to_string(worstRef) + " px, uncentred control min="
        + std::to_string(uncentred) + " px";
    return ok;
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

    const QVector<camsolve::Pose> views{poseFrom(identity, zero), poseFrom(r, translation)};
    std::string detail;
    const bool g2 = gate2(views, pts, detail);
    tally.gate(2, g2, detail);
    const bool g3 = gate3(detail);
    tally.gate(3, g3, detail);
    const bool g4 = gate4(views, pts, detail);
    tally.gate(4, g4, detail);
    const bool g5 = gate5(views, pts, detail);
    tally.gate(5, g5, detail);
    return tally.finish("camsolve-probe");
}
