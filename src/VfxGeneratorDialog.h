#pragma once

#include "VfxGenerators.h"

#include <QColor>
#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QVector>

class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;

class VfxGeneratorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VfxGeneratorDialog(QWidget *parent = nullptr);

    VfxGeneratorType generatorType() const;
    VfxGeneratorParameters parameters() const;
    void setGeneratorType(VfxGeneratorType type);
    void setParameters(VfxGeneratorType type,
                       const VfxGeneratorParameters &parameters);

private slots:
    void onGeneratorChanged(int index);
    void updatePreview();
    void restartPreview();
    void chooseColor();

private:
    struct PageControls {
        QWidget *page = nullptr;
        QFormLayout *form = nullptr;
        QHash<QString, QDoubleSpinBox *> doubles;
        QHash<QString, QSpinBox *> ints;
        QPushButton *colorButton = nullptr;
        QColor color = Qt::white;
    };

    void buildUi();
    void buildExplosionPage();
    void buildLightningPage();
    void buildShockWavePage();
    void buildEnergyBeamPage();
    void buildMagicCirclePage();
    void buildMuzzleFlashPage();
    void buildEnergyShieldPage();

    QDoubleSpinBox *addDouble(PageControls &page, const QString &name,
                              const QString &label, double minValue,
                              double maxValue, double value,
                              double step = 0.01);
    QSpinBox *addInt(PageControls &page, const QString &name,
                     const QString &label, int minValue, int maxValue,
                     int value);
    void addColor(PageControls &page, const QColor &color);
    PageControls &pageFor(VfxGeneratorType type);
    const PageControls &pageFor(VfxGeneratorType type) const;
    void setPageValue(PageControls &page, const QString &name, double value);
    void setPageValue(PageControls &page, const QString &name, int value);
    double doubleValue(const PageControls &page, const QString &name,
                       double fallback) const;
    int intValue(const PageControls &page, const QString &name,
                 int fallback) const;
    void updateColorButton(PageControls &page);

    QComboBox *m_typeCombo = nullptr;
    QStackedWidget *m_pagesStack = nullptr;
    QLabel *m_previewLabel = nullptr;
    QPushButton *m_restartButton = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QTimer *m_previewTimer = nullptr;
    QElapsedTimer m_elapsed;
    QVector<PageControls> m_pages;
};

