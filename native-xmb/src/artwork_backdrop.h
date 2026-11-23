#pragma once
#include <algorithm>
namespace xmb::backdrop {
// The focused game row's PIC1 backdrop, timed from the RPCS3 recording of the 4.x XMB (audit/BOOT-AND-BACKDROP-2026-11-22.md,
// 33 ms and 100 ms frame samples of two game rows, GTA V and Black Ops II):
//  - dwell: the first visible change comes 1.2 s after the focus change begins (GTA V: category move first visible at
//    83.50 s, backdrop first changed at 84.73 s; Black Ops II: row change at 110.5 s, first change by 111.7 s);
//  - fade in: linear, 200 ms (mean RGB of a PIC1 region steps 9, 27, 46, 64, 83, 100 % over six 33 ms frames);
//  - fade out: gone within one 33 ms sample (full at 85.066 s, none at 85.100 s), the frame the next column starts to
//    move, so it is a cut: 33 ms here.
// The Jellyfin page backdrop rides the same slot and keeps these constants (JELLYFIN-NATIVE.md).
inline constexpr long long dwellMs=1200;
inline constexpr double fadeInMs=200.;
inline constexpr double fadeOutMs=33.;

// True once the focus has rested long enough for the backdrop to start fading in.
constexpr bool dwellElapsed(long long focusMs) { return focusMs>=dwellMs; }

// One frame of the backdrop's opacity: up when it should show, down otherwise, linear, clamped to 0..1.
constexpr double step(double opacity,bool show,double dtSeconds) {
    const double stepSize=dtSeconds*1000./(show?fadeInMs:fadeOutMs);
    return std::clamp(opacity+(show?stepSize:-stepSize),0.,1.);
}

// The opacity of a backdrop that is leaving (the previous row's art, kept while it fades out).
constexpr double departingStep(double opacity,double dtSeconds) {
    return std::max(0.,opacity-dtSeconds*1000./fadeOutMs);
}
}
