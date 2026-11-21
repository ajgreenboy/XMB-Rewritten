#pragma once
// Pure decisions behind the video player's loading card and chrome legibility (no Qt, unit-tested in
// tests/player_chrome_tests.cpp). Everything here is a DESIGN CHOICE of this build: the firmware's
// videoplayer plugin publishes no loading layout, shadow or scrim numbers, and there are no console
// frames to compare against. Coordinates are 1920x1080 scene pixels (the view scales them for DPI).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

namespace playerchrome {

// A local item is a video by its file suffix, the same list the media library accepts. Needed because the
// player only reports a video track after the file has loaded, and the load card must already be the
// video's, not the music player's.
inline bool isVideoPath(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return false;
    std::string suffix(name.substr(dot + 1));
    for (auto& c : suffix) c = char(std::tolower(static_cast<unsigned char>(c)));
    static constexpr std::string_view videos[] = {"mp4", "mkv", "avi", "mov", "webm", "m4v", "mpg", "mpeg", "wmv", "ts", "m2ts"};
    return std::find(std::begin(videos), std::end(videos), suffix) != std::end(videos);
}

// The loading card (title, one buffering indicator, Back hint) covers the screen until the first video frame
// is on screen, so the sequence is card -> video with the controls, never card -> black + opaque bar -> video.
struct LoadInput {
    bool pendingVideo = false;    // a video item is selected but the player has not reported a video track yet
    bool videoOpen = false;       // the player reports a video track
    bool haveFrame = false;       // a decoded frame is available to draw
    long long elapsedMs = 0;      // time spent in this state
};
// A decoder that reports video but never delivers a frame (audio-only stream with a cover track, a failed
// decode) must not pin the card forever; the normal chrome returns after this long. A pending item is still
// resolving its stream, which has no fixed bound, so it keeps the card.
constexpr long long kDecodeGiveUpMs = 8000;
inline bool loadCardActive(const LoadInput& in) {
    if (in.haveFrame) return false;
    if (in.pendingVideo && !in.videoOpen) return true;
    return in.videoOpen && in.elapsedMs < kDecodeGiveUpMs;
}

// Text shadow for the player's own chrome over video. The XMB default (4,5 px, grey .25) was measured on
// opaque XMB pages; over a bright video frame its ~5 px offset reads as a doubled glyph. A tight, dark
// shadow keeps the lift without the ghost. Offsets are scene pixels, so they scale with DPI like the text.
constexpr double kShadowDx = 1.0;
constexpr double kShadowDy = 1.5;
constexpr double kShadowGrey = 0.06;    // multiplies what is under the glyph: darker than the XMB default .25

// A subtle scrim behind the bottom band (hints, time, title) so light video does not wash them out.
constexpr double kScrimTop = 770.0;
constexpr double kScrimBottom = 1080.0;
constexpr int kScrimMaxAlpha = 165;
// Alpha of the scrim at scene y: 0 above the top, a smooth ease up to the max at the bottom edge.
inline int scrimAlpha(double y) {
    if (y <= kScrimTop) return 0;
    const double t = std::min(1.0, (y - kScrimTop) / (kScrimBottom - kScrimTop));
    const double eased = t * t * (3.0 - 2.0 * t);
    return int(eased * kScrimMaxAlpha + 0.5);
}

// Where the subtitle band's bottom edge sits. Normally above the queue line (y 895); while the control matrix
// is open its backing plate (610..930), name and status row (490..604) own the lower screen, so the cue
// moves above them instead of being drawn through the icons.
constexpr double kSubtitleBottomNormal = 850.0;
constexpr double kSubtitleBottomMatrix = 470.0;
inline double subtitleBandBottom(bool matrixOpen) { return matrixOpen ? kSubtitleBottomMatrix : kSubtitleBottomNormal; }

// A fixed-width clock for the time cells: HH:MM:SS always eight characters, so the left and right labels never
// change width as the position crosses a minute or an hour. Negative (unknown) stays the console's dashes.
inline std::string clockLabel(long long ms) {
    if (ms < 0) return "--:--:--";
    const long long total = ms / 1000;
    const long long h = total / 3600, m = (total / 60) % 60, s = total % 60;
    char buffer[32];
    const auto hours = std::min<long long>(h, 99);
    std::snprintf(buffer, sizeof buffer, "%02lld:%02lld:%02lld", hours, m, s);
    return buffer;
}

}  // namespace playerchrome
