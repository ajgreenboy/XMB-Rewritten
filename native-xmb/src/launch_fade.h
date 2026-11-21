#pragma once
// The launch cover's timing (audit/GAME-LAUNCH-FADE.md): black over the screen before a title starts, held while the
// title's window settles, faded off once that window is stable. Pure: no Qt, no Win32, the clock is passed in (ms),
// so every path is driven by tests/launch_fade_tests.cpp. launch_cover.cpp feeds it window samples and draws it.
//
// None of these numbers is firmware. The firmware's anim_gameboot ends at 2.8 s (coldboot.h GameBoot, TRACED); what the
// console shows between that and the game's first frame is not in the traced notes. Every default here is a DESIGN
// CHOICE of this build, picked so the hand-off reads as one fade from black into the game.
#include <algorithm>
#include <cstdint>

namespace xmb::launchfade {

enum class Phase {
    Idle,           // nothing covered
    FadeToBlack,    // the cover is coming up; the title is not started yet
    Hold,           // fully black; the title has been asked to start and its window is awaited
    FadeFromBlack,  // the cover is going away
    Done            // gone (same as Idle, but remembers why)
};

enum class Reason {
    None,
    WindowStable,     // a title window was visible with the same rect and mode for the settle time
    ProcessExited,    // the tracked title ended while the cover was up
    StartFailed,      // the title could not be started at all
    Press,            // the user pressed something while waiting
    NoWindowTimeout,  // no stable window inside the timeout: fall back to the old behaviour
    HardCap,          // the absolute limit; the cover is removed without a fade
    Aborted           // the shell withdrew it (e.g. a Steam window that needs the user)
};

// Mirrors game_capture.h GameWindowMode without depending on it.
enum class WindowMode { None, Minimized, Windowed, Borderless, Exclusive };

struct Timing {
    int64_t fadeToBlackMs = 250;     // only when the XMB is not already black (no game boot ran just before)
    int64_t stableMs = 500;          // same window, rect and mode for this long counts as settled
    int64_t fadeFromBlackMs = 400;   // the reveal
    int64_t abortFadeMs = 150;       // exit, start failure, press, withdrawal: quick, so an error page shows at once
    int64_t noWindowTimeoutMs = 20000;
    // > 0: a window that does not cover its monitor only counts once it has been stable this long. Used for Steam,
    // whose launches often show a small launcher or splash window before the game's own monitor-sized one.
    int64_t windowedGraceMs = 0;
    int64_t hardCapMs = 0;           // from begin(); 0 = derived (see hardCap())

    int64_t hardCap() const {
        return hardCapMs > 0 ? hardCapMs : fadeToBlackMs + noWindowTimeoutMs + std::max(fadeFromBlackMs, abortFadeMs) + 2000;
    }
    // Defaults for a launch. timeoutOverrideMs (> 0) replaces the no-window timeout (scripted runs).
    static Timing forLaunch(bool steam, int64_t timeoutOverrideMs = 0) {
        Timing t;
        if (steam) { t.noWindowTimeoutMs = 45000; t.windowedGraceMs = 4000; }
        if (timeoutOverrideMs > 0) t.noWindowTimeoutMs = timeoutOverrideMs;
        return t;
    }
};

struct WindowSample {
    bool present = false;            // a visible top-level window of the title was found
    WindowMode mode = WindowMode::None;
    int x = 0, y = 0, w = 0, h = 0;  // window rect
    bool coversMonitor = false;
    uint64_t id = 0;                 // the window handle
    bool usable() const { return present && id != 0 && w > 0 && h > 0 && mode != WindowMode::None && mode != WindowMode::Minimized; }
    bool same(const WindowSample& o) const {
        return present == o.present && id == o.id && mode == o.mode && x == o.x && y == o.y && w == o.w && h == o.h &&
               coversMonitor == o.coversMonitor;
    }
};

class Machine {
public:
    explicit Machine(Timing timing = {}) : t_(timing) {}

    const Timing& timing() const { return t_; }
    Phase phase() const { return phase_; }
    Reason reason() const { return reason_; }
    bool active() const { return phase_ == Phase::FadeToBlack || phase_ == Phase::Hold || phase_ == Phase::FadeFromBlack; }
    // The window that settled (valid once reason() == WindowStable).
    const WindowSample& window() const { return settled_; }
    // Length of the fade that is running (or ran) out of black.
    int64_t fadeOutMs() const { return fadeOutMs_; }

    // alreadyBlack: the XMB's own game boot has just left the screen black, so the cover is opaque at once.
    void begin(int64_t now, bool alreadyBlack) {
        *this = Machine(t_);
        begin_ = now;
        if (alreadyBlack || t_.fadeToBlackMs <= 0) enterHold(now);
        else { phase_ = Phase::FadeToBlack; phaseAt_ = now; }
    }
    // True exactly once: the cover is opaque, start the title now.
    bool takeLaunchDue() { const bool due = launchDue_; launchDue_ = false; return due; }
    // True exactly once: the fade into a settled window began; give that window the foreground.
    bool takeHandoffDue() { const bool due = handoffDue_; handoffDue_ = false; return due; }

    void startFailed(int64_t now) { if (phase_ == Phase::Hold || phase_ == Phase::FadeToBlack) leave(now, Reason::StartFailed, t_.abortFadeMs); }
    void processExited(int64_t now) { if (phase_ == Phase::Hold) leave(now, Reason::ProcessExited, t_.abortFadeMs); }
    // A press only counts once the title has been asked to start (the press that chose it is not a skip).
    void press(int64_t now) { if (phase_ == Phase::Hold) leave(now, Reason::Press, t_.abortFadeMs); }
    void abort(int64_t now) {
        if (phase_ == Phase::FadeToBlack) { finish(now, Reason::Aborted); return; }   // gone at once; the launch still runs
        if (phase_ == Phase::Hold) leave(now, Reason::Aborted, t_.abortFadeMs);
    }

    void sample(int64_t now, const WindowSample& s) {
        tick(now);
        if (phase_ != Phase::Hold) return;
        if (!s.usable()) { haveLast_ = false; return; }
        if (!haveLast_ || !s.same(last_)) { last_ = s; haveLast_ = true; stableSince_ = now; return; }
        const int64_t need = (t_.windowedGraceMs > 0 && !s.coversMonitor) ? std::max(t_.stableMs, t_.windowedGraceMs) : t_.stableMs;
        if (now - stableSince_ < need) return;
        settled_ = s;
        // An exclusive-fullscreen title is not composed by DWM, so a fading layered window over it shows nothing useful
        // and may knock it out of exclusive mode: the cover is removed at once (DESIGN CHOICE).
        leave(now, Reason::WindowStable, s.mode == WindowMode::Exclusive ? 0 : t_.fadeFromBlackMs);
        handoffDue_ = true;
    }

    void tick(int64_t now) {
        if (!active()) return;
        if (now - begin_ >= t_.hardCap()) { finish(now, Reason::HardCap); return; }
        switch (phase_) {
        case Phase::FadeToBlack:
            if (now - phaseAt_ >= t_.fadeToBlackMs) enterHold(now);
            break;
        case Phase::Hold:
            if (now - phaseAt_ >= t_.noWindowTimeoutMs) leave(now, Reason::NoWindowTimeout, t_.fadeFromBlackMs);
            break;
        case Phase::FadeFromBlack:
            if (now - phaseAt_ >= fadeOutMs_) finish(now, reason_);
            break;
        default:
            break;
        }
    }

    // Cover opacity, 0..1. Both fades are smoothstep in opacity (DESIGN CHOICE: no traced curve exists for this hand-off).
    double opacity(int64_t now) const {
        switch (phase_) {
        case Phase::FadeToBlack: return smooth(progress(now, t_.fadeToBlackMs));
        case Phase::Hold: return 1.0;
        case Phase::FadeFromBlack: return 1.0 - smooth(progress(now, fadeOutMs_));
        default: return 0.0;
        }
    }

    static const char* phaseName(Phase p) {
        switch (p) {
        case Phase::Idle: return "idle";
        case Phase::FadeToBlack: return "fade-to-black";
        case Phase::Hold: return "hold";
        case Phase::FadeFromBlack: return "fade-from-black";
        case Phase::Done: return "done";
        }
        return "?";
    }
    static const char* reasonName(Reason r) {
        switch (r) {
        case Reason::None: return "none";
        case Reason::WindowStable: return "window-stable";
        case Reason::ProcessExited: return "process-exited";
        case Reason::StartFailed: return "start-failed";
        case Reason::Press: return "press";
        case Reason::NoWindowTimeout: return "no-window-timeout";
        case Reason::HardCap: return "hard-cap";
        case Reason::Aborted: return "aborted";
        }
        return "?";
    }

private:
    void enterHold(int64_t now) { phase_ = Phase::Hold; phaseAt_ = now; launchDue_ = true; }
    void leave(int64_t now, Reason why, int64_t fadeMs) {
        reason_ = why;
        fadeOutMs_ = std::max<int64_t>(0, fadeMs);
        if (fadeOutMs_ == 0) { finish(now, why); return; }
        phase_ = Phase::FadeFromBlack; phaseAt_ = now;
    }
    // A cover that goes before it was ever opaque still owes the launch: the title starts exactly once whatever happens.
    void finish(int64_t now, Reason why) {
        if (phase_ == Phase::FadeToBlack) launchDue_ = true;
        phase_ = Phase::Done; phaseAt_ = now; reason_ = why;
    }
    double progress(int64_t now, int64_t length) const {
        if (length <= 0) return 1.0;
        return std::clamp(double(now - phaseAt_) / double(length), 0.0, 1.0);
    }
    static double smooth(double x) { return x * x * (3.0 - 2.0 * x); }

    Timing t_;
    Phase phase_ = Phase::Idle;
    Reason reason_ = Reason::None;
    int64_t begin_ = 0, phaseAt_ = 0, stableSince_ = 0, fadeOutMs_ = 0;
    bool launchDue_ = false, handoffDue_ = false, haveLast_ = false;
    WindowSample last_, settled_;
};

}  // namespace xmb::launchfade
