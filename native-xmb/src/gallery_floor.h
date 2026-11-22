#pragma once
// Photo Gallery floor brightness: the six levels of the console's "Floor Brightness" setting and the factor each applies
// to the floor. Pure (no Qt, no GL): tests/gallery_floor_tests.cpp links it alone.
//
// TRACED: hknw_plugin/parameters.txt defines FLOOR_BRIGHTNESS_TYPE0..5 = 1.0, 0.83, 0.66, 0.5, 0.3, 0.1; the file's own
// comments name TYPE0 the standard level and TYPE1 "-1" (TYPE2..5 repeat "-2" from a copied line), and hknw_plugin.prx
// carries the label key msg_floor_brightness. The scene reads the factors from parameters.txt at run time
// (parameterName); fallbackFactor is the same table, used by tests and when the file is not loaded.
// INFERRED: the level names "-3".."-5" for TYPE3..5 (continuing the file's -1 / -2 pattern) and which stored number maps to
// which TYPE (the stored level n is TYPE n).
#include <algorithm>
#include <array>

namespace xmb::galleryfloor {

inline constexpr int levelCount = 6;
inline constexpr int defaultLevel = 0;   // FLOOR_BRIGHTNESS_TYPE0: the standard level, what the gate states render

inline constexpr int clampLevel(int level) { return std::clamp(level, 0, levelCount - 1); }

// The parameters.txt name of a level's factor (an out-of-range level is clamped).
inline constexpr const char* parameterName(int level) {
    constexpr std::array<const char*, levelCount> names{"FLOOR_BRIGHTNESS_TYPE0", "FLOOR_BRIGHTNESS_TYPE1", "FLOOR_BRIGHTNESS_TYPE2",
        "FLOOR_BRIGHTNESS_TYPE3", "FLOOR_BRIGHTNESS_TYPE4", "FLOOR_BRIGHTNESS_TYPE5"};
    return names[std::size_t(clampLevel(level))];
}

// The factor each level multiplies the floor with (values of parameters.txt in 4.93).
inline constexpr double fallbackFactor(int level) {
    constexpr std::array<double, levelCount> factors{1.0, 0.83, 0.66, 0.5, 0.3, 0.1};
    return factors[std::size_t(clampLevel(level))];
}

// The option label of a level: "Normal" then "-1" .. "-5".
inline constexpr const char* levelLabel(int level) {
    constexpr std::array<const char*, levelCount> labels{"Normal", "-1", "-2", "-3", "-4", "-5"};
    return labels[std::size_t(clampLevel(level))];
}

}  // namespace xmb::galleryfloor
