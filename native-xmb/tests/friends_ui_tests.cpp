// Friends column visual rules and truthful gating: the pure logic only (src/friends_ui.h).
//   xmb-friends-ui-tests
#include "friends_ui.h"

#include <QCoreApplication>
#include <cstdio>

using namespace xmb::friendsui;

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
bool near(double a, double b, double eps = .01) { return std::abs(a - b) <= eps; }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // Avatar tile: 54 px at 720p unfocused (kept 78 at 1080p), 107 focused, 163 with the options panel open.
    check(near(avatarSize(0), 78.), "unfocused tile is 78 px");
    check(near(avatarSize(1), 160.), "focused tile is 160 px");
    check(near(avatarSize(1, 1), 244.), "focused tile with the options panel open is 244 px");
    check(near(avatarSize(0, 1), 78.), "an unfocused tile ignores the options panel");
    check(near(avatarSize(.5), 119.), "half focus is halfway");
    check(near(avatarSize(2, 3), 244.) && near(avatarSize(-1, -1), 78.), "inputs are clamped");

    check(near(avatarSize(0) * kDefaultAvatarInk, 53.04) && near(avatarSize(1) * kDefaultAvatarInk, 108.8), "default avatar ink matches the measured ball (54 and 73 px at 720p, x1.5)");

    // Status dot: only where a real presence value exists.
    check(!dotVisible(Presence::Unknown), "no presence value: no dot (Discord's friend list carries none)");
    check(dotVisible(Presence::Online) && dotVisible(Presence::Offline), "a real value draws a dot");
    check(dotColour(Presence::Online) != dotColour(Presence::Offline), "online and offline differ");
    check(((dotColour(Presence::Online) >> 16) & 0xFF) < (dotColour(Presence::Online) & 0xFF), "online is blue");
    check(near(kDotDiameter, 21.), "dot is 14 px at 720p");

    // Speech bubble: real text only, and only after the row has rested.
    check(!bubbleVisible(QString(), 5000), "no status text: no bubble");
    check(!bubbleVisible(QStringLiteral("   \n\t "), 5000), "blank status text: no bubble");
    check(!bubbleVisible(QStringLiteral("Away"), 100), "text but not rested yet: no bubble");
    check(bubbleVisible(QStringLiteral("Away"), kBubbleDelayMs), "text and rested: bubble");
    check(bubbleText(QStringLiteral("  a \n b  ")) == QStringLiteral("a b"), "whitespace is collapsed");
    const QString longText(500, QLatin1Char('x'));
    check(bubbleText(longText).size() == kBubbleMaxChars && bubbleText(longText).endsWith(QChar(0x2026)), "long text is capped with an ellipsis");
    const auto one = bubbleRect(565., 505., 1), two = bubbleRect(565., 505., 2);
    check(near(one.left(), 565. + 141.) && near(one.width(), 449.), "bubble starts at the name and is 449 px wide");
    check(near(one.bottom(), 505. - 48.) && near(two.bottom(), one.bottom()), "bubble bottom is fixed above the row");
    check(two.height() > one.height() && two.top() < one.top(), "a second line grows the bubble upward");

    // Word wrap: greedy, never breaks a word, never returns an empty line.
    const auto chars = [](const QString& s) { return double(s.size()) * 10.; };
    check(wrapWords(QStringLiteral("aa bb cc"), 1000., chars) == QStringList{QStringLiteral("aa bb cc")}, "fits on one line");
    check(wrapWords(QStringLiteral("aaaa bbbb cccc"), 90., chars) == (QStringList{QStringLiteral("aaaa bbbb"), QStringLiteral("cccc")}), "wraps at the last word that fits");
    check(wrapWords(QStringLiteral("abcdefghijkl x"), 50., chars) == (QStringList{QStringLiteral("abcdefghijkl"), QStringLiteral("x")}), "an over-long word stays whole");
    check(wrapWords(QStringLiteral("  "), 50., chars).isEmpty(), "blank text has no lines");

    // Plate and band geometry (measured; the resting plate stays off).
    check(!kRestingPlate, "the newer firmware's resting plate is not drawn (older look is the target)");
    const auto plate = plateRect(565., 505.);
    check(near(plate.left(), 479.5) && near(plate.width(), 796.5) && near(plate.height(), 174.) && near(plate.center().y(), 505.), "plate rect");
    const auto band = optionsBandRect(565., 505.);
    check(near(band.left(), 457.) && near(band.center().y(), 500.) && near(band.width(), 862.5), "options band rect");

    // Triangle options for a friend row: Status / Comment / Profile, each real or explained.
    const auto discord = friendOptions(true, false);
    check(discord.size() == 3 && discord[0].kind == OptionKind::Status && discord[1].kind == OptionKind::Comment && discord[2].kind == OptionKind::Profile, "order is Status, Comment, Profile");
    check(!discord[0].enabled && discord[0].note.contains(QStringLiteral("PlayStation Network")), "Status without presence is disabled and says why");
    check(discord[0].submenu && !discord[1].submenu && !discord[2].submenu, "only Status carries the arrow");
    check(!discord[1].enabled && discord[1].note.contains(QStringLiteral("PlayStation Network")), "Comment is disabled and says why (no PSN)");
    check(discord[2].enabled, "Profile opens a real page when the friend has one");
    check(!friendOptions(false, false)[2].enabled && !friendOptions(false, false)[2].note.isEmpty(), "no profile page: Profile disabled with a reason");
    check(friendOptions(true, true)[0].enabled && friendOptions(true, true)[0].note.isEmpty(), "Status is available when presence is real");
    for (const auto& row : discord) check(!row.title.isEmpty(), "every row has a title");

    std::printf("friends-ui tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
