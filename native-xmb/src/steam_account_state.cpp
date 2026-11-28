#include "steam_account_state.h"

namespace xmb {

SteamAccountSnapshot loadSteamAccountSnapshot(const SteamInstallation& steam, const QString& /*dataDirectory*/) {
    SteamAccountSnapshot snapshot;
    snapshot.steamId = steam.steamId;
    snapshot.accounts = scanSteamAccounts(steam.root);
    for (const auto& account : snapshot.accounts)
        if (account.steamId64 == snapshot.steamId) { snapshot.persona = account.personaName; break; }
    if (snapshot.steamId.isEmpty()) return snapshot;   // no account: every account-specific list stays empty

    snapshot.activity = scanSteamActivity(steam.root, snapshot.steamId);
    // XMB_STEAM_SHORTCUTS=0 leaves the non-Steam shortcuts out (the render gate sets it so this PC's own shortcuts never
    // reach a capture, as it does for API keys); everything else about them is documented in audit/STEAM-SHORTCUTS.md.
    if (qEnvironmentVariable("XMB_STEAM_SHORTCUTS") != QLatin1String("0")) {
        auto shortcuts = scanSteamShortcuts(steam.root, snapshot.steamId);
        snapshot.shortcuts = std::move(shortcuts.shortcuts);
        snapshot.shortcutsError = shortcuts.error;
    }
    QString collectionsError;
    snapshot.collections = scanSteamCollectionsIn(steam.root, snapshot.steamId, &collectionsError);
    QHash<QString, QString> installedTitles;
    for (const auto& game : steam.games) installedTitles.insert(game.appId, game.title);
    snapshot.keyless = scanSteamKeylessLibrary(steam.root, snapshot.steamId, installedTitles);
    return snapshot;
}

} // namespace xmb
