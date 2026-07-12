#include "ExtrudedMesh.h"

#include <QPainterPath>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>

namespace mesh3d {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline QVector3D normalisedOrZero(const QVector3D &v)
{
    const float len = v.length();
    if (len < 1e-6f) return QVector3D(0, 0, 0);
    return v / len;
}

static inline bool isFiniteVec(const QVector3D &v)
{
    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

/* Face normal from three vertices (CCW winding => normal points toward viewer). */
static QVector3D faceNormal(const QVector3D &a, const QVector3D &b, const QVector3D &c)
{
    const QVector3D u = b - a;
    const QVector3D v = c - a;
    return QVector3D::normal(u, v);
}

/* Cross product of two edge vectors (unnormalised). */
static QVector3D crossEdge(const QVector3D &a, const QVector3D &b, const QVector3D &c)
{
    return QVector3D::crossProduct(b - a, c - a);
}

// ---------------------------------------------------------------------------
// glyphContours
// ---------------------------------------------------------------------------

QVector<QPolygonF> glyphContours(const QString &text, const QFont &font, double flatness)
{
    QVector<QPolygonF> result;

    QPainterPath path;
    path.addText(0, 0, font, text);

    Q_UNUSED(flatness);
    const auto subpaths = path.toSubpathPolygons(QTransform());
    for (const QPolygonF &poly : subpaths) {
        if (poly.size() < 3) continue;
        result.append(poly);
    }

    if (result.isEmpty()) return result;

    // Compute bounding box across all polygons
    qreal minX = std::numeric_limits<qreal>::infinity();
    qreal maxX = -std::numeric_limits<qreal>::infinity();
    qreal minY = std::numeric_limits<qreal>::infinity();
    qreal maxY = -std::numeric_limits<qreal>::infinity();

    for (const auto &poly : result) {
        for (const auto &pt : poly) {
            if (pt.x() < minX) minX = pt.x();
            if (pt.x() > maxX) maxX = pt.x();
            if (pt.y() < minY) minY = pt.y();
            if (pt.y() > maxY) maxY = pt.y();
        }
    }

    const double cx = (minX + maxX) * 0.5;
    const double cy = (minY + maxY) * 0.5;
    const double h  = maxY - minY;

    // Translate to centre, then scale so max height = 1.0
    const double scale = (h > 1e-6) ? (1.0 / h) : 1.0;

    for (auto &poly : result) {
        for (auto &pt : poly) {
            pt.setX((pt.x() - cx) * scale);
            pt.setY((pt.y() - cy) * scale);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// triangulateRing — centroid-fan
// ---------------------------------------------------------------------------

QVector<quint32> triangulateRing(const QPolygonF &ring)
{
    QVector<quint32> indices;
    const int n = ring.size();
    if (n < 3) return indices;

    // Centroid
    double cx = 0, cy = 0;
    for (int i = 0; i < n; ++i) {
        cx += ring[i].x();
        cy += ring[i].y();
    }
    cx /= n;
    cy /= n;

    const quint32 centroidIdx = static_cast<quint32>(n); // virtual index after ring

    // Fan from centroid: triangles (centroid, i, i+1)
    for (int i = 0; i < n; ++i) {
        indices.append(centroidIdx);
        indices.append(static_cast<quint32>(i));
        indices.append(static_cast<quint32>((i + 1) % n));
    }

    return indices;
}

// ---------------------------------------------------------------------------
// buildExtrudedMesh
// ---------------------------------------------------------------------------

TriMesh buildExtrudedMesh(const QVector<QPolygonF> &contours, const ExtrudeParams &params)
{
    TriMesh mesh;

    if (contours.isEmpty()) return mesh;

    // -----------------------------------------------------------------------
    // Phase 1: collect all ring vertices and their triangulation indices.
    // We store per-ring data so we can build caps and walls.
    // -----------------------------------------------------------------------
    struct RingData {
        QVector<QVector3D> capVerts;      // XY at z=0
        QVector<quint32>   triIndices;    // flat triplets into capVerts + centroid
        quint32            vertOffset;    // offset into global position array
        quint32            centroidIdx;   // global index of this ring's centroid
    };

    QVector<RingData> rings;
    quint32 globalVertOffset = 0;

    for (const auto &contour : contours) {
        if (contour.size() < 3) continue;

        RingData rd;
        const int n = contour.size();

        // Build cap vertices at z=0
        for (int i = 0; i < n; ++i) {
            rd.capVerts.append(QVector3D(
                static_cast<float>(contour[i].x()),
                static_cast<float>(contour[i].y()),
                0.0f
            ));
        }

        // Centroid vertex
        double cx = 0, cy = 0;
        for (int i = 0; i < n; ++i) {
            cx += contour[i].x();
            cy += contour[i].y();
        }
        cx /= n;
        cy /= n;
        rd.capVerts.append(QVector3D(static_cast<float>(cx), static_cast<float>(cy), 0.0f));

        rd.triIndices = triangulateRing(contour);
        rd.vertOffset = globalVertOffset;
        rd.centroidIdx = globalVertOffset + static_cast<quint32>(n);

        rings.append(rd);
        globalVertOffset += static_cast<quint32>(n + 1); // ring + centroid
    }

    if (rings.isEmpty()) return mesh;

    // -----------------------------------------------------------------------
    // Phase 2: build bevel profile.
    // A bevel profile is a list of (inset, z) pairs from front (0,0) to back.
    // When bevelSegments==0: profile = [(0,0), (0,-depth)] — no bevel.
    // When bevelSegments>=1: profile has steps from front edge to back edge.
    // -----------------------------------------------------------------------
    struct BevelStep {
        double inset; // how far to shrink the XY contour (0 = original)
        double z;     // z coordinate
    };

    QVector<BevelStep> profile;

    if (params.bevelSegments <= 0 || params.bevelDepth <= 0.0) {
        // No bevel: just front and back
        profile.append({0.0, 0.0});
        profile.append({0.0, -params.depth});
    } else {
        const int segs = params.bevelSegments;
        const double dz = params.depth / (segs * 2 + 1);
        // Front bevel: from z=0 to z=-bevelDepth (inset grows)
        for (int i = 0; i <= segs; ++i) {
            const double t = static_cast<double>(i) / segs;
            profile.append({params.bevelWidth * t, -params.bevelDepth * t});
        }
        // Middle: straight extrusion
        const double midZ = -params.bevelDepth;
        const double midEnd = -params.depth + params.bevelDepth;
        if (midEnd > midZ + 1e-6) {
            const int midSteps = std::max(1, static_cast<int>((midEnd - midZ) / dz));
            for (int i = 1; i <= midSteps; ++i) {
                const double t = static_cast<double>(i) / midSteps;
                profile.append({params.bevelWidth, midZ + (midEnd - midZ) * t});
            }
        }
        // Back bevel: from z=-depth+bevelDepth to z=-depth (inset shrinks)
        const double backStart = -params.depth + params.bevelDepth;
        for (int i = 0; i <= segs; ++i) {
            const double t = static_cast<double>(i) / segs;
            profile.append({params.bevelWidth * (1.0 - t), backStart - params.bevelDepth * t});
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3: generate vertices for each ring at each profile step.
    // Layout: for each profile step s, for each ring r, for each vertex v in r:
    //   position = (ringVert * (1 - inset_fraction)) + centroid * inset_fraction
    // Actually simpler: shrink toward centroid by inset amount.
    // -----------------------------------------------------------------------
    struct ProfileLayer {
        QVector<QVector3D> verts;   // all ring vertices at this z level
        QVector<quint32>   centroids; // centroid index per ring
    };

    QVector<ProfileLayer> layers;

    for (const auto &step : profile) {
        ProfileLayer layer;
        for (const auto &rd : rings) {
            const int n = rd.capVerts.size() - 1; // exclude centroid
            const QVector3D &centroid = rd.capVerts[n];

            for (int i = 0; i < n; ++i) {
                const QVector3D &orig = rd.capVerts[i];
                // Shrink toward centroid by step.inset
                const QVector3D dir = centroid - orig;
                const float dirLen = dir.length();
                QVector3D pos;
                if (dirLen > 1e-6f) {
                    const float shrink = static_cast<float>(step.inset);
                    pos = orig + dir * (shrink / dirLen);
                } else {
                    pos = orig;
                }
                pos.setZ(static_cast<float>(step.z));
                layer.verts.append(pos);
            }
            // Centroid at this layer's z
            QVector3D c = centroid;
            c.setZ(static_cast<float>(step.z));
            layer.centroids.append(static_cast<quint32>(layer.verts.size()));
            layer.verts.append(c);
        }
        layers.append(layer);
    }

    // -----------------------------------------------------------------------
    // Phase 4: build global position array and index mappings.
    // -----------------------------------------------------------------------
    // Global vertex index = layerIndex * vertsPerLayer + localIndex
    // But vertsPerLayer varies, so we track offsets.
    QVector<quint32> layerOffsets;
    quint32 totalVerts = 0;
    for (const auto &layer : layers) {
        layerOffsets.append(totalVerts);
        totalVerts += static_cast<quint32>(layer.verts.size());
    }

    mesh.positions.reserve(totalVerts);
    for (const auto &layer : layers) {
        mesh.positions.append(layer.verts);
    }

    // -----------------------------------------------------------------------
    // Phase 5: front cap triangles (layer 0, CCW from +Z side).
    // -----------------------------------------------------------------------
    {
        const quint32 base = layerOffsets[0];
        for (const auto &rd : rings) {
            for (int t = 0; t < rd.triIndices.size(); t += 3) {
                quint32 i0 = rd.triIndices[t];
                quint32 i1 = rd.triIndices[t + 1];
                quint32 i2 = rd.triIndices[t + 2];
                // Convert to global
                mesh.indices.append(base + rd.vertOffset + i0);
                mesh.indices.append(base + rd.vertOffset + i1);
                mesh.indices.append(base + rd.vertOffset + i2);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 6: back cap triangles (last layer, reversed winding for -Z normal).
    // -----------------------------------------------------------------------
    {
        const int lastLayerIdx = layers.size() - 1;
        const quint32 base = layerOffsets[lastLayerIdx];
        for (const auto &rd : rings) {
            for (int t = 0; t < rd.triIndices.size(); t += 3) {
                quint32 i0 = rd.triIndices[t];
                quint32 i1 = rd.triIndices[t + 1];
                quint32 i2 = rd.triIndices[t + 2];
                // Reverse winding: swap i1 and i2
                mesh.indices.append(base + rd.vertOffset + i0);
                mesh.indices.append(base + rd.vertOffset + i2);
                mesh.indices.append(base + rd.vertOffset + i1);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 7: side walls between consecutive profile layers.
    // For each ring, for each edge (v[i], v[i+1]), create two triangles
    // connecting layer L to layer L+1.
    // -----------------------------------------------------------------------
    for (int L = 0; L < layers.size() - 1; ++L) {
        const quint32 baseA = layerOffsets[L];
        const quint32 baseB = layerOffsets[L + 1];

        quint32 ringOffsetA = 0;
        quint32 ringOffsetB = 0;

        for (int r = 0; r < rings.size(); ++r) {
            const int n = rings[r].capVerts.size() - 1; // ring vertices (no centroid)

            for (int i = 0; i < n; ++i) {
                const int next = (i + 1) % n;
                const quint32 a0 = baseA + ringOffsetA + i;
                const quint32 a1 = baseA + ringOffsetA + next;
                const quint32 b0 = baseB + ringOffsetB + i;
                const quint32 b1 = baseB + ringOffsetB + next;

                // Two triangles: (a0, b0, a1) and (a1, b0, b1)
                mesh.indices.append(a0);
                mesh.indices.append(b0);
                mesh.indices.append(a1);

                mesh.indices.append(a1);
                mesh.indices.append(b0);
                mesh.indices.append(b1);
            }

            ringOffsetA += static_cast<quint32>(n + 1); // +1 for centroid
            ringOffsetB += static_cast<quint32>(n + 1);
        }
    }

    // -----------------------------------------------------------------------
    // Phase 8: compute normals.
    // When smoothCapNormals=true: accumulate face normals per vertex for all
    //   triangles, then normalise (smooth caps + walls).
    // When smoothCapNormals=false: cap vertices get flat per-face normals
    //   (+Z for front cap, -Z for back cap); side-wall vertices still
    //   accumulate and smooth from their wall triangles only.
    //   Cap vertices in layer 0 (front) and last layer (back) are identified
    //   by their vertex-index range.
    // -----------------------------------------------------------------------
    mesh.normals.resize(mesh.positions.size());

    if (params.smoothCapNormals) {
        // Accumulate face normals for all triangles
        for (int t = 0; t < mesh.indices.size(); t += 3) {
            const quint32 i0 = mesh.indices[t];
            const quint32 i1 = mesh.indices[t + 1];
            const quint32 i2 = mesh.indices[t + 2];

            const QVector3D &p0 = mesh.positions[i0];
            const QVector3D &p1 = mesh.positions[i1];
            const QVector3D &p2 = mesh.positions[i2];

            const QVector3D fn = crossEdge(p0, p1, p2);

            if (fn.lengthSquared() < 1e-12f) continue;

            mesh.normals[i0] += fn;
            mesh.normals[i1] += fn;
            mesh.normals[i2] += fn;
        }
    } else {
        // Identify cap vertex indices: layer 0 (front) and last layer (back)
        const quint32 frontBegin = layerOffsets[0];
        const quint32 frontEnd = layers.size() > 1
                                      ? layerOffsets[1]
                                      : totalVerts;
        const quint32 backBegin = layerOffsets[layers.size() - 1];
        const quint32 backEnd = totalVerts;

        auto isCapVertex = [frontBegin, frontEnd, backBegin, backEnd](quint32 idx) {
            if (idx >= frontBegin && idx < frontEnd) return 1;    // front cap
            if (idx >= backBegin && idx < backEnd) return -1; // back cap
            return 0;
        };

        // Set flat cap normals
        for (quint32 i = frontBegin; i < frontEnd; ++i)
            mesh.normals[i] = QVector3D(0, 0, 1);
        for (quint32 i = backBegin; i < backEnd; ++i)
            mesh.normals[i] = QVector3D(0, 0, -1);

        // Accumulate side-wall normals only into non-cap vertices
        for (int t = 0; t < mesh.indices.size(); t += 3) {
            const quint32 i0 = mesh.indices[t];
            const quint32 i1 = mesh.indices[t + 1];
            const quint32 i2 = mesh.indices[t + 2];

            const QVector3D &p0 = mesh.positions[i0];
            const QVector3D &p1 = mesh.positions[i1];
            const QVector3D &p2 = mesh.positions[i2];

            const QVector3D fn = crossEdge(p0, p1, p2);
            if (fn.lengthSquared() < 1e-12f) continue;

            const int c0 = isCapVertex(i0);
            const int c1 = isCapVertex(i1);
            const int c2 = isCapVertex(i2);
            if (c0 == 0) mesh.normals[i0] += fn;
            if (c1 == 0) mesh.normals[i1] += fn;
            if (c2 == 0) mesh.normals[i2] += fn;
        }
    }

    // Normalise all
    for (int i = 0; i < mesh.normals.size(); ++i) {
        mesh.normals[i] = normalisedOrZero(mesh.normals[i]);
        if (!isFiniteVec(mesh.normals[i])) {
            mesh.normals[i] = QVector3D(0, 0, 0);
        }
    }

    return mesh;
}

} // namespace mesh3d
