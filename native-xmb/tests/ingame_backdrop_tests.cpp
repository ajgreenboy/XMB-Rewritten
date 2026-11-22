// In-game backdrop decisions (src/ingame_backdrop.h, audit/INGAME-BG-CAPTURE.md): blank-frame rejection, the title-window
// latch and the live-vs-frozen choice, every rule on plain values.
#include "ingame_backdrop.h"
#include <cstdio>
#include <vector>
using namespace xmb::ingamebg;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static WindowFacts game(uint64_t hwnd, int64_t area = 1920 * 1080) {
    WindowFacts w;
    w.hwnd = hwnd; w.pid = 42; w.area = area; w.exists = w.visible = w.foreground = w.belongsToTitle = true;
    return w;
}

int main() {
    {   // Probe level: a few KB whatever the frame size.
        CHECK(probeLevel(1920, 1080) == 5);   // 60x33
        CHECK(probeLevel(3840, 2160) == 6);   // 60x33
        CHECK(probeLevel(64, 64) == 0);
        CHECK(probeLevel(1, 1) == 0);
        CHECK(levelSize(1080, 5) == 33);
        CHECK(levelSize(1, 5) == 1);
    }
    {   // Blank frames.
        const int w = 8, h = 4, stride = 8 * 4 + 4;
        std::vector<uint8_t> px(size_t(stride) * h, 0);
        CHECK(levelIsBlank(px.data(), w, h, stride));
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;           // opaque black is still blank
        CHECK(levelIsBlank(px.data(), w, h, stride));
        px[size_t(2) * stride + 5 * 4 + 1] = kBlankMaxChannel;            // at the threshold: blank
        CHECK(levelIsBlank(px.data(), w, h, stride));
        px[size_t(2) * stride + 5 * 4 + 1] = kBlankMaxChannel + 1;        // one level above: a real frame
        CHECK(!levelIsBlank(px.data(), w, h, stride));
        px.assign(px.size(), 0);
        px[size_t(3) * stride + 7 * 4 + 2] = 40;                          // a dim pixel in the last texel
        CHECK(!levelIsBlank(px.data(), w, h, stride));
        CHECK(levelIsBlank(nullptr, 4, 4, 16));                           // nothing to look at is not a frame
        CHECK(levelIsBlank(px.data(), 0, 4, 16));
    }
    {   // Latch: made when the title first holds the foreground, not before.
        WindowLatch l;
        WindowFacts none;
        CHECK(l.observe(true, none, false) == WindowLatch::Action::None);
        WindowFacts other = game(7); other.belongsToTitle = false;        // some other application
        CHECK(l.observe(true, other, true) == WindowLatch::Action::None);
        CHECK(!l.latched());
        WindowFacts self = game(8); self.isSelf = true;                   // the XMB itself
        CHECK(l.observe(true, self, true) == WindowLatch::Action::None);
        WindowFacts bg = game(9); bg.foreground = false;                  // the title's window, but not in front
        CHECK(l.observe(true, bg, true) == WindowLatch::Action::None);
        WindowFacts mini = game(10); mini.iconic = true;
        CHECK(l.observe(true, mini, true) == WindowLatch::Action::None);
        WindowFacts cloak = game(11); cloak.cloaked = true;
        CHECK(l.observe(true, cloak, true) == WindowLatch::Action::None);
        WindowFacts zero = game(0);
        CHECK(l.observe(true, zero, true) == WindowLatch::Action::None);  // Steam can report hwnd 0
        CHECK(!l.latched());
        CHECK(l.observe(false, game(12), true) == WindowLatch::Action::None);   // no title running: nothing latches
        CHECK(!l.latched());
        CHECK(l.observe(true, game(12), true) == WindowLatch::Action::Latched);
        CHECK(l.hwnd() == 12 && l.pid() == 42);
    }
    {   // Latch: kept while the title runs, whatever else is in front; replaced only by a larger window of the title.
        WindowLatch l;
        CHECK(l.observe(true, game(20, 640 * 360), true) == WindowLatch::Action::Latched);   // a splash
        CHECK(l.observe(true, game(20, 640 * 360), true) == WindowLatch::Action::None);       // same window again
        WindowFacts desktop = game(99); desktop.belongsToTitle = false;
        CHECK(l.observe(true, desktop, true) == WindowLatch::Action::None);
        CHECK(l.hwnd() == 20);
        WindowFacts self = game(98); self.isSelf = true;
        CHECK(l.observe(true, self, true) == WindowLatch::Action::None);                      // the in-game XMB in front
        CHECK(l.hwnd() == 20);
        CHECK(l.observe(true, game(21, 320 * 200), true) == WindowLatch::Action::None);       // smaller: keep the latch
        CHECK(l.hwnd() == 20);
        CHECK(l.observe(true, game(22, 1920 * 1080), true) == WindowLatch::Action::Latched);  // the real game window
        CHECK(l.hwnd() == 22);
    }
    {   // Latch: released when the window is destroyed (and re-made on the next foreground), and when the title ends.
        WindowLatch l;
        CHECK(l.observe(true, game(30), true) == WindowLatch::Action::Latched);
        WindowFacts none;
        CHECK(l.observe(true, none, false) == WindowLatch::Action::Released);
        CHECK(!l.latched());
        CHECK(l.observe(true, game(31), true) == WindowLatch::Action::Latched);
        CHECK(l.observe(true, game(32), false) == WindowLatch::Action::Latched);              // gone and a new one at once
        CHECK(l.hwnd() == 32);
        CHECK(l.observe(false, none, true) == WindowLatch::Action::Released);                 // the title ended
        CHECK(!l.latched() && l.hwnd() == 0 && l.pid() == 0);
        CHECK(l.observe(false, none, true) == WindowLatch::Action::None);
    }
    {   // Entry: a good frame ever seen = frozen; none = the old behaviour.
        CHECK(chooseEntryBackdrop(true) == Backdrop::Frozen);
        CHECK(chooseEntryBackdrop(false) == Backdrop::Legacy);
    }
    {   // Live: only while good frames keep arriving after the raise.
        LiveFacts f;
        f.modeAllowsOverlay = true; f.msSinceRaise = 320; f.goodFramesSinceRaise = 4; f.msSinceLastGood = 40;
        CHECK(shouldGoLive(f));
        { auto g = f; g.msSinceRaise = 299; CHECK(!shouldGoLive(g)); }          // not settled yet
        { auto g = f; g.goodFramesSinceRaise = 1; CHECK(!shouldGoLive(g)); }    // a lone frame is not "keeps arriving"
        { auto g = f; g.goodFramesSinceRaise = 0; CHECK(!shouldGoLive(g)); }    // paused on blur
        { auto g = f; g.msSinceLastGood = 251; CHECK(!shouldGoLive(g)); }       // stalled
        { auto g = f; g.msSinceLastGood = -1; CHECK(!shouldGoLive(g)); }
        { auto g = f; g.lastFrameBlank = true; CHECK(!shouldGoLive(g)); }      // black on blur
        { auto g = f; g.iconic = true; CHECK(!shouldGoLive(g)); }               // minimised on blur
        { auto g = f; g.cloaked = true; CHECK(!shouldGoLive(g)); }
        { auto g = f; g.modeAllowsOverlay = false; CHECK(!shouldGoLive(g)); }  // exclusive
        { auto g = f; g.liveEnabled = false; CHECK(!shouldGoLive(g)); }        // XMB_INGAME_LIVE=0
    }
    {   // Dropping live: the window stopped being what is behind the overlay. Silence alone keeps the picture.
        LiveFacts f;
        f.modeAllowsOverlay = true; f.msSinceRaise = 2000; f.goodFramesSinceRaise = 40; f.msSinceLastGood = 5000;
        CHECK(!shouldDropLive(f));                                              // a static menu / paused game
        { auto g = f; g.iconic = true; CHECK(shouldDropLive(g)); }
        { auto g = f; g.cloaked = true; CHECK(shouldDropLive(g)); }
        { auto g = f; g.lastFrameBlank = true; CHECK(shouldDropLive(g)); }
        { auto g = f; g.modeAllowsOverlay = false; CHECK(shouldDropLive(g)); }
    }
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("ingame_backdrop: all checks passed\n");
    return 0;
}
