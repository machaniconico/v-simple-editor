#pragma once

#include "GpuVendorSupport.h"

#include <QDialog>
#include <QImage>
#include <QMutex>
#include <QSize>
#include <QString>

#if __has_include(<QRhiWidget>)
#include <QRhiWidget>
#define VEDITOR_HAVE_QRHI_WIDGET 1
#else
#include <QWidget>
#define VEDITOR_HAVE_QRHI_WIDGET 0
#endif

class QComboBox;
class QLabel;
class Timeline;

namespace qrhipreview {

enum class PreviewBackend {
    OpenGL = 0,   // Legacy default GLPreview (byte-identical default)
    RhiD3D11,     // Qt6 QRhi Direct3D 11 (vendor-neutral Windows zero-interop path)
    RhiVulkan,    // Qt6 QRhi Vulkan (cross-vendor Windows / Linux)
    RhiOpenGL,    // Qt6 QRhi OpenGL
    Auto          // Select best QRhi backend for detected GPU vendor
};

struct QrhiProbeInfo {
    bool compiledWithQrhi = false;
    bool available = false;
    PreviewBackend requestedBackend = PreviewBackend::RhiD3D11;
    QString backendName;
    QString deviceName;
    quint32 vendorId = 0;
    quint32 deviceId = 0;
    bool supportsRgba16F = false;
    bool supportsRgb10A2 = false;
    int maxTextureSize = 0;
    QString error;
};

struct QrhiRoundtripResult {
    bool ok = false;
    double maxChannelDiff = 255.0;
    QString backendName;
    QString deviceName;
    QString error;
};

QString backendDisplayName(PreviewBackend backend);
QString backendId(PreviewBackend backend);
PreviewBackend backendFromId(const QString &id);

// Persistence & env override (VEDITOR_PREVIEW_BACKEND=opengl|rhi-d3d11|rhi-vulkan|rhi-opengl|auto)
PreviewBackend preferredPreviewBackend();
void setPreferredPreviewBackend(PreviewBackend backend);
PreviewBackend resolveEffectivePreviewBackend(const QByteArray &envOverride,
                                              PreviewBackend stored);
void clearSettingsForTest();

// Vendor-recommended QRhi backend (Windows AMD/Intel/NVIDIA -> D3D11, Linux -> Vulkan).
PreviewBackend recommendedQrhiBackendForVendor(gpuvendor::GpuVendor vendor, bool isWindows);

// Instantiate a real offscreen QRhi instance and query hardware limits & texture format support.
QrhiProbeInfo probeQrhiBackend(PreviewBackend backend, bool useNullBackendForTest = false);

// Real GPU QRhiTexture upload -> readBackTexture pixel round-trip smoke verification.
QrhiRoundtripResult runQrhiUploadReadbackSmoke(PreviewBackend backend,
                                               const QImage &testImage,
                                               bool useNullBackendForTest = false);

#if VEDITOR_HAVE_QRHI_WIDGET
class QrhiPreviewWidget : public QRhiWidget
#else
class QrhiPreviewWidget : public QWidget
#endif
{
    Q_OBJECT
public:
    explicit QrhiPreviewWidget(PreviewBackend backend = PreviewBackend::RhiD3D11,
                               QWidget *parent = nullptr);
    ~QrhiPreviewWidget() override;

    void setPreviewBackend(PreviewBackend backend);
    PreviewBackend previewBackend() const { return m_backend; }

    void setHdr16BitPreferred(bool enable);
    bool isHdr16BitPreferred() const { return m_preferHdr16; }

    void setFrame(const QImage &frame);
    QImage currentFrame() const;
    QString activeRhiBackendName() const;

#if VEDITOR_HAVE_QRHI_WIDGET
protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
#endif

private:
    PreviewBackend m_backend = PreviewBackend::RhiD3D11;
    bool m_preferHdr16 = false;
    mutable QMutex m_frameMutex;
    QImage m_pendingFrame;
    bool m_frameDirty = false;
    QString m_activeBackendName;
    void *m_uploadTexOpaque = nullptr; // QRhiTexture* owned by widget when QRhi active
    QSize m_uploadTexSize;
};

class QrhiPreviewDialog : public QDialog
{
    Q_OBJECT
public:
    explicit QrhiPreviewDialog(const QImage &initialFrame,
                               QWidget *parent = nullptr);

    QrhiPreviewWidget *previewWidget() const { return m_widget; }
    PreviewBackend selectedBackend() const;

private:
    void onBackendChanged(int idx);
    void refreshInfo();

    QImage m_frame;
    QComboBox *m_backendCombo = nullptr;
    QLabel *m_infoLabel = nullptr;
    QrhiPreviewWidget *m_widget = nullptr;
};

} // namespace qrhipreview
