#include "GpuVendorSupport.h"
#include "CodecDetector.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gpuvendor {

namespace {

constexpr const char *kSettingsGroup = "GpuVendorSupport";

#ifdef Q_OS_WIN
bool canLoadSystemDll(const wchar_t *dllName)
{
    HMODULE mod = LoadLibraryExW(dllName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod)
        mod = LoadLibraryW(dllName);
    if (!mod)
        return false;
    FreeLibrary(mod);
    return true;
}
#endif

} // namespace

GpuVendor classifyGpuVendor(const QString &glVendor, const QString &glRenderer)
{
    const QString combined = (glVendor + QLatin1Char(' ') + glRenderer).toLower();
    if (combined.contains(QStringLiteral("llvmpipe"))
        || combined.contains(QStringLiteral("swiftshader"))
        || combined.contains(QStringLiteral("microsoft basic render"))
        || combined.contains(QStringLiteral("gdi generic"))) {
        return GpuVendor::Software;
    }
    if (combined.contains(QStringLiteral("amd"))
        || combined.contains(QStringLiteral("ati technologies"))
        || combined.contains(QStringLiteral("radeon"))) {
        return GpuVendor::AMD;
    }
    if (combined.contains(QStringLiteral("nvidia"))
        || combined.contains(QStringLiteral("geforce"))
        || combined.contains(QStringLiteral("quadro"))
        || combined.contains(QStringLiteral("rtx"))) {
        return GpuVendor::NVIDIA;
    }
    if (combined.contains(QStringLiteral("intel"))
        || combined.contains(QStringLiteral("iris"))
        || combined.contains(QStringLiteral("uhd graphics"))
        || combined.contains(QStringLiteral("arc"))) {
        return GpuVendor::Intel;
    }
    return GpuVendor::Unknown;
}

AmdArchitecture classifyAmdArchitecture(const QString &glRenderer)
{
    const QString r = glRenderer.toLower();
    if (!r.contains(QStringLiteral("radeon")) && !r.contains(QStringLiteral("amd")))
        return AmdArchitecture::Unknown;

    // RDNA 4: RX 9000 series (RX 9070, RX 9060, etc.)
    if (QRegularExpression(QStringLiteral("\\brx\\s*9\\d{3}\\b")).match(r).hasMatch()
        || r.contains(QStringLiteral("gfx12"))) {
        return AmdArchitecture::RDNA4;
    }
    // RDNA 3 / 3.5: RX 7000 series, Radeon 740M/760M/780M/860M/880M/890M, Strix/Phoenix/Hawk
    if (QRegularExpression(QStringLiteral("\\brx\\s*7\\d{3}\\b")).match(r).hasMatch()
        || QRegularExpression(QStringLiteral("\\b[78]\\d{2}m\\b")).match(r).hasMatch()
        || r.contains(QStringLiteral("gfx11"))) {
        return AmdArchitecture::RDNA3;
    }
    // RDNA 2: RX 6000 series, Radeon 660M/680M, Rembrandt/Raphael/Mendocino
    if (QRegularExpression(QStringLiteral("\\brx\\s*6\\d{3}\\b")).match(r).hasMatch()
        || QRegularExpression(QStringLiteral("\\b6\\d{2}m\\b")).match(r).hasMatch()
        || r.contains(QStringLiteral("gfx103"))) {
        return AmdArchitecture::RDNA2;
    }
    // RDNA 1: RX 5000 series
    if (QRegularExpression(QStringLiteral("\\brx\\s*5\\d{3}\\b")).match(r).hasMatch()
        || r.contains(QStringLiteral("gfx101"))) {
        return AmdArchitecture::RDNA1;
    }
    // Vega / Polaris / GCN (RX 400/500, Vega 8/10/11/56/64, Radeon(TM) Graphics)
    if (r.contains(QStringLiteral("vega"))
        || QRegularExpression(QStringLiteral("\\brx\\s*[45]\\d{2}\\b")).match(r).hasMatch()
        || r.contains(QStringLiteral("radeon"))) {
        return AmdArchitecture::VegaOrGcn;
    }
    return AmdArchitecture::Unknown;
}

QString vendorDisplayName(GpuVendor vendor)
{
    switch (vendor) {
    case GpuVendor::AMD:      return QStringLiteral("AMD Radeon / Ryzen iGPU");
    case GpuVendor::NVIDIA:   return QStringLiteral("NVIDIA GeForce / RTX");
    case GpuVendor::Intel:    return QStringLiteral("Intel Arc / Iris / UHD");
    case GpuVendor::Software: return QStringLiteral("Software Rasterizer");
    case GpuVendor::Unknown:  return QStringLiteral("Unknown GPU");
    }
    return QStringLiteral("Unknown GPU");
}

QString amdArchDisplayName(AmdArchitecture arch)
{
    switch (arch) {
    case AmdArchitecture::RDNA4:     return QStringLiteral("RDNA 4 (RX 9000)");
    case AmdArchitecture::RDNA3:     return QStringLiteral("RDNA 3 / 3.5 (RX 7000 / 780M-890M)");
    case AmdArchitecture::RDNA2:     return QStringLiteral("RDNA 2 (RX 6000 / 680M)");
    case AmdArchitecture::RDNA1:     return QStringLiteral("RDNA 1 (RX 5000)");
    case AmdArchitecture::VegaOrGcn: return QStringLiteral("Vega / GCN");
    case AmdArchitecture::Unknown:   return QStringLiteral("N/A");
    }
    return QStringLiteral("N/A");
}

D3d11vaCapabilities queryD3d11vaCapabilities(GpuVendor vendor, AmdArchitecture amdArch)
{
    D3d11vaCapabilities c;
    if (vendor == GpuVendor::Software || vendor == GpuVendor::Unknown) {
        c.h264Decode = false;
        c.hevc8Decode = false;
        c.hevc10Decode = false;
        c.av1Decode = false;
        c.av1EncodeSupported = false;
        c.fallbackNote = QStringLiteral("ソフトウェアデコードへ自動フォールバック");
        return c;
    }

    if (vendor == GpuVendor::AMD) {
        c.h264Decode = true;
        c.hevc8Decode = true;
        c.hevc10Decode = true; // Polaris/Vega/RDNA1+ all support HEVC Main10 decode
        c.av1Decode = (amdArch == AmdArchitecture::RDNA2
                       || amdArch == AmdArchitecture::RDNA3
                       || amdArch == AmdArchitecture::RDNA4);
        c.av1EncodeSupported = (amdArch == AmdArchitecture::RDNA3
                                || amdArch == AmdArchitecture::RDNA4);
        if (!c.av1Decode) {
            c.fallbackNote = QStringLiteral("RDNA 1 / Vega 世代では AV1 デコードを自動でソフトウェアフォールバックします");
        } else if (!c.av1EncodeSupported) {
            c.fallbackNote = QStringLiteral("RDNA 2 世代は AV1 デコード対応（AV1 エンコードは RDNA 3+ または SW フォールバック）");
        } else {
            c.fallbackNote = QStringLiteral("H.264 / HEVC 10-bit / AV1 ハードウェアデコード＆AMF エンコード対応世代");
        }
        return c;
    }

    if (vendor == GpuVendor::NVIDIA || vendor == GpuVendor::Intel) {
        c.h264Decode = true;
        c.hevc8Decode = true;
        c.hevc10Decode = true;
        c.av1Decode = true;
        c.av1EncodeSupported = true;
        c.fallbackNote = QStringLiteral("D3D11VA ハードウェアデコード対応（非対応プロファイル時は自動 SW フォールバック）");
        return c;
    }
    return c;
}

bool shouldUseD3d11vaForCodec(const QString &codecName,
                              bool isTenBit,
                              const D3d11vaCapabilities &caps)
{
    const QString c = codecName.trimmed().toLower();
    if (c == QLatin1String("h264") || c == QLatin1String("avc"))
        return !isTenBit && caps.h264Decode; // D3D11VA H.264 Hi10P is universally unsupported in HW
    if (c == QLatin1String("hevc") || c == QLatin1String("h265"))
        return isTenBit ? caps.hevc10Decode : caps.hevc8Decode;
    if (c == QLatin1String("av1"))
        return caps.av1Decode;
    return false;
}

WglDxInteropMode wglDxInteropMode()
{
    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    const int raw = s.value(QStringLiteral("WglDxInteropMode"), 0).toInt();
    s.endGroup();
    if (raw < 0 || raw > 2)
        return WglDxInteropMode::Auto;
    return static_cast<WglDxInteropMode>(raw);
}

void setWglDxInteropMode(WglDxInteropMode mode)
{
    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    s.setValue(QStringLiteral("WglDxInteropMode"), static_cast<int>(mode));
    s.endGroup();
    s.sync();
}

WglDxInteropMode resolveEffectiveInteropMode(const QByteArray &envOverride,
                                             WglDxInteropMode storedSetting)
{
    const QByteArray trimmed = envOverride.trimmed().toLower();
    if (trimmed == "0" || trimmed == "off" || trimmed == "false" || trimmed == "cpu")
        return WglDxInteropMode::ForceSafeCpu;
    if (trimmed == "1" || trimmed == "on" || trimmed == "force")
        return WglDxInteropMode::ForceEnable;
    if (trimmed == "auto")
        return WglDxInteropMode::Auto;
    return storedSetting;
}

WglDxInteropCircuitBreaker::WglDxInteropCircuitBreaker(GpuVendor vendor)
    : m_vendor(vendor)
{
}

void WglDxInteropCircuitBreaker::setVendor(GpuVendor vendor)
{
    m_vendor = vendor;
}

int WglDxInteropCircuitBreaker::failureThreshold() const
{
    // AMD Adrenalin drivers are stricter with WGL_NV_DX_interop2 handle churn:
    // trip quickly (2 consecutive failures) to avoid texture leaks or driver TDR.
    return (m_vendor == GpuVendor::AMD) ? 2 : 3;
}

void WglDxInteropCircuitBreaker::recordSuccess()
{
    m_consecutiveFailures = 0;
}

bool WglDxInteropCircuitBreaker::recordFailure(const QString &reason)
{
    ++m_consecutiveFailures;
    m_lastReason = reason;
    if (m_consecutiveFailures >= failureThreshold())
        m_tripped = true;
    return m_tripped;
}

void WglDxInteropCircuitBreaker::reset()
{
    m_consecutiveFailures = 0;
    m_tripped = false;
    m_lastReason.clear();
}

// ============================================================================
// Phase 3: Cross-vendor GLSL Strictness Static Auditor
// ============================================================================

GlslAuditResult auditGlslShaderForStrictDrivers(const QString &source,
                                                const QString &shaderName)
{
    GlslAuditResult res;
    res.shaderName = shaderName;

    const QStringList lines = source.split(QLatin1Char('\n'));
    const QRegularExpression verRe(QStringLiteral("^\\s*#version\\s+(\\d+)"));
    // Strict GLSL (AMD Adrenalin) rejects implicit int literals in float-only overloads
    // such as pow(x, 2) or clamp(x, 0, 1) or mix(a, b, 0).
    const QRegularExpression strictFuncRe(
        QStringLiteral("\\b(pow|clamp|smoothstep|mix)\\s*\\(([^\\)]*)\\)"));
    const QRegularExpression bareIntRe(
        QStringLiteral("(^|[,\\s\\(])([0-9]+)(?=[,\\s\\)]|$)"));

    bool foundVersion = false;
    bool isCore330 = false;
    for (int idx = 0; idx < lines.size(); ++idx) {
        const int lineNo = idx + 1;
        QString line = lines[idx];
        // Strip single-line comments
        const int cmt = line.indexOf(QStringLiteral("//"));
        if (cmt >= 0)
            line = line.left(cmt);

        if (!foundVersion) {
            const auto m = verRe.match(line);
            if (m.hasMatch()) {
                foundVersion = true;
                res.version = m.captured(1).toInt();
                isCore330 = (res.version >= 330);
            }
        }

        if (isCore330) {
            if (line.contains(QRegularExpression(QStringLiteral("\\btexture2D\\s*\\(")))) {
                res.issues.append({lineNo, QStringLiteral("DEPRECATED_TEXTURE2D"),
                                   QStringLiteral("#version 330 core で廃止済みの texture2D() が使用されています")});
            }
            if (line.contains(QRegularExpression(QStringLiteral("\\bgl_FragColor\\b")))) {
                res.issues.append({lineNo, QStringLiteral("DEPRECATED_FRAGCOLOR"),
                                   QStringLiteral("#version 330 core で廃止済みの gl_FragColor が使用されています")});
            }
        } else if (foundVersion && res.version <= 120) {
            if (line.contains(QRegularExpression(QStringLiteral("\\btexture\\s*\\(")))) {
                res.issues.append({lineNo, QStringLiteral("GLSL120_TEXTURE_FUNC"),
                                   QStringLiteral("#version 120 では texture() ではなく texture2D() を使用してください")});
            }
        }

        // Check float built-ins for bare integer literal arguments (AMD strict GLSL trap)
        auto it = strictFuncRe.globalMatch(line);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString fn = m.captured(1);
            const QString args = m.captured(2);
            // Ignore nested calls to avoid false positives; check simple top-level comma args
            if (args.contains(QLatin1Char('(')))
                continue;
            const QStringList parts = args.split(QLatin1Char(','));
            for (const QString &rawPart : parts) {
                const QString p = rawPart.trimmed();
                if (QRegularExpression(QStringLiteral("^-?\\d+$")).match(p).hasMatch()) {
                    res.issues.append({
                        lineNo,
                        QStringLiteral("IMPLICIT_INT_TO_FLOAT"),
                        QStringLiteral("AMD 厳格 GLSL 違反: %1() の引数 '%2' は浮動小数点リテラル (%2.0) にしてください")
                            .arg(fn, p)
                    });
                }
            }
        }
    }

    if (!foundVersion) {
        res.issues.append({1, QStringLiteral("MISSING_VERSION"),
                           QStringLiteral("#version ディレクティブが欠落しています")});
    }

    res.ok = res.issues.isEmpty();
    return res;
}

// ============================================================================
// Phase 4: Non-CUDA AI Compute Backend Discovery & Selection
// ============================================================================

QString aiBackendDisplayName(AiComputeBackend backend)
{
    switch (backend) {
    case AiComputeBackend::Auto:     return QStringLiteral("自動選択 (GPU ベンダー最適化)");
    case AiComputeBackend::CUDA:     return QStringLiteral("NVIDIA CUDA");
    case AiComputeBackend::DirectML: return QStringLiteral("DirectML (AMD / Intel / NVIDIA 共通)");
    case AiComputeBackend::Vulkan:   return QStringLiteral("Vulkan Compute (クロスベンダー)");
    case AiComputeBackend::ROCm:     return QStringLiteral("AMD ROCm / HIP");
    case AiComputeBackend::CPU:      return QStringLiteral("CPU (フォールバック)");
    }
    return QStringLiteral("CPU");
}

QString aiBackendId(AiComputeBackend backend)
{
    switch (backend) {
    case AiComputeBackend::Auto:     return QStringLiteral("auto");
    case AiComputeBackend::CUDA:     return QStringLiteral("cuda");
    case AiComputeBackend::DirectML: return QStringLiteral("directml");
    case AiComputeBackend::Vulkan:   return QStringLiteral("vulkan");
    case AiComputeBackend::ROCm:     return QStringLiteral("rocm");
    case AiComputeBackend::CPU:      return QStringLiteral("cpu");
    }
    return QStringLiteral("cpu");
}

AiComputeBackend aiBackendFromId(const QString &id)
{
    const QString k = id.trimmed().toLower();
    if (k == QLatin1String("cuda"))     return AiComputeBackend::CUDA;
    if (k == QLatin1String("directml")) return AiComputeBackend::DirectML;
    if (k == QLatin1String("vulkan"))   return AiComputeBackend::Vulkan;
    if (k == QLatin1String("rocm"))     return AiComputeBackend::ROCm;
    if (k == QLatin1String("cpu"))      return AiComputeBackend::CPU;
    return AiComputeBackend::Auto;
}

QVector<AiComputeBackend> probeAvailableAiBackends()
{
    QVector<AiComputeBackend> avail;
#ifdef Q_OS_WIN
    if (canLoadSystemDll(L"nvcuda.dll"))
        avail.append(AiComputeBackend::CUDA);
    if (canLoadSystemDll(L"DirectML.dll"))
        avail.append(AiComputeBackend::DirectML);
    if (canLoadSystemDll(L"vulkan-1.dll"))
        avail.append(AiComputeBackend::Vulkan);
    if (canLoadSystemDll(L"amdhip64.dll") || canLoadSystemDll(L"amdhip64_6.dll"))
        avail.append(AiComputeBackend::ROCm);
#else
    if (QFileInfo::exists(QStringLiteral("/usr/lib/x86_64-linux-gnu/libvulkan.so.1"))
        || QFileInfo::exists(QStringLiteral("/usr/lib/libvulkan.so.1"))) {
        avail.append(AiComputeBackend::Vulkan);
    }
    if (QFileInfo::exists(QStringLiteral("/opt/rocm/lib/libamdhip64.so")))
        avail.append(AiComputeBackend::ROCm);
#endif
    avail.append(AiComputeBackend::CPU);
    return avail;
}

AiComputeBackend selectBestAiBackend(GpuVendor vendor,
                                     const QVector<AiComputeBackend> &available,
                                     AiComputeBackend userPreference)
{
    auto has = [&](AiComputeBackend b) { return available.contains(b); };

    if (userPreference != AiComputeBackend::Auto) {
        if (userPreference == AiComputeBackend::CPU || has(userPreference))
            return userPreference;
    }

    QVector<AiComputeBackend> order;
    if (vendor == GpuVendor::AMD) {
        // Radeon / Ryzen iGPU: DirectML first on Windows, then Vulkan Compute, then ROCm, never CUDA.
        order = {AiComputeBackend::DirectML, AiComputeBackend::Vulkan, AiComputeBackend::ROCm, AiComputeBackend::CPU};
    } else if (vendor == GpuVendor::NVIDIA) {
        order = {AiComputeBackend::CUDA, AiComputeBackend::DirectML, AiComputeBackend::Vulkan, AiComputeBackend::CPU};
    } else if (vendor == GpuVendor::Intel) {
        order = {AiComputeBackend::DirectML, AiComputeBackend::Vulkan, AiComputeBackend::CPU};
    } else {
        order = {AiComputeBackend::DirectML, AiComputeBackend::Vulkan, AiComputeBackend::CPU};
    }

    for (AiComputeBackend cand : order) {
        if (cand == AiComputeBackend::CPU || has(cand))
            return cand;
    }
    return AiComputeBackend::CPU;
}

AiComputeBackend preferredAiBackend()
{
    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    const QString id = s.value(QStringLiteral("PreferredAiBackend"), QStringLiteral("auto")).toString();
    s.endGroup();
    return aiBackendFromId(id);
}

void setPreferredAiBackend(AiComputeBackend backend)
{
    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    s.setValue(QStringLiteral("PreferredAiBackend"), aiBackendId(backend));
    s.endGroup();
    s.sync();
}

void clearSettingsForTest()
{
    QSettings s;
    s.remove(QLatin1String(kSettingsGroup));
    s.sync();
}

// ============================================================================
// Phase 5: Host GPU Diagnostics & Radeon Verification Summary
// ============================================================================

GpuEnvironmentInfo gatherHostGpuInfo(const QString &glVendor,
                                     const QString &glRenderer,
                                     const QString &glVersion)
{
    GpuEnvironmentInfo info;
    info.glVendor = glVendor;
    info.glRenderer = glRenderer;
    info.glVersion = glVersion;
    info.vendor = classifyGpuVendor(glVendor, glRenderer);
    info.amdArch = (info.vendor == GpuVendor::AMD)
        ? classifyAmdArchitecture(glRenderer) : AmdArchitecture::Unknown;
    info.d3d11va = queryD3d11vaCapabilities(info.vendor, info.amdArch);
    info.availableAiBackends = probeAvailableAiBackends();
    info.selectedAiBackend = selectBestAiBackend(info.vendor, info.availableAiBackends, preferredAiBackend());
    info.interopMode = resolveEffectiveInteropMode(qgetenv("VEDITOR_WGL_DX_INTEROP"), wglDxInteropMode());

    info.h264Amf = CodecDetector::isEncoderAvailable(QStringLiteral("h264_amf"));
    info.hevcAmf = CodecDetector::isEncoderAvailable(QStringLiteral("hevc_amf"));
    info.hevcAmf10Bit = CodecDetector::isTenBitEncoderAvailable(QStringLiteral("hevc_amf"));
    info.av1Amf = CodecDetector::isEncoderAvailable(QStringLiteral("av1_amf"));
    return info;
}

QString buildDiagnosticsReportText(const GpuEnvironmentInfo &info)
{
    QStringList availNames;
    for (AiComputeBackend b : info.availableAiBackends)
        availNames.append(aiBackendId(b));

    auto yn = [](bool v) { return v ? QStringLiteral("対応 / 利用可能") : QStringLiteral("未対応 / 利用不可"); };

    QString out;
    out += QStringLiteral("=== GPU / AMD Radeon 対応診断レポート ===\n");
    out += QStringLiteral("GPU ベンダー : %1\n").arg(vendorDisplayName(info.vendor));
    if (!info.glRenderer.isEmpty())
        out += QStringLiteral("GL_RENDERER  : %1 (%2)\n").arg(info.glRenderer, info.glVendor);
    if (info.vendor == GpuVendor::AMD)
        out += QStringLiteral("AMD 世代     : %1\n").arg(amdArchDisplayName(info.amdArch));

    out += QStringLiteral("\n[Phase 1: AMD AMF ハードウェアエンコード]\n");
    out += QStringLiteral("  h264_amf       : %1\n").arg(yn(info.h264Amf));
    out += QStringLiteral("  hevc_amf (8b)  : %1\n").arg(yn(info.hevcAmf));
    out += QStringLiteral("  hevc_amf (10b) : %1\n").arg(yn(info.hevcAmf10Bit));
    out += QStringLiteral("  av1_amf        : %1\n").arg(yn(info.av1Amf));

    out += QStringLiteral("\n[Phase 2: D3D11VA デコード & WGL-DX プレビュー]\n");
    out += QStringLiteral("  H.264 / HEVC 8b / HEVC 10b / AV1 Decode: %1 / %2 / %3 / %4\n")
               .arg(info.d3d11va.h264Decode ? 1 : 0)
               .arg(info.d3d11va.hevc8Decode ? 1 : 0)
               .arg(info.d3d11va.hevc10Decode ? 1 : 0)
               .arg(info.d3d11va.av1Decode ? 1 : 0);
    out += QStringLiteral("  ポリシー備考   : %1\n").arg(info.d3d11va.fallbackNote);
    out += QStringLiteral("  WGL-DX モード  : %1\n").arg(
        info.interopMode == WglDxInteropMode::Auto ? QStringLiteral("自動 (AMD サーキットブレーカー有効)")
        : (info.interopMode == WglDxInteropMode::ForceEnable ? QStringLiteral("強制有効")
                                                             : QStringLiteral("安全モード (CPU テクスチャアップロード)")));

    out += QStringLiteral("\n[Phase 4: 非 CUDA AI 推論バックエンド (ゼロ肥大化設計)]\n");
    out += QStringLiteral("  検出ランタイム : %1\n").arg(availNames.join(QStringLiteral(", ")));
    out += QStringLiteral("  選択バックエンド: %1\n").arg(aiBackendDisplayName(info.selectedAiBackend));

    out += QStringLiteral("\n[Phase 5: Radeon 実機出荷ゲート項目]\n");
    out += QStringLiteral("  1. --selftest=radeon-amf & --selftest=amd-gpu-support 全 PASS\n");
    out += QStringLiteral("  2. H.264 / HEVC 10-bit HDR10 / AV1 の AMF 実書き出し検証\n");
    out += QStringLiteral("  3. 4K 連続再生・スクラブ時の D3D11VA + WGL-DX 安定性検証\n");
    out += QStringLiteral("  4. gpu-composite-parity / hdr-export16 / matte16-parity 画素パリティ確認\n");
    return out;
}

// ============================================================================
// GpuDiagnosticsDialog
// ============================================================================

GpuDiagnosticsDialog::GpuDiagnosticsDialog(const QString &glVendor,
                                           const QString &glRenderer,
                                           const QString &glVersion,
                                           QWidget *parent)
    : QDialog(parent)
    , m_glVendor(glVendor)
    , m_glRenderer(glRenderer)
    , m_glVersion(glVersion)
{
    setWindowTitle(QStringLiteral("GPU / AMD Radeon 対応診断・設定"));
    resize(580, 480);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    m_interopCombo = new QComboBox(this);
    m_interopCombo->addItem(QStringLiteral("自動 (AMD Adrenalin 保護・自動フォールバック付き)"),
                            static_cast<int>(WglDxInteropMode::Auto));
    m_interopCombo->addItem(QStringLiteral("強制有効 (WGL_NV_DX_interop2)"),
                            static_cast<int>(WglDxInteropMode::ForceEnable));
    m_interopCombo->addItem(QStringLiteral("安全モード (CPU テクスチャアップロード固定)"),
                            static_cast<int>(WglDxInteropMode::ForceSafeCpu));
    m_interopCombo->setCurrentIndex(static_cast<int>(wglDxInteropMode()));
    form->addRow(QStringLiteral("ゼロコピー表示 (WGL-DX):"), m_interopCombo);

    m_aiBackendCombo = new QComboBox(this);
    const AiComputeBackend curAi = preferredAiBackend();
    int curIdx = 0;
    const QVector<AiComputeBackend> allOpts = {
        AiComputeBackend::Auto, AiComputeBackend::DirectML,
        AiComputeBackend::Vulkan, AiComputeBackend::ROCm,
        AiComputeBackend::CUDA, AiComputeBackend::CPU
    };
    for (int i = 0; i < allOpts.size(); ++i) {
        m_aiBackendCombo->addItem(aiBackendDisplayName(allOpts[i]), static_cast<int>(allOpts[i]));
        if (allOpts[i] == curAi)
            curIdx = i;
    }
    m_aiBackendCombo->setCurrentIndex(curIdx);
    form->addRow(QStringLiteral("AI 推論バックエンド:"), m_aiBackendCombo);
    root->addLayout(form);

    m_reportView = new QPlainTextEdit(this);
    m_reportView->setReadOnly(true);
    root->addWidget(m_reportView, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &GpuDiagnosticsDialog::saveAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_interopCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &GpuDiagnosticsDialog::refreshReport);
    connect(m_aiBackendCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &GpuDiagnosticsDialog::refreshReport);

    refreshReport();
}

AiComputeBackend GpuDiagnosticsDialog::selectedAiPreference() const
{
    return static_cast<AiComputeBackend>(m_aiBackendCombo->currentData().toInt());
}

WglDxInteropMode GpuDiagnosticsDialog::selectedInteropMode() const
{
    return static_cast<WglDxInteropMode>(m_interopCombo->currentData().toInt());
}

QString GpuDiagnosticsDialog::reportText() const
{
    return m_reportView ? m_reportView->toPlainText() : QString();
}

void GpuDiagnosticsDialog::refreshReport()
{
    GpuEnvironmentInfo info = gatherHostGpuInfo(m_glVendor, m_glRenderer, m_glVersion);
    info.interopMode = selectedInteropMode();
    info.selectedAiBackend = selectBestAiBackend(
        info.vendor, info.availableAiBackends, selectedAiPreference());
    m_reportView->setPlainText(buildDiagnosticsReportText(info));
}

void GpuDiagnosticsDialog::saveAndAccept()
{
    setWglDxInteropMode(selectedInteropMode());
    setPreferredAiBackend(selectedAiPreference());
    accept();
}

} // namespace gpuvendor
