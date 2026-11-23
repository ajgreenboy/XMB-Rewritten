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

}  // namespace xmb::toast
