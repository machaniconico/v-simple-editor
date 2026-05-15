#include "ProjectTemplateDialog.h"
#include "ProjectTemplate.h"

#include <QByteArray>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ProjectTemplateDialog::ProjectTemplateDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("projectTemplateDialog"));
    setWindowTitle(tr("New Project from Template"));
    resize(720, 480);
    setModal(false);

    // -----------------------------------------------------------------------
    // Left: template list
    // -----------------------------------------------------------------------
    m_list = new QListWidget(this);
    m_list->setMinimumWidth(260);

    const QVector<projtmpl::TemplateMeta> templates =
        projtmpl::TemplateLibrary::allTemplates();
    for (const projtmpl::TemplateMeta &meta : templates) {
        auto *item = new QListWidgetItem(
            QStringLiteral("[%1] %2").arg(meta.category, meta.name));
        item->setData(Qt::UserRole, meta.id);
        m_list->addItem(item);
    }

    // -----------------------------------------------------------------------
    // Right: preview label
    // -----------------------------------------------------------------------
    m_preview = new QLabel(this);
    m_preview->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_preview->setWordWrap(true);
    m_preview->setText(tr("Select a template to see details."));
    m_preview->setMinimumWidth(340);

    // -----------------------------------------------------------------------
    // Bottom: create button
    // -----------------------------------------------------------------------
    m_createBtn = new QPushButton(tr("Create Project"), this);
    m_createBtn->setEnabled(false);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(m_createBtn);

    // -----------------------------------------------------------------------
    // Main layout
    // -----------------------------------------------------------------------
    auto *contentRow = new QHBoxLayout;
    contentRow->addWidget(m_list);
    contentRow->addWidget(m_preview, 1);

    auto *root = new QVBoxLayout(this);
    root->addLayout(contentRow, 1);
    root->addLayout(btnRow);

    // -----------------------------------------------------------------------
    // Connections
    // -----------------------------------------------------------------------
    connect(m_list, &QListWidget::itemSelectionChanged,
            this,   &ProjectTemplateDialog::onSelectionChanged);
    connect(m_createBtn, &QPushButton::clicked,
            this,        &ProjectTemplateDialog::onCreateClicked);
}

// ---------------------------------------------------------------------------
// onSelectionChanged
// ---------------------------------------------------------------------------
void ProjectTemplateDialog::onSelectionChanged()
{
    const QList<QListWidgetItem *> sel = m_list->selectedItems();
    if (sel.isEmpty()) {
        m_preview->setText(tr("Select a template to see details."));
        m_createBtn->setEnabled(false);
        return;
    }

    const QString id = sel.first()->data(Qt::UserRole).toString();

    // Find meta to populate preview
    const QVector<projtmpl::TemplateMeta> all =
        projtmpl::TemplateLibrary::allTemplates();
    for (const projtmpl::TemplateMeta &meta : all) {
        if (meta.id == id) {
            const QString text =
                QStringLiteral("<b>%1</b><br/>"
                               "Category: %2<br/>"
                               "Resolution: %3 x %4<br/>"
                               "Frame rate: %5 fps<br/><br/>"
                               "%6")
                    .arg(meta.name,
                         meta.category,
                         QString::number(meta.width),
                         QString::number(meta.height),
                         QString::number(meta.fps),
                         meta.description);
            m_preview->setText(text);
            m_createBtn->setEnabled(true);
            return;
        }
    }

    m_preview->setText(tr("Template not found."));
    m_createBtn->setEnabled(false);
}

// ---------------------------------------------------------------------------
// onCreateClicked
// ---------------------------------------------------------------------------
void ProjectTemplateDialog::onCreateClicked()
{
    const QList<QListWidgetItem *> sel = m_list->selectedItems();
    if (sel.isEmpty())
        return;

    const QString id = sel.first()->data(Qt::UserRole).toString();
    const QByteArray json =
        projtmpl::TemplateLibrary::createProjectFromTemplate(id);

    if (json.isEmpty()) {
        QMessageBox::warning(this,
                             tr("Template Error"),
                             tr("Could not create project from the selected template."));
        return;
    }

    emit projectCreated(json);
}
