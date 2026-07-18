#include <QDebug>
#include <QImage>

#include "../ImportIngest.h"

namespace {

blender::mesh::MeshData makeCubeMesh()
{
    blender::mesh::MeshData mesh;
    mesh.vertices = {
        QVector3D(-1.0f, -1.0f, -1.0f),
        QVector3D( 1.0f, -1.0f, -1.0f),
        QVector3D( 1.0f,  1.0f, -1.0f),
        QVector3D(-1.0f,  1.0f, -1.0f),
        QVector3D(-1.0f, -1.0f,  1.0f),
        QVector3D( 1.0f, -1.0f,  1.0f),
        QVector3D( 1.0f,  1.0f,  1.0f),
        QVector3D(-1.0f,  1.0f,  1.0f),
    };
    mesh.triangleIndices = {
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        0, 4, 5, 0, 5, 1,
        1, 5, 6, 1, 6, 2,
        2, 6, 7, 2, 7, 3,
        3, 7, 4, 3, 4, 0,
    };
    return mesh;
}

int countPixelsDifferentFrom(const QImage& image, QRgb reference)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) != reference)
                ++count;
        }
    }
    return count;
}

bool imagesEqual(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.format() != b.format())
        return false;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y))
                return false;
        }
    }
    return true;
}

} // namespace

int runImportIngestSelftest()
{
    qInfo().noquote() << "[import-ingest] selftest start";
    int passed = 0;
    int failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[import-ingest] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[import-ingest] FAIL" << name << ":" << msg; };

    const blender::mesh::MeshData cube = makeCubeMesh();
    const QImage cubeA = importingest::meshToPreviewImage(cube, 160, 90);
    const QImage cubeB = importingest::meshToPreviewImage(cube, 160, 90);

    if (!cubeA.isNull() && cubeA.width() == 160 && cubeA.height() == 90) {
        pass("G1 cube preview is non-empty and sized");
    } else {
        fail("G1 cube preview", QStringLiteral("null=%1 size=%2x%3")
             .arg(cubeA.isNull()).arg(cubeA.width()).arg(cubeA.height()));
    }

    const int changedPixels = cubeA.isNull() ? 0 : countPixelsDifferentFrom(cubeA, cubeA.pixel(0, 0));
    if (changedPixels > 0) {
        pass("G2 cube preview contains wire pixels");
    } else {
        fail("G2 wire pixels", QStringLiteral("changedPixels=%1").arg(changedPixels));
    }

    if (imagesEqual(cubeA, cubeB)) {
        pass("G3 same mesh renders deterministically");
    } else {
        fail("G3 determinism", QStringLiteral("second render differed"));
    }

    const blender::mesh::MeshData empty;
    const QImage emptyImage = importingest::meshToPreviewImage(empty, 64, 36);
    if (!emptyImage.isNull() && emptyImage.width() == 64 && emptyImage.height() == 36) {
        pass("G4 empty mesh returns non-empty placeholder");
    } else {
        fail("G4 empty mesh", QStringLiteral("null=%1 size=%2x%3")
             .arg(emptyImage.isNull()).arg(emptyImage.width()).arg(emptyImage.height()));
    }

    const int emptyChangedPixels = emptyImage.isNull() ? 0 : countPixelsDifferentFrom(emptyImage, emptyImage.pixel(0, 0));
    if (emptyChangedPixels > 0) {
        pass("G5 empty placeholder has distinct pixels");
    } else {
        fail("G5 placeholder pixels", QStringLiteral("changedPixels=%1").arg(emptyChangedPixels));
    }

    qInfo().noquote() << "[import-ingest] summary:"
                      << passed << "passed," << failed << "failed";
    return failed == 0 ? 0 : 1;
}
