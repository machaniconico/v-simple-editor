// Wave 29 AMD GPU (Radeon / Ryzen iGPU) Full Support Selftest (10 gates):
//   G1: GPU vendor & AMD RDNA 1/2/3/4 + Ryzen iGPU architecture classification
//   G2: Phase 2 D3D11VA decode capability matrix + safe SW fallback for unsupported profiles
//   G3: Phase 2 WGL-DX Interop mode env/settings resolution (auto / force / safe-cpu)
//   G4: Phase 2 WGL-DX Interop Circuit Breaker (AMD trips at 2, NVIDIA at 3, success resets)
//   G5: Phase 3 GLSL strictness auditor catches AMD Adrenalin traps (int->float, deprecated 330 core)
//   G6: Phase 3 GLSL strictness auditor passes clean #version 330 core & #version 120 shaders
//   G7: Phase 4 AI compute backend selection (AMD -> DirectML/Vulkan/ROCm/CPU, never CUDA)
//   G8: Phase 4 AI backend preference persistence & unavailable fallback
//   G9: Phase 1/2 host GPU environment probe consistency
//   G10: Phase 5 GpuDiagnosticsDialog + report text + clean settings restoration

#include "../GpuVendorSupport.h"
#include "SelftestGate.h"

#include <QString>
#include <QVector>

int runAmdGpuSupportSelftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };

    gpuvendor::clearSettingsForTest();

    // G1: Vendor & RDNA architecture classification
    {
        const auto vAmd = gpuvendor::classifyGpuVendor(
            QStringLiteral("ATI Technologies Inc."), QStringLiteral("AMD Radeon RX 7900 XTX"));
        const auto vNv = gpuvendor::classifyGpuVendor(
            QStringLiteral("NVIDIA Corporation"), QStringLiteral("NVIDIA GeForce RTX 5090"));
        const auto vIntel = gpuvendor::classifyGpuVendor(
            QStringLiteral("Intel"), QStringLiteral("Intel(R) Arc(TM) A770 Graphics"));
        const auto vSw = gpuvendor::classifyGpuVendor(
            QStringLiteral("Microsoft Corporation"), QStringLiteral("Microsoft Basic Render Driver"));

        const auto aRdna4 = gpuvendor::classifyAmdArchitecture(QStringLiteral("AMD Radeon RX 9070 XT"));
        const auto aRdna3 = gpuvendor::classifyAmdArchitecture(QStringLiteral("AMD Radeon(TM) 780M Graphics"));
        const auto aRdna2 = gpuvendor::classifyAmdArchitecture(QStringLiteral("AMD Radeon RX 6800 XT"));
        const auto aRdna1 = gpuvendor::classifyAmdArchitecture(QStringLiteral("AMD Radeon RX 5700 XT"));
        const auto aVega = gpuvendor::classifyAmdArchitecture(QStringLiteral("AMD Radeon(TM) Vega 8 Graphics"));

        const bool ok = (vAmd == gpuvendor::GpuVendor::AMD)
            && (vNv == gpuvendor::GpuVendor::NVIDIA)
            && (vIntel == gpuvendor::GpuVendor::Intel)
            && (vSw == gpuvendor::GpuVendor::Software)
            && (aRdna4 == gpuvendor::AmdArchitecture::RDNA4)
            && (aRdna3 == gpuvendor::AmdArchitecture::RDNA3)
            && (aRdna2 == gpuvendor::AmdArchitecture::RDNA2)
            && (aRdna1 == gpuvendor::AmdArchitecture::RDNA1)
            && (aVega == gpuvendor::AmdArchitecture::VegaOrGcn);
        rec(1, ok, QStringLiteral("vendors=4 archs=5"));
    }

    // G2: D3D11VA capability matrix & codec gating
    {
        const auto rdna3 = gpuvendor::queryD3d11vaCapabilities(
            gpuvendor::GpuVendor::AMD, gpuvendor::AmdArchitecture::RDNA3);
        const auto rdna2 = gpuvendor::queryD3d11vaCapabilities(
            gpuvendor::GpuVendor::AMD, gpuvendor::AmdArchitecture::RDNA2);
        const auto rdna1 = gpuvendor::queryD3d11vaCapabilities(
            gpuvendor::GpuVendor::AMD, gpuvendor::AmdArchitecture::RDNA1);

        const bool ok =
            gpuvendor::shouldUseD3d11vaForCodec(QStringLiteral("h264"), false, rdna3)
            && !gpuvendor::shouldUseD3d11vaForCodec(QStringLiteral("h264"), true, rdna3) // Hi10P -> SW
            && gpuvendor::shouldUseD3d11vaForCodec(QStringLiteral("hevc"), true, rdna3)
            && gpuvendor::shouldUseD3d11vaForCodec(QStringLiteral("av1"), false, rdna3)
            && gpuvendor::shouldUseD3d11vaForCodec(QStringLiteral("av1"), false, rdna2)
            && !gpuvendor::shouldUseD3d11vaForCodec(QStringLiteral("av1"), false, rdna1) // RDNA1 no AV1 decode
            && rdna3.av1EncodeSupported && !rdna2.av1EncodeSupported;
        rec(2, ok, QStringLiteral("rdna3Av1=%1 rdna2Av1=%2 rdna1Av1=%3")
                       .arg(rdna3.av1Decode ? 1 : 0).arg(rdna2.av1Decode ? 1 : 0)
                       .arg(rdna1.av1Decode ? 1 : 0));
    }

    // G3: WGL-DX Interop env/settings mode resolution
    {
        using M = gpuvendor::WglDxInteropMode;
        const bool envOff = gpuvendor::resolveEffectiveInteropMode("0", M::ForceEnable) == M::ForceSafeCpu;
        const bool envOn = gpuvendor::resolveEffectiveInteropMode("1", M::ForceSafeCpu) == M::ForceEnable;
        const bool envAuto = gpuvendor::resolveEffectiveInteropMode("auto", M::ForceSafeCpu) == M::Auto;
        const bool fallbackStored = gpuvendor::resolveEffectiveInteropMode("", M::ForceSafeCpu) == M::ForceSafeCpu;
        rec(3, envOff && envOn && envAuto && fallbackStored,
            QStringLiteral("off=%1 on=%2 auto=%3 stored=%4")
                .arg(envOff ? 1 : 0).arg(envOn ? 1 : 0).arg(envAuto ? 1 : 0).arg(fallbackStored ? 1 : 0));
    }

    // G4: WGL-DX Circuit Breaker (AMD trips at 2, NVIDIA at 3, success resets)
    {
        gpuvendor::WglDxInteropCircuitBreaker amdBr(gpuvendor::GpuVendor::AMD);
        const bool amdFail1 = amdBr.recordFailure(QStringLiteral("err1"));
        amdBr.recordSuccess(); // resets counter
        const bool amdFailAfterReset = amdBr.recordFailure(QStringLiteral("err2"));
        const bool amdTripsAt2 = amdBr.recordFailure(QStringLiteral("err3"));

        gpuvendor::WglDxInteropCircuitBreaker nvBr(gpuvendor::GpuVendor::NVIDIA);
        const bool nvFail1 = nvBr.recordFailure();
        const bool nvFail2 = nvBr.recordFailure();
        const bool nvTripsAt3 = nvBr.recordFailure();

        const bool ok = !amdFail1 && !amdFailAfterReset && amdTripsAt2 && amdBr.isTripped()
            && !nvFail1 && !nvFail2 && nvTripsAt3 && nvBr.isTripped();
        rec(4, ok, QStringLiteral("amdThr=%1 nvThr=%2")
                       .arg(amdBr.failureThreshold()).arg(nvBr.failureThreshold()));
    }

    // G5: Phase 3 GLSL strictness auditor catches AMD Adrenalin violations
    {
        const QString bad330 = QStringLiteral(
            "#version 330 core\n"
            "uniform sampler2D uTex;\n"
            "in vec2 vUv;\n"
            "void main() {\n"
            "    vec4 c = texture2D(uTex, vUv);\n"
            "    float g = pow(c.r, 2);\n"
            "    float k = clamp(g, 0, 1);\n"
            "    gl_FragColor = vec4(k);\n"
            "}\n");
        const auto rBad = gpuvendor::auditGlslShaderForStrictDrivers(bad330, QStringLiteral("bad330"));
        bool hasTex2d = false, hasFragColor = false, hasIntFloat = false;
        for (const auto &iss : rBad.issues) {
            if (iss.code == QLatin1String("DEPRECATED_TEXTURE2D")) hasTex2d = true;
            if (iss.code == QLatin1String("DEPRECATED_FRAGCOLOR")) hasFragColor = true;
            if (iss.code == QLatin1String("IMPLICIT_INT_TO_FLOAT")) hasIntFloat = true;
        }
        const auto rNoVer = gpuvendor::auditGlslShaderForStrictDrivers(
            QStringLiteral("void main() {}"), QStringLiteral("noVer"));
        rec(5, !rBad.ok && hasTex2d && hasFragColor && hasIntFloat && !rNoVer.ok,
            QStringLiteral("badIssues=%1 noVerIssues=%2").arg(rBad.issues.size()).arg(rNoVer.issues.size()));
    }

    // G6: Phase 3 GLSL strictness auditor passes clean 330 core & 120 shaders
    {
        const QString clean330 = QStringLiteral(
            "#version 330 core\n"
            "uniform sampler2D uTexture;\n"
            "in vec2 vTexCoord;\n"
            "out vec4 fragColor;\n"
            "void main() {\n"
            "    vec4 c = texture(uTexture, vTexCoord);\n"
            "    vec3 lin = pow(max(c.rgb, vec3(0.0)), vec3(2.2));\n"
            "    fragColor = vec4(clamp(lin, 0.0, 1.0), c.a);\n"
            "}\n");
        const QString clean120 = QStringLiteral(
            "#version 120\n"
            "uniform sampler2D uTex;\n"
            "uniform float uOpacity;\n"
            "varying vec2 vTexCoord;\n"
            "void main() {\n"
            "    vec4 c = texture2D(uTex, vTexCoord);\n"
            "    gl_FragColor = c * uOpacity;\n"
            "}\n");
        const auto r330 = gpuvendor::auditGlslShaderForStrictDrivers(clean330, QStringLiteral("clean330"));
        const auto r120 = gpuvendor::auditGlslShaderForStrictDrivers(clean120, QStringLiteral("clean120"));
        rec(6, r330.ok && r330.version == 330 && r120.ok && r120.version == 120,
            QStringLiteral("ok330=%1 ok120=%2").arg(r330.ok ? 1 : 0).arg(r120.ok ? 1 : 0));
    }

    // G7: Phase 4 AI compute backend selection per vendor (AMD never picks CUDA!)
    {
        using B = gpuvendor::AiComputeBackend;
        const QVector<B> allAvail = {B::CUDA, B::DirectML, B::Vulkan, B::ROCm, B::CPU};
        const QVector<B> vkOnly = {B::Vulkan, B::CPU};
        const QVector<B> rocmOnly = {B::ROCm, B::CPU};

        const B amdDml = gpuvendor::selectBestAiBackend(gpuvendor::GpuVendor::AMD, allAvail, B::Auto);
        const B amdVk = gpuvendor::selectBestAiBackend(gpuvendor::GpuVendor::AMD, vkOnly, B::Auto);
        const B amdRocm = gpuvendor::selectBestAiBackend(gpuvendor::GpuVendor::AMD, rocmOnly, B::Auto);
        const B nvCuda = gpuvendor::selectBestAiBackend(gpuvendor::GpuVendor::NVIDIA, allAvail, B::Auto);
        const B intelDml = gpuvendor::selectBestAiBackend(gpuvendor::GpuVendor::Intel, allAvail, B::Auto);

        const bool ok = (amdDml == B::DirectML)
            && (amdVk == B::Vulkan)
            && (amdRocm == B::ROCm)
            && (nvCuda == B::CUDA)
            && (intelDml == B::DirectML);
        rec(7, ok, QStringLiteral("amd=%1/%2/%3 nv=%4 intel=%5")
                       .arg(gpuvendor::aiBackendId(amdDml), gpuvendor::aiBackendId(amdVk),
                            gpuvendor::aiBackendId(amdRocm), gpuvendor::aiBackendId(nvCuda),
                            gpuvendor::aiBackendId(intelDml)));
    }

    // G8: Phase 4 AI backend user preference persistence & unavailable fallback
    {
        using B = gpuvendor::AiComputeBackend;
        gpuvendor::setPreferredAiBackend(B::Vulkan);
        const B stored = gpuvendor::preferredAiBackend();
        // If user asks for DirectML when only Vulkan+CPU exist, fall back to Vulkan
        const B fellBack = gpuvendor::selectBestAiBackend(
            gpuvendor::GpuVendor::AMD, {B::Vulkan, B::CPU}, B::DirectML);
        // If user asks for CPU explicitly, honor CPU even when DirectML exists
        const B forcedCpu = gpuvendor::selectBestAiBackend(
            gpuvendor::GpuVendor::AMD, {B::DirectML, B::CPU}, B::CPU);
        rec(8, stored == B::Vulkan && fellBack == B::Vulkan && forcedCpu == B::CPU,
            QStringLiteral("stored=%1 fb=%2 cpu=%3")
                .arg(gpuvendor::aiBackendId(stored), gpuvendor::aiBackendId(fellBack),
                     gpuvendor::aiBackendId(forcedCpu)));
    }

    // G9: Host GPU info gathering consistency
    {
        const auto info = gpuvendor::gatherHostGpuInfo(
            QStringLiteral("ATI Technologies Inc."),
            QStringLiteral("AMD Radeon RX 7800 XT"),
            QStringLiteral("4.6.0"));
        const bool ok = (info.vendor == gpuvendor::GpuVendor::AMD)
            && (info.amdArch == gpuvendor::AmdArchitecture::RDNA3)
            && info.d3d11va.av1Decode
            && info.availableAiBackends.contains(gpuvendor::AiComputeBackend::CPU);
        rec(9, ok, QStringLiteral("availAi=%1 sel=%2")
                       .arg(info.availableAiBackends.size())
                       .arg(gpuvendor::aiBackendId(info.selectedAiBackend)));
    }

    // G10: Phase 5 GpuDiagnosticsDialog + report generation + clean settings restore
    {
        gpuvendor::setWglDxInteropMode(gpuvendor::WglDxInteropMode::ForceSafeCpu);
        gpuvendor::setPreferredAiBackend(gpuvendor::AiComputeBackend::DirectML);
        gpuvendor::GpuDiagnosticsDialog dlg(
            QStringLiteral("ATI Technologies Inc."),
            QStringLiteral("AMD Radeon RX 9070 XT"),
            QStringLiteral("4.6.0"));
        const QString report = dlg.reportText();
        const bool ok = (dlg.selectedInteropMode() == gpuvendor::WglDxInteropMode::ForceSafeCpu)
            && (dlg.selectedAiPreference() == gpuvendor::AiComputeBackend::DirectML)
            && report.contains(QStringLiteral("RDNA 4"))
            && report.contains(QStringLiteral("DirectML"))
            && report.contains(QStringLiteral("Phase 5"));

        gpuvendor::clearSettingsForTest();
        const bool resetOk = (gpuvendor::preferredAiBackend() == gpuvendor::AiComputeBackend::Auto)
            && (gpuvendor::wglDxInteropMode() == gpuvendor::WglDxInteropMode::Auto);
        rec(10, ok && resetOk, QStringLiteral("dlg=%1 reset=%2").arg(ok ? 1 : 0).arg(resetOk ? 1 : 0));
    }

    return tally.finish("amd-gpu-support");
}
