#pragma once
#include <QObject>
#include <QRectF>
#include <QStringList>
#include <QString>
#include <QVector>
#include <algorithm>
#include <cmath>

// The Friends column's visual system and its truthful rules (audit/FRIENDS-COLUMN.md).
//
// This build has NO PlayStation Network connection. A friend row exists only for a person a real source lists (the
// signed-in Discord friend list today), and every decoration below is drawn only from a value that source really
// carries: no dot without a presence value, no speech bubble without real text, no "me" marker without an own row.
// Geometry is measured on the older-firmware recording (m2, 1280x720 frames, +/-1 px) and scaled by 1.5 to the
// 1920x1080 frame. Pure functions, covered by tests/friends_ui_tests.cpp.
namespace xmb::friendsui {

// ---- Avatar tile -------------------------------------------------------------------------------------------------
// m2 measures the TILE (the avatar picture's square): 54 px at 720p for an unfocused friend (m2 26 s, the "B" tile; the
// default smiley ball is a round picture inside the same tile, so its ink reads 36 px), 107 px when focused (m2 42 s)
// and about 163 px with the Triangle panel open (m2 34 s: the ball's ink grows from 73 to 111 px). The brief's "73 px
// focused" is the ball's ink at rest, not the tile. Scaled by 1.5 to the 1080p frame; the unfocused tile is kept at 78 px
// so a column of friends does not overlap on the 80 px row pitch.
constexpr double kAvatarUnfocused=78.;
constexpr double kAvatarFocused=160.;      // 107 x 1.5
constexpr double kAvatarOptionsOpen=244.;  // 163 x 1.5
// The firmware's default avatar is a round picture with a transparent margin: its ink is 0.68 of the tile (m2: ball 36 of 54 px
// unfocused, 73 of 107 focused). This build's default avatar glyph (tex_Avatar_Default) is drawn at that share of the tile; a real
// avatar picture fills the whole tile.
constexpr double kDefaultAvatarInk=.68;
constexpr double kTileRadius=6.;           // the tile's corner rounding (the B tile reads square with a soft edge)

inline double avatarSize(double focus,double optionsOpen=0.) {
    const double f=std::clamp(focus,0.,1.),o=std::clamp(optionsOpen,0.,1.)*f;
    const double rest=kAvatarUnfocused+(kAvatarFocused-kAvatarUnfocused)*f;
    return rest+(kAvatarOptionsOpen-kAvatarFocused)*o;
}

// ---- Label and status dot ----------------------------------------------------------------------------------------
// Offsets from the avatar column's centre (m2: avatar centre x 377, dot centre 456, name left 471, at 720p).
constexpr double kDotOffsetX=118.5;        // (456-377) x 1.5
constexpr double kDotDiameter=21.;         // 14 x 1.5
constexpr double kNameOffsetX=141.;        // (471-377) x 1.5

enum class Presence { Unknown, Offline, Online };

// A dot is drawn only where a real presence value exists. Discord's friend list carries none, so those rows have no dot.
inline bool dotVisible(Presence presence) { return presence!=Presence::Unknown; }
// Blue online, grey-blue offline (sampled from m2 26 s).
inline quint32 dotColour(Presence presence) { return presence==Presence::Online?0xFF1C88D8u:0xFF55697Fu; }

// ---- Speech bubble -----------------------------------------------------------------------------------------------
// m2 42 s: after the row has held focus for about a second, a grey rounded bubble with a tail toward the name sits
// above it (x 471-770, y 258-306 at 720p, white 15 px text). The delay is INFERRED from the 1 fps sampling.
constexpr qint64 kBubbleDelayMs=900;
constexpr double kBubbleLeftOffset=141.;   // (471-377) x 1.5, aligned with the name
constexpr double kBubbleWidth=449.;        // 299 x 1.5
constexpr double kBubbleBottomAboveCentre=48.; // name centre y338 - bubble bottom y306, x 1.5
constexpr double kBubbleLineHeight=24.;
constexpr double kBubblePadding=12.;
constexpr int kBubbleMaxChars=96;

// What the bubble says: the friend's real status text, trimmed and capped. Empty means no bubble at all.
inline QString bubbleText(const QString& statusText) {
    QString text=statusText.simplified();
    if(text.size()>kBubbleMaxChars)text=text.left(kBubbleMaxChars-1)+QChar(0x2026);
    return text;
}
inline bool bubbleVisible(const QString& statusText,qint64 restMs) {
    return !bubbleText(statusText).isEmpty() && restMs>=kBubbleDelayMs;
}
inline double bubbleHeight(int lines) { return kBubblePadding*2+kBubbleLineHeight*std::max(1,lines); }
// The bubble above a friend row whose avatar column centre is (iconX,y).
inline QRectF bubbleRect(double iconX,double y,int lines) {
    const double h=bubbleHeight(lines);
    return {iconX+kBubbleLeftOffset,y-kBubbleBottomAboveCentre-h,kBubbleWidth,h};
}

// Greedy word wrap for the bubble: `width` measures a string in px. A word wider than the line stays whole on its own line.
template<class Measure>
inline QStringList wrapWords(const QString& text,double maxWidth,Measure width) {
    QStringList lines;
    QString line;
    for(const QString& word:text.split(QLatin1Char(' '),Qt::SkipEmptyParts)) {
        const QString trial=line.isEmpty()?word:line+QLatin1Char(' ')+word;
        if(!line.isEmpty() && width(trial)>maxWidth){lines<<line;line=word;}
        else line=trial;
    }
    if(!line.isEmpty())lines<<line;
    return lines;
}

// ---- Selection plate (newer-firmware recording r 73/82/91 s) ----------------------------------------------------------
// A translucent grey-white rounded plate behind the focused friend: x 320-851, y 279-395 at 720p around an avatar
// centre of x 377, y 337. The owner's decision (VIDEO-GAP-ANALYSIS section 9) is that the older look is the target and the
// older recording shows no resting plate, so it is kept off. The right-end star (a PSN level badge) is never drawn.
constexpr bool kRestingPlate=false;
constexpr double kPlateRadius=15.;
inline QRectF plateRect(double iconX,double y) {
    return {iconX-85.5,y-87.,796.5,174.};
}

// ---- The darker band behind the focused row while the Triangle panel is open (m2 33 s: x 305-880, y 255-415) ----------
inline QRectF optionsBandRect(double iconX,double y) {
    return {iconX-108.,y-125.,862.5,240.};
}

// ---- Triangle options for a friend row ---------------------------------------------------------------------------------
// m2 33-35 s: Status (with an arrow), Comment, Profile. Each maps to a real action or says why it cannot.
enum class OptionKind { Status, Comment, Profile };
struct FriendOption {
    OptionKind kind;
    QString title;
    bool enabled;
    bool submenu;
    QString note;
};
inline QVector<FriendOption> friendOptions(bool hasProfilePage,bool presenceKnown) {
    QVector<FriendOption> rows;
    rows.push_back({OptionKind::Status,QObject::tr("Status"),presenceKnown,true,
        presenceKnown?QString():QObject::tr("Online status needs PlayStation Network, which this application does not connect to. Discord's friend list does not share one.")});
    rows.push_back({OptionKind::Comment,QObject::tr("Comment"),false,false,
        QObject::tr("A comment is shown to PlayStation Network friends, and this application does not connect to PlayStation Network.")});
    rows.push_back({OptionKind::Profile,QObject::tr("Profile"),hasProfilePage,false,
        hasProfilePage?QObject::tr("Opens this friend's profile page."):QObject::tr("This friend has no profile page to open.")});
    return rows;
}

} // namespace xmb::friendsui
