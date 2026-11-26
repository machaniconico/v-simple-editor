#include "ClipRoles.h"
#include "AudioExportMix.h"
#include "ExternalGenerator.h"
#include "Timeline.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>

#include <cmath>

namespace cliproles {

QVector<RoleMeta> allRoles()
{
    return {
        { Role::None,     QStringLiteral("none"),     QStringLiteral("未分類"),           QColor(140, 148, 160), false },
        { Role::Dialogue, QStringLiteral("dialogue"), QStringLiteral("ダイアログ (音声)"), QColor(59, 130, 246),  true  },
        { Role::Music,    QStringLiteral("music"),    QStringLiteral("BGM / 音楽"),       QColor(34, 197, 94),   true  },
        { Role::SFX,      QStringLiteral("sfx"),      QStringLiteral("効果音 (SFX)"),     QColor(6, 182, 212),   true  },
        { Role::Ambience, QStringLiteral("ambience"), QStringLiteral("環境音"),           QColor(20, 184, 166),  true  },
        { Role::Video,    QStringLiteral("video"),    QStringLiteral("メイン映像"),       QColor(99, 102, 241),  false },
        { Role::Titles,   QStringLiteral("titles"),   QStringLiteral("テロップ / タイトル"), QColor(168, 85, 247), false },
        { Role::BRoll,    QStringLiteral("broll"),    QStringLiteral("B-Roll / インサート"), QColor(245, 158, 11), false },
    };
}

RoleMeta metaFor(Role role)
{
    for (const auto &m : allRoles()) {
        if (m.role == role)
            return m;
    }
    return allRoles().first();
}

Role roleFromId(const QString &id)
{
    const QString norm = id.trimmed().toLower();
    for (const auto &m : allRoles()) {
        if (m.id == norm)
            return m.role;
    }
    return Role::None;
}

Role roleForClip(const QString &clipUid)
{
    if (clipUid.isEmpty())
        return Role::None;
    QSettings s;
    s.beginGroup(QStringLiteral("ClipRoles"));
    const QString val = s.value(clipUid).toString();
    s.endGroup();
    return roleFromId(val);
}

void setRoleForClip(const QString &clipUid, Role role)
{
    if (clipUid.isEmpty())
        return;
    QSettings s;
    s.beginGroup(QStringLiteral("ClipRoles"));
    if (role == Role::None)
        s.remove(clipUid);
    else
        s.setValue(clipUid, metaFor(role).id);
    s.sync();
    s.endGroup();
}

void clearAllRoles()
{
    QSettings s;
    s.beginGroup(QStringLiteral("ClipRoles"));
    s.remove(QString());
    s.sync();
    s.endGroup();
}

QHash<QString, Role> allAssignedRoles()
{
    QHash<QString, Role> map;
    QSettings s;
    s.beginGroup(QStringLiteral("ClipRoles"));
    for (const QString &key : s.childKeys()) {
        const Role r = roleFromId(s.value(key).toString());
        if (r != Role::None)
            map.insert(key, r);
    }
    s.endGroup();
    return map;
}

Role effectiveRoleForClip(const ClipInfo &clip, bool isAudioTrack)
{
    const Role assigned = roleForClip(clip.uid);
    if (assigned != Role::None)
        return assigned;
    if (isAudioTrack)
        return Role::Dialogue;
    if (!clip.shapes.isEmpty() || extgen::isExternalGeneratorFilePath(clip.filePath))
        return Role::Titles;
    return Role::Video;
}

int countClipsWithRole(const Timeline *timeline, Role role)
{
    if (!timeline)
        return 0;
    int count = 0;
    for (const TimelineTrack *tr : timeline->videoTracks()) {
        if (!tr) continue;
        for (const ClipInfo &c : tr->clips()) {
            if (effectiveRoleForClip(c, false) == role)
                ++count;
        }
    }
    for (const TimelineTrack *tr : timeline->audioTracks()) {
        if (!tr) continue;
        for (const ClipInfo &c : tr->clips()) {
            if (effectiveRoleForClip(c, true) == role)
                ++count;
        }
    }
    return count;
}

MultiStemExportResult exportAudioStemsByRole(const Timeline *timeline,
                                             const QString &outputDir,
                                             const QString &baseName)
{
    MultiStemExportResult res;
    if (!timeline) {
        res.summary = QStringLiteral("タイムラインが無効です。");
        return res;
    }

    const QString ffmpeg = audioexport::findFfmpegBinary();
    if (ffmpeg.isEmpty()) {
        res.summary = QStringLiteral("ffmpeg が見つかりません。");
        return res;
    }

    struct AudioPlacement {
        ClipInfo clip;
        double tlStart = 0.0;
    };
    QHash<int, QVector<AudioPlacement>> clipsByRole;
    for (const TimelineTrack *tr : timeline->audioTracks()) {
        if (!tr || tr->isMuted())
            continue;
        double cursor = 0.0;
        for (const ClipInfo &c : tr->clips()) {
            cursor += qMax(0.0, c.leadInSec);
            const double dur = qMax(0.0, c.effectiveDuration());
            if (c.visible && !c.filePath.isEmpty() && QFileInfo::exists(c.filePath) && dur > 0.0) {
                const Role r = effectiveRoleForClip(c, true);
                clipsByRole[static_cast<int>(r)].append({c, cursor});
            }
            cursor += dur;
        }
    }

    if (clipsByRole.isEmpty()) {
        res.summary = QStringLiteral("書き出し対象の音声クリップがありません。");
        return res;
    }

    QDir().mkpath(outputDir);
    int successCount = 0;

    const QVector<Role> stemOrder = { Role::Dialogue, Role::Music, Role::SFX, Role::Ambience };
    for (Role r : stemOrder) {
        const auto placements = clipsByRole.value(static_cast<int>(r));
        if (placements.isEmpty())
            continue;

        const RoleMeta meta = metaFor(r);
        StemExportItem item;
        item.role = r;
        item.clipCount = placements.size();
        item.filePath = QDir(outputDir).filePath(
            QStringLiteral("%1_%2.wav").arg(baseName, meta.id));

        QStringList args;
        args << QStringLiteral("-y") << QStringLiteral("-hide_banner") << QStringLiteral("-loglevel") << QStringLiteral("error");
        QStringList chains;
        QStringList mixInputs;
        for (int i = 0; i < placements.size(); ++i) {
            const auto &p = placements[i];
            args << QStringLiteral("-i") << p.clip.filePath;
            const double inSec = qMax(0.0, p.clip.inPoint);
            const double outSec = (p.clip.outPoint > inSec) ? p.clip.outPoint : p.clip.duration;
            const int delayMs = qMax(0, qRound(p.tlStart * 1000.0));
            const double vol = qBound(0.0, p.clip.volume, 4.0);
            QString ch = QStringLiteral("[%1:a]atrim=start=%2:end=%3,asetpts=PTS-STARTPTS,aresample=48000,volume=%4")
                .arg(i)
                .arg(QString::number(inSec, 'f', 6))
                .arg(QString::number(outSec, 'f', 6))
                .arg(QString::number(vol, 'f', 6));
            if (delayMs > 0)
                ch += QStringLiteral(",adelay=%1:all=1").arg(delayMs);
            ch += QStringLiteral("[s%1]").arg(i);
            chains << ch;
            mixInputs << QStringLiteral("[s%1]").arg(i);
        }

        QString filterComplex = chains.join(QLatin1Char(';'));
        if (placements.size() == 1) {
            args << QStringLiteral("-filter_complex") << filterComplex
                 << QStringLiteral("-map") << QStringLiteral("[s0]");
        } else {
            filterComplex += QStringLiteral(";%1amix=inputs=%2:duration=longest:normalize=0[out]")
                .arg(mixInputs.join(QString())).arg(placements.size());
            args << QStringLiteral("-filter_complex") << filterComplex
                 << QStringLiteral("-map") << QStringLiteral("[out]");
        }
        args << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le")
             << QStringLiteral("-ar") << QStringLiteral("48000")
             << QStringLiteral("-ac") << QStringLiteral("2")
             << item.filePath;

        QString err;
        item.ok = audioexport::runFfmpegForAudioMix(args, &err) && QFileInfo(item.filePath).size() > 44;
        item.error = err;
        if (item.ok)
            ++successCount;
        res.stems.append(item);
    }

    res.ok = (successCount > 0 && successCount == res.stems.size());
    res.summary = QStringLiteral("ステム書き出し完了: %1 / %2 ロール (%3)")
        .arg(successCount).arg(res.stems.size()).arg(outputDir);
    return res;
}

} // namespace cliproles
