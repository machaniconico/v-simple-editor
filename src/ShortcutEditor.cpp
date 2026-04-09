#include "ShortcutEditor.h"
#include <QSettings>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QApplication>

// ---------------------------------------------------------------------------
// ShortcutManager
// ---------------------------------------------------------------------------

ShortcutManager &ShortcutManager::instance()
{
    static ShortcutManager inst;
    return inst;
}

void ShortcutManager::registerShortcut(const QString &id,
                                        const QString &displayName,
                                        const QKeySequence &defaultKeySequence,
                                        QAction *action)
{
    ShortcutEntry entry;
    entry.id = id;
    entry.displayName = displayName;
    entry.defaultKey = defaultKeySequence;
    entry.currentKey = defaultKeySequence;
    entry.action = action;
    m_shortcuts[id] = entry;

    if (action)
        action->setShortcut(defaultKeySequence);
}

void ShortcutManager::resetToDefaults()
{
    for (auto &entry : m_shortcuts) {
        entry.currentKey = entry.defaultKey;
        if (entry.action)
            entry.action->setShortcut(entry.defaultKey);
    }
    saveShortcuts();
}

void ShortcutManager::saveShortcuts()
{
    QSettings settings("VEditorSimple", "Shortcuts");
    for (const auto &entry : m_shortcuts)
        settings.setValue(entry.id, entry.currentKey.toString());
}

void ShortcutManager::loadShortcuts()
{
    QSettings settings("VEditorSimple", "Shortcuts");
    for (auto &entry : m_shortcuts) {
        if (settings.contains(entry.id)) {
            QKeySequence key(settings.value(entry.id).toString());
            entry.currentKey = key;
            if (entry.action)
                entry.action->setShortcut(key);
        }
    }
}

QKeySequence ShortcutManager::shortcutForId(const QString &id) const
{
    if (m_shortcuts.contains(id))
        return m_shortcuts[id].currentKey;
    return QKeySequence();
}

QVector<ShortcutEntry> ShortcutManager::allShortcuts() const
{
    return QVector<ShortcutEntry>(m_shortcuts.values().begin(),
                                  m_shortcuts.values().end());
}

void ShortcutManager::applyShortcut(const QString &id, const QKeySequence &key)
{
    if (!m_shortcuts.contains(id))
        return;
    m_shortcuts[id].currentKey = key;
    if (m_shortcuts[id].action)
        m_shortcuts[id].action->setShortcut(key);
}

// ---------------------------------------------------------------------------
// ShortcutEditorDialog
// ---------------------------------------------------------------------------

ShortcutEditorDialog::ShortcutEditorDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Customize Shortcuts");
    setMinimumSize(600, 500);
    setupUI();
    populateTable();
}

void ShortcutEditorDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);

    // Search bar
    auto *searchLayout = new QHBoxLayout;
    auto *searchLabel = new QLabel("Filter:", this);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Type to filter shortcuts...");
    m_searchEdit->setClearButtonEnabled(true);
    searchLayout->addWidget(searchLabel);
    searchLayout->addWidget(m_searchEdit);
    mainLayout->addLayout(searchLayout);

    // Table
    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({"Name", "Current Key", "Default Key"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    mainLayout->addWidget(m_table);

    // Hint label
    m_hintLabel = new QLabel("Click a row, then press a key combination to reassign it.", this);
    m_hintLabel->setStyleSheet("color: #666; font-size: 12px; padding: 4px;");
    mainLayout->addWidget(m_hintLabel);

    // Row buttons
    auto *rowBtnLayout = new QHBoxLayout;
    m_resetRowBtn = new QPushButton("Reset Selected", this);
    m_resetRowBtn->setEnabled(false);
    rowBtnLayout->addWidget(m_resetRowBtn);
    rowBtnLayout->addStretch();
    mainLayout->addLayout(rowBtnLayout);

    // Dialog buttons
    auto *buttons = new QDialogButtonBox(this);
    m_applyBtn = buttons->addButton("Apply", QDialogButtonBox::ApplyRole);
    m_resetAllBtn = buttons->addButton("Reset All", QDialogButtonBox::ResetRole);
    buttons->addButton(QDialogButtonBox::Ok);
    buttons->addButton(QDialogButtonBox::Cancel);
    mainLayout->addWidget(buttons);

    // Connections
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ShortcutEditorDialog::onSearchChanged);
    connect(m_table, &QTableWidget::cellClicked, this, &ShortcutEditorDialog::onCellClicked);
    connect(m_resetRowBtn, &QPushButton::clicked, this, &ShortcutEditorDialog::onResetRow);
    connect(m_resetAllBtn, &QPushButton::clicked, this, &ShortcutEditorDialog::onResetAll);
    connect(m_applyBtn, &QPushButton::clicked, this, &ShortcutEditorDialog::onApply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        onApply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ShortcutEditorDialog::populateTable(const QString &filter)
{
    m_table->setRowCount(0);
    stopCapture();

    const QVector<ShortcutEntry> entries = ShortcutManager::instance().allShortcuts();
    for (const auto &entry : entries) {
        if (!filter.isEmpty() &&
            !entry.displayName.contains(filter, Qt::CaseInsensitive) &&
            !entry.currentKey.toString().contains(filter, Qt::CaseInsensitive))
        {
            continue;
        }

        int row = m_table->rowCount();
        m_table->insertRow(row);

        auto *nameItem = new QTableWidgetItem(entry.displayName);
        nameItem->setData(Qt::UserRole, entry.id);
        m_table->setItem(row, 0, nameItem);

        auto *currentItem = new QTableWidgetItem(entry.currentKey.toString(QKeySequence::NativeText));
        currentItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 1, currentItem);

        auto *defaultItem = new QTableWidgetItem(entry.defaultKey.toString(QKeySequence::NativeText));
        defaultItem->setTextAlignment(Qt::AlignCenter);
        defaultItem->setForeground(QApplication::palette().color(QPalette::PlaceholderText));
        m_table->setItem(row, 2, defaultItem);
    }

    m_resetRowBtn->setEnabled(false);
}

void ShortcutEditorDialog::startCapture(int row)
{
    m_captureRow = row;
    m_capturing = true;
    m_table->item(row, 1)->setText("Press a key...");
    m_table->item(row, 1)->setBackground(QColor(60, 100, 160));
    m_hintLabel->setText("Press a key combination to assign. Press Escape to cancel.");
    m_resetRowBtn->setEnabled(true);
    setFocus();
}

void ShortcutEditorDialog::stopCapture()
{
    if (m_captureRow >= 0 && m_captureRow < m_table->rowCount()) {
        // Restore display from manager state
        QString id = rowId(m_captureRow);
        QKeySequence current = ShortcutManager::instance().shortcutForId(id);
        auto *cell = m_table->item(m_captureRow, 1);
        if (cell) {
            cell->setText(current.toString(QKeySequence::NativeText));
            cell->setBackground(QBrush());
        }
    }
    m_captureRow = -1;
    m_capturing = false;
    m_hintLabel->setText("Click a row, then press a key combination to reassign it.");
}

void ShortcutEditorDialog::keyPressEvent(QKeyEvent *event)
{
    if (!m_capturing) {
        QDialog::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        stopCapture();
        return;
    }

    // Build key sequence from event
    Qt::KeyboardModifiers mods = event->modifiers();
    int key = event->key();

    // Ignore bare modifier presses
    if (key == Qt::Key_Shift || key == Qt::Key_Control ||
        key == Qt::Key_Alt   || key == Qt::Key_Meta)
    {
        return;
    }

    QKeySequence newKey(QKeyCombination(mods, static_cast<Qt::Key>(key)));
    applyConflictCheck(rowId(m_captureRow), newKey, m_captureRow);
}

void ShortcutEditorDialog::applyConflictCheck(const QString &id,
                                               const QKeySequence &key,
                                               int row)
{
    // Check for conflict
    const QVector<ShortcutEntry> entries = ShortcutManager::instance().allShortcuts();
    for (const auto &entry : entries) {
        if (entry.id == id) continue;
        if (entry.currentKey == key && !key.isEmpty()) {
            int ret = QMessageBox::warning(
                this,
                "Shortcut Conflict",
                QString("'%1' is already used by '%2'.\nDo you want to reassign it?")
                    .arg(key.toString(QKeySequence::NativeText), entry.displayName),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No
            );
            if (ret != QMessageBox::Yes) {
                stopCapture();
                return;
            }
            // Clear conflicting shortcut
            ShortcutManager::instance().applyShortcut(entry.id, QKeySequence());
            // Update table cell if visible
            for (int r = 0; r < m_table->rowCount(); ++r) {
                if (rowId(r) == entry.id) {
                    m_table->item(r, 1)->setText(QString());
                    break;
                }
            }
            break;
        }
    }

    ShortcutManager::instance().applyShortcut(id, key);

    if (row >= 0 && row < m_table->rowCount() && m_table->item(row, 1)) {
        m_table->item(row, 1)->setText(key.toString(QKeySequence::NativeText));
        m_table->item(row, 1)->setBackground(QBrush());
    }

    m_captureRow = -1;
    m_capturing = false;
    m_hintLabel->setText("Click a row, then press a key combination to reassign it.");
}

QString ShortcutEditorDialog::rowId(int row) const
{
    if (row < 0 || row >= m_table->rowCount()) return QString();
    auto *item = m_table->item(row, 0);
    if (!item) return QString();
    return item->data(Qt::UserRole).toString();
}

void ShortcutEditorDialog::onSearchChanged(const QString &text)
{
    populateTable(text);
}

void ShortcutEditorDialog::onCellClicked(int row, int /*column*/)
{
    if (m_capturing && m_captureRow != row)
        stopCapture();
    startCapture(row);
}

void ShortcutEditorDialog::onResetRow()
{
    int row = m_table->currentRow();
    if (row < 0) return;

    QString id = rowId(row);
    if (id.isEmpty()) return;

    stopCapture();

    const QVector<ShortcutEntry> entries = ShortcutManager::instance().allShortcuts();
    for (const auto &entry : entries) {
        if (entry.id == id) {
            ShortcutManager::instance().applyShortcut(id, entry.defaultKey);
            m_table->item(row, 1)->setText(entry.defaultKey.toString(QKeySequence::NativeText));
            break;
        }
    }
}

void ShortcutEditorDialog::onResetAll()
{
    int ret = QMessageBox::question(
        this,
        "Reset All Shortcuts",
        "Reset all shortcuts to their defaults?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    if (ret != QMessageBox::Yes) return;

    ShortcutManager::instance().resetToDefaults();
    populateTable(m_searchEdit->text());
}

void ShortcutEditorDialog::onApply()
{
    stopCapture();
    ShortcutManager::instance().saveShortcuts();
}
