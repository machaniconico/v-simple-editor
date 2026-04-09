#pragma once

#include <QObject>
#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <QProcess>
#include <QMap>
#include <QTreeWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QAction>
#include <QLabel>
#include <QSplitter>
#include <functional>

// ---------------------------------------------------------------------------
// ScriptResult
// ---------------------------------------------------------------------------

struct ScriptResult {
    bool success = false;
    QString output;
    QString error;
    QVariant returnValue;
};

// ---------------------------------------------------------------------------
// PythonSyntaxHighlighter
// ---------------------------------------------------------------------------

class PythonSyntaxHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit PythonSyntaxHighlighter(QTextDocument *parent = nullptr);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct HighlightRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    QVector<HighlightRule> m_rules;

    QTextCharFormat m_keywordFormat;
    QTextCharFormat m_builtinFormat;
    QTextCharFormat m_commentFormat;
    QTextCharFormat m_stringFormat;
    QTextCharFormat m_numberFormat;

    // Triple-quote string state tracking
    QRegularExpression m_tripleDoubleStart;
    QRegularExpression m_tripleDoubleEnd;
    QRegularExpression m_tripleSingleStart;
    QRegularExpression m_tripleSingleEnd;
};

// ---------------------------------------------------------------------------
// ScriptEngine
// ---------------------------------------------------------------------------

class ScriptEngine : public QObject
{
    Q_OBJECT

public:
    explicit ScriptEngine(QObject *parent = nullptr);
    ~ScriptEngine();

    bool init();
    void shutdown();

    bool isAvailable() const;
    bool isEmbedded() const;   // true = Python/C API, false = QProcess fallback

    ScriptResult executeScript(const QString &code);
    ScriptResult executeFile(const QString &filePath);

    // Expose a C++ callback to Python scripts under the given name.
    // The callback receives a QVariantList of arguments and returns a QVariant.
    void registerFunction(const QString &name,
                          std::function<QVariant(const QVariantList &)> callback);

signals:
    void outputReady(const QString &text);
    void errorOccurred(const QString &error);

private:
    ScriptResult runViaProcess(const QString &code, const QString &scriptFile = {});
    QString pythonExecutable() const;

    bool m_initialized = false;
    bool m_embedded = false;           // always false in this build (process mode)
    QString m_pythonExe;

    QMap<QString, std::function<QVariant(const QVariantList &)>> m_functions;

    // Persistent helper script written to temp dir on init
    QString m_helperScriptPath;
};

// ---------------------------------------------------------------------------
// ScriptManager
// ---------------------------------------------------------------------------

class ScriptManager : public QObject
{
    Q_OBJECT

public:
    explicit ScriptManager(QObject *parent = nullptr);

    void loadScriptsFromDirectory(const QString &path);

    QStringList savedScripts() const;
    QString scriptCode(const QString &name) const;

    bool saveScript(const QString &name, const QString &code);
    bool deleteScript(const QString &name);

    QString scriptsDirectory() const;

private:
    QString m_scriptsDir;
    QMap<QString, QString> m_scripts;   // name -> code
};

// ---------------------------------------------------------------------------
// ScriptConsole  (QDialog)
// ---------------------------------------------------------------------------

class ScriptConsole : public QDialog
{
    Q_OBJECT

public:
    explicit ScriptConsole(ScriptEngine *engine,
                           ScriptManager *manager,
                           QWidget *parent = nullptr);

private slots:
    void onRun();
    void onStop();
    void onClear();
    void onOpenFile();
    void onSaveFile();
    void onScriptSelected(QTreeWidgetItem *item, int column);
    void onEngineOutput(const QString &text);
    void onEngineError(const QString &error);

private:
    void setupUI();
    void setupToolbar(QToolBar *toolbar);
    void setupEditorTab(QWidget *container);
    void setupLibraryPanel(QWidget *container);
    void setupApiTab(QWidget *container);
    void appendOutput(const QString &text, bool isError = false);
    void refreshLibrary();
    static QString QInputDialog_getText(QWidget *parent);

    ScriptEngine  *m_engine;
    ScriptManager *m_manager;

    QPlainTextEdit *m_editor;
    QPlainTextEdit *m_output;
    QTreeWidget    *m_library;
    QTabWidget     *m_tabs;

    QAction *m_runAction;
    QAction *m_stopAction;

    bool m_running = false;
};

// ---------------------------------------------------------------------------
// ScriptAPI  – stub implementations that log actions
// ---------------------------------------------------------------------------

class ScriptAPI : public QObject
{
    Q_OBJECT

public:
    explicit ScriptAPI(ScriptEngine *engine, QObject *parent = nullptr);

    // Register all API functions with the engine
    void registerAll();

signals:
    void logMessage(const QString &msg);

private:
    ScriptEngine *m_engine;
};
