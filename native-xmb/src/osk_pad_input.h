#pragma once

// The on-screen keyboard's pad extras (audit/BEHAVIOUR-GAPS-SPEC.md section 3, audit/OSK-FIRMWARE.md section 6):
// L1/R1 move the caret, L2 the accent page, R2 the caps page, SELECT is Input Mode, and the panel fades in and out.
// Pure and header-only so tests/osk_pad_input_tests.cpp covers it without a window or a controller.

#include "osk_layout.h"

#include <QVector>
#include <algorithm>

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

// The page an Accent or Shift action lands on, or -1 when it changes nothing. Shift toggles lower <-> caps by page
// name; Accent toggles the accent page and is inert (-1) while the layout carries none: the accent rows are not
// decoded (audit/OSK-FIRMWARE.md 5, 13), and this build does not invent a glyph table to fill the page. A layout
// whose pages are not named lower/caps (the tenkey panel) falls back to the plain next-page step.
inline int oskPageIndex(const QVector<OskPage>& pages, const QString& name) {
    for (int i = 0; i < pages.size(); ++i)
        if (pages.at(i).name.compare(name, Qt::CaseInsensitive) == 0) return i;
    return -1;
}
inline int oskPageForAction(const QVector<OskPage>& pages, int current, OskAction action) {
    const int lower = oskPageIndex(pages, QStringLiteral("lower"));
    if (action == OskAction::Accent) {
        const int accent = oskPageIndex(pages, QStringLiteral("accent"));
        if (accent < 0) return -1;
        return current == accent ? (lower >= 0 ? lower : 0) : accent;
    }
    if (action == OskAction::Shift) {
        const int caps = oskPageIndex(pages, QStringLiteral("caps"));
        if (lower < 0 || caps < 0) return pages.size() > 1 ? (current + 1) % int(pages.size()) : -1;
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

} // namespace xmb
