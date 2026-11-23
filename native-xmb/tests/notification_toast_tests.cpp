// Info-bar message slot and toast geometry: the pure logic only.
//   xmb-notification-toast-tests
#include "notification_toast.h"

#include <QCoreApplication>
#include <cmath>
#include <cstdio>

using namespace xmb::toast;

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
bool near(double a, double b, double eps = .01) { return std::fabs(a - b) <= eps; }
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ---- The log: truthful by construction -----------------------------------------------------------------
    MessageLog log;
    check(log.unread() == 0 && log.isEmpty(), "a fresh log has nothing unread");
    check(badgeText(log.unread()).isEmpty(), "no unread messages draws no slot");
    log.add(QString(), QStringLiteral("detail only"));
    check(log.isEmpty(), "an entry without a title is not recorded");
    log.add(QStringLiteral("Steam"), QStringLiteral("Library refreshed"));
    log.add(QStringLiteral("Jellyfin"), QString());
    check(log.unread() == 2 && log.size() == 2, "two events are two unread");
    check(badgeText(log.unread()) == QStringLiteral("2"), "the badge shows the count");
    check(log.receivedText() == QStringLiteral("Jellyfin\nSteam - Library refreshed"),
          "Received lists newest first, 'title - detail' or the bare title");
    check(log.receivedText(1) == QStringLiteral("Jellyfin"), "Received honours its line limit");
    check(log.markAllRead() == 2 && log.unread() == 0, "reading marks everything read");
    check(log.markAllRead() == 0, "reading twice changes nothing (no redundant redraw)");
    check(log.size() == 2, "read messages stay in the list");
    log.add(QStringLiteral("Spotify"), QStringLiteral("Signed out."));
    check(log.unread() == 1, "a new event after reading is unread again");

    // ---- Capacity and the badge cap ------------------------------------------------------------------------
    MessageLog many;
    for (int i = 0; i < MessageLog::capacity + 25; ++i) many.add(QStringLiteral("e%1").arg(i), QString());
    check(many.size() == MessageLog::capacity, "the log is bounded");
    check(many.all().first().title == QStringLiteral("e25"), "the oldest entries are the ones dropped");
    check(many.unread() == MessageLog::capacity, "the bound counts only what is kept");
    check(badgeText(99) == QStringLiteral("99") && badgeText(100) == QStringLiteral("99+"), "the badge caps at 99+");
    check(badgeText(-3).isEmpty(), "a negative count draws nothing");

    // ---- Toast geometry (1920x1080, measured at 720p x 1.5) --------------------------------------------------
    const Box small = toastBox(118.5);  // "Signed out." measured 79 px at 720p
    check(near(small.right(), 1818.), "the right edge is fixed at 1818 (720p 1212)");
    check(near(small.y, 82.) && near(small.h, 107.), "the box is 107 px tall at y 82 (720p 54.7..126.3)");
    check(near(small.w, 248.5, .6), "the measured 'Signed out.' box is about 248 px wide (720p 165.5)");
    const Box tiny = toastBox(10.);
    check(near(tiny.w, kMinWidth), "a very short text keeps the minimum width");
    check(tiny.x > small.x, "a shorter text means a narrower box, still hung off the right edge");
    const Box wide = toastBox(400.);
    check(near(wide.right(), 1818.) && wide.x < small.x, "a longer text grows the box leftwards");
    const Box huge = toastBox(5000.);
    check(near(huge.w, kMaxWidth), "the box never exceeds its maximum width");
    check(near(maxTextWidth(), kMaxWidth - kTextLeft - kTextRightPad), "the elision budget matches the maximum box");
    check(small.x >= 0 && huge.x >= 0, "the box stays on screen");

    // ---- Icon placement: the measured ink square -------------------------------------------------------------
    const IconRect icon = iconRect(small);
    check(near(icon.side, kIconInkSize), "the icon is the measured 48 px square");
    check(near(icon.x, small.x + kIconInkLeft), "the icon starts 30 px inside the box");
    check(near(icon.y + icon.side / 2., small.y + small.h / 2.), "the icon is centred on the box");
    check(icon.x + icon.side < small.x + kTextLeft, "the icon ends before the text begins");

    // ---- The shared layout (XMB view and in-game overlay draw from it) ---------------------------------------
    const Layout one = layoutToast(118.5, true, false);
    check(near(one.box.w, small.w) && near(one.box.right(), 1818.), "the layout's box is the measured toast box");
    check(near(one.title.x, one.box.x + kTextLeft), "with a glyph the text starts at the measured text left");
    check(near(one.title.y, one.box.y) && near(one.title.h, one.box.h), "a one-line title is centred over the whole box");
    check(near(one.title.x + one.title.w, one.box.right()), "the text row runs to the box's right edge");
    check(near(one.icon.x, iconRect(one.box).x) && near(one.icon.side, kIconInkSize), "the layout carries the icon square");
    const Layout bare = layoutToast(118.5, false, false);
    check(near(bare.title.x, bare.box.x + kIconInkLeft), "without a glyph the text takes the icon's place");
    const Layout two = layoutToast(200., true, true);
    check(two.twoLines && two.detail.y > two.title.y, "a two-line toast stacks the detail under the title");
    check(near(two.title.y + two.title.h, two.detail.y + 2.), "the detail row follows the title row (2 px overlap, as the view drew it)");
    check(two.title.y >= two.box.y && two.detail.y + two.detail.h <= two.box.bottom(), "both rows fit inside the 107 px box");
    check(near((two.title.y - two.box.y), (two.box.bottom() - (two.detail.y + two.detail.h)), 2.1), "the two-line block is vertically centred (rows overlap 2 px)");
    check(kTitleGrey == 255 && kDetailGrey < kTitleGrey, "the detail line is dimmer than the title");
    check(kFadeInMs == 200 && kFadeOutMs == 100, "fade times are the traced 200 ms in / 100 ms out");

    // ---- The overlay window: the box mapped onto a monitor ---------------------------------------------------
    const PixelRect hd = overlayWindow(small, 1., 0., 0.);
    check(hd.x + hd.w == 1818 && hd.y == 82 && hd.h == 107, "at 1080p the overlay window is the box itself");
    const PixelRect qhd = overlayWindow(small, 1440. / 1080., 0., 0.);
    check(qhd.x + qhd.w == 2424 && qhd.y == 109 && qhd.h >= 142 && qhd.h <= 144, "at 1440p the window scales by 4/3 and keeps its right edge");
    const PixelRect ultra = overlayWindow(small, 1., 240., 0.);   // 16:9 area centred on a 2400x1080 monitor
    check(ultra.x + ultra.w == 2058, "a monitor wider than 16:9 shifts the box with the design area's origin");
    const PixelRect half = overlayWindow(small, 720. / 1080., 0., 0.);
    check(half.w * 3 >= int(small.w * 2) && half.w * 3 <= int(small.w * 2) + 6, "a 720p monitor shrinks the window by 2/3");
    const PixelRect fractional = overlayWindow(small, 1.25, 0.4, 0.6);
    check(fractional.x <= small.x * 1.25 + .4 && fractional.x + fractional.w >= small.right() * 1.25 + .4, "the window is rounded outward so the edge is not clipped");

    std::printf("notification-toast: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
