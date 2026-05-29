#include <QDebug>
#include <QString>

#include "YtdlpDownloader.h"

int runYtdlpDownloaderSelftest()
{
    qInfo().noquote() << "[ytdlp-downloader] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[ytdlp-downloader] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[ytdlp-downloader] FAIL" << name << ":" << msg; };

    // G1: isYoutubeUrl positive cases
    if (YtdlpDownloader::isYoutubeUrl(QStringLiteral("https://www.youtube.com/watch?v=abc"))
     && YtdlpDownloader::isYoutubeUrl(QStringLiteral("https://youtube.com/watch?v=xyz"))
     && YtdlpDownloader::isYoutubeUrl(QStringLiteral("http://youtu.be/abc123"))
     && YtdlpDownloader::isYoutubeUrl(QStringLiteral("http://www.youtube.com/watch?v=abc"))) {
        pass("G1 isYoutubeUrl true for valid YouTube URLs");
    } else {
        fail("G1 isYoutubeUrl", QStringLiteral("one of the valid URLs returned false"));
    }

    // G2: isYoutubeUrl negative cases
    if (!YtdlpDownloader::isYoutubeUrl(QStringLiteral("https://example.com/video"))
     && !YtdlpDownloader::isYoutubeUrl(QStringLiteral("ftp://youtube.com/x"))
     && !YtdlpDownloader::isYoutubeUrl(QStringLiteral(""))
     && !YtdlpDownloader::isYoutubeUrl(QStringLiteral("random text"))) {
        pass("G2 isYoutubeUrl false for non-YouTube URLs");
    } else {
        fail("G2 isYoutubeUrl negative", QStringLiteral("one of the invalid URLs returned true"));
    }

    // G3: buildOutputTemplate contains %(title).100B.%(ext)s
    const QString tmpl = YtdlpDownloader::buildOutputTemplate(QStringLiteral("/tmp/out"));
    if (tmpl.contains(QStringLiteral("%(title).100B.%(ext)s"))) {
        pass("G3 buildOutputTemplate contains byte-limited title formatter");
    } else {
        fail("G3 buildOutputTemplate format", QStringLiteral("got: %1").arg(tmpl));
    }

    // G4: buildOutputTemplate joins outputDir with forward slash
    if (tmpl.startsWith(QStringLiteral("/tmp/out"))) {
        pass("G4 buildOutputTemplate prefixes outputDir");
    } else {
        fail("G4 buildOutputTemplate outputDir prefix", QStringLiteral("got: %1").arg(tmpl));
    }

    // G5: YtdlpDownloader instance construction (no parent), isRunning false initially
    YtdlpDownloader dl;
    if (!dl.isRunning()) {
        pass("G5 YtdlpDownloader default-constructed not running");
    } else {
        fail("G5 default not running", QStringLiteral("isRunning() returned true"));
    }

    // G6: cancel() is no-op when not running (does not crash)
    dl.cancel();
    if (!dl.isRunning()) {
        pass("G6 cancel() on idle downloader is safe no-op");
    } else {
        fail("G6 idle cancel", QStringLiteral("cancel after no-start put downloader in running state"));
    }

    qInfo().noquote().nospace() << "[ytdlp-downloader] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
