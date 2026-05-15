#pragma once

#include <QDialog>
#include <QImage>

class QLabel;
class QComboBox;
class QPushButton;

// Modeless dialog for HDR -> SDR tone-map preview (Sprint 21, US-HDR-1).
class HdrGradingDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HdrGradingDialog(QWidget *parent = nullptr);

private slots:
    void onBrowseClicked();
    void onApplyClicked();

private:
    QLabel    *m_beforeView = nullptr;
    QLabel    *m_afterView  = nullptr;
    QComboBox *m_tfCombo    = nullptr;
    QComboBox *m_opCombo    = nullptr;
    QImage     m_source;
};
