#include "Camera3D.h"
#include "Text3DLayer.h"

#include <QByteArray>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QVector3D>
#include <cstdio>

namespace {

bool cameraEquals(const Camera3D &a, const Camera3D &b)
{
    return a.toJson() == b.toJson();
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

    std::printf("text3d-preview: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : failed;
}
