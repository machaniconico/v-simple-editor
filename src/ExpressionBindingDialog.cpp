#include "ExpressionBindingDialog.h"

#include "Expression.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ExpressionBindingDialog::ExpressionBindingDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("式バインディング編集"));
    resize(820, 520);

    // -----------------------------------------------------------------------
    // Left panel: binding list + add/remove buttons
    // -----------------------------------------------------------------------
    m_listWidget = new QListWidget(this);
    m_listWidget->setMinimumWidth(220);

    m_addButton    = new QPushButton(QStringLiteral("追加"), this);
    m_removeButton = new QPushButton(QStringLiteral("削除"), this);
    m_removeButton->setEnabled(false);

    auto *leftButtonLayout = new QHBoxLayout;
    leftButtonLayout->addWidget(m_addButton);
    leftButtonLayout->addWidget(m_removeButton);
    leftButtonLayout->addStretch();

    auto *leftLayout = new QVBoxLayout;
    leftLayout->addWidget(m_listWidget);
    leftLayout->addLayout(leftButtonLayout);

    auto *leftWidget = new QWidget(this);
    leftWidget->setLayout(leftLayout);

    // -----------------------------------------------------------------------
    // Right panel: expression editor, validity, preview, functions reference
    // -----------------------------------------------------------------------

    // Expression editor
    m_codeEdit = new QPlainTextEdit(this);
    m_codeEdit->setPlaceholderText(QStringLiteral("ここに式を入力 (例: value * 0.5)"));
    m_codeEdit->setEnabled(false);

    // Validity label
    m_validityLabel = new QLabel(QStringLiteral("—"), this);
    m_validityLabel->setWordWrap(true);

    // Evaluated value preview
    m_previewLabel = new QLabel(QStringLiteral("—"), this);
    m_previewLabel->setWordWrap(true);

    // Functions reference (collapsible via a checkable QGroupBox)
    m_funcBox  = new QGroupBox(QStringLiteral("関数リファレンス"), this);
    m_funcBox->setCheckable(true);
    m_funcBox->setChecked(false);   // collapsed by default
    m_funcList = new QListWidget(m_funcBox);
    m_funcList->setMaximumHeight(120);

    const QStringList funcs = Expression::availableFunctions();
    for (const QString &fn : funcs) {
        m_funcList->addItem(fn);
    }

    auto *funcBoxLayout = new QVBoxLayout;
    funcBoxLayout->addWidget(m_funcList);
    m_funcBox->setLayout(funcBoxLayout);

    // Connect collapse toggle
    connect(m_funcBox, &QGroupBox::toggled, this, [this](bool open) {
        m_funcList->setVisible(open);
    });
    m_funcList->setVisible(false);

    // Assemble right panel
    auto *validityRow = new QHBoxLayout;
    validityRow->addWidget(new QLabel(QStringLiteral("構文:"), this));
    validityRow->addWidget(m_validityLabel, 1);

    auto *previewRow = new QHBoxLayout;
    previewRow->addWidget(new QLabel(QStringLiteral("評価値:"), this));
    previewRow->addWidget(m_previewLabel, 1);

    auto *rightLayout = new QVBoxLayout;
    rightLayout->addWidget(new QLabel(QStringLiteral("式コード:"), this));
    rightLayout->addWidget(m_codeEdit, 1);
    rightLayout->addLayout(validityRow);
    rightLayout->addLayout(previewRow);
    rightLayout->addWidget(m_funcBox);

    auto *rightWidget = new QWidget(this);
    rightWidget->setLayout(rightLayout);

    // -----------------------------------------------------------------------
    // Splitter
    // -----------------------------------------------------------------------
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    // -----------------------------------------------------------------------
    // OK / Cancel
    // -----------------------------------------------------------------------
    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // -----------------------------------------------------------------------
    // Top-level layout
    // -----------------------------------------------------------------------
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(splitter, 1);
    mainLayout->addWidget(m_buttonBox);
    setLayout(mainLayout);

    // -----------------------------------------------------------------------
    // Signal / slot connections
    // -----------------------------------------------------------------------
    connect(m_addButton, &QPushButton::clicked,
            this, &ExpressionBindingDialog::onAddBinding);

    connect(m_removeButton, &QPushButton::clicked,
            this, &ExpressionBindingDialog::onRemoveBinding);

    connect(m_listWidget, &QListWidget::currentRowChanged,
            this, [this](int) { onSelectionChanged(); });

    connect(m_codeEdit, &QPlainTextEdit::textChanged,
            this, &ExpressionBindingDialog::onExpressionTextChanged);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ExpressionBindingDialog::setBindings(const exprbind::ClipExpressionBindings &b)
{
    m_bindings = b;

    m_updating = true;
    m_listWidget->clear();

    const QStringList paths = b.boundPaths();
    for (const QString &path : paths) {
        const QString code = b.expression(path);
        const QString label = path + QStringLiteral("  =  ") + code;
        auto *item = new QListWidgetItem(label, m_listWidget);
        item->setData(Qt::UserRole,       path);
        item->setData(Qt::UserRole + 1,   code);
    }

    m_updating = false;

    // Select first row if any
    if (m_listWidget->count() > 0) {
        m_listWidget->setCurrentRow(0);
    } else {
        onSelectionChanged();
    }
}

exprbind::ClipExpressionBindings ExpressionBindingDialog::bindings() const
{
    return m_bindings;
}

void ExpressionBindingDialog::setContextHints(double durationSeconds, double fps, QSize canvas,
                                              double dynamicTimeOffsetSec)
{
    m_duration = durationSeconds;
    m_fps      = fps;
    m_canvas   = canvas;
    m_dynamicTimeOffsetSec = dynamicTimeOffsetSec;
}

// ---------------------------------------------------------------------------
// Private slots
// ---------------------------------------------------------------------------

void ExpressionBindingDialog::onAddBinding()
{
    // Gather known paths
    const QStringList paths = exprbind::knownPropertyPaths();
    if (paths.isEmpty()) {
        return;
    }

    // Show an input dialog with a combo of known paths
    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this,
        QStringLiteral("プロパティを選択"),
        QStringLiteral("バインドするプロパティパス:"),
        paths,
        0,
        /*editable=*/false,
        &ok);

    if (!ok || chosen.isEmpty()) {
        return;
    }

    // If already in the list, just select it instead of adding a duplicate
    for (int i = 0; i < m_listWidget->count(); ++i) {
        if (m_listWidget->item(i)->data(Qt::UserRole).toString() == chosen) {
            m_listWidget->setCurrentRow(i);
            return;
        }
    }

    // Add new row with empty expression
    const QString label = chosen + QStringLiteral("  =  ");
    auto *item = new QListWidgetItem(label, m_listWidget);
    item->setData(Qt::UserRole,     chosen);
    item->setData(Qt::UserRole + 1, QString());

    // Update internal bindings (empty code = cleared, so we register an empty
    // entry only in the list; m_bindings will be set when user types code).
    // We still emit bindingsChanged so callers know the structure changed.
    m_listWidget->setCurrentItem(item);

    emit bindingsChanged();
}

void ExpressionBindingDialog::onRemoveBinding()
{
    const int row = m_listWidget->currentRow();
    if (row < 0) {
        return;
    }

    const QString path = m_listWidget->item(row)->data(Qt::UserRole).toString();
    m_bindings.clearExpression(path);

    m_updating = true;
    delete m_listWidget->takeItem(row);
    m_updating = false;

    // Select next sensible row
    const int newCount = m_listWidget->count();
    if (newCount > 0) {
        m_listWidget->setCurrentRow(qMin(row, newCount - 1));
    } else {
        onSelectionChanged();
    }

    emit bindingsChanged();
}

void ExpressionBindingDialog::onSelectionChanged()
{
    if (m_updating) {
        return;
    }

    const int row = m_listWidget->currentRow();
    const bool hasSelection = (row >= 0);

    m_removeButton->setEnabled(hasSelection);
    m_codeEdit->setEnabled(hasSelection);

    if (!hasSelection) {
        m_updating = true;
        m_codeEdit->clear();
        m_updating = false;
        m_validityLabel->setText(QStringLiteral("—"));
        m_validityLabel->setStyleSheet(QString());
        m_previewLabel->setText(QStringLiteral("—"));
        return;
    }

    const QString code = m_listWidget->item(row)->data(Qt::UserRole + 1).toString();

    m_updating = true;
    m_codeEdit->setPlainText(code);
    m_updating = false;

    updateValidityAndPreview(code);
}

void ExpressionBindingDialog::onExpressionTextChanged()
{
    if (m_updating) {
        return;
    }

    const int row = m_listWidget->currentRow();
    if (row < 0) {
        return;
    }

    const QString code = m_codeEdit->toPlainText();
    const QString path = m_listWidget->item(row)->data(Qt::UserRole).toString();

    // Update the list item display
    const QString displayLabel = path + QStringLiteral("  =  ") + code;
    m_listWidget->item(row)->setText(displayLabel);
    m_listWidget->item(row)->setData(Qt::UserRole + 1, code);

    // Update internal bindings
    const QString trimmed = code.trimmed();
    if (trimmed.isEmpty()) {
        m_bindings.clearExpression(path);
    } else {
        m_bindings.setExpression(path, code);
    }

    updateValidityAndPreview(code);

    emit bindingsChanged();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

ExpressionContext ExpressionBindingDialog::buildSampleContext(double t) const
{
    ExpressionContext ctx;
    ctx.time         = t + m_dynamicTimeOffsetSec;
    ctx.fps          = m_fps > 0.0 ? m_fps : 30.0;
    ctx.duration     = m_duration;
    ctx.canvasWidth  = m_canvas.width()  > 0 ? m_canvas.width()  : 1920;
    ctx.canvasHeight = m_canvas.height() > 0 ? m_canvas.height() : 1080;
    ctx.value        = 100.0; // nominal upstream value
    ctx.layerIndex   = 0;
    // sampleValueAtTime left empty (default)
    return ctx;
}

void ExpressionBindingDialog::updateValidityAndPreview(const QString &code)
{
    const QString trimmed = code.trimmed();

    if (trimmed.isEmpty()) {
        m_validityLabel->setText(QStringLiteral("(空)"));
        m_validityLabel->setStyleSheet(QStringLiteral("color: gray;"));
        m_previewLabel->setText(QStringLiteral("—"));
        return;
    }

    // --- Validity ---
    const QString validationError = Expression::validate(code);
    if (validationError.isEmpty()) {
        m_validityLabel->setText(QStringLiteral("OK"));
        m_validityLabel->setStyleSheet(QStringLiteral("color: green; font-weight: bold;"));
    } else {
        m_validityLabel->setText(validationError);
        m_validityLabel->setStyleSheet(QStringLiteral("color: red;"));
    }

    // --- Evaluated value preview ---
    // Choose sample times
    QVector<double> sampleTimes;
    if (m_duration > 0.0) {
        sampleTimes << 0.0 << m_duration * 0.25 << m_duration * 0.5 << m_duration * 0.75;
    } else {
        sampleTimes << 0.0 << 0.5 << 1.0;
    }

    QString previewText;
    bool anyValid = false;

    for (int i = 0; i < sampleTimes.size(); ++i) {
        const double t = sampleTimes.at(i);
        const ExpressionContext ctx = buildSampleContext(t);
        const ExpressionResult  res = Expression::evaluate(code, ctx);

        if (!previewText.isEmpty()) {
            previewText += QStringLiteral("  ");
        }

        previewText += QStringLiteral("t=")
                     + QString::number(t, 'f', 2)
                     + QStringLiteral(": ");

        if (res.success) {
            previewText += QString::number(res.value, 'f', 2);
            anyValid = true;
        } else {
            previewText += QStringLiteral("—");
        }
    }

    m_previewLabel->setText(anyValid ? previewText : QStringLiteral("—"));
}
