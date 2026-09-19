#include "BundleAdjustment.h"
#include "FeatureTracker.h"
#include "TwoViewGeometry.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

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
    const auto at = [&](int row, int col) -> double& { return A[qsizetype(row)*n+col]; };
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

} // namespace sfm
