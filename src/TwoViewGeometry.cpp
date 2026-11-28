#include "TwoViewGeometry.h"

#include <cmath>

namespace sfm {
namespace {
constexpr double pi = 3.14159265358979323846;
double infinity() { return HUGE_VAL; }
bool intrinsicsOk(const camsolve::Intrinsics& k)
{
    return std::isfinite(k.f) && k.f > 0 && std::isfinite(k.cx) && std::isfinite(k.cy);
}
bool finite(const QPointF& p) { return std::isfinite(p.x()) && std::isfinite(p.y()); }
double dot(const double* a, const double* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
bool unit(double* a)
{
    const double n = std::sqrt(dot(a,a));
    if (!(n > 1e-15) || !std::isfinite(n)) return false;
    for (int i=0;i<3;++i) a[i] /= n;
    return true;
}
void cross(const double* a, const double* b, double* c)
{
    c[0]=a[1]*b[2]-a[2]*b[1]; c[1]=a[2]*b[0]-a[0]*b[2]; c[2]=a[0]*b[1]-a[1]*b[0];
}
void transpose(const double* a, double* b)
{
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) b[3*i+j]=a[3*j+i];
}
void multiply(const double* a, const double* b, double* c)
{
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) {
        c[3*i+j]=0;
        for (int k=0;k<3;++k) c[3*i+j]+=a[3*i+k]*b[3*k+j];
    }
}
double determinant(const double* a)
{
    return a[0]*(a[4]*a[8]-a[5]*a[7])-a[1]*(a[3]*a[8]-a[5]*a[6])+a[2]*(a[3]*a[7]-a[4]*a[6]);
}
// Cyclic symmetric Jacobi; eigenvectors are columns, sorted ascending.
template<int N> bool eigen(double (&a)[N*N], double (&v)[N*N])
{
    double scale=0;
    for (int i=0;i<N*N;++i) {
        if (!std::isfinite(a[i])) return false;
        scale=qMax(scale,std::abs(a[i])); v[i]=0;
    }
    if (!(scale > 0)) return false;
    for (int i=0;i<N*N;++i) a[i]/=scale;
    for (int i=0;i<N;++i) v[N*i+i]=1;
    bool converged=false;
    for (int sweep=0;sweep<64;++sweep) {
        double off=0;
        for (int p=0;p<N;++p) for (int q=p+1;q<N;++q) {
            const double apq=a[N*p+q]; off=qMax(off,std::abs(apq));
            if (std::abs(apq)<1e-16) continue;
            const double tau=(a[N*q+q]-a[N*p+p])/(2*apq);
            const double t=std::copysign(1.0,tau)/(std::abs(tau)+std::hypot(1.0,tau));
            const double c=1/std::sqrt(1+t*t), s=t*c;
            a[N*p+p]-=t*apq; a[N*q+q]+=t*apq;
            a[N*p+q]=a[N*q+p]=0;
            for (int k=0;k<N;++k) {
                if (k!=p && k!=q) {
                    const double x=a[N*k+p], y=a[N*k+q];
                    a[N*k+p]=a[N*p+k]=c*x-s*y;
                    a[N*k+q]=a[N*q+k]=s*x+c*y;
                }
                const double x=v[N*k+p], y=v[N*k+q];
                v[N*k+p]=c*x-s*y; v[N*k+q]=s*x+c*y;
            }
        }
        if (off<1e-14) { converged=true; break; }
    }
    if (!converged) return false;
    for (int i=0;i<N;++i) {
        int best=i;
        for (int j=i+1;j<N;++j) if (a[N*j+j]<a[N*best+best]) best=j;
        if (best!=i) {
            qSwap(a[N*i+i],a[N*best+best]);
            for (int k=0;k<N;++k) qSwap(v[N*k+i],v[N*k+best]);
        }
    }
    for (int i=0;i<N;++i) a[N*i+i]*=scale;
    return true;
}
// Rank-two-safe SVD. Only the first two singular values are needed for E.
// Complete both bases by a cross product, so U and V are right handed even
// when the third singular value is zero (no division by sigma_3).
bool svd(const double* a, double* u, double* s, double* v)
{
    double ata[9]={}, ev[9];
    for (int i=0;i<3;++i) for (int j=0;j<3;++j)
        for (int k=0;k<3;++k) ata[3*i+j]+=a[3*k+i]*a[3*k+j];
    if (!eigen<3>(ata,ev)) return false;
    double vc[3][3]={}, uc[3][3]={};
    for (int j=0;j<2;++j) {
        s[j]=std::sqrt(qMax(0.0,ata[3*(2-j)+(2-j)]));
        if (!(s[j]>1e-12*qMax(s[0],1e-30))) return false;
        for (int i=0;i<3;++i) vc[j][i]=ev[3*i+2-j];
        for (int i=0;i<3;++i) for (int k=0;k<3;++k) uc[j][i]+=a[3*i+k]*vc[j][k]/s[j];
        if (j==1) {
            const double d=dot(uc[0],uc[1]);
            for (int i=0;i<3;++i) uc[1][i]-=d*uc[0][i];
        }
        if (!unit(uc[j])) return false;
    }
    cross(vc[0],vc[1],vc[2]); cross(uc[0],uc[1],uc[2]);
    if (!unit(vc[2]) || !unit(uc[2])) return false;
    s[2]=std::sqrt(qMax(0.0,ata[0]));
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) { u[3*i+j]=uc[j][i]; v[3*i+j]=vc[j][i]; }
    return true;
}
bool projectEssential(const double* raw, double* e)
{
    double u[9],s[3],v[9];
    if (!svd(raw,u,s,v)) return false;
    // Unit Frobenius norm and equal nonzero singular values.
    for (int i=0;i<3;++i) for (int j=0;j<3;++j)
        e[3*i+j]=(u[3*i]*v[3*j]+u[3*i+1]*v[3*j+1])/std::sqrt(2.0);
    return true;
}
double signedResidual(const Correspondence& p, const double* e)
{
    const double x=p.a.x(),y=p.a.y(),u=p.b.x(),v=p.b.y();
    const double ex=e[0]*x+e[1]*y+e[2], ey=e[3]*x+e[4]*y+e[5];
    const double tx=e[0]*u+e[3]*v+e[6], ty=e[1]*u+e[4]*v+e[7];
    const double d=ex*ex+ey*ey+tx*tx+ty*ty;
    return d>1e-30 ? (u*ex+v*ey+e[6]*x+e[7]*y+e[8])/std::sqrt(d) : 1e10;
}
// Algebraic eight-point fitting is biased under image noise. Polish its
// essential projection by damped least squares on the Sampson residuals.
// Every finite-difference and accepted step remains on the essential manifold.
void polish(const QVector<Correspondence>& points, const QVector<int>& indices, double* e)
{
    double damping=1e-5;
    for (int iteration=0;iteration<15;++iteration) {
        double perturbed[9][9]; bool ok=true;
        constexpr double step=1e-6;
        for (int j=0;j<9;++j) {
            double raw[9]; for (int h=0;h<9;++h) raw[h]=e[h];
            raw[j]+=step; ok=projectEssential(raw,perturbed[j]) && ok;
        }
        if (!ok) return;
        double hessian[81]={},gradient[9]={},cost=0;
        for (int i:indices) {
            const double residual=signedResidual(points[i],e); cost+=residual*residual;
            double jac[9];
            for (int j=0;j<9;++j) jac[j]=(signedResidual(points[i],perturbed[j])-residual)/step;
            for (int j=0;j<9;++j) {
                gradient[j]+=jac[j]*residual;
                for (int h=0;h<9;++h) hessian[9*j+h]+=jac[j]*jac[h];
            }
        }
        if (cost<1e-24) return;
        for (int j=0;j<9;++j) hessian[9*j+j]+=damping;
        double vectors[81]; if (!eigen<9>(hessian,vectors)) return;
        double raw[9]; for (int j=0;j<9;++j) raw[j]=e[j];
        for (int j=0;j<9;++j) {
            if (!(hessian[9*j+j]>0)) return;
            double coefficient=0;
            for (int h=0;h<9;++h) coefficient+=vectors[9*h+j]*gradient[h];
            coefficient/=hessian[9*j+j];
            for (int h=0;h<9;++h) raw[h]-=vectors[9*h+j]*coefficient;
        }
        double candidate[9]; if (!projectEssential(raw,candidate)) return;
        double nextCost=0;
        for (int i:indices) { const double d=signedResidual(points[i],candidate); nextCost+=d*d; }
        if (nextCost<cost) {
            for (int j=0;j<9;++j) e[j]=candidate[j];
            if (cost-nextCost<1e-14*cost) return;
            damping=qMax(1e-10,damping*0.3);
        } else damping*=10;
    }
}
bool fit(const QVector<Correspondence>& points, const QVector<int>& indices, double* e)
{
    if (indices.size()<8) return false;
    QPointF ca,cb;
    for (int i:indices) { ca+=points[i].a; cb+=points[i].b; }
    ca/=double(indices.size()); cb/=double(indices.size());
    double da=0,db=0;
    for (int i:indices) {
        const auto a=points[i].a-ca, b=points[i].b-cb;
        da+=std::hypot(a.x(),a.y()); db+=std::hypot(b.x(),b.y());
    }
    if (!(da>1e-12) || !(db>1e-12)) return false;
    const double sa=std::sqrt(2.0)*indices.size()/da, sb=std::sqrt(2.0)*indices.size()/db;
    double ata[81]={}, v[81];
    for (int i:indices) {
        const auto a=(points[i].a-ca)*sa, b=(points[i].b-cb)*sb;
        const double row[9]={b.x()*a.x(),b.x()*a.y(),b.x(),b.y()*a.x(),b.y()*a.y(),b.y(),a.x(),a.y(),1};
        for (int j=0;j<9;++j) for (int k=0;k<9;++k) ata[9*j+k]+=row[j]*row[k];
    }
    if (!eigen<9>(ata,v)) return false;
    // Reject collinear/repeated samples, but permit pure-rotation nullspaces;
    // their actual degeneracy is decided by the recovered ray parallax.
    if (!(ata[4*9+4]>1e-12*ata[80])) return false;
    double h[9], temp[9], raw[9];
    for (int i=0;i<9;++i) h[i]=v[9*i];
    const double ta[9]={sa,0,-sa*ca.x(),0,sa,-sa*ca.y(),0,0,1};
    const double tbT[9]={sb,0,0,0,sb,0,-sb*cb.x(),-sb*cb.y(),1};
    multiply(tbT,h,temp); multiply(temp,ta,raw);
    if (!projectEssential(raw,e)) return false;
    if (indices.size()>8) polish(points,indices,e);
    return true;
}
double sampson(const Correspondence& p, const double* e)
{
    const double x=p.a.x(), y=p.a.y(), u=p.b.x(), v=p.b.y();
    const double ex=e[0]*x+e[1]*y+e[2], ey=e[3]*x+e[4]*y+e[5];
    const double tx=e[0]*u+e[3]*v+e[6], ty=e[1]*u+e[4]*v+e[7];
    const double r=u*ex+v*ey+e[6]*x+e[7]*y+e[8], d=ex*ex+ey*ey+tx*tx+ty*ty;
    return d>1e-30 ? r*r/d : infinity();
}
bool pointDLT(const Correspondence& p, const double* r, const double* t, double* x)
{
    const double a[16]={-1,0,p.a.x(),0, 0,-1,p.a.y(),0,
        p.b.x()*r[6]-r[0],p.b.x()*r[7]-r[1],p.b.x()*r[8]-r[2],p.b.x()*t[2]-t[0],
        p.b.y()*r[6]-r[3],p.b.y()*r[7]-r[4],p.b.y()*r[8]-r[5],p.b.y()*t[2]-t[1]};
    double ata[16]={},v[16];
    for (int i=0;i<4;++i) for (int j=0;j<4;++j) for (int k=0;k<4;++k) ata[4*i+j]+=a[4*k+i]*a[4*k+j];
    if (!eigen<4>(ata,v) || std::abs(v[12])<1e-12) return false;
    for (int i=0;i<3;++i) { x[i]=v[4*i]/v[12]; if (!std::isfinite(x[i])) return false; }
    return true;
}
Correspondence normalize(const Correspondence& p, const camsolve::Intrinsics& k)
{
    return {(p.a-QPointF(k.cx,k.cy))/k.f,(p.b-QPointF(k.cx,k.cy))/k.f};
}
double error(const Correspondence& p, const double* x, const camsolve::Intrinsics& k, const double* r, const double* t)
{
    double b[3];
    for (int i=0;i<3;++i) b[i]=dot(r+3*i,x)+t[i];
    if (!std::isfinite(x[2]) || !std::isfinite(b[2]) || std::abs(x[2])<1e-12 || std::abs(b[2])<1e-12) return infinity();
    const double result=0.5*(std::hypot(k.f*x[0]/x[2]+k.cx-p.a.x(),k.f*x[1]/x[2]+k.cy-p.a.y())
        +std::hypot(k.f*b[0]/b[2]+k.cx-p.b.x(),k.f*b[1]/b[2]+k.cy-p.b.y()));
    return std::isfinite(result) ? result : infinity();
}
double median(QVector<double> values)
{
    // Insertion sort keeps this small utility independent of STL algorithms.
    for (qsizetype i=1;i<values.size();++i) {
        const double value=values[i]; qsizetype j=i;
        while (j>0 && values[j-1]>value) { values[j]=values[j-1]; --j; }
        values[j]=value;
    }
    if (values.isEmpty()) return 0;
    const qsizetype n=values.size();
    return n%2 ? values[n/2] : (values[n/2-1]+values[n/2])/2;
}
} // namespace

QVector<QVector3D> triangulate(const QVector<Correspondence>& px, const camsolve::Intrinsics& k, const double r[9], const double t[3])
{
    QVector<QVector3D> out; out.reserve(px.size());
    for (const auto& p:px) {
        double x[3];
        if (intrinsicsOk(k) && finite(p.a) && finite(p.b) && pointDLT(normalize(p,k),r,t,x))
            out.append(QVector3D(float(x[0]),float(x[1]),float(x[2])));
        else out.append(QVector3D(float(NAN),float(NAN),float(NAN)));
    }
    return out;
}
double reprojectionError(const Correspondence& p, const QVector3D& point, const camsolve::Intrinsics& k, const double r[9], const double t[3])
{
    if (!intrinsicsOk(k) || !finite(p.a) || !finite(p.b)) return infinity();
    const double x[3]={point.x(),point.y(),point.z()};
    return error(p,x,k,r,t);
}
TwoViewResult estimateRelativePose(const QVector<Correspondence>& px, const camsolve::Intrinsics& k, double thresholdPx, int iterations, unsigned seed)
{
    TwoViewResult out; out.inliers.fill(false,px.size());
    if (px.size()<8 || !intrinsicsOk(k) || !(thresholdPx>0) || !std::isfinite(thresholdPx) || iterations<=0) return out;
    QVector<Correspondence> points; points.reserve(px.size());
    for (const auto& p:px) {
        if (!finite(p.a) || !finite(p.b)) return out;
        const auto n=normalize(p,k);
        if (!finite(n.a) || !finite(n.b)) return out;
        points.append(n);
    }
    // Pure rotation has a three-dimensional epipolar nullspace: selecting an
    // arbitrary null vector can manufacture a large-parallax, wrong rotation.
    // Resolve the exact rotation-only case directly from the bearing vectors.
    double covariance[9]={};
    for (const auto& p:points) {
        double a[3]={p.a.x(),p.a.y(),1}, b[3]={p.b.x(),p.b.y(),1};
        unit(a); unit(b);
        for (int i=0;i<3;++i) for (int j=0;j<3;++j) covariance[3*i+j]+=b[i]*a[j];
    }
    double ru[9],rs[3],rv[9],rvt[9],rotationOnly[9];
    if (svd(covariance,ru,rs,rv)) {
        transpose(rv,rvt); multiply(ru,rvt,rotationOnly);
        QVector<double> angles; double maximum=0;
        for (const auto& p:points) {
            double a[3]={p.a.x(),p.a.y(),1}, b[3]={p.b.x(),p.b.y(),1}, ray[3]={};
            for (int i=0;i<3;++i) for (int j=0;j<3;++j) ray[i]+=rotationOnly[3*j+i]*b[j];
            unit(a); unit(ray);
            const double angle=std::acos(qBound(-1.0,dot(a,ray),1.0))*180/pi;
            angles.append(angle); maximum=qMax(maximum,angle);
        }
        // Require numerical agreement for every ray, so a translating scene
        // with shallow depth variation is not mistaken for pure rotation.
        if (maximum<1e-5) {
            for (int i=0;i<9;++i) out.R[i]=rotationOnly[i];
            out.medianParallaxDeg=median(angles);
            return out;
        }
    }
    const double threshold=(thresholdPx/k.f)*(thresholdPx/k.f);
    QVector<int> best;
    double bestCost=infinity();
    // Explicit 32-bit LCG gives identical sampling on MSVC and other platforms.
    quint32 state=seed;
    for (int it=0;it<iterations;++it) {
        QVector<int> sample; sample.reserve(8);
        while (sample.size()<8) {
            state=1664525u*state+1013904223u;
            const int index=int((quint64(state)*quint64(points.size()))>>32);
            if (!sample.contains(index)) sample.append(index);
        }
        double e[9]; if (!fit(points,sample,e)) continue;
        QVector<int> inliers; double cost=0;
        for (qsizetype i=0;i<points.size();++i) {
            const double d=sampson(points[i],e);
            if (d<=threshold) inliers.append(int(i));
            cost+=qMin(d,threshold);
        }
        // Local consensus refinement reduces the noise sensitivity of an
        // eight-point minimal sample. Keep only improvements; a bad refit must
        // never destroy a better hypothesis.
        if (inliers.size()>=qMax(20,int(best.size()/4))) for (int pass=0;pass<3;++pass) {
            double refined[9];
            if (!fit(points,inliers,refined)) break;
            QVector<int> next; double nextCost=0;
            for (qsizetype i=0;i<points.size();++i) {
                const double d=sampson(points[i],refined);
                if (d<=threshold) next.append(int(i));
                nextCost+=qMin(d,threshold);
            }
            if (next.size()<inliers.size() || (next.size()==inliers.size() && nextCost>=cost)) break;
            inliers=next; cost=nextCost;
        }
        if (inliers.size()>best.size() || (inliers.size()==best.size() && cost<bestCost)) {
            best=inliers; bestCost=cost;
        }
    }
    if (best.size()<8) return out;
    // Refit and reclassify consensus before the final all-inlier fit.
    for (int pass=0;pass<3;++pass) {
        if (!fit(points,best,out.E)) return out;
        QVector<int> next;
        for (qsizetype i=0;i<points.size();++i) if (sampson(points[i],out.E)<=threshold) next.append(int(i));
        if (next.size()<8) return out;
        if (next==best) break;
        best=next;
    }
    if (!fit(points,best,out.E)) return out;
    // Report membership against the actual returned E.
    best.clear();
    for (qsizetype i=0;i<points.size();++i) if (sampson(points[i],out.E)<=threshold) best.append(int(i));
    if (best.size()<8) return out;
    out.inlierCount=int(best.size());
    for (int i:best) out.inliers[i]=true;
    double u[9],s[3],v[9],vt[9];
    if (!svd(out.E,u,s,v)) return out;
    transpose(v,vt);
    const double w[9]={0,-1,0,1,0,0,0,0,1}; double wt[9]; transpose(w,wt);
    int mostPositive=-1;
    for (int rotation=0;rotation<2;++rotation) {
        double temp[9],r[9]; multiply(u,rotation==0?w:wt,temp); multiply(temp,vt,r);
        if (determinant(r)<0) for (double& value:r) value=-value;
        for (int sign: {-1,1}) {
            const double t[3]={sign*u[2],sign*u[5],sign*u[8]}; int positive=0;
            for (int i:best) {
                double x[3];
                if (pointDLT(points[i],r,t,x) && x[2]>0 && dot(r+6,x)+t[2]>0) ++positive;
            }
            if (positive>mostPositive) {
                mostPositive=positive;
                for (int i=0;i<9;++i) out.R[i]=r[i];
                for (int i=0;i<3;++i) out.t[i]=t[i];
            }
        }
    }
    QVector<double> angles; double sum=0; int reconstructed=0;
    for (int i:best) {
        double a[3]={points[i].a.x(),points[i].a.y(),1}, b[3]={points[i].b.x(),points[i].b.y(),1}, ray[3]={};
        for (int j=0;j<3;++j) for (int h=0;h<3;++h) ray[j]+=out.R[3*h+j]*b[h];
        if (unit(a) && unit(ray)) angles.append(std::acos(qBound(-1.0,dot(a,ray),1.0))*180/pi);
        double x[3];
        if (pointDLT(points[i],out.R,out.t,x)) { sum+=error(px[i],x,k,out.R,out.t); ++reconstructed; }
    }
    out.medianParallaxDeg=median(angles);
    out.meanReprojErr=reconstructed ? sum/reconstructed : infinity();
    out.valid=out.medianParallaxDeg>=0.5 && mostPositive>0.5*best.size()
        && reconstructed>=8 && std::isfinite(out.meanReprojErr);
    return out;
}
} // namespace sfm
