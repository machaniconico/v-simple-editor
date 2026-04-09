#include "VSTHost.h"

#include <cstring>

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

// Platform dynamic-library loading
#if defined(Q_OS_WIN)
#  include <windows.h>
   using LibHandle = HMODULE;
#  define LOAD_LIB(path)       LoadLibraryW(reinterpret_cast<LPCWSTR>((path).utf16()))
#  define FREE_LIB(h)          FreeLibrary(h)
#  define GET_PROC(h, sym)     GetProcAddress(h, sym)
#elif defined(Q_OS_UNIX)
#  include <dlfcn.h>
   using LibHandle = void*;
#  define LOAD_LIB(path)       dlopen((path).toUtf8().constData(), RTLD_LAZY | RTLD_LOCAL)
#  define FREE_LIB(h)          dlclose(h)
#  define GET_PROC(h, sym)     dlsym(h, sym)
#endif

// ============================================================================
// Internal stub implementations
// ============================================================================
//
// VST3PluginInstance and AUPluginInstance are stub classes that demonstrate
// where the real SDK calls would be placed.  They log every operation so the
// host framework can be verified without actual SDK binaries.
//
// To hook up the real VST3 SDK:
//   1. Add the Steinberg VST3 SDK headers to target_include_directories().
//   2. Replace the LOAD_LIB / GET_PROC calls below with the VST3 module loader
//      (VST3::Hosting::Module::create()) and IPluginFactory queries.
//   3. Implement processBlock() using IAudioProcessor::process().
//   4. Implement showEditor() using IEditController::createView() + IPlugView::attached().
//
// To hook up the CoreAudio AudioUnit SDK (macOS):
//   1. Link against AudioUnit.framework and CoreAudio.framework.
//   2. Use AudioComponentFindNext / AudioComponentInstanceNew.
//   3. Implement processBlock() via AudioUnitRender().
//   4. Implement showEditor() via AUCocoaUIElement / AudioUnitGetProperty.

// ----------------------------------------------------------------------------
// VST3PluginInstance
// ----------------------------------------------------------------------------

class VST3PluginInstance : public AudioPluginInstance
{
public:
    ~VST3PluginInstance() override
    {
        if (m_libHandle) {
            // TODO (real SDK): call IPluginFactory::release(), IComponent::terminate()
            qDebug() << "[VSTHost] VST3 unloaded:" << m_info.pluginPath;
            FREE_LIB(m_libHandle);
            m_libHandle = nullptr;
        }
    }

    bool loadPlugin(const AudioPluginInfo &info) override
    {
        m_info = info;

        qDebug() << "[VSTHost] Loading VST3:" << info.pluginPath;

        // Dynamic load of the .vst3 bundle / .dll
        m_libHandle = LOAD_LIB(info.pluginPath);
        if (!m_libHandle) {
            qWarning() << "[VSTHost] VST3: dlopen failed for" << info.pluginPath;
            return false;
        }

        // TODO (real VST3 SDK): retrieve GetPluginFactory export and enumerate
        // IComponent + IAudioProcessor interfaces.
        //
        //   using GetPluginFactoryFunc = IPluginFactory*(*)();
        //   auto getFactory = reinterpret_cast<GetPluginFactoryFunc>(
        //       GET_PROC(m_libHandle, "GetPluginFactory"));
        //   if (!getFactory) { FREE_LIB(m_libHandle); m_libHandle=nullptr; return false; }
        //   m_factory = getFactory();
        //   ...

        qDebug() << "[VSTHost] VST3 stub loaded OK:" << info.name;

        // Populate some fake parameters for demonstration
        auto makeParam = [](const QString &n, int i, double v, double lo, double hi, double def) {
            AudioPluginParam p;
            p.name = n; p.id = i; p.value = v;
            p.minValue = lo; p.maxValue = hi; p.defaultValue = def;
            p.isAutomatable = true;
            return p;
        };
        m_params = {
            makeParam("Gain",     0,   0.8,    0.0,    1.0,     0.8),
            makeParam("Pan",      1,   0.5,    0.0,    1.0,     0.5),
            makeParam("Low Cut",  2,  80.0,   20.0,  500.0,    80.0),
            makeParam("High Cut", 3, 16000.0, 2000.0, 20000.0, 16000.0),
        };
        return true;
    }

    void processBlock(float **input, float **output, int numSamples, int sampleRate) override
    {
        Q_UNUSED(sampleRate)
        if (bypass || !m_libHandle) {
            // Pass-through when bypassed or not loaded
            for (int ch = 0; ch < m_info.numOutputChannels; ++ch) {
                if (input && output && input[ch] && output[ch])
                    std::memcpy(output[ch], input[ch], sizeof(float) * numSamples);
            }
            return;
        }

        // TODO (real VST3 SDK): fill Vst::ProcessData, call IAudioProcessor::process()
        //
        //   Vst::ProcessData data;
        //   data.numSamples = numSamples;
        //   data.inputs[0].channelBuffers32 = input;
        //   data.outputs[0].channelBuffers32 = output;
        //   m_audioProcessor->process(data);

        // Stub: apply a simple gain from parameter 0 and copy to output
        double gainParam = m_params.isEmpty() ? 1.0 : m_params[0].value;
        float  gain      = static_cast<float>(gainParam);
        for (int ch = 0; ch < m_info.numOutputChannels; ++ch) {
            if (!input || !output || !input[ch] || !output[ch]) continue;
            for (int i = 0; i < numSamples; ++i)
                output[ch][i] = input[ch][i] * gain;
        }
    }

    int getParameterCount() const override { return m_params.size(); }

    AudioPluginParam getParameterInfo(int id) const override
    {
        for (const auto &p : m_params)
            if (p.id == id) return p;
        return {};
    }

    double getParameter(int id) const override
    {
        for (const auto &p : m_params)
            if (p.id == id) return p.value;
        return 0.0;
    }

    void setParameter(int id, double value) override
    {
        // TODO (real VST3 SDK): IEditController::setParamNormalized()
        for (auto &p : m_params) {
            if (p.id == id) {
                p.value = value;
                qDebug() << "[VSTHost] VST3 param" << id << "=" << value;
                return;
            }
        }
    }

    void showEditor(QWidget *parent) override
    {
        // TODO (real VST3 SDK):
        //   IEditController::createView("editor") -> IPlugView*
        //   IPlugView::attached(windowHandle, kPlatformTypeNSView / HWND)
        //   Embed the returned view into a QWindow::fromWinId wrapper.
        qDebug() << "[VSTHost] VST3 showEditor requested (stub — no native view)";

        auto *dlg = new QDialog(parent);
        dlg->setWindowTitle(QString("VST3 Editor — %1 (stub)").arg(m_info.name));
        dlg->resize(400, 200);
        auto *lbl = new QLabel(
            QString("Native VST3 editor for <b>%1</b> would appear here.\n\n"
                    "Real implementation requires the Steinberg VST3 SDK\n"
                    "and IPlugView integration.").arg(m_info.name),
            dlg);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setWordWrap(true);
        auto *lay = new QVBoxLayout(dlg);
        lay->addWidget(lbl);
        dlg->exec();
    }

    QString name()    const override { return m_info.name; }
    int     latency() const override
    {
        // TODO (real VST3 SDK): IAudioProcessor::getLatencySamples()
        return 0;
    }

private:
    AudioPluginInfo        m_info;
    LibHandle              m_libHandle = nullptr;
    QVector<AudioPluginParam> m_params;
};

// ----------------------------------------------------------------------------
// AUPluginInstance  (macOS AudioUnit)
// ----------------------------------------------------------------------------

class AUPluginInstance : public AudioPluginInstance
{
public:
    ~AUPluginInstance() override
    {
        if (m_instanceCreated) {
            // TODO (real AU SDK): AudioUnitUninitialize(), AudioComponentInstanceDispose()
            qDebug() << "[VSTHost] AU disposed:" << m_info.name;
            m_instanceCreated = false;
        }
    }

    bool loadPlugin(const AudioPluginInfo &info) override
    {
        m_info = info;
        qDebug() << "[VSTHost] Loading AudioUnit:" << info.uniqueId;

#if defined(Q_OS_MAC)
        // TODO (real AU SDK):
        //   AudioComponentDescription desc = { ... };   // parse from uniqueId
        //   AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        //   if (!comp) return false;
        //   AudioComponentInstanceNew(comp, &m_auInstance);
        //   AudioUnitInitialize(m_auInstance);

        m_instanceCreated = true;
        qDebug() << "[VSTHost] AU stub loaded OK:" << info.name;

        // Stub parameters
        auto makeAUParam = [](const QString &n, int i, double v, double lo, double hi, double def) {
            AudioPluginParam p;
            p.name = n; p.id = i; p.value = v;
            p.minValue = lo; p.maxValue = hi; p.defaultValue = def;
            p.isAutomatable = true;
            return p;
        };
        m_params = {
            makeAUParam("Mix",       0, 1.0, 0.0, 1.0, 1.0),
            makeAUParam("Room Size", 1, 0.5, 0.0, 1.0, 0.5),
            makeAUParam("Damping",   2, 0.5, 0.0, 1.0, 0.5),
        };
        return true;
#else
        qWarning() << "[VSTHost] AudioUnit plugins are macOS-only";
        return false;
#endif
    }

    void processBlock(float **input, float **output, int numSamples, int sampleRate) override
    {
        Q_UNUSED(sampleRate)
        if (bypass || !m_instanceCreated) {
            for (int ch = 0; ch < m_info.numOutputChannels; ++ch) {
                if (input && output && input[ch] && output[ch])
                    std::memcpy(output[ch], input[ch], sizeof(float) * numSamples);
            }
            return;
        }

#if defined(Q_OS_MAC)
        // TODO (real AU SDK):
        //   AudioUnitRenderActionFlags flags = 0;
        //   AudioTimeStamp timeStamp = { ... };
        //   AudioBufferList bufList = { ... };
        //   AudioUnitRender(m_auInstance, &flags, &timeStamp, 0, numSamples, &bufList);

        // Stub: pass through
        for (int ch = 0; ch < m_info.numOutputChannels; ++ch) {
            if (!input || !output || !input[ch] || !output[ch]) continue;
            std::memcpy(output[ch], input[ch], sizeof(float) * numSamples);
        }
#endif
    }

    int getParameterCount() const override { return m_params.size(); }

    AudioPluginParam getParameterInfo(int id) const override
    {
        for (const auto &p : m_params)
            if (p.id == id) return p;
        return {};
    }

    double getParameter(int id) const override
    {
        for (const auto &p : m_params)
            if (p.id == id) return p.value;
        return 0.0;
    }

    void setParameter(int id, double value) override
    {
        // TODO (real AU SDK): AudioUnitSetParameter(m_auInstance, id, ...)
        for (auto &p : m_params) {
            if (p.id == id) {
                p.value = value;
                qDebug() << "[VSTHost] AU param" << id << "=" << value;
                return;
            }
        }
    }

    void showEditor(QWidget *parent) override
    {
#if defined(Q_OS_MAC)
        // TODO (real AU SDK):
        //   AUCocoaUIElement cocoaUI = {};
        //   UInt32 size = sizeof(cocoaUI);
        //   AudioUnitGetProperty(m_auInstance, kAudioUnitProperty_CocoaUI,
        //                        kAudioUnitScope_Global, 0, &cocoaUI, &size);
        //   id<AUCocoaUIBase> uiBase = [cocoaUI.CocoaUIFactory uiViewForAudioUnit:...];
        //   Wrap the NSView* into a QWindow and embed via QWidget::createWindowContainer().
        qDebug() << "[VSTHost] AU showEditor requested (stub)";

        auto *dlg = new QDialog(parent);
        dlg->setWindowTitle(QString("AU Editor — %1 (stub)").arg(m_info.name));
        dlg->resize(400, 200);
        auto *lbl = new QLabel(
            QString("Native AudioUnit editor for <b>%1</b> would appear here.\n\n"
                    "Real implementation requires linking AudioUnit.framework\n"
                    "and AUCocoaUI embedding.").arg(m_info.name),
            dlg);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setWordWrap(true);
        auto *lay = new QVBoxLayout(dlg);
        lay->addWidget(lbl);
        dlg->exec();
#else
        Q_UNUSED(parent)
#endif
    }

    QString name()    const override { return m_info.name; }
    int     latency() const override
    {
        // TODO (real AU SDK): kAudioUnitProperty_Latency
        return 0;
    }

private:
    AudioPluginInfo           m_info;
    bool                      m_instanceCreated = false;
    QVector<AudioPluginParam> m_params;
#if defined(Q_OS_MAC)
    // AudioComponentInstance m_auInstance = nullptr;   // uncomment with real SDK
#endif
};

// ============================================================================
// AudioPluginScanner
// ============================================================================

AudioPluginScanner::AudioPluginScanner(QObject *parent)
    : QObject(parent)
{
}

QStringList AudioPluginScanner::defaultVST3Paths()
{
    QStringList paths;
#if defined(Q_OS_WIN)
    paths << "C:/Program Files/Common Files/VST3";
    paths << "C:/Program Files (x86)/Common Files/VST3";
#elif defined(Q_OS_MAC)
    paths << "/Library/Audio/Plug-Ins/VST3";
    paths << QDir::homePath() + "/Library/Audio/Plug-Ins/VST3";
#else
    // Linux: XDG_DATA_HOME and common system dirs
    paths << QDir::homePath() + "/.vst3";
    paths << "/usr/lib/vst3";
    paths << "/usr/local/lib/vst3";
#endif
    return paths;
}

QStringList AudioPluginScanner::defaultAUPaths()
{
    QStringList paths;
#if defined(Q_OS_MAC)
    paths << "/Library/Audio/Plug-Ins/Components";
    paths << QDir::homePath() + "/Library/Audio/Plug-Ins/Components";
#endif
    return paths;
}

void AudioPluginScanner::scanDefaultDirectories()
{
    QStringList paths;
    paths << defaultVST3Paths();
    paths << defaultAUPaths();
    scanDirectories(paths);
}

bool AudioPluginScanner::isPluginFile(const QString &filePath)
{
    const QString lower = filePath.toLower();
#if defined(Q_OS_WIN)
    return lower.endsWith(".vst3");
#elif defined(Q_OS_MAC)
    return lower.endsWith(".vst3") || lower.endsWith(".component");
#else
    return lower.endsWith(".vst3") || lower.endsWith(".so");
#endif
}

AudioPluginType AudioPluginScanner::detectType(const QString &filePath)
{
    const QString lower = filePath.toLower();
    if (lower.endsWith(".component"))
        return AudioPluginType::AudioUnit;
    return AudioPluginType::VST3;
}

AudioPluginInfo AudioPluginScanner::probeFile(const QString &filePath)
{
    AudioPluginInfo info;
    info.pluginPath = filePath;
    info.pluginType = detectType(filePath);

    QFileInfo fi(filePath);
    // Derive a display name from the bundle/file stem
    QString stem = fi.baseName();
    info.name   = stem;
    info.vendor = "(unknown)";

    if (info.pluginType == AudioPluginType::VST3) {
        info.category = "Fx";

        // TODO (real VST3 SDK): load the bundle, call GetPluginFactory,
        // iterate PClassInfo2 to extract name/vendor/category/subcategories/sdkVersion.
        //
        //   VST3::Hosting::Module::Ptr module = VST3::Hosting::Module::create(filePath, error);
        //   auto factory = module->getFactory();
        //   for (auto &classInfo : factory.classInfos()) {
        //       if (classInfo.category() == kVstAudioEffectClass) {
        //           info.name     = QString::fromStdString(classInfo.name());
        //           info.vendor   = QString::fromStdString(factory.info().vendor());
        //           info.version  = QString::fromStdString(classInfo.version());
        //           info.uniqueId = QString::fromStdString(classInfo.ID().toString());
        //           info.hasEditor = (classInfo.flags() & Vst::ComponentFlags::kDistributable) == 0;
        //       }
        //   }

        info.uniqueId  = "vst3:" + stem;
        info.version   = "1.0.0";
        info.hasEditor = true;
        info.numInputChannels  = 2;
        info.numOutputChannels = 2;
    } else {
        // AudioUnit (macOS .component bundles)
        info.category = "AU Fx";

        // TODO (real AU SDK): read the plist inside the bundle to get
        // AudioComponent type/subtype/manufacturer codes; use
        // AudioComponentFindNext to verify the AU is registered.
        //
        //   NSDictionary *plist = bundle.infoDictionary[@"AudioComponents"][0];
        //   info.name     = QString::fromNSString(plist[@"name"]);
        //   info.vendor   = QString::fromNSString(plist[@"manufacturer"]);
        //   info.uniqueId = QString::fromNSString(
        //       [NSString stringWithFormat:@"%@:%@:%@",
        //           plist[@"type"], plist[@"subtype"], plist[@"manufacturer"]]);

        info.uniqueId  = "au:" + stem;
        info.version   = "1.0.0";
        info.hasEditor = true;
        info.numInputChannels  = 2;
        info.numOutputChannels = 2;
    }

    return info;
}

void AudioPluginScanner::scanDirectories(const QStringList &paths)
{
    // Run scan on a background thread to avoid blocking the UI
    QThread *thread = QThread::create([this, paths]() {
        QVector<AudioPluginInfo> results;
        QStringList allFiles;

        // Collect all candidate files first so we can report progress
        for (const QString &dirPath : paths) {
            QDir dir(dirPath);
            if (!dir.exists()) continue;

            QDirIterator it(dirPath, QDir::Files | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                if (isPluginFile(it.filePath()))
                    allFiles << it.filePath();
            }
        }

        const int total = allFiles.size();
        for (int i = 0; i < total; ++i) {
            emit scanProgress(i + 1, total);
            AudioPluginInfo info = probeFile(allFiles[i]);
            if (!info.name.isEmpty())
                results << info;
        }

        emit scanComplete(results);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ============================================================================
// VSTHostManager
// ============================================================================

VSTHostManager &VSTHostManager::instance()
{
    static VSTHostManager s_instance;
    return s_instance;
}

VSTHostManager::VSTHostManager()
    : QObject(nullptr)
    , m_scanner(new AudioPluginScanner(this))
{
    // Propagate scan results into the persistent database
    connect(m_scanner, &AudioPluginScanner::scanComplete,
            this, &VSTHostManager::savePluginDatabase);
}

AudioPluginScanner *VSTHostManager::scanner()
{
    return m_scanner;
}

std::shared_ptr<AudioPluginInstance> VSTHostManager::loadPlugin(const AudioPluginInfo &info)
{
    std::shared_ptr<AudioPluginInstance> instance;

    switch (info.pluginType) {
    case AudioPluginType::VST3:
        instance = std::make_shared<VST3PluginInstance>();
        break;
    case AudioPluginType::AudioUnit:
        instance = std::make_shared<AUPluginInstance>();
        break;
    case AudioPluginType::Builtin:
        qWarning() << "[VSTHost] Builtin plugin type should not go through VSTHostManager";
        return nullptr;
    }

    if (!instance->loadPlugin(info)) {
        qWarning() << "[VSTHost] Failed to load plugin:" << info.name;
        return nullptr;
    }

    m_loadedPlugins << instance;
    emit pluginLoaded(instance);
    return instance;
}

QVector<std::shared_ptr<AudioPluginInstance>> VSTHostManager::loadedPlugins() const
{
    return m_loadedPlugins;
}

void VSTHostManager::unloadPlugin(std::shared_ptr<AudioPluginInstance> instance)
{
    m_loadedPlugins.removeAll(instance);
    emit pluginUnloaded(instance);
    // instance ref-count drops to zero here if no other owners
}

QVector<AudioPluginInfo> VSTHostManager::pluginDatabase() const
{
    QSettings settings("v-editor", "VSTHost");
    int count = settings.beginReadArray("plugins");
    QVector<AudioPluginInfo> db;
    db.reserve(count);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        AudioPluginInfo info;
        info.name               = settings.value("name").toString();
        info.vendor             = settings.value("vendor").toString();
        info.category           = settings.value("category").toString();
        info.version            = settings.value("version").toString();
        info.pluginPath         = settings.value("pluginPath").toString();
        info.uniqueId           = settings.value("uniqueId").toString();
        info.numInputChannels   = settings.value("numInputChannels", 2).toInt();
        info.numOutputChannels  = settings.value("numOutputChannels", 2).toInt();
        info.hasEditor          = settings.value("hasEditor", false).toBool();
        int typeVal             = settings.value("pluginType", 0).toInt();
        info.pluginType         = static_cast<AudioPluginType>(typeVal);
        db << info;
    }
    settings.endArray();
    return db;
}

void VSTHostManager::savePluginDatabase(const QVector<AudioPluginInfo> &plugins)
{
    QSettings settings("v-editor", "VSTHost");
    settings.beginWriteArray("plugins", plugins.size());
    for (int i = 0; i < plugins.size(); ++i) {
        settings.setArrayIndex(i);
        const AudioPluginInfo &info = plugins[i];
        settings.setValue("name",              info.name);
        settings.setValue("vendor",            info.vendor);
        settings.setValue("category",          info.category);
        settings.setValue("version",           info.version);
        settings.setValue("pluginPath",        info.pluginPath);
        settings.setValue("uniqueId",          info.uniqueId);
        settings.setValue("numInputChannels",  info.numInputChannels);
        settings.setValue("numOutputChannels", info.numOutputChannels);
        settings.setValue("hasEditor",         info.hasEditor);
        settings.setValue("pluginType",        static_cast<int>(info.pluginType));
    }
    settings.endArray();
}

// ============================================================================
// VSTPluginDialog
// ============================================================================

VSTPluginDialog::VSTPluginDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("VST / AudioUnit Plugin Manager");
    setMinimumSize(800, 560);
    setupUI();

    // Load cached database on open
    QVector<AudioPluginInfo> db = VSTHostManager::instance().pluginDatabase();
    if (!db.isEmpty()) {
        m_allPlugins = db;
        populateTree(db);
        m_scanStatusLabel->setText(
            QString("%1 plugins in database (click Scan to refresh)").arg(db.size()));
    }

    // Wire scanner signals
    AudioPluginScanner *scanner = VSTHostManager::instance().scanner();
    connect(scanner, &AudioPluginScanner::scanProgress,
            this,    &VSTPluginDialog::onScanProgress);
    connect(scanner, &AudioPluginScanner::scanComplete,
            this,    &VSTPluginDialog::onScanComplete);
}

void VSTPluginDialog::setupUI()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);

    // --- Top bar: search + scan ---
    auto *topBar = new QHBoxLayout;
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText("Search plugins...");
    m_scanButton = new QPushButton("Scan");
    m_scanStatusLabel = new QLabel("No plugins loaded.");
    m_scanStatusLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);

    topBar->addWidget(new QLabel("Search:"));
    topBar->addWidget(m_searchEdit, 2);
    topBar->addSpacing(12);
    topBar->addWidget(m_scanButton);
    topBar->addWidget(m_scanStatusLabel, 1);
    rootLayout->addLayout(topBar);

    // --- Main splitter: tree | detail + params ---
    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);

    // Left: plugin tree
    m_pluginTree = new QTreeWidget;
    m_pluginTree->setHeaderLabels({"Plugin", "Vendor", "Type"});
    m_pluginTree->setColumnWidth(0, 200);
    m_pluginTree->setColumnWidth(1, 130);
    m_pluginTree->setAlternatingRowColors(true);
    m_pluginTree->setSortingEnabled(true);
    m_pluginTree->sortByColumn(0, Qt::AscendingOrder);
    splitter->addWidget(m_pluginTree);

    // Right panel
    auto *rightWidget = new QWidget;
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(4, 0, 0, 0);

    // Detail group
    auto *detailGroup = new QGroupBox("Plugin Details");
    auto *detailGrid  = new QGridLayout(detailGroup);
    detailGrid->setColumnStretch(1, 1);

    auto makeDetailRow = [&](const QString &label, QLabel *&valueLabel, int row) {
        detailGrid->addWidget(new QLabel(label), row, 0, Qt::AlignRight);
        valueLabel = new QLabel("—");
        valueLabel->setWordWrap(true);
        detailGrid->addWidget(valueLabel, row, 1);
    };

    makeDetailRow("Name:",     m_detailName,     0);
    makeDetailRow("Vendor:",   m_detailVendor,   1);
    makeDetailRow("Category:", m_detailCategory, 2);
    makeDetailRow("Version:",  m_detailVersion,  3);
    makeDetailRow("Channels:", m_detailChannels, 4);
    makeDetailRow("Path:",     m_detailPath,     5);

    rightLayout->addWidget(detailGroup);

    // Action buttons
    auto *btnRow = new QHBoxLayout;
    m_loadButton   = new QPushButton("Load Plugin");
    m_bypassButton = new QPushButton("Bypass");
    m_editorButton = new QPushButton("Show Editor");
    m_loadButton->setEnabled(false);
    m_bypassButton->setEnabled(false);
    m_editorButton->setEnabled(false);
    m_bypassButton->setCheckable(true);
    btnRow->addWidget(m_loadButton);
    btnRow->addWidget(m_bypassButton);
    btnRow->addWidget(m_editorButton);
    btnRow->addStretch();
    rightLayout->addLayout(btnRow);

    // Parameter panel (scrollable)
    auto *paramGroup = new QGroupBox("Parameters");
    auto *paramGroupLayout = new QVBoxLayout(paramGroup);
    m_paramScrollArea = new QScrollArea;
    m_paramScrollArea->setWidgetResizable(true);
    m_paramScrollArea->setFrameShape(QFrame::NoFrame);
    m_paramContainer = new QWidget;
    m_paramScrollArea->setWidget(m_paramContainer);
    new QVBoxLayout(m_paramContainer);  // empty layout placeholder
    paramGroupLayout->addWidget(m_paramScrollArea);
    paramGroup->setMinimumHeight(160);
    rightLayout->addWidget(paramGroup, 1);

    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    rootLayout->addWidget(splitter, 1);

    // --- Bottom: close ---
    auto *bottomRow = new QHBoxLayout;
    auto *closeBtn  = new QPushButton("Close");
    bottomRow->addStretch();
    bottomRow->addWidget(closeBtn);
    rootLayout->addLayout(bottomRow);

    // Connections
    connect(m_searchEdit, &QLineEdit::textChanged, this, &VSTPluginDialog::onSearchChanged);
    connect(m_scanButton, &QPushButton::clicked,   this, &VSTPluginDialog::onScanClicked);
    connect(m_pluginTree, &QTreeWidget::itemSelectionChanged,
            this, &VSTPluginDialog::onPluginSelected);
    connect(m_loadButton,   &QPushButton::clicked, this, &VSTPluginDialog::onLoadClicked);
    connect(m_bypassButton, &QPushButton::toggled, this, &VSTPluginDialog::onBypassToggled);
    connect(m_editorButton, &QPushButton::clicked, this, [this]() {
        if (m_loadedInstance)
            m_loadedInstance->showEditor(this);
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
}

void VSTPluginDialog::populateTree(const QVector<AudioPluginInfo> &plugins)
{
    m_pluginTree->clear();

    // Group by category
    QMap<QString, QTreeWidgetItem *> categoryItems;

    for (const AudioPluginInfo &info : plugins) {
        QString cat = info.category.isEmpty() ? "Uncategorized" : info.category;

        if (!categoryItems.contains(cat)) {
            auto *catItem = new QTreeWidgetItem(m_pluginTree, {cat, "", ""});
            QFont f = catItem->font(0);
            f.setBold(true);
            catItem->setFont(0, f);
            catItem->setExpanded(true);
            categoryItems[cat] = catItem;
        }

        QString typeStr = (info.pluginType == AudioPluginType::VST3) ? "VST3"
                        : (info.pluginType == AudioPluginType::AudioUnit) ? "AU"
                        : "Builtin";

        auto *item = new QTreeWidgetItem(categoryItems[cat],
                                         {info.name, info.vendor, typeStr});
        item->setData(0, Qt::UserRole, QVariant::fromValue<int>(
            static_cast<int>(&info - plugins.constData())));
        // Store uniqueId for lookup
        item->setData(0, Qt::UserRole + 1, info.uniqueId);
    }
}

void VSTPluginDialog::showPluginDetails(const AudioPluginInfo &info)
{
    m_detailName->setText(info.name.isEmpty() ? "—" : info.name);
    m_detailVendor->setText(info.vendor.isEmpty() ? "—" : info.vendor);
    m_detailCategory->setText(info.category.isEmpty() ? "—" : info.category);
    m_detailVersion->setText(info.version.isEmpty() ? "—" : info.version);
    m_detailChannels->setText(
        QString("%1 in / %2 out").arg(info.numInputChannels).arg(info.numOutputChannels));
    m_detailPath->setText(info.pluginPath.isEmpty() ? "—" : info.pluginPath);
    m_loadButton->setEnabled(!info.uniqueId.isEmpty());
}

void VSTPluginDialog::buildParameterPanel(std::shared_ptr<AudioPluginInstance> instance)
{
    clearParameterPanel();
    if (!instance) return;

    // Re-create the layout after clearParameterPanel() removed the old one
    auto *layout = new QVBoxLayout;
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    const int count = instance->getParameterCount();
    for (int i = 0; i < count; ++i) {
        AudioPluginParam p = instance->getParameterInfo(i);

        auto *row = new QHBoxLayout;
        auto *nameLabel = new QLabel(p.name);
        nameLabel->setFixedWidth(100);
        nameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 1000);
        double range = (p.maxValue - p.minValue);
        int sliderVal = (range > 0.0)
            ? static_cast<int>((p.value - p.minValue) / range * 1000.0)
            : 0;
        slider->setValue(sliderVal);

        auto *valueLabel = new QLabel(QString::number(p.value, 'f', 3));
        valueLabel->setFixedWidth(60);
        valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        // Capture param id and value range for the lambda
        int paramId = p.id;
        double pMin = p.minValue;
        double pMax = p.maxValue;

        connect(slider, &QSlider::valueChanged, this, [this, paramId, pMin, pMax, valueLabel,
                                                        instanceWeak = std::weak_ptr<AudioPluginInstance>(instance)]
                                                        (int sv) {
            auto inst = instanceWeak.lock();
            if (!inst) return;
            double v = pMin + (sv / 1000.0) * (pMax - pMin);
            inst->setParameter(paramId, v);
            valueLabel->setText(QString::number(v, 'f', 3));
        });

        row->addWidget(nameLabel);
        row->addWidget(slider, 1);
        row->addWidget(valueLabel);
        layout->addLayout(row);
    }

    layout->addStretch();
    m_paramContainer->setLayout(layout);
}

void VSTPluginDialog::clearParameterPanel()
{
    // Delete the old layout and all child widgets
    if (QLayout *old = m_paramContainer->layout()) {
        QLayoutItem *child;
        while ((child = old->takeAt(0)) != nullptr) {
            if (child->widget()) child->widget()->deleteLater();
            if (child->layout()) {
                // Nested layouts
                QLayoutItem *sub;
                while ((sub = child->layout()->takeAt(0)) != nullptr) {
                    if (sub->widget()) sub->widget()->deleteLater();
                    delete sub;
                }
            }
            delete child;
        }
        delete old;
    }
}

// --- Slots ---

void VSTPluginDialog::onScanClicked()
{
    m_scanButton->setEnabled(false);
    m_scanStatusLabel->setText("Scanning...");
    VSTHostManager::instance().scanner()->scanDefaultDirectories();
}

void VSTPluginDialog::onScanProgress(int current, int total)
{
    m_scanStatusLabel->setText(
        QString("Scanning... %1 / %2").arg(current).arg(total));
}

void VSTPluginDialog::onScanComplete(QVector<AudioPluginInfo> plugins)
{
    m_allPlugins = plugins;
    populateTree(plugins);
    m_scanStatusLabel->setText(
        QString("Scan complete. %1 plugins found.").arg(plugins.size()));
    m_scanButton->setEnabled(true);
}

void VSTPluginDialog::onSearchChanged(const QString &text)
{
    if (text.isEmpty()) {
        // Show all
        populateTree(m_allPlugins);
        return;
    }

    QVector<AudioPluginInfo> filtered;
    for (const AudioPluginInfo &info : m_allPlugins) {
        if (info.name.contains(text, Qt::CaseInsensitive)
         || info.vendor.contains(text, Qt::CaseInsensitive)
         || info.category.contains(text, Qt::CaseInsensitive)) {
            filtered << info;
        }
    }
    populateTree(filtered);
}

void VSTPluginDialog::onPluginSelected()
{
    QList<QTreeWidgetItem *> sel = m_pluginTree->selectedItems();
    if (sel.isEmpty()) return;

    QTreeWidgetItem *item = sel.first();
    QString uid = item->data(0, Qt::UserRole + 1).toString();
    if (uid.isEmpty()) return; // category header selected

    for (const AudioPluginInfo &info : m_allPlugins) {
        if (info.uniqueId == uid) {
            m_selectedInfo = info;
            showPluginDetails(info);
            break;
        }
    }
}

void VSTPluginDialog::onLoadClicked()
{
    if (m_selectedInfo.uniqueId.isEmpty()) return;

    m_loadedInstance = VSTHostManager::instance().loadPlugin(m_selectedInfo);
    if (!m_loadedInstance) {
        m_scanStatusLabel->setText("Failed to load: " + m_selectedInfo.name);
        return;
    }

    m_scanStatusLabel->setText("Loaded: " + m_loadedInstance->name());
    m_bypassButton->setEnabled(true);
    m_editorButton->setEnabled(m_selectedInfo.hasEditor);
    buildParameterPanel(m_loadedInstance);
}

void VSTPluginDialog::onBypassToggled(bool bypassed)
{
    if (m_loadedInstance)
        m_loadedInstance->bypass = bypassed;
    m_bypassButton->setText(bypassed ? "Bypassed" : "Bypass");
}

void VSTPluginDialog::onParameterSliderMoved(int paramId, int sliderValue)
{
    // Handled inline in buildParameterPanel lambdas; kept here for clarity.
    Q_UNUSED(paramId)
    Q_UNUSED(sliderValue)
}
