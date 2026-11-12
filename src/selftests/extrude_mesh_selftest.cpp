#include "SelftestGate.h"
#include "../ExtrudedMesh.h"
#include "../ProjectFile.h"
#include "../ShapeLayer.h"
#include "../SoftRaster3D.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QColor>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

// US-2006: buildExtrudedMesh sanitises its parameters (finite-or-zero, depth
// 0..5, bevels 0..1, segments 0..8), drops the bevel when the middle profile
// would exceed 40000 steps or the profile times the contour vertices would
// exceed the 2000000-vertex budget, and the mesh cache key / MCP guard share the
// same limits. The glyph is "A" in Arial 72 (2 rings, 16 ring vertices).
namespace {

using mesh3d::ExtrudeParams;
using mesh3d::TriMesh;

constexpr qint64 kBuildLimitMs = 2000;
constexpr qint64 kRenderLimitMs = 5000;
constexpr int kW = 320;
constexpr int kH = 180;

ExtrudeParams EP(double depth, double bevelDepth, double bevelWidth, int segs)
{
    ExtrudeParams p;
    p.depth = depth;
    p.bevelDepth = bevelDepth;
    p.bevelWidth = bevelWidth;
    p.bevelSegments = segs;
    p.smoothCapNormals = true;
    return p;
}

QVector<QPolygonF> glyphA()
{
    return mesh3d::glyphContours(QStringLiteral("A"), QFont(QStringLiteral("Arial"), 72), 0.5);
}

int ringVertices(const QVector<QPolygonF> &contours)
{
    int n = 0;
    for (const QPolygonF &p : contours)
        n += p.size();
    return n;
}

// positions || normals || indices raw bytes (Step 0 shaMesh).
std::string shaMesh(const TriMesh &m)
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(QByteArrayView(reinterpret_cast<const char *>(m.positions.constData()),
                             m.positions.size() * qsizetype(sizeof(QVector3D))));
    h.addData(QByteArrayView(reinterpret_cast<const char *>(m.normals.constData()),
                             m.normals.size() * qsizetype(sizeof(QVector3D))));
    h.addData(QByteArrayView(reinterpret_cast<const char *>(m.indices.constData()),
                             m.indices.size() * qsizetype(sizeof(quint32))));
    return h.result().toHex().toStdString();
}

std::string shaImage(const QImage &img0)
{
    if (img0.isNull())
        return "null";
    const QImage img = img0.convertToFormat(QImage::Format_RGBA8888);
    QCryptographicHash h(QCryptographicHash::Sha256);
    for (int y = 0; y < img.height(); ++y)
        h.addData(QByteArrayView(reinterpret_cast<const char *>(img.constScanLine(y)), img.width() * 4));
    return h.result().toHex().toStdString();
}

double meanAbsDiff(const QImage &a0, const QImage &b0)
{
    if (a0.isNull() || b0.isNull() || a0.size() != b0.size())
        return -1.0;
    const QImage a = a0.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = b0.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        const uchar *pa = a.constScanLine(y);
        const uchar *pb = b.constScanLine(y);
        for (int x = 0; x < a.width() * 4; ++x)
            sum += std::abs(int(pa[x]) - int(pb[x]));
    }
    return sum / (double(a.width()) * a.height() * 4.0);
}

bool sameMesh(const TriMesh &a, const TriMesh &b)
{
    return a.positions == b.positions && a.normals == b.normals && a.indices == b.indices;
}

bool allFinite(const ExtrudeParams &p)
{
    return std::isfinite(p.depth) && std::isfinite(p.bevelDepth) && std::isfinite(p.bevelWidth);
}

std::string num(double v)
{
    return QString::number(v, 'g', 12).toStdString();
}

std::string paramsText(const ExtrudeParams &p)
{
    return num(p.depth) + "/" + num(p.bevelDepth) + "/" + num(p.bevelWidth) + "/" + std::to_string(p.bevelSegments);
}

quint64 workingSetMB(bool peak)
{
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return 0;
    return quint64(peak ? pmc.PeakWorkingSetSize : pmc.WorkingSetSize) / (1024 * 1024);
}

struct Built {
    TriMesh mesh;
    bool changed = false;
    qint64 ms = 0;
};

Built build(const QVector<QPolygonF> &contours, const ExtrudeParams &p)
{
    Built b;
    QElapsedTimer t;
    t.start();
    b.mesh = mesh3d::buildExtrudedMesh(contours, p, &b.changed);
    b.ms = t.elapsed();
    return b;
}

// Profile step count by the builder's formula: int((midEnd - midZ) / dz) + 2 * (segs + 1).
int profileSteps(const ExtrudeParams &p)
{
    const int segs = p.bevelSegments;
    const double dz = p.depth / (2.0 * segs + 1.0);
    const double midZ = -p.bevelDepth;
    const double midEnd = -p.depth + p.bevelDepth;
    const int mid = midEnd > midZ + 1e-6 ? (std::max)(1, int((midEnd - midZ) / dz)) : 0;
    return mid + 2 * (segs + 1);
}

// V1 = full-frame dark shape clip (2 s) with optional text3D, through
// toJsonString / fromJsonString into a Timeline, rendered at 0.5 s.
struct Rendered {
    QImage img;
    qint64 ms = 0;
    bool parsed = false;
};

Rendered renderProject(const QJsonObject &text3D)
{
    Shape rect;
    rect.type = ShapeType::Rectangle;
    rect.position = QPointF(kW / 2.0, kH / 2.0);
    rect.properties.size = QSizeF(kW, kH);
    rect.fill.color = QColor(30, 30, 40);
    rect.stroke.enabled = false;
    ClipInfo clip;
    clip.displayName = QStringLiteral("extrude");
    clip.duration = clip.outPoint = 2.0;
    clip.shapes = {rect};
    clip.text3DConfig = text3D;
    ProjectData d;
    d.videoTracks = {{clip}};
    d.audioTracks = {{}};
    ProjectData r;
    Rendered out;
    out.parsed = ProjectFile::fromJsonString(ProjectFile::toJsonString(d), r);
    Timeline tl;
    tl.setProjectOutputConfig(kW, kH, false, 10);
    tl.restoreFromProject(r.videoTracks, r.audioTracks, 0.0, -1.0, -1.0, 100);
    QElapsedTimer t;
    t.start();
    out.img = tlrender::renderFrameAt(&tl, qRound64(0.5 * 1000000.0), QSize(kW, kH));
    out.ms = t.elapsed();
    return out;
}

QJsonObject text3DConfig(double depth, double bevelDepth, int segs)
{
    return QJsonObject{{QStringLiteral("extrudeEnabled"), true},
                       {QStringLiteral("text"), QStringLiteral("A")},
                       {QStringLiteral("extrudeDepth"), depth},
                       {QStringLiteral("extrudeBevelDepth"), bevelDepth},
                       {QStringLiteral("extrudeBevelSegments"), segs}};
}

QStringList *g_g7Warnings = nullptr;
QtMessageHandler g_g7PreviousHandler = nullptr;

void captureG7Warning(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    if (type == QtWarningMsg && g_g7Warnings && msg.contains(QStringLiteral("buildExtrudedMesh")))
        g_g7Warnings->append(msg);
}

} // namespace

int runExtrudeMeshSelftest()
{
    selftest::GateTally tally;
    const QVector<QPolygonF> cA = glyphA();
    const int vA = ringVertices(cA);
    const QFont arial(QStringLiteral("Arial"), 72);
    const double kNaN = std::numeric_limits<double>::quiet_NaN();
    const double kInf = std::numeric_limits<double>::infinity();

    // G1: depth 0 with an active bevel (HEAD: inf step count -> UB / hang) builds the no-bevel mesh.
    {
        const ExtrudeParams s = mesh3d::sanitizeExtrudeParams(EP(0.0, 0.1, 0.1, 8));
        const Built hostile = build(cA, EP(0.0, 0.1, 0.1, 8));
        const Built flat = build(cA, EP(0.0, 0.1, 0.1, 0));
        const bool ok = !cA.isEmpty() && hostile.ms <= kBuildLimitMs && s.bevelSegments == 0
            && hostile.mesh.positions.size() == flat.mesh.positions.size() && hostile.changed;
        tally.gate(1, ok, "depth 0 / bevel 0.1 / segs 8: ms=" + std::to_string(hostile.ms)
                              + " verts=" + std::to_string(hostile.mesh.positions.size())
                              + " segs0 verts=" + std::to_string(flat.mesh.positions.size())
                              + " changed=" + std::to_string(hostile.changed) + " sanitized=" + paramsText(s)
                              + " rings=" + std::to_string(cA.size())
                              + " vA=" + std::to_string(vA));
    }

    // G2: depth 1e-7 with bevel 0.1 / 8 segments (HEAD: ~3.4e7 middle steps) builds the no-bevel mesh.
    {
        // The step limit (not the vertex budget) drops the bevel: the sanitised segments are 0.
        const ExtrudeParams s = mesh3d::sanitizeExtrudeParams(EP(1e-7, 0.1, 0.02, 8));
        const Built hostile = build(cA, EP(1e-7, 0.1, 0.02, 8));
        const Built flat = build(cA, EP(1e-7, 0.1, 0.02, 0));
        const bool ok = hostile.ms <= kBuildLimitMs && s.bevelSegments == 0
            && hostile.mesh.positions.size() == flat.mesh.positions.size() && hostile.changed;
        tally.gate(2, ok, "depth 1e-7 / bevel 0.1 / segs 8: ms=" + std::to_string(hostile.ms)
                              + " verts=" + std::to_string(hostile.mesh.positions.size())
                              + " segs0 verts=" + std::to_string(flat.mesh.positions.size())
                              + " changed=" + std::to_string(hostile.changed) + " sanitized=" + paramsText(s)
                              + " midSteps(raw)=" + num(mesh3d::extrudeMidSteps(EP(1e-7, 0.1, 0.02, 8))));
    }

    // G3: out-of-range values build the same mesh as their clamped values; the sanitised values are finite.
    {
        struct Case {
            const char *name;
            ExtrudeParams raw;
            ExtrudeParams clamped;
        };
        const ExtrudeParams def;
        const Case cases[] = {
            {"segs 1e9 -> 8", EP(def.depth, def.bevelDepth, def.bevelWidth, 1000000000),
             EP(def.depth, def.bevelDepth, def.bevelWidth, 8)},
            {"segs -5 -> 0", EP(def.depth, def.bevelDepth, def.bevelWidth, -5),
             EP(def.depth, def.bevelDepth, def.bevelWidth, 0)},
            {"depth NaN -> 0", EP(kNaN, def.bevelDepth, def.bevelWidth, def.bevelSegments),
             EP(0.0, def.bevelDepth, def.bevelWidth, def.bevelSegments)},
            {"bevelWidth inf -> 0", EP(def.depth, def.bevelDepth, kInf, def.bevelSegments),
             EP(def.depth, def.bevelDepth, 0.0, def.bevelSegments)},
            {"bevelDepth -inf -> 0", EP(def.depth, -kInf, def.bevelWidth, def.bevelSegments),
             EP(def.depth, 0.0, def.bevelWidth, def.bevelSegments)},
            {"depth 7 -> 5", EP(7.0, def.bevelDepth, def.bevelWidth, def.bevelSegments),
             EP(5.0, def.bevelDepth, def.bevelWidth, def.bevelSegments)},
        };
        bool ok = true;
        std::string detail;
        for (const Case &c : cases) {
            bool sanChanged = false;
            const ExtrudeParams s = mesh3d::sanitizeExtrudeParams(c.raw, &sanChanged);
            const Built raw = build(cA, c.raw);
            const Built clamped = build(cA, c.clamped);
            const bool caseOk = sanChanged && raw.changed && allFinite(s)
                && raw.mesh.positions.size() == clamped.mesh.positions.size() && sameMesh(raw.mesh, clamped.mesh);
            ok = ok && caseOk;
            detail += std::string(" [") + c.name + ": sanitized=" + paramsText(s)
                + " verts=" + std::to_string(raw.mesh.positions.size()) + "/"
                + std::to_string(clamped.mesh.positions.size()) + " same=" + std::to_string(sameMesh(raw.mesh, clamped.mesh))
                + " changed=" + std::to_string(sanChanged) + "/" + std::to_string(raw.changed)
                + " finite=" + std::to_string(allFinite(s)) + "]";
        }
        tally.gate(3, ok, "range" + detail);
    }

    // G4: in-range settings are unchanged (sha compared with the base values in the raw log), and the
    // cache key is built from the sanitised values.
    {
        struct Case {
            const char *name;
            ExtrudeParams p;
        };
        const Case cases[] = {
            {"default", EP(0.2, 0.02, 0.02, 2)},
            {"mograph", EP(1.0, 0.05, 0.05, 2)},
            {"boundary", EP(5.0, 1.0, 1.0, 8)},
        };
        bool ok = true;
        std::string detail;
        for (const Case &c : cases) {
            bool sanChanged = true;
            const ExtrudeParams s = mesh3d::sanitizeExtrudeParams(c.p, &sanChanged);
            const Built b = build(cA, c.p);
            const double steps = mesh3d::extrudeMidSteps(c.p);
            const bool keySame = mesh3d::extrudeMeshCacheKey(QStringLiteral("A"), arial, c.p)
                == mesh3d::extrudeMeshCacheKey(QStringLiteral("A"), arial, s);
            ok = ok && !sanChanged && !b.changed && steps <= mesh3d::kMaxExtrudeMidSteps && keySame;
            detail += std::string(" [") + c.name + " " + paramsText(c.p) + ": changed=" + std::to_string(sanChanged)
                + "/" + std::to_string(b.changed) + " midSteps=" + num(steps) + " keySame=" + std::to_string(keySame)
                + " verts=" + std::to_string(b.mesh.positions.size()) + " sha=" + shaMesh(b.mesh) + "]";
        }
        // NaN depth: the key is the same string on every build and equals the depth-0 key.
        const ExtrudeParams nanDepth = EP(kNaN, 0.02, 0.02, 2);
        const QString k1 = mesh3d::extrudeMeshCacheKey(QStringLiteral("A"), arial, mesh3d::sanitizeExtrudeParams(nanDepth));
        const QString k2 = mesh3d::extrudeMeshCacheKey(QStringLiteral("A"), arial, mesh3d::sanitizeExtrudeParams(nanDepth));
        const QString k0 = mesh3d::extrudeMeshCacheKey(QStringLiteral("A"), arial,
                                                       mesh3d::sanitizeExtrudeParams(EP(0.0, 0.02, 0.02, 2)));
        const bool nanKeyOk = k1 == k2 && k1 == k0 && !k1.contains(QStringLiteral("nan"), Qt::CaseInsensitive);
        ok = ok && nanKeyOk;
        detail += " [NaN depth key stable=" + std::to_string(k1 == k2) + " ==depth0=" + std::to_string(k1 == k0)
            + " key=" + k1.section(QLatin1Char('|'), 2).toStdString() + "]";
        tally.gate(4, ok, "in range" + detail);
    }

    // G5: a hostile saved project (depth 0 / 8 segments, then 1000000 segments) renders in time;
    // the in-range render sha is compared with the base value in the raw log.
    {
        const Rendered h1 = renderProject(text3DConfig(0.0, 0.1, 8));
        const Rendered h2 = renderProject(text3DConfig(0.0, 0.1, 1000000));
        const Rendered inRange = renderProject(text3DConfig(0.2, 0.1, 2));
        const Rendered inRange2 = renderProject(text3DConfig(0.2, 0.1, 2));
        const Rendered plain = renderProject(QJsonObject());
        const bool ok = h1.parsed && h2.parsed && inRange.parsed
            && !h1.img.isNull() && h1.ms <= kRenderLimitMs
            && !h2.img.isNull() && h2.ms <= kRenderLimitMs
            && !inRange.img.isNull();
        tally.gate(5, ok, "hostile depth0/segs8 ms=" + std::to_string(h1.ms) + " null=" + std::to_string(h1.img.isNull())
                              + " ; depth0/segs1e6 ms=" + std::to_string(h2.ms) + " null=" + std::to_string(h2.img.isNull())
                              + " ; in-range format=" + std::to_string(int(inRange.img.format()))
                              + " size=" + std::to_string(inRange.img.width()) + "x" + std::to_string(inRange.img.height())
                              + " shaRGBA=" + shaImage(inRange.img) + " run2=" + shaImage(inRange2.img)
                              + " meanAbsDiff(vs no text3D)=" + num(meanAbsDiff(inRange.img, plain.img)));
    }

    // G6: extrudeMidSteps matches the MCP formula.
    {
        const double a = mesh3d::extrudeMidSteps(EP(0.0, 0.1, 0.1, 0));
        const double b = mesh3d::extrudeMidSteps(EP(1.0, 0.1, 0.1, 8));
        const double c = mesh3d::extrudeMidSteps(EP(0.1, 0.1, 0.1, 2));
        const double d = mesh3d::extrudeMidSteps(EP(0.0, 0.1, 0.1, 1));
        const bool ok = a == 0.0 && b == 0.0 && std::abs(c - 5.0) < 1e-9 && std::isinf(d) && d > 0.0;
        tally.gate(6, ok, "segs0/depth0 -> " + num(a) + " ; segs8/depth1 -> " + num(b) + " ; segs2/depth0.1 -> "
                              + num(c) + " ; segs1/depth0 -> " + num(d));
    }

    // G7: vertex budget. N glyphs of "A" at depth 8.5e-4 / bevel 1.0 / 8 segments exceed 2e6 profile
    // vertices while staying under the step limit. The builder reduces the segment count one step at a
    // time to the largest count that fits (N = 4 -> 6, not the halving 4), and drops the bevel only when
    // one segment does not fit (N = 18).
    {
        const ExtrudeParams p = EP(8.5e-4, 1.0, 1.0, 8);
        const auto glyphs = [&](int n) {
            QVector<QPolygonF> c;
            for (int i = 0; i < n; ++i)
                c += cA;
            return c;
        };
        // Expected result from the profileSteps formula alone: the largest s in 1..requested whose
        // layers fit the budget, or 0 (no bevel) when s = 1 does not fit.
        const auto expectedSegs = [&](int n) {
            for (int s = p.bevelSegments; s >= 1; --s)
                if (qint64(profileSteps(EP(8.5e-4, 1.0, 1.0, s))) * n * vA <= mesh3d::kMaxExtrudeVertices)
                    return s;
            return 0;
        };
        const int P = profileSteps(p);
        const quint64 wsBefore = workingSetMB(false);

        // (a) N = 4: 8 does not fit, the largest that fits is 6; the mesh equals one built at 6 requested.
        const int nA = 4;
        const QVector<QPolygonF> cNa = glyphs(nA);
        const int kA = expectedSegs(nA);
        QStringList warnings;
        g_g7Warnings = &warnings;
        g_g7PreviousHandler = qInstallMessageHandler(captureG7Warning);
        const Built over = build(cNa, p);
        qInstallMessageHandler(g_g7PreviousHandler);
        g_g7Warnings = nullptr;
        const Built atK = build(cNa, EP(8.5e-4, 1.0, 1.0, 6));
        const QString expectedNote = QStringLiteral("bevel segments 8 -> 6");
        const bool warnOk = warnings.size() == 1 && warnings.first().contains(expectedNote);

        // (b) N = 18: one segment does not fit either -> no bevel (HEAD behaviour).
        const int nB = 18;
        const QVector<QPolygonF> cNb = glyphs(nB);
        const int kB = expectedSegs(nB);
        QStringList warningsB;
        g_g7Warnings = &warningsB;
        g_g7PreviousHandler = qInstallMessageHandler(captureG7Warning);
        const Built none = build(cNb, p);
        qInstallMessageHandler(g_g7PreviousHandler);
        g_g7Warnings = nullptr;
        const Built flatB = build(cNb, EP(8.5e-4, 1.0, 1.0, 0));
        const bool warnBOk = warningsB.size() == 1 && warningsB.first().contains(QStringLiteral("(no bevel)"));

        // (c) within the budget nothing changes (the 1-glyph default and the 1-glyph 8-segment case).
        const Built control = build(cA, ExtrudeParams());
        const Built inBudget = build(cA, p);
        const Built inBudgetSame = build(cA, p);

        // (d) every leg stays inside the vertex budget, derived from the mesh itself: the builder's metric
        // (profile.size() * totalContourVertices) is layers * n * vA, and each layer carries n * (vA +
        // one centroid per ring) mesh positions, so layers = positions / perLayer.
        const auto meshWithinBudget = [&](const Built &b, int n) {
            const qint64 perLayer = qint64(n) * (vA + cA.size());
            const qint64 positions = b.mesh.positions.size();
            return positions % perLayer == 0 && (positions / perLayer) * n * vA <= mesh3d::kMaxExtrudeVertices;
        };
        const bool budgetOk = meshWithinBudget(over, nA) && meshWithinBudget(none, nB);

        // (e) the N = 4 mesh rendered through the soft rasteriser stays inside the render time limit
        // together with its build (Text3DLayer render parameters, fixed yaw / pitch).
        softras::RenderParams rp;
        rp.background = QColor(0, 0, 0, 0);
        rp.wireframe = false;
        QElapsedTimer rasterTimer;
        rasterTimer.start();
        const QImage rasterImg = softras::renderMeshAuto(over.mesh, QSize(kW, kH), 0.0, 0.0, 3.0, rp);
        const qint64 rasterMs = rasterTimer.elapsed();
        const bool rasterOk = !rasterImg.isNull() && over.ms + rasterMs <= kRenderLimitMs;

        const qint64 totalA = qint64(P) * nA * vA;
        const qint64 totalB = qint64(P) * nB * vA;
        const bool ok = totalA > mesh3d::kMaxExtrudeVertices && totalB > mesh3d::kMaxExtrudeVertices
            && mesh3d::extrudeMidSteps(p) <= mesh3d::kMaxExtrudeMidSteps
            && kA == 6 && kB == 0
            && sameMesh(over.mesh, atK.mesh) && over.mesh.positions.size() > flatB.mesh.positions.size()
            && over.changed && warnOk
            && sameMesh(none.mesh, flatB.mesh) && none.changed && warnBOk
            && !control.changed && !inBudget.changed && sameMesh(inBudget.mesh, inBudgetSame.mesh)
            && budgetOk && rasterOk
            && over.ms <= kRenderLimitMs && none.ms <= kRenderLimitMs;
        tally.gate(7, ok, "vA=" + std::to_string(vA) + " P(8)=" + std::to_string(P)
                              + " midSteps=" + num(mesh3d::extrudeMidSteps(p))
                              + " ; (a) N=" + std::to_string(nA) + " P*N*vA=" + std::to_string(totalA)
                              + " expectedSegs=" + std::to_string(kA) + " verts=" + std::to_string(over.mesh.positions.size())
                              + " atSegs6 verts=" + std::to_string(atK.mesh.positions.size())
                              + " sameMesh=" + std::to_string(sameMesh(over.mesh, atK.mesh))
                              + " changed=" + std::to_string(over.changed) + " warnNote=" + std::to_string(warnOk)
                              + " ms=" + std::to_string(over.ms)
                              + " ; (b) N=" + std::to_string(nB) + " P*N*vA=" + std::to_string(totalB)
                              + " expectedSegs=" + std::to_string(kB) + " verts=" + std::to_string(none.mesh.positions.size())
                              + " segs0 verts=" + std::to_string(flatB.mesh.positions.size())
                              + " sameMesh=" + std::to_string(sameMesh(none.mesh, flatB.mesh))
                              + " changed=" + std::to_string(none.changed) + " warnNoBevel=" + std::to_string(warnBOk)
                              + " ms=" + std::to_string(none.ms)
                              + " ; (c) control changed=" + std::to_string(control.changed)
                              + " verts=" + std::to_string(control.mesh.positions.size())
                              + " 1-glyph segs8 changed=" + std::to_string(inBudget.changed)
                              + " verts=" + std::to_string(inBudget.mesh.positions.size())
                              + " ; (d) budgetOk=" + std::to_string(budgetOk)
                              + " ; (e) N=4 k=6 rasterMs=" + std::to_string(rasterMs) + " buildMs=" + std::to_string(over.ms)
                              + " total=" + std::to_string(over.ms + rasterMs) + " null=" + std::to_string(rasterImg.isNull())
                              + " ; workingSetMB before=" + std::to_string(wsBefore)
                              + " after=" + std::to_string(workingSetMB(false))
                              + " peak=" + std::to_string(workingSetMB(true)));
    }

    return tally.finish("extrude-mesh");
}
