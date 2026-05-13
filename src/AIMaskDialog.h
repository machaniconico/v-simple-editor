#pragma once
#include <QDialog>
#include <QImage>
#include "AIMask.h"

class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QLineEdit;
class QLabel;

class AIMaskDialog : public QDialog {
    Q_OBJECT
public:
    explicit AIMaskDialog(QWidget* parent = nullptr);

    void              setSourceImage(const QImage& source);
    aimask::MaskParams params() const;
    QImage            maskImage() const;

private slots:
    void onGenerateClicked();

private:
    void updateWidgetStates();
    void showPreview(const QImage& img);

    QImage             m_source;
    QImage             m_mask;

    QComboBox*         m_engineCombo    = nullptr;
    QDoubleSpinBox*    m_lumaSpin       = nullptr;
    QPushButton*       m_colorButton    = nullptr;
    QDoubleSpinBox*    m_toleranceSpin  = nullptr;
    QLineEdit*         m_pluginIdEdit   = nullptr;
    QLabel*            m_preview        = nullptr;
    QColor             m_colorTarget    = Qt::green;
};
