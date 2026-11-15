// Jellyfin music in the Music column (audit/JELLYFIN-MUSIC.md). Pages are ordinary XMB pages of rows; nothing here draws.
//
//   Music > Jellyfin Music     one row per registered Jellyfin app, under Spotify. It uses the ONE Jellyfin session the TV
//                              row uses (jellyfin_shell.cpp): signed in, it opens at once; a remembered session restores
//                              silently; with no session it opens the same sign-in page and lands on the music pages after.
//   the music root             Artists, Albums, Playlists, Songs, Recently Added, Random Albums, Search Music, Sign Out
//                              (with several music libraries, the libraries first; each opens this same root)
//   an artist / album /        its albums / its tracks in disc order / its tracks in the playlist's own order; Cross on a
//   playlist                   track plays the page's tracks as the queue (the existing audio player: Now Playing, Next and
//                              Previous, progress reports to the server)
//
// Every list is one server request of at most 200 rows (StartIndex, Limit); a "More..." row opens the next window, so a
// library of any size never blocks the page. Covers are the server's Primary pictures, fetched at most 60 a page and
// cached on disk by the client (jellyfin_client.cpp).
#include "shell.h"
#include "glyphs.h"
#include "service_icons.h"
#include "media_controls.h"
#include <QDir>
#include <QUrl>
#include <algorithm>

namespace xmb {
namespace {
bool sameServer(QUrl a,QUrl b){return a.adjusted(QUrl::StripTrailingSlash|QUrl::NormalizePathSegments)==b.adjusted(QUrl::StripTrailingSlash|QUrl::NormalizePathSegments);}
constexpr int CoversPerPage=60;
constexpr int AlbumArtKept=40;
Entry musicRow(QString id,QString title,Action action,QString value={},QString kind={},QString library={},QString detail={}) {
    Entry e;e.id=std::move(id);e.title=std::move(title);e.action=action;e.value=std::move(value);e.setting=std::move(kind);
    e.option=std::move(library);e.detail=std::move(detail);
    e.container=QStringLiteral("explore_plugin_full");e.texture=QStringLiteral("item_tex_plain_folder");e.source=QStringLiteral("native");
    return e;
}
Entry withMark(Entry e,const QImage& icon){if(!icon.isNull()){e.artwork=icon;e.iconSized=true;}return e;}
// A cover is square in the icon box: the middle of the picture, scaled to the box the Spotify rows use.
QImage squareCover(const QImage& image) {
    if(image.isNull())return {};
    const int side=std::min(image.width(),image.height());
    return image.copy((image.width()-side)/2,(image.height()-side)/2,side,side)
        .scaled(192,192,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
bool musicKind(const QString& kind,JellyfinClient::MusicList& out) {
    static const QHash<QString,JellyfinClient::MusicList> kinds{
        {"artists",JellyfinClient::MusicList::Artists},{"albums",JellyfinClient::MusicList::Albums},{"songs",JellyfinClient::MusicList::Songs},
        {"playlists",JellyfinClient::MusicList::Playlists},{"recent",JellyfinClient::MusicList::Recent},{"random",JellyfinClient::MusicList::Random},
        {"artist",JellyfinClient::MusicList::ArtistAlbums},{"album",JellyfinClient::MusicList::AlbumTracks},
        {"playlist",JellyfinClient::MusicList::PlaylistTracks},{"search",JellyfinClient::MusicList::Search}};
    const auto it=kinds.constFind(kind);if(it==kinds.constEnd())return false;
    out=it.value();return true;
}
QString joinDetail(const QStringList& parts){return parts.join(QStringLiteral(" - "));}
}

// The column row: its second line says whether the shared session is live. The row stays while a Jellyfin app is registered,
// so signing in or out changes what it says, never whether the way in exists.
void Shell::refreshJellyfinMusicRow() {
    for(auto& category:categories_)if(category.id==QLatin1String("music"))for(auto& row:category.entries) {
        if(row.action!=Action::JellyfinMusicApp)continue;
        const auto app=std::find_if(contentApps_.cbegin(),contentApps_.cend(),[&](const ContentApp& a){return a.id==row.value && a.kind=="jellyfin";});
        if(app==contentApps_.cend())continue;
        const bool live=jellyfin_.signedIn() && sameServer(QUrl(app->url),jellyfin_.serverUrl());
        if(live && jellyfinMusicLibraries_==0)row.detail=tr("No music libraries");
        else if(live || jellyfin_.hasStoredSession())row.detail=tr("Artists, albums and playlists");
        else row.detail=tr("Not signed in - Cross to sign in");
    }
}
void Shell::openJellyfinMusic(const Entry& entry) {
    const auto app=std::find_if(contentApps_.cbegin(),contentApps_.cend(),[&](const ContentApp& a){return a.id==entry.value && a.kind=="jellyfin";});
    if(app==contentApps_.cend()){showMessage(tr("This Jellyfin registration could not be found."),MessageKind::Page);emit changed();return;}
    jellyfinServiceTitle_=app->title;jellyfinRootDepth_=int(pages_.size());
    const QUrl server(app->url);
    jellyfinServiceServer_=server;jellyfinMusicApp_=app->id;
    if(jellyfin_.signedIn() && sameServer(server,jellyfin_.serverUrl())){startJellyfinMusicRoot();return;}
    if(jellyfin_.hasStoredSession() && jellyfin_.restoreSession() && sameServer(server,jellyfin_.serverUrl())) {
        qInfo().noquote()<<"jellyfin: music uses the remembered session for"<<server.toString()<<"(DPAPI record, no password)";
        jellyfinRestoring_=true;startJellyfinMusicRoot();return;
    }
    // No session: the sign-in page the TV row shows, nothing of its own. When it is accepted, authenticated() lands on the music root.
    jellyfinRequest_=0;jellyfin_.signOut();jellyfinMusicWanted_=true;
    qInfo().noquote()<<"jellyfin: music has no session; opening the shared sign-in page";
    emit requestJellyfinSignIn(server);
}
void Shell::startJellyfinMusicRoot() {
    jellyfinMusicWanted_=false;
    const bool pending=!pages_.isEmpty() && pages_.last().jellyfinMusic==QLatin1String("root");   // a refused remembered session left it waiting
    loadJellyfinMusicPage(QStringLiteral("root"),{},{},tr("Jellyfin Music"),0,pending);
}
void Shell::loadJellyfinMusicPage(const QString& kind,const QString& id,const QString& library,const QString& title,int start,bool replace) {
    if(!jellyfin_.signedIn()){showMessage(QStringLiteral("Jellyfin"),tr("Sign in to Jellyfin again from Music > Jellyfin Music."),MessageKind::Page);emit changed();return;}
    if(!replace && pages_.size()>=32){showMessage(tr("The maximum folder depth has been reached."),MessageKind::Page);emit changed();return;}
    if(!replace){Page page;page.title=title;pages_.push_back(std::move(page));}
    if(pages_.empty())return;
    auto& page=pages_.last();page.jellyfin=true;page.jellyfinViews=false;page.jellyfinSearch=false;
    page.jellyfinMusic=kind;page.jellyfinParent=id;page.jellyfinLibrary=library;page.jellyfinStart=start;
    jellyfinBackdrop_={};jellyfinBackdropId_.clear();jellyfinBackdropWanted_.clear();jellyfinBackdropDebounce_.stop();
    if(kind==QLatin1String("lib")) {   // one of several music libraries: its root needs no request
        page.entries=jellyfinMusicRootRows(library);page.selection=0;loading_=false;status_.clear();jellyfinRequest_=0;
        emit changed();return;
    }
    JellyfinClient::MusicList list{};
    loading_=true;status_=tr("Loading Jellyfin music...");jellyfinRequestDepth_=int(pages_.size());
    qInfo().noquote()<<"jellyfin: music page"<<kind<<"from"<<start<<(library.isEmpty()?QString{}:QStringLiteral("in one library"));
    if(kind==QLatin1String("root"))jellyfinRequest_=jellyfin_.views();
    else if(musicKind(kind,list))jellyfinRequest_=jellyfin_.music(list,library,id,start);
    else {loading_=false;status_.clear();jellyfinRequest_=0;showMessage(tr("This Jellyfin music page is not known."),MessageKind::Page);}
    emit changed();
}
QVector<Entry> Shell::jellyfinMusicRootRows(const QString& library) {
    QVector<Entry> rows;
    const auto node=[&](const QString& id,const QString& title,const QString& kind,const QString& detail,const QImage& mark){
        auto e=withMark(musicRow(QStringLiteral("jfm:")+id,title,Action::JellyfinMusicNode,{},kind,library,detail),mark);
        e.texture=QStringLiteral("tex_music");rows.push_back(std::move(e));
    };
    node(QStringLiteral("artists"),tr("Artists"),QStringLiteral("artists"),tr("Album artists, A to Z"),glyphs::library(QStringLiteral("music")));
    node(QStringLiteral("albums"),tr("Albums"),QStringLiteral("albums"),tr("Every album, A to Z"),glyphs::library(QStringLiteral("music")));
    node(QStringLiteral("playlists"),tr("Playlists"),QStringLiteral("playlists"),tr("Your audio playlists"),glyphs::library(QStringLiteral("playlists")));
    node(QStringLiteral("songs"),tr("Songs"),QStringLiteral("songs"),tr("Every song, A to Z"),glyphs::library(QStringLiteral("music")));
    node(QStringLiteral("recent"),tr("Recently Added"),QStringLiteral("recent"),tr("Newest albums first"),glyphs::library(QStringLiteral("music")));
    node(QStringLiteral("random"),tr("Random Albums"),QStringLiteral("random"),tr("Fifty albums, drawn fresh each time"),glyphs::library(QStringLiteral("music")));
    node(QStringLiteral("search"),tr("Search Music"),QStringLiteral("searchbox"),tr("Type an artist, an album or a song"),glyphs::search());
    rows.push_back(withMark(musicRow(QStringLiteral("jellyfin:signout"),QStringLiteral("Sign Out of Jellyfin"),Action::JellyfinSignOut),glyphs::signOut()));
    return rows;
}
void Shell::openJellyfinMusicNode(const Entry& entry) {
    const QString kind=entry.setting,library=entry.option.toString();
    if(kind==QLatin1String("searchbox")) {
        jellyfinMusicSearchLibrary_=library;
        emit requestJellyfinFieldEdit(QStringLiteral("musicsearch"),tr("Search Jellyfin Music"),jellyfinSearchTerm_,false);
        return;
    }
    if(kind==QLatin1String("lib")){loadJellyfinMusicPage(kind,{},entry.value,entry.title);return;}
    const bool listOnly=kind=="artists"||kind=="albums"||kind=="playlists"||kind=="songs"||kind=="recent"||kind=="random";
    loadJellyfinMusicPage(kind,listOnly?QString{}:entry.value,library,entry.title);
}
void Shell::openJellyfinMusicSearch(const QString& term) {
    const QString query=term.trimmed();
    if(query.isEmpty())return;
    if(!jellyfin_.signedIn()){showMessage(QStringLiteral("Jellyfin"),tr("Sign in to Jellyfin again from Music > Jellyfin Music."),MessageKind::Page);emit changed();return;}
    if(!pages_.isEmpty() && pages_.last().jellyfinMusic==QLatin1String("search"))pages_.removeLast();   // one result page at a time
    qInfo().noquote()<<"jellyfin: music search term accepted ("<<query.size()<<"character(s))";
    loadJellyfinMusicPage(QStringLiteral("search"),query,jellyfinMusicSearchLibrary_,tr("Search: %1").arg(query));
}
// The answer to a music request: the root (the server's libraries, only the music ones) or one list.
void Shell::buildJellyfinMusicPage(const JellyfinPage& result) {
    auto& page=pages_.last();
    const QString kind=page.jellyfinMusic;
    page.entries.clear();page.selection=0;
    if(kind==QLatin1String("root")) {
        QVector<JellyfinItem> libraries;
        for(const auto& view:result.items)if(view.collectionType==QLatin1String("music"))libraries.push_back(view);
        jellyfinMusicLibraries_=int(libraries.size());refreshJellyfinMusicRow();
        if(libraries.isEmpty()) {
            page.entries.push_back(withMark(musicRow(QStringLiteral("jellyfin:signout"),QStringLiteral("Sign Out of Jellyfin"),Action::JellyfinSignOut),glyphs::signOut()));
            status_=tr("This Jellyfin server has no music libraries.");
            qInfo().noquote()<<"jellyfin: the server lists no music library";
        } else if(libraries.size()==1) {
            page.jellyfinLibrary=libraries.first().id;
            page.entries=jellyfinMusicRootRows(libraries.first().id);
        } else {
            for(const auto& library:libraries) {
                auto e=withMark(musicRow(QStringLiteral("jfm:lib:")+library.id,library.title,Action::JellyfinMusicNode,library.id,QStringLiteral("lib")),
                    glyphs::library(QStringLiteral("music")));
                e.detail=tr("Music library");page.entries.push_back(std::move(e));
                if(!library.primaryTag.isEmpty() && page.entries.size()<=CoversPerPage)jellyfin_.artwork(library.id,library.primaryTag,false);
            }
            page.entries.push_back(withMark(musicRow(QStringLiteral("jellyfin:signout"),QStringLiteral("Sign Out of Jellyfin"),Action::JellyfinSignOut),glyphs::signOut()));
        }
        qInfo().noquote()<<QString("jellyfin: music root with %1 music librar%2").arg(libraries.size()).arg(libraries.size()==1?"y":"ies");
        return;
    }
    const bool albumPage=kind==QLatin1String("album");
    int covers=0;QSet<QString> asked;
    for(const auto& media:result.items) {
        if(media.type=="MusicArtist" || media.type=="MusicAlbum" || media.type=="Playlist") {
            const bool artist=media.type=="MusicArtist",album=media.type=="MusicAlbum";
            QStringList parts;
            if(album && !media.artist.isEmpty())parts<<media.artist;
            if(album && media.year>0)parts<<QString::number(media.year);
            auto e=musicRow(QStringLiteral("jellyfin:")+media.id,media.title,Action::JellyfinMusicNode,media.id,
                artist?QStringLiteral("artist"):album?QStringLiteral("album"):QStringLiteral("playlist"),page.jellyfinLibrary,joinDetail(parts));
            e.texture=QStringLiteral("music_tex_album_pic");e.iconSized=true;
            if(const auto cached=jellyfinAlbumArt_.constFind(media.id);cached!=jellyfinAlbumArt_.constEnd())e.artwork=squareCover(cached.value());
            else if(!media.primaryTag.isEmpty() && covers<CoversPerPage){++covers;jellyfin_.artwork(media.id,media.primaryTag,false);}
            page.entries.push_back(std::move(e));
            continue;
        }
        if(media.type!="Audio")continue;
        Entry e=musicRow(QStringLiteral("jellyfin:")+media.id,media.title,Action::JellyfinPlay,media.id);
        e.container=QStringLiteral("explore_plugin_full");e.texture=QStringLiteral("music_tex_album_pic");e.iconSized=true;
        e.mediaKind=QStringLiteral("Music");e.option=true;
        e.resumeMs=media.resumeMs;e.durationMs=media.durationMs;e.played=media.played;
        auto meta=std::make_shared<Entry::MusicMeta>();meta->artist=media.artist;meta->album=media.album;meta->albumId=media.albumId;meta->albumTag=media.albumTag;
        QStringList info;
        if(!media.artist.isEmpty())info<<media.artist;
        if(!albumPage && !media.album.isEmpty())info<<media.album;
        meta->info=joinDetail(info);e.music=std::move(meta);
        // The cover is the album's (a track rarely has its own); one request per album per page, and none for one already held.
        const QString coverId=!media.albumId.isEmpty() && !media.albumTag.isEmpty()?media.albumId:media.id;
        const QString coverTag=!media.albumId.isEmpty() && !media.albumTag.isEmpty()?media.albumTag:media.primaryTag;
        if(const auto cached=jellyfinAlbumArt_.constFind(coverId);cached!=jellyfinAlbumArt_.constEnd())e.artwork=squareCover(cached.value());
        else if(!coverTag.isEmpty() && !asked.contains(coverId) && covers<CoversPerPage){asked.insert(coverId);++covers;jellyfin_.artwork(coverId,coverTag,false);}
        page.entries.push_back(std::move(e));
    }
    // progressDetail() lives with the other Jellyfin rows; the rows carry the same fields it reads.
    for(auto& row:page.entries)if(row.action==Action::JellyfinPlay)row.detail=jellyfinRowDetail(row);
    const qint64 next=qint64(page.jellyfinStart)+result.items.size();
    if(kind!=QLatin1String("random") && !result.items.empty() && next<result.total && next<=1000000) {
        auto more=withMark(musicRow(QStringLiteral("jellyfin:more"),QStringLiteral("More..."),Action::JellyfinMore,page.jellyfinParent),glyphs::more());
        more.option=int(next);more.detail=page.title;page.entries.push_back(std::move(more));
    }
    if(kind==QLatin1String("search")) {
        auto again=withMark(musicRow(QStringLiteral("jfm:searchagain"),tr("Search Again"),Action::JellyfinMusicNode,{},QStringLiteral("searchbox"),page.jellyfinLibrary,
            tr("Type another artist, album or song.")),glyphs::search());
        page.entries.push_back(std::move(again));
    }
    if(page.entries.isEmpty()) {
        status_=kind==QLatin1String("search")?tr("No Jellyfin music matches that search."):
            kind==QLatin1String("playlists")?tr("This server has no audio playlists."):
            kind==QLatin1String("album") || kind==QLatin1String("playlist")?tr("This list has no tracks the server can play."):
            kind==QLatin1String("artist")?tr("This artist has no albums here."):tr("Nothing to show in this library.");
    }
    qInfo().noquote()<<QString("jellyfin: music %1 page with %2 row(s) of %3; %4 cover request(s)")
        .arg(kind).arg(page.entries.size()).arg(result.total).arg(covers);
}
// What Now Playing shows for a queue's tracks: the rows' own tags, because a server stream carries none this player reads.
void Shell::rememberJellyfinTracks(const QVector<Entry>& rows) {
    jellyfinTracks_.clear();
    for(const auto& row:rows)if(row.action==Action::JellyfinPlay)
        if(row.music)jellyfinTracks_.insert(row.value,{row.title,row.music->artist,row.music->album,row.music->albumId,row.music->albumTag});
}
void Shell::showJellyfinTrack(const QString& itemId) {
    mediaArtist_.clear();mediaTagTitle_.clear();mediaAlbum_.clear();mediaCover_=QImage();
    const auto it=jellyfinTracks_.constFind(itemId);
    if(it==jellyfinTracks_.constEnd())return;
    mediaTagTitle_=it->title;mediaArtist_=it->artist;mediaAlbum_=it->album;
    if(it->albumId.isEmpty() || it->albumTag.isEmpty())return;
    if(const auto cached=jellyfinAlbumArt_.constFind(it->albumId);cached!=jellyfinAlbumArt_.constEnd())mediaCover_=cached.value();
    else jellyfin_.artwork(it->albumId,it->albumTag,false);   // arrives through applyJellyfinMusicArt()
}
// A cover arrived (artworkReady): remember it, put it on the rows of that album, and on Now Playing when it is the playing album.
bool Shell::applyJellyfinMusicArt(const QString& itemId,const QImage& image) {
    bool wanted=false;
    for(const auto& track:jellyfinTracks_)if(track.albumId==itemId){wanted=true;break;}
    if(!wanted)for(const auto& page:pages_)if(page.jellyfin && !page.jellyfinMusic.isEmpty()){wanted=true;break;}
    if(!wanted)return false;
    if(!jellyfinAlbumArt_.contains(itemId)) {
        jellyfinAlbumArt_.insert(itemId,image);jellyfinAlbumArtOrder_.push_back(itemId);
        while(jellyfinAlbumArtOrder_.size()>AlbumArtKept)jellyfinAlbumArt_.remove(jellyfinAlbumArtOrder_.takeFirst());
    }
    const QImage square=squareCover(image);
    bool updated=false;
    for(auto& page:pages_)if(page.jellyfin && !page.jellyfinMusic.isEmpty())for(auto& row:page.entries) {
        const bool own=row.value==itemId && (row.action==Action::JellyfinMusicNode || row.action==Action::JellyfinPlay);
        if(!own && !(row.action==Action::JellyfinPlay && row.music && row.music->albumId==itemId))continue;
        row.artwork=square;row.iconSized=true;updated=true;
    }
    if(mediaOpen_ && jellyfinQueue_ && !jellyfinPlayback_.itemId.isEmpty()) {
        const auto it=jellyfinTracks_.constFind(jellyfinPlayback_.itemId);
        if(it!=jellyfinTracks_.constEnd() && it->albumId==itemId){mediaCover_=image;updated=true;}
    }
    return updated;
}
}
