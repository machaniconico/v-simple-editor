#include "../TwoViewGeometry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>

namespace {
constexpr double pi=3.14159265358979323846;
const camsolve::Intrinsics k{1000,960,540};
const double baseline=std::sqrt(0.3*0.3+0.05*0.05+0.1*0.1);
const double translation[3]={0.3,0.05,0.1};
void rotation(double* r)
{
    const double y=8*pi/180, p=3*pi/180;
    const double m[9]={std::cos(y),std::sin(y)*std::sin(p),std::sin(y)*std::cos(p),
        0,std::cos(p),-std::sin(p),-std::sin(y),std::cos(y)*std::sin(p),std::cos(y)*std::cos(p)};
    for (int i=0;i<9;++i) r[i]=m[i];
}
QPointF project(const QVector3D& p, const double* r, const double* t)
{
    double b[3];
    for (int i=0;i<3;++i) b[i]=r[3*i]*p.x()+r[3*i+1]*p.y()+r[3*i+2]*p.z()+t[i];
    return {k.f*b[0]/b[2]+k.cx,k.f*b[1]/b[2]+k.cy};
}
QVector<sfm::Correspondence> scene(int count, double noise, bool outliers,
    bool pureRotation, QVector<QVector3D>* truth=nullptr)
{
    std::mt19937 rng(48213);
    // Explicit conversion and Box-Muller avoid implementation-dependent
    // standard-library distributions and argument evaluation order on MSVC.
    const auto uniform=[&]() { return (double(rng())+0.5)/4294967296.0; };
    const auto normal=[&]() {
        const double radius=std::sqrt(-2*std::log(uniform()));
        return radius*std::cos(2*pi*uniform());
    };
    const double identity[9]={1,0,0,0,1,0,0,0,1}, zero[3]={};
    double r[9]; rotation(r);
    QVector<sfm::Correspondence> pairs;
    for (int i=0;i<count;++i) {
        // A cube, with all projected points inside a 1920x1080 image.
        const float x=float(2*uniform()-1), y=float(2*uniform()-1), z=float(4+2*uniform());
        const QVector3D point(x,y,z);
        if (truth) truth->append(point);
        auto a=project(point,identity,zero), b=project(point,r,pureRotation?zero:translation);
        const double ax=noise*normal(), ay=noise*normal(), bx=noise*normal(), by=noise*normal();
        a+=QPointF(ax,ay); b+=QPointF(bx,by);
        if (outliers && i%5==0) {
            const double ox=1920*uniform(), oy=1080*uniform();
            b=QPointF(ox,oy);
        }
        pairs.append({a,b});
    }
    return pairs;
}
bool accurate(const sfm::TwoViewResult& result, double maxR, double maxT)
{
    double r[9]; rotation(r); double trace=0, direction=0, norm=0;
    for (int i=0;i<9;++i) trace+=r[i]*result.R[i];
    for (int i=0;i<3;++i) { direction+=translation[i]*result.t[i]/baseline; norm+=result.t[i]*result.t[i]; }
    const double re=std::acos(qBound(-1.0,(trace-1)/2,1.0))*180/pi;
    const double te=std::acos(qBound(-1.0,direction,1.0))*180/pi;
    std::cerr << "R error=" << re << " deg, t error=" << te << " deg, inliers="
              << result.inlierCount << ", parallax=" << result.medianParallaxDeg
              << ", reprojection=" << result.meanReprojErr << " px\n";
    return result.valid && re<maxR && te<maxT && std::abs(norm-1)<1e-10;
}
bool identical(const sfm::TwoViewResult& a, const sfm::TwoViewResult& b)
{
    for (int i=0;i<9;++i) if (a.R[i]!=b.R[i] || a.E[i]!=b.E[i]) return false;
    for (int i=0;i<3;++i) if (a.t[i]!=b.t[i]) return false;
    return a.inliers==b.inliers && a.inlierCount==b.inlierCount && a.valid==b.valid
        && a.meanReprojErr==b.meanReprojErr && a.medianParallaxDeg==b.medianParallaxDeg;
}
} // namespace

int runSfmTwoViewSelftest()
{
    int passed=0,failed=0;
    const auto gate=[&](int n,bool ok) {
        std::cerr << (ok?"PASS G":"FAIL G") << n << '\n';
        if (ok) ++passed; else ++failed;
    };
    QVector<QVector3D> truth;
    const auto clean=scene(200,0,false,false,&truth);
    const auto exact=sfm::estimateRelativePose(clean,k);
    gate(1,accurate(exact,0.1,0.5) && exact.inlierCount==200 && exact.meanReprojErr<1e-5);
    const auto noisy=sfm::estimateRelativePose(scene(200,0.5,false,false),k);
    gate(2,accurate(noisy,0.5,3));
    const auto contaminated=scene(200,0.5,true,false);
    // A 2px consensus threshold accommodates noise in both measured images.
    const auto robust=sfm::estimateRelativePose(contaminated,k,2.0);
    gate(3,accurate(robust,0.5,3) && robust.inlierCount>=150);
    const auto points=sfm::triangulate(clean,k,exact.R,exact.t);
    QVector<double> errors; bool reprojectionOk=points.size()==truth.size();
    for (qsizetype i=0;i<points.size();++i) {
        errors.append(double((points[i]*float(baseline)-truth[i]).length())/truth[i].z());
        reprojectionOk=reprojectionOk && sfm::reprojectionError(clean[i],points[i],k,exact.R,exact.t)<0.001;
    }
    std::sort(errors.begin(),errors.end());
    const double middle=(errors[99]+errors[100])/2;
    std::cerr << "triangulation median relative error=" << middle << '\n';
    gate(4,reprojectionOk && middle<0.01);
    const auto pure=sfm::estimateRelativePose(scene(200,0,false,true),k);
    double smallR[9]; rotation(smallR);
    const double smallT[3]={translation[0]*0.01,translation[1]*0.01,translation[2]*0.01};
    auto lowParallax=clean;
    for (qsizetype i=0;i<truth.size();++i) lowParallax[i].b=project(truth[i],smallR,smallT);
    const auto shallow=sfm::estimateRelativePose(lowParallax,k);
    bool shortRejected=true;
    for (int n=0;n<8;++n) shortRejected=shortRejected && !sfm::estimateRelativePose(clean.mid(0,n),k).valid;
    auto invalid=clean;
    invalid[0].a.setX(std::numeric_limits<double>::quiet_NaN());
    const QVector<sfm::Correspondence> repeated(20,clean[0]);
    std::cerr << "pure rotation parallax=" << pure.medianParallaxDeg << '\n';
    gate(5,shortRejected && !pure.valid && pure.medianParallaxDeg<0.5
        && !shallow.valid && shallow.medianParallaxDeg<0.5
        && !sfm::estimateRelativePose(invalid,k).valid
        && !sfm::estimateRelativePose(repeated,k).valid
        && !sfm::estimateRelativePose(clean,{0,960,540}).valid
        && !sfm::estimateRelativePose(clean,k,0).valid
        && !sfm::estimateRelativePose(clean,k,1,0).valid);
    const auto hd=scene(500,0.5,true,false);
    const auto start=std::chrono::steady_clock::now();
    const auto timed=sfm::estimateRelativePose(hd,k,2,500,12345);
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    bool timeOk=true;
#ifdef NDEBUG
    timeOk=ms<=200;
#else
    if (ms>200) std::cerr << "WARN G6 Debug exceeds Release budget\n";
#endif
    std::cerr << "G6 1080p 500 points: " << ms << " ms\n";
    gate(6,timeOk && accurate(timed,0.5,3)
        && identical(timed,sfm::estimateRelativePose(hd,k,2,500,12345))
        && identical(robust,sfm::estimateRelativePose(contaminated,k,2)));
    std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    return failed;
}
