#pragma once

// "Library without a key": the games a Steam account has, built from nothing but that account's own files in the
// local Steam installation, for when no Steam Web API key is configured (SteamOwned::apiKey() is empty). Valve's
// owned-games list is the complete answer and wins whenever a key exists; this is the fallback, and it is honest
// about being smaller: it lists games the account has played or collected, not every purchase.
//
// Only the signed-in account's data is read. Nothing is taken from another account's userdata folder, and the
// installed games of the PC (which belong to whichever account installed them) add nothing by themselves: an
// installed manifest is used only to NAME an app that this account's own data already lists.
//
// Sources, all under <steam>/ and all read-only:
//   userdata/<accountid>/config/localconfig.vdf
//       UserLocalConfigStore > Software > Valve > Steam > apps > <appid> > LastPlayed / Playtime. An app counts when
//       it has playtime or a last-played time; an entry with neither is noise (a stub Steam wrote for a launch
//       option or a cloud record) and is ignored.
//   userdata/<accountid>/config/cloudstorage/cloud-storage-namespace-1.json
//       The account's static collections ("user-collections.*" entries, "added" app ids). Dynamic collections only
//       store a filter and have no member list, so they contribute nothing.
//   userdata/<accountid>/config/librarycache/<appid>.json
//       One file per app the Steam library has shown this account (a JSON array of [section, {version, data}]
//       pairs: achievements, descriptions, ...). The file name is the app id; the content carries no title, so it
//       only says "this app is in this account's library".
//   appcache/appinfo.vdf   (steam_appinfo.h)
//       The client's own metadata: the authoritative title and type. Only type "Game" is kept, which drops tools,
//       redistributables, dedicated servers, soundtracks, DLC, videos and demos. Demos are left out on purpose: they
//       are not purchases, they vanish from the store, and Valve's own owned list is where a real demo would show.
//       When appinfo has no entry for an app, the installed manifest's name is the fallback and the app is kept only
//       if such a name exists (an unnamed, untyped id is never listed).
//
// The result is bounded (kSteamKeylessMaximumGames) and cached per account id; the cache entry is invalidated by the
// size and modification time of every file above, so a played game shows up on the next scan.

#include "steam_owned.h"

#include <QHash>
#include <QString>
#include <QStringList>

namespace xmb {

inline constexpr int kSteamKeylessMaximumGames = 5000;

struct SteamKeylessLibrary {
    QString steamId64;
    OwnedGameList games;      // game rows, title order; `installed` is set from installedTitles
    int playedCount = 0;      // how many rows have a play record
    int collectedCount = 0;   // how many rows are in at least one static collection
    int libraryCacheCount = 0;// how many rows have a librarycache file
    bool truncated = false;   // more than kSteamKeylessMaximumGames candidates; the most recently played were kept
    bool fromCache = false;   // the scan was answered from the per-account cache
    QStringList warnings;
};

// `installedTitles` maps installed app ids to manifest names (the PC's installs); it only names and flags apps.
// Returns an empty library for an empty/invalid account, a missing userdata folder, or XMB_STEAM_ACTIVITY=0 (the
// switch deterministic captures use to turn every live Steam record off). Safe to call off the UI thread.
SteamKeylessLibrary scanSteamKeylessLibrary(const QString& steamRoot, const QString& steamId64,
                                            const QHash<QString, QString>& installedTitles = {});

// Drops the per-account cache (tests; the shell never needs it because entries are validated on every read).
void clearSteamKeylessCache();

// The line shown where the library list's source is named, e.g. on the All Games row. `persona` is the signed-in
// account's persona name; empty falls back to "this account".
QString steamKeylessSourceText(const QString& persona);

} // namespace xmb
