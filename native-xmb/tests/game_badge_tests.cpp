// Game badge: the pure helpers only (opt-out value, draw rule, date line text, line composition, badge rect).
//   xmb-game-badge-tests
#include "game_badge.h"

#include <QCoreApplication>
#include <QTimeZone>
#include <cmath>
#include <cstdio>

using namespace xmb::gameBadge;

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    check(!enabledFor(QStringLiteral("0")), "0 turns the badge off");
    check(enabledFor(QString()) && enabledFor(QStringLiteral("1")) && enabledFor(QStringLiteral("x")), "unset or any other value leaves it on");
    check(shouldDraw(true, true, true), "focused game tile draws");
    check(!shouldDraw(true, true, false), "unfocused game row draws nothing");
    check(!shouldDraw(true, false, true), "non-game row draws nothing");
    check(!shouldDraw(false, true, true), "opt-out draws nothing");

    // 2026-11-21 07:34:00 UTC
    const qint64 t = QDateTime(QDate(2026, 11, 21), QTime(7, 34), QTimeZone::UTC).toSecsSinceEpoch();
    check(dateLine(t, 0, QTimeZone::UTC) == QStringLiteral("21/11/2026  7:34"), "DD/MM/YYYY, hour unpadded");
    check(dateLine(t, 1, QTimeZone::UTC) == QStringLiteral("11/21/2026  7:34"), "MM/DD/YYYY");
    check(dateLine(t, 2, QTimeZone::UTC) == QStringLiteral("2026/11/21  7:34"), "YYYY/MM/DD");
    const qint64 t2 = QDateTime(QDate(2026, 11, 21), QTime(13, 5), QTimeZone::UTC).toSecsSinceEpoch();
    check(dateLine(t2, 0, QTimeZone::UTC) == QStringLiteral("21/11/2026  13:05"), "24 h, minutes padded");
    check(dateLine(0, 0).isEmpty() && dateLine(86400, 0).isEmpty() && dateLine(-5, 0).isEmpty(), "no real date gives no line");

    check(composeLine(QStringLiteral("a"), QStringLiteral("b")) == QStringLiteral("a - b"), "date then rest");
    check(composeLine(QStringLiteral("a"), QString()) == QStringLiteral("a"), "date alone");
    check(composeLine(QString(), QStringLiteral("b")) == QStringLiteral("b"), "rest alone, nothing invented");
    check(composeLine(QString(), QString()).isEmpty(), "both empty");

    const QRectF r = badgeRect(700, 500);
    check(std::abs(r.left() - 1117.75) < 0.01 && std::abs(r.width() - 78) < 0.01 && std::abs(r.height() - 18.75) < 0.01, "badge x/size from the measured frame");
    check(std::abs(r.top() - 519.5) < 0.01, "badge top 19.5 px under the row centre");
    check(std::abs(badgeRect(900, 500).left() - r.left() - 200) < 0.01, "badge follows the label gutter, not the title width");

    if (failures) { std::fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    std::printf("game-badge-tests: %d checks passed\n", checks);
    return 0;
}
