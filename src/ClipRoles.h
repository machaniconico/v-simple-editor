#pragma once

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

class Timeline;
struct ClipInfo;

namespace cliproles {

// Final Cut Pro Roles + Adobe Premiere Pro Essential Sound categories.
enum class Role {
    None = 0,
    Dialogue,
    Music,
    SFX,
    Ambience,
    Video,
    Titles,
    BRoll
};

struct RoleMeta {
    Role role = Role::None;
    QString id;
    QString displayName;
    QColor color;
    bool isAudioRole = false;
};

struct StemExportItem {
    Role role = Role::None;
    QString filePath;
    int clipCount = 0;
    bool ok = false;
    QString error;
};

struct MultiStemExportResult {
    bool ok = false;
    QVector<StemExportItem> stems;
    QString summary;
};

QVector<RoleMeta> allRoles();
RoleMeta metaFor(Role role);
Role roleFromId(const QString &id);

// Per-clip UID role registry persisted in QSettings (zero Timeline.h schema edits).
Role roleForClip(const QString &clipUid);
void setRoleForClip(const QString &clipUid, Role role);
void clearAllRoles();
QHash<QString, Role> allAssignedRoles();

// Infer a sensible default role when unassigned (e.g. audio clip -> Dialogue, text/gen -> Titles).
Role effectiveRoleForClip(const ClipInfo &clip, bool isAudioTrack);

// Solo / unsolo a specific audio role across Timeline tracks/clips (sets clip.visible temporarily or returns matching UIDs).
int countClipsWithRole(const Timeline *timeline, Role role);

// Export separate stereo WAV stems per active audio role (Dialogue / Music / SFX / Ambience)
// into `outputDir` (e.g. "<baseName>_Dialogue.wav", "<baseName>_Music.wav") using FFmpeg.
MultiStemExportResult exportAudioStemsByRole(const Timeline *timeline,
                                             const QString &outputDir,
                                             const QString &baseName = QStringLiteral("stem"));

} // namespace cliproles
