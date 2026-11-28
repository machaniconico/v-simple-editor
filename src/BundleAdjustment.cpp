#include "BundleAdjustment.h"
#include "FeatureTracker.h"
#include "TwoViewGeometry.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <array>
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
        track.strength = double(points.size() - result.tracks.size());
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

// Block diagonal camera/point Hessians and only observed camera-point edges.
struct BundleEdge {
    int camera = 0;
    std::array<double,18> h{}; // 6 x 3, row-major
};
struct BundleBlocks {
    QVector<std::array<double,36>> cameras;
    QVector<std::array<double,9>> points;
    QVector<QVector<BundleEdge>> edges;
    QVector<double> gradient;
    BundleBlocks(int frames, int count)
        : cameras(frames-1), points(count), edges(count),
          gradient(6*(frames-1)+3*count,0.0) {}
};

// Apply the same LM diagonal damping before elimination as the dense oracle.
// S = U - W V^-1 W^T, rhs = -gc + W V^-1 gp. Tags used on entry, so the tag
// follows this function rather than the caller's selector.
bool solveBundleSchur(const BundleBlocks& blocks, double lambda, QVector<double>& delta,
                      BundleResult::Backend& used)
{
    used = BundleResult::Backend::Schur;
    const int m = 6*int(blocks.cameras.size());
    QVector<double> reduced(m*m,0.0), rhs(m);
    double* const matrix = reduced.data();
    for (int camera = 0; camera < blocks.cameras.size(); ++camera) {
        const auto& u = blocks.cameras[camera];
        for (int i = 0; i < 6; ++i) {
            const int row = 6*camera+i;
            rhs[row] = -blocks.gradient[row];
            for (int j = 0; j < 6; ++j) matrix[row*m+6*camera+j] = u[6*i+j];
            matrix[row*m+row] += lambda*std::max(u[6*i+i],1e-12);
        }
    }
    QVector<std::array<double,9>> inverses(blocks.points.size());
    for (int point = 0; point < blocks.points.size(); ++point) {
        const auto& v = blocks.points[point];
        QVector<double> damped(9);
        for (int i = 0; i < 9; ++i) damped[i] = v[i];
        for (int i = 0; i < 3; ++i) damped[3*i+i] += lambda*std::max(v[3*i+i],1e-12);
        auto& inverse = inverses[point];
        for (int col = 0; col < 3; ++col) {
            QVector<double> unit(3,0.0), solution;
            unit[col] = 1;
            if (!solveDenseLinearSystem(damped,unit,3,solution)) return false;
            for (int row = 0; row < 3; ++row) inverse[3*row+col] = solution[row];
        }
        const auto& edges = blocks.edges[point];
        for (int a = 0; a < edges.size(); ++a) {
            const auto& left = edges[a];
            double product[18] = {};
            for (int i = 0; i < 6; ++i) for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    product[3*i+j] += left.h[3*i+k]*inverse[3*k+j];
            for (int i = 0; i < 6; ++i) {
                const int row = 6*left.camera+i;
                for (int j = 0; j < 3; ++j)
                    rhs[row] += product[3*i+j]*blocks.gradient[m+3*point+j];
                // Form each scalar in one triangle, mirror it exactly.
                for (int b = 0; b <= a; ++b) {
                    const auto& right = edges[b];
                    for (int j = 0; j < (b == a ? i+1 : 6); ++j) {
                        const int col = 6*right.camera+j;
                        const double value = product[3*i]*right.h[3*j]
                            + product[3*i+1]*right.h[3*j+1]
                            + product[3*i+2]*right.h[3*j+2];
                        matrix[row*m+col] -= value;
                        if (row != col) matrix[col*m+row] -= value;
                    }
                }
            }
        }
    }
    QVector<double> cameraDelta;
    if (!solveDenseLinearSystem(std::move(reduced),std::move(rhs),m,cameraDelta)) return false;
    delta.resize(blocks.gradient.size());
    std::copy(cameraDelta.cbegin(),cameraDelta.cend(),delta.begin());
    for (int point = 0; point < blocks.points.size(); ++point) {
        double pointRhs[3];
        for (int j = 0; j < 3; ++j) pointRhs[j] = -blocks.gradient[m+3*point+j];
        for (const auto& edge : blocks.edges[point])
            for (int j = 0; j < 3; ++j) for (int i = 0; i < 6; ++i)
                pointRhs[j] -= edge.h[3*i+j]*cameraDelta[6*edge.camera+i];
        for (int i = 0; i < 3; ++i) {
            double value = 0;
            for (int j = 0; j < 3; ++j) value += inverses[point][3*i+j]*pointRhs[j];
            if (!std::isfinite(value)) return false;
            delta[m+3*point+i] = value;
        }
    }
    return true;
}

// Optionally accumulate only the nine active columns of each observation.
// Blocks are used in production; the optional dense matrix is the test oracle.
// The observation Jacobian is never allocated globally.
double bundleCost(const MultiViewTracks& tracks, const QVector<BundleCamera>& cameras,
                  const QVector<BundlePoint>& points, const camsolve::Intrinsics& k,
                  QVector<double>* normal = nullptr, QVector<double>* gradient = nullptr,
                  BundleBlocks* blocks = nullptr)
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
            if (!h && !blocks) continue;
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
            if (blocks) {
                auto& v = blocks->points[point];
                for (int i = 0; i < 3; ++i) {
                    blocks->gradient[cameraParams+3*point+i] +=
                        jacobian[0][6+i]*residual[0]+jacobian[1][6+i]*residual[1];
                    for (int j = 0; j < 3; ++j)
                        v[3*i+j] += jacobian[0][6+i]*jacobian[0][6+j]
                                  +jacobian[1][6+i]*jacobian[1][6+j];
                }
                if (frame > 0) {
                    auto& u = blocks->cameras[frame-1];
                    BundleEdge edge;
                    edge.camera = frame-1;
                    for (int i = 0; i < 6; ++i) {
                        blocks->gradient[6*(frame-1)+i] +=
                            jacobian[0][i]*residual[0]+jacobian[1][i]*residual[1];
                        for (int j = 0; j < 6; ++j)
                            u[6*i+j] += jacobian[0][i]*jacobian[0][j]+jacobian[1][i]*jacobian[1][j];
                        for (int j = 0; j < 3; ++j)
                            edge.h[3*i+j] = jacobian[0][i]*jacobian[0][6+j]
                                          +jacobian[1][i]*jacobian[1][6+j];
                    }
                    blocks->edges[point].append(edge);
                }
                continue;
            }
            // Preserved dense assembly, reachable only with the selftest switch.
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
    // Each limit has its own reason so a rejection names the limit that was exceeded.
    if (tracks.frameCount > kMaxBundleFrames) {
        result.reason = QStringLiteral("フレーム数が上限（%1フレーム）を超えています").arg(kMaxBundleFrames);
        return result;
    }
    if (tracks.tracks.size() > kMaxBundlePoints) {
        result.reason = QStringLiteral("点数が上限（%1点）を超えています").arg(kMaxBundlePoints);
        return result;
    }
    if (initialPoints.size() > kMaxBundlePoints) {
        result.reason = QStringLiteral("初期点の数が上限（%1点）を超えています").arg(kMaxBundlePoints);
        return result;
    }
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
    bool converged = cost <= 1e-24;
    const bool denseSelftest = qEnvironmentVariableIntValue("VEDITOR_BUNDLE_DENSE_SELFTEST") == 1;
    // Each solver writes the tag when it is entered, so the tag names the solver that
    // ran rather than the selector; it stays None when no linear solve is attempted.
    const auto solveDense = [&](QVector<double> A, QVector<double> b, QVector<double>& x) {
        result.backend = BundleResult::Backend::Dense;
        return solveDenseLinearSystem(std::move(A),std::move(b),n,x);
    };
    double lambda = 1e-3;
    for (int iteration = 0; iteration < maxIters && cost > 1e-24; ++iteration) {
        QVector<double> normal, gradient;
        BundleBlocks blocks(tracks.frameCount,int(points.size()));
        if (denseSelftest) {
            normal.fill(0.0,n*n);
            gradient.fill(0.0,n);
            bundleCost(tracks,cameras,points,k,&normal,&gradient);
        } else {
            bundleCost(tracks,cameras,points,k,nullptr,nullptr,&blocks);
        }
        bool accepted = false;
        const double oldRms = std::sqrt(cost/observationCount);
        for (int retry = 0; retry < 8; ++retry) {
            QVector<double> delta;
            bool solved = false;
            if (denseSelftest) {
                auto damped = normal;
                double* matrix = damped.data();
                QVector<double> rhs(n);
                for (int i = 0; i < n; ++i) {
                    matrix[i*n+i] += lambda*std::max(normal[i*n+i],1e-12);
                    rhs[i] = -gradient[i];
                }
                solved = solveDense(std::move(damped),std::move(rhs),delta);
            } else {
                solved = solveBundleSchur(blocks,lambda,delta,result.backend);
            }
            if (solved) {
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
        converged = cost <= 1e-24 || (accepted
            && (oldRms-result.rmsHistory.last())/std::max(oldRms,1e-30) < 1e-6);
        // An already exact reconstruction need not accept a numerically smaller step.
        converged = converged || std::sqrt(cost/observationCount) < 1e-4;
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
    result.converged = converged;
    return result;
}

CameraTrackSolve sampleCameraTracks(const MultiViewTracks& tracks)
{
    CameraTrackSolve result;
    if (tracks.frameCount <= 0) return result;
    const int count = std::min(tracks.frameCount, kMaxBundleFrames);
    result.input.frameCount = count;
    for (int i = 0; i < count; ++i)
        result.sourceFrames.append(count == 1 ? 0
            : int(qint64(i) * (tracks.frameCount - 1) / (count - 1)));
    auto ranked = tracks.tracks;
    std::stable_sort(ranked.begin(), ranked.end(), [](const Track2D& a, const Track2D& b) {
        const double x = std::isfinite(a.strength) ? a.strength : 0;
        const double y = std::isfinite(b.strength) ? b.strength : 0;
        return x > y;
    });
    for (auto track : ranked) {
        const auto observations = track.observations;
        track.observations.clear();
        for (const auto& obs : observations) {
            const auto it = std::lower_bound(result.sourceFrames.cbegin(), result.sourceFrames.cend(), obs.first);
            if (it != result.sourceFrames.cend() && *it == obs.first)
                track.observations.append(qMakePair(int(it-result.sourceFrames.cbegin()), obs.second));
        }
        // A point lost before the second retained frame cannot be triangulated.
        // Keep the strongest usable tracks; report the actual number used.
        if (track.observations.size() < 2 || track.observations.first().first != 0) continue;
        result.input.tracks.append(std::move(track));
        if (result.input.tracks.size() == kMaxBundlePoints) break;
    }
    result.thinned = count != tracks.frameCount || result.input.tracks.size() != tracks.tracks.size();
    return result;
}

CameraTrackSolve solveCameraTracks(const MultiViewTracks& tracks, const camsolve::Intrinsics& k)
{
    auto result = sampleCameraTracks(tracks);
    const auto reject = [&](const QString& reason) {
        result.bundle.valid = false;
        result.bundle.reason = reason;
        return result;
    };
    if (result.input.frameCount < 2 || result.input.tracks.size() < 8)
        return reject(QStringLiteral("最低2フレーム・8点が必要です"));
    const auto poses = chainTwoViewPoses(result.input, k);
    for (const auto& pose : poses) if (!pose.valid)
        return reject(QStringLiteral("姿勢の連結に失敗しました"));
    QVector<QVector3D> points;
    for (const auto& track : result.input.tracks) {
        QPointF first, last;
        int lastFrame = -1;
        bool hasFirst = false;
        for (const auto& obs : track.observations) {
            if (obs.first == 0) { first = obs.second; hasFirst = true; }
            if (obs.first > lastFrame) { lastFrame = obs.first; last = obs.second; }
        }
        if (!hasFirst || lastFrame <= 0)
            return reject(QStringLiteral("三角測量に必要な観測が不足しています"));
        const auto& pose = poses[lastFrame];
        const double t[3] = {pose.t.x(), pose.t.y(), pose.t.z()};
        const auto point = triangulate({Correspondence{first, last}}, k, pose.R, t);
        if (point.size() != 1) return reject(QStringLiteral("三角測量に失敗しました"));
        points.append(point.first());
    }
    result.bundle = bundleAdjust(result.input, poses, points, k, 40);
    if (!result.bundle.valid || !result.bundle.converged || result.bundle.rms > 2.0)
        return reject(result.bundle.reason.isEmpty()
            ? QStringLiteral("未収束、または RMS が許容値 2 px を超えました") : result.bundle.reason);
    return result;
}

QVector<camsolve::Pose> toCameraPoses(const BundleResult& result)
{
    return result.valid ? result.poses : QVector<camsolve::Pose>{};
}

} // namespace sfm
