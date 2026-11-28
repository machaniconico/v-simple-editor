#include "TimelineIndexDock.h"
#include "ClipGeometry.h"
#include "ClipRoles.h"
#include "DuplicateFrameDetector.h"
#include "ExternalGenerator.h"
#include "Timeline.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace tlindex {

namespace {

bool isClipOffline(const ClipInfo &c)
{
    if (c.filePath.isEmpty() || c.isSequenceReference() || c.isAdjustment
        || !c.shapes.isEmpty() || clipgeom::isNullObjectFilePath(c.filePath)
        || extgen::isExternalGeneratorFilePath(c.filePath)) {
        return false;
    }
    return !QFileInfo::exists(c.filePath);
}

QString formatTimeSec(double sec)
{
    const int totalMs = qMax(0, qRound(sec * 1000.0));
    const int mins = totalMs / 60000;
    const int secs = (totalMs % 60000) / 1000;
    const int ms = totalMs % 1000;
    return QStringLiteral("%1:%2.%3")
        .arg(mins, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

} // namespace

QVector<IndexRow> buildIndexRows(const Timeline *timeline, const FilterOptions &filter)
{
    QVector<IndexRow> out;
    if (!timeline)
        return out;

    const dupframes::AnalysisResult dupRes = dupframes::analyzeTimeline(timeline, true, true);
    QSet<QString> dupKeys;
    for (const auto &ov : dupRes.overlaps) {
        dupKeys.insert(QStringLiteral("%1:%2:%3")
            .arg(ov.first.isAudio ? 1 : 0).arg(ov.first.trackIdx).arg(ov.first.clipIdx));
        dupKeys.insert(QStringLiteral("%1:%2:%3")
            .arg(ov.second.isAudio ? 1 : 0).arg(ov.second.trackIdx).arg(ov.second.clipIdx));
    }

    const QString q = filter.query.trimmed().toLower();
    const bool filterRole = !filter.roleFilter.isEmpty() && filter.roleFilter != QStringLiteral("all");

    auto acceptRow = [&](const IndexRow &r, const QString &roleId) {
        if (filter.kindFilter == 1 && r.kind != IndexRow::Kind::VideoClip) return false;
        if (filter.kindFilter == 2 && r.kind != IndexRow::Kind::AudioClip) return false;
        if (filter.kindFilter == 3 && r.kind != IndexRow::Kind::Marker) return false;
        if (filterRole && roleId != filter.roleFilter) return false;
        if (filter.onlyOffline && !r.offline) return false;
        if (filter.onlyDuplicates && !r.duplicated) return false;
        if (!q.isEmpty()) {
            const bool match = r.name.toLower().contains(q)
                || r.trackLabel.toLower().contains(q)
                || r.roleName.toLower().contains(q);
            if (!match) return false;
        }
        return true;
    };

    auto collectClips = [&](const QVector<TimelineTrack *> &tracks, bool isAudio) {
        for (int t = 0; t < tracks.size(); ++t) {
            const TimelineTrack *tr = tracks[t];
            if (!tr) continue;
            const QString tLabel = tr->customName.isEmpty()
                ? QStringLiteral("%1%2").arg(isAudio ? QStringLiteral("A") : QStringLiteral("V")).arg(t + 1)
                : tr->customName;
            double cursor = 0.0;
            const auto clips = tr->clips();
            for (int c = 0; c < clips.size(); ++c) {
                const ClipInfo &ci = clips[c];
                cursor += qMax(0.0, ci.leadInSec);
                const double dur = qMax(0.0, ci.effectiveDuration());
                const cliproles::Role role = cliproles::effectiveRoleForClip(ci, isAudio);
                const cliproles::RoleMeta rmeta = cliproles::metaFor(role);

                IndexRow row;
                row.kind = isAudio ? IndexRow::Kind::AudioClip : IndexRow::Kind::VideoClip;
                row.trackIdx = t;
                row.clipIdx = c;
                row.uid = ci.uid;
                row.name = ci.displayName.isEmpty() ? QFileInfo(ci.filePath).fileName() : ci.displayName;
                row.trackLabel = tLabel;
                row.roleName = rmeta.displayName;
                row.startSec = cursor;
                row.durationSec = dur;
                row.offline = isClipOffline(ci);
                row.duplicated = dupKeys.contains(
                    QStringLiteral("%1:%2:%3").arg(isAudio ? 1 : 0).arg(t).arg(c));
                if (acceptRow(row, rmeta.id))
                    out.append(row);
                cursor += dur;
            }
        }
    };

    collectClips(timeline->videoTracks(), false);
    collectClips(timeline->audioTracks(), true);

    for (const Marker &m : timeline->markers()) {
        IndexRow row;
        row.kind = IndexRow::Kind::Marker;
        row.name = m.label.isEmpty() ? QStringLiteral("(無題マーカー)") : m.label;
        row.trackLabel = QStringLiteral("マーカー");
        row.roleName = QStringLiteral("マーカー");
        row.startSec = static_cast<double>(m.timelineUs) / 1e6;
        row.durationSec = static_cast<double>(m.durationUs) / 1e6;
        if (acceptRow(row, QStringLiteral("marker")))
            out.append(row);
    }

    std::stable_sort(out.begin(), out.end(), [](const IndexRow &a, const IndexRow &b) {
        return a.startSec < b.startSec;
    });
    return out;
}

TimelineIndexDialog::TimelineIndexDialog(Timeline *timeline, QWidget *parent)
    : QDialog(parent)
    , m_timeline(timeline)
{
    setWindowTitle(QStringLiteral("タイムライン・インデックス"));
    resize(780, 480);

    auto *root = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout();

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(QStringLiteral("tlIndexSearchEdit"));
    m_searchEdit->setPlaceholderText(QStringLiteral("クリップ名・トラック・ロール・マーカーで検索…"));

    m_kindCombo = new QComboBox(this);
    m_kindCombo->setObjectName(QStringLiteral("tlIndexKindCombo"));
    m_kindCombo->addItems({QStringLiteral("すべて"), QStringLiteral("動画"),
                           QStringLiteral("音声"), QStringLiteral("マーカー")});

    m_roleCombo = new QComboBox(this);
    m_roleCombo->setObjectName(QStringLiteral("tlIndexRoleCombo"));
    m_roleCombo->addItem(QStringLiteral("全ロール"), QStringLiteral("all"));
    for (const auto &rm : cliproles::allRoles())
        m_roleCombo->addItem(rm.displayName, rm.id);

    m_offlineOnly = new QCheckBox(QStringLiteral("オフラインのみ"), this);
    m_offlineOnly->setObjectName(QStringLiteral("tlIndexOfflineCheck"));
    m_dupOnly = new QCheckBox(QStringLiteral("重複フレームのみ"), this);
    m_dupOnly->setObjectName(QStringLiteral("tlIndexDupCheck"));

    bar->addWidget(m_searchEdit, 1);
    bar->addWidget(m_kindCombo);
    bar->addWidget(m_roleCombo);
    bar->addWidget(m_offlineOnly);
    bar->addWidget(m_dupOnly);
    root->addLayout(bar);

    m_table = new QTableWidget(0, 6, this);
    m_table->setObjectName(QStringLiteral("tlIndexTable"));
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("開始"), QStringLiteral("長さ"), QStringLiteral("トラック"),
        QStringLiteral("名前"), QStringLiteral("ロール"), QStringLiteral("状態")
    });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(m_table, 1);

    auto *bottom = new QHBoxLayout();
    auto *refreshBtn = new QPushButton(QStringLiteral("再読み込み"), this);
    auto *jumpBtn = new QPushButton(QStringLiteral("選択位置へ移動"), this);
    auto *closeBtn = new QPushButton(QStringLiteral("閉じる"), this);
    bottom->addWidget(refreshBtn);
    bottom->addStretch();
    bottom->addWidget(jumpBtn);
    bottom->addWidget(closeBtn);
    root->addLayout(bottom);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &TimelineIndexDialog::refresh);
    connect(m_kindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TimelineIndexDialog::refresh);
    connect(m_roleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TimelineIndexDialog::refresh);
    connect(m_offlineOnly, &QCheckBox::toggled, this, &TimelineIndexDialog::refresh);
    connect(m_dupOnly, &QCheckBox::toggled, this, &TimelineIndexDialog::refresh);
    connect(refreshBtn, &QPushButton::clicked, this, &TimelineIndexDialog::refresh);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &TimelineIndexDialog::onRowActivated);
    connect(jumpBtn, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r >= 0) onRowActivated(r, 0);
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    refresh();
}

void TimelineIndexDialog::refresh()
{
    FilterOptions opt;
    opt.query = m_searchEdit->text();
    opt.kindFilter = m_kindCombo->currentIndex();
    opt.roleFilter = m_roleCombo->currentData().toString();
    opt.onlyOffline = m_offlineOnly->isChecked();
    opt.onlyDuplicates = m_dupOnly->isChecked();

    m_rows = buildIndexRows(m_timeline, opt);
    m_table->setRowCount(m_rows.size());
    for (int i = 0; i < m_rows.size(); ++i) {
        const auto &r = m_rows[i];
        QStringList flags;
        if (r.offline) flags << QStringLiteral("オフライン");
        if (r.duplicated) flags << QStringLiteral("重複フレーム");
        m_table->setItem(i, 0, new QTableWidgetItem(formatTimeSec(r.startSec)));
        m_table->setItem(i, 1, new QTableWidgetItem(formatTimeSec(r.durationSec)));
        m_table->setItem(i, 2, new QTableWidgetItem(r.trackLabel));
        m_table->setItem(i, 3, new QTableWidgetItem(r.name));
        m_table->setItem(i, 4, new QTableWidgetItem(r.roleName));
        m_table->setItem(i, 5, new QTableWidgetItem(flags.isEmpty() ? QStringLiteral("OK") : flags.join(QStringLiteral(", "))));
    }
}

int TimelineIndexDialog::rowCount() const
{
    return m_table ? m_table->rowCount() : 0;
}

void TimelineIndexDialog::onRowActivated(int row, int)
{
    if (row < 0 || row >= m_rows.size())
        return;
    emit jumpToTimeRequested(m_rows[row].startSec);
}

} // namespace tlindex
