#pragma once

// The focused Game row's "PS3" badge and date line (RPCS3 4.93 captures, Game column): under the focused title a
// small date/time line, and a small grey rounded "PS3" badge at a fixed offset right of the label gutter. Unfocused
// rows show neither. Pure layout and text helpers (no painting, no shell state). The badge is a rendering element
// only; it does not say where a title runs. XMB_GAME_BADGE=0 turns it off.
//
// Measured on 1280x720 frames (frame-00110000 / frame-00112000, scaled x1.5 to the 1920x1080 render space):
// title x 506.5, row centre y 336 (cap height 15 px, 21 px font); date line x 507, digit height about 10 px, centre
// y 355.5; badge ink x 785..837, y 349..361.5 (52 x 12.5; the art is the firmware's explore_plugin_full
// item_tex_ps3format, 110x28).
// The badge x does not follow the title width (a long and a short title put it at the same x).

#include <QDateTime>
#include <QRectF>
#include <QString>
#include <QTimeZone>

namespace xmb::gameBadge {

// "0" turns the badge off; anything else (including unset) leaves it on.
inline bool enabledFor(const QString& envValue) { return envValue != QLatin1String("0"); }
inline bool enabled() { return enabledFor(qEnvironmentVariable("XMB_GAME_BADGE")); }

// Drawn only on the focused row of a game tile.
inline bool shouldDraw(bool lookEnabled, bool gameTile, bool focused) { return lookEnabled && gameTile && focused; }

// "21/11/2026  7:34" (two spaces, as the frame spaces date and time): the row's own date, in the user's date order (0 DD/MM/YYYY, 1 MM/DD/YYYY, 2 YYYY/MM/DD), the
// hour 24 h and unpadded, the minutes padded, as the console prints it. Empty when there is no real date (Unix
// seconds of a day or less mean "never recorded"); a date is never invented.
inline QString dateLine(qint64 unixSeconds, int dateFormat, const QTimeZone& zone = QTimeZone::LocalTime) {
    if (unixSeconds <= 86400) return {};
    const QDateTime when = QDateTime::fromSecsSinceEpoch(unixSeconds, zone);
    const QString pattern = dateFormat == 0 ? QStringLiteral("dd/MM/yyyy  H:mm")
                          : dateFormat == 2 ? QStringLiteral("yyyy/MM/dd  H:mm") : QStringLiteral("MM/dd/yyyy  H:mm");
    return when.toString(pattern);
}

// The one line under the title: the date first, then whatever else the row says (time played), empty parts dropped.
inline QString composeLine(const QString& date, const QString& rest) {
    if (date.isEmpty()) return rest;
    return rest.isEmpty() ? date : date + QStringLiteral(" - ") + rest;
}

// The date line's top, below the row centre (the shared help line uses 19): digits 355.5 against the row's 336 in the frame.
constexpr double dateLineOffset = 14;

// The badge rectangle for a focused row whose label gutter is labelX and whose title centre line is y (render space).
inline QRectF badgeRect(double labelX, double y) {
    constexpr double scale = 1.5;
    constexpr double width = 52 * scale, height = 12.5 * scale;
    return QRectF(labelX + (785 - 506.5) * scale, y + (349 - 336) * scale, width, height);
}

} // namespace xmb::gameBadge
