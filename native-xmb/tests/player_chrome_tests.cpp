// Video player loading card and chrome legibility decisions (src/player_chrome.h).
#include "player_chrome.h"
#include <cstdio>

using namespace playerchrome;
static int failures = 0;
static double id(double v) { return v; }   // keeps constant-vs-constant checks from tripping /W4 C4127
#define CHECK(c) do{if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)

int main() {
    // A local item is a video by suffix, any case, any separator; music and extensionless names are not.
    CHECK(isVideoPath("C:/media/Cue Test.mp4"));
    CHECK(isVideoPath("/srv/movies/Film.MKV"));
    CHECK(isVideoPath("a.m2ts") && isVideoPath("a.webm") && isVideoPath("a.TS"));
    CHECK(!isVideoPath("song.flac") && !isVideoPath("song.mp3") && !isVideoPath("README") && !isVideoPath("dir.mp4/file"));
    CHECK(!isVideoPath("") && !isVideoPath("."));

    // The card is up from the moment a video is selected until the first frame, and only then.
    CHECK(loadCardActive({true, false, false, 0}));
    CHECK(loadCardActive({true, false, false, 60000}));            // resolving a stream has no fixed bound
    CHECK(loadCardActive({false, true, false, 100}));              // track reported, first frame not yet drawn
    CHECK(!loadCardActive({false, true, true, 100}));              // first frame: controls over video
    CHECK(!loadCardActive({true, false, true, 0}));
    CHECK(!loadCardActive({false, false, false, 0}));              // audio item: the music card, not this one
    CHECK(!loadCardActive({false, true, false, kDecodeGiveUpMs})); // a decoder that never draws does not pin it
    CHECK(loadCardActive({false, true, false, kDecodeGiveUpMs - 1}));

    // Shadow: tight and dark compared with the XMB's 4/5 px default, so glyphs do not read as doubled.
    CHECK(id(kShadowDx) > 0 && id(kShadowDx) <= 2 && id(kShadowDy) > 0 && id(kShadowDy) <= 2 && id(kShadowDy) < 5);
    CHECK(id(kShadowGrey) < 0.25);

    // Scrim: nothing above its top, monotonic, bounded, full strength at the bottom edge.
    CHECK(scrimAlpha(0) == 0 && scrimAlpha(kScrimTop) == 0);
    int previous = 0;
    for (double y = kScrimTop; y <= kScrimBottom; y += 5) {
        const int a = scrimAlpha(y);
        CHECK(a >= previous && a <= kScrimMaxAlpha);
        previous = a;
    }
    CHECK(scrimAlpha(kScrimBottom) == kScrimMaxAlpha && scrimAlpha(5000) == kScrimMaxAlpha);
    CHECK(id(kScrimMaxAlpha) < 255);

    // Subtitle: clear of the queue line normally, and above the matrix's status row (y 490) while it is open.
    CHECK(id(subtitleBandBottom(false)) <= 895 - 20);
    CHECK(id(subtitleBandBottom(true)) <= 490 - 10);
    CHECK(id(subtitleBandBottom(true)) < id(subtitleBandBottom(false)));

    // Clock: eight characters at every position, so the labels keep one width across the minute and hour.
    CHECK(clockLabel(0) == "00:00:00" && clockLabel(59999) == "00:00:59" && clockLabel(60000) == "00:01:00");
    CHECK(clockLabel(3600000) == "01:00:00" && clockLabel(36000000) == "10:00:00");
    for (long long ms = 0; ms < 40000000; ms += 7919) CHECK(clockLabel(ms).size() == 8);
    CHECK(clockLabel(-1) == "--:--:--" && clockLabel(-1).size() == 8);
    CHECK(clockLabel(99LL * 3600000 + 5999999).size() == 8 && clockLabel(500LL * 3600000).size() == 8);

    // Panel plate: solid over the list, gone before the time bar (950) and the hint row (~1040), monotonic.
    CHECK(id(panelPlateAlpha(0)) == 1.0 && id(panelPlateAlpha(kPanelPlateSolidY)) == 1.0);
    CHECK(id(panelPlateAlpha(kPanelPlateEndY)) == 0.0 && id(panelPlateAlpha(1040)) == 0.0 && id(panelPlateAlpha(950)) == 0.0);
    double plate = 1.0;
    for (double y = kPanelPlateSolidY; y <= kPanelPlateEndY; y += 2) { const double a = panelPlateAlpha(y); CHECK(a <= plate && a >= 0.0); plate = a; }
    CHECK(id(kPanelPlateEndY) <= 950 && id(kPanelPlateRamp) > 0);

    // Panel note: under the list while that spot is above the matrix plate, under the heading once it would land in it.
    CHECK(!panelNoteUnderHeading(300, 22));
    CHECK(panelNoteUnderHeading(633, 22));
    CHECK(panelNoteUnderHeading(600, 22) && !panelNoteUnderHeading(931, 22));

    // Native slot label: padded on both sides inside a 70..90 px box, never above the max or below the min.
    CHECK(fitLabelPx(63, 14, 70, 6, 17, 11) == 12);            // "Chapters" at 14 px is 63 wide: 58 inner px -> 12
    CHECK(fitLabelPx(63, 14, 90, 8, 17, 11) == 16);            // widened box: fits at 16 with 8 px each side
    CHECK(fitLabelPx(40, 17, 70, 6, 17, 11) == 17);            // short word keeps the full size
    CHECK(fitLabelPx(0, 17, 70, 6, 17, 11) == 17 && fitLabelPx(63, 0, 70, 6, 17, 11) == 17);
    for (int w = 20; w < 200; w += 7) { const int px = fitLabelPx(w, 14, 90, 8, 17, 11); CHECK(px >= 11 && px <= 17); }

    // Caption follows the focused slot and stays inside the matrix plate.
    CHECK(id(captionCentre(960, 400)) == 960 && id(captionCentre(1260, 400)) == 1260);
    CHECK(id(captionCentre(360, 400)) == 360 && id(captionCentre(150, 400)) == 310 && id(captionCentre(1800, 400)) == 1610);

    // Video queue position: numerals only, none for a single item or a stale index.
    CHECK(videoQueuePosition(0, 5) == "1 / 5" && videoQueuePosition(4, 5) == "5 / 5");
    CHECK(videoQueuePosition(0, 1).empty() && videoQueuePosition(0, 0).empty() && videoQueuePosition(5, 5).empty() && videoQueuePosition(-1, 5).empty());

    // Visible span of the wait bar art: the first and last column that carry ink.
    CHECK(visibleSpan({}).first == -1);
    CHECK(visibleSpan({0, 0, 0}).first == -1);
    CHECK(visibleSpan({0, 0, 200, 255, 90, 0}).first == 2 && visibleSpan({0, 0, 200, 255, 90, 0}).last == 4);
    CHECK(visibleSpan({5, 0, 200}).first == 2);

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("player-chrome tests ok\n");
    return 0;
}
