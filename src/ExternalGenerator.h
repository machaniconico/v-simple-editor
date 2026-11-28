#pragma once

#include <QDialog>
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class Timeline;
struct ClipInfo;

namespace extgen {

// Canonical URI prefix stored in ClipInfo::filePath (zero Timeline.h schema change).
// Example:
//   veditor://extgen?renderer=edge&entry=C%3A%2Fproj%2Ftitle.html&params=%7B%7D&fps=30
inline constexpr char kUriPrefix[] = "veditor://extgen";

struct GeneratorSpec {
    QString renderer = QStringLiteral("dummy"); // "dummy", "edge", "quickjs", or plugin id
    QString entryPath;                          // Script / HTML path (optional for "dummy")
    QJsonObject params;                         // Arbitrary user JSON parameters
    double fps = 30.0;                          // Generator evaluation FPS
    QString cacheDir;                           // Optional override cache directory

    bool isValid() const { return !renderer.trimmed().isEmpty(); }
};

struct PluginManifest {
    QString id;
    QString name;
    QString executablePath;
    QString protocol = QStringLiteral("stdio-rgba"); // "stdio-rgba" or "cli-png"
    QStringList extraArgs;
    QStringList extensions;
    QString manifestDir;
    bool valid = false;
};

struct RenderOutcome {
    QImage image;
    bool ok = false;
    bool fromCache = false;
    bool missingRenderer = false;
    bool untrusted = false;
    QString statusMessage;
};

struct BakeResult {
    bool ok = false;
    QString outputPath;
    int framesRendered = 0;
    QString error;
};

// URI detection / serialization
bool isExternalGeneratorFilePath(const QString &filePath);
GeneratorSpec parseGeneratorUri(const QString &uri);
QString buildGeneratorUri(const GeneratorSpec &spec);
QString computeSpecHash(const GeneratorSpec &spec);

// R3 Security: folder-level trust registry persisted in QSettings.
QString canonicalTrustFolderForEntry(const QString &entryPath);
bool isFolderTrusted(const QString &entryPath);
void trustFolder(const QString &entryPath);
void revokeFolderTrust(const QString &entryPath);
void clearTrustedFolders();
QStringList trustedFolders();
bool ensureEntryTrustedInteractive(const QString &entryPath, QWidget *parent = nullptr);

// Optional external plugin discovery (0 bytes bundled in main executable).
QStringList pluginSearchDirs();
QVector<PluginManifest> discoverPlugins();
PluginManifest findPlugin(const QString &rendererId);
bool isRendererAvailable(const QString &rendererId);

// Binary stdin/stdout frame framing ("VGEN" + width u32le + height u32le + byteLen u32le + RGBA).
QByteArray encodeVgenFrame(const QImage &image);
bool decodeVgenFrame(const QByteArray &data, QImage *outImage);

// Execute an external plugin subprocess with timeout protection.
RenderOutcome renderViaSubprocess(const PluginManifest &plugin,
                                  const GeneratorSpec &spec,
                                  int frameIndex,
                                  QSize size,
                                  int timeoutMs = 15000);

// Disk & memory cache helpers
QString defaultCacheRootDir();
QString frameCachePath(const GeneratorSpec &spec, int frameIndex, QSize size);
QString lastCachedFramePath(const GeneratorSpec &spec);
void storeFrameInCache(const GeneratorSpec &spec, int frameIndex, QSize size, const QImage &img);
QImage loadFrameFromCache(const GeneratorSpec &spec, int frameIndex, QSize size);
QImage loadLastCachedFrame(const GeneratorSpec &spec);
void clearMemoryCache();

// Placeholder cards (never silent black frames on missing plugin / untrusted script)
QImage renderMissingPlaceholder(const GeneratorSpec &spec, QSize size, const QString &detail = {});
QImage renderUntrustedPlaceholder(const GeneratorSpec &spec, QSize size);
QImage renderDummyFrame(const GeneratorSpec &spec, int frameIndex, QSize size);

// Main SSOT render entry point called by TimelineFrameRenderer::renderClipSourceFrame.
// `isPreview` selects scaled preview cache resolution when true, full `outSize` when false.
RenderOutcome renderGeneratorFrame(const GeneratorSpec &spec,
                                   int frameIndex,
                                   QSize outSize,
                                   bool isPreview = false);

QImage renderGeneratorClipFrame(const ClipInfo &clip,
                                double clipLocalSec,
                                QSize outSize,
                                const Timeline *timeline = nullptr,
                                bool isPreview = false);

// Bake an External Generator clip into a standalone video file and replace clip.filePath
// on Timeline with a single undo step so the project opens cleanly on any machine.
BakeResult bakeExternalGeneratorClip(Timeline *timeline,
                                     int trackIdx,
                                     int clipIdx,
                                     const QString &outputPath,
                                     QSize renderSize,
                                     double fps = 30.0,
                                     bool forceSoftwareCodec = false);

// Deterministic test hooks (selftest never invokes real Edge or QuickJS).
using TestRendererFn = std::function<QImage(const GeneratorSpec &, int frameIndex, QSize size)>;
void registerTestRenderer(const QString &rendererId, TestRendererFn fn);
void clearTestRenderers();
void setPreviewModeHintForCurrentThread(bool isPreview);
bool previewModeHintForCurrentThread();

// Configuration dialog for inserting or editing an External Generator clip.
class ExternalGeneratorDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExternalGeneratorDialog(const GeneratorSpec &initialSpec = GeneratorSpec{},
                                     double initialDurationSec = 5.0,
                                     QWidget *parent = nullptr);

    GeneratorSpec spec() const;
    double durationSec() const;

private:
    void refreshTrustStatus();
    void browseEntry();

    QComboBox *m_rendererCombo = nullptr;
    QLineEdit *m_entryEdit = nullptr;
    QLabel *m_trustLabel = nullptr;
    QPushButton *m_trustButton = nullptr;
    QPlainTextEdit *m_paramsEdit = nullptr;
    QDoubleSpinBox *m_fpsSpin = nullptr;
    QDoubleSpinBox *m_durationSpin = nullptr;
};

} // namespace extgen
