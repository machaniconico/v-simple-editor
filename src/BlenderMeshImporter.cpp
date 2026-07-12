#include "BlenderMeshImporter.h"

#include <QByteArray>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtEndian>
#include <QtGlobal>

#include <cstring>
#include <limits>

#if __has_include(<assimp/Importer.hpp>)
#  include <assimp/Importer.hpp>
#  include <assimp/postprocess.h>
#  include <assimp/scene.h>
#  define VEDITOR_BLENDER_HAVE_ASSIMP 1
#else
#  define VEDITOR_BLENDER_HAVE_ASSIMP 0
#endif

namespace blender {
namespace mesh {

namespace {

// ---------- shared helpers ----------------------------------------------------

SourceFormat detectFormatFromExtension(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("obj"))                                return SourceFormat::OBJ;
    if (ext == QLatin1String("gltf") || ext == QLatin1String("glb")) return SourceFormat::GLTF;
    if (ext == QLatin1String("fbx"))                                return SourceFormat::FBX;
    if (ext == QLatin1String("abc"))                                return SourceFormat::Alembic;
    return SourceFormat::Unknown;
}

MeshData makeEmpty(SourceFormat fmt)
{
    MeshData m;
    m.sourceFormat = fmt;
    return m;
}

// Ensure index array length is a multiple of 3 by trimming trailing partial
// triangles. Used as a final guard so AC #6 holds on every code path.
void normaliseTriangleIndices(MeshData &m)
{
    const int rem = m.triangleIndices.size() % 3;
    if (rem != 0) {
        m.triangleIndices.resize(m.triangleIndices.size() - rem);
    }
}

// ---------- OBJ parser --------------------------------------------------------

struct ObjFaceVertex {
    int v  = 0;  // 1-indexed in source; 0 means "absent"
    int vt = 0;
    int vn = 0;
};

ObjFaceVertex parseObjFaceToken(const QString &tok)
{
    ObjFaceVertex out;
    const QStringList parts = tok.split(QLatin1Char('/'));
    if (parts.size() >= 1 && !parts[0].isEmpty()) out.v  = parts[0].toInt();
    if (parts.size() >= 2 && !parts[1].isEmpty()) out.vt = parts[1].toInt();
    if (parts.size() >= 3 && !parts[2].isEmpty()) out.vn = parts[2].toInt();
    return out;
}

// Resolve an OBJ index. Supports negative (relative-to-end) indexing per spec.
// Returns -1 if out of range.
int resolveObjIndex(int oneBased, int collectionSize)
{
    if (oneBased == 0) return -1;
    if (oneBased > 0) {
        const int idx = oneBased - 1;
        return (idx < collectionSize) ? idx : -1;
    }
    const int idx = collectionSize + oneBased; // negative indexing
    return (idx >= 0 && idx < collectionSize) ? idx : -1;
}

MeshData loadObj(const QString &path)
{
    MeshData out = makeEmpty(SourceFormat::OBJ);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("blender::mesh::loadObj: cannot open %s", qUtf8Printable(path));
        return out;
    }

    QVector<QVector3D> srcPos;
    QVector<QVector3D> srcNorm;
    QVector<QVector2D> srcUv;

    // String-keyed dedup of (v,vt,vn) tuples. Avoids needing a custom qHash
    // overload for a local struct, which is awkward in Qt6.
    QHash<QString, int> dedup;
    dedup.reserve(1024);

    auto emitVertex = [&](const ObjFaceVertex &fv) -> int {
        const int vi  = resolveObjIndex(fv.v,  srcPos.size());
        if (vi < 0) return -1;
        const int vti = (fv.vt != 0) ? resolveObjIndex(fv.vt, srcUv.size()) : -1;
        const int vni = (fv.vn != 0) ? resolveObjIndex(fv.vn, srcNorm.size()) : -1;

        const QString key = QStringLiteral("%1/%2/%3").arg(vi).arg(vti).arg(vni);
        auto it = dedup.find(key);
        if (it != dedup.end()) return it.value();

        const int newIdx = out.vertices.size();
        out.vertices.append(srcPos[vi]);
        out.uvs.append(vti >= 0 ? srcUv[vti] : QVector2D(0.0f, 0.0f));
        out.normals.append(vni >= 0 ? srcNorm[vni] : QVector3D(0.0f, 0.0f, 1.0f));
        dedup.insert(key, newIdx);
        return newIdx;
    };

    while (!f.atEnd()) {
        const QByteArray rawLine = f.readLine();
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;

        const QStringList tok = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (tok.isEmpty()) continue;

        const QString &head = tok.first();
        if (head == QLatin1String("v") && tok.size() >= 4) {
            srcPos.append(QVector3D(tok[1].toFloat(), tok[2].toFloat(), tok[3].toFloat()));
        } else if (head == QLatin1String("vn") && tok.size() >= 4) {
            srcNorm.append(QVector3D(tok[1].toFloat(), tok[2].toFloat(), tok[3].toFloat()));
        } else if (head == QLatin1String("vt") && tok.size() >= 3) {
            srcUv.append(QVector2D(tok[1].toFloat(), tok[2].toFloat()));
        } else if (head == QLatin1String("usemtl") && tok.size() >= 2) {
            if (out.materialName.isEmpty()) out.materialName = tok[1];
        } else if (head == QLatin1String("f") && tok.size() >= 4) {
            // Triangulate as a fan from the first vertex (handles tris, quads,
            // and convex n-gons).
            QVector<int> faceIdx;
            faceIdx.reserve(tok.size() - 1);
            for (int i = 1; i < tok.size(); ++i) {
                const ObjFaceVertex fv = parseObjFaceToken(tok[i]);
                const int idx = emitVertex(fv);
                if (idx < 0) { faceIdx.clear(); break; }
                faceIdx.append(idx);
            }
            if (faceIdx.size() < 3) continue;
            for (int i = 1; i + 1 < faceIdx.size(); ++i) {
                out.triangleIndices.append(faceIdx[0]);
                out.triangleIndices.append(faceIdx[i]);
                out.triangleIndices.append(faceIdx[i + 1]);
            }
        }
    }

    normaliseTriangleIndices(out);
    return out;
}

// ---------- glTF / GLB parser ------------------------------------------------

constexpr quint32 kGlbMagic     = 0x46546C67u; // "glTF"
constexpr quint32 kGlbChunkJson = 0x4E4F534Au; // "JSON"
constexpr quint32 kGlbChunkBin  = 0x004E4942u; // "BIN\0"

struct GltfBuffers {
    QVector<QByteArray> data; // indexed by buffers[i]
};

int gltfComponentSize(int componentType)
{
    switch (componentType) {
        case 5120: return 1; // BYTE
        case 5121: return 1; // UNSIGNED_BYTE
        case 5122: return 2; // SHORT
        case 5123: return 2; // UNSIGNED_SHORT
        case 5125: return 4; // UNSIGNED_INT
        case 5126: return 4; // FLOAT
        default:   return 0;
    }
}

int gltfComponentCountForType(const QString &type)
{
    if (type == QLatin1String("SCALAR")) return 1;
    if (type == QLatin1String("VEC2"))   return 2;
    if (type == QLatin1String("VEC3"))   return 3;
    if (type == QLatin1String("VEC4"))   return 4;
    if (type == QLatin1String("MAT2"))   return 4;
    if (type == QLatin1String("MAT3"))   return 9;
    if (type == QLatin1String("MAT4"))   return 16;
    return 0;
}

// Read raw bytes for an accessor. Returns empty on any structural problem.
QByteArray readAccessorBytes(const QJsonObject &gltf,
                             const GltfBuffers &buffers,
                             int accessorIndex,
                             int *outCount,
                             int *outComponentType,
                             int *outComponentCount)
{
    const QJsonArray accessors  = gltf.value(QStringLiteral("accessors")).toArray();
    const QJsonArray bufferViews = gltf.value(QStringLiteral("bufferViews")).toArray();
    if (accessorIndex < 0 || accessorIndex >= accessors.size()) return {};

    const QJsonObject acc = accessors.at(accessorIndex).toObject();
    const int viewIdx     = acc.value(QStringLiteral("bufferView")).toInt(-1);
    const int compType    = acc.value(QStringLiteral("componentType")).toInt(0);
    const int count       = acc.value(QStringLiteral("count")).toInt(0);
    const QString type    = acc.value(QStringLiteral("type")).toString();
    const int compCount   = gltfComponentCountForType(type);
    const int compSize    = gltfComponentSize(compType);
    const int accByteOff  = acc.value(QStringLiteral("byteOffset")).toInt(0);

    if (viewIdx < 0 || viewIdx >= bufferViews.size() || count <= 0
        || compCount == 0 || compSize == 0) {
        return {};
    }

    const QJsonObject view = bufferViews.at(viewIdx).toObject();
    const int bufferIdx    = view.value(QStringLiteral("buffer")).toInt(-1);
    const int viewByteOff  = view.value(QStringLiteral("byteOffset")).toInt(0);
    const int viewByteLen  = view.value(QStringLiteral("byteLength")).toInt(0);
    if (bufferIdx < 0 || bufferIdx >= buffers.data.size()) return {};

    const QByteArray &buf = buffers.data[bufferIdx];
    const qint64 needed = qint64(count) * compCount * compSize;
    const qint64 start  = qint64(viewByteOff) + accByteOff;
    if (start < 0 || needed <= 0
        || needed > std::numeric_limits<qsizetype>::max()
        || start > buf.size()
        || needed > qint64(buf.size()) - start) {
        return {};
    }
    Q_UNUSED(viewByteLen);

    if (outCount)          *outCount = count;
    if (outComponentType)  *outComponentType = compType;
    if (outComponentCount) *outComponentCount = compCount;
    return QByteArray(buf.constData() + static_cast<qsizetype>(start),
                      static_cast<qsizetype>(needed));
}

bool checkedAccessorElementCount(int count, int componentCount, int *out)
{
    if (count <= 0 || componentCount <= 0) return false;
    const qint64 total = qint64(count) * componentCount;
    if (total > std::numeric_limits<int>::max()) return false;
    if (out) *out = static_cast<int>(total);
    return true;
}

QVector<float> decodeFloats(const QByteArray &raw, int compType, int count)
{
    QVector<float> out;
    out.reserve(count);
    const char *p = raw.constData();
    const int n = count;

    auto le16 = [](const char *q) {
        quint16 v;
        std::memcpy(&v, q, 2);
        return qFromLittleEndian(v);
    };
    auto le32 = [](const char *q) {
        quint32 v;
        std::memcpy(&v, q, 4);
        return qFromLittleEndian(v);
    };

    switch (compType) {
        case 5126: { // FLOAT
            for (int i = 0; i < n; ++i) {
                quint32 bits = le32(p + i * 4);
                float f;
                std::memcpy(&f, &bits, 4);
                out.append(f);
            }
            break;
        }
        case 5123: { // UNSIGNED_SHORT (normalised path not used for POSITION)
            for (int i = 0; i < n; ++i) out.append(static_cast<float>(le16(p + i * 2)));
            break;
        }
        case 5121: { // UNSIGNED_BYTE
            for (int i = 0; i < n; ++i) out.append(static_cast<float>(static_cast<quint8>(p[i])));
            break;
        }
        default:
            break;
    }
    return out;
}

QVector<int> decodeIntIndices(const QByteArray &raw, int compType, int count)
{
    QVector<int> out;
    out.reserve(count);
    const char *p = raw.constData();

    auto le16 = [](const char *q) {
        quint16 v;
        std::memcpy(&v, q, 2);
        return qFromLittleEndian(v);
    };
    auto le32 = [](const char *q) {
        quint32 v;
        std::memcpy(&v, q, 4);
        return qFromLittleEndian(v);
    };

    switch (compType) {
        case 5121: // UNSIGNED_BYTE
            for (int i = 0; i < count; ++i) out.append(static_cast<int>(static_cast<quint8>(p[i])));
            break;
        case 5123: // UNSIGNED_SHORT
            for (int i = 0; i < count; ++i) out.append(static_cast<int>(le16(p + i * 2)));
            break;
        case 5125: // UNSIGNED_INT
            for (int i = 0; i < count; ++i) out.append(static_cast<int>(le32(p + i * 4)));
            break;
        default:
            break;
    }
    return out;
}

// Resolve gltf "buffers" array to in-memory bytes. For .gltf this loads
// external .bin files (relative to the gltf path) or decodes data URIs. For
// .glb the embedded BIN chunk should already be supplied by the caller in
// `embeddedBin` and assigned to buffers[0] before calling. data URIs supported
// for the data:application/octet-stream;base64,... case.
GltfBuffers resolveGltfBuffers(const QJsonObject &gltf, const QString &gltfPath,
                               const QByteArray &embeddedBin)
{
    GltfBuffers out;
    const QJsonArray bufs = gltf.value(QStringLiteral("buffers")).toArray();
    out.data.reserve(bufs.size());
    const QString baseDir = QFileInfo(gltfPath).absolutePath();

    for (int i = 0; i < bufs.size(); ++i) {
        const QJsonObject b = bufs.at(i).toObject();
        const QString uri   = b.value(QStringLiteral("uri")).toString();
        const int byteLen   = b.value(QStringLiteral("byteLength")).toInt(0);

        QByteArray bytes;
        if (i == 0 && uri.isEmpty() && !embeddedBin.isEmpty()) {
            bytes = embeddedBin;
        } else if (uri.startsWith(QLatin1String("data:"))) {
            const int comma = uri.indexOf(QLatin1Char(','));
            if (comma > 0) {
                const QString meta = uri.left(comma);
                const QString data = uri.mid(comma + 1);
                if (meta.contains(QLatin1String(";base64"))) {
                    bytes = QByteArray::fromBase64(data.toUtf8());
                } else {
                    bytes = QByteArray::fromPercentEncoding(data.toUtf8());
                }
            }
        } else if (!uri.isEmpty()) {
            QFile f(baseDir + QLatin1Char('/') + uri);
            if (f.open(QIODevice::ReadOnly)) bytes = f.readAll();
        }

        if (byteLen > 0 && bytes.size() < byteLen) {
            qWarning("blender::mesh: gltf buffer[%d] short (%d < %d)", i, bytes.size(), byteLen);
        }
        out.data.append(bytes);
    }
    return out;
}

MeshData parseGltfDocument(const QJsonObject &gltf, const QString &path,
                           const QByteArray &embeddedBin)
{
    MeshData out = makeEmpty(SourceFormat::GLTF);

    const QJsonArray meshes = gltf.value(QStringLiteral("meshes")).toArray();
    if (meshes.isEmpty()) return out;

    const QJsonObject mesh0  = meshes.first().toObject();
    const QJsonArray prims   = mesh0.value(QStringLiteral("primitives")).toArray();
    if (prims.isEmpty()) return out;

    const QJsonObject prim   = prims.first().toObject();
    const QJsonObject attrs  = prim.value(QStringLiteral("attributes")).toObject();
    const int posAcc         = attrs.value(QStringLiteral("POSITION")).toInt(-1);
    const int normAcc        = attrs.value(QStringLiteral("NORMAL")).toInt(-1);
    const int uvAcc          = attrs.value(QStringLiteral("TEXCOORD_0")).toInt(-1);
    const int idxAcc         = prim.value(QStringLiteral("indices")).toInt(-1);

    const GltfBuffers buffers = resolveGltfBuffers(gltf, path, embeddedBin);

    // POSITION
    if (posAcc >= 0) {
        int count = 0, ct = 0, cc = 0;
        const QByteArray raw = readAccessorBytes(gltf, buffers, posAcc, &count, &ct, &cc);
        int elementCount = 0;
        if (!raw.isEmpty() && cc == 3 && checkedAccessorElementCount(count, cc, &elementCount)) {
            const QVector<float> f = decodeFloats(raw, ct, elementCount);
            if (f.size() == elementCount) {
                out.vertices.reserve(count);
                for (int i = 0; i < count; ++i) {
                    out.vertices.append(QVector3D(f[i * 3 + 0], f[i * 3 + 1], f[i * 3 + 2]));
                }
            }
        }
    }
    // NORMAL
    if (normAcc >= 0) {
        int count = 0, ct = 0, cc = 0;
        const QByteArray raw = readAccessorBytes(gltf, buffers, normAcc, &count, &ct, &cc);
        int elementCount = 0;
        if (!raw.isEmpty() && cc == 3 && checkedAccessorElementCount(count, cc, &elementCount)) {
            const QVector<float> f = decodeFloats(raw, ct, elementCount);
            if (f.size() == elementCount) {
                out.normals.reserve(count);
                for (int i = 0; i < count; ++i) {
                    out.normals.append(QVector3D(f[i * 3 + 0], f[i * 3 + 1], f[i * 3 + 2]));
                }
            }
        }
    }
    // TEXCOORD_0
    if (uvAcc >= 0) {
        int count = 0, ct = 0, cc = 0;
        const QByteArray raw = readAccessorBytes(gltf, buffers, uvAcc, &count, &ct, &cc);
        int elementCount = 0;
        if (!raw.isEmpty() && cc == 2 && checkedAccessorElementCount(count, cc, &elementCount)) {
            const QVector<float> f = decodeFloats(raw, ct, elementCount);
            if (f.size() == elementCount) {
                out.uvs.reserve(count);
                for (int i = 0; i < count; ++i) {
                    out.uvs.append(QVector2D(f[i * 2 + 0], f[i * 2 + 1]));
                }
            }
        }
    }
    // Indices (or fall back to range[0..vertex-count) when omitted, treating
    // the position stream as a flat triangle list per glTF spec).
    if (idxAcc >= 0) {
        int count = 0, ct = 0, cc = 0;
        const QByteArray raw = readAccessorBytes(gltf, buffers, idxAcc, &count, &ct, &cc);
        if (!raw.isEmpty()) {
            out.triangleIndices = decodeIntIndices(raw, ct, count);
        }
    } else {
        out.triangleIndices.reserve(out.vertices.size());
        for (int i = 0; i < out.vertices.size(); ++i) out.triangleIndices.append(i);
    }

    // Material (just record the name if available)
    const int matIdx = prim.value(QStringLiteral("material")).toInt(-1);
    const QJsonArray materials = gltf.value(QStringLiteral("materials")).toArray();
    if (matIdx >= 0 && matIdx < materials.size()) {
        out.materialName = materials.at(matIdx).toObject()
                             .value(QStringLiteral("name")).toString();
    }

    // Pad missing UVs/normals so per-vertex arrays line up if downstream
    // code zips them by index. (Optional: keep arrays aligned to vertex count.)
    if (!out.normals.isEmpty() && out.normals.size() < out.vertices.size()) {
        out.normals.resize(out.vertices.size());
    }
    if (!out.uvs.isEmpty() && out.uvs.size() < out.vertices.size()) {
        out.uvs.resize(out.vertices.size());
    }

    normaliseTriangleIndices(out);
    return out;
}

MeshData loadGltf(const QString &path)
{
    MeshData empty = makeEmpty(SourceFormat::GLTF);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("blender::mesh::loadGltf: cannot open %s", qUtf8Printable(path));
        return empty;
    }
    const QByteArray bytes = f.readAll();
    if (bytes.size() < 4) return empty;

    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("glb")
        || (bytes.size() >= 4
            && static_cast<quint8>(bytes[0]) == 'g'
            && static_cast<quint8>(bytes[1]) == 'l'
            && static_cast<quint8>(bytes[2]) == 'T'
            && static_cast<quint8>(bytes[3]) == 'F')) {

        // GLB: 12-byte header + chunk(JSON) + optional chunk(BIN).
        if (bytes.size() < 12) return empty;
        auto rd32 = [&](qsizetype offset) {
            quint32 v;
            std::memcpy(&v, bytes.constData() + offset, 4);
            return qFromLittleEndian(v);
        };
        const quint32 magic = rd32(0);
        if (magic != kGlbMagic) return empty;
        const quint32 totalLen = rd32(8);
        Q_UNUSED(totalLen);

        qsizetype cursor = 12;
        QByteArray jsonBytes;
        QByteArray binBytes;
        while (cursor <= bytes.size() - 8) {
            const quint32 chunkLen  = rd32(cursor);
            const quint32 chunkType = rd32(cursor + 4);
            cursor += 8;
            const qint64 chunkLen64 = chunkLen;
            if (chunkLen64 > std::numeric_limits<qsizetype>::max()
                || chunkLen64 > qint64(bytes.size()) - cursor) {
                break;
            }
            const qsizetype chunkSize = static_cast<qsizetype>(chunkLen64);
            if (chunkType == kGlbChunkJson) {
                jsonBytes = QByteArray(bytes.constData() + cursor, chunkSize);
            } else if (chunkType == kGlbChunkBin) {
                binBytes = QByteArray(bytes.constData() + cursor, chunkSize);
            }
            cursor += chunkSize;
        }
        if (jsonBytes.isEmpty()) return empty;

        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return empty;
        return parseGltfDocument(doc.object(), path, binBytes);
    }

    // .gltf (text JSON, possibly with external .bin).
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning("blender::mesh::loadGltf: JSON parse error in %s", qUtf8Printable(path));
        return empty;
    }
    return parseGltfDocument(doc.object(), path, QByteArray());
}

// ---------- FBX / Alembic via assimp (compile-time gated) --------------------

#if VEDITOR_BLENDER_HAVE_ASSIMP
MeshData loadAssimp(const QString &path, SourceFormat fmt)
{
    MeshData out = makeEmpty(fmt);
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
        path.toStdString(),
        aiProcess_Triangulate | aiProcess_GenNormals);
    if (!scene || scene->mNumMeshes == 0 || !scene->mMeshes[0]) {
        qWarning("blender::mesh::loadAssimp: assimp failed for %s", qUtf8Printable(path));
        return out;
    }

    const aiMesh *m = scene->mMeshes[0];
    out.vertices.reserve(m->mNumVertices);
    out.normals.reserve(m->mNumVertices);
    out.uvs.reserve(m->mNumVertices);
    for (unsigned int i = 0; i < m->mNumVertices; ++i) {
        const aiVector3D &v = m->mVertices[i];
        out.vertices.append(QVector3D(v.x, v.y, v.z));
        if (m->HasNormals()) {
            const aiVector3D &n = m->mNormals[i];
            out.normals.append(QVector3D(n.x, n.y, n.z));
        } else {
            out.normals.append(QVector3D(0.0f, 0.0f, 1.0f));
        }
        if (m->HasTextureCoords(0)) {
            const aiVector3D &t = m->mTextureCoords[0][i];
            out.uvs.append(QVector2D(t.x, t.y));
        } else {
            out.uvs.append(QVector2D(0.0f, 0.0f));
        }
    }

    out.triangleIndices.reserve(m->mNumFaces * 3);
    for (unsigned int i = 0; i < m->mNumFaces; ++i) {
        const aiFace &face = m->mFaces[i];
        if (face.mNumIndices != 3) continue; // post-Triangulate guarantees 3
        out.triangleIndices.append(static_cast<int>(face.mIndices[0]));
        out.triangleIndices.append(static_cast<int>(face.mIndices[1]));
        out.triangleIndices.append(static_cast<int>(face.mIndices[2]));
    }

    if (m->mMaterialIndex < scene->mNumMaterials) {
        aiString name;
        if (scene->mMaterials[m->mMaterialIndex]->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
            out.materialName = QString::fromUtf8(name.C_Str());
        }
    }

    normaliseTriangleIndices(out);
    return out;
}
#else
MeshData loadAssimp(const QString &path, SourceFormat /*fmt*/)
{
    qWarning("blender::mesh: assimp not available; %s ignored", qUtf8Printable(path));
    // AC #4: when assimp is unavailable, sourceFormat=Unknown + empty data.
    return makeEmpty(SourceFormat::Unknown);
}
#endif

} // anonymous namespace

// ---------- public entry point ----------------------------------------------

MeshData loadMeshFile(const QString &path)
{
    if (path.isEmpty()) {
        return makeEmpty(SourceFormat::Unknown);
    }
    const SourceFormat fmt = detectFormatFromExtension(path);

    // Missing file short-circuit (still reports the detected format so callers
    // can distinguish "wrong path" from "unknown extension"). AC #5: empty.
    if (!QFileInfo(path).isFile()) {
        if (fmt == SourceFormat::Unknown) return makeEmpty(SourceFormat::Unknown);
        // For known extensions but missing file, parsers return empty; keep
        // the detected sourceFormat for diagnostics.
        MeshData m = makeEmpty(fmt);
        return m;
    }

    MeshData result;
    switch (fmt) {
        case SourceFormat::OBJ:     result = loadObj(path); break;
        case SourceFormat::GLTF:    result = loadGltf(path); break;
        case SourceFormat::FBX:     result = loadAssimp(path, SourceFormat::FBX); break;
        case SourceFormat::Alembic: result = loadAssimp(path, SourceFormat::Alembic); break;
        case SourceFormat::Unknown:
        default:
            result = makeEmpty(SourceFormat::Unknown);
            break;
    }
    normaliseTriangleIndices(result);
    return result;
}

} // namespace mesh
} // namespace blender
