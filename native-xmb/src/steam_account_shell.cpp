// What follows the signed-in Steam account inside the shell (audit/STEAM-ACCOUNT-SWITCH.md, "Full account switch").
//
// Every list the Game column derives from the account is replaced as one unit (steam_account_state.h): play records,
// collections, and the owned list (Valve's when a Web API key exists, otherwise
// the account's own keyless list from steam_keyless.h). The sources of a change are all funnelled here:
//   - the XMB's own switch (steamSwitchFinished -> steamLibraryAccountChanged),
//   - a sign-in noticed by the Steam-window sweep,
//   - a switch made inside the Steam client, noticed by the registry poll below (no sweep or kiosk needed),
//   - a restart or rescan that finds a different account than the one on screen (applySteamAccountSnapshot).
#include "shell.h"
#include "diagnostics.h"
#include "steam_owned.h"
#include <QTimer>
#include <QtDebug>

namespace xmb {

void Shell::resetSteamAccountState() {
    steamOwned_.cancel();      // an in-flight owned request for the previous account can no longer arrive
    ownedInFlight_=false;
    ownedGames_.clear();
    ownedSignature_.clear();
    ownedFromKeyless_=false;
    baseCollections_.clear();
    collections_.clear();
    steamActivity_.clear();
    steamPersona_.clear();
    closeSteamLibraryPages();
}

// Steam folder, collection and hub pages carry rows of the account that was signed in when they were built; they are
// closed (with the options panel that may refer to one of their rows) rather than left showing a previous account's games.
void Shell::closeSteamLibraryPages() {
    int firstStale=-1;
    for(int i=0;i<pages_.size() && firstStale<0;++i)
        for(const auto& entry:pages_[i].entries)
            if(entry.action==Action::SteamFolder || entry.id.startsWith(QStringLiteral("steam:")) || entry.id.startsWith(QStringLiteral("steam-folder"))){firstStale=i;break;}
    if(firstStale<0)return;
    qInfo().noquote()<<QStringLiteral("steam: closed %1 library page(s) built for the previous account").arg(pages_.size()-firstStale);
    pages_.resize(firstStale);
    optionsOpen_=false;optionsNested_=false;
}

void Shell::applySteamAccountSnapshot(SteamAccountSnapshot snapshot) {
    if(steamId_!=snapshot.steamId) {
        // The rescan found a different account than the one on screen (a switch the XMB did not see, or a restart
        // under another account): nothing of the previous account may survive into this one.
        qInfo().noquote()<<QStringLiteral("steam: library account is now %1 (was %2)")
            .arg(snapshot.persona.isEmpty()?snapshot.steamId:snapshot.persona,steamId_.isEmpty()?QStringLiteral("none"):steamId_);
        resetSteamAccountState();
    }
    steamId_=snapshot.steamId;
    steamPersona_=snapshot.persona;
    steamActivity_=std::move(snapshot.activity);
    steamAccounts_=std::move(snapshot.accounts);
    // Local and keyless: the account's own groups need no Web API key. With no account found the list is left as it is
    // (an account change already cleared it); the owned-games handler may still fill it from an XMB_STEAM_ID account.
    if(!snapshot.steamId.isEmpty())baseCollections_=std::move(snapshot.collections);
    steamOverlay_=loadSteamUserOverlay(dataDirectory_);
    collections_=mergeSteamCollectionsWithOverlay(baseCollections_,steamOverlay_);

    // The owned list. Valve's wins whenever a key exists and its list is what is on screen; otherwise the account's
    // own local data stands in (no key, or the key's list has not arrived yet after an account change).
    const bool ownedIsFromValve=!ownedGames_.isEmpty() && !ownedFromKeyless_;
    if(steamKeylessReplacesOwned(SteamOwned::hasApiKey(),ownedIsFromValve)) {
        const bool changed=ownedFromKeyless_ || !ownedGames_.isEmpty() || !snapshot.keyless.games.isEmpty();
        ownedGames_=std::move(snapshot.keyless.games);
        ownedFromKeyless_=!ownedGames_.isEmpty();
        if(changed)ownedSignature_.clear();   // a later Valve answer must replace this list even if its signature matches an old one
        if(ownedFromKeyless_)
            qInfo().noquote()<<QStringLiteral("steam: keyless library %1 game(s): %2 played, %3 collected, %4 in the library cache%5")
                .arg(ownedGames_.size()).arg(snapshot.keyless.playedCount).arg(snapshot.keyless.collectedCount)
                .arg(snapshot.keyless.libraryCacheCount).arg(snapshot.keyless.fromCache?QStringLiteral(" (cached)"):QString{});
        for(const auto& warning:snapshot.keyless.warnings)qWarning().noquote()<<"steam keyless:"<<warning;
    }
    refreshSteamAccountsPage();

    if(!steamAccountPollStarted_ && qEnvironmentVariable("XMB_STEAM_ACCOUNT_POLL")!=QLatin1String("0")) {
        steamAccountPollStarted_=true;
        steamAccountPoll_.setInterval(3000);
        connect(&steamAccountPoll_,&QTimer::timeout,this,&Shell::pollSteamActiveUser);
        steamAccountPoll_.start();
    }
}

// Steam reports a different signed-in account than the library shows: a switch made inside the Steam client (or any
// other way), noticed here without a sweep, a kiosk or an XMB prompt. Read-only: one registry value.
void Shell::pollSteamActiveUser() {
    if(!contentLoaded_ || steamRoot_.isEmpty())return;
    const QString registry=steamActiveAccountFromRegistry(steamRoot_);
    if(registry==steamId_)steamPollTriggered_.clear();
    const QString target=steamPollAccountChange(registry,steamId_,steamPollTriggered_);
    if(target.isEmpty())return;
    steamPollTriggered_=target;
    const auto accounts=scanSteamAccounts(steamRoot_);
    QString persona;for(const auto& account:accounts)if(account.steamId64==target){persona=account.personaName;break;}
    steamLibraryAccountChanged(steamRefreshName({},persona,target));
}

QString Shell::steamLibrarySourceText() const {
    return ownedFromKeyless_?steamKeylessSourceText(steamPersona_):QString{};
}

// An open Steam Account page re-marks its rows (the signed-in dot, "signed in") from the fresh account list.
void Shell::refreshSteamAccountsPage() {
    const bool anyActive=std::any_of(steamAccounts_.cbegin(),steamAccounts_.cend(),[](const SteamAccount& a){return a.active;});
    for(auto& page:pages_) {
        if(!page.steamAccounts)continue;
        for(auto& entry:page.entries) {
            if(!entry.id.startsWith(QStringLiteral("steam-account:")))continue;
            const auto account=std::find_if(steamAccounts_.cbegin(),steamAccounts_.cend(),
                [&](const SteamAccount& a){return a.steamId64==entry.value;});
            if(account==steamAccounts_.cend())continue;
            const Entry fresh=steamAccountRow(*account,anyActive);
            entry.fieldValue=fresh.fieldValue;entry.detail=fresh.detail;
        }
    }
}

} // namespace xmb
