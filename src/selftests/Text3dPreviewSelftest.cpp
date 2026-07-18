#include "Camera3D.h"
#include "Text3DExtrusionDialog.h"
#include "Text3DLayer.h"

#include <QApplication>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QVector3D>
#include <QtGlobal>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <memory>

namespace {

bool cameraEquals(const Camera3D &a, const Camera3D &b)
{
    return a.toJson() == b.toJson();
}

bool cameraEqualsDefault(const Camera3D &camera)
{
    return cameraEquals(camera, Camera3D{});
}

Camera3D makeNonDefaultCamera()
{
    Camera3D camera;
    Camera3DState state;
    state.position = QVector3D(1.5f, -2.0f, 6.0f);
    state.target = QVector3D(0.25f, 0.5f, -3.0f);
    state.fov = 42.0;
    state.nearPlane = 0.25;
    state.farPlane = 500.0;
    state.roll = 7.0;
    camera.setCamera(state);
    return camera;
}

void configureExtrudedLayerForRenderGate(Text3DLayer &layer)
{
    layer.setText(QStringLiteral("FIX09"), QFont(QStringLiteral("Arial"), 72));
    layer.setExtrudeEnabled(true);
    layer.setExtrude(0.35, 0.03, 0.03, 2);
    // The extruded mesh is normalized to unit scale, but m_cameraDistance
    // defaults to 400 (quad-path pixel units) — without an orbit-scale
    // distance the text subtends <1px and renders fully transparent.
    layer.setCameraDistance(3.0);
}

bool imageBytesEqual(const QImage &a, const QImage &b)
{
    return a.size() == b.size()
        && a.format() == b.format()
        && a.sizeInBytes() == b.sizeInBytes()
        && (a.sizeInBytes() == 0
            || std::memcmp(a.constBits(), b.constBits(), static_cast<std::size_t>(a.sizeInBytes())) == 0);
}

void printGate(int gate, bool ok, const QString &reason, int &passed, int &failed)
{
    if (ok) {
        std::printf("G%d: PASS\n", gate);
        ++passed;
        return;
    }

    std::printf("G%d: FAIL %s\n", gate, reason.toUtf8().constData());
    ++failed;
}

} // namespace

int runText3dPreviewSelftest()
{
    int passed = 0;
    int failed = 0;

    // G1: Camera3D round-trip, including default camera omission/reconstruction.
    {
        QString reason;
        const Camera3D camera = makeNonDefaultCamera();

        Text3DLayer layer;
        layer.setCamera(camera);
        const QJsonObject json = layer.toJson();
        Text3DLayer restored;
        restored.fromJson(json);

        bool ok = true;
        if (!json.value(QStringLiteral("camera")).isObject()) {
            ok = false;
            reason = QStringLiteral("non-default camera was not serialized");
        } else if (!cameraEquals(restored.camera(), camera)) {
            ok = false;
            reason = QStringLiteral("non-default camera changed after toJson/fromJson");
        }

        Text3DLayer defaultLayer;
        const QJsonObject defaultJson = defaultLayer.toJson();
        Text3DLayer defaultRestored;
        defaultRestored.fromJson(defaultJson);
        if (ok && !cameraEquals(defaultRestored.camera(), Camera3D{})) {
            ok = false;
            reason = QStringLiteral("default camera did not round-trip as Camera3D{}");
        }
        if (ok && defaultJson.value(QStringLiteral("camera")).isObject()) {
            Camera3D serializedDefault;
            serializedDefault.fromJson(defaultJson.value(QStringLiteral("camera")).toObject());
            if (!cameraEquals(serializedDefault, Camera3D{})) {
                ok = false;
                reason = QStringLiteral("serialized default camera is not Camera3D{}");
            }
        }

        printGate(1, ok, reason, passed, failed);
    }

    // G2: Logic-only proxy-size gate. Rendering is intentionally not exercised
    // here so the selftest stays headless and does not depend on GL/font setup.
    {
        const QSize fullSize(1920, 1080);
        const QSize proxySize = text3DPreviewProxySize(fullSize);
        const bool ok = proxySize == QSize(640, 360)
            && proxySize.width() < fullSize.width()
            && proxySize.height() < fullSize.height()
            && proxySize.width() * fullSize.height() == proxySize.height() * fullSize.width();
        const QString reason = QStringLiteral("expected 1920x1080 proxy size 640x360 with preserved aspect ratio");
        printGate(2, ok, reason, passed, failed);
    }

    // G3: proxy policy predicate.
    {
        Text3DLayer extruded;
        extruded.setExtrudeEnabled(true);

        Text3DLayer flat;
        const bool ok =
            !shouldUseText3DPreviewProxy(extruded, QSize(640, 480), 1280)
            && shouldUseText3DPreviewProxy(extruded, QSize(1920, 1080), 1280)
            && !shouldUseText3DPreviewProxy(extruded, QSize(1920, 1080), 0)
            && !shouldUseText3DPreviewProxy(flat, QSize(1920, 1080), 1280);
        const QString reason = QStringLiteral("predicate did not match threshold/extrude requirements");
        printGate(3, ok, reason, passed, failed);
    }

    // G4: config without a camera field must reconstruct Camera3D{}.
    {
        QJsonObject legacyConfig;
        legacyConfig[QStringLiteral("text")] = QStringLiteral("Legacy");
        legacyConfig[QStringLiteral("extrudeEnabled")] = true;

        Text3DLayer layer;
        layer.setCamera(makeNonDefaultCamera());
        layer.fromJson(legacyConfig);

        const bool ok = cameraEquals(layer.camera(), Camera3D{});
        const QString reason = QStringLiteral("missing camera field did not reset to Camera3D{}");
        printGate(4, ok, reason, passed, failed);
    }

    // G5/G6 construct a QDialog and rasterize glyphs, so this selftest is
    // registered with needsQApplication=true. The deployed Windows build ships
    // only the "windows" platform plugin, so a hand-rolled offscreen
    // QApplication cannot initialize here (it pops the fatal plugin dialog).
    if (!QApplication::instance()) {
        printGate(5, false, QStringLiteral("QApplication missing — must dispatch post-QApplication"), passed, failed);
        printGate(6, false, QStringLiteral("QApplication missing — must dispatch post-QApplication"), passed, failed);
        std::printf("text3d-preview: %d passed, %d failed\n", passed, failed);
        return failed;
    }

    // G5: dialog resave preserves camera JSON.
    {
        QString reason;
        bool ok = true;

        Text3DLayer src;
        src.setText(QStringLiteral("Camera"), QFont(QStringLiteral("Arial"), 72));
        src.setExtrudeEnabled(true);
        src.setCamera(makeNonDefaultCamera());

        Text3DExtrusionDialog dialog;
        dialog.setLayer(src);
        std::unique_ptr<Text3DLayer> out(dialog.layer(nullptr));

        if (!out) {
            ok = false;
            reason = QStringLiteral("dialog.layer(nullptr) returned null");
        } else if (QJsonDocument(out->camera().toJson()) != QJsonDocument(src.camera().toJson())) {
            ok = false;
            reason = QStringLiteral("dialog resave changed non-default camera JSON");
        }

        if (ok) {
            Text3DLayer defaultSrc;
            defaultSrc.setText(QStringLiteral("Default"), QFont(QStringLiteral("Arial"), 72));
            defaultSrc.setExtrudeEnabled(true);

            Text3DExtrusionDialog defaultDialog;
            defaultDialog.setLayer(defaultSrc);
            std::unique_ptr<Text3DLayer> defaultOut(defaultDialog.layer(nullptr));

            if (!defaultOut) {
                ok = false;
                reason = QStringLiteral("dialog.layer(nullptr) returned null for default camera");
            } else if (!cameraEqualsDefault(defaultOut->camera())) {
                ok = false;
                reason = QStringLiteral("default camera did not round-trip through dialog as Camera3D{}");
            } else if (defaultOut->toJson().value(QStringLiteral("camera")).isObject()) {
                ok = false;
                reason = QStringLiteral("dialog resave emitted a spurious default camera object");
            }
        }

        printGate(5, ok, reason, passed, failed);
    }

    // G6: non-default camera affects extrude render, while default-camera output stays byte-identical.
    {
        QString reason;
        bool ok = true;
        const QSize size(192, 128);

        Text3DLayer defaultLayer;
        configureExtrudedLayerForRenderGate(defaultLayer);
        const QImage a = defaultLayer.renderFrame(size, 0.0, Camera3D{});

        Text3DLayer cameraLayer;
        configureExtrudedLayerForRenderGate(cameraLayer);
        cameraLayer.setCamera(makeNonDefaultCamera());
        const QImage b = cameraLayer.renderFrame(size, 0.0, Camera3D{});

        Text3DLayer defaultLayer2;
        configureExtrudedLayerForRenderGate(defaultLayer2);
        const QImage a2 = defaultLayer2.renderFrame(size, 0.0, Camera3D{});

        auto opaquePixels = [](const QImage &img) {
            qint64 n = 0;
            for (int y = 0; y < img.height(); ++y) {
                const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(line[x]) != 0)
                        ++n;
            }
            return n;
        };

        if (a.isNull() || b.isNull() || a2.isNull()) {
            ok = false;
            reason = QStringLiteral("extrude render returned a null image");
        } else if (imageBytesEqual(a, b)) {
            ok = false;
            reason = QStringLiteral("non-default camera did not change extrude render output")
                + QStringLiteral(" (opaque px: default=%1 camera=%2)")
                      .arg(opaquePixels(a)).arg(opaquePixels(b));
        } else if (!imageBytesEqual(a, a2)) {
            ok = false;
            reason = QStringLiteral("default camera extrude render was not byte-identical");
        }

        printGate(6, ok, reason, passed, failed);
    }

    std::printf("text3d-preview: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : failed;
}
