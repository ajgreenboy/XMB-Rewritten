#include "shell.h"
#include <QDir>
#include <QSet>
#include <algorithm>

namespace xmb {

namespace {
Entry nativeGameRow(const QString& id,const QString& title,Action action) {
    Entry entry;entry.id=id;entry.title=title;entry.action=action;entry.container=QStringLiteral("explore_plugin_full");
    entry.texture=QStringLiteral("game_tex_default_ps3");entry.source=QStringLiteral("native");return entry;
}
QString nativeOptionCount(const GameResult& game) {
    QSet<QString> sources;for(const auto& option:game.options)sources.insert(option.source);
    if(sources.size()>1)return QObject::tr("%n source(s)",nullptr,int(sources.size()));
    return sources.isEmpty()?QObject::tr("No download sources"):*sources.constBegin();
}
}

// The GEN 2 (2008-2012) PlayStation Store catalogue and detail, over the JSON/Hydra download-games feed.
//
// These are ordinary pages in the page stack (Page::legacyStore / Page::legacyDetail), so Back (Circle)
// pops them exactly like any other page and the search OSK result, the source-choice page and the
// Download Management job lifecycle all keep their existing code paths (release_shell.cpp /
// release_jobs.cpp) untouched.
//
// Cursor model: Page::selection is the focused grid cell's game index (0..N-1; the left selected-game panel
// follows it), laid out in `legacyStoreGridColumns` columns, so the ordinary selection()<entries().size()
// invariant the capture sidecar and the item menu rely on keeps holding. The top Search button is a separate
// focus flag (Page::legacySearchFocus); View::drawLegacySurface reads the same mapping.
//
// Navigation is a native choice: the 2008 store screenshots document the static layout (selected-game panel
// left, 5-column cover grid right, Search/Redeem/View Downloads/View Cart controls, silver footer) but not
// the D-pad cursor, so the moves below are a clean, bounded 2D cursor over the documented regions rather than
// a firmware-traced behaviour.

void Shell::legacyStoreMove(int dx,int dy) {
    if(pages_.isEmpty() || !pages_.last().legacyStore)return;
    Page& page=pages_.last();
    const int n=page.downloadResults.size();
    if(n<=0)return;                              // empty catalogue: nothing to move over
    const int cols=legacyStoreGridColumns;
    const int visRows=legacyStoreGridRows;
    const int rows=(n+cols-1)/cols;
    const int sel=std::clamp(page.selection,0,n-1);
    if(page.legacySearchFocus) {
        if(dy>0) {                               // Down -> the grid (the selected-game panel follows it)
            page.legacySearchFocus=false;
            page.selection=sel;
            page.legacyScroll=0;
            columnSink_=0;
            interactionAudio_.play(InteractionAudio::Cue::Move);
            emit changed();
        }
        // Up/Left/Right stay on Search: the other control labels (Redeem Codes / View Downloads / View Cart)
        // are out of scope and not focusable, so there is nothing to move to.
        return;
    }
    const int col=sel%cols,row=sel/cols;
    if(dy<0) {
        if(row==0) {                             // top row -> Search
            page.legacySearchFocus=true;
            columnSink_=0;
            interactionAudio_.play(InteractionAudio::Cue::Move);
            emit changed();
        } else {
            page.selection=sel-cols;
            columnSink_=0;
            interactionAudio_.play(InteractionAudio::Cue::Move);
            emit changed();
        }
        return;
    }
    int target=sel;
    if(dy>0) {
        if(row+1<rows && sel+cols<n)target=sel+cols;
    } else if(dx<0) {
        if(col>0)target=sel-1;
    } else if(dx>0) {
        if(col+1<cols && sel+1<n)target=sel+1;
    }
    if(target!=sel) {
        page.selection=target;
        // Keep the focused grid cell inside the visible window: the grid scrolls one row at a time.
        const int tr=target/cols;
        if(tr<page.legacyScroll)page.legacyScroll=tr;
        else if(tr>=page.legacyScroll+visRows)page.legacyScroll=tr-visRows+1;
        columnSink_=0;
        interactionAudio_.play(InteractionAudio::Cue::Move);
        emit changed();
    }
}

void Shell::legacyStoreActivate() {
    if(pages_.isEmpty() || !pages_.last().legacyStore)return;
    const Page& page=pages_.last();
    if(page.legacySearchFocus) {
        // Top Search button -> the separate on-screen-keyboard search screen, scoped to online + imported
        // sources (the same "all" scope the existing Search Games row offers; openGameSearch("all")'s OSK
        // result arrives through setJellyfinSignInField -> runGameSearch).
        interactionAudio_.play(InteractionAudio::Cue::Confirm);
        emit requestJellyfinFieldEdit(QStringLiteral("game:search:all"),tr("Search Games"),{},false);
        return;
    }
    const int gameIndex=page.selection;          // the focused grid cell's game
    if(gameIndex<0 || gameIndex>=page.downloadResults.size())return;
    if(page.legacyGameState.value(gameIndex)==0) {
        showMessage(tr("Sources are still loading"),tr("This game cannot be opened until all source results are ready."),MessageKind::Inline);
        return;
    }
    if(page.legacyGameState.value(gameIndex)==2) {
        retryLegacyStoreGame(gameIndex);
        return;
    }
    interactionAudio_.play(InteractionAudio::Cue::Confirm);
    openLegacyDetail(gameIndex);
}

void Shell::openLegacyDetail(int gameIndex) {
    if(pages_.isEmpty() || pages_.size()>=32)return;
    const Page& src=pages_.last();
    if(gameIndex<0 || gameIndex>=src.downloadResults.size())return;
    const GameResult game=src.downloadResults[gameIndex];
    QSet<QString> sources;
    for(const auto& option:game.options)sources.insert(option.source);
    // The only metadata this feed actually carries: the source list, a per-source size and a trust status
    // (Trusted / Safe For Use). There is no description, genre, release year, developer, publisher or
    // platform in HydraLibrary's GameResult, so the detail page shows those as explicitly unavailable
    // rather than inventing them.
    QString status;
    for(const auto& option:game.options)if(!option.sourceStatus.isEmpty()){status=option.sourceStatus;break;}
    QString detail;
    if(sources.size()>1)detail=tr("%n source(s)",nullptr,int(sources.size()));
    else if(!game.options.isEmpty())detail=game.options.first().source;
    if(!status.isEmpty())detail+=QStringLiteral(" · ")+status;
    Page page;page.title=game.name;page.legacyDetail=true;
    page.downloadResults={game};
    Entry entry;
    entry.id=QStringLiteral("game:detail");
    entry.title=game.name;
    entry.action=Action::GameSearchResult;
    entry.value=QStringLiteral("0");
    entry.container=QStringLiteral("explore_plugin_full");
    entry.texture=QStringLiteral("game_tex_default_ps3");
    entry.source=QStringLiteral("native");
    entry.detail=detail;
    entry.fieldValue=game.options.isEmpty()?QString{}:game.options.first().fileSize;
    entry.gameTile=true;
    if(gameIndex<src.entries.size())entry.artwork=src.entries[gameIndex].artwork;
    entry.artworkUrl=game.coverUrl;
    page.entries.append(entry);
    pages_.push_back(std::move(page));
    // Detail-only metadata: resolve the game's RAWG id from its title and fetch the
    // description/genre/year/developer/publisher/platform asynchronously. Never per
    // grid item (only the detail page triggers it); no key means no network and the
    // view keeps its honest "Not available" lines.
    requestLegacyDetails(game.name);
    emit changed();
}

void Shell::openGameDetails(const GameResult& game,const QImage& artwork) {
    if(pages_.size()>=32)return;
    Page page;page.title=game.name;page.gameDetails=true;page.identity=++pageIdentityCounter_;page.downloadResults={game};
    Entry download=nativeGameRow(QStringLiteral("game:detail:download"),tr("Download"),Action::GameDetailsDownload);
    download.detail=nativeOptionCount(game);download.gameTile=true;download.artwork=artwork;download.artworkUrl=game.coverUrl;page.entries.append(download);
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:description"),tr("Description"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:genre"),tr("Genre"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:year"),tr("Release Year"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:developer"),tr("Developer"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:publisher"),tr("Publisher"),Action::Unavailable));
    page.entries.append(nativeGameRow(QStringLiteral("game:detail:platform"),tr("Platform"),Action::Unavailable));
    pages_.push_back(std::move(page));requestLegacyDetails(game.name);emit changed();
}

void Shell::legacyDetailActivate() {
    if(pages_.isEmpty() || !pages_.last().legacyDetail)return;
    if(pages_.last().downloadResults.isEmpty())return;
    const GameResult game=pages_.last().downloadResults.first();
    interactionAudio_.play(InteractionAudio::Cue::Confirm);
    // Hand off to the existing source-choice page (the separate worker's surface): it lists each download
    // source for this game, and from there the drive picker, confirm and Download Management job lifecycle
    // are unchanged.
    openGameSources(game);
}

void Shell::requestLegacyDetails(const QString& title) {
    // Stale-response guard: record the page depth this lookup belongs to. Any search or
    // detail response that lands after the user has navigated (the detail page popped, or
    // another page pushed on top) is dropped instead of repainting a page it was not for.
    legacyDetailDepth_=pages_.size();
    legacyDetailTitle_=title;
    if(!RawgClient::hasApiKey())return;   // no key: honest unavailable state, no network, never blocks
    if(!rawgDetails_) {
        rawgDetails_=std::make_unique<RawgClient>(QDir(dataDirectory_).filePath(QStringLiteral("cache/release-art")),this);
        connect(rawgDetails_.get(),&RawgClient::searchReady,this,[this](const RawgGameList& games) {
            if(pages_.isEmpty() || !pages_.last().gameDetails || pages_.size()!=legacyDetailDepth_)return;
            const int id=resolveRawgGameId(games,legacyDetailTitle_);
            if(id>0)rawgDetails_->fetchDetails(id);
            // id==0 (no exact match, or ambiguous same-name releases): leave the page on its
            // "Not available" lines rather than risk a wrong description.
        });
        connect(rawgDetails_.get(),&RawgClient::detailsReady,this,[this](const RawgGameDetails& details) {
            if(pages_.isEmpty() || !pages_.last().gameDetails || pages_.size()!=legacyDetailDepth_)return;
            if(pages_.last().title!=legacyDetailTitle_)return;
            pages_.last().gameDetailsData=details;
            const auto set=[&](const QString& id,const QString& value){for(auto& row:pages_.last().entries)if(row.id==id){row.detail=value;row.disabled=value.isEmpty();}};
            set(QStringLiteral("game:detail:description"),details.description.isEmpty()?tr("Not available"):details.description);
            set(QStringLiteral("game:detail:genre"),details.genres.isEmpty()?tr("Not available"):details.genres.join(QStringLiteral(", ")));
            set(QStringLiteral("game:detail:year"),details.releaseYear?QString::number(details.releaseYear):tr("Not available"));
            set(QStringLiteral("game:detail:developer"),details.developers.isEmpty()?tr("Not available"):details.developers.join(QStringLiteral(", ")));
            set(QStringLiteral("game:detail:publisher"),details.publishers.isEmpty()?tr("Not available"):details.publishers.join(QStringLiteral(", ")));
            set(QStringLiteral("game:detail:platform"),details.platforms.isEmpty()?tr("Not available"):details.platforms.join(QStringLiteral(", ")));
            emit changed();
        });
    }
    rawgDetails_->search(title);
}

} // namespace xmb
