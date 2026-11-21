// The video player's control panel model: the console's 25-slot button grid (videoplayer_plugin.rco,
// videoplayer_control_panel_page / videoplayer_cp_button_group, audit/VIDEO-PLAYER-PARITY-2026-11-16.md),
// its focus walk, the Go To time entry, Screen Mode and the repeat-status strings. Pure logic, no Qt: the
// view draws it and the shell acts on it, and tests/video_panel_tests.cpp checks the order and the rules.
//
// Provenance. Every slot, cell, icon and label key is TRACED from the container's object tree (offsets
// 0xc1e0..0xd740). The one slot that is not the console's is the final native-extras slot: it sits after every
// console cell, is marked `native`, and is never reachable before a console row in the walk's reading order.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace videopanel {

enum class Command {
    Unavailable, Audio, Subtitle, Volume, Display, Beginning, Next, SeekBack, SeekForward, Play, Pause, Stop,
    Repeat, GoTo, ScreenMode,
    Extras,   // native only: this build's own rows (quality, speed, next episode, shuffle)
    Chapters  // native only: the title's Jellyfin chapter marks, shown only when the item has some
};

struct Button {
    int x, y;                 // RCO grid cell; +y is up
    const char* icon;         // tex_cp_* (the _shadow and _focus variants share the name); null: no art
    const char* label;        // videoplayer_plugin text key
    Command command;
    const char* reason;       // why this build cannot honour it (null: it can)
    bool native;              // true only for the extras slot
};

constexpr std::size_t kButtonCount = 27;
constexpr int kDummy = 0;
constexpr int kPlay = 15;       // the slot the panel opens on
constexpr int kFlashBack = 18;
constexpr int kFlashForward = 19;
constexpr int kRepeat = 24;
constexpr int kExtras = 25;
constexpr int kChapters = 26;   // native, drawn and focusable only while chapterSelectorVisible()

// videoplayer_plugin.rco @0xc1e0..0xd740 in object order. The iconless dummy is kept (TRACED) but never drawn
// or focused. Rows: y=+1 (11 slots, the dummy first), y=0 (13 slots), y=-1 (repeat).
inline constexpr std::array<Button, kButtonCount> kButtons{{
    {-6, 1, nullptr, nullptr, Command::Unavailable, "The firmware dummy slot has no control.", false},
    {-5, 1, "tex_cp_scene_search", "msg_scene_search", Command::Unavailable,
        "Scene thumbnails and scene search are not implemented by this player.", false},
    {-4, 1, "tex_cp_jump", "msg_goto", Command::GoTo, nullptr, false},
    {-3, 1, "tex_cp_audio", "msg_audio_change", Command::Audio, nullptr, false},
    {-2, 1, "tex_cp_subtitle", "msg_subtitle_change", Command::Subtitle, nullptr, false},
    {-1, 1, "tex_cp_volume", "msg_volume_control", Command::Volume, nullptr, false},
    {0, 1, "tex_cp_av_settings", "msg_audio_video_settings", Command::Unavailable,
        "PS3 AV output settings have no equivalent in this video backend.", false},
    {1, 1, "tex_cp_screen_mode", "msg_viewmode", Command::ScreenMode, nullptr, false},
    {2, 1, "tex_cp_change_icon", "msg_icon_change", Command::Unavailable,
        "The PC library has no PS3 video-icon database to update.", false},
    {3, 1, "tex_cp_delete", "msg_del", Command::Unavailable,
        "The media library does not expose file deletion.", false},
    {4, 1, "tex_cp_display", "msg_display_display_with_icon", Command::Display, nullptr, false},
    {-6, 0, "tex_cp_prev", "msg_beginning_with_icon", Command::Beginning, nullptr, false},
    {-5, 0, "tex_cp_next", "msg_next_with_icon", Command::Next, nullptr, false},
    {-4, 0, "tex_cp_search_b", "msg_fast_r_with_icon", Command::SeekBack, nullptr, false},
    {-3, 0, "tex_cp_search_f", "msg_fast_f_with_icon", Command::SeekForward, nullptr, false},
    {-2, 0, "tex_cp_play", "msg_play_with_icon", Command::Play, nullptr, false},
    {-1, 0, "tex_cp_pause", "msg_pause_with_icon", Command::Pause, nullptr, false},
    {0, 0, "tex_cp_stop", "msg_stop", Command::Stop, nullptr, false},
    {1, 0, "tex_cp_flash_b", "msg_flash_r", Command::SeekBack, nullptr, false},
    {2, 0, "tex_cp_flash_f", "msg_flash_f", Command::SeekForward, nullptr, false},
    {3, 0, "tex_cp_slow_b", "msg_slow_r", Command::Unavailable,
        "Qt playback has no implemented reverse slow-motion transport.", false},
    {4, 0, "tex_cp_slow_f", "msg_slow_f", Command::Unavailable,
        "This player has no slow-motion rate control.", false},
    {5, 0, "tex_cp_step_b", "msg_oneframe_r", Command::Unavailable,
        "Qt seeking is not frame-accurate; reverse frame stepping cannot be promised.", false},
    {6, 0, "tex_cp_step", "msg_oneframe_f", Command::Unavailable,
        "Qt seeking is not frame-accurate; frame advance cannot be promised.", false},
    {0, -1, "tex_cp_repeat", "msg_repeat", Command::Repeat, nullptr, false},
    // Not the console's: one cell right of repeat, after every console cell.
    {2, -1, nullptr, nullptr, Command::Extras, nullptr, true},
    // Not the console's either: next to the extras slot, present only for an item that has chapter marks.
    {3, -1, nullptr, nullptr, Command::Chapters, nullptr, true},
}};

// The geometric focus walk the console grid needs: horizontal moves stay on the row and take the nearest
// cell in that direction; vertical moves take the nearest row (the cell closest in x within it). +dy is up.
// The dummy slot (index 0) is never a target, and the chapters slot only when `chapters` is true (the item has
// chapter marks). Returns `current` when nothing lies that way.
inline int moveFocus(int current, int dx, int dy, bool chapters = false) {
    const Button& from = kButtons[std::size_t(std::clamp(current, 0, int(kButtonCount) - 1))];
    int best = -1, score = 10000;
    for(int i = 1; i < int(kButtonCount); ++i) {
        if(i == kChapters && !chapters) continue;
        const Button& candidate = kButtons[std::size_t(i)];
        const int x = candidate.x - from.x, y = candidate.y - from.y;
        if(dx && (y != 0 || x * dx <= 0)) continue;
        if(dy && y * dy <= 0) continue;
        const int distance = dx ? std::abs(x) : std::abs(y) * 20 + std::abs(x);   // 20 > the widest x gap (12)
        if(distance < score) { score = distance; best = i; }
    }
    return best >= 0 ? best : current;
}

// ---- Chapter selector (native: the console's videoplayer_display_chapter_* objects are not drawn) -----------------
// The selector exists only for a title whose server sent chapter marks (Jellyfin MediaSources[].Chapters, or the
// item's Chapters field). A local file has no chapter data this build can read, so it never gets the slot: nothing
// is invented from the file name or from fixed intervals. One mark is not a list to choose from, so two are the
// minimum (the shell's own skip rule needs two as well). Placement and wording are NOT firmware-verified.
struct ChapterContext {
    bool jellyfin = false;     // the open title is a Jellyfin stream
    bool video = false;        // a video is open (not music or a photo)
    bool seekable = false;     // the player can jump to a position
    int chapterCount = 0;      // normalized marks the server sent
};
constexpr int kMinChapters = 2;
inline bool chapterSelectorVisible(const ChapterContext& c) {
    return c.jellyfin && c.video && c.seekable && c.chapterCount >= kMinChapters;
}
inline bool chapterTargetValid(int index, int count) { return index >= 0 && index < count; }
// The focus to keep when the slot is gone (the chapter marks disappeared while it was focused): the extras slot.
inline int focusWithout(int focus, bool chapters) { return focus == kChapters && !chapters ? kExtras : focus; }

// ---- Go To: videoplayer_display_time_jump_hour / _minute / _second (three ItemSpin objects) --------------
// Up and down turn the focused field, left and right move between fields, Cross jumps, Circle cancels. A
// target past the end of the title is the console's msg_error_overlimit_goto. Each field wraps inside its own
// range (a spin, not a counter); the hour range ends at the title's own length. FIT: wrap, the field order and
// the starting field (minutes) are the standard spin behaviour, not traced from the .prx.
struct GotoEntry {
    int hour = 0, minute = 0, second = 0;
    int field = 1;                 // 0 hour, 1 minute, 2 second
    std::int64_t durationMs = 0;

    static GotoEntry at(std::int64_t positionMs, std::int64_t durationMs) {
        GotoEntry entry;
        entry.durationMs = std::max<std::int64_t>(0, durationMs);
        const std::int64_t seconds = std::max<std::int64_t>(0, positionMs) / 1000;
        entry.hour = int(seconds / 3600);
        entry.minute = int(seconds / 60 % 60);
        entry.second = int(seconds % 60);
        return entry;
    }
    int hourLimit() const { return int(durationMs / 3600000); }
    void turn(int delta) {
        const int limit = field == 0 ? hourLimit() + 1 : 60;
        int& value = field == 0 ? hour : field == 1 ? minute : second;
        value = ((value + delta) % limit + limit) % limit;
    }
    void move(int delta) { field = std::clamp(field + delta, 0, 2); }
    std::int64_t targetMs() const { return (std::int64_t(hour) * 3600 + minute * 60 + second) * 1000; }
    // A target strictly beyond the title's length is refused; the last whole second and earlier are fine.
    bool inRange() const { return durationMs <= 0 || targetMs() <= durationMs; }
};

// ---- Screen Mode (videoplayer_cp_viewmode_button; msg_normal / msg_original / msg_full_screen / msg_zoom) ---
// The four names are TRACED strings; what each draws is FIT (the container carries no geometry for them):
// Normal letterboxes, Original shows the picture at its own pixel size (never upscaled, never larger than the
// screen), Full Screen stretches to the screen, Zoom fills it and crops the overflow.
enum class ScreenMode { Normal, Original, FullScreen, Zoom };
inline ScreenMode nextScreenMode(ScreenMode mode) {
    return ScreenMode((int(mode) + 1) % 4);
}
inline const char* screenModeKey(ScreenMode mode) {
    switch(mode) {
    case ScreenMode::Original: return "msg_original";
    case ScreenMode::FullScreen: return "msg_full_screen";
    case ScreenMode::Zoom: return "msg_zoom";
    default: return "msg_normal";
    }
}
struct Rect { double x = 0, y = 0, w = 0, h = 0; };
inline Rect screenModeRect(ScreenMode mode, double frameW, double frameH, double screenW, double screenH) {
    if(frameW <= 0 || frameH <= 0) return {0, 0, screenW, screenH};
    const double fit = std::min(screenW / frameW, screenH / frameH);
    const double fill = std::max(screenW / frameW, screenH / frameH);
    double w = frameW * fit, h = frameH * fit;
    switch(mode) {
    case ScreenMode::Original: { const double s = std::min(1.0, fit); w = frameW * s; h = frameH * s; break; }
    case ScreenMode::FullScreen: w = screenW; h = screenH; break;
    case ScreenMode::Zoom: w = frameW * fill; h = frameH * fill; break;
    case ScreenMode::Normal: break;
    }
    return {(screenW - w) / 2., (screenH - h) / 2., w, h};
}

// ---- Repeat status text (videoplayer_status_text): repeat mode 0 off, 1 one title, 2 all ------------------
inline const char* repeatKey(int mode) {
    return mode == 1 ? "msg_repeat_title" : mode == 2 ? "msg_repeat_on" : "msg_repeat_off";
}

// What a console row's own refusal says. These are the container's strings, used where the console itself
// refuses (nothing to choose), not this build's capability reasons (those stay in Button::reason).
inline const char* noChoiceKey(Command command) {
    return command == Command::Audio ? "msg_error_no_audio" : command == Command::Subtitle ? "msg_error_no_subtitle" : nullptr;
}

// ---- Display: videoplayer_display_infobar_group (codec art) ---------------------------------------------------
// The console's information bar shows the stream's codec as a label picture: tex_vc_* (video) and tex_ac_* (audio)
// from videoplayer_plugin.rco. A codec the console has no picture for gets none (never a borrowed one).
inline std::string codecKey(const std::string& name) {
    std::string key;
    for(char c : name) {
        if(c >= 'A' && c <= 'Z') key += char(c - 'A' + 'a');
        else if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) key += c;
    }
    return key;
}
inline const char* videoCodecArt(const std::string& name) {
    const std::string k = codecKey(name);
    if(k == "h264" || k == "avc" || k == "avc1" || k == "mpeg4avc") return "tex_vc_mpeg4avc";
    if(k == "mpeg1" || k == "mpeg1video") return "tex_vc_mpeg1";
    if(k == "mpeg2" || k == "mpeg2video") return "tex_vc_mpeg2";
    if(k == "mpeg4" || k == "mpeg4part2") return "tex_vc_mpeg4";
    if(k == "divx") return "tex_vc_divx";
    if(k == "vc1") return "tex_vc_vc_1";
    if(k == "mjpeg" || k == "motionjpeg") return "tex_vc_motionjpeg";
    return nullptr;
}
inline const char* audioCodecArt(const std::string& name) {
    const std::string k = codecKey(name);
    if(k == "aac" || k == "aaclc") return "tex_ac_aac";
    if(k == "ac3" || k == "eac3" || k == "dolbydigital") return "tex_ac_dd";
    if(k == "mp3") return "tex_ac_mp3";
    if(k == "mp2" || k == "mpeg" || k == "mpegaudio") return "tex_ac_mpeg";
    if(k == "wma" || k == "wmav2" || k == "wmapro") return "tex_ac_wma";
    if(k == "wave" || k == "pcm" || k == "lpcm" || k == "pcms16le") return "tex_ac_lpcm";
    if(k == "atrac" || k == "atrac3" || k == "atrac3plus") return "tex_ac_atrac";
    return nullptr;
}

} // namespace videopanel
