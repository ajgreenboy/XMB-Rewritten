#pragma once
#include <algorithm>
#include <cstddef>
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

// The background ramp and the ribbon's fade-in that run under the wordmark (audit/BOOT-AND-BACKDROP-2026-11-22.md "The ramp under the wordmark").
// On m2 the background is black until about 11.6 s, then brightens near-linearly to its plateau by about 17.6 s while the
// ribbon, faint and warm from 11.1 s, brightens with it; this boot's own background fade (view.cpp: smoothstep of t / 7.5 s, the
// 4.93 BootBG2 trace) is too slow for the first seconds and its ribbon is bright grey from the first frame. With the wordmark
// stage on, the background alpha and the ribbon gain follow m2 instead, on the clock below, and settle at 1 (the existing
// background and ribbon, unscaled) by recordingEnd, well before ShowGUI (5.5 s). A departure from the 4.93-only boot, deliberate
// and switched off with the wordmark (XMB_BOOT_WORDMARK=0 keeps the previous boot exactly).
//
// Clock: one linear map from this boot's seconds to the m2 recording's. The wordmark's first visible moment (m2 11.67 s) lands
// on startSeconds and its last trace (m2 14.84 s) on logoFirstVisibleSeconds, so the ramp is stretched by the same factor over the
// whole stage (the text's hold is the part that gives way, the background has no hold to give).
inline constexpr double recordingTextStart=11.67,recordingTextEnd=14.84;
inline constexpr double recordingPerSecond=(recordingTextEnd-recordingTextStart)/(logoFirstVisibleSeconds-startSeconds);
constexpr double recordingTime(double t) { return recordingTextStart+(t-startSeconds)*recordingPerSecond; }

struct Knot { double at,value; };
// Piecewise-linear through ascending knots, held flat before the first and after the last.
template<std::size_t N> constexpr double piecewise(const Knot (&k)[N],double x) {
    if(x<=k[0].at)return k[0].value;
    for(std::size_t i=1;i<N;++i)
        if(x<k[i].at)return k[i-1].value+(k[i].value-k[i-1].value)*(x-k[i-1].at)/(k[i].at-k[i-1].at);
    return k[N-1].value;
}

// Background alpha (the shader's _Alpha) against m2 seconds. Fitted, not read: the m2 frame's mean luma (top and bottom rows, over its plateau)
// is the target, and the knots were iterated until this renderer's own capture (clock pinned to 09:38, XMB_BOOT_CLOCK) matched it within about 0.03 (alpha is scene radiance before the tone map, and the ribbon's glare adds to it).
inline constexpr Knot backgroundKnots[]={
    {11.4,0.000},{11.6,0.084},{11.8,0.134},{12.0,0.168},{12.2,0.205},{12.4,0.238},{12.6,0.275},{12.8,0.314},{13.0,0.357},{13.2,0.398},{13.4,0.440},{13.6,0.482},{13.8,0.523},{14.0,0.564},{14.2,0.605},{14.4,0.643},{14.6,0.683},{14.8,0.718},{15.0,0.750},{15.2,0.780},{15.4,0.810},{15.6,0.827},{15.8,0.855},{16.0,0.872},{16.2,0.888},{16.4,0.900},{16.6,0.923},{16.8,0.936},{17.0,0.947},{17.6,0.990},{18.2,1.000}};
constexpr double backgroundAlpha(double t) { return piecewise(backgroundKnots,recordingTime(t)); }
// The boot second at which the ramp has finished (alpha 1).
inline constexpr double rampEnd=startSeconds+(18.2-recordingTextStart)/recordingPerSecond;

// Ribbon gain (multiplies the ribbon's additive composite) against this boot's seconds. Our native ribbon is bright grey at t=0
// and dims as coldboot1 gives way to coldboot2; m2's is faint and grows. Fitted so the ribbon's peak (top 1 % of pixels, green
// channel, left half of the frame) follows m2's. It reaches 1 (the native ribbon) at ribbonFullAt and is never above it.
inline constexpr Knot ribbonKnots[]={{0.0,0.00},{0.2,0.14},{0.4,0.24},{0.6,0.33},{0.8,0.43},{1.0,0.60},{1.2,0.77},{1.4,0.92},{1.6,1.00},{1.8,1.00}};
constexpr double ribbonGain(double t) { return piecewise(ribbonKnots,t); }
inline constexpr double ribbonFullAt=1.6;
}
