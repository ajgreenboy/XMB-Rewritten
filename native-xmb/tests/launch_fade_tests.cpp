// Launch cover timing (src/launch_fade.h, audit/GAME-LAUNCH-FADE.md): every path of the state machine on a fake clock.
#include "launch_fade.h"
#include <cmath>
#include <cstdio>
using namespace xmb::launchfade;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static WindowSample win(uint64_t id, WindowMode mode, int w, int h, bool covers) {
    WindowSample s;
    s.present = true; s.id = id; s.mode = mode; s.w = w; s.h = h; s.coversMonitor = covers;
    return s;
}
// Samples `s` every 100 ms from `from` to `to` (exclusive); returns the time of the first sample that left Hold, or -1.
static int64_t feed(Machine& m, int64_t from, int64_t to, const WindowSample& s) {
    for (int64_t t = from; t < to; t += 100) {
        m.sample(t, s);
        if (m.phase() != Phase::Hold) return t;
    }
    return -1;
}

int main() {
    const auto windowed = win(0x10, WindowMode::Windowed, 1280, 720, false);
    const auto borderless = win(0x10, WindowMode::Borderless, 1920, 1080, true);

    {   // After a game boot: opaque at once, the launch is due immediately and exactly once.
        Machine m(Timing::forLaunch(false));
        m.begin(0, true);
        CHECK(m.phase() == Phase::Hold);
        CHECK(m.opacity(0) == 1.0);
        CHECK(m.takeLaunchDue());
        CHECK(!m.takeLaunchDue());
    }
    {   // Without a game boot: fades to black first and starts the title only once opaque.
        Machine m(Timing::forLaunch(false));
        m.begin(1000, false);
        CHECK(m.phase() == Phase::FadeToBlack);
        CHECK(!m.takeLaunchDue());
        CHECK(m.opacity(1000) == 0.0);
        m.tick(1125);
        CHECK(m.opacity(1125) > 0.4 && m.opacity(1125) < 0.6);
        CHECK(!m.takeLaunchDue());
        m.tick(1250);
        CHECK(m.phase() == Phase::Hold);
        CHECK(m.opacity(1250) == 1.0);
        CHECK(m.takeLaunchDue());
    }
    {   // A window that shows and stays put: fade out once it has been stable for stableMs, hand-off once.
        Machine m(Timing::forLaunch(false));
        m.begin(0, true); m.takeLaunchDue();
        m.sample(1000, {});                       // nothing yet
        CHECK(m.phase() == Phase::Hold);
        const int64_t left = feed(m, 2000, 5000, windowed);
        CHECK(left == 2500);                      // first seen at 2000, stable 500 ms later
        CHECK(m.phase() == Phase::FadeFromBlack);
        CHECK(m.reason() == Reason::WindowStable);
        CHECK(m.window().id == 0x10);
        CHECK(m.takeHandoffDue());
        CHECK(!m.takeHandoffDue());
        CHECK(m.fadeOutMs() == 400);
        CHECK(m.opacity(2500) == 1.0);
        CHECK(std::abs(m.opacity(2700) - 0.5) < 1e-9);
        m.tick(2900);
        CHECK(m.phase() == Phase::Done);
        CHECK(m.opacity(2900) == 0.0);
    }
    {   // Shows windowed, then goes fullscreen: the rect/mode change restarts the settle clock.
        Machine m(Timing::forLaunch(false));
        m.begin(0, true);
        CHECK(feed(m, 1000, 1400, windowed) == -1);   // 300 ms windowed, not yet stable
        const int64_t left = feed(m, 1400, 4000, borderless);
        CHECK(left == 1900);
        CHECK(m.window().mode == WindowMode::Borderless);
    }
    {   // A window that is minimised, then flickers away, is never "stable".
        Machine m(Timing::forLaunch(false));
        m.begin(0, true);
        auto minimised = windowed; minimised.mode = WindowMode::Minimized;
        CHECK(feed(m, 0, 2000, minimised) == -1);
        for (int64_t t = 2000; t < 4000; t += 100) m.sample(t, (t / 100) % 3 ? windowed : WindowSample{});
        CHECK(m.phase() == Phase::Hold);
    }
    {   // Exclusive fullscreen: removed without a fade (no layered fade over a surface DWM does not compose).
        Machine m(Timing::forLaunch(false));
        m.begin(0, true);
        const int64_t left = feed(m, 0, 3000, win(0x20, WindowMode::Exclusive, 1920, 1080, true));
        CHECK(left == 500);
        CHECK(m.phase() == Phase::Done);
        CHECK(m.reason() == Reason::WindowStable);
        CHECK(m.takeHandoffDue());
        CHECK(m.opacity(500) == 0.0);
    }
    {   // The process exits while black: out quickly, no hand-off.
        Machine m(Timing::forLaunch(false));
        m.begin(0, true);
        m.processExited(800);
        CHECK(m.phase() == Phase::FadeFromBlack);
        CHECK(m.reason() == Reason::ProcessExited);
        CHECK(m.fadeOutMs() == 150);
        CHECK(!m.takeHandoffDue());
        m.tick(950);
        CHECK(m.phase() == Phase::Done);
    }
    {   // Start failure (waitForStarted failed / FailedToStart).
        Machine m(Timing::forLaunch(false));
        m.begin(0, true);
        m.startFailed(10);
        CHECK(m.reason() == Reason::StartFailed);
        m.tick(200);
        CHECK(m.phase() == Phase::Done);
    }
    {   // A press while holding fades out; a press during the fade to black is ignored (that press chose the title).
        Machine m(Timing::forLaunch(false));
        m.begin(0, false);
        m.press(100);
        CHECK(m.phase() == Phase::FadeToBlack);
        m.tick(250);
        CHECK(m.takeLaunchDue());
        m.press(1000);
        CHECK(m.phase() == Phase::FadeFromBlack);
        CHECK(m.reason() == Reason::Press);
        m.processExited(1050);                    // later events do not change the reason
        CHECK(m.reason() == Reason::Press);
    }
    {   // Never shows a window: the no-window timeout, measured from the launch, then the normal fade.
        Machine m(Timing::forLaunch(false));
        m.begin(0, false);
        m.tick(250); m.takeLaunchDue();
        for (int64_t t = 300; t < 20250; t += 100) m.sample(t, {});
        CHECK(m.phase() == Phase::Hold);
        m.tick(20250);
        CHECK(m.phase() == Phase::FadeFromBlack);
        CHECK(m.reason() == Reason::NoWindowTimeout);
        CHECK(m.fadeOutMs() == 400);
    }
    {   // The scripted timeout override and Steam's longer default.
        CHECK(Timing::forLaunch(false, 4000).noWindowTimeoutMs == 4000);
        CHECK(Timing::forLaunch(true).noWindowTimeoutMs == 45000);
        CHECK(Timing::forLaunch(true).noWindowTimeoutMs > Timing::forLaunch(false).noWindowTimeoutMs);
        CHECK(Timing::forLaunch(true, 3000).noWindowTimeoutMs == 3000);
    }
    {   // Steam: a small launcher window must be stable for the windowed grace; a monitor-covering one for stableMs.
        Machine m(Timing::forLaunch(true));
        m.begin(0, true);
        CHECK(feed(m, 0, 3900, windowed) == -1);
        const int64_t left = feed(m, 3900, 9000, windowed);
        CHECK(left == 4000);
        Machine n(Timing::forLaunch(true));
        n.begin(0, true);
        CHECK(feed(n, 0, 3000, borderless) == 500);
    }
    {   // Hard cap: whatever the inputs, the cover is gone by hardCap() and without a fade.
        Timing t = Timing::forLaunch(false);
        t.noWindowTimeoutMs = 1000000;            // a broken timeout must still not trap the user
        t.hardCapMs = 30000;
        Machine m(t);
        m.begin(0, true);
        for (int64_t s = 0; s < 29900; s += 100) m.sample(s, {});
        CHECK(m.phase() == Phase::Hold);
        m.tick(30000);
        CHECK(m.phase() == Phase::Done);
        CHECK(m.reason() == Reason::HardCap);
        CHECK(m.opacity(30000) == 0.0);
        CHECK(Timing::forLaunch(false).hardCap() >= 20000 + 400);
        CHECK(Timing::forLaunch(true).hardCap() >= 45000 + 400);
    }
    {   // Withdrawn before it was opaque: gone at once, and the launch is still owed exactly once.
        Machine m(Timing::forLaunch(false));
        m.begin(0, false);
        m.abort(50);
        CHECK(m.phase() == Phase::Done);
        CHECK(m.reason() == Reason::Aborted);
        CHECK(m.takeLaunchDue());
        CHECK(!m.takeLaunchDue());
        Machine n(Timing::forLaunch(false));
        n.begin(0, true); n.takeLaunchDue();
        n.abort(100);
        CHECK(n.phase() == Phase::FadeFromBlack);
        CHECK(!n.takeLaunchDue());
    }
    {   // begin() resets a used machine.
        Machine m(Timing::forLaunch(false));
        m.begin(0, true); m.processExited(10); m.tick(1000);
        CHECK(m.phase() == Phase::Done);
        m.begin(5000, true);
        CHECK(m.phase() == Phase::Hold && m.reason() == Reason::None && m.takeLaunchDue());
    }
    {   // Opacity is monotonic through both fades.
        Machine m(Timing::forLaunch(false));
        m.begin(0, false);
        double previous = -1;
        for (int64_t t = 0; t <= 250; t += 10) { m.tick(t); const double o = m.opacity(t); CHECK(o >= previous); previous = o; }
        m.takeLaunchDue();
        feed(m, 300, 2000, borderless);
        CHECK(m.phase() == Phase::FadeFromBlack);
        const int64_t start = 800;
        previous = 2;
        for (int64_t t = start; t <= start + 400; t += 10) { const double o = m.opacity(t); CHECK(o <= previous); previous = o; }
    }
    std::printf("%s (%d failure%s)\n", failures ? "launch-fade tests FAILED" : "launch-fade tests passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
