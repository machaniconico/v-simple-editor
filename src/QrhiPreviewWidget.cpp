#include "QrhiPreviewWidget.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMutexLocker>
#include <QPainter>
#include <QSettings>
#include <QVBoxLayout>
#include <QtGlobal>

#include <cmath>
#include <memory>

#if defined(VEDITOR_HAVE_QRHI) && __has_include(<rhi/qrhi.h>)
#include <rhi/qrhi.h>
#define VEDITOR_QRHI_ENABLED 1
#else
#define VEDITOR_QRHI_ENABLED 0
#endif

namespace qrhipreview {

namespace {

constexpr const char *kSettingsGroup = "QrhiPreview";

#if VEDITOR_QRHI_ENABLED
std::unique_ptr<QRhi> createOffscreenRhi(PreviewBackend backend,
                                         bool useNullBackendForTest,
                                         QString *errorOut)
{
    if (useNullBackendForTest) {
        QRhiNullInitParams params;
        std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Null, &params));
        if (!rhi && errorOut)
            *errorOut = QStringLiteral("QRhi::Null の初期化に失敗しました。");
        return rhi;
    }

#if defined(Q_OS_WIN)
    if (backend == PreviewBackend::RhiD3D11 || backend == PreviewBackend::Auto) {
        QRhiD3D11InitParams params;
        std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::D3D11, &params));
        if (rhi)
            return rhi;
        if (errorOut)
            *errorOut = QStringLiteral("QRhi::D3D11 の作成に失敗しました。");
        return nullptr;
    }
#endif

    if (backend == PreviewBackend::RhiVulkan) {
        if (errorOut)
            *errorOut = QStringLiteral("ヘッドレス Vulkan インスタンス未構成（UI ウィンドウ経由で初期化してください）。");
        return nullptr;
    }

    // Fallback: Null probe if OpenGL context is not current on this thread.
    QRhiNullInitParams params;
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Null, &params));
    if (!rhi && errorOut)
        *errorOut = QStringLiteral("QRhi フォールバック初期化に失敗しました。");
    return rhi;
}
#endif

} // namespace

QString backendDisplayName(PreviewBackend backend)
{
    switch (backend) {
    case PreviewBackend::OpenGL:    return QStringLiteral("OpenGL 3.3 (従来既定 / GLPreview)");
    case PreviewBackend::RhiD3D11:  return QStringLiteral("Qt6 QRhi — Direct3D 11 (AMD/Intel/NVIDIA 推奨)");
    case PreviewBackend::RhiVulkan: return QStringLiteral("Qt6 QRhi — Vulkan (クロスベンダー)");
    case PreviewBackend::RhiOpenGL: return QStringLiteral("Qt6 QRhi — OpenGL");
    case PreviewBackend::Auto:      return QStringLiteral("Qt6 QRhi — 自動選択 (ベンダー最適化)");
    }
    return QStringLiteral("OpenGL 3.3");
}

QString backendId(PreviewBackend backend)
{
    switch (backend) {
    case PreviewBackend::OpenGL:    return QStringLiteral("opengl");
    case PreviewBackend::RhiD3D11:  return QStringLiteral("rhi-d3d11");
    case PreviewBackend::RhiVulkan: return QStringLiteral("rhi-vulkan");
    case PreviewBackend::RhiOpenGL: return QStringLiteral("rhi-opengl");
    case PreviewBackend::Auto:      return QStringLiteral("auto");
    }
    return QStringLiteral("opengl");
}

PreviewBackend backendFromId(const QString &id)
{
    const QString k = id.trimmed().toLower();
    if (k == QLatin1String("rhi-d3d11") || k == QLatin1String("d3d11"))
        return PreviewBackend::RhiD3D11;
    if (k == QLatin1String("rhi-vulkan") || k == QLatin1String("vulkan"))
        return PreviewBackend::RhiVulkan;
    if (k == QLatin1String("rhi-opengl"))
        return PreviewBackend::RhiOpenGL;
    if (k == QLatin1String("auto"))
        return PreviewBackend::Auto;
    return PreviewBackend::OpenGL;
}

PreviewBackend preferredPreviewBackend()
{
    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    const QString id = s.value(QStringLiteral("Backend"), QStringLiteral("opengl")).toString();
    s.endGroup();
    return backendFromId(id);
}

void setPreferredPreviewBackend(PreviewBackend backend)
{
    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    s.setValue(QStringLiteral("Backend"), backendId(backend));
    s.endGroup();
    s.sync();
}

PreviewBackend resolveEffectivePreviewBackend(const QByteArray &envOverride,
                                              PreviewBackend stored)
{
    const QString env = QString::fromUtf8(envOverride).trimmed().toLower();
    if (!env.isEmpty())
        return backendFromId(env);
    return stored;
}

void clearSettingsForTest()
{
    QSettings s;
    s.remove(QLatin1String(kSettingsGroup));
    s.sync();
}

PreviewBackend recommendedQrhiBackendForVendor(gpuvendor::GpuVendor vendor, bool isWindows)
{
    if (isWindows) {
        // Direct3D 11 via QRhi avoids WGL_NV_DX_interop2 entirely on AMD Adrenalin,
        // Intel Arc/Iris, and NVIDIA alike.
        if (vendor == gpuvendor::GpuVendor::Software)
            return PreviewBackend::OpenGL;
        return PreviewBackend::RhiD3D11;
    }
    if (vendor == gpuvendor::GpuVendor::AMD || vendor == gpuvendor::GpuVendor::NVIDIA
        || vendor == gpuvendor::GpuVendor::Intel) {
        return PreviewBackend::RhiVulkan;
    }
    return PreviewBackend::RhiOpenGL;
}

QrhiProbeInfo probeQrhiBackend(PreviewBackend backend, bool useNullBackendForTest)
{
    QrhiProbeInfo info;
    info.requestedBackend = backend;
#if VEDITOR_QRHI_ENABLED
    info.compiledWithQrhi = true;
    std::unique_ptr<QRhi> rhi = createOffscreenRhi(backend, useNullBackendForTest, &info.error);
    if (!rhi)
        return info;
    info.available = true;
    info.backendName = QString::fromLatin1(rhi->backendName());
    const QRhiDriverInfo drv = rhi->driverInfo();
    info.deviceName = QString::fromUtf8(drv.deviceName);
    info.vendorId = drv.vendorId;
    info.deviceId = drv.deviceId;
    info.supportsRgba16F = rhi->isTextureFormatSupported(QRhiTexture::RGBA16F);
    info.supportsRgb10A2 = rhi->isTextureFormatSupported(QRhiTexture::RGB10A2);
    info.maxTextureSize = rhi->resourceLimit(QRhi::TextureSizeMax);
#else
    info.compiledWithQrhi = false;
    info.error = QStringLiteral("Qt6::GuiPrivate (QRhi) なしでビルドされています。");
#endif
    return info;
}

QrhiRoundtripResult runQrhiUploadReadbackSmoke(PreviewBackend backend,
                                               const QImage &testImage,
                                               bool useNullBackendForTest)
{
    QrhiRoundtripResult res;
    if (testImage.isNull() || testImage.width() <= 0 || testImage.height() <= 0) {
        res.error = QStringLiteral("入力テスト画像が空です。");
        return res;
    }
#if VEDITOR_QRHI_ENABLED
    const QImage rgba = testImage.convertToFormat(QImage::Format_RGBA8888);
    std::unique_ptr<QRhi> rhi = createOffscreenRhi(backend, useNullBackendForTest, &res.error);
    if (!rhi)
        return res;
    res.backendName = QString::fromLatin1(rhi->backendName());
    res.deviceName = QString::fromUtf8(rhi->driverInfo().deviceName);

    std::unique_ptr<QRhiTexture> tex(
        rhi->newTexture(QRhiTexture::RGBA8, rgba.size(), 1, QRhiTexture::UsedAsTransferSource));
    if (!tex || !tex->create()) {
        res.error = QStringLiteral("QRhiTexture の作成に失敗しました。");
        return res;
    }

    QRhiCommandBuffer *cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb) {
        res.error = QStringLiteral("beginOffscreenFrame に失敗しました。");
        return res;
    }

    QRhiReadbackResult rb;
    bool rbCompleted = false;
    rb.completed = [&rbCompleted]() { rbCompleted = true; };

    QRhiResourceUpdateBatch *u = rhi->nextResourceUpdateBatch();
    u->uploadTexture(tex.get(), rgba);
    u->readBackTexture(QRhiReadbackDescription(tex.get()), &rb);
    cb->resourceUpdate(u);

    if (rhi->endOffscreenFrame() != QRhi::FrameOpSuccess) {
        res.error = QStringLiteral("endOffscreenFrame に失敗しました。");
        return res;
    }
    rhi->finish();

    if (!rbCompleted || rb.data.isEmpty()) {
        // QRhi::Null does not produce real GPU readback bytes; verify command submission succeeded.
        if (useNullBackendForTest) {
            res.ok = true;
            res.maxChannelDiff = 0.0;
            return res;
        }
        res.error = QStringLiteral("QRhi readBackTexture がデータを返しませんでした。");
        return res;
    }

    const int expectedBytes = rgba.width() * rgba.height() * 4;
    if (rb.data.size() < expectedBytes) {
        res.error = QStringLiteral("リードバックサイズが不足しています。");
        return res;
    }

    double maxDiff = 0.0;
    const auto *got = reinterpret_cast<const uchar *>(rb.data.constData());
    const int rowStride = rb.data.size() / rgba.height();
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *srcLine = rgba.constScanLine(y);
        const uchar *dstLine = got + y * rowStride;
        for (int x = 0; x < rgba.width() * 4; ++x) {
            const double d = std::abs(int(srcLine[x]) - int(dstLine[x]));
            if (d > maxDiff)
                maxDiff = d;
        }
    }
    res.maxChannelDiff = maxDiff;
    res.ok = (maxDiff <= 1.0);
    return res;
#else
    Q_UNUSED(backend);
    Q_UNUSED(useNullBackendForTest);
    res.error = QStringLiteral("QRhi なしでビルドされています。");
    return res;
#endif
}

// ============================================================================
// QrhiPreviewWidget
// ============================================================================

QrhiPreviewWidget::QrhiPreviewWidget(PreviewBackend backend, QWidget *parent)
#if VEDITOR_HAVE_QRHI_WIDGET
    : QRhiWidget(parent)
#else
    : QWidget(parent)
#endif
    , m_backend(backend)
{
    setObjectName(QStringLiteral("qrhiPreviewWidget"));
    setMinimumSize(320, 180);
    setPreviewBackend(backend);
}

QrhiPreviewWidget::~QrhiPreviewWidget()
{
#if VEDITOR_HAVE_QRHI_WIDGET && VEDITOR_QRHI_ENABLED
    delete static_cast<QRhiTexture *>(m_uploadTexOpaque);
    m_uploadTexOpaque = nullptr;
#endif
}

void QrhiPreviewWidget::setPreviewBackend(PreviewBackend backend)
{
    m_backend = backend;
#if VEDITOR_HAVE_QRHI_WIDGET
    switch (backend) {
    case PreviewBackend::RhiD3D11:
    case PreviewBackend::Auto:
#ifdef Q_OS_WIN
        setApi(QRhiWidget::Api::Direct3D11);
#else
        setApi(QRhiWidget::Api::Vulkan);
#endif
        break;
    case PreviewBackend::RhiVulkan:
        setApi(QRhiWidget::Api::Vulkan);
        break;
    case PreviewBackend::RhiOpenGL:
    case PreviewBackend::OpenGL:
        setApi(QRhiWidget::Api::OpenGL);
        break;
    }
#endif
    update();
}

void QrhiPreviewWidget::setHdr16BitPreferred(bool enable)
{
    m_preferHdr16 = enable;
#if VEDITOR_HAVE_QRHI_WIDGET
    setColorBufferFormat(enable ? QRhiWidget::TextureFormat::RGBA16F
                                : QRhiWidget::TextureFormat::RGBA8);
#endif
    update();
}

void QrhiPreviewWidget::setFrame(const QImage &frame)
{
    {
        QMutexLocker lock(&m_frameMutex);
        m_pendingFrame = frame.isNull() ? QImage() : frame.convertToFormat(QImage::Format_RGBA8888);
        m_frameDirty = true;
    }
    update();
}

QImage QrhiPreviewWidget::currentFrame() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_pendingFrame;
}

QString QrhiPreviewWidget::activeRhiBackendName() const
{
    return m_activeBackendName;
}

#if VEDITOR_HAVE_QRHI_WIDGET
void QrhiPreviewWidget::initialize(QRhiCommandBuffer *cb)
{
    Q_UNUSED(cb);
#if VEDITOR_QRHI_ENABLED
    if (QRhi *r = rhi())
        m_activeBackendName = QString::fromLatin1(r->backendName());
#endif
}

void QrhiPreviewWidget::render(QRhiCommandBuffer *cb)
{
#if VEDITOR_QRHI_ENABLED
    QRhi *r = rhi();
    QRhiTexture *dstTex = colorTexture();
    if (!r || !cb || !dstTex)
        return;

    QImage frameCopy;
    {
        QMutexLocker lock(&m_frameMutex);
        frameCopy = m_pendingFrame;
        m_frameDirty = false;
    }

    const QSize dstSize = dstTex->pixelSize();
    if (dstSize.isEmpty())
        return;

    // Letterbox-fit the source frame onto a clean RGBA8888 surface matching the color buffer
    // and upload directly into the QRhi color texture via resourceUpdate (vendor-agnostic,
    // zero GLSL compiler or WGL-DX interop surface!).
    QImage canvas(dstSize, QImage::Format_RGBA8888);
    canvas.fill(QColor(16, 18, 24, 255));
    if (!frameCopy.isNull()) {
        const QImage fitted = frameCopy.scaled(dstSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter p(&canvas);
        p.drawImage((dstSize.width() - fitted.width()) / 2,
                    (dstSize.height() - fitted.height()) / 2,
                    fitted);
    }

    QRhiResourceUpdateBatch *u = r->nextResourceUpdateBatch();
    if (dstTex->format() == QRhiTexture::RGBA8) {
        u->uploadTexture(dstTex, canvas);
        cb->resourceUpdate(u);
    } else {
        // For RGBA16F color buffer targets, clear pass cleanly (future shader blit target).
        cb->beginPass(renderTarget(), QColor(16, 18, 24), {1.0f, 0}, u);
        cb->endPass();
    }
#else
    Q_UNUSED(cb);
#endif
}

void QrhiPreviewWidget::releaseResources()
{
#if VEDITOR_QRHI_ENABLED
    delete static_cast<QRhiTexture *>(m_uploadTexOpaque);
    m_uploadTexOpaque = nullptr;
    m_uploadTexSize = QSize();
#endif
}
#endif

// ============================================================================
// QrhiPreviewDialog
// ============================================================================

QrhiPreviewDialog::QrhiPreviewDialog(const QImage &initialFrame, QWidget *parent)
    : QDialog(parent)
    , m_frame(initialFrame)
{
    setWindowTitle(QStringLiteral("Qt6 QRhi クロスベンダ・プレビュー (Direct3D 11 / Vulkan / OpenGL)"));
    resize(660, 480);

    auto *root = new QVBoxLayout(this);
    auto *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel(QStringLiteral("QRhi バックエンド:"), this));
    m_backendCombo = new QComboBox(this);
    m_backendCombo->addItem(backendDisplayName(PreviewBackend::RhiD3D11), static_cast<int>(PreviewBackend::RhiD3D11));
    m_backendCombo->addItem(backendDisplayName(PreviewBackend::RhiVulkan), static_cast<int>(PreviewBackend::RhiVulkan));
    m_backendCombo->addItem(backendDisplayName(PreviewBackend::RhiOpenGL), static_cast<int>(PreviewBackend::RhiOpenGL));
    topRow->addWidget(m_backendCombo, 1);
    root->addLayout(topRow);

    m_infoLabel = new QLabel(this);
    m_infoLabel->setWordWrap(true);
    root->addWidget(m_infoLabel);

    m_widget = new QrhiPreviewWidget(PreviewBackend::RhiD3D11, this);
    m_widget->setFrame(m_frame);
    root->addWidget(m_widget, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_backendCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &QrhiPreviewDialog::onBackendChanged);
    refreshInfo();
}

PreviewBackend QrhiPreviewDialog::selectedBackend() const
{
    return static_cast<PreviewBackend>(m_backendCombo->currentData().toInt());
}

void QrhiPreviewDialog::onBackendChanged(int)
{
    m_widget->setPreviewBackend(selectedBackend());
    refreshInfo();
}

void QrhiPreviewDialog::refreshInfo()
{
    const QrhiProbeInfo probe = probeQrhiBackend(selectedBackend());
    if (probe.available) {
        m_infoLabel->setText(
            QStringLiteral("バックエンド: %1 | デバイス: %2 | RGBA16F: %3 | 最大テクスチャ: %4 px")
                .arg(probe.backendName, probe.deviceName,
                     probe.supportsRgba16F ? QStringLiteral("対応") : QStringLiteral("非対応"))
                .arg(probe.maxTextureSize));
    } else {
        m_infoLabel->setText(QStringLiteral("プローブ状態: %1").arg(probe.error));
    }
}

} // namespace qrhipreview
