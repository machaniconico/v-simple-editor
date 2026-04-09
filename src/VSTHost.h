#pragma once

#include <QDialog>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>

// ============================================================================
// AudioPluginInfo
// ============================================================================

enum class AudioPluginType {
    VST3,
    AudioUnit,
    Builtin
};

struct AudioPluginInfo {
    QString         name;
    QString         vendor;
    QString         category;       // "Instrument", "Fx", "Analyzer", etc.
    QString         version;
    QString         pluginPath;
    AudioPluginType pluginType      = AudioPluginType::VST3;
    QString         uniqueId;       // stable identifier (e.g. VST3 FUID or AU component ID)
    int             numInputChannels  = 2;
    int             numOutputChannels = 2;
    bool            hasEditor       = false;
};

// ============================================================================
// AudioPluginParam
// ============================================================================

struct AudioPluginParam {
    QString name;
    int     id           = 0;
    double  value        = 0.0;
    double  minValue     = 0.0;
    double  maxValue     = 1.0;
    double  defaultValue = 0.0;
    bool    isAutomatable = true;
};

// ============================================================================
// AudioPluginInstance  (abstract)
// ============================================================================
//
// Concrete subclasses: VST3PluginInstance, AUPluginInstance
//
// processBlock() is called from the audio thread. All other methods are
// safe to call from the main/GUI thread.

class AudioPluginInstance
{
public:
    virtual ~AudioPluginInstance() = default;

    // Load the plugin from the given info. Returns true on success.
    virtual bool loadPlugin(const AudioPluginInfo &info) = 0;

    // Audio processing — called from the audio thread.
    // input/output are non-interleaved float arrays, one pointer per channel.
    virtual void processBlock(float **input, float **output,
                              int numSamples, int sampleRate) = 0;

    // Parameter access
    virtual int            getParameterCount() const = 0;
    virtual AudioPluginParam getParameterInfo(int id) const = 0;
    virtual double         getParameter(int id) const = 0;
    virtual void           setParameter(int id, double value) = 0;

    // Editor — opens the plugin's native GUI as a child of parent.
    // No-op if hasEditor() returns false.
    virtual void showEditor(QWidget *parent) = 0;

    virtual QString name() const = 0;

    // Latency introduced by the plugin in samples
    virtual int latency() const = 0;

    bool bypass = false;
};

// ============================================================================
// AudioPluginScanner
// ============================================================================

class AudioPluginScanner : public QObject
{
    Q_OBJECT

public:
    explicit AudioPluginScanner(QObject *parent = nullptr);

    // Scan the given directories and return discovered plugins.
    // Runs the scan on a background QThread; progress/complete signals are
    // emitted when the thread is done.
    void scanDirectories(const QStringList &paths);

    // Convenience: scan the platform-default directories.
    void scanDefaultDirectories();

    // Default platform scan paths (static so they can be queried before scanning)
    static QStringList defaultVST3Paths();
    static QStringList defaultAUPaths();   // macOS only; empty on Windows/Linux

signals:
    void scanProgress(int current, int total);
    void scanComplete(QVector<AudioPluginInfo> plugins);

private:
    // Scans a single file; returns a valid AudioPluginInfo if it is a plugin.
    AudioPluginInfo probeFile(const QString &filePath);

    // Type detection helpers
    static AudioPluginType detectType(const QString &filePath);
    static bool            isPluginFile(const QString &filePath);
};

// ============================================================================
// VSTHostManager  (singleton)
// ============================================================================

class VSTHostManager : public QObject
{
    Q_OBJECT

public:
    static VSTHostManager &instance();

    // Access the shared scanner
    AudioPluginScanner *scanner();

    // Instantiate and load a plugin. Returns nullptr on failure.
    std::shared_ptr<AudioPluginInstance> loadPlugin(const AudioPluginInfo &info);

    // All currently loaded plugin instances
    QVector<std::shared_ptr<AudioPluginInstance>> loadedPlugins() const;

    // Remove a previously loaded instance
    void unloadPlugin(std::shared_ptr<AudioPluginInstance> instance);

    // Plugin database: cached scan results stored via QSettings.
    // Populated by the scanner; persists across sessions.
    QVector<AudioPluginInfo> pluginDatabase() const;
    void                     savePluginDatabase(const QVector<AudioPluginInfo> &plugins);

signals:
    void pluginLoaded(std::shared_ptr<AudioPluginInstance> instance);
    void pluginUnloaded(std::shared_ptr<AudioPluginInstance> instance);

private:
    VSTHostManager();

    AudioPluginScanner *m_scanner = nullptr;
    QVector<std::shared_ptr<AudioPluginInstance>> m_loadedPlugins;
};

// ============================================================================
// VSTPluginDialog  (QDialog)
// ============================================================================

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QPushButton;
class QLabel;
class QSplitter;
class QGroupBox;
class QSlider;
class QScrollArea;

class VSTPluginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VSTPluginDialog(QWidget *parent = nullptr);

    // If the user clicked Load, returns the loaded instance; otherwise nullptr.
    std::shared_ptr<AudioPluginInstance> loadedInstance() const { return m_loadedInstance; }

private slots:
    void onScanClicked();
    void onScanProgress(int current, int total);
    void onScanComplete(QVector<AudioPluginInfo> plugins);
    void onSearchChanged(const QString &text);
    void onPluginSelected();
    void onLoadClicked();
    void onBypassToggled(bool bypassed);
    void onParameterSliderMoved(int paramId, int sliderValue);

private:
    void setupUI();
    void populateTree(const QVector<AudioPluginInfo> &plugins);
    void showPluginDetails(const AudioPluginInfo &info);
    void buildParameterPanel(std::shared_ptr<AudioPluginInstance> instance);
    void clearParameterPanel();

    // Widget pointers
    QLineEdit   *m_searchEdit       = nullptr;
    QPushButton *m_scanButton       = nullptr;
    QLabel      *m_scanStatusLabel  = nullptr;
    QTreeWidget *m_pluginTree       = nullptr;
    QLabel      *m_detailName       = nullptr;
    QLabel      *m_detailVendor     = nullptr;
    QLabel      *m_detailCategory   = nullptr;
    QLabel      *m_detailVersion    = nullptr;
    QLabel      *m_detailChannels   = nullptr;
    QLabel      *m_detailPath       = nullptr;
    QPushButton *m_loadButton       = nullptr;
    QPushButton *m_bypassButton     = nullptr;
    QPushButton *m_editorButton     = nullptr;
    QScrollArea *m_paramScrollArea  = nullptr;
    QWidget     *m_paramContainer   = nullptr;

    QVector<AudioPluginInfo>             m_allPlugins;
    AudioPluginInfo                      m_selectedInfo;
    std::shared_ptr<AudioPluginInstance> m_loadedInstance;
};
