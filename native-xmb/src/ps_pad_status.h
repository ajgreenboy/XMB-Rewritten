#pragma once

// What the PS-menu controller panel and the PS-tap plate print (FIDELITY-BEHAVIOUR P8, BEHAVIOUR-GAPS-SPEC 1).
// Pure and header-only so tests/ps_pad_status_tests.cpp covers it without a window or a pad.
//
// The console frame shows slot 1 lit, the controller number "1" and the charge dash. A build without a live pad
// reading keeps that frame (Reference: scripted captures, where pad::info() is withheld on purpose). A live
// build prints what the report carries: no pad means no lit slot and a dash for both, a pad lights slot 1 and
// prints its battery percent, or the dash when the report carries none (a DualShock 3 does not).
// The slot is always 1: pad::Info has no player index and the HID path opens one pad at a time, so a pad is
// never claimed to be anything other than controller 1.

#include "pad.h"

#include <QString>

namespace xmb {

enum class PsPadSource { Reference, Live };

struct PsPadPanel {
    int litSlot = -1;        // 0-based slot drawn red, -1 when none is lit
    QString controller;      // text after the pad glyph: "1" or "-"
    QString charge;          // text on the Charge Level row: "75%" or "-"
    bool operator==(const PsPadPanel& other) const {
        return litSlot == other.litSlot && controller == other.controller && charge == other.charge;
    }
};

inline QString psPadChargeText(const pad::Info& info) {
    if (info.batteryPercent < 0) return QStringLiteral("-");
    const int percent = info.batteryPercent > 100 ? 100 : info.batteryPercent;
    return QString::number(percent) + QLatin1Char('%');
}

inline PsPadPanel psPadPanel(PsPadSource source, bool attached, const pad::Info& info) {
    if (source == PsPadSource::Reference) return {0, QStringLiteral("1"), QStringLiteral("-")};
    if (!attached || info.kind == pad::Kind::None) return {-1, QStringLiteral("-"), QStringLiteral("-")};
    return {0, QStringLiteral("1"), psPadChargeText(info)};
}

// The second line under "Turn Off the System?". Appliance (--kiosk) mode shuts the PC down; otherwise the XMB only
// quits (main.cpp's confirmed branch), so the line says which. Wording for the non-kiosk case is unchanged.
inline QString psConfirmHint(bool powersOffPc) {
    return powersOffPc ? QStringLiteral("X turns the PC off   O cancels")
                       : QStringLiteral("X turns the XMB off; the PC stays on   O cancels");
}

} // namespace xmb
