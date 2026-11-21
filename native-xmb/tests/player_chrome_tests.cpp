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

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("player-chrome tests ok\n");
    return 0;
}
