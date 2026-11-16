// Jellyfin player state logic (audit/JELLYFIN-PLAYER.md): position ticks, the 90 percent played rule, what a stop
// leaves as a resume point, chapter navigation, which audio stream is asked for and the per-title choice store.
// Pure functions: no network, no player, no server.
//   xmb-jellyfin-player-tests
#include "jellyfin_player.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
#include <limits>

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
xmb::JellyfinTrack audio(int index, bool isDefault = false) {
    xmb::JellyfinTrack t; t.index = index; t.type = QStringLiteral("Audio"); t.title = QStringLiteral("a"); t.isDefault = isDefault; return t;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace xmb;
    using namespace xmb::jfplayer;

    // ---- ticks ----------------------------------------------------------------------------------------------------
    check(ticksFromMs(0) == 0 && ticksFromMs(1) == 10000 && ticksFromMs(61000) == 610000000, "ticks: ms times 10000");
    check(ticksFromMs(-5) == 0, "ticks: negative is 0");
    check(ticksFromMs(std::numeric_limits<qint64>::max()) == std::numeric_limits<qint64>::max(), "ticks: overflow saturates");

    // ---- the played rule ------------------------------------------------------------------------------------------
    check(!reachedPlayed(89999, 100000) && reachedPlayed(90000, 100000) && reachedPlayed(100000, 100000), "played: the boundary is exactly 90 percent");
    check(!reachedPlayed(5000000, 0) && !reachedPlayed(5000000, -1), "played: an unknown duration is never played");
    check(!reachedPlayed(-1, 100000), "played: a negative position is not played");
    check(reachedPlayed(std::numeric_limits<qint64>::max(), 100000) && !reachedPlayed(1, std::numeric_limits<qint64>::max()), "played: no overflow at the extremes");
    {
        const auto mid = stopOutcome(1800000, 3600000, false);
        check(!mid.played && mid.resumeMs == 1800000, "stop: halfway keeps a resume point");
        const auto late = stopOutcome(3300000, 3600000, false);
        check(late.played && late.resumeMs == 0, "stop: past 90 percent is watched with no resume point");
        const auto end = stopOutcome(0, 3600000, true);
        check(end.played && end.resumeMs == 0, "stop: reaching the end is watched even when the backend rewound to 0");
        const auto early = stopOutcome(9999, 3600000, false);
        check(!early.played && early.resumeMs == 0, "stop: under ten seconds in is no resume point");
        check(stopOutcome(10000, 3600000, false).resumeMs == 10000, "stop: ten seconds in is kept");
        const auto unknown = stopOutcome(600000, 0, false);
        check(!unknown.played && unknown.resumeMs == 600000, "stop: an unknown duration keeps the position and never marks played");
    }

    // ---- chapters -------------------------------------------------------------------------------------------------
    {
        const auto list = normalizedChapters({{300000, "C"}, {0, "A"}, {-5, "bad"}, {300000, "dup"}, {100000, "B"}});
        check(list.size() == 3 && list[0].startMs == 0 && list[1].startMs == 100000 && list[2].startMs == 300000 && list[2].name == "C", "chapters: sorted, negatives and duplicate starts dropped");
        QVector<Chapter> many;
        for (int i = 0; i < 500; ++i) many.push_back({qint64(i) * 1000, {}});
        check(normalizedChapters(many).size() == 200, "chapters: bounded to 200");
        check(chapterIndexAt(list, 0) == 0 && chapterIndexAt(list, 99999) == 0 && chapterIndexAt(list, 100000) == 1 && chapterIndexAt(list, 9999999) == 2, "chapters: the chapter a position is in");
        check(chapterIndexAt({}, 5) == -1 && chapterIndexAt({{1000, "x"}}, 500) == -1, "chapters: none before the first mark");
        check(chapterSkipTarget(list, 50000, 1) == 100000 && chapterSkipTarget(list, 150000, 1) == 300000, "skip: next goes to the next chapter start");
        check(chapterSkipTarget(list, 400000, 1) == -1, "skip: past the last chapter falls through to the queue");
        check(chapterSkipTarget(list, 150000, -1) == 100000, "skip: back more than 3 s into a chapter restarts it");
        check(chapterSkipTarget(list, 101000, -1) == 0, "skip: back within 3 s goes to the previous chapter");
        check(chapterSkipTarget(list, 1000, -1) == -1, "skip: back at the very start falls through to the previous item");
        check(chapterSkipTarget(list, 5000, -1) == 0, "skip: back inside the first chapter restarts it");
        check(chapterSkipTarget({{0, "only"}}, 5000, 1) == -1 && chapterSkipTarget(list, 5000, 0) == -1, "skip: one chapter or no direction does nothing");
        check(chapterLabel({0, "Opening"}, 0) == QLatin1String("1. Opening") && chapterLabel({0, "  "}, 2) == QLatin1String("3. Chapter 3"), "chapters: label, with a fallback name");
    }

    // ---- which audio stream ---------------------------------------------------------------------------------------
    {
        QVector<JellyfinTrack> s{audio(1, true), audio(2)};
        JellyfinTrack video; video.index = 0; video.type = QStringLiteral("Video"); s.prepend(video);
        check(pickAudioStream(s, -1) == 1, "audio: the server's default stream when nothing is remembered");
        check(pickAudioStream(s, 2) == 2, "audio: a remembered stream the item still has");
        check(pickAudioStream(s, 7) == 1, "audio: a remembered stream that is gone falls back to the default");
        check(pickAudioStream(s, 0) == 1, "audio: a remembered non-audio stream is not taken");
        check(pickAudioStream({audio(4), audio(5)}, -1) == 4, "audio: no default flag picks the first audio stream");
        check(pickAudioStream({video}, -1) == -1, "audio: no audio stream at all is -1");
        check(audioOrdinal(s, 2) == 1 && audioOrdinal(s, 1) == 0 && audioOrdinal(s, 0) == -1 && audioOrdinal(s, 9) == -1, "audio: the ordinal counts audio streams only");
    }

    // ---- the per-title choice -------------------------------------------------------------------------------------
    {
        QJsonObject map;
        check(!choiceFor(map, "jfabc").any(), "choice: nothing remembered reads -2/-2");
        map = withChoice(map, "jfabc", 2, -2, 1000);
        check(choiceFor(map, "jfabc").audio == 2 && choiceFor(map, "jfabc").subtitle == -2, "choice: audio stored, subtitle untouched");
        map = withChoice(map, "jfabc", -2, -1, 1001);
        check(choiceFor(map, "jfabc").audio == 2 && choiceFor(map, "jfabc").subtitle == -1 && choiceFor(map, "jfabc").at == 1001, "choice: subtitle off added, audio kept");
        map = withChoice(map, "jfabc", -2, 4, 1002);
        check(choiceFor(map, "jfabc").subtitle == 4, "choice: a later subtitle replaces the earlier one");
        check(choiceMapValid(map), "choice: what the store writes validates");
        check(withChoice(map, "", 1, 1, 5) == map, "choice: an empty key writes nothing");
        QJsonObject big;
        for (int i = 0; i < 80; ++i) big = withChoice(big, QStringLiteral("k%1").arg(i), i, -2, 5000 + i);
        check(big.size() == kChoiceLimit && !big.contains("k0") && big.contains("k79"), "choice: bounded to 64, least recently changed first out");
        check(choiceMapValid(big), "choice: a full map validates");
        QJsonObject tooMany;
        for (int i = 0; i < 65; ++i) tooMany.insert(QStringLiteral("k%1").arg(i), QJsonObject{{"a", 1}});
        check(!choiceMapValid(tooMany), "choice: 65 entries are refused");
        check(!choiceMapValid(QJsonObject{{"bad key!", QJsonObject{{"a", 1}}}}), "choice: a key with odd characters is refused");
        check(!choiceMapValid(QJsonObject{{"k", QJsonObject{{"x", 1}}}}), "choice: an unknown field is refused");
        check(!choiceMapValid(QJsonObject{{"k", QJsonObject{{"a", 1.5}}}}), "choice: a non-integer is refused");
        check(!choiceMapValid(QJsonObject{{"k", QJsonObject{{"a", 99999}}}}), "choice: an absurd stream index is refused");
        check(!choiceMapValid(QJsonObject{{"k", 7}}), "choice: a non-object record is refused");
        check(choiceMapValid(QJsonObject{}), "choice: the empty map is valid");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
