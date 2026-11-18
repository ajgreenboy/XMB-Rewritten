// Sidecar subtitle parsing, cue lookup, labels and the Jellyfin English-only rule (audit/SUBTITLES.md,
// src/subtitles.*, src/jellyfin_client.h). Pure functions: no window, no player, no network.
//
// This is the regression surface for "changing the SRT subtitle leaves the language unchanged": it proves
// that two same-timed sidecar SRT files stay two distinct tracks with two distinct sets of cue text, so the
// text a renderer draws is the text of the *chosen* track and never another track's. The shell's track
// selection (Shell::chooseSubtitleTrack / subtitleTrackIndex_) is not reachable here: it needs QMediaPlayer.
//
// Build with -DXMB_SUBTITLE_BUILD_TESTS=ON; exit code 0 = all pass. Pass a scratch parent folder as the
// first argument to keep the test off %TEMP%.
#include "subtitles.h"
#include "jellyfin_client.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>

namespace sub = xmb::subtitles;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static bool writeText(const QString& path, const QByteArray& body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(body) == body.size();
}

// Two same-timed SRT files with different text, the exact shape of the bug report's fixture.
static const QByteArray kEnglish =
    "1\r\n"
    "00:00:01,000 --> 00:00:04,000\r\n"
    "Hello world\r\n"
    "\r\n"
    "2\r\n"
    "00:00:05,000 --> 00:00:08,000\r\n"
    "Second English line\r\n";
static const QByteArray kFrench =
    "1\r\n"
    "00:00:01,000 --> 00:00:04,000\r\n"
    "Bonjour le monde\r\n"
    "\r\n"
    "2\r\n"
    "00:00:05,000 --> 00:00:08,000\r\n"
    "Deuxieme ligne\r\n";

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString parent = argc > 1 ? QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[1])) : QDir::tempPath();
    QTemporaryDir scratch(parent + QStringLiteral("/xmb-subtitle-XXXXXX"));
    if (!scratch.isValid()) { std::fprintf(stderr, "cannot create scratch folder\n"); return 2; }
    const QString base = QDir::cleanPath(scratch.path());

    // ---- discovery of two sidecars ----------------------------------------------------------------------------------
    {
        const QString media = base + QStringLiteral("/Movie.mp4");
        check(writeText(media, QByteArrayLiteral("x")), "scratch media file");
        check(writeText(base + QStringLiteral("/Movie.en.srt"), kEnglish), "English sidecar written");
        check(writeText(base + QStringLiteral("/Movie.fr.srt"), kFrench), "French sidecar written");
        const sub::Discovery d = sub::discover(media);
        check(d.tracks.size() == 2, "discover: two usable sidecars");
        if (d.tracks.size() == 2) {
            const sub::Track& en = d.tracks[0];
            const sub::Track& fr = d.tracks[1];
            check(en.language == QStringLiteral("en") && fr.language == QStringLiteral("fr"),
                  "discover: tags read en then fr (sorted by label)");
            check(en.format == QStringLiteral("srt") && fr.format == QStringLiteral("srt"),
                  "discover: both are SRT");
            check(en.cues.size() == 2 && fr.cues.size() == 2, "discover: two cues each");
            if (en.cues.size() == 2 && fr.cues.size() == 2) {
                check(en.cues[0].text == QStringLiteral("Hello world"),
                      "discover: English cue 1 text is the English sentence");
                check(fr.cues[0].text == QStringLiteral("Bonjour le monde"),
                      "discover: French cue 1 text is the French sentence");
                check(en.cues[0].start == 1000 && fr.cues[0].start == 1000,
                      "discover: both cues share the same start time");
                check(en.cues[0].text != fr.cues[0].text,
                      "discover: same-timed cues carry different text (the language switch must change the text)");
            }
        }
        check(d.rejected.isEmpty(), "discover: nothing rejected");
    }

    // ---- labels ------------------------------------------------------------------------------------------------------
    {
        sub::Track en; en.language = QStringLiteral("en"); en.format = QStringLiteral("srt");
        sub::Track fr; fr.language = QStringLiteral("fr"); fr.format = QStringLiteral("srt");
        sub::Track forced; forced.language = QStringLiteral("en.forced"); forced.format = QStringLiteral("srt");
        sub::Track bare; bare.language = QString{}; bare.format = QStringLiteral("srt");
        sub::Track vtt; vtt.language = QString{}; vtt.format = QStringLiteral("vtt");
        sub::Track pgs; pgs.language = QStringLiteral("eng"); pgs.format = QStringLiteral("sup"); pgs.image = true;
        check(sub::displayName(en) == QStringLiteral("English"), "displayName: en -> English");
        check(sub::displayName(fr) == QStringLiteral("French"), "displayName: fr -> French");
        check(sub::displayName(forced) == QStringLiteral("English (forced)"), "displayName: en.forced -> English (forced)");
        check(sub::displayName(bare) == QStringLiteral("On (SRT)"), "displayName: untagged SRT -> On (SRT)");
        check(sub::displayName(vtt) == QStringLiteral("On (VTT)"), "displayName: untagged WebVTT -> On (VTT)");
        check(sub::displayName(pgs) == QStringLiteral("English (PGS)"), "displayName: image track keeps its kind");
    }

    // ---- parse() on server bytes (the Jellyfin text path) ------------------------------------------------------------
    {
        sub::Track en; en.language = QStringLiteral("en");
        QString reason;
        check(sub::parse(kEnglish, QStringLiteral("srt"), en, &reason), "parse: English bytes usable");
        check(reason.isEmpty() && en.cues.size() == 2 && en.cues[0].text == QStringLiteral("Hello world"),
              "parse: English bytes give the English cue");
        sub::Track fr; fr.language = QStringLiteral("fr");
        check(sub::parse(kFrench, QStringLiteral("srt"), fr, &reason), "parse: French bytes usable");
        check(fr.cues.size() == 2 && fr.cues[0].text == QStringLiteral("Bonjour le monde"),
              "parse: French bytes give the French cue");
        // A failed parse must clear whatever the track already held: the Jellyfin flow parses into a live
        // subtitleTracks_ element, so a re-parse that kept the previous cues would render the wrong language's
        // text. Each failure path below re-uses a track that was seeded first.
        sub::Track stale; stale.language = QStringLiteral("en");
        check(sub::parse(kEnglish, QStringLiteral("srt"), stale, &reason), "parse: seed the track first");
        check(stale.cues.size() == 2, "parse: the seed took");
        const QByteArray junk = "no timing line anywhere\njust text\n";
        check(!sub::parse(junk, QStringLiteral("srt"), stale, &reason), "parse: a stream with no cue is refused");
        check(stale.cues.isEmpty(), "parse: a refused re-parse clears the previously-parsed cues");
        check(stale.skipped == 0 && stale.bytes == 0, "parse: a refused re-parse also resets skipped and bytes");
        check(sub::parse(kEnglish, QStringLiteral("srt"), stale, &reason) && stale.cues.size() == 2,
              "parse: the same track parses again after a failure");
        check(!sub::parse(QByteArray{}, QStringLiteral("srt"), stale, &reason) && stale.cues.isEmpty(),
              "parse: empty bytes clear the cues too");
        check(sub::parse(kFrench, QStringLiteral("srt"), stale, &reason) && stale.cues[0].text == QStringLiteral("Bonjour le monde"),
              "parse: re-seed with French");
        check(!sub::parse(kFrench, QStringLiteral("ass"), stale, &reason) && stale.cues.isEmpty(),
              "parse: an unsupported format clears the cues too");
    }

    // ---- cue lookup ---------------------------------------------------------------------------------------------------
    {
        sub::Track track;
        QString reason;
        sub::parse(kEnglish, QStringLiteral("srt"), track, &reason);
        check(sub::cueAt(track.cues, 500) == -1, "cueAt: before the first cue is none");
        check(sub::cueAt(track.cues, 1000) == 0 && sub::cueAt(track.cues, 3999) == 0, "cueAt: cue 1 covers [1000,4000)");
        check(sub::cueAt(track.cues, 4000) == -1, "cueAt: the 1 s gap between cues is none");
        check(sub::cueAt(track.cues, 5000) == 1 && sub::cueAt(track.cues, 7999) == 1, "cueAt: cue 2 covers [5000,8000)");
        check(sub::cueAt(track.cues, 8000) == -1, "cueAt: past the last cue is none");
        check(sub::cueAt({}, 0) == -1 && sub::cueAt(track.cues, -1) == -1, "cueAt: empty list and negative position are none");
    }

    // ---- the Jellyfin English-only rule (text and PGS alike) ---------------------------------------------------------
    {
        auto sub = [](const char* lang, const char* title, const char* codec, bool text) {
            xmb::JellyfinTrack t; t.type = QStringLiteral("Subtitle"); t.index = 1;
            t.language = QString::fromLatin1(lang); t.title = QString::fromLatin1(title);
            t.codec = QString::fromLatin1(codec); t.text = text; return t;
        };
        check(xmb::isEnglishSubtitle(sub("eng", "English", "subrip", true)), "English rule: eng text is English");
        check(xmb::isEnglishSubtitle(sub("en", "English", "subrip", true)), "English rule: en text is English");
        check(xmb::isEnglishSubtitle(sub("en-US", "English", "subrip", true)), "English rule: en-US text is English");
        check(xmb::isEnglishSubtitle(sub("eng", "English (SDH)", "pgssub", false)), "English rule: eng PGS is English");
        check(xmb::isEnglishSubtitle(sub("", "English (Forced)", "subrip", true)), "English rule: a title naming English is English");
        check(!xmb::isEnglishSubtitle(sub("fr", "French", "subrip", true)), "English rule: fr text is not English");
        check(!xmb::isEnglishSubtitle(sub("fra", "French", "subrip", true)), "English rule: fra text is not English");
        check(!xmb::isEnglishSubtitle(sub("de", "Deutsch", "subrip", true)), "English rule: de text is not English");
        check(!xmb::isEnglishSubtitle(sub("fra", "French", "pgssub", false)), "English rule: fr PGS is not English");
        check(!xmb::isEnglishSubtitle(sub("", "Francais", "subrip", true)), "English rule: a non-English title is not English");
    }

    std::printf(failures ? "subtitle tests: %d FAILED\n" : "subtitle tests: all passed\n", failures);
    return failures ? 1 : 0;
}
