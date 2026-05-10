#pragma once

// TitlePresetDialog
// -----------------
// Premiere Pro Essential Graphics / Resolve Fusion Titles parity. The user
// picks a pre-built animated title from the list on the left, edits the text
// + colour, and clicks "適用". The resolved EnhancedTextOverlay (font /
// colour / animations / position keyframes baked) is then available via
// resolvedOverlay() for the caller to insert into the active TextManager.
//
// The dialog is intentionally self-contained: it never modifies
// TextManager.h. All preset definitions live in this translation unit and
// are converted into existing EnhancedTextOverlay fields at apply time.

#include <QDialog>
#include <QVector>

#include "TextManager.h"
#include "TitlePresetData.h"

class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QPushButton;
class QLabel;
class QTimer;

class TitlePresetPreviewWidget; // forward — defined in the .cpp

class TitlePresetDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TitlePresetDialog(QWidget *parent = nullptr);
    ~TitlePresetDialog() override = default;

    // Returns the overlay constructed when the user clicked "適用".
    // Valid only after exec() returns QDialog::Accepted.
    const EnhancedTextOverlay &resolvedOverlay() const { return m_resolved; }

    // Helper used internally and exposed for tests / future MainWindow
    // wiring. Bakes the preset's animation / colour / font into out using
    // only public TextManager fields (animIn, animOut, positionKeyframes,
    // shadow, etc.). out.text is set to the user-entered text and out.color
    // to the user-picked colour.
    static void applyPresetTo(const TitlePreset &preset,
                              const QString &userText,
                              const QColor &userColour,
                              EnhancedTextOverlay &out);

    // Built-in preset catalogue (7 presets — at least 6 required by spec).
    static QVector<TitlePreset> builtInPresets();

private slots:
    void onPresetChanged(int row);
    void onPickColour();
    void onAccept();

private:
    void buildUi();
    void populateList();

    QListWidget               *m_list           = nullptr;
    QLineEdit                 *m_textEdit       = nullptr;
    QPushButton               *m_colourButton   = nullptr;
    QLabel                    *m_colourSwatch   = nullptr;
    TitlePresetPreviewWidget  *m_preview        = nullptr;

    QVector<TitlePreset>       m_presets;
    int                        m_currentRow     = 0;
    QColor                     m_currentColour  = Qt::white;
    EnhancedTextOverlay        m_resolved;
};
