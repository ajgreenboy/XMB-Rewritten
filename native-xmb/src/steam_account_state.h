#pragma once

// Everything the Game column derives from "which Steam account is signed in", gathered in one place so it can be
// read in one pass off the UI thread and replaced as one unit. When the account changes (an XMB switch, a switch made
// inside the Steam client, a restart that finds a different account) the shell drops the previous snapshot and adopts
// the new one; nothing account-specific is kept anywhere else. audit/STEAM-ACCOUNT-SWITCH.md lists every item.

#include "steam_accounts.h"
#include "steam_activity.h"
#include "steam_collections.h"
#include "steam_keyless.h"
#include "steam_library.h"
#include "steam_shortcuts.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace xmb {

struct SteamAccountSnapshot {
    QString steamId;                         // SteamID64 the data below belongs to (empty: no account found)
    QString persona;                         // that account's persona name from loginusers.vdf; empty when unknown
    QHash<QString, SteamActivity> activity;  // play records (userdata/<id>/config/localconfig.vdf)
    SteamCollectionList collections;         // the account's static collections (local, keyless)
    QVector<SteamShortcut> shortcuts;        // the account's non-Steam shortcuts (userdata/<id>/config/shortcuts.vdf)
    QString shortcutsError;                  // why shortcuts.vdf was only partly read; empty when it read cleanly
    SteamKeylessLibrary keyless;             // the keyless library (steam_keyless.h)
    QVector<SteamAccount> accounts;          // the accounts the client remembers (active marks follow the registry)
};

// Reads the snapshot for `steam.steamId` from the installation `steam` describes. Pure file reads, safe off the UI
// thread; installed titles only name apps the account's own data already lists.
SteamAccountSnapshot loadSteamAccountSnapshot(const SteamInstallation& steam, const QString& dataDirectory);

} // namespace xmb
