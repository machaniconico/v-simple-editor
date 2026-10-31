#include "CameraSolver.h"
#include "BundleAdjustment.h"
#include "Camera3D.h"
#include "PlanarTracker.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace camsolve {
namespace {
constexpr double pi = 3.14159265358979323846;
using Mat = std::array<double, 9>;
const Mat identity = {1,0,0, 0,1,0, 0,0,1};
Mat transpose(const Mat& a)
{
    return {a[0],a[3],a[6], a[1],a[4],a[7], a[2],a[5],a[8]};
}
Mat multiply(const Mat& a, const Mat& b)
{
    Mat c{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) c[3*i+j] += a[3*i+k]*b[3*k+j];
    return c;
}
double determinant(const Mat& a)
{
    return a[0]*(a[4]*a[8]-a[5]*a[7]) - a[1]*(a[3]*a[8]-a[5]*a[6])
        + a[2]*(a[3]*a[7]-a[4]*a[6]);
}

// One-sided Jacobi SVD: rotate column pairs until mutually orthogonal.
// All arithmetic remains double precision, including the small singular value.
bool svd(Mat b, Mat& u, double s[3], Mat& v)
{
    v = identity;
    bool converged = false;
    for (int sweep = 0; sweep < 64; ++sweep) {
        bool changed = false;
        for (int p = 0; p < 2; ++p) for (int q = p+1; q < 3; ++q) {
            double aa = 0, bb = 0, ab = 0;
            for (int r = 0; r < 3; ++r) {
                aa += b[3*r+p]*b[3*r+p]; bb += b[3*r+q]*b[3*r+q];
                ab += b[3*r+p]*b[3*r+q];
            }
            if (std::abs(ab) <= 1e-15*std::sqrt(aa*bb)) continue;
            changed = true;
            const double tau = (bb-aa)/(2*ab);
            const double t = std::copysign(1.0, tau)/(std::abs(tau)+std::hypot(1.0, tau));
            const double c = 1/std::hypot(1.0, t), sn = c*t;
            for (int r = 0; r < 3; ++r) {
                const double x = b[3*r+p], y = b[3*r+q];
                b[3*r+p] = c*x-sn*y; b[3*r+q] = sn*x+c*y;
                const double vx = v[3*r+p], vy = v[3*r+q];
                v[3*r+p] = c*vx-sn*vy; v[3*r+q] = sn*vx+c*vy;
            }
        }
        if (!changed) { converged = true; break; }
    }
    if (!converged) return false;
    for (int j = 0; j < 3; ++j) s[j] = std::hypot(std::hypot(b[j], b[3+j]), b[6+j]);
    for (int p = 0; p < 2; ++p) for (int q = p+1; q < 3; ++q) {
        if (s[p] >= s[q]) continue;
        std::swap(s[p], s[q]);
        for (int r = 0; r < 3; ++r) {
            std::swap(b[3*r+p], b[3*r+q]); std::swap(v[3*r+p], v[3*r+q]);
        }
    }
    if (!(s[2] > 1e-12*s[0])) return false;
    for (int r = 0; r < 3; ++r) for (int j = 0; j < 3; ++j) u[3*r+j] = b[3*r+j]/s[j];
    return true;
}
double rotationDistance(const double* a, const double* b)
{
    double trace = 0;
    for (int j = 0; j < 9; ++j) trace += a[j]*b[j];
    return std::acos(std::clamp((trace-1)/2, -1.0, 1.0));
}
void finish(Pose& p, const Mat& h)
{
    double error = 0;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        const double e = h[3*i+j]-p.R[3*i+j]-double(p.t[i])*double(p.n[j]);
        error += e*e;
    }
    p.residual = std::sqrt(error);
    p.valid = std::isfinite(p.residual) && p.residual < 1e-4;
}
bool corners(const QPolygonF& polygon, planar::CornerSet& result)
{
    if (polygon.size() != 4) return false;
    for (const auto& p : polygon)
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) return false;
    // Reject collinear triples: the planar helper otherwise returns identity on failure.
    for (int i = 0; i < 4; ++i) {
        const QPointF a = polygon[(i+1)%4]-polygon[i];
        const QPointF b = polygon[(i+2)%4]-polygon[i];
        const double scale = std::hypot(a.x(), a.y())*std::hypot(b.x(), b.y());
        if (!(scale > 0) || std::abs(a.x()*b.y()-a.y()*b.x()) <= 1e-10*scale) return false;
    }
    result = {polygon[0], polygon[1], polygon[2], polygon[3]};
    return true;
}
} // namespace

Intrinsics makeIntrinsics(double fovDeg, QSize canvas)
{
    const double f = canvas.width() > 0 && canvas.height() > 0
        && std::isfinite(fovDeg) && fovDeg > 0 && fovDeg < 180
        ? canvas.height()/2.0/std::tan(fovDeg*pi/360.0) : 0.0;
    return {f, canvas.width()/2.0, canvas.height()/2.0};
}

Intrinsics makeIntrinsicsFromFocalPx(double fPx, QSize canvas)
{
    return {std::isfinite(fPx) && fPx > 0 ? fPx : 60.0,
            canvas.width()/2.0, canvas.height()/2.0};
}

Pose decomposeHomography(const double H[9], const Intrinsics& k, const Pose* previous)
{
    if (!H || !std::isfinite(k.f) || k.f <= 0 || !std::isfinite(k.cx) || !std::isfinite(k.cy)) return {};
    Mat h;
    double scale = 0;
    for (int j = 0; j < 9; ++j) {
        if (!std::isfinite(H[j])) return {};
        scale = std::max(scale, std::abs(H[j]));
    }
    if (scale == 0) return {};
    for (int j = 0; j < 9; ++j) h[j] = H[j]/scale;
    const Mat K = {k.f,0,k.cx, 0,k.f,k.cy, 0,0,1};
    const Mat inverseK = {1/k.f,0,-k.cx/k.f, 0,1/k.f,-k.cy/k.f, 0,0,1};
    h = multiply(multiply(inverseK, h), K);
    scale = 0;
    for (double x : h) { if (!std::isfinite(x)) return {}; scale = std::max(scale, std::abs(x)); }
    if (scale == 0) return {};
    for (double& x : h) x /= scale;
    // H and -H describe the same projectivity. Choose the positive determinant.
    if (determinant(h) < 0) for (double& x : h) x = -x;
    Mat u{}, v{};
    double s[3];
    if (!svd(h, u, s, v)) return {};
    for (double& x : h) x /= s[1];
    const double a = s[0]/s[1], c = s[2]/s[1];
    const Mat vt = transpose(v);
    if (a-c <= 1e-6) {
        Pose p;
        const Mat r = multiply(u, vt); // polar factor U V^T
        std::copy(r.begin(), r.end(), p.R);
        finish(p, h);
        return p;
    }

    // Faugeras/Zhang in the singular-vector basis: D = Q + t' n'^T.
    // Q rotates the (largest, smallest) singular plane; D-Q has rank one.
    const double cosine = std::clamp((a*c+1)/(a+c), -1.0, 1.0);
    const double sine = std::sqrt(std::max(0.0, (a*a-1)*(1-c*c)))/(a+c);
    Pose best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (int branch : {-1, 1}) {
        const Mat q = {cosine,0,branch*sine, 0,1,0, -branch*sine,0,cosine};
        const Mat difference = {a-cosine,0,-branch*sine, 0,0,0, branch*sine,0,c-cosine};
        const int row = std::hypot(difference[0], difference[2])
            >= std::hypot(difference[6], difference[8]) ? 0 : 2;
        const double length = std::hypot(difference[3*row], difference[3*row+2]);
        if (length <= 1e-15) continue;
        const Mat r = multiply(multiply(u, q), vt);
        for (int sign : {-1, 1}) {
            double np[3] = {sign*difference[3*row]/length, 0, sign*difference[3*row+2]/length};
            double tp[3]{};
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) tp[i] += difference[3*i+j]*np[j];
            Pose p;
            std::copy(r.begin(), r.end(), p.R);
            for (int i = 0; i < 3; ++i) {
                double n = 0, t = 0;
                for (int j = 0; j < 3; ++j) { n += v[3*i+j]*np[j]; t += u[3*i+j]*tp[j]; }
                p.n[i] = static_cast<float>(n); p.t[i] = static_cast<float>(t);
            }
            if (!(p.n.z() < 0)) continue;
            finish(p, h);
            if (!p.valid) continue;
            double score;
            if (previous && previous->valid) score = rotationDistance(p.R, previous->R);
            else {
                // Expansion has det(Hn)<1 and t.n<0 (front-facing plane -> +z).
                const double trend = determinant(h)-1;
                const double dot = QVector3D::dotProduct(p.t, p.n);
                const bool signMatches = std::abs(trend) < 1e-9 || dot*trend >= 0;
                score = (signMatches ? 0.0 : 4*pi) + rotationDistance(p.R, identity.data());
            }
            if (score < bestScore) { best = p; bestScore = score; }
        }
    }
    return best;
}

QVector<Pose> solveSequence(const QVector<QPolygonF>& frames, const QVector<double>& confidence,
                            int refFrame, const Intrinsics& intrinsics, double threshold)
{
    QVector<Pose> poses(frames.size());
    planar::CornerSet reference;
    if (refFrame < 0 || refFrame >= frames.size() || confidence.size() != frames.size()
        || !std::isfinite(threshold) || !corners(frames[refFrame], reference)) return poses;
    Pose previous;
    for (qsizetype i = 0; i < frames.size(); ++i) {
        planar::CornerSet current;
        if (!std::isfinite(confidence[i]) || confidence[i] < threshold || !corners(frames[i], current)) continue;
        const auto h = planar::homographyFromCorners(reference, current);
        poses[i] = decomposeHomography(h.m, intrinsics, previous.valid ? &previous : nullptr);
        if (poses[i].valid) previous = poses[i];
    }
    return poses;
}

Camera3DState poseToCameraState(const Pose& p, const Camera3DState& base)
{
    if (!p.valid) return base;
    Camera3DState state = base;
    for (int i = 0; i < 3; ++i)
        state.position[i] = static_cast<float>(-(p.R[i]*p.t.x()+p.R[3+i]*p.t.y()+p.R[6+i]*p.t.z()));
    const QVector3D forward(-float(p.R[6]), -float(p.R[7]), -float(p.R[8]));
    state.target = state.position + forward;
    // Match Camera3D's lookAt up-vector fallback and clockwise view-plane roll.
    const QVector3D up = std::abs(QVector3D::dotProduct(forward, QVector3D(0,1,0))) > 0.9999f
        ? QVector3D(0,0,1) : QVector3D(0,1,0);
    const QVector3D right = QVector3D::crossProduct(forward, up).normalized();
    const QVector3D viewUp = QVector3D::crossProduct(right, forward).normalized();
    const QVector3D poseRight{float(p.R[0]), float(p.R[1]), float(p.R[2])};
    state.roll = std::atan2(QVector3D::dotProduct(poseRight, viewUp),
                            QVector3D::dotProduct(poseRight, right))*180/pi;
    return state;
}
void applyPosesToCamera(Camera3D& camera, const QVector<Pose>& poses,
                        double fps, double startSec)
{
    if (!std::isfinite(fps) || fps <= 0 || !std::isfinite(startSec)
        || !std::any_of(poses.cbegin(), poses.cend(), [](const Pose& p) { return p.valid; }))
        return;
    auto base = camera.camera();
    base.trueProjection = true;
    camera.setCamera(base);
    // setCameraKeyframe also writes Fov; preserve that unrelated track verbatim.
    const KeyframeTrack fov = *camera.track(Camera3DProperty::Fov);
    for (qsizetype i = 0; i < poses.size(); ++i) {
        if (poses[i].valid)
            camera.setCameraKeyframe(startSec + double(i) / fps,
                                      poseToCameraState(poses[i], base));
    }
    *camera.track(Camera3DProperty::Fov) = fov;
}

double canvasFocalPx(const SolveMapping& m)
{
    return m.focalPx / std::max(double(m.analysisSize.width()) / m.canvasSize.width(),
                                double(m.analysisSize.height()) / m.canvasSize.height());
}

int referencePoseIndex(const QVector<Pose>& poses)
{
    for (qsizetype i = 0; i < poses.size(); ++i)
        if (poses[i].valid) return int(i);
    return -1;
}

bool normaliseToReference(QVector<Pose>& poses, QVector<QVector3D>& points, int refIndex)
{
    if (refIndex < 0 || refIndex >= poses.size() || !poses[refIndex].valid) return false;
    const auto centre = [](const Pose& p) {
        std::array<double, 3> c{};
        for (int i = 0; i < 3; ++i)
            c[i] = -(p.R[i]*p.t.x() + p.R[3+i]*p.t.y() + p.R[6+i]*p.t.z());
        return c;
    };
    Mat rRef;
    std::copy(poses[refIndex].R, poses[refIndex].R + 9, rRef.begin());
    const auto cRef = centre(poses[refIndex]);
    // Reference-frame coordinates of a world point: R_ref * (x - c_ref).
    const auto toRef = [&](const std::array<double, 3>& x) {
        std::array<double, 3> d{x[0]-cRef[0], x[1]-cRef[1], x[2]-cRef[2]}, r{};
        for (int i = 0; i < 3; ++i) r[i] = rRef[3*i]*d[0] + rRef[3*i+1]*d[1] + rRef[3*i+2]*d[2];
        return r;
    };
    const Mat rRefT = transpose(rRef);
    for (Pose& p : poses) {
        if (!p.valid) continue;
        const auto c = toRef(centre(p));
        Mat r;
        std::copy(p.R, p.R + 9, r.begin());
        r = multiply(r, rRefT);
        std::copy(r.cbegin(), r.cend(), p.R);
        for (int i = 0; i < 3; ++i)
            p.t[i] = float(-(r[3*i]*c[0] + r[3*i+1]*c[1] + r[3*i+2]*c[2]));
    }
    for (QVector3D& point : points) {
        const auto q = toRef({point.x(), point.y(), point.z()});
        point = QVector3D(float(q[0]), float(q[1]), float(q[2]));
    }
    return true;
}

double defaultUnitScale(const QVector<QVector3D>& normalisedPoints, double f)
{
    QVector<double> depths;
    for (const QVector3D& p : normalisedPoints)
        if (std::isfinite(p.z()) && p.z() > 0) depths.append(p.z());
    if (depths.isEmpty()) return f;
    std::sort(depths.begin(), depths.end());
    const qsizetype n = depths.size();
    const double median = n % 2 ? depths[n / 2] : (depths[n / 2 - 1] + depths[n / 2]) / 2;
    const double scale = f / median;
    return median > 0 && std::isfinite(scale) ? scale : f;
}

Camera3DState poseToProjectCameraState(const Pose& p, const Camera3DState& base,
                                       const SolveMapping& mapping)
{
    if (!p.valid) return base;
    // Roll and forward = -R3 are those of the legacy mapping; only position / fov differ.
    Camera3DState state = poseToCameraState(p, base);
    const double s = mapping.unitScalePx, f = canvasFocalPx(mapping);
    const double e3[3] = {0, 0, 1};
    for (int i = 0; i < 3; ++i) {
        const double c = -(p.R[i]*p.t.x() + p.R[3+i]*p.t.y() + p.R[6+i]*p.t.z());
        state.position[i] = static_cast<float>(s*c + f*(p.R[6+i] - e3[i]));
    }
    state.target = state.position + QVector3D(-float(p.R[6]), -float(p.R[7]), -float(p.R[8]));
    state.fov = f;
    return state;
}

QVector3D solvePointToWorldPx(const QVector3D& p, const SolveMapping& mapping)
{
    const double s = mapping.unitScalePx, f = canvasFocalPx(mapping);
    return QVector3D(float(s*p.x()), float(s*p.y()), float(s*p.z() - f));
}

bool applySolvedPosesToCamera(Camera3D& camera, const CameraSolveApplied& applied)
{
    const double f = canvasFocalPx(applied.mapping), s = applied.mapping.unitScalePx;
    const int ref = referencePoseIndex(applied.poses);
    if (!(applied.mapping.focalPx > 0) || !std::isfinite(f) || !(f > 0)
        || !std::isfinite(s) || !(s > 0) || ref < 0
        || !std::isfinite(applied.fps) || applied.fps <= 0 || !std::isfinite(applied.startSec))
        return false;
    QVector<Pose> poses = applied.poses;
    QVector<QVector3D> points = applied.points;
    normaliseToReference(poses, points, ref);
    SolveMapping mapping = applied.mapping;
    mapping.referenceIndex = ref;
    mapping.legacy = false;
    auto base = camera.camera();
    base.trueProjection = true;
    base.fov = f;
    camera.setCamera(base);
    // Decision C2: the solve's f replaces the whole Fov track (constant f).
    *camera.track(Camera3DProperty::Fov) = KeyframeTrack(
        Camera3D::propertyName(Camera3DProperty::Fov),
        Camera3D::propertyDefaultValue(Camera3DProperty::Fov));
    for (qsizetype i = 0; i < poses.size(); ++i) {
        if (poses[i].valid)
            camera.setCameraKeyframe(applied.startSec + double(i) / applied.fps,
                                     poseToProjectCameraState(poses[i], base, mapping));
    }
    camera.setSolveMapping(mapping);
    // The cloud replaces any earlier one; it is bounded like the bundle that produced it.
    if (points.size() > sfm::kMaxBundlePoints) points.resize(sfm::kMaxBundlePoints);
    camera.setSolvePoints(points);
    return true;
}
} // namespace camsolve
