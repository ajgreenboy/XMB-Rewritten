#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace xmb {

// The Steam Families group this account belongs to, as Steam records it locally in
// userdata/<accountid>/config/localconfig.vdf ("FamilyGroup": groupid, name, members).
// Membership is local and keyless; the *shared app list* is not stored there.
struct SteamFamily {
    QString groupId;
    QString name;
    QVector<QString> memberAccountIds;  // 32-bit account ids, as Steam writes them
    bool valid = false;
};

SteamFamily scanSteamFamily(const QString& steamId64 = {});
// The same read against one named Steam installation (no scanSteam() call): what the keyless worker uses.
SteamFamily scanSteamFamilyIn(const QString& steamRoot, const QString& steamId64);

// Family-shared titles are licensed to another member, so IPlayerService/GetOwnedGames
// does not return them, and Valve's IFamilyGroupsService requires an OAuth access token
// (a Web API key is rejected with 401). Until that token flow exists, the XMB reads an
// optional list the user provides under <data>/family-shared.json:
//
//   {"apps": [{"id": 620, "title": "Portal 2"}, ...]}
//
// Titles are required, because no local source maps these ids to names.
struct FamilyGame {
    QString appId;
    QString title;
};

// With `steamId64` the list is account-scoped: <data>/family-shared-<accountid>.json is preferred, and the shared
// family-shared.json is ignored when it carries an "account" naming a different account (so switching accounts never
// shows the previous account's family titles). Without it the behaviour is unchanged.
QVector<FamilyGame> scanFamilySharedGames(const QString& dataDirectory, QString* error = nullptr,
                                          const QString& steamId64 = {});

} // namespace xmb
