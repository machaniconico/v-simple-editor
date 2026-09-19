#include "BundleAdjustment.h"
#include "FeatureTracker.h"
#include "TwoViewGeometry.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sfm {

bool solveDenseLinearSystem(QVector<double> A, QVector<double> b, int n,
                            QVector<double>& x)
{
    x.clear();
    if (n <= 0 || qint64(n)*n != A.size() || b.size() != n) return false;
    double scale = 0;
    for (double value : A) {
        if (!std::isfinite(value)) return false;
        scale = std::max(scale, std::abs(value));
    }
    for (double value : b) if (!std::isfinite(value)) return false;
    if (!(scale > 0)) return false;
    // Normalize globally so the singularity tolerance is independent of units.
    for (double& value : A) value /= scale;
    for (double& value : b) {
        value /= scale;
        if (!std::isfinite(value)) return false;
    }
    const double tolerance = std::numeric_limits<double>::epsilon()*n;
    double* const matrix = A.data();
    const auto at = [&](int row, int col) -> double& { return matrix[qsizetype(row)*n+col]; };
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int row = col+1; row < n; ++row)
            if (std::abs(at(row,col)) > std::abs(at(pivot,col))) pivot = row;
        if (!std::isfinite(at(pivot,col)) || std::abs(at(pivot,col)) <= tolerance) return false;
        if (pivot != col) {
            for (int j = col; j < n; ++j) std::swap(at(col,j),at(pivot,j));
            std::swap(b[col],b[pivot]);
        }
        for (int row = col+1; row < n; ++row) {
            const double factor = at(row,col)/at(col,col);
            at(row,col) = 0;
            for (int j = col+1; j < n; ++j) at(row,j) -= factor*at(col,j);
            b[row] -= factor*b[col];
        }
    }
    QVector<double> solution(n);
    for (int row = n-1; row >= 0; --row) {
        double value = b[row];
        for (int j = row+1; j < n; ++j) value -= at(row,j)*solution[j];
        solution[row] = value/at(row,row);
        if (!std::isfinite(solution[row])) return false;
    }
    x = solution;
    return true;
}

MultiViewTracks trackAcrossFrames(const QVector<QImage>& frames, int maxPoints)
{
    MultiViewTracks result;
    if (frames.size() > std::numeric_limits<int>::max()) return result;
    result.frameCount = int(frames.size());
    if (frames.isEmpty() || maxPoints <= 0) return result;
    QImage previous = frames[0].convertToFormat(QImage::Format_Grayscale8);
    const auto points = feattrack::detectShiTomasi(previous,maxPoints);
    QVector<feattrack::Track> live;
    for (const auto& point : points) {
        Track2D track;
        track.pointId = int(result.tracks.size());
        track.observations.append(qMakePair(0,point));
        result.tracks.append(track);
        live.append({point,0.0f,true});
    }
    for (int frame = 1; frame < result.frameCount && !live.isEmpty(); ++frame) {
        const QImage next = frames[frame].convertToFormat(QImage::Format_Grayscale8);
        feattrack::trackPyramidalLK(previous,next,live,3,15,20,0.01,true,1.0);
        bool anyAlive = false;
        for (qsizetype i = 0; i < live.size(); ++i) if (live[i].alive) {
            result.tracks[i].observations.append(qMakePair(frame,live[i].pt));
            anyAlive = true;
        }
        if (!anyAlive) break;
        previous = next;
    }
    return result;
}

QVector<camsolve::Pose> chainTwoViewPoses(const MultiViewTracks& tracks,
                                         const camsolve::Intrinsics& intrinsics)
{
    if (tracks.frameCount <= 0) return {};
    QVector<camsolve::Pose> poses(tracks.frameCount);
    if (!std::isfinite(intrinsics.f) || intrinsics.f <= 0
        || !std::isfinite(intrinsics.cx) || !std::isfinite(intrinsics.cy)) return poses;
    QVector<QHash<int,QPointF>> frames(tracks.frameCount);
    QSet<int> ids;
    for (const auto& track : tracks.tracks) {
        if (ids.contains(track.pointId)) return poses;
        ids.insert(track.pointId);
        int previousFrame = -1;
        for (const auto& observation : track.observations) {
            const int frame = observation.first;
            if (frame < 0 || frame >= tracks.frameCount || frame <= previousFrame
                || !std::isfinite(observation.second.x())
                || !std::isfinite(observation.second.y())) return poses;
            frames[frame].insert(track.pointId,observation.second);
            previousFrame = frame;
        }
    }
    poses[0].valid = true;
    poses[0].residual = 0;
    QHash<int,double> previousDepths;
    double worldT[3] = {};
    for (int frame = 1; frame < tracks.frameCount; ++frame) {
        QVector<Correspondence> pairs;
        QVector<int> sharedIds;
        // Track order keeps RANSAC deterministic, independent of QHash's seed.
        for (const auto& track : tracks.tracks) {
            const auto a = frames[frame-1].constFind(track.pointId);
            const auto b = frames[frame].constFind(track.pointId);
            if (a != frames[frame-1].cend() && b != frames[frame].cend()) {
                pairs.append({a.value(),b.value()});
                sharedIds.append(track.pointId);
            }
        }
        if (pairs.size() < 8) break;
        const auto relative = estimateRelativePose(pairs,intrinsics);
        if (!relative.valid) break;
        const auto points = triangulate(pairs,intrinsics,relative.R,relative.t);
        QVector<double> ratios;
        QHash<int,double> nextDepths;
        for (qsizetype i = 0; i < points.size(); ++i) {
            if (!relative.inliers[i]) continue;
            const auto& p = points[i];
            const double depth = relative.R[6]*p.x()+relative.R[7]*p.y()
                               + relative.R[8]*p.z()+relative.t[2];
            if (!(p.z() > 0) || !(depth > 0) || !std::isfinite(depth)
                || reprojectionError(pairs[i],p,intrinsics,relative.R,relative.t) > 2.0) continue;
            nextDepths.insert(sharedIds[i],depth);
            const auto previous = previousDepths.constFind(sharedIds[i]);
            if (previous != previousDepths.cend()) {
                const double ratio = previous.value()/p.z();
                if (ratio > 0 && std::isfinite(ratio)) ratios.append(ratio);
            }
        }
        double scale = 1;
        if (frame > 1) {
            if (ratios.size() < 8) break;
            std::sort(ratios.begin(),ratios.end());
            const qsizetype middle = ratios.size()/2;
            scale = ratios.size()%2 ? ratios[middle]
                                   : 0.5*ratios[middle-1]+0.5*ratios[middle];
        }
        for (auto it = nextDepths.begin(); it != nextDepths.end(); ++it) it.value() *= scale;
        camsolve::Pose pose;
        double nextT[3] = {};
        for (int row = 0; row < 3; ++row) {
            nextT[row] = scale*relative.t[row];
            for (int k = 0; k < 3; ++k) nextT[row] += relative.R[3*row+k]*worldT[k];
            for (int col = 0; col < 3; ++col) {
                pose.R[3*row+col] = 0;
                for (int k = 0; k < 3; ++k)
                    pose.R[3*row+col] += relative.R[3*row+k]*poses[frame-1].R[3*k+col];
            }
        }
        pose.t = QVector3D(float(nextT[0]),float(nextT[1]),float(nextT[2]));
        if (!std::isfinite(pose.t.x()) || !std::isfinite(pose.t.y()) || !std::isfinite(pose.t.z())) break;
        // Pose::n belongs to planar homography decomposition; no plane is
        // estimated here, so retain its default value.
        pose.valid = true;
        pose.residual = relative.meanReprojErr;
        poses[frame] = pose;
        for (int row = 0; row < 3; ++row) worldT[row] = nextT[row];
        previousDepths = nextDepths;
    }
    return poses;
}

namespace {
struct BundleCamera { double R[9]; double t[3]; };
struct BundlePoint { double x[3]; };

// Exp([w]x) * R: three local axis-angle parameters avoid Euler singularities.
void rotateBundleCamera(BundleCamera& camera, const double* w)
{
    const double theta2 = w[0]*w[0]+w[1]*w[1]+w[2]*w[2];
    const double theta = std::sqrt(theta2);
    const double a = theta2 < 1e-12 ? 1-theta2/6 : std::sin(theta)/theta;
    const double b = theta2 < 1e-12 ? 0.5-theta2/24 : (1-std::cos(theta))/theta2;
    const double skew[9] = {0,-w[2],w[1], w[2],0,-w[0], -w[1],w[0],0};
    double increment[9] = {}, rotated[9] = {};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        increment[3*i+j] = (i == j ? 1.0 : 0.0)+a*skew[3*i+j];
        for (int k = 0; k < 3; ++k) increment[3*i+j] += b*skew[3*i+k]*skew[3*k+j];
    }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) rotated[3*i+j] += increment[3*i+k]*camera.R[3*k+j];
    std::copy(rotated,rotated+9,camera.R);
}

// Optionally accumulate only the nine active columns of each observation.
// The normal matrix is dense; the observation Jacobian is never allocated.
double bundleCost(const MultiViewTracks& tracks, const QVector<BundleCamera>& cameras,
                  const QVector<BundlePoint>& points, const camsolve::Intrinsics& k,
                  QVector<double>* normal = nullptr, QVector<double>* gradient = nullptr)
{
    const int cameraParams = 6*(tracks.frameCount-1);
    const int n = cameraParams+3*int(points.size());
    double* h = normal ? normal->data() : nullptr;
    double* g = gradient ? gradient->data() : nullptr;
    double cost = 0;
    for (int point = 0; point < points.size(); ++point) {
        for (const auto& observation : tracks.tracks[point].observations) {
            const int frame = observation.first;
            const auto& camera = cameras[frame];
            double rotated[3] = {}, p[3];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) rotated[i] += camera.R[3*i+j]*points[point].x[j];
                p[i] = rotated[i]+camera.t[i];
            }
            if (!(p[2] > 1e-9)) return std::numeric_limits<double>::infinity();
            const double residual[2] = {k.f*p[0]/p[2]+k.cx-observation.second.x(),
                                        k.f*p[1]/p[2]+k.cy-observation.second.y()};
            cost += residual[0]*residual[0]+residual[1]*residual[1];
            if (!h) continue;
            const double projection[2][3] = {{k.f/p[2],0,-k.f*p[0]/(p[2]*p[2])},
                                              {0,k.f/p[2],-k.f*p[1]/(p[2]*p[2])}};
            const double rotation[3][3] = {{0,rotated[2],-rotated[1]},
                                          {-rotated[2],0,rotated[0]},
                                          {rotated[1],-rotated[0],0}};
            double jacobian[2][9] = {};
            int indices[9];
            for (int j = 0; j < 6; ++j) indices[j] = 6*(frame-1)+j;
            for (int j = 0; j < 3; ++j) indices[6+j] = cameraParams+3*point+j;
            for (int row = 0; row < 2; ++row) for (int j = 0; j < 3; ++j) {
                jacobian[row][3+j] = projection[row][j];
                for (int axis = 0; axis < 3; ++axis) {
                    jacobian[row][j] += projection[row][axis]*rotation[axis][j];
                    jacobian[row][6+j] += projection[row][axis]*camera.R[3*axis+j];
                }
            }
            for (int i = frame == 0 ? 6 : 0; i < 9; ++i) {
                g[indices[i]] += jacobian[0][i]*residual[0]+jacobian[1][i]*residual[1];
                for (int j = frame == 0 ? 6 : 0; j < 9; ++j)
                    h[indices[i]*n+indices[j]] += jacobian[0][i]*jacobian[0][j]
                                                  +jacobian[1][i]*jacobian[1][j];
            }
        }
    }
    return cost;
}
} // namespace

BundleResult bundleAdjust(const MultiViewTracks& tracks, QVector<camsolve::Pose> initialPoses,
                          QVector<QVector3D> initialPoints, const camsolve::Intrinsics& k,
                          int maxIters)
{
    BundleResult result;
    const auto reject = [&](const char* reason) {
        result.reason = QString::fromUtf8(reason);
        return result;
    };
    if (tracks.frameCount > 30 || tracks.tracks.size() > 150 || initialPoints.size() > 150)
        return reject("上限は30フレーム、150点です");
    if (tracks.frameCount < 2 || tracks.tracks.isEmpty()
        || initialPoses.size() != tracks.frameCount || initialPoints.size() != tracks.tracks.size()
        || maxIters < 0) return reject("初期姿勢・点と観測のサイズが不正です");
    if (!(k.f > 0) || !std::isfinite(k.f) || !std::isfinite(k.cx) || !std::isfinite(k.cy))
        return reject("内部パラメータが不正です");
    QVector<BundleCamera> cameras(tracks.frameCount);
    QVector<BundlePoint> points(initialPoints.size());
    QVector<int> frameObservations(tracks.frameCount,0);
    QSet<int> ids;
    int observationCount = 0;
    for (int i = 0; i < tracks.frameCount; ++i) {
        const auto& pose = initialPoses[i];
        if (!pose.valid) return reject("無効な初期姿勢です");
        for (int j = 0; j < 9; ++j) {
            if (!std::isfinite(pose.R[j])) return reject("回転が有限ではありません");
            cameras[i].R[j] = pose.R[j];
        }
        // Reject reflections and non-rotations instead of optimizing malformed poses.
        for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
            double dot = 0;
            for (int j = 0; j < 3; ++j) dot += pose.R[3*a+j]*pose.R[3*b+j];
            if (std::abs(dot-(a == b ? 1.0 : 0.0)) > 1e-5) return reject("回転行列が不正です");
        }
        const double* r = pose.R;
        const double det = r[0]*(r[4]*r[8]-r[5]*r[7])-r[1]*(r[3]*r[8]-r[5]*r[6])
                         +r[2]*(r[3]*r[7]-r[4]*r[6]);
        if (det < 0) return reject("回転行列が反転しています");
        for (int j = 0; j < 3; ++j) {
            cameras[i].t[j] = pose.t[j];
            if (!std::isfinite(cameras[i].t[j])) return reject("並進が有限ではありません");
        }
    }
    for (int i = 0; i < tracks.tracks.size(); ++i) {
        const auto& track = tracks.tracks[i];
        if (ids.contains(track.pointId) || track.observations.size() < 2)
            return reject("点IDが重複しているか観測が不足しています");
        ids.insert(track.pointId);
        QSet<int> seen;
        for (const auto& observation : track.observations) {
            const int frame = observation.first;
            if (frame < 0 || frame >= tracks.frameCount || seen.contains(frame)
                || !std::isfinite(observation.second.x()) || !std::isfinite(observation.second.y()))
                return reject("観測が不正です");
            seen.insert(frame);
            ++frameObservations[frame];
            ++observationCount;
        }
        for (int j = 0; j < 3; ++j) {
            points[i].x[j] = initialPoints[i][j];
            if (!std::isfinite(points[i].x[j])) return reject("初期点が有限ではありません");
        }
    }
    for (int count : frameObservations) if (count < 3) return reject("フレームの観測が不足しています");
    double cost = bundleCost(tracks,cameras,points,k);
    if (!std::isfinite(cost)) return reject("初期点がカメラの背後にあるか投影が不正です");
    result.rmsHistory.append(std::sqrt(cost/observationCount));
    const int cameraParams = 6*(tracks.frameCount-1), n = cameraParams+3*int(points.size());
    double lambda = 1e-3;
    for (int iteration = 0; iteration < maxIters && cost > 1e-24; ++iteration) {
        QVector<double> normal(n*n,0.0), gradient(n,0.0);
        bundleCost(tracks,cameras,points,k,&normal,&gradient);
        bool accepted = false;
        const double oldRms = std::sqrt(cost/observationCount);
        for (int retry = 0; retry < 8; ++retry) {
            auto damped = normal;
            double* matrix = damped.data();
            QVector<double> rhs(n), delta;
            for (int i = 0; i < n; ++i) {
                matrix[i*n+i] += lambda*std::max(normal[i*n+i],1e-12);
                rhs[i] = -gradient[i];
            }
            if (solveDenseLinearSystem(std::move(damped),std::move(rhs),n,delta)) {
                auto candidateCameras = cameras;
                auto candidatePoints = points;
                for (int frame = 1; frame < tracks.frameCount; ++frame) {
                    rotateBundleCamera(candidateCameras[frame],delta.constData()+6*(frame-1));
                    for (int j = 0; j < 3; ++j) candidateCameras[frame].t[j] += delta[6*(frame-1)+3+j];
                }
                for (int point = 0; point < points.size(); ++point)
                    for (int j = 0; j < 3; ++j) candidatePoints[point].x[j] += delta[cameraParams+3*point+j];
                const double nextCost = bundleCost(tracks,candidateCameras,candidatePoints,k);
                if (std::isfinite(nextCost) && nextCost < cost) {
                    cameras = std::move(candidateCameras);
                    points = std::move(candidatePoints);
                    cost = nextCost;
                    result.rmsHistory.append(std::sqrt(cost/observationCount));
                    lambda = std::max(lambda/10,1e-15);
                    accepted = true;
                    break;
                }
            }
            lambda *= 10;
        }
        if (!accepted || (oldRms-result.rmsHistory.last())/std::max(oldRms,1e-30) < 1e-6) break;
    }
    result.poses = std::move(initialPoses);
    result.points.resize(points.size());
    for (int frame = 0; frame < tracks.frameCount; ++frame) {
        // Preserve the fixed pose exactly, including its metadata.
        if (frame == 0) continue;
        auto& pose = result.poses[frame];
        std::copy(cameras[frame].R,cameras[frame].R+9,pose.R);
        pose.t = QVector3D(float(cameras[frame].t[0]),float(cameras[frame].t[1]),float(cameras[frame].t[2]));
        for (int j = 0; j < 3; ++j) {
            if (!std::isfinite(pose.t[j])) return reject("結果が浮動小数点範囲を超えています");
            cameras[frame].t[j] = pose.t[j];
        }
    }
    for (int point = 0; point < points.size(); ++point) {
        result.points[point] = QVector3D(float(points[point].x[0]),float(points[point].x[1]),float(points[point].x[2]));
        for (int j = 0; j < 3; ++j) {
            if (!std::isfinite(result.points[point][j])) return reject("結果が浮動小数点範囲を超えています");
            points[point].x[j] = result.points[point][j];
        }
    }
    // Report the RMS of the public (float-coordinate) reconstruction.
    result.rms = std::sqrt(bundleCost(tracks,cameras,points,k)/observationCount);
    if (!std::isfinite(result.rms)) return reject("結果の投影が不正です");
    for (int frame = 1; frame < result.poses.size(); ++frame) result.poses[frame].residual = result.rms;
    result.valid = true;
    return result;
}

QVector<camsolve::Pose> toCameraPoses(const BundleResult& result)
{
    return result.valid ? result.poses : QVector<camsolve::Pose>{};
}

} // namespace sfm
