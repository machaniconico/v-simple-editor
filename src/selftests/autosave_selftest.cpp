// US-2105: autosave writes through QSaveFile and only reports success after commit,
// skips an unchanged JSON, names backups with a UTC millisecond stamp plus a
// counter so rotation and recovery can order by name, reports a failed write
// through saveFailed; SubtitleStyle persists all 12 fields via toJson / fromJson.
#include "../AutoSave.h"
#include "../ProjectFile.h"
#include "../SubtitleTrackRenderer.h"
#include "SelftestGate.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>

#include <cmath>
#include <string>

namespace {

QStringList names(const QString &dir)
{
    // Name descending = newest first, the order rotation and recovery use.
    return QDir(dir).entryList({"*"}, QDir::Files, QDir::Name | QDir::Reversed);
}

QStringList backups(const QString &dir)
{
    return QDir(dir).entryList({"project_autosave_*.veditor"}, QDir::Files,
                               QDir::Name | QDir::Reversed);
}

QString readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

bool writeFile(const QString &path, const QString &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content.toUtf8());
    return true;
}

AutoSaveConfig cfg(const QString &dir, int maxBackups = 5)
{
    AutoSaveConfig c;
    c.backupDir = dir;
    c.maxBackups = maxBackups;
    return c;
}

QString jsonN(const char *tag) { return QStringLiteral("{\"autosave\":\"%1\"}").arg(tag); }

bool styleEq(const SubtitleStyle &a, const SubtitleStyle &b, std::string &why)
{
    auto d = [](double x, double y) { return std::fabs(x - y) < 1e-9; };
    bool ok = true;
    auto chk = [&](const char *n, bool eq) { if (!eq) { ok = false; why += std::string(n) + " "; } };
    chk("fontName", a.font.family() == b.font.family());
    chk("fontSize", a.font.pointSize() == b.font.pointSize());
    chk("fontBold", a.font.bold() == b.font.bold());
    chk("color", a.color.name(QColor::HexArgb) == b.color.name(QColor::HexArgb));
    chk("outlineColor", a.outlineColor.name(QColor::HexArgb) == b.outlineColor.name(QColor::HexArgb));
    chk("outlineWidth", d(a.outlineWidth, b.outlineWidth));
    chk("verticalPos", d(a.verticalPos, b.verticalPos));
    chk("boxEnabled", a.boxEnabled == b.boxEnabled);
    chk("boxColor", a.boxColor.name(QColor::HexArgb) == b.boxColor.name(QColor::HexArgb));
    chk("boxOpacity", d(a.boxOpacity, b.boxOpacity));
    chk("maxWidthFraction", d(a.maxWidthFraction, b.maxWidthFraction));
    chk("alignment", a.alignment == b.alignment);
    chk("karaokeEnabled", a.karaokeEnabled == b.karaokeEnabled);
    chk("karaokeHighlightColor",
        a.karaokeHighlightColor.name(QColor::HexArgb) == b.karaokeHighlightColor.name(QColor::HexArgb));
    return ok;
}

SubtitleStyle nonDefaultStyle()
{
    SubtitleStyle s;
    s.font = QFont("Courier New", 41);
    s.font.setBold(false);
    s.color = QColor("#80112233");
    s.outlineColor = QColor("#ff445566");
    s.outlineWidth = 3.25;
    s.boxEnabled = true;
    s.boxColor = QColor("#cc778899");
    s.boxOpacity = 0.35;
    s.verticalPos = 0.2;
    s.maxWidthFraction = 0.55;
    s.alignment = Qt::AlignRight;
    s.karaokeEnabled = true;
    s.karaokeHighlightColor = QColor("#ff00aa55");
    return s;
}

} // namespace

int runAutosaveSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir root;
    if (!root.isValid()) {
        tally.gate(1, false, "temp dir");
        return tally.finish("autosave");
    }
    auto mk = [&](const char *n) {
        const QString p = root.filePath(n);
        QDir().mkpath(p);
        return p;
    };

    // ---- G1: one save writes one file with the callback JSON ----
    {
        const QString dir = mk("g1");
        const QString A = jsonN("A");
        AutoSave as;
        as.setConfig(cfg(dir));
        as.setProjectData([&]() { return A; });
        QSignalSpy saved(&as, &AutoSave::autoSaved);
        as.performAutoSave();
        const QStringList f = backups(dir);
        const bool ok = f.size() == 1 && readAll(QDir(dir).filePath(f.value(0))) == A
                        && saved.count() == 1 && f.value(0).startsWith("project_autosave_utc_");
        tally.gate(1, ok, "files=" + std::to_string(f.size()) + " autoSaved=" + std::to_string(saved.count()));
    }

    // ---- G2: unchanged JSON is not written again ----
    {
        const QString dir = mk("g2");
        QString cur = jsonN("A");
        AutoSave as;
        as.setConfig(cfg(dir));
        as.setProjectData([&]() { return cur; });
        QSignalSpy saved(&as, &AutoSave::autoSaved);
        as.performAutoSave();
        as.performAutoSave();
        const int a1 = backups(dir).size(), a2 = saved.count();
        cur = jsonN("B");
        as.performAutoSave();
        const int b = backups(dir).size();
        as.resetLastSavedHash();
        as.performAutoSave();
        const int c = backups(dir).size();

        const QString dir2 = mk("g2seed");
        const QString C = jsonN("C");
        const bool seeded = writeFile(QDir(dir2).filePath("project_autosave_20200101_000000.veditor"), C);
        AutoSave as2;
        as2.setConfig(cfg(dir2));
        as2.setProjectData([&]() { return C; });
        as2.seedLastSavedHashFromNewestBackup();
        as2.performAutoSave();
        const int d = backups(dir2).size();
        const bool ok = a1 == 1 && a2 == 1 && b == 2 && c == 3 && seeded && d == 1;
        tally.gate(2, ok,
                   "a files=" + std::to_string(a1) + " autoSaved=" + std::to_string(a2)
                   + " b files=" + std::to_string(b) + " c files=" + std::to_string(c)
                   + " d files=" + std::to_string(d));
    }

    // ---- G3: a failed write is reported and changes nothing ----
    {
        const QString A = jsonN("A");
        // (a) open fails: backupDir is a regular file.
        const QString notDir = root.filePath("g3_file");
        writeFile(notDir, "x");
        AutoSave as;
        as.setConfig(cfg(notDir));
        as.setProjectData([&]() { return A; });
        QSignalSpy saved(&as, &AutoSave::autoSaved);
        QSignalSpy failed(&as, &AutoSave::saveFailed);
        as.performAutoSave();
        const int aFailed = failed.count(), aSaved = saved.count();
        const QString dirA = mk("g3a");
        as.setConfig(cfg(dirA));
        as.performAutoSave();
        const int aRetry = backups(dirA).size();
        const bool legA = aFailed == 1 && aSaved == 0 && aRetry == 1;

        // (b) commit fails: the target is held open by another QFile.
        const QString dirB = mk("g3b");
        const QString fixed = "project_autosave_utc_commitfail_000.veditor";
        const QString target = QDir(dirB).filePath(fixed);
        const QString X = jsonN("X");
        writeFile(target, X);
        AutoSave bs;
        bs.setConfig(cfg(dirB));
        bs.setProjectData([&]() { return A; });
        QSignalSpy bSavedSpy(&bs, &AutoSave::autoSaved);
        QSignalSpy bFailedSpy(&bs, &AutoSave::saveFailed);
        int bFailed = -1, bSaved = -1;
        bool keptX = false, noTemp = false;
        {
            QFile held(target);
            if (held.open(QIODevice::ReadOnly)) {
                autosavetest::setFixedBackupNameForTest(fixed);
                bs.performAutoSave();
                bFailed = bFailedSpy.count();
                bSaved = bSavedSpy.count();
                keptX = QString::fromUtf8(held.readAll()) == X;
            }
        }
        autosavetest::setFixedBackupNameForTest(QString());
        noTemp = true;
        for (const QString &n : names(dirB))
            if (!n.endsWith(".veditor"))
                noTemp = false;
        bs.performAutoSave();
        const int bAfter = backups(dirB).size();
        const bool legB = bFailed == 1 && bSaved == 0 && keptX && noTemp && bAfter == 2
                          && readAll(target) == X;
        tally.gate(3, legA && legB,
                   "a failed=" + std::to_string(aFailed) + " saved=" + std::to_string(aSaved)
                   + " retry=" + std::to_string(aRetry) + " b failed=" + std::to_string(bFailed)
                   + " saved=" + std::to_string(bSaved) + " keptX=" + std::to_string(keptX)
                   + " noTemp=" + std::to_string(noTemp) + " after=" + std::to_string(bAfter));
    }

    // ---- G4: rotation by name, utc names sort after old local-time names ----
    {
        const QString dir = mk("g4a");
        QString cur;
        AutoSave as;
        as.setConfig(cfg(dir, 2));
        as.setProjectData([&]() { return cur; });
        QStringList written;
        QObject::connect(&as, &AutoSave::autoSaved, &as, [&](const QString &p) { written << p; });
        QElapsedTimer t;
        t.start();
        const QString J[3] = {jsonN("1"), jsonN("2"), jsonN("3")};
        for (const QString &j : J) {
            cur = j;
            as.performAutoSave();
        }
        const qint64 ms = t.elapsed();
        const QStringList f = backups(dir);
        const bool distinct = written.size() == 3 && QSet<QString>(written.begin(), written.end()).size() == 3;
        const bool okA = f.size() == 2 && distinct && readAll(QDir(dir).filePath(f.value(0))) == J[2]
                         && !QFile::exists(written.value(0));

        const QString dirB = mk("g4b");
        const QString oldName = "project_autosave_20991231_235959.veditor";
        writeFile(QDir(dirB).filePath(oldName), jsonN("Z"));
        QString curB = jsonN("N1");
        AutoSave bs;
        bs.setConfig(cfg(dirB, 2));
        bs.setProjectData([&]() { return curB; });
        bs.performAutoSave();
        curB = jsonN("N2");
        bs.performAutoSave();
        const QStringList fb = backups(dirB);
        const bool okB = fb.size() == 2 && fb.value(0).startsWith("project_autosave_utc_")
                         && fb.value(1).startsWith("project_autosave_utc_");
        tally.gate(4, okA && okB,
                   "a files=" + std::to_string(f.size()) + " distinct=" + std::to_string(distinct)
                   + " elapsedMs=" + std::to_string(ms) + " b files=" + std::to_string(fb.size())
                   + " oldKept=" + std::to_string(fb.contains(oldName)));
    }

    // ---- G5: all 12 subtitle style fields persist ----
    {
        std::string why;
        const SubtitleStyle full = nonDefaultStyle();
        const QJsonObject json = full.toJson();
        const bool okA = styleEq(full, SubtitleStyle::fromJson(json), why);
        const std::string whyA = why;

        why.clear();
        QJsonObject old;
        for (const char *k : {"fontName", "fontSize", "fontBold", "color", "outlineColor",
                              "outlineWidth", "verticalPos"})
            old[k] = json[k];
        const SubtitleStyle fromOld = SubtitleStyle::fromJson(old);
        SubtitleStyle expect = full; // first 7 from the file, rest at struct defaults
        const SubtitleStyle def;
        expect.boxEnabled = def.boxEnabled;
        expect.boxColor = def.boxColor;
        expect.boxOpacity = def.boxOpacity;
        expect.maxWidthFraction = def.maxWidthFraction;
        expect.alignment = def.alignment;
        expect.karaokeEnabled = def.karaokeEnabled;
        expect.karaokeHighlightColor = def.karaokeHighlightColor;
        bool okB = styleEq(expect, fromOld, why) && fromOld.maxWidthFraction == 0.8
                   && fromOld.boxOpacity == 0.6;
        QJsonObject badAlign = json;
        badAlign["alignment"] = 999;
        okB = okB && SubtitleStyle::fromJson(badAlign).alignment == def.alignment;
        const std::string whyB = why;

        why.clear();
        ProjectData data;
        data.subtitleStyle = json;
        ProjectData back;
        const bool parsed = ProjectFile::fromJsonString(ProjectFile::toJsonString(data), back);
        const bool okC = parsed && styleEq(full, SubtitleStyle::fromJson(back.subtitleStyle), why);
        tally.gate(5, okA && okB && okC,
                   "a=" + std::to_string(okA) + " [" + whyA + "] b=" + std::to_string(okB) + " [" + whyB
                   + "] c=" + std::to_string(okC) + " parsed=" + std::to_string(parsed) + " [" + why + "]");
    }

    return tally.finish("autosave");
}
