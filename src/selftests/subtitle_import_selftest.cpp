#include "SelftestGate.h"
#include "../SubtitleIO.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringConverter>
#include <QTemporaryDir>

#include <string>

// US-2104: SRT / VTT are read as raw bytes and decoded by BOM, strict UTF-8, then
// Shift_JIS / System. A '.' millisecond separator is accepted on SRT import, an import
// whose cue blocks all parse to nothing fails, and skipped blocks / fallback decoding /
// a missing WEBVTT header come back as warnings.
namespace {

bool writeBytes(const QTemporaryDir &dir, const char *name, const QByteArray &bytes, QString *path)
{
    *path = dir.filePath(QString::fromLatin1(name));
    QFile f(*path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

std::string s(const QString &q) { return q.toStdString(); }

bool sameClip(const caption::Clip &a, const caption::Clip &b)
{
    return a.startMs == b.startMs && a.endMs == b.endMs && a.text == b.text && a.actor == b.actor;
}

} // namespace

int runSubtitleImportSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "subtitle-import: temp dir unavailable\n";
        tally.gate(1, false, "temp dir");
        return tally.finish("subtitle-import");
    }
    const QString kon = QString::fromUtf8("こんにちは");
    QString path;

    // G1: BOM UTF-8, two cues, ',' separator (control).
    {
        const QByteArray bytes = QByteArray("\xEF\xBB\xBF")
            + "1\r\n00:00:01,000 --> 00:00:02,000\r\nhello\r\n\r\n"
              "2\r\n00:00:03,000 --> 00:00:04,500\r\nworld\r\n";
        const bool w = writeBytes(dir, "g1.srt", bytes, &path);
        const subtitle::ImportResult r = subtitle::importSrt(path);
        const bool ok = w && r.success && r.clips.size() == 2 && r.clips[0].text == "hello"
            && r.clips[0].startMs == 1000 && r.clips[1].text == "world" && r.clips[1].endMs == 4500
            && r.encodingUsed == "UTF-8" && r.warnings.isEmpty();
        tally.gate(1, ok, "w=" + std::to_string(w) + " clips=" + std::to_string(r.clips.size())
                   + " enc=" + s(r.encodingUsed) + " warnings=" + std::to_string(r.warnings.size()));
    }

    // G2: Shift_JIS bytes without BOM.
    {
        const QByteArray sjis("\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD");
        const QByteArray bytes = QByteArray("1\n00:00:01,000 --> 00:00:02,000\n") + sjis + "\n";
        const bool w = writeBytes(dir, "g2.srt", bytes, &path);
        const subtitle::ImportResult r = subtitle::importSrt(path);
        const bool required = w && r.success && r.encodingUsed != "UTF-8" && !r.encodingUsed.isEmpty()
            && r.warnings.size() == 1 && r.clips.size() == 1;

        QStringDecoder sj("Shift_JIS");
        QStringDecoder sys(QStringConverter::System);
        const bool sjOk = sj.isValid() && QString(sj(sjis)) == kon;
        const bool sysOk = QString(sys(sjis)) == kon;
        bool text = true;
        std::string note = "codec none";
        if (sjOk || sysOk) {
            text = r.clips.size() == 1 && r.clips[0].text == kon
                && !r.clips[0].text.contains(QChar(0xFFFD));
            note = sjOk ? "codec Shift_JIS" : "codec System";
        }
        tally.gate(2, required && text, note + " enc=" + s(r.encodingUsed)
                   + " warnings=" + std::to_string(r.warnings.size())
                   + " text=" + (r.clips.isEmpty() ? std::string("-") : s(r.clips[0].text.toUtf8().toHex())));
    }

    // G3: '.' millisecond separator in SRT.
    {
        const bool w = writeBytes(dir, "g3.srt",
            "1\n00:00:01.000 --> 00:00:02.000\ndot\n", &path);
        const subtitle::ImportResult r = subtitle::importSrt(path);
        const bool ok = w && r.success && r.clips.size() == 1 && r.clips[0].startMs == 1000
            && r.clips[0].endMs == 2000 && r.clips[0].text == "dot";
        tally.gate(3, ok, "w=" + std::to_string(w) + " clips=" + std::to_string(r.clips.size())
                   + " ok=" + std::to_string(r.success));
    }

    // G4: every cue broken -> failure; WEBVTT header + NOTE only -> success with no clips.
    {
        const bool w = writeBytes(dir, "g4.srt",
            "1\n00:00:xx,000 --> 00:00:02,000\nbad one\n\n"
            "2\n00:00:05,000 --> 00:00:03,000\nbad two\n", &path);
        const subtitle::ImportResult r = subtitle::importSrt(path);
        const bool bad = w && !r.success && !r.error.isEmpty() && r.clips.isEmpty();
        const bool w2 = writeBytes(dir, "g4.vtt", "WEBVTT\n\nNOTE just a comment\n", &path);
        const subtitle::ImportResult c = subtitle::importVtt(path);
        const bool ctl = w2 && c.success && c.clips.isEmpty() && c.error.isEmpty();
        tally.gate(4, bad && ctl, "bad=" + std::to_string(bad) + " err=" + s(r.error)
                   + " ctl=" + std::to_string(ctl));
    }

    // G5: one good + one broken cue -> success with a skip warning.
    {
        const bool w = writeBytes(dir, "g5.srt",
            "1\n00:00:01,000 --> 00:00:02,000\ngood\n\n"
            "2\n00:00:zz,000 --> 00:00:04,000\nbad\n", &path);
        const subtitle::ImportResult r = subtitle::importSrt(path);
        const bool ok = w && r.success && r.clips.size() == 1 && r.warnings.size() == 1
            && r.warnings[0].contains(QString::fromUtf8("読み飛ばし"));
        tally.gate(5, ok, "clips=" + std::to_string(r.clips.size())
                   + " warnings=" + s(r.warnings.join('|')));
    }

    // G6: VTT tab vs space cue settings; UTF-16LE BOM + CRLF; header-less VTT.
    {
        const bool wt = writeBytes(dir, "g6t.vtt",
            "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\tline:0 align:start\ntab\n", &path);
        const subtitle::ImportResult t = subtitle::importVtt(path);
        const bool ws = writeBytes(dir, "g6s.vtt",
            "WEBVTT\n\n00:00:01.000 --> 00:00:02.000 line:0 align:start\ntab\n", &path);
        const subtitle::ImportResult sp = subtitle::importVtt(path);
        const bool settings = wt && ws && t.success && sp.success && t.clips.size() == 1
            && sp.clips.size() == 1 && sameClip(t.clips[0], sp.clips[0]) && t.clips[0].endMs == 2000;

        const QString u16text = QStringLiteral("﻿WEBVTT\r\n\r\n00:00:01.000 --> 00:00:02.000\r\n")
            + kon + QStringLiteral("\r\n\r\n00:00:03.000 --> 00:00:04.000\r\n")
            + QString::fromUtf8("字幕二") + QStringLiteral("\r\n");
        QStringEncoder enc(QStringConverter::Utf16LE, QStringConverter::Flag::Stateless);
        const QByteArray u16 = enc(u16text);
        const bool wu = writeBytes(dir, "g6u.vtt", u16, &path);
        const subtitle::ImportResult u = subtitle::importVtt(path);
        const bool utf16 = wu && u.success && u.clips.size() == 2 && u.clips[0].text == kon
            && u.clips[1].text == QString::fromUtf8("字幕二")
            && !u.clips[0].text.contains(QChar(0xFFFD)) && !u.clips[1].text.contains(QChar(0xFFFD))
            && u.encodingUsed == "UTF-16LE" && u.warnings.isEmpty();

        const bool wh = writeBytes(dir, "g6h.vtt", "00:00:01.000 --> 00:00:02.000\nnohdr\n", &path);
        const subtitle::ImportResult h = subtitle::importVtt(path);
        const bool header = wh && h.success && h.clips.size() == 1 && h.warnings.size() == 1
            && h.error.isEmpty();

        tally.gate(6, settings && utf16 && header,
                   "settings=" + std::to_string(settings) + " utf16=" + std::to_string(utf16)
                   + " enc=" + s(u.encodingUsed) + " u.warnings=" + std::to_string(u.warnings.size())
                   + " header=" + std::to_string(header) + " h.error=" + s(h.error));
    }

    return tally.finish("subtitle-import");
}
