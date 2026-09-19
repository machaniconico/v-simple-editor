#include "../BundleAdjustment.h"

#include <algorithm>
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
    std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    return failed;
}
