// Video player control panel model (src/video_panel.h): the console's slot order, the focus walk, the native
// extras slot's position, Go To entry rules, Screen Mode rectangles and the repeat status keys.
#include "video_panel.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace videopanel;
static int failures = 0;
#define CHECK(c) do{if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)
static bool near(double a, double b) { return a - b < 1e-6 && b - a < 1e-6; }

int main() {
    // The container's own order: object order @0xc1e0..0xd740, then the two native slots, last.
    CHECK(kButtons.size() == 27);
    CHECK(kButtons[kDummy].icon == nullptr && kButtons[kDummy].x == -6 && kButtons[kDummy].y == 1);
    CHECK(std::strcmp(kButtons[kPlay].icon, "tex_cp_play") == 0);
    CHECK(std::strcmp(kButtons[kFlashBack].icon, "tex_cp_flash_b") == 0 && std::strcmp(kButtons[kFlashForward].icon, "tex_cp_flash_f") == 0);
    CHECK(std::strcmp(kButtons[kRepeat].icon, "tex_cp_repeat") == 0 && kButtons[kRepeat].x == 0 && kButtons[kRepeat].y == -1);
    int natives = 0;
    for(std::size_t i = 0; i < kButtons.size(); ++i) {
        if(kButtons[i].native) { ++natives; CHECK(int(i) == kExtras || int(i) == kChapters); }
    }
    CHECK(natives == 2);
    CHECK(kChapters == kExtras + 1 && kButtons[kChapters].command == Command::Chapters && kButtons[kChapters].x > kButtons[kExtras].x && kButtons[kChapters].y == kButtons[kRepeat].y);
    CHECK(kButtons[kExtras].command == Command::Extras && kButtons[kExtras].x > kButtons[kRepeat].x && kButtons[kExtras].y == kButtons[kRepeat].y);
    // Titles come from the firmware table: every console slot with art carries a text key, the extras slot none.
    std::set<std::string> icons;
    for(std::size_t i = 1; i < kButtonCount; ++i) {
        const Button& b = kButtons[i];
        if(b.native) { CHECK(b.label == nullptr); continue; }
        CHECK(b.icon && b.label && std::strncmp(b.label, "msg_", 4) == 0 && std::strncmp(b.icon, "tex_cp_", 7) == 0);
        icons.insert(b.icon);
    }
    CHECK(icons.size() == 24);   // 24 visible console icons, not 25
    // Row 1 order (left to right) as traced.
    const char* row1[] = {"tex_cp_scene_search", "tex_cp_jump", "tex_cp_audio", "tex_cp_subtitle", "tex_cp_volume",
                          "tex_cp_av_settings", "tex_cp_screen_mode", "tex_cp_change_icon", "tex_cp_delete", "tex_cp_display"};
    for(int i = 0; i < 10; ++i) CHECK(std::strcmp(kButtons[std::size_t(i + 1)].icon, row1[i]) == 0);
    // Functions this build now has are not marked unavailable; the ones it cannot do still carry their reason.
    CHECK(kButtons[2].command == Command::GoTo && kButtons[2].reason == nullptr);
    CHECK(kButtons[7].command == Command::ScreenMode && kButtons[7].reason == nullptr);
    CHECK(kButtons[1].reason && kButtons[6].reason && kButtons[8].reason && kButtons[9].reason);
    CHECK(kButtons[20].reason && kButtons[21].reason && kButtons[22].reason && kButtons[23].reason);

    // Focus walk: horizontal stays on the row, skips nothing, never lands on the dummy.
    CHECK(moveFocus(kPlay, -1, 0) == 14 && moveFocus(kPlay, 1, 0) == 16);
    CHECK(moveFocus(11, -1, 0) == 11);                      // left end of row 0: stays
    CHECK(moveFocus(1, -1, 0) == 1);                        // the dummy is never a target
    CHECK(moveFocus(kPlay, 0, 1) == 5 || moveFocus(kPlay, 0, 1) == 4 || moveFocus(kPlay, 0, 1) == 3);   // up from x=-2
    CHECK(kButtons[std::size_t(moveFocus(kPlay, 0, 1))].x == -2);
    CHECK(moveFocus(17, 0, -1) == kRepeat);                 // stop (0,0) down: repeat (0,-1)
    CHECK(moveFocus(kRepeat, 1, 0) == kExtras);             // the extras slot is reached from repeat, rightwards
    CHECK(moveFocus(kExtras, -1, 0) == kRepeat);
    CHECK(moveFocus(kExtras, 1, 0) == kExtras);                   // chapters hidden: nothing right of extras
    CHECK(moveFocus(kExtras, 1, 0, true) == kChapters && moveFocus(kChapters, -1, 0, true) == kExtras);
    CHECK(moveFocus(kChapters, 1, 0, true) == kChapters);
    CHECK(moveFocus(kRepeat, 1, 0, true) == kExtras);
    CHECK(moveFocus(kRepeat, 0, -1) == kRepeat);
    // Every visible console slot is reachable from Play.
    std::set<int> seen{kPlay};
    bool grew = true;
    while(grew) {
        grew = false;
        const std::set<int> frontier = seen;
        for(int from : frontier)
            for(int dir = 0; dir < 4; ++dir) {
                const int to = moveFocus(from, dir == 0 ? -1 : dir == 1 ? 1 : 0, dir == 2 ? 1 : dir == 3 ? -1 : 0);
                if(seen.insert(to).second) grew = true;
            }
    }
    CHECK(seen.size() == kButtonCount - 2 && !seen.count(kDummy) && !seen.count(kChapters));   // chapters hidden: never a target
    // With chapter marks the slot joins the walk, and only then.
    std::set<int> withChapters{kPlay};
    for(bool grown = true; grown;) {
        grown = false;
        const std::set<int> frontier = withChapters;
        for(int from : frontier)
            for(int dir = 0; dir < 4; ++dir)
                if(withChapters.insert(moveFocus(from, dir == 0 ? -1 : dir == 1 ? 1 : 0, dir == 2 ? 1 : dir == 3 ? -1 : 0, true)).second) grown = true;
    }
    CHECK(withChapters.size() == kButtonCount - 1 && withChapters.count(kChapters));

    // Chapter selector: Jellyfin video, seekable, two or more server marks. A local file never gets it.
    ChapterContext chapters{true, true, true, 3};
    CHECK(chapterSelectorVisible(chapters));
    CHECK(chapterSelectorVisible({true, true, true, 2}));
    CHECK(!chapterSelectorVisible({true, true, true, 1}) && !chapterSelectorVisible({true, true, true, 0}) && !chapterSelectorVisible({true, true, true, -4}));
    CHECK(!chapterSelectorVisible({false, true, true, 12}));   // local file: no chapter data is faked
    CHECK(!chapterSelectorVisible({true, false, true, 3}) && !chapterSelectorVisible({true, true, false, 3}));
    CHECK(chapterTargetValid(0, 3) && chapterTargetValid(2, 3) && !chapterTargetValid(3, 3) && !chapterTargetValid(-1, 3) && !chapterTargetValid(0, 0));
    CHECK(focusWithout(kChapters, false) == kExtras && focusWithout(kChapters, true) == kChapters && focusWithout(kPlay, false) == kPlay);

    // Go To: opens on the current position, minutes first; fields wrap inside their own range.
    GotoEntry g = GotoEntry::at(3725000, 7200000);   // 1:02:05 of 2:00:00
    CHECK(g.hour == 1 && g.minute == 2 && g.second == 5 && g.field == 1 && g.targetMs() == 3725000);
    g.turn(1); CHECK(g.minute == 3);
    g.move(1); g.turn(-6); CHECK(g.second == 59 && g.minute == 3);   // 5-6 wraps to 59 without carry
    g.move(-2); CHECK(g.field == 0); g.turn(1); CHECK(g.hour == 2); g.turn(1); CHECK(g.hour == 0);   // hours 0..2 wrap
    g.move(-1); CHECK(g.field == 0); g.move(5); CHECK(g.field == 2);
    GotoEntry beyond = GotoEntry::at(0, 90000);       // 1:30 title: hour range is 0 only
    CHECK(beyond.hourLimit() == 0);
    beyond.turn(1); CHECK(beyond.minute == 1 && beyond.targetMs() == 60000 && beyond.inRange());
    beyond.turn(1); CHECK(beyond.minute == 2 && !beyond.inRange());                  // 2:00 > 1:30: msg_error_overlimit_goto
    GotoEntry exact = GotoEntry::at(0, 61000); exact.turn(1); exact.move(1); exact.turn(1); CHECK(exact.targetMs() == 61000 && exact.inRange());
    GotoEntry unknown = GotoEntry::at(5000, -1); unknown.turn(5); CHECK(unknown.inRange());   // no known length: nothing to exceed
    CHECK(GotoEntry::at(-5, 1000).targetMs() == 0);

    // Screen Mode: the four traced names in order, wrapping.
    CHECK(nextScreenMode(ScreenMode::Normal) == ScreenMode::Original && nextScreenMode(ScreenMode::Original) == ScreenMode::FullScreen);
    CHECK(nextScreenMode(ScreenMode::FullScreen) == ScreenMode::Zoom && nextScreenMode(ScreenMode::Zoom) == ScreenMode::Normal);
    CHECK(std::strcmp(screenModeKey(ScreenMode::Normal), "msg_normal") == 0 && std::strcmp(screenModeKey(ScreenMode::Original), "msg_original") == 0);
    CHECK(std::strcmp(screenModeKey(ScreenMode::FullScreen), "msg_full_screen") == 0 && std::strcmp(screenModeKey(ScreenMode::Zoom), "msg_zoom") == 0);
    {   // a 4:3 picture on the 16:9 frame
        const Rect normal = screenModeRect(ScreenMode::Normal, 640, 480, 1920, 1080);
        CHECK(near(normal.h, 1080) && near(normal.w, 1440) && near(normal.x, 240) && near(normal.y, 0));
        const Rect original = screenModeRect(ScreenMode::Original, 640, 480, 1920, 1080);
        CHECK(near(original.w, 640) && near(original.h, 480) && near(original.x, 640) && near(original.y, 300));
        const Rect full = screenModeRect(ScreenMode::FullScreen, 640, 480, 1920, 1080);
        CHECK(near(full.w, 1920) && near(full.h, 1080) && near(full.x, 0) && near(full.y, 0));
        const Rect zoom = screenModeRect(ScreenMode::Zoom, 640, 480, 1920, 1080);
        CHECK(near(zoom.w, 1920) && near(zoom.h, 1440) && near(zoom.x, 0) && near(zoom.y, -180));
        const Rect big = screenModeRect(ScreenMode::Original, 3840, 2160, 1920, 1080);   // never larger than the screen
        CHECK(near(big.w, 1920) && near(big.h, 1080));
        const Rect none = screenModeRect(ScreenMode::Zoom, 0, 0, 1920, 1080);
        CHECK(near(none.w, 1920) && near(none.h, 1080));
    }

    // Repeat status strings follow this build's modes (off / one title / all).
    CHECK(std::strcmp(repeatKey(0), "msg_repeat_off") == 0 && std::strcmp(repeatKey(1), "msg_repeat_title") == 0 && std::strcmp(repeatKey(2), "msg_repeat_on") == 0);
    // The console's own "nothing to choose" sentences for the two selectors.
    CHECK(std::strcmp(noChoiceKey(Command::Audio), "msg_error_no_audio") == 0);
    CHECK(std::strcmp(noChoiceKey(Command::Subtitle), "msg_error_no_subtitle") == 0);
    CHECK(noChoiceKey(Command::Play) == nullptr);

    // Display information bar: Qt/Jellyfin codec names map to the container's own pictures; unknown codecs get none.
    CHECK(std::strcmp(videoCodecArt("H.264"), "tex_vc_mpeg4avc") == 0 && std::strcmp(videoCodecArt("h264"), "tex_vc_mpeg4avc") == 0);
    CHECK(std::strcmp(videoCodecArt("MPEG-2"), "tex_vc_mpeg2") == 0 && std::strcmp(videoCodecArt("Motion JPEG"), "tex_vc_motionjpeg") == 0);
    CHECK(videoCodecArt("H.265") == nullptr && videoCodecArt("av1") == nullptr && videoCodecArt("") == nullptr);
    CHECK(std::strcmp(audioCodecArt("AAC"), "tex_ac_aac") == 0 && std::strcmp(audioCodecArt("EAC3"), "tex_ac_dd") == 0);
    CHECK(std::strcmp(audioCodecArt("MP3"), "tex_ac_mp3") == 0 && std::strcmp(audioCodecArt("Wave"), "tex_ac_lpcm") == 0);
    CHECK(audioCodecArt("FLAC") == nullptr && audioCodecArt("Opus") == nullptr);

    if(failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::puts("video panel tests ok");
    return 0;
}
