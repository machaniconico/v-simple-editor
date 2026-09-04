#pragma once

#include <QDialog>

class QDoubleSpinBox;

class DialogueLevelerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DialogueLevelerDialog(QWidget *parent = nullptr);

    double targetLufs() const;
    double smoothingSec() const;

private:
    QDoubleSpinBox *m_targetSpin = nullptr;
    QDoubleSpinBox *m_smoothingSpin = nullptr;
};
