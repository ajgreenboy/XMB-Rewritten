#pragma once
// In-game XMB backdrop decisions (audit/INGAME-BG-CAPTURE.md). Pure: no Qt, no Win32, no clock. Everything the
// view feeds in is a plain value, so tests/ingame_backdrop_tests.cpp walks every rule on a fake clock.
//
//   1. Blank frames: a frame whose box-filtered probe level has no channel above a tiny threshold is a blank frame
//      (a game that blanks or cloaks on focus loss). The tap keeps the last NON-blank frame.
//   2. Window latch: the title's window is latched the first time it holds the foreground, not at the PS press.
//   3. Live vs frozen: the opaque last-good frame is the backdrop on every entry; the DirectComposition live overlay
//      replaces it only while good frames keep arriving, and is dropped when they stop being good.
#include <cstdint>

namespace xmb::ingamebg {

// ---- 1. Blank-frame rejection ---------------------------------------------------------------------------------------
// The probe is a mip level of the frame (a box-filtered ~60x34 image at 1080p/4K), so one bright HUD pixel still lifts a
// texel; a real game scene that is merely dark keeps non-zero texels. True black or an all-but-black fade is "blank".
constexpr int kBlankMaxChannel = 2;

// The mip level whose longest side is at most `targetMax` (64 -> 60x33 at 1080p, 60x33 at 4K): the probe readback is a
// few KB, not a frame.
inline int probeLevel(int width, int height, int targetMax = 64) {
    int longest = width > height ? width : height;
    int level = 0;
    while (longest > targetMax && level < 15) { longest >>= 1; ++level; }
    return level;
}
inline int levelSize(int size, int level) {
    const int s = size >> level;
    return s < 1 ? 1 : s;
}

// BGRA8 probe pixels, `stride` bytes per row. True when no colour channel exceeds `threshold` (alpha is ignored).
inline bool levelIsBlank(const uint8_t* bgra, int width, int height, int stride, int threshold = kBlankMaxChannel) {
    if (!bgra || width <= 0 || height <= 0) return true;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = bgra + static_cast<long long>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = row + x * 4;
            if (p[0] > threshold || p[1] > threshold || p[2] > threshold) return false;
        }
    }
    return true;
}

// ---- 2. Title-window latch ------------------------------------------------------------------------------------------
// What the view knows about the window that currently holds the foreground. `belongsToTitle` is computed by the caller:
// the window's process is in the tracked tree, or (a Steam title, which is not our process) it covers its monitor and is
// not the Steam client itself.
struct WindowFacts {
    uint64_t hwnd = 0;
    uint32_t pid = 0;
    int64_t area = 0;            // pixels
    bool exists = false;
    bool visible = false;
    bool iconic = false;
    bool cloaked = false;
    bool foreground = false;     // it is GetForegroundWindow()
    bool isSelf = false;         // the XMB's own window
    bool belongsToTitle = false;
};

class WindowLatch {
public:
    enum class Action { None, Latched, Released };

    // Called on a slow tick. `foreground` describes the foreground window (default-constructed when there is none);
    // `latchedExists` is IsWindow(latched). The latch is made when a qualifying window of the title first holds the
    // foreground, kept for as long as the title runs and the window exists, replaced only when the window is destroyed
    // or a LARGER window of the same title takes the foreground (a launcher splash followed by the real game window).
    Action observe(bool titleRunning, const WindowFacts& foreground, bool latchedExists) {
        if (!titleRunning) return reset();
        Action result = Action::None;
        if (hwnd_ && !latchedExists) { result = reset(); }
        if (qualifies(foreground) && (!hwnd_ || (foreground.hwnd != hwnd_ && foreground.area > area_))) {
            hwnd_ = foreground.hwnd; pid_ = foreground.pid; area_ = foreground.area;
            return Action::Latched;
        }
        return result;
    }
    // A qualifying window is a real, shown, uncloaked window of the title that holds the foreground and is not us.
    static bool qualifies(const WindowFacts& w) {
        return w.hwnd && w.exists && w.visible && !w.iconic && !w.cloaked && w.foreground && !w.isSelf && w.belongsToTitle;
    }
    Action reset() {
        const bool had = hwnd_ != 0;
        hwnd_ = 0; pid_ = 0; area_ = 0;
        return had ? Action::Released : Action::None;
    }
    uint64_t hwnd() const { return hwnd_; }
    uint32_t pid() const { return pid_; }
    bool latched() const { return hwnd_ != 0; }
private:
    uint64_t hwnd_ = 0;
    uint32_t pid_ = 0;
    int64_t area_ = 0;
};

// ---- 3. Live vs frozen ----------------------------------------------------------------------------------------------
// The DirectComposition overlay shows whatever DWM composes behind it after the game loses focus, so it is only right
// while the game window is still being drawn. Frames keep arriving (and are not blank) = it is. Anything else stays on,
// or returns to, the frozen last-good frame.
constexpr int64_t kLiveSettleMs = 300;       // how long after the XMB is raised frames must keep arriving
constexpr int kLiveMinGoodFrames = 2;        // good frames seen since the raise
constexpr int64_t kLiveMaxFrameAgeMs = 250;  // the newest good frame must be this fresh

struct LiveFacts {
    bool liveEnabled = true;          // XMB_INGAME_LIVE != 0
    bool modeAllowsOverlay = false;   // Windowed or Borderless (DWM composes it); not exclusive or minimised
    bool iconic = false;
    bool cloaked = false;
    int64_t msSinceRaise = 0;
    int goodFramesSinceRaise = 0;
    int64_t msSinceLastGood = -1;     // -1: never
    bool lastFrameBlank = false;      // the newest classified frame was blank
};

inline bool shouldGoLive(const LiveFacts& f) {
    if (!f.liveEnabled || !f.modeAllowsOverlay || f.iconic || f.cloaked) return false;
    if (f.msSinceRaise < kLiveSettleMs) return false;
    if (f.goodFramesSinceRaise < kLiveMinGoodFrames) return false;
    if (f.msSinceLastGood < 0 || f.msSinceLastGood > kLiveMaxFrameAgeMs) return false;
    return !f.lastFrameBlank;
}

// Once live: fall back to the frozen frame when the window can no longer be what is behind the overlay. A game that
// merely stops producing frames (paused, a static menu) keeps its picture, so silence alone does not drop it.
inline bool shouldDropLive(const LiveFacts& f) {
    return f.iconic || f.cloaked || f.lastFrameBlank || !f.modeAllowsOverlay;
}

enum class Backdrop { Legacy, Frozen };
// The first decision, at the moment the XMB is raised: a good frame was ever seen = the opaque frozen frame; none =
// the pre-existing behaviour (live overlay or one blocking capture or plain dark), untouched.
inline Backdrop chooseEntryBackdrop(bool tapHasGoodFrame) { return tapHasGoodFrame ? Backdrop::Frozen : Backdrop::Legacy; }

}  // namespace xmb::ingamebg
