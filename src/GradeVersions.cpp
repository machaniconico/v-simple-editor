#include "GradeVersions.h"
#include "Timeline.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QUuid>
#include <QVBoxLayout>

namespace gradeversions {

QJsonObject gradeToJson(const ColorCorrection &cc)
{
    QJsonObject o;
    o[QStringLiteral("brightness")] = cc.brightness;
    o[QStringLiteral("contrast")] = cc.contrast;
    o[QStringLiteral("saturation")] = cc.saturation;
    o[QStringLiteral("hue")] = cc.hue;
    o[QStringLiteral("temperature")] = cc.temperature;
    o[QStringLiteral("tint")] = cc.tint;
    o[QStringLiteral("gamma")] = cc.gamma;
    o[QStringLiteral("highlights")] = cc.highlights;
    o[QStringLiteral("shadows")] = cc.shadows;
    o[QStringLiteral("exposure")] = cc.exposure;
    o[QStringLiteral("offsetR")] = cc.offsetR;
    o[QStringLiteral("offsetG")] = cc.offsetG;
    o[QStringLiteral("offsetB")] = cc.offsetB;
    o[QStringLiteral("liftR")] = cc.liftR;
    o[QStringLiteral("liftG")] = cc.liftG;
    o[QStringLiteral("liftB")] = cc.liftB;
    o[QStringLiteral("gammaR")] = cc.gammaR;
    o[QStringLiteral("gammaG")] = cc.gammaG;
    o[QStringLiteral("gammaB")] = cc.gammaB;
    o[QStringLiteral("gainR")] = cc.gainR;
    o[QStringLiteral("gainG")] = cc.gainG;
    o[QStringLiteral("gainB")] = cc.gainB;
    o[QStringLiteral("logShadowR")] = cc.logShadowR;
    o[QStringLiteral("logShadowG")] = cc.logShadowG;
    o[QStringLiteral("logShadowB")] = cc.logShadowB;
    o[QStringLiteral("logMidR")] = cc.logMidR;
    o[QStringLiteral("logMidG")] = cc.logMidG;
    o[QStringLiteral("logMidB")] = cc.logMidB;
    o[QStringLiteral("logHighR")] = cc.logHighR;
    o[QStringLiteral("logHighG")] = cc.logHighG;
    o[QStringLiteral("logHighB")] = cc.logHighB;
    return o;
}

ColorCorrection gradeFromJson(const QJsonObject &o)
{
    ColorCorrection cc;
    cc.brightness = o.value(QStringLiteral("brightness")).toDouble(0.0);
    cc.contrast = o.value(QStringLiteral("contrast")).toDouble(0.0);
    cc.saturation = o.value(QStringLiteral("saturation")).toDouble(0.0);
    cc.hue = o.value(QStringLiteral("hue")).toDouble(0.0);
    cc.temperature = o.value(QStringLiteral("temperature")).toDouble(0.0);
    cc.tint = o.value(QStringLiteral("tint")).toDouble(0.0);
    cc.gamma = o.value(QStringLiteral("gamma")).toDouble(1.0);
    cc.highlights = o.value(QStringLiteral("highlights")).toDouble(0.0);
    cc.shadows = o.value(QStringLiteral("shadows")).toDouble(0.0);
    cc.exposure = o.value(QStringLiteral("exposure")).toDouble(0.0);
    cc.offsetR = o.value(QStringLiteral("offsetR")).toDouble(0.0);
    cc.offsetG = o.value(QStringLiteral("offsetG")).toDouble(0.0);
    cc.offsetB = o.value(QStringLiteral("offsetB")).toDouble(0.0);
    cc.liftR = o.value(QStringLiteral("liftR")).toDouble(0.0);
    cc.liftG = o.value(QStringLiteral("liftG")).toDouble(0.0);
    cc.liftB = o.value(QStringLiteral("liftB")).toDouble(0.0);
    cc.gammaR = o.value(QStringLiteral("gammaR")).toDouble(0.0);
    cc.gammaG = o.value(QStringLiteral("gammaG")).toDouble(0.0);
    cc.gammaB = o.value(QStringLiteral("gammaB")).toDouble(0.0);
    cc.gainR = o.value(QStringLiteral("gainR")).toDouble(0.0);
    cc.gainG = o.value(QStringLiteral("gainG")).toDouble(0.0);
    cc.gainB = o.value(QStringLiteral("gainB")).toDouble(0.0);
    cc.logShadowR = o.value(QStringLiteral("logShadowR")).toDouble(0.0);
    cc.logShadowG = o.value(QStringLiteral("logShadowG")).toDouble(0.0);
    cc.logShadowB = o.value(QStringLiteral("logShadowB")).toDouble(0.0);
    cc.logMidR = o.value(QStringLiteral("logMidR")).toDouble(0.0);
    cc.logMidG = o.value(QStringLiteral("logMidG")).toDouble(0.0);
    cc.logMidB = o.value(QStringLiteral("logMidB")).toDouble(0.0);
    cc.logHighR = o.value(QStringLiteral("logHighR")).toDouble(0.0);
    cc.logHighG = o.value(QStringLiteral("logHighG")).toDouble(0.0);
    cc.logHighB = o.value(QStringLiteral("logHighB")).toDouble(0.0);
    return cc;
}

namespace {

void storeVersions(const QString &clipUid, const QVector<GradeVersion> &list)
{
    if (clipUid.isEmpty())
        return;
    QJsonArray arr;
    for (const auto &v : list) {
        QJsonObject o;
        o[QStringLiteral("id")] = v.id;
        o[QStringLiteral("name")] = v.name;
        o[QStringLiteral("grade")] = gradeToJson(v.grade);
        if (!v.lutPath.isEmpty())
            o[QStringLiteral("lutPath")] = v.lutPath;
        arr.append(o);
    }
    QSettings s;
    s.beginGroup(QStringLiteral("GradeVersions"));
    s.setValue(clipUid, QJsonDocument(arr).toJson(QJsonDocument::Compact));
    s.sync();
    s.endGroup();
}

} // namespace

QVector<GradeVersion> versionsForClip(const QString &clipUid)
{
    QVector<GradeVersion> out;
    if (clipUid.isEmpty())
        return out;
    QSettings s;
    s.beginGroup(QStringLiteral("GradeVersions"));
    const QByteArray raw = s.value(clipUid).toByteArray();
    s.endGroup();
    if (raw.isEmpty())
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray())
        return out;
    for (const QJsonValue &val : doc.array()) {
        const QJsonObject o = val.toObject();
        GradeVersion gv;
        gv.id = o.value(QStringLiteral("id")).toString();
        gv.name = o.value(QStringLiteral("name")).toString();
        gv.grade = gradeFromJson(o.value(QStringLiteral("grade")).toObject());
        gv.lutPath = o.value(QStringLiteral("lutPath")).toString();
        if (!gv.id.isEmpty())
            out.append(gv);
    }
    return out;
}

GradeVersion saveVersion(const QString &clipUid,
                         const QString &name,
                         const ColorCorrection &grade,
                         const QString &lutPath)
{
    QVector<GradeVersion> list = versionsForClip(clipUid);
    GradeVersion gv;
    gv.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    gv.name = name.trimmed().isEmpty()
        ? QStringLiteral("Version %1").arg(list.size() + 1)
        : name.trimmed();
    gv.grade = grade;
    gv.lutPath = lutPath;
    list.append(gv);
    storeVersions(clipUid, list);
    return gv;
}

bool deleteVersion(const QString &clipUid, const QString &versionId)
{
    QVector<GradeVersion> list = versionsForClip(clipUid);
    QVector<GradeVersion> kept;
    for (const auto &v : list) {
        if (v.id != versionId)
            kept.append(v);
    }
    if (kept.size() == list.size())
        return false;
    storeVersions(clipUid, kept);
    return true;
}

void clearVersionsForClip(const QString &clipUid)
{
    if (clipUid.isEmpty())
        return;
    QSettings s;
    s.beginGroup(QStringLiteral("GradeVersions"));
    s.remove(clipUid);
    s.sync();
    s.endGroup();
}

bool applyVersionToClip(Timeline *timeline,
                        int trackIdx,
                        int clipIdx,
                        const QString &versionId)
{
    if (!timeline)
        return false;
    const auto tracks = timeline->videoTracks();
    if (trackIdx < 0 || trackIdx >= tracks.size() || !tracks[trackIdx])
        return false;
    TimelineTrack *tr = tracks[trackIdx];
    if (tr->isLocked())
        return false;
    QVector<ClipInfo> clips = tr->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return false;
    const QString uid = clips[clipIdx].uid;
    const auto versions = versionsForClip(uid);
    for (const auto &v : versions) {
        if (v.id == versionId) {
            clips[clipIdx].colorCorrection = v.grade;
            clips[clipIdx].lutFilePath = v.lutPath;
            tr->setClips(clips);
            timeline->saveUndoState(QStringLiteral("カラーバージョン切替 (%1)").arg(v.name));
            return true;
        }
    }
    return false;
}

GradeVersionsDialog::GradeVersionsDialog(Timeline *timeline,
                                         int trackIdx,
                                         int clipIdx,
                                         QWidget *parent)
    : QDialog(parent)
    , m_timeline(timeline)
    , m_trackIdx(trackIdx)
    , m_clipIdx(clipIdx)
{
    setWindowTitle(QStringLiteral("カラーグレード・バージョン管理 (A/B/C 比較)"));
    resize(420, 320);

    if (m_timeline && m_trackIdx >= 0 && m_trackIdx < m_timeline->videoTracks().size()) {
        const auto clips = m_timeline->videoTracks()[m_trackIdx]->clips();
        if (m_clipIdx >= 0 && m_clipIdx < clips.size())
            m_clipUid = clips[m_clipIdx].uid;
    }

    auto *root = new QVBoxLayout(this);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("gradeVersionsList"));
    root->addWidget(m_list, 1);

    auto *row = new QHBoxLayout();
    auto *saveBtn = new QPushButton(QStringLiteral("現在のグレードを保存…"), this);
    auto *applyBtn = new QPushButton(QStringLiteral("選択バージョンを適用"), this);
    auto *delBtn = new QPushButton(QStringLiteral("削除"), this);
    auto *closeBtn = new QPushButton(QStringLiteral("閉じる"), this);
    row->addWidget(saveBtn);
    row->addWidget(applyBtn);
    row->addWidget(delBtn);
    row->addStretch();
    row->addWidget(closeBtn);
    root->addLayout(row);

    connect(saveBtn, &QPushButton::clicked, this, &GradeVersionsDialog::onSaveCurrent);
    connect(applyBtn, &QPushButton::clicked, this, &GradeVersionsDialog::onApplySelected);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &GradeVersionsDialog::onApplySelected);
    connect(delBtn, &QPushButton::clicked, this, &GradeVersionsDialog::onDeleteSelected);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    reloadList();
}

void GradeVersionsDialog::reloadList()
{
    m_list->clear();
    const auto list = versionsForClip(m_clipUid);
    for (const auto &v : list) {
        const QString summary = QStringLiteral("%1  (Exp %2, Sat %3, Temp %4)")
            .arg(v.name)
            .arg(v.grade.exposure, 0, 'f', 1)
            .arg(v.grade.saturation, 0, 'f', 0)
            .arg(v.grade.temperature, 0, 'f', 0);
        auto *item = new QListWidgetItem(summary, m_list);
        item->setData(Qt::UserRole, v.id);
    }
}

void GradeVersionsDialog::onSaveCurrent()
{
    if (!m_timeline || m_clipUid.isEmpty()) return;
    const auto clips = m_timeline->videoTracks()[m_trackIdx]->clips();
    if (m_clipIdx < 0 || m_clipIdx >= clips.size()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("グレードバージョン保存"),
        QStringLiteral("バージョン名 (例: Natural / Cinematic / Warm):"),
        QLineEdit::Normal,
        QStringLiteral("Version %1").arg(versionsForClip(m_clipUid).size() + 1), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    saveVersion(m_clipUid, name, clips[m_clipIdx].colorCorrection, clips[m_clipIdx].lutFilePath);
    reloadList();
}

void GradeVersionsDialog::onApplySelected()
{
    auto *item = m_list->currentItem();
    if (!item) return;
    applyVersionToClip(m_timeline, m_trackIdx, m_clipIdx, item->data(Qt::UserRole).toString());
}

void GradeVersionsDialog::onDeleteSelected()
{
    auto *item = m_list->currentItem();
    if (!item) return;
    deleteVersion(m_clipUid, item->data(Qt::UserRole).toString());
    reloadList();
}

} // namespace gradeversions
