#pragma once

// Pure decisions for "the Steam account changed, so the Game column must follow" (audit/STEAM-ACCOUNT-SWITCH.md,
// "Library refresh after a switch"). Header-only and free of Qt GUI/network so tests/steam_switch_tests.cpp covers it.

#include "steam_switch.h"

#include <QString>

namespace xmb {

// Which switch outcomes mean the library has to be read again. SignedIn and OtherAccount: Steam is now signed in as
// someone, and the column follows whoever is active. Everything else changed nothing the library depends on, or the
// user is still signing in (LoginWindow is handled by the ActiveUserWatch below, once Steam reports the account).
inline bool steamSwitchRefreshesLibrary(SteamSwitchResult::Outcome outcome) {
    return outcome == SteamSwitchResult::Outcome::SignedIn || outcome == SteamSwitchResult::Outcome::OtherAccount;
}
inline bool steamSwitchWatchesActiveUser(SteamSwitchResult::Outcome outcome) {
    return outcome == SteamSwitchResult::Outcome::LoginWindow;
}

// An owned-games response belongs to the account the request was made for. It is stale when both ids are known and
// differ; an unknown id (no account at all yet) is never called stale.
inline bool ownedResponseIsStale(const QString& requestedFor, const QString& currentAccount) {
    return !requestedFor.isEmpty() && !currentAccount.isEmpty() && requestedFor != currentAccount;
}

// SteamID64 of an individual account from Steam's 32-bit ActiveUser; empty for 0 (nobody) or out of range.
inline QString steamIdFromActiveUser(qulonglong accountId) {
    constexpr qulonglong base = 76561197960265728ULL;
    if (accountId == 0 || accountId > 0xffffffffULL) return {};
    return QString::number(base + accountId);
}

// The name the "Library refreshed for ..." notice uses: the first non-empty of what Steam reported, the active
// account's persona and the prompt's label.
inline QString steamRefreshName(const QString& reported, const QString& activePersona, const QString& label) {
    for (const QString* candidate : {&reported, &activePersona, &label})
        if (!candidate->trimmed().isEmpty()) return candidate->trimmed();
    return QStringLiteral("Steam");
}

// Watches the ActiveUser the sweep reads from Steam's registry record. The first observation only primes it; after
// that a change to a non-zero account is reported once (0 is "signed out" and is remembered, not reported), so a
// registry value that stays put never refreshes twice.
struct ActiveUserWatch {
    bool primed = false;
    qulonglong last = 0;
    void reset() { primed = false; last = 0; }
    bool observe(qulonglong activeUser) {
        if (!primed) { primed = true; last = activeUser; return false; }
        const bool changed = activeUser != 0 && activeUser != last;
        last = activeUser;
        return changed;
    }
};

} // namespace xmb
