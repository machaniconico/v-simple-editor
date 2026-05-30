// edl-export selftest (ED-2): edl:: CMX3600 純粋エンジンの回帰ゲート。
//
// EdlExport は QObject / QWidget を持たない純粋エンジンなので QApplication 不要
// (SelftestRegistry の edl-export エントリは needsQApplication=false)。ここでは
// 文字列 contains / 行解析 / 数値整合だけでロジックを検証する。実際の encode や
// GUI には一切触れない。流儀は command_search_selftest.cpp に倣い [edl-export]
// プレフィックスで PASS/FAIL を出力する。

#include <QDebug>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>

#include "../EdlExport.h"
#include "../Timeline.h"   // ClipInfo

int runEdlExportSelftest()
{
    qInfo().noquote() << "[edl-export] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[edl-export] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[edl-export] FAIL" << name << ":" << msg; };

    // ------------------------------------------------------------------
    // G1: secToFrames — 1 秒 @ 30fps = 30 frames、四捨五入と負クランプ。
    // ------------------------------------------------------------------
    {
        const qint64 f1 = edl::secToFrames(1.0, 30.0);
        const qint64 f2 = edl::secToFrames(2.0, 30.0);
        const qint64 fNeg = edl::secToFrames(-5.0, 30.0);
        if (f1 == 30 && f2 == 60 && fNeg == 0) {
            pass("G1 secToFrames 1s@30fps=30, 2s=60, negative clamps to 0");
        } else {
            fail("G1 secToFrames", QStringLiteral("f1=%1 f2=%2 fNeg=%3").arg(f1).arg(f2).arg(fNeg));
        }
    }

    // ------------------------------------------------------------------
    // G2: framesToTimecode NDF — 0→"00:00:00:00"、90f@30→"00:00:03:00"。
    //     整数 fps は dropFrame=true 指定でも ':' 区切りの NDF とする。
    // ------------------------------------------------------------------
    {
        const QString tcZero = edl::framesToTimecode(0, 30.0, false);
        const QString tc90 = edl::framesToTimecode(90, 30.0, false);
        const QString tc90DropRequested = edl::framesToTimecode(90, 30.0, true);
        const QString tc60DropRequested = edl::framesToTimecode(120, 60.0, true);
        const QString tcNegative = edl::framesToTimecode(-10, 25.0, false);
        const QStringList segs = tc90.split(QLatin1Char(':'));
        const bool fmtOk = (segs.size() == 4)
            && segs.at(0).size() == 2 && segs.at(1).size() == 2
            && segs.at(2).size() == 2 && segs.at(3).size() == 2;
        if (tcZero == QStringLiteral("00:00:00:00")
            && tc90 == QStringLiteral("00:00:03:00")
            && tc90DropRequested == QStringLiteral("00:00:03:00")
            && tc60DropRequested == QStringLiteral("00:00:02:00")
            && tcNegative == QStringLiteral("00:00:00:00")
            && fmtOk
            && !tc90.contains(QLatin1Char(';'))) {
            pass("G2 framesToTimecode NDF uses ':' for integer fps and clamps negatives");
        } else {
            fail("G2 NDF timecode", QStringLiteral("zero=%1 tc90=%2 tc90Drop=%3 tc60Drop=%4 neg=%5 fmtOk=%6")
                    .arg(tcZero).arg(tc90).arg(tc90DropRequested)
                    .arg(tc60DropRequested).arg(tcNegative).arg(fmtOk));
        }
    }

    // ------------------------------------------------------------------
    // G3: framesToTimecode DF — 29.97 dropFrame=true で ';' 区切り。
    //     SMPTE ドロップフレーム: 1 分目で 2 フレームをスキップするので
    //     frame 1800 (名目 60.0s) は 00:01:00;02 になる (毎分 2f drop)。
    //     0 フレームは drop 対象外なので 00:00:00;00。
    // ------------------------------------------------------------------
    {
        const QString tcZero = edl::framesToTimecode(0, 29.97, true);
        const QString tcMin = edl::framesToTimecode(1800, 29.97, true);
        const QString tc5994Min = edl::framesToTimecode(3600, 59.94, true);
        // 区切りは "HH:MM:SS;FF" の形: ';' を 1 つ含み、その前段 "HH:MM:SS" は ':' を 2 つ持つ。
        const bool semicolonOk = tcMin.contains(QLatin1Char(';'));
        const QStringList colonSegs = tcMin.split(QLatin1Char(';'));
        const bool shapeOk = colonSegs.size() == 2
            && colonSegs.at(0).count(QLatin1Char(':')) == 2;
        if (tcZero == QStringLiteral("00:00:00;00")
            && tcMin == QStringLiteral("00:01:00;02")
            && tc5994Min == QStringLiteral("00:01:00;04")
            && semicolonOk && shapeOk) {
            pass("G3 framesToTimecode DF drops 2@29.97 and 4@59.94 at minute 1");
        } else {
            fail("G3 DF timecode", QStringLiteral("zero=%1 min2997=%2 min5994=%3")
                    .arg(tcZero).arg(tcMin).arg(tc5994Min));
        }
    }

    // ------------------------------------------------------------------
    // 共通ドキュメント: G4-G7 で使う 2 イベント EDL を組み立てる。
    // ------------------------------------------------------------------
    edl::EdlDocument doc;
    doc.title = QStringLiteral("MY EDL");
    doc.frameRate = 29.97;
    doc.dropFrame = true;
    {
        edl::EdlEvent e1;
        e1.number = 1;
        e1.reel = QStringLiteral("TAPE001");
        e1.trackType = QStringLiteral("V");
        e1.transition = QLatin1Char('C');
        e1.clipName = QStringLiteral("intro.mov");
        e1.srcInFrames = 0;
        e1.srcOutFrames = 30;
        e1.recInFrames = 0;
        e1.recOutFrames = 30;
        doc.events.push_back(e1);

        edl::EdlEvent e2;
        e2.number = 2;
        e2.reel = QStringLiteral("TAPE002");
        e2.trackType = QStringLiteral("V");
        e2.transition = QLatin1Char('C');
        e2.clipName = QStringLiteral("scene2.mov");
        e2.srcInFrames = 100;
        e2.srcOutFrames = 160;
        e2.recInFrames = 30;
        e2.recOutFrames = 90;
        doc.events.push_back(e2);
    }
    const QString cmx = edl::toCmx3600(doc);

    // ------------------------------------------------------------------
    // G4: TITLE ヘッダと FCM DROP FRAME 行を NLE 互換の表記で出す。
    // ------------------------------------------------------------------
    {
        const bool titleOk = cmx.startsWith(QStringLiteral("TITLE: MY EDL\n"));
        const bool fcmOk = cmx.contains(QStringLiteral("FCM: DROP FRAME\n\n"))
            && !cmx.contains(QStringLiteral("NON-DROP"));
        if (titleOk && fcmOk) {
            pass("G4 toCmx3600 header has exact TITLE/FCM lines");
        } else {
            fail("G4 header", QStringLiteral("titleOk=%1 fcmOk=%2").arg(titleOk).arg(fcmOk));
        }
    }

    // ------------------------------------------------------------------
    // G5: イベント行に number(3桁0埋め) / reel / トラック種別 / 'C' /
    //     4 つのタイムコードが含まれる。1 件目の行を解析する。
    // ------------------------------------------------------------------
    {
        const QStringList lines = cmx.split(QLatin1Char('\n'));
        QString evtLine;
        for (const QString& ln : lines) {
            if (ln.startsWith(QStringLiteral("001"))) { evtLine = ln; break; }
        }
        const QString expected = QStringLiteral("001  ")
            + QStringLiteral("TAPE001").leftJustified(8, QLatin1Char(' '))
            + QLatin1Char(' ')
            + QStringLiteral("V").leftJustified(6, QLatin1Char(' '))
            + QStringLiteral("C").leftJustified(8, QLatin1Char(' '))
            + QStringLiteral(" 00:00:00;00 00:00:01;00 00:00:00;00 00:00:01;00");
        // タイムコード 4 つ (DF なので ';' を含む) が単一スペース区切りで並ぶこと。
        const int tcCount = evtLine.count(QLatin1Char(';'));
        const bool rowOk = !evtLine.isEmpty()
            && evtLine == expected
            && tcCount == 4; // srcIn/srcOut/recIn/recOut 各 1 つの ';'
        if (rowOk) {
            pass("G5 event row has CMX3600 field widths and single-spaced timecodes");
        } else {
            fail("G5 event row", QStringLiteral("line='%1' expected='%2' tcCount=%3")
                    .arg(evtLine).arg(expected).arg(tcCount));
        }
    }

    // ------------------------------------------------------------------
    // G6: "* FROM CLIP NAME:" コメントに clipName が出る。
    // ------------------------------------------------------------------
    {
        const bool c1 = cmx.contains(QStringLiteral("* FROM CLIP NAME: intro.mov"));
        const bool c2 = cmx.contains(QStringLiteral("* FROM CLIP NAME: scene2.mov"));
        if (c1 && c2) {
            pass("G6 FROM CLIP NAME comment carries clipName for each event");
        } else {
            fail("G6 clip-name comment", QStringLiteral("intro=%1 scene2=%2").arg(c1).arg(c2));
        }
    }

    // ------------------------------------------------------------------
    // G7: 複数イベントが連番 (001, 002, ...) で列挙される。
    // ------------------------------------------------------------------
    {
        const bool has001 = cmx.contains(QStringLiteral("001"));
        const bool has002 = cmx.contains(QStringLiteral("002"));
        // 003 は存在しない (イベントは 2 件)。
        const QStringList lines = cmx.split(QLatin1Char('\n'));
        int eventLineCount = 0;
        for (const QString& ln : lines) {
            if (ln.startsWith(QStringLiteral("001")) || ln.startsWith(QStringLiteral("002"))) {
                ++eventLineCount;
            }
        }
        if (has001 && has002 && eventLineCount == 2) {
            pass("G7 events enumerated with sequential numbers 001, 002");
        } else {
            fail("G7 sequential numbers", QStringLiteral("has001=%1 has002=%2 count=%3")
                    .arg(has001).arg(has002).arg(eventLineCount));
        }
    }

    // ------------------------------------------------------------------
    // G8: fromClips — ClipInfo 列から recIn/recOut がタイムライン絶対位置、
    //     srcIn/srcOut が inPoint / outPoint に対応する (代表値で検証)。
    //
    //   clip A: leadInSec=1.0, inPoint=2.0, outPoint=5.0  (eff=3.0s)
    //   clip B: leadInSec=0.5, inPoint=0.0, outPoint=4.0, speed=2.0  (eff=2.0s)
    //   @30fps (整数 fps なので NDF だがフレーム計算は同じ)
    //   timeline: A は 1.0s lead → rec 1.0..4.0、B は +0.5s lead → rec 4.5..6.5
    // ------------------------------------------------------------------
    {
        ClipInfo a;
        a.filePath = QStringLiteral("/tmp/A.mov");
        a.displayName = QStringLiteral("Clip A 01");
        a.duration = 10.0;
        a.leadInSec = 1.0;
        a.inPoint = 2.0;
        a.outPoint = 5.0;
        a.speed = 1.0;

        ClipInfo b;
        b.filePath = QStringLiteral("/tmp/B.mov");
        b.displayName = QStringLiteral("Two Words 02");
        b.duration = 10.0;
        b.leadInSec = 0.5;
        b.inPoint = 0.0;
        b.outPoint = 4.0;
        b.speed = 2.0;

        QVector<ClipInfo> clips;
        clips.push_back(a);
        clips.push_back(b);

        const edl::EdlDocument fc = edl::fromClips(clips, QStringLiteral("FC"), 30.0, false);

        bool ok = fc.events.size() == 2;
        if (ok) {
            const edl::EdlEvent& ea = fc.events.at(0);
            const edl::EdlEvent& eb = fc.events.at(1);
            // 連番
            ok = ok && ea.number == 1 && eb.number == 2;
            // src: inPoint/outPoint @30fps
            ok = ok && ea.srcInFrames == 60 && ea.srcOutFrames == 150;   // 2.0s,5.0s
            ok = ok && eb.srcInFrames == 0 && eb.srcOutFrames == 120;    // 0.0s,4.0s
            // rec: A lead 1.0s → 1.0..4.0 (eff 3.0s); B lead +0.5s → 4.5..6.5 (eff 2.0s)
            ok = ok && ea.recInFrames == 30 && ea.recOutFrames == 120;   // 1.0s,4.0s
            ok = ok && eb.recInFrames == 135 && eb.recOutFrames == 195;  // 4.5s,6.5s
            // reel names: uppercase, no spaces, <=8 chars.
            ok = ok && ea.reel == QStringLiteral("CLIPA01")
                && eb.reel == QStringLiteral("TWOWORDS");
            // cut transition / V track
            ok = ok && ea.transition == QLatin1Char('C') && ea.trackType == QStringLiteral("V");
            if (ok) {
                pass("G8 fromClips maps source range, speed duration, absolute rec, reel names");
            } else {
                fail("G8 fromClips values",
                     QStringLiteral("A reel=%1 src=%2..%3 rec=%4..%5 | B reel=%6 src=%7..%8 rec=%9..%10")
                        .arg(ea.reel)
                        .arg(ea.srcInFrames).arg(ea.srcOutFrames).arg(ea.recInFrames).arg(ea.recOutFrames)
                        .arg(eb.reel)
                        .arg(eb.srcInFrames).arg(eb.srcOutFrames).arg(eb.recInFrames).arg(eb.recOutFrames));
            }
        } else {
            fail("G8 fromClips count", QStringLiteral("events=%1 (expected 2)").arg(fc.events.size()));
        }
    }

    // ------------------------------------------------------------------
    // G9: 整数 fps では dropFrame=true 指定でも NON-DROP FRAME 行、タイムコードは ':' 区切り。
    // ------------------------------------------------------------------
    {
        edl::EdlDocument ndf;
        ndf.title = QStringLiteral("NDF EDL");
        ndf.frameRate = 30.0;
        ndf.dropFrame = true;
        edl::EdlEvent e;
        e.number = 1;
        e.reel = QStringLiteral("AX");
        e.trackType = QStringLiteral("V");
        e.transition = QLatin1Char('C');
        e.clipName = QStringLiteral("c.mov");
        e.srcInFrames = 0; e.srcOutFrames = 30; e.recInFrames = 0; e.recOutFrames = 30;
        ndf.events.push_back(e);
        const QString out = edl::toCmx3600(ndf);
        const bool noSemicolon = !out.contains(QLatin1Char(';')); // NDF はタイムコードに ';' なし
        if (out.contains(QStringLiteral("FCM: NON-DROP FRAME")) && noSemicolon) {
            pass("G9 integer fps with dropFrame request yields NON-DROP FRAME and ':' timecodes");
        } else {
            fail("G9 NDF document", QStringLiteral("hasNonDrop=%1 noSemicolon=%2")
                    .arg(out.contains(QStringLiteral("FCM: NON-DROP FRAME"))).arg(noSemicolon));
        }
    }

    // ------------------------------------------------------------------
    // G10: toJson / fromJson round-trip — title / frameRate / dropFrame /
    //      events 件数 + 代表フィールドが保たれる。
    // ------------------------------------------------------------------
    {
        const QJsonObject obj = edl::toJson(doc);
        const edl::EdlDocument rt = edl::fromJson(obj);
        bool ok = rt.title == doc.title
            && qAbs(rt.frameRate - doc.frameRate) < 1e-6
            && rt.dropFrame == doc.dropFrame
            && rt.events.size() == doc.events.size();
        if (ok && rt.events.size() == 2) {
            const edl::EdlEvent& a = rt.events.at(0);
            const edl::EdlEvent& b = rt.events.at(1);
            ok = ok && a.number == 1 && a.reel == QStringLiteral("TAPE001")
                && a.clipName == QStringLiteral("intro.mov")
                && a.srcInFrames == 0 && a.srcOutFrames == 30
                && a.recInFrames == 0 && a.recOutFrames == 30;
            ok = ok && b.number == 2 && b.reel == QStringLiteral("TAPE002")
                && b.recInFrames == 30 && b.recOutFrames == 90;
        }
        if (ok) {
            pass("G10 toJson/fromJson round-trips title/frameRate/dropFrame/events");
        } else {
            fail("G10 json round-trip",
                 QStringLiteral("title='%1' fps=%2 df=%3 events=%4")
                    .arg(rt.title).arg(rt.frameRate).arg(rt.dropFrame).arg(rt.events.size()));
        }
    }

    qInfo().noquote().nospace() << "[edl-export] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
