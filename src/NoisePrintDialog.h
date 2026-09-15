#pragma once
#include <QDialog>
class QDoubleSpinBox;
class NoisePrintDialog : public QDialog {
    Q_OBJECT
public:
    explicit NoisePrintDialog(QWidget* parent = nullptr);
    double amountDb() const;
    double floorDb() const;
private:
    QDoubleSpinBox* m_amount;
    QDoubleSpinBox* m_floor;
};
