// The pure logic behind the info bar's message slot and the notification toast, kept free of Qt Widgets and of
// the renderer so a unit test can pin it (tests/notification_toast_tests.cpp).
//
// Two things live here:
//
//   1. MessageLog - the app's OWN message history. Every notification this build raises (Shell::notify) is also
//      recorded here, unread, and the info bar's message icon shows the unread count while it is above zero.
//      There is no PlayStation Network, so no message ever arrives from outside: the log holds only events this
//      application produced itself, and the slot stays hidden when the count is zero. Opening Friends >
//      Message Box > Received reads them (marks them read).
//
//   2. The toast box - the dark rounded box the recordings show for "Signed in." / "Signed out.", measured from
//      the 1280x720 frames of the M2 recording (audit/INFO-BAR.md, "Toast"): box x 1046.5..1212, y 54.7..126.3,
//      fill about #3e3e3e at 0.88 over the background, icon ink 48 px at 1080p, text 79 px wide for "Signed out." at 720p. Everything
//      below is in the 1920x1080 coordinates the view draws in (the 720p measurement times 1.5).
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace xmb::toast {

struct Message {
    QString title;
    QString detail;
    bool read = false;
};

class MessageLog {
public:
    static constexpr int capacity = 50;

    // Newest last. Beyond the capacity the OLDEST entry goes, read or not, so a long-running session cannot
    // grow the list without bound.
    void add(const QString& title, const QString& detail) {
        if (title.isEmpty()) return;
        messages_.push_back(Message{title, detail, false});
        while (messages_.size() > capacity) messages_.removeFirst();
    }
    int unread() const {
        int count = 0;
        for (const auto& message : messages_)
            if (!message.read) ++count;
        return count;
    }
    // Returns how many entries changed state, so a caller can skip a redraw when nothing did.
    int markAllRead() {
        int changed = 0;
        for (auto& message : messages_)
            if (!message.read) {
                message.read = true;
                ++changed;
            }
        return changed;
    }
    int size() const { return messages_.size(); }
    bool isEmpty() const { return messages_.isEmpty(); }
    const QVector<Message>& all() const { return messages_; }

    // The Received list as one text block: newest first, at most `limit` lines, "title - detail" per line.
    QString receivedText(int limit = 20) const {
        QStringList lines;
        for (int i = messages_.size() - 1; i >= 0 && lines.size() < limit; --i) {
            const auto& message = messages_.at(i);
            lines << (message.detail.isEmpty() ? message.title : message.title + QStringLiteral(" - ") + message.detail);
        }
        return lines.join(QLatin1Char('\n'));
    }

private:
    QVector<Message> messages_;
};

// The text beside the message icon (the RCO's online_num-style count). Empty means "draw no slot at all".
inline QString badgeText(int unread) {
    if (unread <= 0) return {};
    if (unread > 99) return QStringLiteral("99+");
    return QString::number(unread);
}

// ---- The toast box (1920x1080) ---------------------------------------------------------------------------
struct Box {
    double x = 0, y = 0, w = 0, h = 0;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
};

inline constexpr double kBoxRight = 1818.;       // 720p 1212 x 1.5
inline constexpr double kBoxTop = 82.;           // 720p 54.7 x 1.5
inline constexpr double kBoxHeight = 107.;       // 720p 71.6 x 1.5
inline constexpr double kBoxRadius = 5.;         // 720p corner ramp ~3 px
inline constexpr double kIconInkLeft = 30.;      // box left to the faces' ink, 720p 20 x 1.5
inline constexpr double kIconInkSize = 51.;      // square the drawImage fit places the glyph in; its ink then measures 46.5 (720p 31 x 1.5)
inline constexpr double kTextLeft = 110.;        // box left to the first text glyph, 720p 73 x 1.5
inline constexpr double kTextRightPad = 20.;     // last glyph to the box's right edge, 720p 13 x 1.5
inline constexpr double kTextSize = 21.;         // fitted so "Signed out." spans the measured 118.5 px (720p 79); the console face is narrower
inline constexpr double kDetailSize = 17.;
inline constexpr double kMinWidth = 248.;        // the measured "Signed out." box
inline constexpr double kMaxWidth = 640.;
inline constexpr int kFillRed = 62, kFillGreen = 62, kFillBlue = 62;
inline constexpr double kFillAlpha = .88;

// The box for a text block `textWidth` px wide (the wider of the title and the detail line). The right edge is
// fixed, so a longer text grows the box leftwards; a text wider than the maximum is the caller's to elide.
inline Box toastBox(double textWidth) {
    const double width = std::clamp(kTextLeft + textWidth + kTextRightPad, kMinWidth, kMaxWidth);
    return Box{kBoxRight - width, kBoxTop, width, kBoxHeight};
}
// The widest text the box can carry at its maximum width.
inline double maxTextWidth() { return kMaxWidth - kTextLeft - kTextRightPad; }

// Where the toast's own icon is drawn: the measured 48 px square, centred on the box vertically. The view's
// drawImage already fits a texture's visible (alpha) bounds into the rectangle it is given, so this rectangle is
// the INK square, not the 100x100 texture.
struct IconRect {
    double x = 0, y = 0, side = 0;
};
inline IconRect iconRect(const Box& box) {
    return IconRect{box.x + kIconInkLeft, box.y + box.h / 2. - kIconInkSize / 2., kIconInkSize};
}

inline constexpr int kTitleGrey = 255;           // title ink (white)
inline constexpr int kDetailGrey = 214;          // detail line ink
inline constexpr double kLineGap = 6.;           // extra height each text row carries below its font size
inline constexpr double kSlideDistance = 48.;   // the XMB toast slides in from the right by this much while it fades
inline constexpr int kFadeInMs = 200;            // the shared mode-5 move time (View::moveMilliseconds)
inline constexpr int kFadeOutMs = 100;           // the firmware's own hide fade (View::notificationHideMilliseconds)

// While a toast is up the console shows no info bar at all (recording R, frames 68-72: plate, clock, face and the
// message slot are absent for the toast's whole life and back on the next 1 fps frame). The bar's opacity is the
// toast's own fade (`toastAmount`, 0..1) turned around, so the two cross-fade in step; `toastDrawn` is false when the
// toast is not being drawn (empty, or the PS menu is up), and the bar is then untouched.
inline double barOpacity(double toastAmount, bool toastDrawn) {
    if (!toastDrawn) return 1.;
    return 1. - std::clamp(toastAmount, 0., 1.);
}

// The toast's text shadow, measured on M2 frame 51 s at 1280x720 (audit/INFO-BAR.md "Toast"): under the full stop
// (ink rows 94-95) the shadow is darkest on the 2 rows straight below it, luma 45 over a 63 background (x0.71) and
// back to the background by the 6th row; beside the stem of the "i" it is 40 (x0.63). That is a soft shadow about
// 2 px at 720p = 3 px at 1080p, right and down, with about 1 px of blur - not the firmware Text template's hard 4/5 px
// copy, which read as doubled text beside this measurement. A blurred shadow is drawn as four taps around the offset.
inline constexpr double kShadowOffset = 3.;      // design px, right and down (720p 2 x 1.5)
inline constexpr double kShadowSpread = .75;     // each tap sits this far from the offset on both axes (blur ~1.1 px)
inline constexpr double kShadowMultiply = .63;   // the darkest the shadow makes the box under it (measured 0.63 .. 0.71)
inline constexpr int kShadowTaps = 4;
struct ShadowTap {
    double dx = 0, dy = 0;
};
inline ShadowTap shadowTap(int index) {
    const double sx = (index & 1) ? 1. : -1., sy = (index & 2) ? 1. : -1.;
    return ShadowTap{kShadowOffset + sx * kShadowSpread, kShadowOffset + sy * kShadowSpread};
}
// Per-tap coverage so that where all taps overlap the box is multiplied by kShadowMultiply: (1 - a)^taps = multiply.
inline double shadowTapCoverage() {
    return 1. - std::pow(kShadowMultiply, 1. / kShadowTaps);
}

struct TextRect {
    double x = 0, y = 0, w = 0, h = 0;
};
// Everything a surface needs to draw one toast, in 1920x1080 coordinates: the box, the glyph square (when the event
// has a glyph) and the one or two text rows. The XMB view and the in-game overlay window both draw from this, so
// the two cannot drift apart.
struct Layout {
    Box box;
    bool hasIcon = false;
    bool twoLines = false;
    IconRect icon;
    TextRect title, detail;   // detail stays empty for a one-line toast
};
inline Layout layoutToast(double textWidth, bool hasIcon, bool twoLines) {
    Layout out;
    out.box = toastBox(textWidth);
    out.hasIcon = hasIcon;
    out.twoLines = twoLines;
    out.icon = iconRect(out.box);
    const double left = out.box.x + (hasIcon ? kTextLeft : kIconInkLeft);
    const double width = out.box.right() - left;
    if (twoLines) {
        const double block = kTextSize + kLineGap + kDetailSize + kLineGap;
        const double top = out.box.y + (out.box.h - block) / 2.;
        out.title = TextRect{left, top, width, kTextSize + kLineGap};
        out.detail = TextRect{left, top + kTextSize + 4., width, kDetailSize + kLineGap};
    } else {
        out.title = TextRect{left, out.box.y, width, out.box.h};
    }
    return out;
}

// The in-game overlay is a window the size of the box alone. `scale` is the monitor's design-to-pixel factor
// (DesignSpace::x) and (originX, originY) the design area's top-left on that monitor in pixels. The result is the
// window rectangle in monitor pixels, rounded OUTWARD so the box's antialiased edge is never clipped.
struct PixelRect {
    int x = 0, y = 0, w = 0, h = 0;
};
inline PixelRect overlayWindow(const Box& box, double scale, double originX, double originY) {
    const int left = int(std::floor(originX + box.x * scale));
    const int top = int(std::floor(originY + box.y * scale));
    const int right = int(std::ceil(originX + box.right() * scale));
    const int bottom = int(std::ceil(originY + box.bottom() * scale));
    return PixelRect{left, top, right - left, bottom - top};
}

}  // namespace xmb::toast
