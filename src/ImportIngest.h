#pragma once

#include "BlenderMeshImporter.h"

#include <QImage>
#include <QString>

namespace importingest {

QImage meshToPreviewImage(const blender::mesh::MeshData& mesh, int w = 640, int h = 360);
bool savePreviewPng(const QImage& img, const QString& path);

} // namespace importingest
