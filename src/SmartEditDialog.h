#pragma once

#include <QDialog>
#include <QVector>

#include "SmartEditAssistant.h"

class QLineEdit;
class QPushButton;
class QProgressBar;
class QTableWidget;

class SmartEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SmartEditDialog(QWidget *parent = nullptr);

private slots:
    void onBrowseClicked();
    void onAnalyzeClicked();
    void onAnalysisProgress(int percent);
    void onAnalysisFinished(const QVector<smartedit::CutSuggestion> &suggestions);
    void onAnalysisFailed(const QString &error);
    void onApplyToTimelineClicked();

private:
    static QString formatTime(qint64 ms);
    static QString reasonToString(smartedit::CutSuggestion::Reason reason);
    void repopulateTable();

    smartedit::Assistant *m_assistant = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_analyzeButton = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_applyButton = nullptr;
    QVector<smartedit::CutSuggestion> m_suggestions;
};
