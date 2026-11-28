#include "PuppetEngine.h"

#include "Expression.h"
#include "Timeline.h"
#include "WiggleTransform.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>

namespace puppet {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kEps = 1e-6;

inline bool approxEq(double a, double b, double eps = 1e-6)
{
    return std::abs(a - b) <= eps;
}

inline bool approxPt(const QPointF &a, const QPointF &b, double eps = 1e-6)
{
    return approxEq(a.x(), b.x(), eps) && approxEq(a.y(), b.y(), eps);
}

double applyEasing(EasingKind kind, double u)
{
    u = std::clamp(u, 0.0, 1.0);
    switch (kind) {
    case EasingKind::Linear:    return u;
    case EasingKind::EaseInOut: return u * u * (3.0 - 2.0 * u);
    case EasingKind::EaseIn:    return u * u * u;
    case EasingKind::EaseOut: {
        const double inv = 1.0 - u;
        return 1.0 - inv * inv * inv;
    }
    case EasingKind::Hold:      return 0.0;
    }
    return u;
}

// Lightweight CSR sparse symmetric matrix + Jacobi-preconditioned Conjugate Gradient solver.
class SparseBuilder {
public:
    explicit SparseBuilder(int dim) : m_dim(dim), m_rows(dim) {}

    void add(int r, int c, double val)
    {
        if (r < 0 || r >= m_dim || c < 0 || c >= m_dim || std::abs(val) < 1e-18)
            return;
        m_rows[r][c] += val;
    }

    // Add weight * (sum_m coeff[m] * x[idx[m]])^2 to Hessian (symmetric).
    void addQuadRow(const int *idx, const double *coeff, int count, double weight)
    {
        for (int a = 0; a < count; ++a) {
            const double wa = weight * coeff[a];
            if (std::abs(wa) < 1e-18) continue;
            auto &row = m_rows[idx[a]];
            for (int b = 0; b < count; ++b) {
                const double v = wa * coeff[b];
                if (std::abs(v) >= 1e-18)
                    row[idx[b]] += v;
            }
        }
    }

    struct Csr {
        int dim = 0;
        QVector<int> rowPtr;
        QVector<int> colIdx;
        QVector<double> values;
        QVector<double> invDiag;
    };

    Csr build() const
    {
        Csr csr;
        csr.dim = m_dim;
        csr.rowPtr.resize(m_dim + 1);
        csr.invDiag.resize(m_dim);
        int nnz = 0;
        for (int r = 0; r < m_dim; ++r)
            nnz += static_cast<int>(m_rows[r].size());
        csr.colIdx.reserve(nnz);
        csr.values.reserve(nnz);
        int cursor = 0;
        for (int r = 0; r < m_dim; ++r) {
            csr.rowPtr[r] = cursor;
            double diag = 0.0;
            for (const auto &kv : m_rows[r]) {
                csr.colIdx.append(kv.first);
                csr.values.append(kv.second);
                if (kv.first == r) diag = kv.second;
                ++cursor;
            }
            csr.invDiag[r] = (std::abs(diag) > 1e-14) ? (1.0 / diag) : 1.0;
        }
        csr.rowPtr[m_dim] = cursor;
        return csr;
    }

private:
    int m_dim = 0;
    std::vector<std::map<int, double>> m_rows;
};

void csrMul(const SparseBuilder::Csr &A, const QVector<double> &x, QVector<double> &out)
{
    const int n = A.dim;
    out.resize(n);
    const int *rp = A.rowPtr.constData();
    const int *ci = A.colIdx.constData();
    const double *val = A.values.constData();
    const double *xd = x.constData();
    double *od = out.data();
    for (int r = 0; r < n; ++r) {
        double sum = 0.0;
        for (int k = rp[r]; k < rp[r + 1]; ++k)
            sum += val[k] * xd[ci[k]];
        od[r] = sum;
    }
}

void solvePcg(const SparseBuilder::Csr &A,
              const QVector<double> &b,
              QVector<double> &x,
              int maxIter = 450,
              double relTol = 1e-6)
{
    const int n = A.dim;
    if (n <= 0) return;
    if (x.size() != n) x.fill(0.0, n);

    QVector<double> r(n), z(n), p(n), Ap(n);
    csrMul(A, x, Ap);

    double bNorm2 = 0.0;
    for (int i = 0; i < n; ++i) {
        r[i] = b[i] - Ap[i];
        z[i] = A.invDiag[i] * r[i];
        p[i] = z[i];
        bNorm2 += b[i] * b[i];
    }
    const double tol2 = std::max(1e-16, relTol * relTol * std::max(1.0, bNorm2));

    double rzOld = 0.0;
    for (int i = 0; i < n; ++i) rzOld += r[i] * z[i];
    if (rzOld <= 1e-24) return;

    for (int iter = 0; iter < maxIter; ++iter) {
        csrMul(A, p, Ap);
        double pAp = 0.0;
        for (int i = 0; i < n; ++i) pAp += p[i] * Ap[i];
        if (pAp <= 1e-24) break;
        const double alpha = rzOld / pAp;

        double rNorm2 = 0.0;
        for (int i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * Ap[i];
            rNorm2 += r[i] * r[i];
        }
        if (rNorm2 <= tol2) break;

        double rzNew = 0.0;
        for (int i = 0; i < n; ++i) {
            z[i] = A.invDiag[i] * r[i];
            rzNew += r[i] * z[i];
        }
        if (rzNew <= 1e-24) break;
        const double beta = rzNew / rzOld;
        for (int i = 0; i < n; ++i)
            p[i] = z[i] + beta * p[i];
        rzOld = rzNew;
    }
}

struct BaryHit {
    int triIndex = -1;
    std::array<double, 3> bary = {1.0, 0.0, 0.0};
    double dist2 = std::numeric_limits<double>::max();
};

bool computeBarycentric(const QPointF &p,
                        const QPointF &a,
                        const QPointF &b,
                        const QPointF &c,
                        std::array<double, 3> &out)
{
    const double v0x = b.x() - a.x();
    const double v0y = b.y() - a.y();
    const double v1x = c.x() - a.x();
    const double v1y = c.y() - a.y();
    const double v2x = p.x() - a.x();
    const double v2y = p.y() - a.y();
    const double denom = v0x * v1y - v1x * v0y;
    if (std::abs(denom) < 1e-12) return false;
    const double inv = 1.0 / denom;
    const double v = (v2x * v1y - v1x * v2y) * inv;
    const double w = (v0x * v2y - v2x * v0y) * inv;
    const double u = 1.0 - v - w;
    out = {u, v, w};
    return true;
}

BaryHit bindPointToMesh(const PuppetMesh &mesh, const QPointF &pt)
{
    BaryHit best;
    for (int ti = 0; ti < mesh.triangles.size(); ++ti) {
        const auto &tri = mesh.triangles[ti];
        const QPointF &a = mesh.restVertices[tri[0]];
        const QPointF &b = mesh.restVertices[tri[1]];
        const QPointF &c = mesh.restVertices[tri[2]];
        std::array<double, 3> bary{};
        if (!computeBarycentric(pt, a, b, c, bary)) continue;
        if (bary[0] >= -1e-5 && bary[1] >= -1e-5 && bary[2] >= -1e-5) {
            best.triIndex = ti;
            best.bary = bary;
            best.dist2 = 0.0;
            return best;
        }
        // Fallback: clamp barycentric coordinates to triangle and measure distance.
        double u = std::clamp(bary[0], 0.0, 1.0);
        double v = std::clamp(bary[1], 0.0, 1.0);
        double w = std::clamp(bary[2], 0.0, 1.0);
        const double sum = u + v + w;
        if (sum > 1e-12) {
            u /= sum;
            v /= sum;
            w /= sum;
        } else {
            u = 1.0 / 3.0;
            v = 1.0 / 3.0;
            w = 1.0 / 3.0;
        }
        const QPointF clamped = a * u + b * v + c * w;
        const double dx = pt.x() - clamped.x();
        const double dy = pt.y() - clamped.y();
        const double d2 = dx * dx + dy * dy;
        if (d2 < best.dist2) {
            best.triIndex = ti;
            best.bary = {u, v, w};
            best.dist2 = d2;
        }
    }
    return best;
}

// Compute fast alpha fingerprint for mesh caching (samples ~64x64 grid).
quint64 alphaFingerprint(const QImage &src, int alphaThreshold)
{
    const int w = src.width();
    const int h = src.height();
    quint64 hash = 1469598103934665603ULL;
    auto mix = [&](quint64 v) {
        hash ^= v;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<quint64>(w));
    mix(static_cast<quint64>(h));
    mix(static_cast<quint64>(alphaThreshold));
    if (!src.hasAlphaChannel()) {
        mix(0xFF00FF00ULL);
        return hash;
    }
    const int stepX = std::max(1, w / 48);
    const int stepY = std::max(1, h / 48);
    for (int y = 0; y < h; y += stepY) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        quint64 bits = 0;
        int bit = 0;
        for (int x = 0; x < w; x += stepX) {
            if (qAlpha(line[x]) >= alphaThreshold)
                bits |= (1ULL << (bit & 63));
            ++bit;
            if ((bit & 63) == 0) {
                mix(bits);
                bits = 0;
            }
        }
        mix(bits);
    }
    return hash;
}

struct MeshCacheKey {
    int width = 0;
    int height = 0;
    int density = 0;
    int expansionPx = 0;
    int alphaThreshold = 0;
    quint64 alphaHash = 0;
    bool operator==(const MeshCacheKey &o) const
    {
        return width == o.width && height == o.height && density == o.density
            && expansionPx == o.expansionPx && alphaThreshold == o.alphaThreshold
            && alphaHash == o.alphaHash;
    }
};

struct MeshCacheEntry {
    MeshCacheKey key;
    PuppetMesh mesh;
};

QMutex g_meshCacheMutex;
QVector<MeshCacheEntry> g_meshCache;
constexpr int kMaxMeshCacheEntries = 8;

// Global RigStore + Timeline Undo snapshots.
QMutex g_storeMutex;
QHash<QString, PuppetRig> g_rigStore;
QHash<const void *, QHash<int, QHash<QString, PuppetRig>>> g_undoSnapshots;

// Premultiplied-alpha bilinear sampler on Format_ARGB32_Premultiplied.
inline QRgb samplePremulBilinear(const QImage &src, double sx, double sy)
{
    const int w = src.width();
    const int h = src.height();
    if (sx < -0.5 || sy < -0.5 || sx > static_cast<double>(w) - 0.5
        || sy > static_cast<double>(h) - 0.5)
        return 0;

    // Sample pixel-center coordinates: pixel i has center at i + 0.5.
    const double fx = std::clamp(sx - 0.5, 0.0, static_cast<double>(w - 1));
    const double fy = std::clamp(sy - 0.5, 0.0, static_cast<double>(h - 1));
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const double tx = fx - x0;
    const double ty = fy - y0;

    const QRgb *row0 = reinterpret_cast<const QRgb *>(src.constScanLine(y0));
    const QRgb *row1 = reinterpret_cast<const QRgb *>(src.constScanLine(y1));
    const QRgb c00 = row0[x0];
    const QRgb c10 = row0[x1];
    const QRgb c01 = row1[x0];
    const QRgb c11 = row1[x1];

    const double w00 = (1.0 - tx) * (1.0 - ty);
    const double w10 = tx * (1.0 - ty);
    const double w01 = (1.0 - tx) * ty;
    const double w11 = tx * ty;

    const int a = std::clamp(static_cast<int>(std::lround(
        w00 * qAlpha(c00) + w10 * qAlpha(c10) + w01 * qAlpha(c01) + w11 * qAlpha(c11))), 0, 255);
    if (a == 0) return 0;
    const int r = std::clamp(static_cast<int>(std::lround(
        w00 * qRed(c00) + w10 * qRed(c10) + w01 * qRed(c01) + w11 * qRed(c11))), 0, a);
    const int g = std::clamp(static_cast<int>(std::lround(
        w00 * qGreen(c00) + w10 * qGreen(c10) + w01 * qGreen(c01) + w11 * qGreen(c11))), 0, a);
    const int b = std::clamp(static_cast<int>(std::lround(
        w00 * qBlue(c00) + w10 * qBlue(c10) + w01 * qBlue(c01) + w11 * qBlue(c11))), 0, a);
    return qRgba(r, g, b, a);
}

// Premultiplied SrcOver composite: src over dst.
inline QRgb premulSrcOver(QRgb src, QRgb dst)
{
    const int sa = qAlpha(src);
    if (sa == 0) return dst;
    if (sa == 255 || dst == 0) return src;
    const int invSa = 255 - sa;
    const int outA = std::min(255, sa + (qAlpha(dst) * invSa + 127) / 255);
    const int outR = std::min(outA, qRed(src) + (qRed(dst) * invSa + 127) / 255);
    const int outG = std::min(outA, qGreen(src) + (qGreen(dst) * invSa + 127) / 255);
    const int outB = std::min(outA, qBlue(src) + (qBlue(dst) * invSa + 127) / 255);
    return qRgba(outR, outG, outB, outA);
}

} // namespace

// ============================================================
//  Enum helpers & Serialization
// ============================================================

QString pinTypeName(PinType type)
{
    switch (type) {
    case PinType::Position: return QStringLiteral("position");
    case PinType::Bend:     return QStringLiteral("bend");
    case PinType::Advanced: return QStringLiteral("advanced");
    case PinType::Starch:   return QStringLiteral("starch");
    case PinType::Overlap:  return QStringLiteral("overlap");
    }
    return QStringLiteral("position");
}

QString pinTypeDisplayName(PinType type)
{
    switch (type) {
    case PinType::Position: return QStringLiteral("位置 (Position)");
    case PinType::Bend:     return QStringLiteral("ベンド (Bend: 回転/拡縮)");
    case PinType::Advanced: return QStringLiteral("アドバンスド (位置+回転+拡縮)");
    case PinType::Starch:   return QStringLiteral("スターチ (硬直/骨格)");
    case PinType::Overlap:  return QStringLiteral("オーバーラップ (前後重なり)");
    }
    return QStringLiteral("位置 (Position)");
}

PinType pinTypeFromString(const QString &s)
{
    if (s == QLatin1String("bend"))     return PinType::Bend;
    if (s == QLatin1String("advanced")) return PinType::Advanced;
    if (s == QLatin1String("starch"))   return PinType::Starch;
    if (s == QLatin1String("overlap"))  return PinType::Overlap;
    return PinType::Position;
}

QString easingName(EasingKind kind)
{
    switch (kind) {
    case EasingKind::Linear:    return QStringLiteral("linear");
    case EasingKind::EaseInOut: return QStringLiteral("ease_in_out");
    case EasingKind::EaseIn:    return QStringLiteral("ease_in");
    case EasingKind::EaseOut:   return QStringLiteral("ease_out");
    case EasingKind::Hold:      return QStringLiteral("hold");
    }
    return QStringLiteral("ease_in_out");
}

EasingKind easingFromString(const QString &s)
{
    if (s == QLatin1String("linear"))   return EasingKind::Linear;
    if (s == QLatin1String("ease_in"))  return EasingKind::EaseIn;
    if (s == QLatin1String("ease_out")) return EasingKind::EaseOut;
    if (s == QLatin1String("hold"))     return EasingKind::Hold;
    return EasingKind::EaseInOut;
}

bool PinKeyframe::operator==(const PinKeyframe &o) const
{
    return approxEq(timeSec, o.timeSec, 1e-4)
        && approxPt(position, o.position, 1e-5)
        && approxEq(rotationDeg, o.rotationDeg, 1e-4)
        && approxEq(scale, o.scale, 1e-4)
        && approxEq(inFront, o.inFront, 1e-4)
        && easing == o.easing;
}

QJsonObject PinKeyframe::toJson() const
{
    QJsonObject o;
    o["t"]       = timeSec;
    o["x"]       = position.x();
    o["y"]       = position.y();
    o["rot"]     = rotationDeg;
    o["scale"]   = scale;
    o["inFront"] = inFront;
    o["easing"]  = easingName(easing);
    return o;
}

PinKeyframe PinKeyframe::fromJson(const QJsonObject &obj)
{
    PinKeyframe k;
    k.timeSec     = std::clamp(obj["t"].toDouble(0.0), 0.0, 86400.0);
    k.position    = QPointF(std::clamp(obj["x"].toDouble(0.5), -1.0, 2.0),
                            std::clamp(obj["y"].toDouble(0.5), -1.0, 2.0));
    k.rotationDeg = std::clamp(obj["rot"].toDouble(0.0), -3600.0, 3600.0);
    k.scale       = std::clamp(obj["scale"].toDouble(1.0), 0.1, 4.0);
    k.inFront     = std::clamp(obj["inFront"].toDouble(0.0), -100.0, 100.0);
    k.easing      = easingFromString(obj["easing"].toString());
    return k;
}

bool PinDynamics::isDefault() const
{
    return !wiggleEnabled
        && exprX.trimmed().isEmpty()
        && exprY.trimmed().isEmpty()
        && exprRot.trimmed().isEmpty()
        && exprScale.trimmed().isEmpty();
}

bool PinDynamics::operator==(const PinDynamics &o) const
{
    return wiggleEnabled == o.wiggleEnabled
        && approxEq(wiggleFreqHz, o.wiggleFreqHz, 1e-4)
        && approxEq(wiggleAmpUv, o.wiggleAmpUv, 1e-5)
        && approxEq(wiggleRotAmpDeg, o.wiggleRotAmpDeg, 1e-4)
        && wiggleOctaves == o.wiggleOctaves
        && wiggleSeed == o.wiggleSeed
        && exprX == o.exprX
        && exprY == o.exprY
        && exprRot == o.exprRot
        && exprScale == o.exprScale;
}

QJsonObject PinDynamics::toJson() const
{
    QJsonObject o;
    if (wiggleEnabled) {
        o["wiggleEnabled"]   = true;
        o["wiggleFreqHz"]    = wiggleFreqHz;
        o["wiggleAmpUv"]     = wiggleAmpUv;
        o["wiggleRotAmpDeg"] = wiggleRotAmpDeg;
        o["wiggleOctaves"]   = wiggleOctaves;
        o["wiggleSeed"]      = static_cast<int>(wiggleSeed);
    }
    if (!exprX.trimmed().isEmpty())     o["exprX"]     = exprX.trimmed();
    if (!exprY.trimmed().isEmpty())     o["exprY"]     = exprY.trimmed();
    if (!exprRot.trimmed().isEmpty())   o["exprRot"]   = exprRot.trimmed();
    if (!exprScale.trimmed().isEmpty()) o["exprScale"] = exprScale.trimmed();
    return o;
}

PinDynamics PinDynamics::fromJson(const QJsonObject &obj)
{
    PinDynamics d;
    d.wiggleEnabled   = obj["wiggleEnabled"].toBool(false);
    d.wiggleFreqHz    = std::clamp(obj["wiggleFreqHz"].toDouble(2.0), 0.05, 20.0);
    d.wiggleAmpUv     = std::clamp(obj["wiggleAmpUv"].toDouble(0.025), 0.0, 0.5);
    d.wiggleRotAmpDeg = std::clamp(obj["wiggleRotAmpDeg"].toDouble(0.0), 0.0, 90.0);
    d.wiggleOctaves   = std::clamp(obj["wiggleOctaves"].toInt(2), 1, 6);
    d.wiggleSeed      = static_cast<unsigned int>(std::max(1, obj["wiggleSeed"].toInt(1)));
    d.exprX           = obj["exprX"].toString().trimmed();
    d.exprY           = obj["exprY"].toString().trimmed();
    d.exprRot         = obj["exprRot"].toString().trimmed();
    d.exprScale       = obj["exprScale"].toString().trimmed();
    return d;
}

bool PuppetPin::operator==(const PuppetPin &o) const
{
    return id == o.id && name == o.name && type == o.type
        && approxPt(restPos, o.restPos, 1e-5)
        && approxEq(radius, o.radius, 1e-5)
        && approxEq(stiffness, o.stiffness, 1e-5)
        && keyframes == o.keyframes
        && dynamics == o.dynamics;
}

QJsonObject PuppetPin::toJson() const
{
    QJsonObject o;
    o["id"]        = id;
    o["name"]      = name;
    o["type"]      = pinTypeName(type);
    o["rx"]        = restPos.x();
    o["ry"]        = restPos.y();
    o["radius"]    = radius;
    o["stiffness"] = stiffness;
    QJsonArray kfs;
    for (const auto &kf : keyframes) kfs.append(kf.toJson());
    o["keyframes"] = kfs;
    if (!dynamics.isDefault())
        o["dynamics"] = dynamics.toJson();
    return o;
}

PuppetPin PuppetPin::fromJson(const QJsonObject &obj)
{
    PuppetPin p;
    p.id        = std::max(0, obj["id"].toInt(0));
    p.name      = obj["name"].toString();
    p.type      = pinTypeFromString(obj["type"].toString());
    p.restPos   = QPointF(std::clamp(obj["rx"].toDouble(0.5), 0.0, 1.0),
                          std::clamp(obj["ry"].toDouble(0.5), 0.0, 1.0));
    p.radius    = std::clamp(obj["radius"].toDouble(0.25), 0.01, 2.0);
    p.stiffness = std::clamp(obj["stiffness"].toDouble(0.5), 0.0, 1.0);
    for (const auto &v : obj["keyframes"].toArray())
        p.keyframes.append(PinKeyframe::fromJson(v.toObject()));
    std::stable_sort(p.keyframes.begin(), p.keyframes.end(),
                     [](const PinKeyframe &a, const PinKeyframe &b) { return a.timeSec < b.timeSec; });
    if (obj.contains("dynamics"))
        p.dynamics = PinDynamics::fromJson(obj["dynamics"].toObject());
    return p;
}

bool MeshSettings::operator==(const MeshSettings &o) const
{
    return density == o.density && expansionPx == o.expansionPx
        && alphaThreshold == o.alphaThreshold
        && showMesh == o.showMesh && autoKey == o.autoKey;
}

QJsonObject MeshSettings::toJson() const
{
    QJsonObject o;
    o["density"]        = density;
    o["expansionPx"]    = expansionPx;
    o["alphaThreshold"] = alphaThreshold;
    o["showMesh"]       = showMesh;
    o["autoKey"]        = autoKey;
    return o;
}

MeshSettings MeshSettings::fromJson(const QJsonObject &obj)
{
    MeshSettings m;
    m.density        = std::clamp(obj["density"].toInt(14), 6, 36);
    m.expansionPx    = std::clamp(obj["expansionPx"].toInt(6), 0, 48);
    m.alphaThreshold = std::clamp(obj["alphaThreshold"].toInt(8), 1, 254);
    m.showMesh       = obj["showMesh"].toBool(true);
    m.autoKey        = obj["autoKey"].toBool(true);
    return m;
}

bool PuppetRig::operator==(const PuppetRig &o) const
{
    return enabled == o.enabled && nextPinId == o.nextPinId
        && mesh == o.mesh && pins == o.pins;
}

QJsonObject PuppetRig::toJson() const
{
    QJsonObject o;
    o["version"]   = 1;
    o["enabled"]   = enabled;
    o["nextPinId"] = nextPinId;
    o["mesh"]      = mesh.toJson();
    QJsonArray arr;
    for (const auto &p : pins) arr.append(p.toJson());
    o["pins"] = arr;
    return o;
}

PuppetRig PuppetRig::fromJson(const QJsonObject &obj)
{
    PuppetRig r;
    r.enabled   = obj["enabled"].toBool(true);
    r.nextPinId = std::max(1, obj["nextPinId"].toInt(1));
    if (obj.contains("mesh"))
        r.mesh = MeshSettings::fromJson(obj["mesh"].toObject());
    int maxId = 0;
    for (const auto &v : obj["pins"].toArray()) {
        PuppetPin p = PuppetPin::fromJson(v.toObject());
        if (p.id <= 0) p.id = ++maxId;
        else maxId = std::max(maxId, p.id);
        r.pins.append(p);
    }
    r.nextPinId = std::max(r.nextPinId, maxId + 1);
    return r;
}

// ============================================================
//  Keyframe evaluation & legacy conversion
// ============================================================

EvaluatedPin evaluatePinAt(const PuppetPin &pin, double clipLocalSec)
{
    EvaluatedPin out;
    out.id          = pin.id;
    out.type        = pin.type;
    out.restPos     = pin.restPos;
    out.position    = pin.restPos;
    out.rotationDeg = 0.0;
    out.scale       = 1.0;
    out.inFront     = 0.0;
    out.radius      = pin.radius;
    out.stiffness   = pin.stiffness;

    const auto applyKf = [&](const PinKeyframe &k) {
        out.position    = (pin.type == PinType::Position || pin.type == PinType::Advanced)
                              ? k.position : pin.restPos;
        out.rotationDeg = k.rotationDeg;
        out.scale       = k.scale;
        out.inFront     = k.inFront;
    };

    if (!pin.keyframes.isEmpty()) {
        if (pin.keyframes.size() == 1 || clipLocalSec <= pin.keyframes.first().timeSec) {
            applyKf(pin.keyframes.first());
        } else if (clipLocalSec >= pin.keyframes.last().timeSec) {
            applyKf(pin.keyframes.last());
        } else {
            bool matched = false;
            for (int i = 0; i + 1 < pin.keyframes.size(); ++i) {
                const PinKeyframe &a = pin.keyframes[i];
                const PinKeyframe &b = pin.keyframes[i + 1];
                if (clipLocalSec >= a.timeSec && clipLocalSec <= b.timeSec) {
                    const double span = b.timeSec - a.timeSec;
                    const double rawU = (span > 1e-9) ? (clipLocalSec - a.timeSec) / span : 0.0;
                    const double t = applyEasing(a.easing, rawU);
                    const QPointF pos = a.position + (b.position - a.position) * t;
                    out.position    = (pin.type == PinType::Position || pin.type == PinType::Advanced)
                                          ? pos : pin.restPos;
                    out.rotationDeg = a.rotationDeg + (b.rotationDeg - a.rotationDeg) * t;
                    out.scale       = a.scale + (b.scale - a.scale) * t;
                    out.inFront     = a.inFront + (b.inFront - a.inFront) * t;
                    matched = true;
                    break;
                }
            }
            if (!matched) applyKf(pin.keyframes.last());
        }
    }

    // Wave 32: Apply per-pin Wiggle & Expression secondary motion.
    const PinDynamics &dyn = pin.dynamics;
    if (!dyn.isDefault()) {
        if (dyn.wiggleEnabled) {
            wiggle::WiggleParams wp;
            wp.enabled              = true;
            wp.positionFrequency    = std::clamp(dyn.wiggleFreqHz, 0.05, 20.0);
            wp.positionAmplitude    = QPointF(std::clamp(dyn.wiggleAmpUv, 0.0, 0.5),
                                              std::clamp(dyn.wiggleAmpUv, 0.0, 0.5));
            wp.rotationFrequency    = wp.positionFrequency;
            wp.rotationAmplitudeDeg = std::clamp(dyn.wiggleRotAmpDeg, 0.0, 90.0);
            wp.octaves              = std::clamp(dyn.wiggleOctaves, 1, 6);
            wp.seed                 = dyn.wiggleSeed > 0 ? dyn.wiggleSeed : static_cast<unsigned int>(std::max(1, pin.id));
            const wiggle::WiggleOffset wo = wiggle::evaluate(wp, clipLocalSec);
            if (pin.type == PinType::Position || pin.type == PinType::Advanced) {
                out.position = QPointF(std::clamp(out.position.x() + wo.positionOffset.x(), -0.5, 1.5),
                                       std::clamp(out.position.y() + wo.positionOffset.y(), -0.5, 1.5));
            }
            if (pin.type == PinType::Bend || pin.type == PinType::Advanced) {
                out.rotationDeg = std::clamp(out.rotationDeg + wo.rotationOffsetDeg, -3600.0, 3600.0);
            }
        }

        auto evalExpr = [&](const QString &code, double baseVal, double lo, double hi) -> double {
            const QString trimmed = code.trimmed();
            if (trimmed.isEmpty()) return baseVal;
            ExpressionContext ctx;
            ctx.time = clipLocalSec;
            ctx.layerIndex = pin.id;
            ctx.value = baseVal;
            const ExpressionResult res = Expression::evaluate(trimmed, ctx);
            if (res.success && std::isfinite(res.value))
                return std::clamp(res.value, lo, hi);
            return baseVal;
        };

        if (pin.type == PinType::Position || pin.type == PinType::Advanced) {
            out.position.setX(evalExpr(dyn.exprX, out.position.x(), -0.5, 1.5));
            out.position.setY(evalExpr(dyn.exprY, out.position.y(), -0.5, 1.5));
        }
        if (pin.type == PinType::Bend || pin.type == PinType::Advanced) {
            out.rotationDeg = evalExpr(dyn.exprRot, out.rotationDeg, -3600.0, 3600.0);
            out.scale       = evalExpr(dyn.exprScale, out.scale, 0.1, 4.0);
        }
    }

    return out;
}

QVector<EvaluatedPin> evaluateRigAt(const PuppetRig &rig, double clipLocalSec)
{
    QVector<EvaluatedPin> out;
    out.reserve(rig.pins.size());
    for (const auto &pin : rig.pins)
        out.append(evaluatePinAt(pin, clipLocalSec));
    return out;
}

void upsertPinKeyframe(PuppetPin &pin, const PinKeyframe &kf)
{
    for (auto &existing : pin.keyframes) {
        if (std::abs(existing.timeSec - kf.timeSec) <= 1e-3) {
            existing = kf;
            return;
        }
    }
    pin.keyframes.append(kf);
    std::stable_sort(pin.keyframes.begin(), pin.keyframes.end(),
                     [](const PinKeyframe &a, const PinKeyframe &b) { return a.timeSec < b.timeSec; });
}

bool removePinKeyframeNear(PuppetPin &pin, double clipLocalSec, double epsSec)
{
    int bestIdx = -1;
    double bestDist = epsSec;
    for (int i = 0; i < pin.keyframes.size(); ++i) {
        const double d = std::abs(pin.keyframes[i].timeSec - clipLocalSec);
        if (d <= bestDist) {
            bestDist = d;
            bestIdx = i;
        }
    }
    if (bestIdx < 0) return false;
    pin.keyframes.removeAt(bestIdx);
    return true;
}

// ============================================================
//  Wave 32: Puppet Sketch RDP Simplification & Sway Presets
// ============================================================

namespace {

// Time-parameterized linear interpolation error in UV space:
// measures how far sample[m] deviates from linear interpolation between sample[a] and sample[b].
double rdpTemporalDeviation(const SketchSample &a, const SketchSample &b, const SketchSample &m)
{
    const double dt = b.timeSec - a.timeSec;
    if (dt <= 1e-9)
        return std::hypot(m.posUv.x() - a.posUv.x(), m.posUv.y() - a.posUv.y());
    const double u = std::clamp((m.timeSec - a.timeSec) / dt, 0.0, 1.0);
    const double ix = a.posUv.x() + (b.posUv.x() - a.posUv.x()) * u;
    const double iy = a.posUv.y() + (b.posUv.y() - a.posUv.y()) * u;
    return std::hypot(m.posUv.x() - ix, m.posUv.y() - iy);
}

void rdpRecurse(const QVector<SketchSample> &pts,
                int first,
                int last,
                double eps,
                QVector<bool> &keep)
{
    if (last <= first + 1) return;
    double maxErr = -1.0;
    int splitIdx = -1;
    for (int i = first + 1; i < last; ++i) {
        const double err = rdpTemporalDeviation(pts[first], pts[last], pts[i]);
        if (err > maxErr) {
            maxErr = err;
            splitIdx = i;
        }
    }
    if (maxErr > eps && splitIdx > first && splitIdx < last) {
        keep[splitIdx] = true;
        rdpRecurse(pts, first, splitIdx, eps, keep);
        rdpRecurse(pts, splitIdx, last, eps, keep);
    }
}

} // namespace

QVector<SketchSample> simplifySketchRdp(const QVector<SketchSample> &samples,
                                        double epsilonUv,
                                        double minDtSec)
{
    if (samples.size() <= 2) return samples;

    // 1. Sort monotonically by timeSec and pre-filter ultra-dense duplicate timestamps.
    QVector<SketchSample> sorted = samples;
    std::stable_sort(sorted.begin(), sorted.end(), [](const SketchSample &a, const SketchSample &b) {
        return a.timeSec < b.timeSec;
    });

    QVector<SketchSample> clean;
    clean.reserve(sorted.size());
    clean.append(sorted.first());
    const double minDt = std::max(0.0, minDtSec);
    for (int i = 1; i + 1 < sorted.size(); ++i) {
        if (sorted[i].timeSec - clean.last().timeSec >= minDt)
            clean.append(sorted[i]);
    }
    if (sorted.last().timeSec > clean.last().timeSec + 1e-6)
        clean.append(sorted.last());
    else
        clean.last() = sorted.last();

    if (clean.size() <= 2) return clean;

    // 2. Ramer-Douglas-Peucker recursion in time-UV trajectory space.
    QVector<bool> keep(clean.size(), false);
    keep[0] = true;
    keep[clean.size() - 1] = true;
    rdpRecurse(clean, 0, clean.size() - 1, std::max(1e-5, epsilonUv), keep);

    QVector<SketchSample> out;
    for (int i = 0; i < clean.size(); ++i) {
        if (keep[i]) out.append(clean[i]);
    }
    return out;
}

int applySketchToPin(PuppetPin &pin,
                     const QVector<SketchSample> &rawSamples,
                     double epsilonUv,
                     EasingKind easing,
                     bool replaceInRange)
{
    if (rawSamples.isEmpty()) return 0;
    const QVector<SketchSample> simplified = simplifySketchRdp(rawSamples, epsilonUv);
    if (simplified.isEmpty()) return 0;

    if (replaceInRange) {
        const double t0 = simplified.first().timeSec - 1e-4;
        const double t1 = simplified.last().timeSec + 1e-4;
        QVector<PinKeyframe> kept;
        kept.reserve(pin.keyframes.size());
        for (const auto &kf : pin.keyframes) {
            if (kf.timeSec < t0 || kf.timeSec > t1)
                kept.append(kf);
        }
        pin.keyframes = kept;
    }

    for (const auto &s : simplified) {
        const EvaluatedPin base = evaluatePinAt(pin, s.timeSec);
        PinKeyframe kf;
        kf.timeSec     = std::max(0.0, s.timeSec);
        kf.position    = QPointF(std::clamp(s.posUv.x(), -0.5, 1.5),
                                 std::clamp(s.posUv.y(), -0.5, 1.5));
        kf.rotationDeg = base.rotationDeg;
        kf.scale       = base.scale;
        kf.inFront     = base.inFront;
        kf.easing      = easing;
        upsertPinKeyframe(pin, kf);
    }
    return simplified.size();
}

void applySwayPreset(PuppetPin &pin, SwayPreset preset)
{
    PinDynamics dyn;
    dyn.wiggleSeed = static_cast<unsigned int>(std::max(1, pin.id * 17 + 3));
    switch (preset) {
    case SwayPreset::None:
        break;
    case SwayPreset::HairRibbon:
        dyn.wiggleEnabled   = true;
        dyn.wiggleFreqHz    = 1.8;
        dyn.wiggleAmpUv     = 0.020;
        dyn.wiggleRotAmpDeg = 4.0;
        dyn.wiggleOctaves   = 2;
        break;
    case SwayPreset::Breathing:
        dyn.wiggleEnabled = false;
        dyn.exprY = QStringLiteral("value + sin(time * 2.0) * 0.015");
        break;
    case SwayPreset::WindGust:
        dyn.wiggleEnabled   = true;
        dyn.wiggleFreqHz    = 3.0;
        dyn.wiggleAmpUv     = 0.045;
        dyn.wiggleRotAmpDeg = 12.0;
        dyn.wiggleOctaves   = 3;
        break;
    }
    pin.dynamics = dyn;
}

PuppetRig rigFromLegacyPins(const QVector<WarpPin> &legacy)
{
    PuppetRig rig;
    rig.enabled = !legacy.isEmpty();
    for (int i = 0; i < legacy.size(); ++i) {
        PuppetPin pin;
        pin.id = rig.nextPinId++;
        pin.name = QStringLiteral("ピン %1").arg(pin.id);
        pin.type = PinType::Position;
        pin.restPos = legacy[i].originalPos;
        pin.radius = std::clamp(legacy[i].radius, 0.01, 2.0);
        pin.stiffness = std::clamp(legacy[i].stiffness, 0.0, 1.0);
        PinKeyframe k;
        k.timeSec = 0.0;
        k.position = legacy[i].deformedPos;
        pin.keyframes.append(k);
        rig.pins.append(pin);
    }
    return rig;
}

QVector<WarpPin> toLegacyPreviewPins(const PuppetRig &rig, double clipLocalSec)
{
    QVector<WarpPin> out;
    if (!rig.isActive()) return out;
    const auto eval = evaluateRigAt(rig, clipLocalSec);
    out.reserve(eval.size());
    for (const auto &ep : eval) {
        WarpPin wp;
        wp.originalPos = ep.restPos;
        wp.deformedPos = ep.position;
        wp.radius      = std::clamp(ep.radius, 0.01, 2.0);
        wp.stiffness   = std::clamp(ep.stiffness, 0.0, 1.0);
        out.append(wp);
    }
    return out;
}

// ============================================================
//  Alpha-Aware & Full-Frame Triangulated Mesh Builder
// ============================================================

PuppetMesh buildMeshForImage(const QImage &image, const MeshSettings &settings)
{
    PuppetMesh mesh;
    if (image.isNull() || image.width() < 2 || image.height() < 2)
        return mesh;

    const int w = image.width();
    const int h = image.height();
    mesh.width = w;
    mesh.height = h;

    const int density = std::clamp(settings.density, 6, 36);
    const int expansionPx = std::clamp(settings.expansionPx, 0, 48);
    const int alphaThresh = std::clamp(settings.alphaThreshold, 1, 254);

    // Aspect-preserving cell grid so triangles are nearly equilateral.
    int cols = density;
    int rows = density;
    if (w >= h) {
        cols = density;
        rows = std::max(4, static_cast<int>(std::lround(static_cast<double>(density) * h / w)));
    } else {
        rows = density;
        cols = std::max(4, static_cast<int>(std::lround(static_cast<double>(density) * w / h)));
    }

    QImage argb = (image.format() == QImage::Format_ARGB32
                   || image.format() == QImage::Format_ARGB32_Premultiplied)
                      ? image
                      : image.convertToFormat(QImage::Format_ARGB32);

    // Detect whether the image is a transparent PNG / cutout or an opaque video frame.
    bool useSilhouette = false;
    if (argb.hasAlphaChannel()) {
        int transparentSamples = 0;
        int opaqueSamples = 0;
        const int sx = std::max(1, w / 64);
        const int sy = std::max(1, h / 64);
        for (int y = 0; y < h; y += sy) {
            const QRgb *row = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
            for (int x = 0; x < w; x += sx) {
                if (qAlpha(row[x]) < alphaThresh) ++transparentSamples;
                else ++opaqueSamples;
            }
        }
        const int total = transparentSamples + opaqueSamples;
        // Transparent silhouette mode if >= 2% transparent AND >= 1 pixel opaque.
        useSilhouette = (total > 0 && opaqueSamples > 0
                         && transparentSamples * 50 >= total);
    }
    mesh.alphaSilhouette = useSilhouette;

    QVector<quint8> cellActive(cols * rows, useSilhouette ? 0 : 1);
    if (useSilhouette) {
        const double cellW = static_cast<double>(w) / cols;
        const double cellH = static_cast<double>(h) / rows;
        int activeCount = 0;
        for (int r = 0; r < rows; ++r) {
            const int y0 = std::max(0, static_cast<int>(std::floor(r * cellH)) - expansionPx);
            const int y1 = std::min(h - 1, static_cast<int>(std::ceil((r + 1) * cellH)) + expansionPx);
            for (int c = 0; c < cols; ++c) {
                const int x0 = std::max(0, static_cast<int>(std::floor(c * cellW)) - expansionPx);
                const int x1 = std::min(w - 1, static_cast<int>(std::ceil((c + 1) * cellW)) + expansionPx);
                bool hit = false;
                // Adaptive stride inside huge cells for speed, stride=1 for small cells.
                const int stepY = std::max(1, (y1 - y0 + 1) / 16);
                const int stepX = std::max(1, (x1 - x0 + 1) / 16);
                for (int y = y0; y <= y1 && !hit; y += stepY) {
                    const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
                    for (int x = x0; x <= x1; x += stepX) {
                        if (qAlpha(line[x]) >= alphaThresh) {
                            hit = true;
                            break;
                        }
                    }
                }
                if (hit) {
                    cellActive[r * cols + c] = 1;
                    ++activeCount;
                }
            }
        }
        if (activeCount == 0) {
            // Fallback to full grid if image is completely transparent.
            cellActive.fill(1);
            mesh.alphaSilhouette = false;
        }
    }

    // Compact vertices referenced by active cells.
    const int vCols = cols + 1;
    const int vRows = rows + 1;
    QVector<int> gridToCompact(vCols * vRows, -1);

    auto getVertex = [&](int gc, int gr) -> int {
        const int gIdx = gr * vCols + gc;
        int idx = gridToCompact[gIdx];
        if (idx >= 0) return idx;
        idx = mesh.restVertices.size();
        gridToCompact[gIdx] = idx;
        const double x = static_cast<double>(gc) * w / cols;
        const double y = static_cast<double>(gr) * h / rows;
        mesh.restVertices.append(QPointF(x, y));
        return idx;
    };

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (!cellActive[r * cols + c]) continue;
            const int v00 = getVertex(c, r);
            const int v10 = getVertex(c + 1, r);
            const int v01 = getVertex(c, r + 1);
            const int v11 = getVertex(c + 1, r + 1);
            // Alternating diagonals prevent directional shear bias in ARAP.
            if (((r + c) & 1) == 0) {
                mesh.triangles.append({v00, v10, v11});
                mesh.triangles.append({v00, v11, v01});
            } else {
                mesh.triangles.append({v00, v10, v01});
                mesh.triangles.append({v10, v11, v01});
            }
        }
    }

    mesh.triangleStiffness.fill(1.0, mesh.triangles.size());
    mesh.triangleDepth.fill(0.0, mesh.triangles.size());
    return mesh;
}

// ============================================================
//  Igarashi 2-Step ARAP Solver (Position / Bend / Advanced / Starch / Overlap)
// ============================================================

DeformedMesh solveArapMesh(const PuppetMesh &mesh, const QVector<EvaluatedPin> &pins)
{
    DeformedMesh out;
    out.base = mesh;
    out.deformedVertices = mesh.restVertices;

    const int nVerts = mesh.restVertices.size();
    const int nTris = mesh.triangles.size();
    if (nVerts == 0 || nTris == 0 || pins.isEmpty())
        return out;

    const double w = static_cast<double>(std::max(1, mesh.width));
    const double h = static_cast<double>(std::max(1, mesh.height));
    const double shortSide = std::min(w, h);

    // 1. Compute per-triangle Starch rigidity weights and Overlap Z-depths.
    out.base.triangleStiffness.fill(1.0, nTris);
    out.base.triangleDepth.fill(0.0, nTris);

    QVector<QPointF> triCentroids(nTris);
    for (int ti = 0; ti < nTris; ++ti) {
        const auto &t = mesh.triangles[ti];
        triCentroids[ti] = (mesh.restVertices[t[0]] + mesh.restVertices[t[1]] + mesh.restVertices[t[2]]) / 3.0;
    }

    for (int ti = 0; ti < nTris; ++ti) {
        const QPointF &cen = triCentroids[ti];
        double stiff = 1.0;
        double depthNum = 0.0;
        double depthDen = 0.0;
        for (const auto &pin : pins) {
            const QPointF pinPx(pin.restPos.x() * w, pin.restPos.y() * h);
            const double radPx = std::max(8.0, pin.radius * shortSide);
            const double dx = cen.x() - pinPx.x();
            const double dy = cen.y() - pinPx.y();
            const double dist = std::sqrt(dx * dx + dy * dy);
            if (dist >= radPx) continue;
            const double falloff = 1.0 - dist / radPx;
            const double wFall = falloff * falloff;
            if (pin.type == PinType::Starch) {
                stiff += 12.0 * std::clamp(pin.stiffness, 0.0, 1.0) * wFall;
            } else if (pin.type == PinType::Overlap) {
                depthNum += wFall * pin.inFront;
                depthDen += wFall;
            }
        }
        out.base.triangleStiffness[ti] = stiff;
        if (depthDen > 1e-9)
            out.base.triangleDepth[ti] = depthNum / depthDen;
    }

    // 2. Gather active control constraints (Position / Bend / Advanced).
    struct PinConstraint {
        int triIndex = -1;
        std::array<double, 3> bary = {1.0, 0.0, 0.0};
        QPointF restPx;
        QPointF targetPx;
        double rotRad = 0.0;
        double scale = 1.0;
        double radiusPx = 50.0;
        bool hasRotScale = false;
    };

    QVector<PinConstraint> constraints;
    bool anyDeformation = false;
    for (const auto &pin : pins) {
        if (pin.type == PinType::Starch || pin.type == PinType::Overlap)
            continue;
        const QPointF restPx(pin.restPos.x() * w, pin.restPos.y() * h);
        const QPointF targetPx = (pin.type == PinType::Bend)
                                     ? restPx
                                     : QPointF(pin.position.x() * w, pin.position.y() * h);
        const BaryHit hit = bindPointToMesh(mesh, restPx);
        if (hit.triIndex < 0) continue;

        PinConstraint pc;
        pc.triIndex    = hit.triIndex;
        pc.bary        = hit.bary;
        pc.restPx      = restPx;
        pc.targetPx    = targetPx;
        pc.rotRad      = (pin.type == PinType::Bend || pin.type == PinType::Advanced)
                             ? (pin.rotationDeg * kDegToRad) : 0.0;
        pc.scale       = (pin.type == PinType::Bend || pin.type == PinType::Advanced)
                             ? std::clamp(pin.scale, 0.1, 4.0) : 1.0;
        pc.radiusPx    = std::max(12.0, pin.radius * shortSide);
        pc.hasRotScale = (pin.type == PinType::Bend || pin.type == PinType::Advanced)
                         && (std::abs(pc.rotRad) > 1e-5 || std::abs(pc.scale - 1.0) > 1e-4);

        if (std::abs(targetPx.x() - restPx.x()) > 1e-4
            || std::abs(targetPx.y() - restPx.y()) > 1e-4
            || pc.hasRotScale) {
            anyDeformation = true;
        }
        constraints.append(pc);
    }

    if (constraints.isEmpty() || !anyDeformation)
        return out;

    // 3. Connected-component analysis so pin-free alpha islands stay anchored at rest.
    QVector<int> parent(nVerts);
    std::iota(parent.begin(), parent.end(), 0);
    auto findRoot = [&](int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        a = findRoot(a);
        b = findRoot(b);
        if (a != b) parent[a] = b;
    };
    for (const auto &tri : mesh.triangles) {
        unite(tri[0], tri[1]);
        unite(tri[1], tri[2]);
    }
    QHash<int, int> compPinCount;
    for (const auto &pc : constraints) {
        const auto &tri = mesh.triangles[pc.triIndex];
        compPinCount[findRoot(tri[0])] += 1;
    }

    // ============================================================
    //  Stage 1: Similarity-invariant solve (2N x 2N SPD system)
    // ============================================================
    const int dim1 = 2 * nVerts;
    SparseBuilder H1(dim1);
    QVector<double> b1(dim1, 0.0);

    for (int ti = 0; ti < nTris; ++ti) {
        const auto &tri = mesh.triangles[ti];
        const double wt = out.base.triangleStiffness[ti];
        for (int e = 0; e < 3; ++e) {
            const int ia = tri[e];
            const int ib = tri[(e + 1) % 3];
            const int ic = tri[(e + 2) % 3];
            const QPointF &va = mesh.restVertices[ia];
            const QPointF &vb = mesh.restVertices[ib];
            const QPointF &vc = mesh.restVertices[ic];

            const double ex = vb.x() - va.x();
            const double ey = vb.y() - va.y();
            const double len2 = ex * ex + ey * ey;
            if (len2 < 1e-10) continue;

            const double rx = vc.x() - va.x();
            const double ry = vc.y() - va.y();
            const double xab = (rx * ex + ry * ey) / len2;
            const double yab = (-rx * ey + ry * ex) / len2;

            // Rx = v_cx - (1 - xab)*v_ax - xab*v_bx - yab*v_ay + yab*v_by
            const int idxX[5] = {2 * ic, 2 * ia, 2 * ib, 2 * ia + 1, 2 * ib + 1};
            const double cX[5] = {1.0, -(1.0 - xab), -xab, -yab, yab};
            H1.addQuadRow(idxX, cX, 5, wt);

            // Ry = v_cy - (1 - xab)*v_ay - xab*v_by + yab*v_ax - yab*v_bx
            const int idxY[5] = {2 * ic + 1, 2 * ia + 1, 2 * ib + 1, 2 * ia, 2 * ib};
            const double cY[5] = {1.0, -(1.0 - xab), -xab, yab, -yab};
            H1.addQuadRow(idxY, cY, 5, wt);
        }
    }

    // Add pin positional & Bend/Advanced local-frame constraints to Stage 1.
    constexpr double kPinPosWeight = 1000.0;
    constexpr double kPinRotWeight = 250.0;
    for (const auto &pc : constraints) {
        const auto &tri = mesh.triangles[pc.triIndex];
        const int idxX[3] = {2 * tri[0], 2 * tri[1], 2 * tri[2]};
        const int idxY[3] = {2 * tri[0] + 1, 2 * tri[1] + 1, 2 * tri[2] + 1};
        H1.addQuadRow(idxX, pc.bary.data(), 3, kPinPosWeight);
        H1.addQuadRow(idxY, pc.bary.data(), 3, kPinPosWeight);
        for (int m = 0; m < 3; ++m) {
            b1[idxX[m]] += kPinPosWeight * pc.bary[m] * pc.targetPx.x();
            b1[idxY[m]] += kPinPosWeight * pc.bary[m] * pc.targetPx.y();
        }
        if (pc.hasRotScale) {
            const double cs = std::cos(pc.rotRad) * pc.scale;
            const double sn = std::sin(pc.rotRad) * pc.scale;
            for (int m = 0; m < 3; ++m) {
                const int vi = tri[m];
                const double dx = mesh.restVertices[vi].x() - pc.restPx.x();
                const double dy = mesh.restVertices[vi].y() - pc.restPx.y();
                const double qx = pc.targetPx.x() + cs * dx - sn * dy;
                const double qy = pc.targetPx.y() + sn * dx + cs * dy;
                H1.add(2 * vi, 2 * vi, kPinRotWeight);
                H1.add(2 * vi + 1, 2 * vi + 1, kPinRotWeight);
                b1[2 * vi]     += kPinRotWeight * qx;
                b1[2 * vi + 1] += kPinRotWeight * qy;
            }
        }
    }

    // Regularization + anchor pin-free disconnected islands.
    for (int vi = 0; vi < nVerts; ++vi) {
        const int pinsInComp = compPinCount.value(findRoot(vi), 0);
        const double reg = (pinsInComp == 0) ? 10.0 : 1e-4;
        H1.add(2 * vi, 2 * vi, reg);
        H1.add(2 * vi + 1, 2 * vi + 1, reg);
        b1[2 * vi]     += reg * mesh.restVertices[vi].x();
        b1[2 * vi + 1] += reg * mesh.restVertices[vi].y();
    }

    QVector<double> v1(dim1);
    for (int vi = 0; vi < nVerts; ++vi) {
        v1[2 * vi]     = mesh.restVertices[vi].x();
        v1[2 * vi + 1] = mesh.restVertices[vi].y();
    }
    const SparseBuilder::Csr csrH1 = H1.build();
    solvePcg(csrH1, b1, v1, 500, 1e-6);

    QVector<double> vx(nVerts), vy(nVerts);
    for (int vi = 0; vi < nVerts; ++vi) {
        vx[vi] = v1[2 * vi];
        vy[vi] = v1[2 * vi + 1];
    }

    // ============================================================
    //  Stage 2: Rotation-fitting & scale-adjusted positions (2 iterations)
    // ============================================================
    // Precompute per-triangle target scale driven by Bend / Advanced pins.
    QVector<double> triTargetScale(nTris, 1.0);
    for (int ti = 0; ti < nTris; ++ti) {
        double scaleDelta = 0.0;
        double scaleWeight = 0.0;
        for (const auto &pc : constraints) {
            if (!pc.hasRotScale || std::abs(pc.scale - 1.0) <= 1e-4) continue;
            const double dx = triCentroids[ti].x() - pc.restPx.x();
            const double dy = triCentroids[ti].y() - pc.restPx.y();
            const double dist = std::sqrt(dx * dx + dy * dy);
            if (dist >= pc.radiusPx) continue;
            const double f = 1.0 - dist / pc.radiusPx;
            const double wf = f * f;
            scaleDelta += wf * (pc.scale - 1.0);
            scaleWeight += wf;
        }
        if (scaleWeight > 1e-9)
            triTargetScale[ti] = std::clamp(1.0 + scaleDelta / std::max(1.0, scaleWeight), 0.15, 4.0);
    }

    // Build decoupled N x N Stage-2 SPD system matrix once.
    SparseBuilder H2(nVerts);
    for (int ti = 0; ti < nTris; ++ti) {
        const auto &tri = mesh.triangles[ti];
        const double wt = out.base.triangleStiffness[ti];
        for (int e = 0; e < 3; ++e) {
            const int ia = tri[e];
            const int ib = tri[(e + 1) % 3];
            H2.add(ia, ia, wt);
            H2.add(ib, ib, wt);
            H2.add(ia, ib, -wt);
            H2.add(ib, ia, -wt);
        }
    }
    for (const auto &pc : constraints) {
        const auto &tri = mesh.triangles[pc.triIndex];
        H2.addQuadRow(tri.data(), pc.bary.data(), 3, kPinPosWeight);
        if (pc.hasRotScale) {
            for (int m = 0; m < 3; ++m)
                H2.add(tri[m], tri[m], kPinRotWeight);
        }
    }
    for (int vi = 0; vi < nVerts; ++vi) {
        const int pinsInComp = compPinCount.value(findRoot(vi), 0);
        const double reg = (pinsInComp == 0) ? 10.0 : 1e-4;
        H2.add(vi, vi, reg);
    }
    const SparseBuilder::Csr csrH2 = H2.build();

    QVector<double> bx(nVerts), by(nVerts);
    for (int iter = 0; iter < 2; ++iter) {
        bx.fill(0.0);
        by.fill(0.0);

        for (int ti = 0; ti < nTris; ++ti) {
            const auto &tri = mesh.triangles[ti];
            const double wt = out.base.triangleStiffness[ti];

            // Closed-form 2D Procrustes rigid rotation from rest -> current deformed triangle.
            const QPointF c0 = (mesh.restVertices[tri[0]] + mesh.restVertices[tri[1]] + mesh.restVertices[tri[2]]) / 3.0;
            const double c1x = (vx[tri[0]] + vx[tri[1]] + vx[tri[2]]) / 3.0;
            const double c1y = (vy[tri[0]] + vy[tri[1]] + vy[tri[2]]) / 3.0;

            double cosAcc = 0.0;
            double sinAcc = 0.0;
            for (int m = 0; m < 3; ++m) {
                const int vi = tri[m];
                const double px = mesh.restVertices[vi].x() - c0.x();
                const double py = mesh.restVertices[vi].y() - c0.y();
                const double qx = vx[vi] - c1x;
                const double qy = vy[vi] - c1y;
                cosAcc += px * qx + py * qy;
                sinAcc += px * qy - py * qx;
            }
            const double mag = std::hypot(cosAcc, sinAcc);
            const double rCos = (mag > 1e-12) ? (cosAcc / mag) : 1.0;
            const double rSin = (mag > 1e-12) ? (sinAcc / mag) : 0.0;
            const double sc = triTargetScale[ti];
            const double sCos = sc * rCos;
            const double sSin = sc * rSin;

            for (int e = 0; e < 3; ++e) {
                const int ia = tri[e];
                const int ib = tri[(e + 1) % 3];
                const double ex = mesh.restVertices[ib].x() - mesh.restVertices[ia].x();
                const double ey = mesh.restVertices[ib].y() - mesh.restVertices[ia].y();
                const double dx = sCos * ex - sSin * ey;
                const double dy = sSin * ex + sCos * ey;
                bx[ia] -= wt * dx;
                bx[ib] += wt * dx;
                by[ia] -= wt * dy;
                by[ib] += wt * dy;
            }
        }

        for (const auto &pc : constraints) {
            const auto &tri = mesh.triangles[pc.triIndex];
            for (int m = 0; m < 3; ++m) {
                bx[tri[m]] += kPinPosWeight * pc.bary[m] * pc.targetPx.x();
                by[tri[m]] += kPinPosWeight * pc.bary[m] * pc.targetPx.y();
            }
            if (pc.hasRotScale) {
                const double cs = std::cos(pc.rotRad) * pc.scale;
                const double sn = std::sin(pc.rotRad) * pc.scale;
                for (int m = 0; m < 3; ++m) {
                    const int vi = tri[m];
                    const double dx = mesh.restVertices[vi].x() - pc.restPx.x();
                    const double dy = mesh.restVertices[vi].y() - pc.restPx.y();
                    bx[vi] += kPinRotWeight * (pc.targetPx.x() + cs * dx - sn * dy);
                    by[vi] += kPinRotWeight * (pc.targetPx.y() + sn * dx + cs * dy);
                }
            }
        }

        for (int vi = 0; vi < nVerts; ++vi) {
            const int pinsInComp = compPinCount.value(findRoot(vi), 0);
            const double reg = (pinsInComp == 0) ? 10.0 : 1e-4;
            bx[vi] += reg * mesh.restVertices[vi].x();
            by[vi] += reg * mesh.restVertices[vi].y();
        }

        solvePcg(csrH2, bx, vx, 300, 1e-6);
        solvePcg(csrH2, by, vy, 300, 1e-6);
    }

    for (int vi = 0; vi < nVerts; ++vi)
        out.deformedVertices[vi] = QPointF(vx[vi], vy[vi]);
    return out;
}

// ============================================================
//  Z-Sorted Premultiplied Bilinear Triangle Rasterizer
// ============================================================

QImage rasterizeDeformedMesh(const QImage &source, const DeformedMesh &deformed)
{
    if (source.isNull() || deformed.base.triangles.isEmpty()
        || deformed.deformedVertices.size() != deformed.base.restVertices.size())
        return source;

    const int w = source.width();
    const int h = source.height();
    QImage srcPremul = (source.format() == QImage::Format_ARGB32_Premultiplied)
                           ? source
                           : source.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage dst(w, h, QImage::Format_ARGB32_Premultiplied);
    dst.fill(0);

    // Sort triangles back-to-front by Overlap Z-depth.
    const int nTris = deformed.base.triangles.size();
    QVector<int> order(nTris);
    std::iota(order.begin(), order.end(), 0);
    const auto &depths = deformed.base.triangleDepth;
    if (depths.size() == nTris) {
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return depths[a] < depths[b];
        });
    }

    auto isTopLeftEdge = [](const QPointF &a, const QPointF &b) -> bool {
        const double dy = b.y() - a.y();
        const double dx = b.x() - a.x();
        return (dy < 0.0) || (dy == 0.0 && dx > 0.0);
    };

    for (int ti : order) {
        const auto &tri = deformed.base.triangles[ti];
        QPointF d0 = deformed.deformedVertices[tri[0]];
        QPointF d1 = deformed.deformedVertices[tri[1]];
        QPointF d2 = deformed.deformedVertices[tri[2]];
        QPointF s0 = deformed.base.restVertices[tri[0]];
        QPointF s1 = deformed.base.restVertices[tri[1]];
        QPointF s2 = deformed.base.restVertices[tri[2]];

        double area = (d1.x() - d0.x()) * (d2.y() - d0.y())
                    - (d1.y() - d0.y()) * (d2.x() - d0.x());
        if (std::abs(area) < 1e-6) continue;
        if (area < 0.0) {
            std::swap(d1, d2);
            std::swap(s1, s2);
            area = -area;
        }
        const double invArea = 1.0 / area;

        const int minX = std::max(0, static_cast<int>(std::floor(std::min({d0.x(), d1.x(), d2.x()}))));
        const int maxX = std::min(w - 1, static_cast<int>(std::ceil(std::max({d0.x(), d1.x(), d2.x()}))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({d0.y(), d1.y(), d2.y()}))));
        const int maxY = std::min(h - 1, static_cast<int>(std::ceil(std::max({d0.y(), d1.y(), d2.y()}))));
        if (minX > maxX || minY > maxY) continue;

        const bool tl0 = isTopLeftEdge(d1, d2);
        const bool tl1 = isTopLeftEdge(d2, d0);
        const bool tl2 = isTopLeftEdge(d0, d1);

        const double e0dx = -(d2.y() - d1.y());
        const double e0dy =  (d2.x() - d1.x());
        const double e1dx = -(d0.y() - d2.y());
        const double e1dy =  (d0.x() - d2.x());
        const double e2dx = -(d1.y() - d0.y());
        const double e2dy =  (d1.x() - d0.x());

        const double startX = minX + 0.5;
        const double startY = minY + 0.5;
        double rowE0 = (d2.x() - d1.x()) * (startY - d1.y()) - (d2.y() - d1.y()) * (startX - d1.x());
        double rowE1 = (d0.x() - d2.x()) * (startY - d2.y()) - (d0.y() - d2.y()) * (startX - d2.x());
        double rowE2 = (d1.x() - d0.x()) * (startY - d0.y()) - (d1.y() - d0.y()) * (startX - d0.x());

        for (int py = minY; py <= maxY; ++py) {
            QRgb *dstRow = reinterpret_cast<QRgb *>(dst.scanLine(py));
            double e0 = rowE0;
            double e1 = rowE1;
            double e2 = rowE2;
            for (int px = minX; px <= maxX; ++px) {
                const bool in0 = tl0 ? (e0 >= 0.0) : (e0 > 0.0);
                const bool in1 = tl1 ? (e1 >= 0.0) : (e1 > 0.0);
                const bool in2 = tl2 ? (e2 >= 0.0) : (e2 > 0.0);
                if (in0 && in1 && in2) {
                    const double b0 = e0 * invArea;
                    const double b1 = e1 * invArea;
                    const double b2 = 1.0 - b0 - b1;
                    const double sx = b0 * s0.x() + b1 * s1.x() + b2 * s2.x();
                    const double sy = b0 * s0.y() + b1 * s1.y() + b2 * s2.y();
                    const QRgb sample = samplePremulBilinear(srcPremul, sx, sy);
                    if (sample != 0)
                        dstRow[px] = premulSrcOver(sample, dstRow[px]);
                }
                e0 += e0dx;
                e1 += e1dx;
                e2 += e2dx;
            }
            rowE0 += e0dy;
            rowE1 += e1dy;
            rowE2 += e2dy;
        }
    }

    return (source.format() == QImage::Format_ARGB32_Premultiplied)
               ? dst
               : dst.convertToFormat(source.format());
}

QImage renderPuppetFrame(const QImage &source, const PuppetRig &rig, double clipLocalSec)
{
    if (source.isNull() || !rig.isActive())
        return source;

    const QVector<EvaluatedPin> evaluated = evaluateRigAt(rig, clipLocalSec);
    if (evaluated.isEmpty())
        return source;

    // Lookup or build cached mesh.
    MeshCacheKey key;
    key.width          = source.width();
    key.height         = source.height();
    key.density        = std::clamp(rig.mesh.density, 6, 36);
    key.expansionPx    = std::clamp(rig.mesh.expansionPx, 0, 48);
    key.alphaThreshold = std::clamp(rig.mesh.alphaThreshold, 1, 254);
    key.alphaHash      = alphaFingerprint(source, key.alphaThreshold);

    PuppetMesh mesh;
    bool found = false;
    {
        QMutexLocker lock(&g_meshCacheMutex);
        for (int i = 0; i < g_meshCache.size(); ++i) {
            if (g_meshCache[i].key == key) {
                mesh = g_meshCache[i].mesh;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        mesh = buildMeshForImage(source, rig.mesh);
        QMutexLocker lock(&g_meshCacheMutex);
        if (g_meshCache.size() >= kMaxMeshCacheEntries)
            g_meshCache.removeFirst();
        g_meshCache.append({key, mesh});
    }

    const DeformedMesh deformed = solveArapMesh(mesh, evaluated);
    return rasterizeDeformedMesh(source, deformed);
}

// ============================================================
//  Global RigStore & Timeline Undo Bridge
// ============================================================

bool hasRig(const QString &clipUid)
{
    if (clipUid.isEmpty()) return false;
    QMutexLocker lock(&g_storeMutex);
    const auto it = g_rigStore.constFind(clipUid);
    return it != g_rigStore.constEnd() && it.value().isActive();
}

PuppetRig rigForClip(const QString &clipUid)
{
    if (clipUid.isEmpty()) return {};
    QMutexLocker lock(&g_storeMutex);
    return g_rigStore.value(clipUid);
}

void setRigForClip(const QString &clipUid, const PuppetRig &rig)
{
    if (clipUid.isEmpty()) return;
    PuppetRig normalized = rig;
    int maxId = 0;
    for (auto &p : normalized.pins) {
        if (p.id <= 0) p.id = ++maxId;
        else maxId = std::max(maxId, p.id);
    }
    normalized.nextPinId = std::max(normalized.nextPinId, maxId + 1);
    QMutexLocker lock(&g_storeMutex);
    if (normalized.isEmpty())
        g_rigStore.remove(clipUid);
    else
        g_rigStore.insert(clipUid, normalized);
}

void removeRigForClip(const QString &clipUid)
{
    if (clipUid.isEmpty()) return;
    QMutexLocker lock(&g_storeMutex);
    g_rigStore.remove(clipUid);
}

void clearAllRigs()
{
    QMutexLocker lock(&g_storeMutex);
    g_rigStore.clear();
}

QHash<QString, PuppetRig> snapshotAllRigs()
{
    QMutexLocker lock(&g_storeMutex);
    return g_rigStore;
}

void restoreAllRigs(const QHash<QString, PuppetRig> &snap)
{
    QMutexLocker lock(&g_storeMutex);
    g_rigStore = snap;
}

void recordTimelineUndoSnapshot(const void *timelineKey, int undoIndex)
{
    if (!timelineKey || undoIndex < 0) return;
    QMutexLocker lock(&g_storeMutex);
    auto &history = g_undoSnapshots[timelineKey];
    // Prune redo entries above undoIndex.
    for (auto it = history.begin(); it != history.end(); ) {
        if (it.key() > undoIndex) it = history.erase(it);
        else ++it;
    }
    history.insert(undoIndex, g_rigStore);
}

void restoreTimelineUndoSnapshot(const void *timelineKey, int undoIndex)
{
    if (!timelineKey || undoIndex < 0) return;
    QMutexLocker lock(&g_storeMutex);
    const auto tlIt = g_undoSnapshots.constFind(timelineKey);
    if (tlIt == g_undoSnapshots.constEnd()) return;
    const auto idxIt = tlIt.value().constFind(undoIndex);
    if (idxIt == tlIt.value().constEnd()) return;
    g_rigStore = idxIt.value();
}

void clearTimelineUndoSnapshots(const void *timelineKey)
{
    if (!timelineKey) return;
    QMutexLocker lock(&g_storeMutex);
    g_undoSnapshots.remove(timelineKey);
}

bool commitRigToTimeline(Timeline *timeline,
                         int trackIdx,
                         int clipIdx,
                         const PuppetRig &rig,
                         const QString &undoLabel)
{
    if (!timeline) return false;
    const auto tracks = timeline->videoTracks();
    if (trackIdx < 0 || trackIdx >= tracks.size()) return false;
    TimelineTrack *track = tracks[trackIdx];
    if (!track || track->isLocked()) return false;
    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size()) return false;

    if (clips[clipIdx].uid.isEmpty())
        clips[clipIdx].uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString uid = clips[clipIdx].uid;

    const PuppetRig prevRig = rigForClip(uid);
    const QVector<WarpPin> legacyPins = toLegacyPreviewPins(rig, 0.0);
    if (prevRig == rig && puppetpin::equal(clips[clipIdx].puppetPins, legacyPins))
        return false;

    setRigForClip(uid, rig);
    clips[clipIdx].puppetPins = legacyPins;
    track->setClips(clips);
    timeline->saveUndoState(undoLabel.isEmpty() ? QStringLiteral("パペットアニメを編集") : undoLabel);
    timeline->refreshPlaybackSequence();
    return true;
}

void previewRigOnTimeline(Timeline *timeline,
                          int trackIdx,
                          int clipIdx,
                          const PuppetRig &rig,
                          double clipLocalSec)
{
    if (!timeline) return;
    const auto tracks = timeline->videoTracks();
    if (trackIdx < 0 || trackIdx >= tracks.size()) return;
    TimelineTrack *track = tracks[trackIdx];
    if (!track || track->isLocked()) return;
    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size()) return;

    if (clips[clipIdx].uid.isEmpty())
        clips[clipIdx].uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setRigForClip(clips[clipIdx].uid, rig);
    clips[clipIdx].puppetPins = toLegacyPreviewPins(rig, clipLocalSec);
    track->setClips(clips);
    timeline->refreshPlaybackSequence();
}

// ============================================================
//  PuppetRigDialog (AE-style Mesh & Keyframe Inspector)
// ============================================================

PuppetRigDialog::PuppetRigDialog(Timeline *timeline,
                                 int trackIdx,
                                 int clipIdx,
                                 double initialClipLocalSec,
                                 std::function<void()> onPreviewRefresh,
                                 QWidget *parent)
    : QDialog(parent)
    , m_timeline(timeline)
    , m_trackIdx(trackIdx)
    , m_clipIdx(clipIdx)
    , m_clipLocalSec(std::max(0.0, initialClipLocalSec))
    , m_onPreviewRefresh(std::move(onPreviewRefresh))
{
    setWindowTitle(QStringLiteral("パペットアニメ設定 (ARAP メッシュ / キーフレーム)"));
    setObjectName(QStringLiteral("puppetRigDialog"));
    resize(560, 520);

    if (m_timeline && m_trackIdx >= 0 && m_trackIdx < m_timeline->videoTracks().size()) {
        auto *track = m_timeline->videoTracks()[m_trackIdx];
        if (track && m_clipIdx >= 0 && m_clipIdx < track->clips().size()) {
            const auto &clip = track->clips()[m_clipIdx];
            if (!clip.uid.isEmpty() && hasRig(clip.uid))
                m_rig = rigForClip(clip.uid);
            else if (!clip.puppetPins.isEmpty())
                m_rig = rigFromLegacyPins(clip.puppetPins);
        }
    }

    auto *root = new QVBoxLayout(this);

    m_timeLabel = new QLabel(
        QStringLiteral("現在のクリップ内時刻: %1 秒（プレビューでドラッグすると現在時刻にオートキー）")
            .arg(m_clipLocalSec, 0, 'f', 2),
        this);
    root->addWidget(m_timeLabel);

    // Mesh & Tool Mode Group
    auto *meshGroup = new QGroupBox(QStringLiteral("ARAP 三角形メッシュ & ツールモード"), this);
    auto *meshForm = new QFormLayout(meshGroup);

    m_toolModeCombo = new QComboBox(this);
    m_toolModeCombo->setObjectName(QStringLiteral("puppetToolModeCombo"));
    for (int t = 0; t <= 4; ++t)
        m_toolModeCombo->addItem(pinTypeDisplayName(static_cast<PinType>(t)), t);
    meshForm->addRow(QStringLiteral("新規配置ピン種別"), m_toolModeCombo);

    m_densitySpin = new QSpinBox(this);
    m_densitySpin->setRange(6, 36);
    m_densitySpin->setValue(m_rig.mesh.density);
    meshForm->addRow(QStringLiteral("メッシュ密度 (分割数)"), m_densitySpin);

    m_expansionSpin = new QSpinBox(this);
    m_expansionSpin->setRange(0, 48);
    m_expansionSpin->setSuffix(QStringLiteral(" px"));
    m_expansionSpin->setValue(m_rig.mesh.expansionPx);
    meshForm->addRow(QStringLiteral("透過輪郭の余白拡張"), m_expansionSpin);

    m_alphaThreshSpin = new QSpinBox(this);
    m_alphaThreshSpin->setRange(1, 254);
    m_alphaThreshSpin->setValue(m_rig.mesh.alphaThreshold);
    meshForm->addRow(QStringLiteral("アルファ輪郭しきい値"), m_alphaThreshSpin);

    auto *checksRow = new QHBoxLayout();
    m_showMeshCheck = new QCheckBox(QStringLiteral("メッシュを表示"), this);
    m_showMeshCheck->setChecked(m_rig.mesh.showMesh);
    m_autoKeyCheck = new QCheckBox(QStringLiteral("ドラッグ時にオートキー"), this);
    m_autoKeyCheck->setChecked(m_rig.mesh.autoKey);
    checksRow->addWidget(m_showMeshCheck);
    checksRow->addWidget(m_autoKeyCheck);
    meshForm->addRow(checksRow);
    root->addWidget(meshGroup);

    // Pin list + inspector
    auto *midRow = new QHBoxLayout();

    auto *leftCol = new QVBoxLayout();
    m_pinList = new QListWidget(this);
    m_pinList->setObjectName(QStringLiteral("puppetPinList"));
    leftCol->addWidget(m_pinList);

    auto *pinBtnRow = new QHBoxLayout();
    auto *addPinBtn = new QPushButton(QStringLiteral("ピン追加"), this);
    auto *delPinBtn = new QPushButton(QStringLiteral("ピン削除"), this);
    pinBtnRow->addWidget(addPinBtn);
    pinBtnRow->addWidget(delPinBtn);
    leftCol->addLayout(pinBtnRow);
    midRow->addLayout(leftCol, 1);

    auto *pinGroup = new QGroupBox(QStringLiteral("選択ピンの現在時刻キーフレーム"), this);
    auto *pinForm = new QFormLayout(pinGroup);

    m_pinTypeCombo = new QComboBox(this);
    for (int t = 0; t <= 4; ++t)
        m_pinTypeCombo->addItem(pinTypeDisplayName(static_cast<PinType>(t)), t);
    pinForm->addRow(QStringLiteral("種別"), m_pinTypeCombo);

    m_posXSpin = new QDoubleSpinBox(this);
    m_posXSpin->setRange(-0.5, 1.5);
    m_posXSpin->setDecimals(4);
    m_posXSpin->setSingleStep(0.02);
    pinForm->addRow(QStringLiteral("位置 X (UV)"), m_posXSpin);

    m_posYSpin = new QDoubleSpinBox(this);
    m_posYSpin->setRange(-0.5, 1.5);
    m_posYSpin->setDecimals(4);
    m_posYSpin->setSingleStep(0.02);
    pinForm->addRow(QStringLiteral("位置 Y (UV)"), m_posYSpin);

    m_rotSpin = new QDoubleSpinBox(this);
    m_rotSpin->setRange(-360.0, 360.0);
    m_rotSpin->setSuffix(QStringLiteral(" °"));
    m_rotSpin->setSingleStep(5.0);
    pinForm->addRow(QStringLiteral("回転 (Bend/Adv)"), m_rotSpin);

    m_scaleSpin = new QDoubleSpinBox(this);
    m_scaleSpin->setRange(0.2, 3.0);
    m_scaleSpin->setDecimals(2);
    m_scaleSpin->setSingleStep(0.05);
    m_scaleSpin->setValue(1.0);
    pinForm->addRow(QStringLiteral("スケール (Bend/Adv)"), m_scaleSpin);

    m_radiusSpin = new QDoubleSpinBox(this);
    m_radiusSpin->setRange(0.02, 2.0);
    m_radiusSpin->setDecimals(3);
    m_radiusSpin->setSingleStep(0.05);
    pinForm->addRow(QStringLiteral("影響半径"), m_radiusSpin);

    m_stiffnessSpin = new QDoubleSpinBox(this);
    m_stiffnessSpin->setRange(0.0, 1.0);
    m_stiffnessSpin->setDecimals(2);
    m_stiffnessSpin->setSingleStep(0.1);
    pinForm->addRow(QStringLiteral("硬さ (Starch)"), m_stiffnessSpin);

    m_inFrontSpin = new QDoubleSpinBox(this);
    m_inFrontSpin->setRange(-100.0, 100.0);
    m_inFrontSpin->setDecimals(1);
    m_inFrontSpin->setSingleStep(10.0);
    pinForm->addRow(QStringLiteral("前後深度 (Overlap)"), m_inFrontSpin);

    m_easingCombo = new QComboBox(this);
    m_easingCombo->addItem(QStringLiteral("イーズインアウト"), static_cast<int>(EasingKind::EaseInOut));
    m_easingCombo->addItem(QStringLiteral("リニア"), static_cast<int>(EasingKind::Linear));
    m_easingCombo->addItem(QStringLiteral("イーズイン"), static_cast<int>(EasingKind::EaseIn));
    m_easingCombo->addItem(QStringLiteral("イーズアウト"), static_cast<int>(EasingKind::EaseOut));
    m_easingCombo->addItem(QStringLiteral("ホールド"), static_cast<int>(EasingKind::Hold));
    pinForm->addRow(QStringLiteral("イージング"), m_easingCombo);

    auto *kfBtnRow = new QHBoxLayout();
    auto *addKfBtn = new QPushButton(QStringLiteral("現在時刻にキー設定"), this);
    auto *delKfBtn = new QPushButton(QStringLiteral("現在時刻のキー削除"), this);
    kfBtnRow->addWidget(addKfBtn);
    kfBtnRow->addWidget(delKfBtn);
    pinForm->addRow(kfBtnRow);

    // Wave 32: Secondary motion (Wiggle / Expression) controls + 1-click presets
    auto *dynGroup = new QGroupBox(QStringLiteral("揺れ物自動アニメ (Wiggle / Expression)"), this);
    auto *dynForm = new QFormLayout(dynGroup);

    auto *presetRow = new QHBoxLayout();
    auto *pHairBtn = new QPushButton(QStringLiteral("髪・リボン"), this);
    auto *pBreathBtn = new QPushButton(QStringLiteral("呼吸"), this);
    auto *pWindBtn = new QPushButton(QStringLiteral("風"), this);
    auto *pClearDynBtn = new QPushButton(QStringLiteral("解除"), this);
    presetRow->addWidget(pHairBtn);
    presetRow->addWidget(pBreathBtn);
    presetRow->addWidget(pWindBtn);
    presetRow->addWidget(pClearDynBtn);
    dynForm->addRow(QStringLiteral("プリセット"), presetRow);

    m_wiggleCheck = new QCheckBox(QStringLiteral("Wiggle 有効"), this);
    dynForm->addRow(m_wiggleCheck);

    m_wiggleFreqSpin = new QDoubleSpinBox(this);
    m_wiggleFreqSpin->setRange(0.05, 20.0);
    m_wiggleFreqSpin->setDecimals(2);
    m_wiggleFreqSpin->setSuffix(QStringLiteral(" Hz"));
    m_wiggleFreqSpin->setValue(2.0);
    dynForm->addRow(QStringLiteral("周波数"), m_wiggleFreqSpin);

    m_wiggleAmpSpin = new QDoubleSpinBox(this);
    m_wiggleAmpSpin->setRange(0.0, 0.5);
    m_wiggleAmpSpin->setDecimals(3);
    m_wiggleAmpSpin->setSingleStep(0.005);
    m_wiggleAmpSpin->setValue(0.025);
    dynForm->addRow(QStringLiteral("振幅 (UV)"), m_wiggleAmpSpin);

    m_wiggleRotSpin = new QDoubleSpinBox(this);
    m_wiggleRotSpin->setRange(0.0, 90.0);
    m_wiggleRotSpin->setDecimals(1);
    m_wiggleRotSpin->setSuffix(QStringLiteral(" °"));
    dynForm->addRow(QStringLiteral("回転振幅"), m_wiggleRotSpin);

    m_wiggleSeedSpin = new QSpinBox(this);
    m_wiggleSeedSpin->setRange(1, 99999);
    m_wiggleSeedSpin->setValue(1);
    dynForm->addRow(QStringLiteral("乱数シード"), m_wiggleSeedSpin);

    m_exprXEdit = new QLineEdit(this);
    m_exprXEdit->setPlaceholderText(QStringLiteral("例: value + sin(time * 3) * 0.02"));
    dynForm->addRow(QStringLiteral("式 X"), m_exprXEdit);

    m_exprYEdit = new QLineEdit(this);
    m_exprYEdit->setPlaceholderText(QStringLiteral("例: value + sin(time * 2) * 0.015"));
    dynForm->addRow(QStringLiteral("式 Y"), m_exprYEdit);

    m_exprRotEdit = new QLineEdit(this);
    m_exprRotEdit->setPlaceholderText(QStringLiteral("例: value + sin(time * 4) * 10"));
    dynForm->addRow(QStringLiteral("式 回転°"), m_exprRotEdit);

    auto *rightCol = new QVBoxLayout();
    rightCol->addWidget(pinGroup);
    rightCol->addWidget(dynGroup);
    midRow->addLayout(rightCol, 2);
    root->addLayout(midRow, 1);

    auto applyPreset = [this](SwayPreset preset) {
        const int row = m_pinList->currentRow();
        if (row < 0 || row >= m_rig.pins.size()) return;
        applySwayPreset(m_rig.pins[row], preset);
        loadSelectedPinToEditor();
    };
    connect(pHairBtn,     &QPushButton::clicked, this, [applyPreset]() { applyPreset(SwayPreset::HairRibbon); });
    connect(pBreathBtn,   &QPushButton::clicked, this, [applyPreset]() { applyPreset(SwayPreset::Breathing); });
    connect(pWindBtn,     &QPushButton::clicked, this, [applyPreset]() { applyPreset(SwayPreset::WindGust); });
    connect(pClearDynBtn, &QPushButton::clicked, this, [applyPreset]() { applyPreset(SwayPreset::None); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("適用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("閉じる"));
    root->addWidget(buttons);

    connect(m_pinList, &QListWidget::currentRowChanged, this, [this](int) {
        loadSelectedPinToEditor();
    });
    connect(addPinBtn, &QPushButton::clicked, this, [this]() {
        PuppetPin pin;
        pin.id = m_rig.nextPinId++;
        pin.type = selectedToolMode();
        pin.name = QStringLiteral("ピン %1").arg(pin.id);
        const double offset = 0.08 * (m_rig.pins.size() % 5);
        pin.restPos = QPointF(std::clamp(0.35 + offset, 0.1, 0.9), 0.5);
        PinKeyframe k;
        k.timeSec = m_clipLocalSec;
        k.position = pin.restPos;
        pin.keyframes.append(k);
        m_rig.pins.append(pin);
        reloadUi();
        m_pinList->setCurrentRow(m_rig.pins.size() - 1);
    });
    connect(delPinBtn, &QPushButton::clicked, this, [this]() {
        const int row = m_pinList->currentRow();
        if (row < 0 || row >= m_rig.pins.size()) return;
        m_rig.pins.removeAt(row);
        reloadUi();
    });
    connect(addKfBtn, &QPushButton::clicked, this, [this]() {
        applyEditorToSelectedPin();
        reloadUi();
    });
    connect(delKfBtn, &QPushButton::clicked, this, [this]() {
        const int row = m_pinList->currentRow();
        if (row < 0 || row >= m_rig.pins.size()) return;
        if (removePinKeyframeNear(m_rig.pins[row], m_clipLocalSec, 0.05))
            reloadUi();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_rig.mesh.density        = m_densitySpin->value();
        m_rig.mesh.expansionPx    = m_expansionSpin->value();
        m_rig.mesh.alphaThreshold = m_alphaThreshSpin->value();
        m_rig.mesh.showMesh       = m_showMeshCheck->isChecked();
        m_rig.mesh.autoKey        = m_autoKeyCheck->isChecked();
        applyEditorToSelectedPin();
        commitRig(QStringLiteral("パペットアニメを編集"));
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    reloadUi();
}

PinType PuppetRigDialog::selectedToolMode() const
{
    return static_cast<PinType>(m_toolModeCombo ? m_toolModeCombo->currentData().toInt() : 0);
}

void PuppetRigDialog::reloadUi()
{
    m_updating = true;
    const int prevRow = m_pinList->currentRow();
    m_pinList->clear();
    for (const auto &pin : m_rig.pins) {
        m_pinList->addItem(QStringLiteral("#%1 [%2] (%3 keys)")
                               .arg(pin.id)
                               .arg(pinTypeName(pin.type))
                               .arg(pin.keyframes.size()));
    }
    if (!m_rig.pins.isEmpty())
        m_pinList->setCurrentRow(std::clamp(prevRow, 0, static_cast<int>(m_rig.pins.size()) - 1));
    m_updating = false;
    loadSelectedPinToEditor();
}

void PuppetRigDialog::loadSelectedPinToEditor()
{
    if (m_updating) return;
    const int row = m_pinList->currentRow();
    const bool valid = (row >= 0 && row < m_rig.pins.size());
    m_pinTypeCombo->setEnabled(valid);
    m_posXSpin->setEnabled(valid);
    m_posYSpin->setEnabled(valid);
    m_rotSpin->setEnabled(valid);
    m_scaleSpin->setEnabled(valid);
    m_radiusSpin->setEnabled(valid);
    m_stiffnessSpin->setEnabled(valid);
    m_inFrontSpin->setEnabled(valid);
    m_easingCombo->setEnabled(valid);
    m_wiggleCheck->setEnabled(valid);
    m_wiggleFreqSpin->setEnabled(valid);
    m_wiggleAmpSpin->setEnabled(valid);
    m_wiggleRotSpin->setEnabled(valid);
    m_wiggleSeedSpin->setEnabled(valid);
    m_exprXEdit->setEnabled(valid);
    m_exprYEdit->setEnabled(valid);
    m_exprRotEdit->setEnabled(valid);
    if (!valid) return;

    m_updating = true;
    const PuppetPin &pin = m_rig.pins[row];
    const EvaluatedPin ep = evaluatePinAt(pin, m_clipLocalSec);
    m_pinTypeCombo->setCurrentIndex(static_cast<int>(pin.type));
    m_posXSpin->setValue(ep.position.x());
    m_posYSpin->setValue(ep.position.y());
    m_rotSpin->setValue(ep.rotationDeg);
    m_scaleSpin->setValue(ep.scale);
    m_radiusSpin->setValue(pin.radius);
    m_stiffnessSpin->setValue(pin.stiffness);
    m_inFrontSpin->setValue(ep.inFront);
    EasingKind easing = EasingKind::EaseInOut;
    if (!pin.keyframes.isEmpty()) easing = pin.keyframes.last().easing;
    const int eIdx = m_easingCombo->findData(static_cast<int>(easing));
    if (eIdx >= 0) m_easingCombo->setCurrentIndex(eIdx);

    m_wiggleCheck->setChecked(pin.dynamics.wiggleEnabled);
    m_wiggleFreqSpin->setValue(pin.dynamics.wiggleFreqHz);
    m_wiggleAmpSpin->setValue(pin.dynamics.wiggleAmpUv);
    m_wiggleRotSpin->setValue(pin.dynamics.wiggleRotAmpDeg);
    m_wiggleSeedSpin->setValue(static_cast<int>(pin.dynamics.wiggleSeed));
    m_exprXEdit->setText(pin.dynamics.exprX);
    m_exprYEdit->setText(pin.dynamics.exprY);
    m_exprRotEdit->setText(pin.dynamics.exprRot);
    m_updating = false;
}

void PuppetRigDialog::applyEditorToSelectedPin()
{
    const int row = m_pinList->currentRow();
    if (row < 0 || row >= m_rig.pins.size()) return;
    PuppetPin &pin = m_rig.pins[row];
    pin.type      = static_cast<PinType>(m_pinTypeCombo->currentData().toInt());
    pin.radius    = m_radiusSpin->value();
    pin.stiffness = m_stiffnessSpin->value();

    pin.dynamics.wiggleEnabled   = m_wiggleCheck->isChecked();
    pin.dynamics.wiggleFreqHz    = m_wiggleFreqSpin->value();
    pin.dynamics.wiggleAmpUv     = m_wiggleAmpSpin->value();
    pin.dynamics.wiggleRotAmpDeg = m_wiggleRotSpin->value();
    pin.dynamics.wiggleSeed      = static_cast<unsigned int>(std::max(1, m_wiggleSeedSpin->value()));
    pin.dynamics.exprX           = m_exprXEdit->text().trimmed();
    pin.dynamics.exprY           = m_exprYEdit->text().trimmed();
    pin.dynamics.exprRot         = m_exprRotEdit->text().trimmed();

    PinKeyframe kf;
    kf.timeSec     = m_clipLocalSec;
    kf.position    = QPointF(m_posXSpin->value(), m_posYSpin->value());
    kf.rotationDeg = m_rotSpin->value();
    kf.scale       = m_scaleSpin->value();
    kf.inFront     = m_inFrontSpin->value();
    kf.easing      = static_cast<EasingKind>(m_easingCombo->currentData().toInt());
    upsertPinKeyframe(pin, kf);
}

void PuppetRigDialog::commitRig(const QString &undoLabel)
{
    commitRigToTimeline(m_timeline, m_trackIdx, m_clipIdx, m_rig, undoLabel);
    if (m_onPreviewRefresh)
        m_onPreviewRefresh();
}

} // namespace puppet
