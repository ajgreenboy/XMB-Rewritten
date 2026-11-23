#pragma once

// The on-screen keyboard's pad extras (audit/BEHAVIOUR-GAPS-SPEC.md section 3, audit/OSK-FIRMWARE.md section 6):
// L1/R1 move the caret, L2 the accent page, R2 the caps page, SELECT is Input Mode, and the panel fades in and out.
// Pure and header-only so tests/osk_pad_input_tests.cpp covers it without a window or a controller.

#include "osk_layout.h"

#include <QVector>
#include <algorithm>
#include <cmath>

namespace xmb {

// XInput-layout button bits, as pad.h documents them.
constexpr quint16 oskPadSelect = 0x0020;
constexpr quint16 oskPadL1 = 0x0100;
constexpr quint16 oskPadR1 = 0x0200;

// L2/R2 arrive as analog trigger values. A press is a rise through 0.5 (the deadzone pollController already uses for
// the sticks); the latch re-arms once the trigger falls below 0.25, so a trigger resting near the threshold does not
// chatter. The two numbers are a fit, not a console measurement.
constexpr float oskTriggerPress = 0.5f;
constexpr float oskTriggerRelease = 0.25f;
struct OskTriggerLatch {
    bool l2 = false, r2 = false;
};
inline bool oskTriggerEdge(float value, bool& latched) {
    if (latched) {
        if (value < oskTriggerRelease) latched = false;
        return false;
    }
    if (value >= oskTriggerPress) {
        latched = true;
        return true;
    }
    return false;
}

// The key actions the pad's extra buttons run, in a fixed order (L1, R1, L2, R2, SELECT) when several land in one poll.
// `edge` is the buttons newly pressed this poll; l2/r2 are the raw trigger values. Every one is an action the on-screen
// keys already dispatch, so the badges and the pad agree.
inline QVector<OskAction> oskPadActions(quint16 edge, float l2, float r2, OskTriggerLatch& latch) {
    QVector<OskAction> actions;
    if (edge & oskPadL1) actions.push_back(OskAction::CaretLeft);
    if (edge & oskPadR1) actions.push_back(OskAction::CaretRight);
    if (oskTriggerEdge(l2, latch.l2)) actions.push_back(OskAction::Accent);
    if (oskTriggerEdge(r2, latch.r2)) actions.push_back(OskAction::Shift);
    if (edge & oskPadSelect) actions.push_back(OskAction::Mode);
    return actions;
}

// The page an Accent or Shift action lands on, or -1 when it changes nothing. The four character pages are two
// independent switches: Shift (R2, the `a/A` key) toggles capital, Accent (L2, the `a/a-grave` key) toggles accent, so
// lower -> accent -> lower, caps -> accentCaps -> caps, and Shift keeps whichever accent state the page is in.
// The page set and the four module row groups are TRACED (audit/OSK-FIRMWARE.md 3.4; the module names both
// MODE_GUIDE_ALPHABET_ACCENT and MODE_GUIDE_ALPHABET_ACCENT_CAP); that L2 returns to the page it came from and that
// the capital state survives the switch is INFERRED from the two switches the key faces show, not captured.
// A layout without an accent page leaves Accent inert (-1). A layout whose pages are not named lower/caps (the
// tenkey panel) falls back to the plain next-page step for Shift.
inline int oskPageIndex(const QVector<OskPage>& pages, const QString& name) {
    for (int i = 0; i < pages.size(); ++i)
        if (pages.at(i).name.compare(name, Qt::CaseInsensitive) == 0) return i;
    return -1;
}
inline int oskPageForAction(const QVector<OskPage>& pages, int current, OskAction action) {
    const int lower = oskPageIndex(pages, QStringLiteral("lower"));
    const int caps = oskPageIndex(pages, QStringLiteral("caps"));
    const int accent = oskPageIndex(pages, QStringLiteral("accent"));
    const int accentCaps = oskPageIndex(pages, QStringLiteral("accentCaps"));
    if (action == OskAction::Accent) {
        if (accent < 0) return -1;
        if (current == accent) return lower >= 0 ? lower : 0;
        if (current == accentCaps) return caps >= 0 ? caps : (lower >= 0 ? lower : 0);
        if (current == caps && accentCaps >= 0) return accentCaps;
        return accent;
    }
    if (action == OskAction::Shift) {
        if (lower < 0 || caps < 0) return pages.size() > 1 ? (current + 1) % int(pages.size()) : -1;
        if (accentCaps >= 0 && accent >= 0) {
            if (current == accent) return accentCaps;
            if (current == accentCaps) return accent;
        }
        return current == caps ? lower : caps;
    }
    return -1;
}

// The panel's fade. OSK-FIRMWARE 13.2 measured the open ramp at about 66-96 ms and the close fade at about 104 ms from
// 45-47 fps title-bar luma bursts. Those durations are audit-derived, not firmware-verified, and the easing curve was
// not separated from the samples, so the ramp is linear. The open value is the midpoint of the measured range.
constexpr int oskOpenFadeMilliseconds = 80;
constexpr int oskCloseFadeMilliseconds = 104;
// Panel alpha `elapsed` ms after it opened (opening) or after it closed (!opening): 0..1 rising or 1..0 falling.
inline double oskFadeAlpha(qint64 elapsed, bool opening) {
    const double span = opening ? oskOpenFadeMilliseconds : oskCloseFadeMilliseconds;
    const double t = std::clamp(double(elapsed) / span, 0., 1.);
    return opening ? t : 1. - t;
}
inline bool oskFadeDone(qint64 elapsed, bool opening) {
    return elapsed >= (opening ? oskOpenFadeMilliseconds : oskCloseFadeMilliseconds);
}

// The recorded panel also scales (audit/OSK-RECORDED-SPEC-2026-11-22.md 7): the first open frame is the 622 px window at
// 1.05 scale (654 px) and the last close frame is at 1.106, both about the screen centre, so the scale runs 1.10 -> 1.00
// while the alpha runs 0 -> 1 (and the reverse on close). The 100 ms frame step cannot separate the scale curve from the
// alpha curve, so one linear parameter drives both: a FIT.
constexpr double oskPanelScaleSpread = 0.10;
inline double oskPanelScale(double alpha, bool opening) {
    (void)opening;
    return 1. + oskPanelScaleSpread * (1. - std::clamp(alpha, 0., 1.));
}

// The darkening behind the panel is not part of the fade. In the recording it eases up over about a second after the
// panel opens (background samples 131 -> 124 -> 102 -> 86 -> 73 -> 65 -> 59 -> 55 -> 50 -> 48 -> 46 -> 43 -> 42 at
// 100 ms steps: the remaining fraction shrinks by about 0.74 per 100 ms, the paf accel curve's per-frame step at 60 Hz)
// and it is held through the panel's close fade, then released within the next 100 ms (it belongs to the calling
// dialog, which is bright again one frame after the panel is gone). Returns the share (0..1) of the measured 0.70 dim.
constexpr double oskDimRemainPer100ms = 0.74;
constexpr int oskDimReleaseMilliseconds = 80;
inline double oskDimFactor(qint64 elapsed, bool opening) {
    if (opening) return 1. - std::pow(oskDimRemainPer100ms, std::max<qint64>(0, elapsed) / 100.);
    const double since = double(elapsed - oskCloseFadeMilliseconds);
    return since <= 0. ? 1. : 1. - std::clamp(since / oskDimReleaseMilliseconds, 0., 1.);
}
// The closing copy of the panel is drawn until the dim has been released too.
inline bool oskClosingDone(qint64 elapsed) {
    return elapsed >= oskCloseFadeMilliseconds + oskDimReleaseMilliseconds;
}

// The focus highlight cross-fades: a key fades its highlight in over ~150 ms when it gains focus and out over ~200 ms
// when it loses it (key 'k' in the recording rises 189 -> 213 -> 221 over 37.5-37.7 s and falls 212 -> 195 -> 188 over
// 37.8-38.0 s; 'i' rises 195 -> 209 -> 215 and later falls 202 -> 189). Linear; the curve is a fit.
constexpr int oskFocusInMilliseconds = 150;
constexpr int oskFocusOutMilliseconds = 200;
inline double oskFocusFade(qint64 sinceChange, bool gaining) {
    const double span = gaining ? oskFocusInMilliseconds : oskFocusOutMilliseconds;
    const double t = std::clamp(double(std::max<qint64>(0, sinceChange)) / span, 0., 1.);
    return gaining ? t : 1. - t;
}

} // namespace xmb
