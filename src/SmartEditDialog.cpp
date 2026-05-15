#include "SmartEditDialog.h"

#include <QAbstractItemView>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <QDebug>

QString SmartEditDialog::formatTime(qint64 ms)
{
    const qint64 totalSeconds = ms / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    const qint64 millis = ms % 1000;

    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(millis, 3, 10, QChar('0'));
}

QString SmartEditDialog::reasonToString(smartedit::CutSuggestion::Reason reason)
{
    switch (reason) {
    case smartedit::CutSuggestion::Silence:
        return QObject::tr("Silence");
    case smartedit::CutSuggestion::SceneChange:
        return QObject::tr("SceneChange");
    case smartedit::CutSuggestion::Combined:
        return QObject::tr("Combined");
    }

    return QObject::tr("Unknown");
}

SmartEditDialog::SmartEditDialog(QWidget *parent)
    : QDialog(parent)
    , m_assistant(new smartedit::Assistant(this))
{
    setWindowTitle(tr("Smart Edit Assistant"));
    setModal(false);
    setWindowModality(Qt::NonModal);
    resize(760, 460);

    auto *mainLayout = new QVBoxLayout(this);

    auto *pathLayout = new QHBoxLayout();
    pathLayout->addWidget(new QLabel(tr("Video:"), this));

    m_pathEdit = new QLineEdit(this);
    pathLayout->addWidget(m_pathEdit, 1);

    m_browseButton = new QPushButton(tr("Browse..."), this);
    pathLayout->addWidget(m_browseButton);

    m_analyzeButton = new QPushButton(tr("Analyze"), this);
    pathLayout->addWidget(m_analyzeButton);

    mainLayout->addLayout(pathLayout);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    mainLayout->addWidget(m_progressBar);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels(
        { tr("Start"), tr("End"), tr("Reason"), tr("Confidence") });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    mainLayout->addWidget(m_table, 1);

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    m_applyButton = new QPushButton(tr("Apply to Timeline"), this);
    buttonLayout->addWidget(m_applyButton);

    auto *closeButton = new QPushButton(tr("Close"), this);
    buttonLayout->addWidget(closeButton);

    mainLayout->addLayout(buttonLayout);

    connect(m_browseButton, &QPushButton::clicked,
            this, &SmartEditDialog::onBrowseClicked);
    connect(m_analyzeButton, &QPushButton::clicked,
            this, &SmartEditDialog::onAnalyzeClicked);
    connect(m_applyButton, &QPushButton::clicked,
            this, &SmartEditDialog::onApplyToTimelineClicked);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    connect(m_assistant, &smartedit::Assistant::analysisProgress,
            this, &SmartEditDialog::onAnalysisProgress);
    connect(m_assistant, &smartedit::Assistant::analysisFinished,
            this, &SmartEditDialog::onAnalysisFinished);
    connect(m_assistant, &smartedit::Assistant::analysisFailed,
            this, &SmartEditDialog::onAnalysisFailed);
}

void SmartEditDialog::onBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select video file"),
        m_pathEdit->text(),
        tr("Video Files (*.mp4 *.mov *.mkv *.avi *.mxf);;All Files (*)"));

    if (!path.isEmpty())
        m_pathEdit->setText(path);
}

void SmartEditDialog::onAnalyzeClicked()
{
    const QString path = m_pathEdit->text().trimmed();
    if (path.isEmpty()) {
        qWarning() << "SmartEditDialog: analyze requested with empty video path";
        return;
    }

    m_suggestions.clear();
    repopulateTable();
    m_progressBar->setValue(0);
    m_analyzeButton->setEnabled(false);

    m_assistant->analyze(path);
}

void SmartEditDialog::onAnalysisProgress(int percent)
{
    m_progressBar->setValue(percent);
}

void SmartEditDialog::onAnalysisFinished(const QVector<smartedit::CutSuggestion> &suggestions)
{
    m_suggestions = suggestions;
    repopulateTable();
    m_progressBar->setValue(100);
    m_analyzeButton->setEnabled(true);
}

void SmartEditDialog::onAnalysisFailed(const QString &error)
{
    qWarning() << "SmartEditDialog:" << error;
    m_progressBar->setValue(0);
    m_analyzeButton->setEnabled(true);
}

void SmartEditDialog::onApplyToTimelineClicked()
{
    qInfo() << "SmartEditDialog: Apply to Timeline requested for"
            << m_suggestions.size() << "suggestions";
}

void SmartEditDialog::repopulateTable()
{
    m_table->setRowCount(m_suggestions.size());

    for (int row = 0; row < m_suggestions.size(); ++row) {
        const smartedit::CutSuggestion &suggestion = m_suggestions[row];

        auto *startItem = new QTableWidgetItem(formatTime(suggestion.startMs));
        auto *endItem = new QTableWidgetItem(formatTime(suggestion.endMs));
        auto *reasonItem = new QTableWidgetItem(reasonToString(suggestion.reason));
        auto *confidenceItem =
            new QTableWidgetItem(QString::number(suggestion.confidence, 'f', 2));

        m_table->setItem(row, 0, startItem);
        m_table->setItem(row, 1, endItem);
        m_table->setItem(row, 2, reasonItem);
        m_table->setItem(row, 3, confidenceItem);
    }
}
