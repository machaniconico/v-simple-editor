#include "ImportIngest.h"

#include <QtGlobal>
#include <QVector2D>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr QRgb kBackground = 0xff151820u;
constexpr QRgb kPlaceholder = 0xff2a3140u;
constexpr QRgb kLine = 0xffe8edf5u;

void setPixelSafe(QImage& image, int x, int y, QRgb color)
{
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height())
        return;
    image.setPixel(x, y, color);
}

void drawLine(QImage& image, int x0, int y0, int x1, int y1, QRgb color)
{
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        setPixelSafe(image, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void drawEdge(QImage& image, const QVector<QVector2D>& projected, int a, int b)
{
    if (a < 0 || b < 0 || a >= projected.size() || b >= projected.size())
        return;
    const QVector2D pa = projected.at(a);
    const QVector2D pb = projected.at(b);
    drawLine(image,
             qRound(pa.x()), qRound(pa.y()),
             qRound(pb.x()), qRound(pb.y()),
             kLine);
}

} // namespace

namespace importingest {

QImage meshToPreviewImage(const blender::mesh::MeshData& mesh, int w, int h)
{
    const int width = std::max(1, w);
    const int height = std::max(1, h);
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(kBackground);

    if (mesh.vertices.isEmpty()) {
        image.fill(kPlaceholder);
        const int insetX = std::max(1, width / 8);
        const int insetY = std::max(1, height / 8);
        drawLine(image, insetX, insetY, width - 1 - insetX, insetY, kLine);
        drawLine(image, width - 1 - insetX, insetY, width - 1 - insetX, height - 1 - insetY, kLine);
        drawLine(image, width - 1 - insetX, height - 1 - insetY, insetX, height - 1 - insetY, kLine);
        drawLine(image, insetX, height - 1 - insetY, insetX, insetY, kLine);
        return image;
    }

    QVector3D minV(std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max());
    QVector3D maxV(std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest());

    for (const QVector3D& v : mesh.vertices) {
        minV.setX(std::min(minV.x(), v.x()));
        minV.setY(std::min(minV.y(), v.y()));
        minV.setZ(std::min(minV.z(), v.z()));
        maxV.setX(std::max(maxV.x(), v.x()));
        maxV.setY(std::max(maxV.y(), v.y()));
        maxV.setZ(std::max(maxV.z(), v.z()));
    }

    const float spanX = std::max(maxV.x() - minV.x(), 1.0e-6f);
    const float spanY = std::max(maxV.y() - minV.y(), 1.0e-6f);
    const float padX = std::max(1.0f, width * 0.08f);
    const float padY = std::max(1.0f, height * 0.08f);
    const float drawableW = std::max(1.0f, static_cast<float>(width) - padX * 2.0f);
    const float drawableH = std::max(1.0f, static_cast<float>(height) - padY * 2.0f);
    const float scale = std::min(drawableW / spanX, drawableH / spanY);
    const float modelW = spanX * scale;
    const float modelH = spanY * scale;
    const float offsetX = (static_cast<float>(width) - modelW) * 0.5f;
    const float offsetY = (static_cast<float>(height) - modelH) * 0.5f;

    QVector<QVector2D> projected;
    projected.reserve(mesh.vertices.size());
    for (const QVector3D& v : mesh.vertices) {
        const float x = offsetX + (v.x() - minV.x()) * scale;
        const float y = static_cast<float>(height) - 1.0f - (offsetY + (v.y() - minV.y()) * scale);
        projected.push_back(QVector2D(x, y));
    }

    const int triangleCount = mesh.triangleIndices.size() / 3;
    for (int tri = 0; tri < triangleCount; ++tri) {
        const int base = tri * 3;
        const int a = mesh.triangleIndices.at(base);
        const int b = mesh.triangleIndices.at(base + 1);
        const int c = mesh.triangleIndices.at(base + 2);
        drawEdge(image, projected, a, b);
        drawEdge(image, projected, b, c);
        drawEdge(image, projected, c, a);
    }

    if (triangleCount == 0 && projected.size() == 1) {
        const QVector2D p = projected.first();
        setPixelSafe(image, qRound(p.x()), qRound(p.y()), kLine);
    } else if (triangleCount == 0) {
        for (int i = 1; i < projected.size(); ++i)
            drawEdge(image, projected, i - 1, i);
    }

    return image;
}

bool savePreviewPng(const QImage& img, const QString& path)
{
    return !img.isNull() && img.save(path, "PNG");
}

} // namespace importingest
