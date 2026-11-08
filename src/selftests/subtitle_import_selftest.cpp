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

    // G7: BOM-less UTF-16 (LE / BE, CRLF) is detected before the Shift_JIS fallback.
    {
        const QString text = QStringLiteral("1\r\n00:00:01,000 --> 00:00:02,000\r\n") + kon
            + QStringLiteral("\r\n\r\n2\r\n00:00:03,000 --> 00:00:04,000\r\n")
            + QString::fromUtf8("字幕二") + QStringLiteral("\r\n");
        QStringEncoder le(QStringConverter::Utf16LE, QStringConverter::Flag::Stateless);
        QStringEncoder be(QStringConverter::Utf16BE, QStringConverter::Flag::Stateless);
        const QByteArray leBytes = le(text);
        const QByteArray beBytes = be(text);
        auto two = [&](const subtitle::ImportResult &r) {
            return r.success && r.clips.size() == 2 && r.clips[0].text == kon
                && r.clips[1].text == QString::fromUtf8("字幕二") && r.clips[0].startMs == 1000
                && r.clips[1].endMs == 4000 && r.encodingUsed.contains("UTF-16");
        };
        const bool wl = writeBytes(dir, "g7le.srt", leBytes, &path);
        const subtitle::ImportResult l = subtitle::importSrt(path);
        const bool leOk = wl && two(l) && l.warnings.size() == 1;
        const bool wb = writeBytes(dir, "g7be.srt", beBytes, &path);
        const subtitle::ImportResult b = subtitle::importSrt(path);
        const bool beOk = wb && two(b) && b.warnings.size() == 1;
        const bool wo = writeBytes(dir, "g7odd.srt", leBytes + "x", &path);
        const subtitle::ImportResult o = subtitle::importSrt(path);
        const bool oddOk = wo && two(o) && o.warnings.size() == 1
            && o.warnings[0].contains(QString::fromUtf8("末尾 1 byte を無視"))
            && o.warnings[0].contains(QString::fromUtf8("として読みました"));
        // Negative control (pin): the Shift_JIS fixture of G2 stays Shift_JIS.
        const QByteArray sjis("\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD");
        const bool ws = writeBytes(dir, "g7sj.srt",
            QByteArray("1\n00:00:01,000 --> 00:00:02,000\n") + sjis + "\n", &path);
        const subtitle::ImportResult sj = subtitle::importSrt(path);
        QStringDecoder sjc("Shift_JIS");
        const bool sjOk = ws && sj.clips.size() == 1 && sj.warnings.size() == 1
            && (!sjc.isValid() || sj.encodingUsed == "Shift_JIS");
        tally.gate(7, leOk && beOk && oddOk && sjOk,
                   "le=" + std::to_string(leOk) + " enc=" + s(l.encodingUsed) + " clips=" + std::to_string(l.clips.size())
                   + " w=" + std::to_string(l.warnings.size()) + " be=" + std::to_string(beOk)
                   + " enc=" + s(b.encodingUsed) + " odd=" + std::to_string(oddOk) + " oddW=" + s(o.warnings.join('|'))
                   + " sjis=" + std::to_string(sjOk) + " enc=" + s(sj.encodingUsed));
    }

    // G8: 1-3 fraction digits on import (right-padded); the public parsers stay strict.
    {
        const bool w1 = writeBytes(dir, "g8.srt", "1\n00:00:01,5 --> 00:00:02,50\nfrac\n", &path);
        const subtitle::ImportResult r = subtitle::importSrt(path);
        const bool srt = w1 && r.success && r.clips.size() == 1 && r.clips[0].startMs == 1500
            && r.clips[0].endMs == 2500 && r.clips[0].text == "frac";
        const bool w2 = writeBytes(dir, "g8.vtt", "WEBVTT\n\n00:01.005 --> 00:02.000\nvtt\n", &path);
        const subtitle::ImportResult v = subtitle::importVtt(path);
        const bool vtt = w2 && v.success && v.clips.size() == 1 && v.clips[0].startMs == 1005
            && v.clips[0].endMs == 2000;
        const bool strict = subtitle::parseSrtTimestamp("00:00:01,5") == -1
            && subtitle::parseSrtTimestamp("00:00:01,500") == 1500;
        tally.gate(8, srt && vtt && strict,
                   "srt=" + std::to_string(srt) + " clips=" + std::to_string(r.clips.size())
                   + " err=" + s(r.error) + " vtt=" + std::to_string(vtt)
                   + " strict=" + std::to_string(strict));
    }

    // G9: the arrow may be surrounded by tab / U+3000 / U+00A0 or nothing; a timestamp line is
    // found per line and anchored, while the block-candidate test stays a loose arrow test.
    {
        const QString ts[4] = {
            QStringLiteral("00:00:01,000\t-->\t00:00:02,000"),
            QStringLiteral("00:00:01,000-->00:00:02,000"),
            QStringLiteral("00:00:01,000") + QChar(0x3000) + QStringLiteral("-->") + QChar(0x3000)
                + QStringLiteral("00:00:02,000"),
            QStringLiteral("00:00:01,000") + QChar(0x00A0) + QStringLiteral("-->") + QChar(0x00A0)
                + QStringLiteral("00:00:02,000")};
        bool arrows = true;
        std::string arrowNote;
        for (int i = 0; i < 4; ++i) {
            const bool w = writeBytes(dir, "g9a.srt",
                (QStringLiteral("1\n") + ts[i] + QStringLiteral("\narrow\n")).toUtf8(), &path);
            const subtitle::ImportResult r = subtitle::importSrt(path);
            const bool ok = w && r.success && r.clips.size() == 1 && r.clips[0].startMs == 1000
                && r.clips[0].endMs == 2000 && r.warnings.isEmpty();
            arrows = arrows && ok;
            arrowNote += std::to_string(ok);
        }
        const bool wv = writeBytes(dir, "g9a.vtt",
            "WEBVTT\n\n00:00:01.000 -->\t00:00:02.000\tline:0 align:start\nset\n", &path);
        const subtitle::ImportResult vs = subtitle::importVtt(path);
        const bool settings = wv && vs.clips.size() == 1 && vs.clips[0].endMs == 2000
            && vs.clips[0].text == "set";

        // (i) body lines with an arrow stay body text; (i') a VTT identifier line with an arrow
        // is not the timestamp line and does not become body text.
        const bool wi = writeBytes(dir, "g9i.srt",
            "1\n00:00:01,000 --> 00:00:02,000\nfirst\na-->b\n\n"
            "2\n00:00:03,000 --> 00:00:04,000\n10-->20\n", &path);
        const subtitle::ImportResult ri = subtitle::importSrt(path);
        const bool body = wi && ri.success && ri.clips.size() == 2 && ri.clips[0].text == "first\na-->b"
            && ri.clips[1].text == "10-->20";
        const bool wid = writeBytes(dir, "g9id.vtt",
            "WEBVTT\n\na-->b\n00:00:01.000 --> 00:00:02.000\nident\n", &path);
        const subtitle::ImportResult rid = subtitle::importVtt(path);
        const bool ident = wid && rid.clips.size() == 1 && rid.clips[0].startMs == 1000
            && rid.clips[0].endMs == 2000 && rid.clips[0].text == "ident";

        // (ii) full-width digits are not a timestamp line: skipped with one warning.
        const bool wf = writeBytes(dir, "g9f.srt",
            (QString::fromUtf8("1\n００:００:０１,０００ --> ００:００:０２,０００\nfull\n")).toUtf8(), &path);
        const subtitle::ImportResult rf = subtitle::importSrt(path);
        const bool full = wf && !rf.success && rf.clips.isEmpty();
        const bool wf2 = writeBytes(dir, "g9f2.srt",
            (QString::fromUtf8("1\n00:00:01,000 --> 00:00:02,000\ngood\n\n"
                               "2\n００:００:０１,０００ --> ００:００:０２,０００\nfull\n")).toUtf8(), &path);
        const subtitle::ImportResult rf2 = subtitle::importSrt(path);
        const bool fullWarn = wf2 && rf2.success && rf2.clips.size() == 1 && rf2.warnings.size() == 1
            && rf2.warnings[0].contains(QString::fromUtf8("読み飛ばし"));

        // (ii') a full-width line is not the timestamp line, so the ASCII line after it is read.
        const bool wf3 = writeBytes(dir, "g9f3.srt",
            (QString::fromUtf8("1\n００:００:０１,０００ --> ００:００:０２,０００\n"
                               "00:00:03,000 --> 00:00:04,000\nreal\n")).toUtf8(), &path);
        const subtitle::ImportResult rf3 = subtitle::importSrt(path);
        const bool fullNext = wf3 && rf3.success && rf3.clips.size() == 1 && rf3.clips[0].startMs == 3000
            && rf3.clips[0].text == "real";

        // (iii) G4 / G5 fixtures re-evaluated with the new predicates, plus the declared cases.
        const bool w4 = writeBytes(dir, "g9g4.srt",
            "1\n00:00:xx,000 --> 00:00:02,000\nbad one\n\n"
            "2\n00:00:05,000 --> 00:00:03,000\nbad two\n", &path);
        const subtitle::ImportResult r4 = subtitle::importSrt(path);
        const bool g4 = w4 && !r4.success && r4.clips.isEmpty()
            && r4.error.contains(QString::fromUtf8("2 ブロック"));
        const bool w5 = writeBytes(dir, "g9g5.srt",
            "1\n00:00:01,000 --> 00:00:02,000\ngood\n\n"
            "2\n00:00:zz,000 --> 00:00:04,000\nbad\n", &path);
        const subtitle::ImportResult r5 = subtitle::importSrt(path);
        const bool g5 = w5 && r5.success && r5.clips.size() == 1 && r5.warnings.size() == 1;
        const bool wa = writeBytes(dir, "g9ab.srt", "1\na --> b\n", &path);
        const subtitle::ImportResult ra = subtitle::importSrt(path);
        const bool ab = wa && !ra.success && ra.clips.isEmpty();
        const bool wa2 = writeBytes(dir, "g9ab2.srt",
            "1\n00:00:01,000 --> 00:00:02,000\ngood\n\n2\na --> b\n", &path);
        const subtitle::ImportResult ra2 = subtitle::importSrt(path);
        const bool abWarn = wa2 && ra2.success && ra2.clips.size() == 1 && ra2.warnings.size() == 1;
        const bool wn = writeBytes(dir, "g9num.srt",
            "1\n00:00:01,000 --> 00:00:02,000\ngood\n\n2\n10-->20\n", &path);
        const subtitle::ImportResult rn = subtitle::importSrt(path);
        const bool numWarn = wn && rn.success && rn.clips.size() == 1 && rn.warnings.size() == 1;
        const bool wvb = writeBytes(dir, "g9vb.vtt",
            "WEBVTT\n\n00:00:xx.000 --> 00:00:02.000\nbad\n\n00:00:03.000 --> 00:00:04.000\ngood\n", &path);
        const subtitle::ImportResult rvb = subtitle::importVtt(path);
        const bool vttBad = wvb && rvb.success && rvb.clips.size() == 1 && rvb.warnings.size() == 1
            && rvb.warnings[0].contains(QString::fromUtf8("読み飛ばし"));
        const bool wvi = writeBytes(dir, "g9vi.vtt",
            "WEBVTT\n\nintro\n00:00:01.000 --> 00:00:02.000\nid\n", &path);
        const subtitle::ImportResult rvi = subtitle::importVtt(path);
        const bool vttId = wvi && rvi.clips.size() == 1 && rvi.clips[0].text == "id";

        tally.gate(9, arrows && settings && body && ident && full && fullWarn && fullNext && g4 && g5 && ab
                   && abWarn && numWarn && vttBad && vttId,
                   "arrows=" + arrowNote + " settings=" + std::to_string(settings)
                   + " body=" + std::to_string(body) + " ident=" + std::to_string(ident)
                   + " full=" + std::to_string(full) + " fullWarn=" + std::to_string(fullWarn)
                   + " fullNext=" + std::to_string(fullNext)
                   + " g4=" + std::to_string(g4) + " g5=" + std::to_string(g5)
                   + " ab=" + std::to_string(ab) + " abWarn=" + std::to_string(abWarn)
                   + " numWarn=" + std::to_string(numWarn) + " vttBad=" + std::to_string(vttBad)
                   + " vttId=" + std::to_string(vttId));
    }

    return tally.finish("subtitle-import");
}
