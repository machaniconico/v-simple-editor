#include "MultiCamDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QShortcut>
#include <QSlider>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtMath>

#include <algorithm>

// ---------------------------------------------------------------------------
// MultiCamSwitchStrip — a thin horizontal widget that draws vertical tick
// lines per switch on top of a horizontal track. Lives below the playhead
// slider; click events forward into the dialog by re-emitting via the
// slider, so this widget stays read-only.
// ---------------------------------------------------------------------------

class MultiCamSwitchStrip : public QWidget
{
public:
    explicit MultiCamSwitchStrip(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(28);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setData(const QVector<MultiCamSwitch> &switches, qint64 durationUs)
    {
        m_switches = switches;
        m_durationUs = durationUs > 0 ? durationUs : 1;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        const QRect r = rect().adjusted(4, 4, -4, -4);
        // Track background.
        p.fillRect(r, QColor(48, 48, 52));
        p.setPen(QColor(80, 80, 88));
        p.drawRect(r);

        if (m_switches.isEmpty() || r.width() <= 0)
            return;

        // Distinct hue per angle id (simple HSV ring).
        for (const MultiCamSwitch &s : m_switches) {
            const double t =
                qBound(0.0,
                       static_cast<double>(s.timelineUs) /
                           static_cast<double>(m_durationUs),
                       1.0);
            const int x = r.left() + static_cast<int>(t * r.width());
            const int hue = (s.activeAngleId * 73) % 360;
            QColor c = QColor::fromHsv(hue, 200, 230);
            p.setPen(QPen(c, 2));
            p.drawLine(x, r.top() + 2, x, r.bottom() - 2);
        }
    }

private:
    QVector<MultiCamSwitch> m_switches;
    qint64 m_durationUs = 1;
};

// ---------------------------------------------------------------------------
// MultiCamDialog
// ---------------------------------------------------------------------------

MultiCamDialog::MultiCamDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Multi-camera angle editor"));
    setModal(true);
    resize(720, 540);

    buildUi();
    wireHotkeys();

    rebuildAngleList();
    rebuildThumbnailGrid();
    refreshSwitchStrip();
    refreshAngleButtons();
}

void MultiCamDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);

    // ---- Thumbnail grid (2x2) ------------------------------------------------
    auto *gridHost = new QWidget(this);
    m_thumbGrid = new QGridLayout(gridHost);
    m_thumbGrid->setHorizontalSpacing(6);
    m_thumbGrid->setVerticalSpacing(6);
    for (int i = 0; i < kMaxAngles; ++i) {
        auto *cell = new QWidget(gridHost);
        auto *cellLay = new QVBoxLayout(cell);
        cellLay->setContentsMargins(0, 0, 0, 0);
        cellLay->setSpacing(2);

        m_thumbLabels[i] = new QLabel(cell);
        m_thumbLabels[i]->setMinimumSize(180, 110);
        m_thumbLabels[i]->setAlignment(Qt::AlignCenter);
        m_thumbLabels[i]->setStyleSheet(
            QStringLiteral("background-color: #202024; color: #888; "
                           "border: 1px solid #3a3a40;"));
        m_thumbLabels[i]->setText(tr("(empty)"));
        cellLay->addWidget(m_thumbLabels[i], 1);

        m_thumbCaptions[i] = new QLabel(cell);
        m_thumbCaptions[i]->setAlignment(Qt::AlignCenter);
        cellLay->addWidget(m_thumbCaptions[i]);

        m_thumbGrid->addWidget(cell, i / 2, i % 2);
    }
    root->addWidget(gridHost, 1);

    // ---- Angle list + add/remove/sync ---------------------------------------
    auto *listRow = new QHBoxLayout();
    m_angleList = new QListWidget(this);
    m_angleList->setMaximumHeight(110);
    listRow->addWidget(m_angleList, 1);

    auto *btnCol = new QVBoxLayout();
    m_addBtn = new QPushButton(tr("Add Angle..."), this);
    m_removeBtn = new QPushButton(tr("Remove"), this);
    m_syncBtn = new QPushButton(tr("Sync (start time)"), this);
    btnCol->addWidget(m_addBtn);
    btnCol->addWidget(m_removeBtn);
    btnCol->addWidget(m_syncBtn);
    btnCol->addStretch(1);
    listRow->addLayout(btnCol);
    root->addLayout(listRow);

    connect(m_addBtn, &QPushButton::clicked,
            this, &MultiCamDialog::onAddAngle);
    connect(m_removeBtn, &QPushButton::clicked,
            this, &MultiCamDialog::onRemoveAngle);
    connect(m_syncBtn, &QPushButton::clicked,
            this, &MultiCamDialog::onSync);
    connect(m_angleList, &QListWidget::itemSelectionChanged,
            this, &MultiCamDialog::onAngleListSelected);

    // ---- Playhead slider + switch strip --------------------------------------
    m_playhead = new QSlider(Qt::Horizontal, this);
    m_playhead->setMinimum(0);
    m_playhead->setMaximum(static_cast<int>(m_timelineDurationUs / 1000));
    m_playhead->setValue(0);
    root->addWidget(m_playhead);
    connect(m_playhead, &QSlider::valueChanged,
            this, &MultiCamDialog::onPlayheadChanged);

    m_strip = new MultiCamSwitchStrip(this);
    root->addWidget(m_strip);

    // ---- Cut-to-angle buttons -----------------------------------------------
    auto *cutRow = new QHBoxLayout();
    cutRow->addWidget(new QLabel(tr("Cut to Angle:"), this));
    for (int i = 0; i < kMaxAngles; ++i) {
        m_cutBtns[i] = new QPushButton(QString::number(i + 1), this);
        m_cutBtns[i]->setFixedWidth(40);
        m_cutBtns[i]->setToolTip(tr("Cut to angle %1 (hotkey %1)").arg(i + 1));
        cutRow->addWidget(m_cutBtns[i]);
        // Capture-by-value is fine; i is bound at connect time.
        connect(m_cutBtns[i], &QPushButton::clicked,
                this, [this, i]() { onCutToAngle(i); });
    }
    cutRow->addStretch(1);
    root->addLayout(cutRow);

    // ---- Apply / Cancel ------------------------------------------------------
    auto *bb = new QDialogButtonBox(this);
    m_applyBtn = bb->addButton(QDialogButtonBox::Apply);
    m_cancelBtn = bb->addButton(QDialogButtonBox::Cancel);
    m_applyBtn->setText(tr("タイムラインに適用"));
    connect(m_applyBtn, &QPushButton::clicked, this, [this]() {
        emit applyToTimeline(m_project);
        accept();
    });
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    root->addWidget(bb);
}

void MultiCamDialog::wireHotkeys()
{
    for (int i = 0; i < kMaxAngles; ++i) {
        m_hotkeys[i] = new QShortcut(
            QKeySequence(Qt::Key_1 + i), this);
        m_hotkeys[i]->setContext(Qt::WindowShortcut);
        connect(m_hotkeys[i], &QShortcut::activated,
                this, [this, i]() { onCutToAngle(i); });
    }
}

void MultiCamDialog::setProject(const MultiCamProject &project)
{
    m_project = project;
    rebuildAngleList();
    rebuildThumbnailGrid();
    refreshSwitchStrip();
    refreshAngleButtons();
}

void MultiCamDialog::setTimelineDurationUs(qint64 durationUs)
{
    m_timelineDurationUs = durationUs > 0 ? durationUs : 1;
    if (m_playhead) {
        const int maxMs = static_cast<int>(m_timelineDurationUs / 1000);
        m_playhead->setMaximum(maxMs > 0 ? maxMs : 1);
    }
    refreshSwitchStrip();
}

// ----------------------------- handlers -----------------------------

void MultiCamDialog::onAddAngle()
{
    if (m_project.angles.size() >= kMaxAngles) {
        QMessageBox::information(
            this, tr("Multi-camera"),
            tr("Maximum %1 angles supported.").arg(kMaxAngles));
        return;
    }
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Add camera angle"),
        QString(),
        tr("Video files (*.mp4 *.mov *.mkv *.avi *.webm);;All files (*.*)"));
    if (file.isEmpty())
        return;

    MultiCamAngle a;
    a.id = allocateAngleId();
    a.sourcePath = file;
    a.syncOffsetUs = 0;
    a.label = QStringLiteral("Cam %1").arg(m_project.angles.size() + 1);
    m_project.angles.append(a);

    if (m_project.angles.size() == 1)
        m_project.defaultAngleId = a.id;

    rebuildAngleList();
    rebuildThumbnailGrid();
    refreshAngleButtons();
}

void MultiCamDialog::onRemoveAngle()
{
    const int row = selectedAngleRow();
    if (row < 0)
        return;

    const int removedId = m_project.angles[row].id;
    m_project.angles.remove(row);

    // Drop switches that targeted the removed angle.
    for (int i = m_project.switches.size() - 1; i >= 0; --i) {
        if (m_project.switches[i].activeAngleId == removedId)
            m_project.switches.remove(i);
    }
    if (m_project.defaultAngleId == removedId) {
        m_project.defaultAngleId =
            m_project.angles.isEmpty() ? 0 : m_project.angles.first().id;
    }

    rebuildAngleList();
    rebuildThumbnailGrid();
    refreshSwitchStrip();
    refreshAngleButtons();
}

void MultiCamDialog::onSync()
{
    // Simplified sync: every angle aligns to timeline time 0. (Audio
    // waveform / clap-board sync deferred to follow-up story — NIT.)
    for (MultiCamAngle &a : m_project.angles)
        a.syncOffsetUs = 0;
    QMessageBox::information(
        this, tr("Multi-camera"),
        tr("All angles aligned to timeline start (0 s)."));
}

void MultiCamDialog::onPlayheadChanged(int valueMs)
{
    m_playheadUs = static_cast<qint64>(valueMs) * 1000LL;
}

void MultiCamDialog::onCutToAngle(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= m_project.angles.size())
        return;  // angle not loaded yet — ignore silently

    const int targetId = m_project.angles[slotIndex].id;

    // De-dup: if there is already a switch within 1 ms of the playhead,
    // overwrite its angle instead of stacking another marker.
    bool replaced = false;
    for (MultiCamSwitch &s : m_project.switches) {
        if (qAbs(s.timelineUs - m_playheadUs) <= 1000) {
            s.activeAngleId = targetId;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        MultiCamSwitch s;
        s.timelineUs = m_playheadUs;
        s.activeAngleId = targetId;
        m_project.switches.append(s);
        std::sort(m_project.switches.begin(), m_project.switches.end(),
                  [](const MultiCamSwitch &a, const MultiCamSwitch &b) {
                      return a.timelineUs < b.timelineUs;
                  });
    }
    refreshSwitchStrip();
}

void MultiCamDialog::onAngleListSelected()
{
    refreshAngleButtons();
}

// ----------------------------- helpers -----------------------------

void MultiCamDialog::rebuildAngleList()
{
    if (!m_angleList) return;
    m_angleList->clear();
    for (int i = 0; i < m_project.angles.size(); ++i) {
        const MultiCamAngle &a = m_project.angles[i];
        const QString fileName = QFileInfo(a.sourcePath).fileName();
        m_angleList->addItem(
            QStringLiteral("[%1] %2  —  %3")
                .arg(i + 1).arg(a.label, fileName));
    }
}

void MultiCamDialog::rebuildThumbnailGrid()
{
    for (int i = 0; i < kMaxAngles; ++i) {
        if (!m_thumbLabels[i] || !m_thumbCaptions[i])
            continue;
        if (i < m_project.angles.size()) {
            m_thumbCaptions[i]->setText(m_project.angles[i].label);
            // Best-effort: kick off a thumbnail probe. Failure → keep
            // the placeholder label text.
            loadThumbnailFor(i);
        } else {
            m_thumbCaptions[i]->setText(QString());
            m_thumbLabels[i]->setPixmap(QPixmap());
            m_thumbLabels[i]->setText(tr("(empty)"));
        }
    }
}

void MultiCamDialog::refreshSwitchStrip()
{
    if (m_strip)
        m_strip->setData(m_project.switches, m_timelineDurationUs);
}

void MultiCamDialog::refreshAngleButtons()
{
    for (int i = 0; i < kMaxAngles; ++i) {
        if (m_cutBtns[i])
            m_cutBtns[i]->setEnabled(i < m_project.angles.size());
    }
    if (m_removeBtn)
        m_removeBtn->setEnabled(selectedAngleRow() >= 0);
}

int MultiCamDialog::allocateAngleId() const
{
    int maxId = 0;
    for (const MultiCamAngle &a : m_project.angles)
        maxId = std::max(maxId, a.id);
    return maxId + 1;
}

int MultiCamDialog::selectedAngleRow() const
{
    if (!m_angleList) return -1;
    return m_angleList->currentRow();
}

void MultiCamDialog::loadThumbnailFor(int slotIndex)
{
    // Lightweight probe via QMediaPlayer + QVideoSink. The first frame
    // received is grabbed into the slot label and the player is torn
    // down. Failures (codec missing, bad file) silently fall through —
    // the placeholder text stays.
    if (slotIndex < 0 || slotIndex >= m_project.angles.size())
        return;
    if (!m_thumbLabels[slotIndex])
        return;

    const QString path = m_project.angles[slotIndex].sourcePath;
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;

    auto *sink   = new QVideoSink(this);
    auto *player = new QMediaPlayer(this);
    player->setVideoSink(sink);
    player->setSource(QUrl::fromLocalFile(path));
    // Mute via audio output absence (we never set one).
    player->setPosition(0);

    QLabel *label = m_thumbLabels[slotIndex];
    QObject::connect(sink, &QVideoSink::videoFrameChanged,
                     this, [label, sink, player](const QVideoFrame &frame) {
        if (!frame.isValid())
            return;
        QImage img = frame.toImage();
        if (img.isNull())
            return;
        const QPixmap pm = QPixmap::fromImage(img).scaled(
            label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        label->setPixmap(pm);
        label->setText(QString());
        player->stop();
        player->deleteLater();
        sink->deleteLater();
    });
    player->play();
}
