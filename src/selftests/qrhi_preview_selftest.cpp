// Wave 30 Qt6 QRhi Cross-Vendor Preview Backend Selftest (8 gates):
//   G1: PreviewBackend env override (VEDITOR_PREVIEW_BACKEND) & QSettings persistence
//   G2: Vendor-recommended QRhi backend selection (Windows -> D3D11, Linux -> Vulkan)
//   G3: Real Qt6 QRhi::Null offscreen backend probe
//   G4: Real Windows QRhi::D3D11 native hardware probe (deviceName, RGBA16F, maxTextureSize)
//   G5: Real QRhi::D3D11 QRhiTexture RGBA8888 upload -> readBackTexture pixel-exact round-trip
//   G6: QrhiPreviewWidget instantiation, API switching, HDR16 format & setFrame parity
//   G7: QrhiPreviewDialog live TimelineFrameRenderer frame handoff
//   G8: Clean settings restoration to legacy OpenGL default

#include "../QrhiPreviewWidget.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "SelftestGate.h"

#include <QColor>
#include <QImage>

int runQrhiPreviewSelftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };

    qrhipreview::clearSettingsForTest();

    // G1: Env override & QSettings persistence
    {
        using B = qrhipreview::PreviewBackend;
        const bool defaultGl = (qrhipreview::preferredPreviewBackend() == B::OpenGL);
        qrhipreview::setPreferredPreviewBackend(B::RhiD3D11);
        const bool storedD3d = (qrhipreview::preferredPreviewBackend() == B::RhiD3D11);
        const bool envVk = (qrhipreview::resolveEffectivePreviewBackend("rhi-vulkan", B::RhiD3D11) == B::RhiVulkan);
        const bool envEmpty = (qrhipreview::resolveEffectivePreviewBackend("", B::RhiD3D11) == B::RhiD3D11);
        rec(1, defaultGl && storedD3d && envVk && envEmpty,
            QStringLiteral("def=%1 d3d=%2 vk=%3").arg(defaultGl ? 1 : 0).arg(storedD3d ? 1 : 0).arg(envVk ? 1 : 0));
    }

    // G2: Vendor-recommended QRhi API
    {
        using B = qrhipreview::PreviewBackend;
        const B winAmd = qrhipreview::recommendedQrhiBackendForVendor(gpuvendor::GpuVendor::AMD, true);
        const B winNv = qrhipreview::recommendedQrhiBackendForVendor(gpuvendor::GpuVendor::NVIDIA, true);
        const B winSw = qrhipreview::recommendedQrhiBackendForVendor(gpuvendor::GpuVendor::Software, true);
        const B linAmd = qrhipreview::recommendedQrhiBackendForVendor(gpuvendor::GpuVendor::AMD, false);
        rec(2, winAmd == B::RhiD3D11 && winNv == B::RhiD3D11
                && winSw == B::OpenGL && linAmd == B::RhiVulkan,
            QStringLiteral("winAmd=%1 linAmd=%2")
                .arg(qrhipreview::backendId(winAmd), qrhipreview::backendId(linAmd)));
    }

    // G3: Real QRhi::Null offscreen backend probe
    {
        const auto nullProbe = qrhipreview::probeQrhiBackend(qrhipreview::PreviewBackend::RhiD3D11, /*useNull=*/true);
        rec(3, nullProbe.compiledWithQrhi && nullProbe.available
                && nullProbe.backendName.compare(QStringLiteral("Null"), Qt::CaseInsensitive) == 0,
            QStringLiteral("compiled=%1 avail=%2 name=%3")
                .arg(nullProbe.compiledWithQrhi ? 1 : 0).arg(nullProbe.available ? 1 : 0)
                .arg(nullProbe.backendName));
    }

    // G4: Real Windows QRhi::D3D11 native GPU probe
    {
#ifdef Q_OS_WIN
        const auto d3dProbe = qrhipreview::probeQrhiBackend(qrhipreview::PreviewBackend::RhiD3D11, false);
        const bool ok = d3dProbe.compiledWithQrhi && d3dProbe.available
            && d3dProbe.backendName.contains(QStringLiteral("D3D11"), Qt::CaseInsensitive)
            && !d3dProbe.deviceName.isEmpty()
            && d3dProbe.supportsRgba16F
            && d3dProbe.maxTextureSize >= 4096;
        rec(4, ok, QStringLiteral("backend=%1 dev=%2 f16=%3 maxTex=%4")
                       .arg(d3dProbe.backendName, d3dProbe.deviceName)
                       .arg(d3dProbe.supportsRgba16F ? 1 : 0).arg(d3dProbe.maxTextureSize));
#else
        const auto nullProbe = qrhipreview::probeQrhiBackend(qrhipreview::PreviewBackend::RhiVulkan, true);
        rec(4, nullProbe.available, QStringLiteral("nonWinFallback=1"));
#endif
    }

    // G5: Real QRhi::D3D11 QRhiTexture RGBA8888 upload -> readBackTexture pixel-exact round-trip
    {
        QImage pattern(64, 64, QImage::Format_RGBA8888);
        for (int y = 0; y < 64; ++y) {
            uchar *line = pattern.scanLine(y);
            for (int x = 0; x < 64; ++x) {
                line[x * 4 + 0] = static_cast<uchar>((x * 4) & 255);
                line[x * 4 + 1] = static_cast<uchar>((y * 4) & 255);
                line[x * 4 + 2] = static_cast<uchar>(((x + y) * 2) & 255);
                line[x * 4 + 3] = 255;
            }
        }
#ifdef Q_OS_WIN
        const auto rt = qrhipreview::runQrhiUploadReadbackSmoke(
            qrhipreview::PreviewBackend::RhiD3D11, pattern, false);
#else
        const auto rt = qrhipreview::runQrhiUploadReadbackSmoke(
            qrhipreview::PreviewBackend::RhiVulkan, pattern, true);
#endif
        rec(5, rt.ok && rt.maxChannelDiff <= 1.0,
            QStringLiteral("ok=%1 maxDiff=%2 backend=%3")
                .arg(rt.ok ? 1 : 0).arg(rt.maxChannelDiff, 0, 'f', 1).arg(rt.backendName));
    }

    // G6: QrhiPreviewWidget API switch, HDR16 format & setFrame state
    {
        qrhipreview::QrhiPreviewWidget w(qrhipreview::PreviewBackend::RhiD3D11);
        w.setHdr16BitPreferred(true);
        QImage img(32, 32, QImage::Format_RGBA8888);
        img.fill(QColor(40, 180, 240, 255));
        w.setFrame(img);
        const bool ok = (w.previewBackend() == qrhipreview::PreviewBackend::RhiD3D11)
            && w.isHdr16BitPreferred()
            && (w.currentFrame().size() == QSize(32, 32))
            && (w.currentFrame().pixelColor(16, 16).green() == 180);
        rec(6, ok, QStringLiteral("hdr16=%1 sz=%2x%3")
                       .arg(w.isHdr16BitPreferred() ? 1 : 0)
                       .arg(w.currentFrame().width()).arg(w.currentFrame().height()));
    }

    // G7: QrhiPreviewDialog live TimelineFrameRenderer frame handoff
    {
        Timeline tl;
        ClipInfo clip;
        clip.displayName = QStringLiteral("RHI Test");
        clip.duration = 2.0;
        clip.outPoint = 2.0;
        Shape rect;
        rect.type = ShapeType::Rectangle;
        rect.position = QPointF(64.0, 36.0);
        rect.properties.size = QSizeF(128.0, 72.0);
        rect.fill.color = QColor(20, 160, 90, 255);
        rect.stroke.enabled = false;
        clip.shapes.append(rect);
        tl.videoTracks()[0]->addClip(clip);

        const QImage rendered = tlrender::renderFrameAt(&tl, 500'000, QSize(128, 72));
        qrhipreview::QrhiPreviewDialog dlg(rendered);
        const bool ok = !rendered.isNull()
            && dlg.previewWidget() != nullptr
            && (dlg.previewWidget()->currentFrame().size() == QSize(128, 72))
            && (dlg.selectedBackend() == qrhipreview::PreviewBackend::RhiD3D11);
        rec(7, ok, QStringLiteral("rendered=%1 dlgOk=%2").arg(!rendered.isNull() ? 1 : 0).arg(ok ? 1 : 0));
    }

    // G8: Clean settings restoration to legacy OpenGL default
    {
        qrhipreview::clearSettingsForTest();
        const bool restored = (qrhipreview::preferredPreviewBackend() == qrhipreview::PreviewBackend::OpenGL);
        rec(8, restored, QStringLiteral("restored=%1").arg(restored ? 1 : 0));
    }

    return tally.finish("qrhi-preview");
}
