#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

class QComboBox;
class QPlainTextEdit;

namespace gpuvendor {

enum class GpuVendor {
    Unknown = 0,
    AMD,
    NVIDIA,
    Intel,
    Software
};

enum class AmdArchitecture {
    Unknown = 0,
    VegaOrGcn, // RX 400/500 / Vega / legacy Ryzen iGPU
    RDNA1,     // RX 5000 series
    RDNA2,     // RX 6000 series / Ryzen 6000-7000 iGPU (AV1 decode)
    RDNA3,     // RX 7000 series / Radeon 780M/890M (AV1 encode+decode)
    RDNA4      // RX 9000 series (AV1 encode+decode + enhanced 10-bit)
};

enum class WglDxInteropMode {
    Auto = 0,      // Enabled with AMD Adrenalin circuit-breaker protection
    ForceEnable,   // Always attempt WGL_NV_DX_interop2
    ForceSafeCpu   // Disable WGL-DX interop; always use safe CPU texture upload
};

enum class AiComputeBackend {
    Auto = 0,
    CUDA,
    DirectML,
    Vulkan,
    ROCm,
    CPU
};

struct D3d11vaCapabilities {
    bool h264Decode = true;
    bool hevc8Decode = true;
    bool hevc10Decode = false;
    bool av1Decode = false;
    bool av1EncodeSupported = false;
    QString fallbackNote;
};

struct GlslAuditIssue {
    int line = 0;
    QString code;
    QString message;
};

struct GlslAuditResult {
    QString shaderName;
    int version = 0;
    bool ok = true;
    QVector<GlslAuditIssue> issues;
};

struct GpuEnvironmentInfo {
    QString glVendor;
    QString glRenderer;
    QString glVersion;
    GpuVendor vendor = GpuVendor::Unknown;
    AmdArchitecture amdArch = AmdArchitecture::Unknown;
    D3d11vaCapabilities d3d11va;
    QVector<AiComputeBackend> availableAiBackends;
    AiComputeBackend selectedAiBackend = AiComputeBackend::CPU;
    WglDxInteropMode interopMode = WglDxInteropMode::Auto;
    bool h264Amf = false;
    bool hevcAmf = false;
    bool hevcAmf10Bit = false;
    bool av1Amf = false;
};

// Phase 2: Vendor & architecture classification (pure functions, testable without GPU).
GpuVendor classifyGpuVendor(const QString &glVendor, const QString &glRenderer);
AmdArchitecture classifyAmdArchitecture(const QString &glRenderer);
QString vendorDisplayName(GpuVendor vendor);
QString amdArchDisplayName(AmdArchitecture arch);

// Phase 2: D3D11VA decode/encode capability policy per vendor/architecture.
D3d11vaCapabilities queryD3d11vaCapabilities(GpuVendor vendor, AmdArchitecture amdArch);
bool shouldUseD3d11vaForCodec(const QString &codecName,
                              bool isTenBit,
                              const D3d11vaCapabilities &caps);

// Phase 2: WGL-DX Interop mode (QSettings + VEDITOR_WGL_DX_INTEROP env override) & circuit breaker.
WglDxInteropMode wglDxInteropMode();
void setWglDxInteropMode(WglDxInteropMode mode);
WglDxInteropMode resolveEffectiveInteropMode(const QByteArray &envOverride,
                                             WglDxInteropMode storedSetting);

class WglDxInteropCircuitBreaker {
public:
    explicit WglDxInteropCircuitBreaker(GpuVendor vendor = GpuVendor::Unknown);

    void setVendor(GpuVendor vendor);
    void recordSuccess();
    // Returns true when this failure trips the breaker and disables interop.
    bool recordFailure(const QString &reason = {});
    bool isTripped() const { return m_tripped; }
    int consecutiveFailures() const { return m_consecutiveFailures; }
    int failureThreshold() const;
    QString lastFailureReason() const { return m_lastReason; }
    void reset();

private:
    GpuVendor m_vendor = GpuVendor::Unknown;
    int m_consecutiveFailures = 0;
    bool m_tripped = false;
    QString m_lastReason;
};

// Phase 3: Cross-vendor GLSL strictness static auditor (AMD Adrenalin compliance).
GlslAuditResult auditGlslShaderForStrictDrivers(const QString &source,
                                                const QString &shaderName = {});

// Phase 4: Non-CUDA AI compute backend discovery (DirectML / Vulkan / ROCm / CUDA / CPU).
QString aiBackendDisplayName(AiComputeBackend backend);
QString aiBackendId(AiComputeBackend backend);
AiComputeBackend aiBackendFromId(const QString &id);
QVector<AiComputeBackend> probeAvailableAiBackends();
AiComputeBackend selectBestAiBackend(GpuVendor vendor,
                                     const QVector<AiComputeBackend> &available,
                                     AiComputeBackend userPreference = AiComputeBackend::Auto);
AiComputeBackend preferredAiBackend();
void setPreferredAiBackend(AiComputeBackend backend);
void clearSettingsForTest();

// Phase 5: Diagnostics & Radeon hardware verification summary.
GpuEnvironmentInfo gatherHostGpuInfo(const QString &glVendor = {},
                                     const QString &glRenderer = {},
                                     const QString &glVersion = {});
QString buildDiagnosticsReportText(const GpuEnvironmentInfo &info);

class GpuDiagnosticsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit GpuDiagnosticsDialog(const QString &glVendor = {},
                                  const QString &glRenderer = {},
                                  const QString &glVersion = {},
                                  QWidget *parent = nullptr);

    AiComputeBackend selectedAiPreference() const;
    WglDxInteropMode selectedInteropMode() const;
    QString reportText() const;

private:
    void refreshReport();
    void saveAndAccept();

    QString m_glVendor;
    QString m_glRenderer;
    QString m_glVersion;
    QComboBox *m_interopCombo = nullptr;
    QComboBox *m_aiBackendCombo = nullptr;
    QPlainTextEdit *m_reportView = nullptr;
};

} // namespace gpuvendor
