#include "steam_account_state.h"

namespace xmb {

SteamAccountSnapshot loadSteamAccountSnapshot(const SteamInstallation& steam, const QString& dataDirectory) {
    SteamAccountSnapshot snapshot;
    snapshot.steamId = steam.steamId;
    snapshot.accounts = scanSteamAccounts(steam.root);
    for (const auto& account : snapshot.accounts)
        if (account.steamId64 == snapshot.steamId) { snapshot.persona = account.personaName; break; }
    if (snapshot.steamId.isEmpty()) return snapshot;   // no account: every account-specific list stays empty

    snapshot.activity = scanSteamActivity(steam.root, snapshot.steamId);
    QString collectionsError;
    snapshot.collections = scanSteamCollectionsIn(steam.root, snapshot.steamId, &collectionsError);
    QHash<QString, QString> installedTitles;
    for (const auto& game : steam.games) installedTitles.insert(game.appId, game.title);
    snapshot.keyless = scanSteamKeylessLibrary(steam.root, snapshot.steamId, installedTitles);
    return snapshot;
}

} // namespace xmb
