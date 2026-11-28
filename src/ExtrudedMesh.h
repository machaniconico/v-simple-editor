#pragma once

#include <QFont>
#include <QPolygonF>
#include <QString>
#include <QVector>
#include <QVector3D>

/*  ExtrudedMesh — pure geometry module for 3D text/shape extrusion.
 *
 *  Normalisation convention (glyphContours):
 *    - The bounding-box centre is translated to (0,0).
 *    - The result is scaled so that the tallest glyph extent (max Y - min Y) = 1.0.
 *    - If the height is zero (degenerate font), no scaling is applied.
 *
 *  Triangulation convention (triangulateRing):
 *    - Centroid-fan triangulation: connect every edge to the ring centroid.
 *    - This works correctly for convex rings. For concave glyph outlines the
 *      result may contain self-intersecting triangles; a full ear-clipping
 *      triangulator is a future enhancement. Documented limitation.
 *
 *  Normal convention (buildExtrudedMesh):
 *    - Cap normals are smoothed (averaged per-vertex) when smoothCapNormals=true.
 *    - Side-wall normals are outward-facing edge normals (flat per-face).
 *    - All returned normals are unit-length (within 1e-3). Degenerate triangles
 *      produce (0,0,0). No NaN/Inf for valid inputs.
 *
 *  Z-range:
 *    - Front cap surface sits at z = 0.
 *    - Bevel steps (if any) descend from z = 0 toward z = -depth within the
 *      same [-depth, 0] envelope.
 *    - Back cap surface sits at z = -depth.
 *    - All vertices stay within [-depth, 0].
 */

namespace mesh3d {

struct ExtrudeParams {
    double depth          = 0.2;
    double bevelDepth     = 0.02;
    double bevelWidth     = 0.02;
    int    bevelSegments  = 2;
    bool   smoothCapNormals = true;
};

struct TriMesh {
    QVector<QVector3D> positions;
    QVector<QVector3D> normals;
    QVector<quint32>   indices;
};

/* Extract glyph outlines from a QFont as closed 2D polygons.
 * Returns polygons normalised: centred at origin, max height = 1.0. */
QVector<QPolygonF> glyphContours(const QString &text, const QFont &font, double flatness = 0.5);

/* Centroid-fan triangulation of a single closed ring.
 * Returns flat triplet indices relative to the input ring vertices. */
QVector<quint32> triangulateRing(const QPolygonF &ring);

/* Extrusion limits shared by the builder, the Text3DLayer mesh cache key and
 * the MCP set_clip_text3d guard (GUI Text3DExtrusionDialog uses the same ranges). */
inline constexpr int    kMaxBevelSegments   = 8;
inline constexpr double kMaxExtrudeDepth    = 5.0;
inline constexpr double kMaxBevelSize       = 1.0;
inline constexpr double kMaxExtrudeMidSteps = 40000.0;
inline constexpr qint64 kMaxExtrudeVertices = 2000000;

/* Middle-profile step count of the bevelled profile: 0 when there is no
 * active bevel (segments > 0, bevelDepth > 0 and 2*bevelDepth - depth > 1e-6),
 * otherwise (2*bevelDepth - depth) * (2*segments + 1) / depth (inf at depth 0). */
double extrudeMidSteps(const ExtrudeParams &params);

/* Non-finite depth / bevelDepth / bevelWidth become 0, then depth is clamped
 * to 0..kMaxExtrudeDepth, the bevel sizes to 0..kMaxBevelSize and the segments
 * to 0..kMaxBevelSegments; a step count over kMaxExtrudeMidSteps drops the
 * bevel (segments 0). *changed is set when any value differs from the input. */
ExtrudeParams sanitizeExtrudeParams(const ExtrudeParams &params, bool *changed = nullptr);

/* Mesh cache key "text|font|depth|bevelDepth|bevelWidth|segments" built from
 * already sanitised parameters. */
QString extrudeMeshCacheKey(const QString &text, const QFont &font, const ExtrudeParams &sanitized);

/* Build an extruded TriMesh from 2D contours.
 * Produces front cap, back cap, side walls, and optional bevel.
 * The parameters are sanitised first (sanitizeExtrudeParams); a profile whose
 * vertex count would exceed kMaxExtrudeVertices falls back to no bevel.
 * *changed is set when either fallback altered the requested mesh. */
TriMesh buildExtrudedMesh(const QVector<QPolygonF> &contours, const ExtrudeParams &params,
                          bool *changed = nullptr);

} // namespace mesh3d
