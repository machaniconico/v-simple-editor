#include "PythonScript.h"

#include <QColor>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSize>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

// ============================================================================
// PythonSyntaxHighlighter
// ============================================================================

PythonSyntaxHighlighter::PythonSyntaxHighlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
{
    // Keywords
    m_keywordFormat.setForeground(QColor("#CC7832"));
    m_keywordFormat.setFontWeight(QFont::Bold);
    const QStringList keywords = {
        "False", "None", "True", "and", "as", "assert", "async", "await",
        "break", "class", "continue", "def", "del", "elif", "else", "except",
        "finally", "for", "from", "global", "if", "import", "in", "is",
        "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
        "while", "with", "yield"
    };
    for (const QString &kw : keywords) {
        HighlightRule rule;
        rule.pattern = QRegularExpression(QString("\\b%1\\b").arg(kw));
        rule.format = m_keywordFormat;
        m_rules.append(rule);
    }

    // Built-in functions
    m_builtinFormat.setForeground(QColor("#8888CC"));
    const QStringList builtins = {
        "abs", "all", "any", "bin", "bool", "breakpoint", "bytearray",
        "bytes", "callable", "chr", "compile", "complex", "delattr", "dict",
        "dir", "divmod", "enumerate", "eval", "exec", "filter", "float",
        "format", "frozenset", "getattr", "globals", "hasattr", "hash",
        "help", "hex", "id", "input", "int", "isinstance", "issubclass",
        "iter", "len", "list", "locals", "map", "max", "memoryview", "min",
        "next", "object", "oct", "open", "ord", "pow", "print", "property",
        "range", "repr", "reversed", "round", "set", "setattr", "slice",
        "sorted", "staticmethod", "str", "sum", "super", "tuple", "type",
        "vars", "zip"
    };
    for (const QString &bi : builtins) {
        HighlightRule rule;
        rule.pattern = QRegularExpression(QString("\\b%1\\b").arg(bi));
        rule.format = m_builtinFormat;
        m_rules.append(rule);
    }

    // Numbers (int, float, hex, octal, binary)
    m_numberFormat.setForeground(QColor("#6897BB"));
    {
        HighlightRule rule;
        rule.pattern = QRegularExpression(
            "\\b(?:0[xX][0-9A-Fa-f]+|0[oO][0-7]+|0[bB][01]+|"
            "[0-9]+(?:\\.[0-9]*)?(?:[eE][+-]?[0-9]+)?|\\.[0-9]+(?:[eE][+-]?[0-9]+)?)\\b");
        rule.format = m_numberFormat;
        m_rules.append(rule);
    }

    // Single-quoted strings (non-greedy, no newline)
    m_stringFormat.setForeground(QColor("#6A8759"));
    {
        HighlightRule rule;
        rule.pattern = QRegularExpression("'[^'\\\\]*(\\\\.[^'\\\\]*)*'");
        rule.format = m_stringFormat;
        m_rules.append(rule);
    }
    {
        HighlightRule rule;
        rule.pattern = QRegularExpression("\"[^\"\\\\]*(\\\\.[^\"\\\\]*)*\"");
        rule.format = m_stringFormat;
        m_rules.append(rule);
    }

    // Comments – must come after strings so '#' inside strings is already coloured
    m_commentFormat.setForeground(QColor("#808080"));
    m_commentFormat.setFontItalic(true);
    {
        HighlightRule rule;
        rule.pattern = QRegularExpression("#[^\n]*");
        rule.format = m_commentFormat;
        m_rules.append(rule);
    }

    // Triple-quote markers (used in highlightBlock for multi-line state)
    m_tripleDoubleStart = QRegularExpression("\"\"\"");
    m_tripleDoubleEnd   = QRegularExpression("\"\"\"");
    m_tripleSingleStart = QRegularExpression("'''");
    m_tripleSingleEnd   = QRegularExpression("'''");
}

void PythonSyntaxHighlighter::highlightBlock(const QString &text)
{
    // Apply single-line rules
    for (const HighlightRule &rule : m_rules) {
        QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), rule.format);
        }
    }

    // Multi-line triple-quote strings
    // State: 0 = normal, 1 = inside """, 2 = inside '''
    setCurrentBlockState(0);

    auto processTriple = [&](const QRegularExpression &startRe,
                              const QRegularExpression &endRe,
                              int stateId) {
        int startIndex = 0;
        if (previousBlockState() != stateId) {
            QRegularExpressionMatch m = startRe.match(text);
            startIndex = m.hasMatch() ? m.capturedStart() : -1;
        }
        while (startIndex >= 0) {
            QRegularExpressionMatch endMatch = endRe.match(text, startIndex + 3);
            int endIndex;
            int length;
            if (endMatch.hasMatch()) {
                endIndex = endMatch.capturedStart();
                length = endIndex - startIndex + endMatch.capturedLength();
                setCurrentBlockState(0);
            } else {
                setCurrentBlockState(stateId);
                length = text.length() - startIndex;
            }
            setFormat(startIndex, length, m_stringFormat);
            if (endMatch.hasMatch()) {
                QRegularExpressionMatch nextStart = startRe.match(text, startIndex + length);
                startIndex = nextStart.hasMatch() ? nextStart.capturedStart() : -1;
            } else {
                break;
            }
        }
    };

    if (previousBlockState() == 1 || previousBlockState() != 2)
        processTriple(m_tripleDoubleStart, m_tripleDoubleEnd, 1);
    if (previousBlockState() == 2 || previousBlockState() != 1)
        processTriple(m_tripleSingleStart, m_tripleSingleEnd, 2);
}

// ============================================================================
// ScriptEngine
// ============================================================================

// Helper script template written to disk; communicates via JSON on stdin/stdout.
// The host writes one JSON object per line; the script replies with one JSON
// object per line.
static const char *kHelperTemplate = R"PYEOF(
import sys
import json
import traceback
import os

# veditor module exposed to user scripts
class _Timeline:
    def add_clip(self, path, track, position):
        _api_call("timeline.add_clip", [path, track, position])
    def split_at(self, position):
        _api_call("timeline.split_at", [position])
    def delete_clip(self, track, index):
        _api_call("timeline.delete_clip", [track, index])
    def get_clips(self, track):
        return _api_call("timeline.get_clips", [track]) or []

class _Effects:
    def apply(self, clip_index, effect_name, params):
        _api_call("effects.apply", [clip_index, effect_name, params])
    def remove(self, clip_index, effect_index):
        _api_call("effects.remove", [clip_index, effect_index])

class _Project:
    def save(self, path):
        _api_call("project.save", [path])
    def fps(self):
        return _api_call("project.fps", []) or 30
    def resolution(self):
        return _api_call("project.resolution", []) or (1920, 1080)

class _VEditor:
    def __init__(self):
        self.timeline = _Timeline()
        self.effects = _Effects()
        self.project = _Project()
    def export(self, output_path, preset_name=""):
        _api_call("export", [output_path, preset_name])
    def log(self, message):
        _api_call("log", [str(message)])
    def progress(self, current, total):
        _api_call("progress", [current, total])

def _api_call(name, args):
    req = json.dumps({"type": "api_call", "name": name, "args": args})
    sys.stdout.write(req + "\n")
    sys.stdout.flush()
    line = sys.stdin.readline()
    try:
        resp = json.loads(line)
        return resp.get("result")
    except Exception:
        return None

veditor = _VEditor()

# Redirect stdout/stderr
import io
_captured_out = io.StringIO()
_captured_err = io.StringIO()

class _Tee:
    def __init__(self, stream, dest):
        self._stream = stream
        self._dest = dest
    def write(self, data):
        self._dest.write(data)
        msg = json.dumps({"type": "output", "text": data})
        self._stream.write(msg + "\n")
        self._stream.flush()
    def flush(self):
        self._stream.flush()

_real_stdout = sys.stdout
sys.stdout = _Tee(_real_stdout, _captured_out)
sys.stderr = _Tee(_real_stdout, _captured_err)

# Main execution loop
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        req = json.loads(line)
    except Exception:
        continue

    rtype = req.get("type")

    if rtype == "exec":
        code = req.get("code", "")
        _captured_out.truncate(0); _captured_out.seek(0)
        _captured_err.truncate(0); _captured_err.seek(0)
        try:
            exec(compile(code, "<script>", "exec"), {"veditor": veditor})
            result = {"type": "result", "success": True,
                      "output": _captured_out.getvalue(),
                      "error": _captured_err.getvalue()}
        except Exception:
            result = {"type": "result", "success": False,
                      "output": _captured_out.getvalue(),
                      "error": traceback.format_exc()}
        _real_stdout.write(json.dumps(result) + "\n")
        _real_stdout.flush()

    elif rtype == "quit":
        break
)PYEOF";

ScriptEngine::ScriptEngine(QObject *parent)
    : QObject(parent)
{
}

ScriptEngine::~ScriptEngine()
{
    shutdown();
}

bool ScriptEngine::init()
{
    if (m_initialized)
        return true;

    m_pythonExe = pythonExecutable();
    if (m_pythonExe.isEmpty())
        return false;

    // Write helper script to a temp file
    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_helperScriptPath = tmpDir + "/veditor_python_helper.py";
    QFile f(m_helperScriptPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(kHelperTemplate);
        f.close();
    } else {
        return false;
    }

    m_initialized = true;
    m_embedded = false;
    return true;
}

void ScriptEngine::shutdown()
{
    if (!m_initialized)
        return;

    if (!m_helperScriptPath.isEmpty())
        QFile::remove(m_helperScriptPath);

    m_initialized = false;
}

bool ScriptEngine::isAvailable() const
{
    return m_initialized && !m_pythonExe.isEmpty();
}

bool ScriptEngine::isEmbedded() const
{
    return m_embedded;
}

void ScriptEngine::registerFunction(const QString &name,
                                    std::function<QVariant(const QVariantList &)> callback)
{
    m_functions[name] = std::move(callback);
}

ScriptResult ScriptEngine::executeScript(const QString &code)
{
    if (!m_initialized) {
        ScriptResult r;
        r.success = false;
        r.error = "Script engine not initialized. Call init() first.";
        return r;
    }
    return runViaProcess(code);
}

ScriptResult ScriptEngine::executeFile(const QString &filePath)
{
    if (!m_initialized) {
        ScriptResult r;
        r.success = false;
        r.error = "Script engine not initialized. Call init() first.";
        return r;
    }
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ScriptResult r;
        r.success = false;
        r.error = QString("Cannot open file: %1").arg(filePath);
        return r;
    }
    const QString code = QString::fromUtf8(f.readAll());
    return runViaProcess(code, filePath);
}

ScriptResult ScriptEngine::runViaProcess(const QString &code, const QString & /*scriptFile*/)
{
    ScriptResult result;

    QProcess proc;
    proc.setProgram(m_pythonExe);
    proc.setArguments({"-u", m_helperScriptPath});
    proc.start();

    if (!proc.waitForStarted(5000)) {
        result.success = false;
        result.error = "Failed to start Python process.";
        emit errorOccurred(result.error);
        return result;
    }

    // Send the exec request
    QJsonObject req;
    req["type"] = "exec";
    req["code"] = code;
    QByteArray reqLine = QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n";
    proc.write(reqLine);

    QString allOutput;
    QString allError;
    bool gotResult = false;

    // Read lines until the result message arrives or process finishes
    while (!gotResult) {
        if (!proc.waitForReadyRead(10000)) {
            if (proc.state() == QProcess::NotRunning)
                break;
        }

        while (proc.canReadLine()) {
            QByteArray line = proc.readLine();
            QJsonDocument doc = QJsonDocument::fromJson(line);
            if (doc.isNull())
                continue;

            QJsonObject obj = doc.object();
            QString type = obj["type"].toString();

            if (type == "output") {
                QString text = obj["text"].toString();
                allOutput += text;
                emit outputReady(text);
            } else if (type == "api_call") {
                // Dispatch registered C++ function
                QString name = obj["name"].toString();
                QVariantList args;
                for (const QJsonValue &v : obj["args"].toArray())
                    args.append(v.toVariant());

                QJsonObject resp;
                if (m_functions.contains(name)) {
                    QVariant ret = m_functions[name](args);
                    resp["result"] = QJsonValue::fromVariant(ret);
                } else {
                    // Stub: just log and return null
                    QStringList argStrs;
                    for (const QVariant &a : args)
                        argStrs << a.toString();
                    QString msg = QString("[ScriptAPI] %1(%2)").arg(name, argStrs.join(", "));
                    emit outputReady(msg + "\n");
                    resp["result"] = QJsonValue::Null;
                }
                QByteArray respLine = QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n";
                proc.write(respLine);
            } else if (type == "result") {
                result.success = obj["success"].toBool();
                allOutput += obj["output"].toString();
                allError  += obj["error"].toString();
                gotResult = true;
            }
        }
    }

    // Tell the helper to exit cleanly
    QJsonObject quit;
    quit["type"] = "quit";
    proc.write(QJsonDocument(quit).toJson(QJsonDocument::Compact) + "\n");
    proc.waitForFinished(3000);

    result.output = allOutput;
    result.error  = allError;

    if (!result.error.isEmpty())
        emit errorOccurred(result.error);

    return result;
}

QString ScriptEngine::pythonExecutable() const
{
    // PATH-only resolution — do NOT spawn a probe process. On Windows
    // when Python isn't installed, both `python.exe` and `python3.exe`
    // resolve to a Microsoft Store stub that opens an Install dialog;
    // QProcess::waitForFinished blocks against that dialog and was the
    // direct cause of the "app cannot start" regression. findExecutable
    // is a pure PATH lookup with no process spawn, so it never blocks.
    for (const QString &candidate : {"python3", "python"}) {
        const QString resolved = QStandardPaths::findExecutable(candidate);
        if (!resolved.isEmpty())
            return resolved;
    }
    return {};
}

// ============================================================================
// ScriptManager
// ============================================================================

ScriptManager::ScriptManager(QObject *parent)
    : QObject(parent)
{
    m_scriptsDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                   + "/scripts";
    QDir().mkpath(m_scriptsDir);
}

void ScriptManager::loadScriptsFromDirectory(const QString &path)
{
    m_scriptsDir = path;
    QDir dir(path);
    if (!dir.exists())
        dir.mkpath(".");

    m_scripts.clear();
    const QStringList files = dir.entryList({"*.py"}, QDir::Files);
    for (const QString &fileName : files) {
        QFile f(dir.filePath(fileName));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString name = QFileInfo(fileName).baseName();
            m_scripts[name] = QString::fromUtf8(f.readAll());
        }
    }
}

QStringList ScriptManager::savedScripts() const
{
    return m_scripts.keys();
}

QString ScriptManager::scriptCode(const QString &name) const
{
    return m_scripts.value(name);
}

bool ScriptManager::saveScript(const QString &name, const QString &code)
{
    QDir dir(m_scriptsDir);
    if (!dir.exists())
        dir.mkpath(".");

    QString filePath = dir.filePath(name + ".py");
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    f.write(code.toUtf8());
    m_scripts[name] = code;
    return true;
}

bool ScriptManager::deleteScript(const QString &name)
{
    QString filePath = QDir(m_scriptsDir).filePath(name + ".py");
    if (!QFile::remove(filePath))
        return false;
    m_scripts.remove(name);
    return true;
}

QString ScriptManager::scriptsDirectory() const
{
    return m_scriptsDir;
}

// ============================================================================
// ScriptConsole
// ============================================================================

ScriptConsole::ScriptConsole(ScriptEngine *engine, ScriptManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_manager(manager)
{
    setWindowTitle("Python Script Console");
    setMinimumSize(900, 600);
    setupUI();

    connect(m_engine, &ScriptEngine::outputReady, this, &ScriptConsole::onEngineOutput);
    connect(m_engine, &ScriptEngine::errorOccurred, this, &ScriptConsole::onEngineError);
}

void ScriptConsole::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // Toolbar
    auto *toolbar = new QToolBar(this);
    toolbar->setIconSize(QSize(16, 16));
    setupToolbar(toolbar);
    mainLayout->addWidget(toolbar);

    // Horizontal splitter: left = editor+output tabs, right = library
    auto *hSplitter = new QSplitter(Qt::Horizontal, this);

    // Left side: vertical splitter with editor on top, output on bottom
    auto *vSplitter = new QSplitter(Qt::Vertical, hSplitter);

    m_tabs = new QTabWidget(vSplitter);

    // Tab 1: Code editor
    auto *editorWidget = new QWidget(m_tabs);
    setupEditorTab(editorWidget);
    m_tabs->addTab(editorWidget, "Script Editor");

    // Tab 2: API Reference
    auto *apiWidget = new QWidget(m_tabs);
    setupApiTab(apiWidget);
    m_tabs->addTab(apiWidget, "API Reference");

    vSplitter->addWidget(m_tabs);

    // Output console
    m_output = new QPlainTextEdit(vSplitter);
    m_output->setReadOnly(true);
    m_output->setPlaceholderText("Output appears here...");
    m_output->setFont(QFont("Courier New", 10));
    m_output->setStyleSheet("background-color: #1e1e1e; color: #d4d4d4;");
    vSplitter->addWidget(m_output);
    vSplitter->setSizes({380, 180});

    hSplitter->addWidget(vSplitter);

    // Right side: script library
    auto *libraryWidget = new QWidget(hSplitter);
    setupLibraryPanel(libraryWidget);
    hSplitter->addWidget(libraryWidget);
    hSplitter->setSizes({650, 220});

    mainLayout->addWidget(hSplitter);

    // Status bar hint
    auto *hint = new QLabel(
        m_engine->isAvailable()
            ? QString("Python available (%1 mode)")
                  .arg(m_engine->isEmbedded() ? "embedded" : "process")
            : "Python not found – install Python 3 to run scripts",
        this);
    hint->setStyleSheet("color: gray; font-size: 11px;");
    mainLayout->addWidget(hint);

    refreshLibrary();
}

void ScriptConsole::setupToolbar(QToolBar *toolbar)
{
    m_runAction = toolbar->addAction("Run");
    m_runAction->setShortcut(QKeySequence("Ctrl+Return"));
    m_runAction->setToolTip("Run script (Ctrl+Return)");
    connect(m_runAction, &QAction::triggered, this, &ScriptConsole::onRun);

    m_stopAction = toolbar->addAction("Stop");
    m_stopAction->setEnabled(false);
    connect(m_stopAction, &QAction::triggered, this, &ScriptConsole::onStop);

    toolbar->addSeparator();

    auto *clearAction = toolbar->addAction("Clear Output");
    connect(clearAction, &QAction::triggered, this, &ScriptConsole::onClear);

    toolbar->addSeparator();

    auto *openAction = toolbar->addAction("Open File...");
    connect(openAction, &QAction::triggered, this, &ScriptConsole::onOpenFile);

    auto *saveAction = toolbar->addAction("Save File...");
    connect(saveAction, &QAction::triggered, this, &ScriptConsole::onSaveFile);
}

void ScriptConsole::setupEditorTab(QWidget *container)
{
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    m_editor = new QPlainTextEdit(container);
    m_editor->setFont(QFont("Courier New", 11));
    m_editor->setTabStopDistance(28.0);  // ~4 spaces
    m_editor->setPlaceholderText(
        "# Write your Python script here\n"
        "# Example:\n"
        "# veditor.log('Hello from Python!')\n"
        "# veditor.timeline.add_clip('/path/to/clip.mp4', 0, 0.0)\n");
    m_editor->setStyleSheet(
        "QPlainTextEdit { background-color: #1e1e1e; color: #d4d4d4; "
        "selection-background-color: #264f78; }");

    new PythonSyntaxHighlighter(m_editor->document());

    layout->addWidget(m_editor);
}

void ScriptConsole::setupLibraryPanel(QWidget *container)
{
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(4, 0, 0, 0);

    auto *label = new QLabel("Saved Scripts", container);
    label->setStyleSheet("font-weight: bold;");
    layout->addWidget(label);

    m_library = new QTreeWidget(container);
    m_library->setHeaderHidden(true);
    m_library->setRootIsDecorated(false);
    connect(m_library, &QTreeWidget::itemDoubleClicked,
            this, &ScriptConsole::onScriptSelected);
    layout->addWidget(m_library);

    auto *saveBtn = new QPushButton("Save to Library", container);
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        QString name = QInputDialog_getText(this);
        if (name.isEmpty()) return;
        m_manager->saveScript(name, m_editor->toPlainText());
        refreshLibrary();
    });
    layout->addWidget(saveBtn);

    auto *deleteBtn = new QPushButton("Delete Selected", container);
    connect(deleteBtn, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem *item = m_library->currentItem();
        if (!item) return;
        m_manager->deleteScript(item->text(0));
        refreshLibrary();
    });
    layout->addWidget(deleteBtn);
}

void ScriptConsole::setupApiTab(QWidget *container)
{
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *apiDoc = new QPlainTextEdit(container);
    apiDoc->setReadOnly(true);
    apiDoc->setFont(QFont("Courier New", 10));
    apiDoc->setPlainText(
        "# VEditor Python API Reference\n"
        "#\n"
        "# The 'veditor' object is pre-imported in every script.\n"
        "\n"
        "## Timeline\n"
        "veditor.timeline.add_clip(path, track, position)\n"
        "    Add a media clip to the timeline.\n"
        "    path     : str  - path to media file\n"
        "    track    : int  - track index (0-based)\n"
        "    position : float - start position in seconds\n"
        "\n"
        "veditor.timeline.split_at(position)\n"
        "    Split the clip at the given timeline position (seconds).\n"
        "\n"
        "veditor.timeline.delete_clip(track, index)\n"
        "    Delete a clip by track and clip index.\n"
        "\n"
        "veditor.timeline.get_clips(track) -> list\n"
        "    Return a list of clip info dicts for the given track.\n"
        "\n"
        "## Effects\n"
        "veditor.effects.apply(clip_index, effect_name, params)\n"
        "    Apply an effect to a clip.\n"
        "    params : dict of parameter name -> value\n"
        "\n"
        "veditor.effects.remove(clip_index, effect_index)\n"
        "    Remove an effect from a clip.\n"
        "\n"
        "## Export\n"
        "veditor.export(output_path, preset_name='')\n"
        "    Export the project.\n"
        "\n"
        "## Project\n"
        "veditor.project.save(path)\n"
        "    Save the project to path.\n"
        "\n"
        "veditor.project.fps() -> int\n"
        "    Return the project frame rate.\n"
        "\n"
        "veditor.project.resolution() -> (width, height)\n"
        "    Return the project resolution as a tuple.\n"
        "\n"
        "## Utilities\n"
        "veditor.log(message)\n"
        "    Print a message to the script console.\n"
        "\n"
        "veditor.progress(current, total)\n"
        "    Report progress (used for long-running operations).\n"
    );
    layout->addWidget(apiDoc);
}

// Minimal inline replacement for QInputDialog::getText to avoid extra include
// (keeps the header clean – uses QDialog directly)
QString ScriptConsole::QInputDialog_getText(QWidget *parent)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Save Script");
    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel("Script name:", &dlg));
    auto *edit = new QLineEdit(&dlg);
    lay->addWidget(edit);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted)
        return edit->text().trimmed();
    return {};
}

void ScriptConsole::refreshLibrary()
{
    if (!m_manager)
        return;
    m_library->clear();
    for (const QString &name : m_manager->savedScripts()) {
        auto *item = new QTreeWidgetItem(m_library, {name});
        item->setToolTip(0, name);
    }
}

void ScriptConsole::onRun()
{
    if (!m_engine->isAvailable()) {
        appendOutput("Python is not available. Install Python 3 and restart the editor.\n", true);
        return;
    }
    QString code = m_editor->toPlainText().trimmed();
    if (code.isEmpty())
        return;

    m_running = true;
    m_runAction->setEnabled(false);
    m_stopAction->setEnabled(true);
    appendOutput("--- Running script ---\n");

    ScriptResult result = m_engine->executeScript(code);

    m_running = false;
    m_runAction->setEnabled(true);
    m_stopAction->setEnabled(false);

    if (!result.output.isEmpty() && !result.output.trimmed().isEmpty())
        appendOutput(result.output);

    if (result.success)
        appendOutput("--- Done ---\n");
    else
        appendOutput(result.error, true);
}

void ScriptConsole::onStop()
{
    // Process mode: killing is handled by QProcess going out of scope.
    // In a full implementation this would signal the running process.
    appendOutput("--- Stop requested (process will exit after current operation) ---\n");
    m_stopAction->setEnabled(false);
}

void ScriptConsole::onClear()
{
    m_output->clear();
}

void ScriptConsole::onOpenFile()
{
    QString path = QFileDialog::getOpenFileName(
        this, "Open Python Script", {}, "Python Files (*.py);;All Files (*)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        m_editor->setPlainText(QString::fromUtf8(f.readAll()));
}

void ScriptConsole::onSaveFile()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Save Python Script", {}, "Python Files (*.py);;All Files (*)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(m_editor->toPlainText().toUtf8());
}

void ScriptConsole::onScriptSelected(QTreeWidgetItem *item, int /*column*/)
{
    if (!item || !m_manager)
        return;
    QString code = m_manager->scriptCode(item->text(0));
    if (!code.isEmpty())
        m_editor->setPlainText(code);
}

void ScriptConsole::onEngineOutput(const QString &text)
{
    appendOutput(text);
}

void ScriptConsole::onEngineError(const QString &error)
{
    appendOutput(error, true);
}

void ScriptConsole::appendOutput(const QString &text, bool isError)
{
    if (isError) {
        // Append in red
        QTextCharFormat fmt;
        fmt.setForeground(QColor("#f48771"));
        QTextCursor cursor = m_output->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(text, fmt);
        m_output->setTextCursor(cursor);
    } else {
        m_output->moveCursor(QTextCursor::End);
        m_output->insertPlainText(text);
    }
    m_output->ensureCursorVisible();
}

// ============================================================================
// ScriptAPI
// ============================================================================

ScriptAPI::ScriptAPI(ScriptEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
}

void ScriptAPI::registerAll()
{
    // Timeline
    m_engine->registerFunction("timeline.add_clip", [this](const QVariantList &args) -> QVariant {
        QString path = args.value(0).toString();
        int track = args.value(1).toInt();
        double pos = args.value(2).toDouble();
        emit logMessage(QString("[timeline] add_clip('%1', track=%2, pos=%3)")
                            .arg(path).arg(track).arg(pos));
        return {};
    });

    m_engine->registerFunction("timeline.split_at", [this](const QVariantList &args) -> QVariant {
        double pos = args.value(0).toDouble();
        emit logMessage(QString("[timeline] split_at(%1)").arg(pos));
        return {};
    });

    m_engine->registerFunction("timeline.delete_clip", [this](const QVariantList &args) -> QVariant {
        int track = args.value(0).toInt();
        int index = args.value(1).toInt();
        emit logMessage(QString("[timeline] delete_clip(track=%1, index=%2)").arg(track).arg(index));
        return {};
    });

    m_engine->registerFunction("timeline.get_clips", [this](const QVariantList &args) -> QVariant {
        int track = args.value(0).toInt();
        emit logMessage(QString("[timeline] get_clips(track=%1)").arg(track));
        return QVariantList{};   // stub returns empty list
    });

    // Effects
    m_engine->registerFunction("effects.apply", [this](const QVariantList &args) -> QVariant {
        int clipIdx = args.value(0).toInt();
        QString effectName = args.value(1).toString();
        emit logMessage(QString("[effects] apply(clip=%1, effect='%2')").arg(clipIdx).arg(effectName));
        return {};
    });

    m_engine->registerFunction("effects.remove", [this](const QVariantList &args) -> QVariant {
        int clipIdx = args.value(0).toInt();
        int fxIdx = args.value(1).toInt();
        emit logMessage(QString("[effects] remove(clip=%1, fx=%2)").arg(clipIdx).arg(fxIdx));
        return {};
    });

    // Export
    m_engine->registerFunction("export", [this](const QVariantList &args) -> QVariant {
        QString outPath = args.value(0).toString();
        QString preset = args.value(1).toString();
        emit logMessage(QString("[export] path='%1' preset='%2'").arg(outPath).arg(preset));
        return {};
    });

    // Project
    m_engine->registerFunction("project.save", [this](const QVariantList &args) -> QVariant {
        QString path = args.value(0).toString();
        emit logMessage(QString("[project] save('%1')").arg(path));
        return {};
    });

    m_engine->registerFunction("project.fps", [](const QVariantList &) -> QVariant {
        return 30;
    });

    m_engine->registerFunction("project.resolution", [](const QVariantList &) -> QVariant {
        return QVariantList{1920, 1080};
    });

    // Utilities
    m_engine->registerFunction("log", [this](const QVariantList &args) -> QVariant {
        emit logMessage(args.value(0).toString());
        return {};
    });

    m_engine->registerFunction("progress", [this](const QVariantList &args) -> QVariant {
        int cur = args.value(0).toInt();
        int total = args.value(1).toInt();
        emit logMessage(QString("[progress] %1 / %2").arg(cur).arg(total));
        return {};
    });
}
