#pragma once
// The game-boot background presets (audit/PIPELINE.md "Game boot (S31)", audit/BEHAVIOUR-GAPS-SPEC.md section 4).
// Pure: no Qt, no GL, so tests/gameboot_background_tests.cpp links it alone.
//
// TRACED (custom_render_plugin.prx event table 0x9cc20..0x9cc4c, anim_gameboot @2520): anim_gameboot fires BG2 at 0 s and
// BG3 at 0.5 s. BG2 runs override/gameboot2 over 0.25 s, BG3 override/gameboot3 over 1.25 s (BG1 0 s, BG4 / BG5 0.5 s
// are fired by other plugins' animations, not by anim_gameboot).
// INFERRED (no RPCS3 reference exists for any of the following; never firmware-verified):
//   - the blend curve of a preset change (linear in time here);
//   - the start state (the clock preset the XMB was already showing) and its level 1;
//   - collapsing a preset to one background level: the mean of BACKGROUND.mnu keys 1 and 2 (the two background
//     colours), so a preset that blacks the background out (gameboot3/4/5: all 0) dims the whole backdrop, ribbon and
//     particles included, by black veil;
//   - when BG1 / BG4 / BG5 would run (not scheduled; their levels are listed so a later trace can schedule them).
#include <algorithm>
#include <array>
#include <cstddef>

namespace xmb::gameboot {

// One BG event: which gameboot preset (1..5) starts at `at` seconds into anim_gameboot and how long its blend lasts.
struct Event { int preset; double at; double seconds; };

// The events anim_gameboot fires. TRACED.
inline constexpr std::array<Event,2> schedule{{{2,0.0,0.25},{3,0.5,1.25}}};

// The backdrop level (1 = the XMB's own background untouched, 0 = black) of gameboot1..5: the mean of BACKGROUND.mnu
// keys "1 RED".."2 BLUE" of override/gameboot<n>, read at run time and given to levelAt. These are the values of the
// 4.93 files (they are only a fallback when the firmware tree has no lines.qrc).
inline constexpr std::array<double,5> fallbackPresetLevel{{0.0022,0.8,0.0,0.0,0.0}};
// The level before the first event: the clock preset the XMB was already showing. INFERRED.
inline constexpr double startLevel=1.0;

struct Blend { int from=0; int to=0; double weight=1; };  // preset numbers (0 = the clock start), weight of `to` in 0..1

// Which preset pair is active at t seconds and how far the move has got. Pure selection: the last event started by t
// is the target and the previous one (or the start) is the source. Linear (INFERRED curve).
inline Blend blendAt(double t) {
    Blend out;
    for(std::size_t i=0;i<schedule.size();++i) {
        if(t<schedule[i].at)break;
        out.from=i?schedule[i-1].preset:0;
        out.to=schedule[i].preset;
        out.weight=schedule[i].seconds>0?std::clamp((t-schedule[i].at)/schedule[i].seconds,0.,1.):1.;
    }
    return out;
}

// Backdrop level at t given each preset's level (index 0 = gameboot1).
inline double levelAt(double t,const std::array<double,5>& presetLevel) {
    const Blend b=blendAt(t);
    const auto level=[&](int preset){return preset>=1 && preset<=5?presetLevel[std::size_t(preset-1)]:startLevel;};
    return std::clamp(level(b.from)+(level(b.to)-level(b.from))*b.weight,0.,1.);
}

// The black veil's opacity over the live backdrop: what is left of the XMB background is 1 - veil.
inline double veilAt(double t,const std::array<double,5>& presetLevel) { return 1.-levelAt(t,presetLevel); }

}
