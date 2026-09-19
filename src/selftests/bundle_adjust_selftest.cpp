#include "../BundleAdjustment.h"

#include <QElapsedTimer>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>

namespace {
constexpr double pi = 3.14159265358979323846;

bool solveKnownSystem(int n)
{
    std::mt19937 rng(513);
    QVector<double> random(n*n), matrix(n*n,0.0), truth(n), rhs(n,0.0), solution;
    for (double& value : random) value = double(rng())/4294967296.0-0.5;
    for (int i = 0; i < n; ++i) {
        truth[i] = std::sin(double(i+1));
        for (int j = 0; j < n; ++j) {
            for (int k = 0; k < n; ++k) matrix[i*n+j] += random[k*n+i]*random[k*n+j];
            if (i == j) matrix[i*n+j] += 1;
        }
    }
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) rhs[i] += matrix[i*n+j]*truth[j];
    if (!sfm::solveDenseLinearSystem(matrix,rhs,n,solution) || solution.size() != n) return false;
    double maxError = 0;
    for (int i = 0; i < n; ++i) maxError = std::max(maxError,std::abs(solution[i]-truth[i]));
    std::cerr << n << "x" << n << " solution error: " << maxError << '\n';
    return maxError < 1e-9;
}

// Same seeded rectangle texture as feature_tracker_selftest.cpp.
QImage texture()
{
    QImage image(640,480,QImage::Format_Grayscale8);
    image.fill(96);
    std::mt19937 rng(12345);
    for (int i = 0; i < 200; ++i) {
        const int w = 8+int(rng()%33), h = 8+int(rng()%33);
        const int x = 48+int(rng()%(image.width()-w-96));
        const int y = 48+int(rng()%(image.height()-h-96));
        const unsigned char value = static_cast<unsigned char>(rng()%256);
        for (int py = y; py < y+h; ++py)
            for (int px = x; px < x+w; ++px) image.scanLine(py)[px] = value;
    }
    return image;
}

QImage translate(const QImage& source, int dx, int dy)
{
    QImage image(source.size(),QImage::Format_Grayscale8);
    image.fill(96);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const int sx = x-dx, sy = y-dy;
            if (sx >= 0 && sy >= 0 && sx < source.width() && sy < source.height())
                image.scanLine(y)[x] = source.constScanLine(sy)[sx];
        }
    return image;
}

struct Camera {
    double R[9];
    double t[3];
};

sfm::MultiViewTracks scene(QVector<Camera>& cameras, double& firstBaseline,
                           const camsolve::Intrinsics& k)
{
    sfm::MultiViewTracks tracks;
    tracks.frameCount = 10;
    cameras.clear();
    for (int frame = 0; frame < tracks.frameCount; ++frame) {
        // Nonuniform arc steps exercise changing baseline scale, not just direction.
        const double angle = 0.04*frame+0.003*frame*frame;
        const double c = std::cos(angle), s = std::sin(angle);
        const double center[3] = {6*s,0.2*std::sin(2*angle),6*(1-c)};
        Camera camera{{c,0,s, 0,1,0, -s,0,c},{0,0,0}};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col) camera.t[row] -= camera.R[3*row+col]*center[col];
        cameras.append(camera);
    }
    firstBaseline = std::sqrt(cameras[1].t[0]*cameras[1].t[0]
        + cameras[1].t[1]*cameras[1].t[1]+cameras[1].t[2]*cameras[1].t[2]);
    std::mt19937 rng(48213);
    const auto uniform = [&]() { return (double(rng())+0.5)/4294967296.0; };
    for (int id = 0; id < 200; ++id) {
        const double x = 3*uniform()-1.5, y = 2*uniform()-1, z = 4+4*uniform();
        const double point[3] = {x,y,z};
        sfm::Track2D track;
        track.pointId = 1000+3*id; // IDs need not be vector indices.
        for (int frame = 0; frame < tracks.frameCount; ++frame) {
            const auto& camera = cameras[frame];
            double p[3] = {};
            for (int row = 0; row < 3; ++row) {
                p[row] = camera.t[row];
                for (int col = 0; col < 3; ++col) p[row] += camera.R[3*row+col]*point[col];
            }
            track.observations.append(qMakePair(frame,QPointF(k.f*p[0]/p[2]+k.cx,k.f*p[1]/p[2]+k.cy)));
        }
        tracks.tracks.append(track);
    }
    return tracks;
}

bool accuratePoses(const QVector<camsolve::Pose>& poses, const QVector<Camera>& truth, double baseline)
{
    if (poses.size() != truth.size()) return false;
    bool ok = true;
    for (qsizetype frame = 0; frame < poses.size(); ++frame) {
        double trace = 0, errorSquared = 0, normSquared = 0;
        for (int i = 0; i < 9; ++i) trace += poses[frame].R[i]*truth[frame].R[i];
        const double rotationError = std::acos(std::clamp((trace-1)/2,-1.0,1.0))*180/pi;
        for (int i = 0; i < 3; ++i) {
            const double expected = truth[frame].t[i]/baseline;
            const double delta = poses[frame].t[i]-expected;
            errorSquared += delta*delta;
            normSquared += expected*expected;
        }
        const double translationError = std::sqrt(errorSquared/std::max(normSquared,1e-30));
        std::cerr << "frame " << frame << ": rotation=" << rotationError
                  << " deg, relative translation error=" << translationError << '\n';
        ok = ok && poses[frame].valid && std::isfinite(poses[frame].residual)
                && rotationError < 1 && translationError < 0.05;
    }
    return ok;
}

struct BundleScene {
    sfm::MultiViewTracks tracks;
    QVector<camsolve::Pose> truth, initial;
    QVector<QVector3D> points;
};

BundleScene bundleScene(const camsolve::Intrinsics& k, bool noisy)
{
    BundleScene scene;
    scene.tracks.frameCount = 20;
    std::mt19937 rng(518);
    const auto uniform = [&]() { return (double(rng())+0.5)/4294967296.0; };
    for (int frame = 0; frame < 20; ++frame) {
        const double angle = 0.025*frame;
        const double c = std::cos(angle), s = std::sin(angle);
        camsolve::Pose pose;
        const double r[9] = {c,0,s, 0,1,0, -s,0,c};
        std::copy(r,r+9,pose.R);
        const double center[3] = {6*s,0.2*std::sin(2*angle),6*(1-c)};
        for (int i = 0; i < 3; ++i) {
            double t = 0;
            for (int j = 0; j < 3; ++j) t -= r[3*i+j]*center[j];
            pose.t[i] = float(t);
        }
        pose.valid = true;
        pose.residual = 0;
        scene.truth.append(pose);
        if (frame) {
            // Exactly five degrees about a varying axis, plus 10% translation noise.
            double axis[3] = {uniform()-0.5,uniform()-0.5,uniform()-0.5};
            const double length = std::sqrt(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
            for (double& value : axis) value /= length;
            const double a = 5*pi/180, ca = std::cos(a), sa = std::sin(a);
            const double skew[9] = {0,-axis[2],axis[1], axis[2],0,-axis[0], -axis[1],axis[0],0};
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
                pose.R[3*i+j] = 0;
                for (int q = 0; q < 3; ++q)
                    pose.R[3*i+j] += ((i == q ? ca : 0)+(1-ca)*axis[i]*axis[q]+sa*skew[3*i+q])*r[3*q+j];
            }
            const double magnitude = pose.t.length()*0.1;
            for (int j = 0; j < 3; ++j) pose.t[j] += float(magnitude*axis[j]);
        }
        scene.initial.append(pose);
    }
    for (int point = 0; point < 100; ++point) {
        const QVector3D truth(float(3*uniform()-1.5),float(2*uniform()-1),float(5+3*uniform()));
        sfm::Track2D track;
        track.pointId = 1000+point*3;
        for (int frame = 0; frame < 20; ++frame) {
            const auto& pose = scene.truth[frame];
            double p[3] = {};
            for (int i = 0; i < 3; ++i) {
                p[i] = pose.t[i];
                for (int j = 0; j < 3; ++j) p[i] += pose.R[3*i+j]*truth[j];
            }
            // Uniform noise with standard deviation 0.5 px per coordinate.
            const double nx = noisy ? (uniform()-0.5)*std::sqrt(3.0) : 0;
            const double ny = noisy ? (uniform()-0.5)*std::sqrt(3.0) : 0;
            track.observations.append(qMakePair(frame,QPointF(k.f*p[0]/p[2]+k.cx+nx,k.f*p[1]/p[2]+k.cy+ny)));
        }
        scene.tracks.tracks.append(track);
        scene.points.append(truth*float(1+0.1*(2*uniform()-1)));
    }
    return scene;
}

double measuredBundleRms(const sfm::BundleResult& result, const sfm::MultiViewTracks& tracks,
                         const camsolve::Intrinsics& k)
{
    if (!result.valid || result.points.size() != tracks.tracks.size()
        || result.poses.size() != tracks.frameCount) return std::numeric_limits<double>::infinity();
    double squared = 0;
    int count = 0;
    for (int point = 0; point < tracks.tracks.size(); ++point)
        for (const auto& observation : tracks.tracks[point].observations) {
            const auto& pose = result.poses[observation.first];
            double p[3] = {};
            for (int i = 0; i < 3; ++i) {
                p[i] = pose.t[i];
                for (int j = 0; j < 3; ++j) p[i] += pose.R[3*i+j]*result.points[point][j];
            }
            if (!(p[2] > 0)) return std::numeric_limits<double>::infinity();
            const double dx = k.f*p[0]/p[2]+k.cx-observation.second.x();
            const double dy = k.f*p[1]/p[2]+k.cy-observation.second.y();
            squared += dx*dx+dy*dy;
            ++count;
        }
    return count ? std::sqrt(squared/count) : std::numeric_limits<double>::infinity();
}

bool bundleAccuracy(const sfm::BundleResult& result, const BundleScene& scene)
{
    if (!result.valid || result.poses.size() != scene.truth.size()) return false;
    double maxRotation = 0, maxDirection = 0;
    for (int frame = 1; frame < result.poses.size(); ++frame) {
        double trace = 0;
        for (int i = 0; i < 9; ++i) trace += result.poses[frame].R[i]*scene.truth[frame].R[i];
        maxRotation = std::max(maxRotation,std::acos(std::clamp((trace-1)/2,-1.0,1.0))*180/pi);
        double dot = 0, aa = 0, bb = 0;
        for (int j = 0; j < 3; ++j) {
            const double a = result.poses[frame].t[j], b = scene.truth[frame].t[j];
            dot += a*b; aa += a*a; bb += b*b;
        }
        if (!(aa > 0 && bb > 0)) return false;
        maxDirection = std::max(maxDirection,std::acos(std::clamp(dot/std::sqrt(aa*bb),-1.0,1.0))*180/pi);
    }
    std::cerr << "bundle rotation=" << maxRotation << " deg, direction=" << maxDirection
              << " deg, RMS=" << result.rms << " px\n";
    return maxRotation < 0.2 && maxDirection < 1 && result.rms < 0.05;
}

bool samePoseBits(const camsolve::Pose& a, const camsolve::Pose& b)
{
    if (std::memcmp(a.R,b.R,sizeof(a.R)) || a.valid != b.valid
        || std::memcmp(&a.residual,&b.residual,sizeof(double))) return false;
    for (int j = 0; j < 3; ++j) {
        const float at = a.t[j], bt = b.t[j], an = a.n[j], bn = b.n[j];
        if (std::memcmp(&at,&bt,sizeof(float)) || std::memcmp(&an,&bn,sizeof(float))) return false;
    }
    return true;
}

bool sameBundleBits(const sfm::BundleResult& a, const sfm::BundleResult& b)
{
    if (!a.valid || !b.valid || a.reason != b.reason || a.poses.size() != b.poses.size()
        || a.points.size() != b.points.size() || a.rmsHistory.size() != b.rmsHistory.size()
        || std::memcmp(&a.rms,&b.rms,sizeof(double))) return false;
    for (int i = 0; i < a.poses.size(); ++i) if (!samePoseBits(a.poses[i],b.poses[i])) return false;
    for (int i = 0; i < a.points.size(); ++i) for (int j = 0; j < 3; ++j) {
        const float x = a.points[i][j], y = b.points[i][j];
        if (std::memcmp(&x,&y,sizeof(float))) return false;
    }
    return !std::memcmp(a.rmsHistory.constData(),b.rmsHistory.constData(),a.rmsHistory.size()*sizeof(double));
}
} // namespace

int runBundleAdjustSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::cerr << (ok ? "PASS G" : "FAIL G") << number << '\n';
        if (ok) ++passed; else ++failed;
    };
    bool solverOk = solveKnownSystem(3);
    solverOk = solveKnownSystem(50) && solverOk;
    QVector<double> x;
    solverOk = sfm::solveDenseLinearSystem({0,2,1,3},{4,7},2,x)
        && x.size() == 2 && std::abs(x[0]-1) < 1e-9 && std::abs(x[1]-2) < 1e-9 && solverOk;
    solverOk = !sfm::solveDenseLinearSystem({1,2,2,4},{3,6},2,x) && x.isEmpty() && solverOk;
    solverOk = !sfm::solveDenseLinearSystem({1},{1},2,x)
        && !sfm::solveDenseLinearSystem({},{},0,x)
        && !sfm::solveDenseLinearSystem({std::numeric_limits<double>::quiet_NaN()},{1},1,x)
        && !sfm::solveDenseLinearSystem({1},{std::numeric_limits<double>::infinity()},1,x) && solverOk;
    gate(1,solverOk);

    const QImage base = texture();
    QVector<QImage> frames;
    for (int frame = 0; frame < 10; ++frame) frames.append(translate(base,2*frame,-frame));
    const auto tracked = sfm::trackAcrossFrames(frames);
    int complete = 0;
    bool trackingOk = tracked.frameCount == 10 && tracked.tracks.size() <= 150;
    for (qsizetype i = 0; i < tracked.tracks.size(); ++i) {
        const auto& track = tracked.tracks[i];
        trackingOk = trackingOk && track.pointId == int(i) && !track.observations.isEmpty();
        bool accurate = track.observations.size() == 10;
        for (qsizetype j = 0; j < track.observations.size(); ++j) {
            const auto& observation = track.observations[j];
            const QPointF expected = track.observations[0].second+QPointF(2*j,-j);
            trackingOk = trackingOk && observation.first == j;
            accurate = accurate
                && std::hypot(observation.second.x()-expected.x(),observation.second.y()-expected.y()) < 0.5;
        }
        if (accurate) ++complete;
    }
    auto occludedFrames = frames;
    for (int frame = 5; frame < 8; ++frame) {
        occludedFrames[frame] = frames[frame].copy();
        for (int y = 0; y < base.height(); ++y)
            for (int xPixel = 0; xPixel < 320+2*frame; ++xPixel)
                occludedFrames[frame].scanLine(y)[xPixel] = 0;
    }
    const auto occluded = sfm::trackAcrossFrames(occludedFrames);
    int hidden = 0, terminated = 0, visibleComplete = 0;
    for (const auto& track : occluded.tracks) {
        // Exclude points that already died in the unobscured sequence, so this
        // assertion measures rejection caused specifically by the occluder.
        if (track.observations[0].second.x() < 300
            && tracked.tracks[track.pointId].observations.size() == 10) {
            ++hidden;
            if (track.observations.size() == 5 && track.observations.last().first == 4) ++terminated;
        }
        if (track.observations[0].second.x() > 350 && track.observations.size() == 10) ++visibleComplete;
        for (qsizetype j = 0; j < track.observations.size(); ++j)
            trackingOk = trackingOk && track.observations[j].first == j;
    }
    std::cerr << "10-frame tracks: " << complete << ", occlusion terminated: " << terminated
              << '/' << hidden << ", visible complete: " << visibleComplete << '\n';
    gate(2,trackingOk && complete >= 100 && hidden > 0 && terminated == hidden && visibleComplete > 0
         && sfm::trackAcrossFrames({}).frameCount == 0
         && sfm::trackAcrossFrames(frames,0).tracks.isEmpty());

    const camsolve::Intrinsics k{1000,960,540};
    QVector<Camera> truth;
    double baseline = 0;
    const auto observations = scene(truth,baseline,k);
    bool chainOk = accuratePoses(sfm::chainTwoViewPoses(observations,k),truth,baseline);
    // Change membership across pairs; scale must match by point ID.
    auto changing = observations;
    for (int i = 0; i < 60; ++i)
        changing.tracks[i].observations = i < 30 ? changing.tracks[i].observations.mid(0,5)
                                                : changing.tracks[i].observations.mid(3);
    chainOk = accuratePoses(sfm::chainTwoViewPoses(changing,k),truth,baseline) && chainOk;
    auto insufficient = observations;
    for (int i = 7; i < insufficient.tracks.size(); ++i) insufficient.tracks[i].observations.removeAt(5);
    const auto stopped = sfm::chainTwoViewPoses(insufficient,k);
    chainOk = chainOk && stopped.size() == 10;
    for (int frame = 0; frame < stopped.size(); ++frame)
        chainOk = chainOk && stopped[frame].valid == (frame < 5);
    // Both adjacent pairs have 100 points, but no three-frame scale connection.
    auto disconnected = observations;
    disconnected.frameCount = 3;
    for (int i = 0; i < disconnected.tracks.size(); ++i)
        disconnected.tracks[i].observations = disconnected.tracks[i].observations.mid(i < 100 ? 0 : 1,2);
    const auto unscaled = sfm::chainTwoViewPoses(disconnected,k);
    chainOk = chainOk && unscaled.size() == 3 && unscaled[0].valid && unscaled[1].valid && !unscaled[2].valid
        && sfm::chainTwoViewPoses({},k).isEmpty();
    const auto badK = sfm::chainTwoViewPoses(observations,{0,960,540});
    for (const auto& pose : badK) chainOk = chainOk && !pose.valid;
    gate(3,chainOk);
    const auto cleanScene = bundleScene(k,false);
    QElapsedTimer timer;
    timer.start();
    const auto adjusted = sfm::bundleAdjust(cleanScene.tracks,cleanScene.initial,cleanScene.points,k);
    const qint64 elapsedMs = timer.elapsed();
    const auto cameraPoses = sfm::toCameraPoses(adjusted);
    bool converted = cameraPoses.size() == cleanScene.truth.size();
    for (int i = 0; converted && i < cameraPoses.size(); ++i)
        converted = samePoseBits(cameraPoses[i],adjusted.poses[i]);
    const double measuredClean = measuredBundleRms(adjusted,cleanScene.tracks,k);
    gate(4,bundleAccuracy(adjusted,cleanScene) && converted && measuredClean < 0.05
         && std::abs(measuredClean-adjusted.rms) < 1e-9
         && samePoseBits(adjusted.poses[0],cleanScene.initial[0]));

    const auto noisyScene = bundleScene(k,true);
    const auto noisy = sfm::bundleAdjust(noisyScene.tracks,noisyScene.initial,noisyScene.points,k);
    bool monotone = noisy.valid && noisy.rmsHistory.size() > 1 && noisy.rms < 1;
    for (int i = 1; i < noisy.rmsHistory.size(); ++i)
        monotone = monotone && noisy.rmsHistory[i] <= noisy.rmsHistory[i-1];
    const auto initial = sfm::bundleAdjust(noisyScene.tracks,noisyScene.initial,noisyScene.points,k,0);
    monotone = monotone && initial.valid && noisy.rms < initial.rms
        && std::abs(measuredBundleRms(noisy,noisyScene.tracks,k)-noisy.rms) < 1e-9;
    std::cerr << "noisy RMS: " << initial.rms << " -> " << noisy.rms << '\n';
    gate(5,monotone);

    const auto repeated = sfm::bundleAdjust(cleanScene.tracks,cleanScene.initial,cleanScene.points,k);
    auto oversized = cleanScene.tracks;
    oversized.frameCount = 31;
    const auto rejected = sfm::bundleAdjust(oversized,cleanScene.initial,cleanScene.points,k);
    oversized = cleanScene.tracks;
    oversized.tracks.resize(151);
    const auto tooManyPoints = sfm::bundleAdjust(oversized,cleanScene.initial,cleanScene.points,k);
    auto malformed = cleanScene.tracks;
    malformed.tracks[0].observations[0].first = -1;
    const auto badObservation = sfm::bundleAdjust(malformed,cleanScene.initial,cleanScene.points,k);
    const auto badIntrinsics = sfm::bundleAdjust(cleanScene.tracks,cleanScene.initial,cleanScene.points,{0,0,0});
    std::cerr << "bundle 20 x 100 (414 parameters): " << elapsedMs << " ms (Release budget: 3000 ms)\n";
    gate(6,sameBundleBits(adjusted,repeated) && elapsedMs <= 3000
         && !rejected.valid && !rejected.reason.isEmpty() && sfm::toCameraPoses(rejected).isEmpty()
         && !tooManyPoints.valid && !tooManyPoints.reason.isEmpty()
         && !badObservation.valid && !badIntrinsics.valid);
    std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    return failed;
}
