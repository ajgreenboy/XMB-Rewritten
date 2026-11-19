#include "shell.h"
#include <QSet>
#include <algorithm>

namespace xmb {

// The GEN 2 (2008-2012) PlayStation Store catalogue and detail, over the JSON/Hydra download-games feed.
//
// These are ordinary pages in the page stack (Page::legacyStore / Page::legacyDetail), so Back (Circle)
// pops them exactly like any other page and the search OSK result, the source-choice page and the
// Download Management job lifecycle all keep their existing code paths (release_shell.cpp /
// release_jobs.cpp) untouched.
//
// Cursor model: Page::selection is a valid game index (0 = the featured pane, 1..N-1 = grid cells, laid
// out in `legacyStoreGridColumns` columns), so the ordinary selection()<entries().size() invariant the
// capture sidecar and the item menu rely on keeps holding. The top Search button is a separate focus flag
// (Page::legacySearchFocus); View::drawLegacySurface reads the same mapping.
//
// Navigation is a native choice: the 2008 store's manual screenshots (manifest.md) document the static
// layout and the button set but not the D-pad cursor, so the moves below are a clean, bounded 2D cursor
// over the documented regions rather than a firmware-traced behaviour.

void Shell::legacyStoreMove(int dx,int dy) {
    if(pages_.isEmpty() || !pages_.last().legacyStore)return;
    Page& page=pages_.last();
    const int n=page.downloadResults.size();
    if(n<=0)return;                              // empty catalogue: nothing to move over
    const int gridCount=n-1;                     // games 1..n-1 sit in the grid (game 0 is featured)
    const int cols=legacyStoreGridColumns;
    const int visRows=legacyStoreGridRows;
    const int rows=gridCount>0?(gridCount+cols-1)/cols:0;
    const int sel=std::clamp(page.selection,0,std::max(0,n-1));
    bool search=page.legacySearchFocus;
    int target=sel;bool targetSearch=search;
    if(search) {
        if(dy>0){targetSearch=false;target=0;}   // Down -> Featured
        // Up/Left/Right stay on Search: the other control labels (Redeem Codes / View Downloads / View
        // Cart) are out of scope and not focusable, so there is nothing to move to.
    } else if(sel==0) {
        // Featured
        if(dy<0)targetSearch=true;               // Up -> Search
        else if(dx>0||dy>0)target=gridCount>0?1:0;   // Right/Down -> first grid cell (or stay)
        // Left stays on Featured.
    } else {
        const int k=sel-1,col=k%cols,row=k/cols; // grid cell k = game sel
        if(dx<0) {                               // Left
            if(col==0)target=0;                  // leftmost column -> Featured
            else target=sel-1;
        } else if(dx>0) {                        // Right
            if(col+1<cols && k+1<gridCount)target=sel+1;
        } else if(dy<0) {                        // Up
            if(row==0)targetSearch=true;         // top row -> Search
            else target=sel-cols;                // row>0 guarantees k-cols>=0
        } else if(dy>0) {                        // Down
            if(row+1<rows && k+cols<gridCount)target=sel+cols;
        }
    }
    if(target!=sel || targetSearch!=search) {
        page.selection=target;
        page.legacySearchFocus=targetSearch;
        // Keep the focused grid cell inside the visible window: the grid scrolls one row at a time.
        if(!targetSearch && target>=1) {
            const int tr=(target-1)/cols;
            if(tr<page.legacyScroll)page.legacyScroll=tr;
            else if(tr>=page.legacyScroll+visRows)page.legacyScroll=tr-visRows+1;
        }
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
    const int gameIndex=page.selection;          // 0 = featured, 1.. = grid cell
    if(gameIndex<0 || gameIndex>=page.downloadResults.size())return;
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
    emit changed();
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

} // namespace xmb
