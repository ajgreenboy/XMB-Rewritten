#pragma once
#include <algorithm>
#include <cstring>
namespace xmb::bootwordmark {
// The "Sony Computer Entertainment" wordmark stage of the older cold boot, timed and placed from the m2 recording
// (1280x720, 200 ms frames; audit/BOOT-AND-BACKDROP-2026-11-22.md "The wordmark stage"). Times are seconds from the moment the background starts
// to brighten (m2 11.7 s); the ribbon and background are the cold boot's own and are not touched here.
//  - fade in: linear, text first visible at 11.67 s, full by 12.39 s (grey-level of 79 stable ink pixels: 41, 101, 162, 203
//    at 11.8 .. 12.4 s, constant after);
//  - hold: full until 14.12 s;
//  - fade out: linear, last trace at 14.84 s (the same ink pixels, measured against the ribbon behind them: 98 %, 88 %, 63 %,
//    37 %, 6 % at 14.0 .. 14.8 s).
// The 4.93 firmware's own tex_scelogo reads "Sony Interactive Entertainment" and is held at alpha 0, so the text is drawn.
inline constexpr double fadeInSeconds=.72;
inline constexpr double holdSeconds=1.73;
inline constexpr double fadeOutSeconds=.72;

// Where it goes in this cold boot. In m2 the wordmark stands in the slot the PS3 logo has in the 4.93 sequence (same
// right-of-centre place) and there is no logo; this boot keeps its logo and every later stage on their own times, so the
// wordmark takes the time before the logo's first visible moment (ghost at 56.6 s on the RPCS3 recording, boot origin
// 54.45 s: 2.1 s) and its hold gives way (fitted()). The measured stage is 3.17 s long; here it runs 0.2 .. 2.1 s.
inline constexpr double logoFirstVisibleSeconds=2.1;
// The stage starts a little after t=0: in m2 the ribbon is already up 0.55 s before the text (ribbon peak 3 at 11.2 s, text at
// 11.67 s), and this boot's ribbon is on screen from its first frame (and flashes bright in the first 0.3 s, see the audit).
inline constexpr double startSeconds=.2;

struct Schedule {
    double start=0,fadeIn=fadeInSeconds,hold=holdSeconds,fadeOut=fadeOutSeconds;
    constexpr double fullAt() const { return start+fadeIn; }
    constexpr double fadeOutAt() const { return start+fadeIn+hold; }
    constexpr double end() const { return start+fadeIn+hold+fadeOut; }
};

// Opacity (0..1) at t seconds: linear in, flat, linear out. Zero outside [start, end].
constexpr double alphaAt(const Schedule& s,double t) {
    if(t<=s.start || t>=s.end())return 0.;
    if(t<s.fullAt())return (t-s.start)/s.fadeIn;
    if(t<=s.fadeOutAt())return 1.;
    return 1.-(t-s.fadeOutAt())/s.fadeOut;
}

// The measured schedule fitted to a window that ends at `limit` seconds (the instant something else takes the screen).
// Only the hold gives way: the fades are measured and keep their length. A window too short for both fades keeps them
// (the stage then overruns `limit`), so the hold is never negative.
constexpr Schedule fitted(double start,double limit) {
    Schedule s;s.start=start;
    s.hold=std::max(0.,std::min(holdSeconds,limit-start-fadeInSeconds-fadeOutSeconds));
    return s;
}

// XMB_BOOT_WORDMARK: on unless set to a value that reads as 0 (0, off, false, no).
inline bool enabledFrom(const char* value) {
    if(!value || !*value)return true;
    return !(std::strcmp(value,"0")==0 || std::strcmp(value,"off")==0 || std::strcmp(value,"false")==0 || std::strcmp(value,"no")==0);
}

// Placement in the 1920x1080 design space (recording x 1.5): the three words' measured ink extents (m2 frame 12.6 s,
// pixels over grey 100: Sony 721..779, Computer 784..904, Entertainment 911..1082 of 1280), the baseline and the cap
// height (cap top 350, baseline 366 of 720). Each word is stretched to its measured width, which fixes the tracking and
// word gaps whatever typeface stands in for the wordmark's own.
struct Word { const char* text;double left,width; };
inline constexpr Word words[3]={{"Sony",1081.5,88.5},{"Computer",1176.,181.5},{"Entertainment",1366.5,258.}};
inline constexpr double baseline=549.,capHeight=24.;
}
