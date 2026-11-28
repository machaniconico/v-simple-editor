#pragma once

#include <QColor>
#include <QDialog>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QComboBox;
class QListWidget;
class QPushButton;
class QSettings;
class Timeline;

namespace ctxmenu {

enum class Target {
    PreviewText = 0,
    PreviewVideoClip = 1,
    PreviewBackground = 2,
    TimelineAudioClip = 3
};

struct ActionDef {
    QString id;
    QString label;
    bool defaultVisible = true;
    bool separatorAfterDefault = false;
};

struct ResolvedItem {
    QString id;
    QString label;
    bool visible = true;
    bool separatorAfter = false;
};

struct SavedLayout {
    QStringList order;
    QSet<QString> hidden;
};

struct PreviewHit {
    Target target = Target::PreviewBackground;
    int overlayIndex = -1;
    int videoTrackIndex = -1;
    int videoClipIndex = -1;
};

QString targetKey(Target target);
QString targetDisplayName(Target target);
QVector<Target> allTargets();
QVector<ActionDef> defaultActionsFor(Target target);

QVector<ResolvedItem> resolveOrder(const QVector<ActionDef> &defaults,
                                   const QStringList &savedOrder,
                                   const QSet<QString> &savedHidden);

QVector<ResolvedItem> loadLayout(Target target, QSettings *settings = nullptr);
void saveLayout(Target target,
                const QStringList &order,
                const QSet<QString> &hidden,
                QSettings *settings = nullptr);
void resetLayout(Target target, QSettings *settings = nullptr);

// Pure hit-testing on the preview canvas in widget coordinates.
// Priority: active text overlay (topmost first) > active video clip (PiP/transformed first, then V1, then V2+) > background.
PreviewHit hitTestPreview(const Timeline *timeline,
                          double playheadSec,
                          const QRectF &letterboxRect,
                          const QPointF &widgetPos);

// Text overlay & SFX editing helpers built on Timeline's public API (with undo).
bool removeTextOverlay(Timeline *timeline, int overlayIndex);
int duplicateTextOverlay(Timeline *timeline, int overlayIndex);
bool updateTextOverlayColor(Timeline *timeline, int overlayIndex, const QColor &color);
bool updateTextOverlayFontSize(Timeline *timeline, int overlayIndex, int pointSize);
bool centerTextOverlay(Timeline *timeline, int overlayIndex);
bool insertAudioClipAtTime(Timeline *timeline,
                           const QString &audioPath,
                           double startSec,
                           int trackIdxOneBased = 2);

class ContextMenuCustomizeDialog : public QDialog {
    Q_OBJECT
public:
    explicit ContextMenuCustomizeDialog(Target initialTarget,
                                        QWidget *parent = nullptr,
                                        QSettings *settings = nullptr);

    Target currentTarget() const;
    QStringList currentOrderIds() const;
    QSet<QString> currentHiddenIds() const;
    void moveSelectedRow(int delta);
    void resetCurrentTargetToDefault();
    void commitDrafts();

private:
    void loadTargetIntoList(Target target);
    void captureCurrentListToDraft();
    void updateMoveButtons();

    QSettings *m_settings = nullptr; // non-owning; nullptr => QSettings("VSimpleEditor", "Preferences")
    QComboBox *m_targetCombo = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_upBtn = nullptr;
    QPushButton *m_downBtn = nullptr;
    QPushButton *m_resetBtn = nullptr;

    Target m_activeTarget = Target::PreviewText;
    QHash<int, SavedLayout> m_drafts;
};

} // namespace ctxmenu
