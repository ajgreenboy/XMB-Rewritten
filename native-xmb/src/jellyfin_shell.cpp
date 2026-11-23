#include "shell.h"
#include "glyphs.h"
#include "media_controls.h"
#include <QMediaPlayer>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>
namespace xmb {
namespace {
Entry jfEntry(QString id,QString title,Action action,QString value={},bool folder=false) {
    Entry e;e.id=std::move(id);e.title=std::move(title);e.action=action;e.value=std::move(value);
    e.container="explore_plugin_full";e.texture=folder?"item_tex_plain_folder":"tex_video";return e;
}
// A drawn icon for a row the server has no picture for.
Entry withGlyph(Entry e,const QImage& icon){if(!icon.isNull()){e.artwork=icon;e.iconSized=true;}return e;}
bool sameService(QUrl a,QUrl b){return a.adjusted(QUrl::StripTrailingSlash|QUrl::NormalizePathSegments)==b.adjusted(QUrl::StripTrailingSlash|QUrl::NormalizePathSegments);}
bool playable(const QString& type){return QStringList{"Movie","Episode","Video","MusicVideo","Audio","Trailer","Recording"}.contains(type);}
// Posters are fetched for the rows a page can show without scrolling far. One request per row per
// page open; the client caches each picture under the data directory, keyed by the server's tag.
constexpr int ArtworkRowsPerPage = 60;
// Which Jellyfin page a backdrop belongs to: its depth, kind, parent (or search term) and offset. Back pops
// pages without reloading them, so the backdrop of a closed child must not show on its parent.
template<class Page> QString jellyfinPageKey(qsizetype depth,const Page& page) {
    return QStringLiteral("%1/%2/%3/%4").arg(depth).arg(page.jellyfinSearch?'s':page.jellyfinViews?'v':'f')
        .arg(page.jellyfinParent).arg(page.jellyfinStart);
}
}
// The row's one detail line: what it is, where it stands, how long it is. Kept to printable ASCII
// because the label renderer is the firmware's and the evidence crops are ASCII text. A music row leads with
// "artist - album" instead of the word Music and is never "Watched".
QString Shell::jellyfinRowDetail(const Entry& entry) {
    const bool music=entry.mediaKind==QLatin1String("Music");
    QStringList parts;
    if(music){if(entry.music && !entry.music->info.isEmpty())parts<<entry.music->info;}
    else if(!entry.mediaKind.isEmpty())parts<<entry.mediaKind;
    if(entry.resumeMs>0)parts<<QObject::tr("Resume at %1").arg(MediaControls::formatTime(entry.resumeMs));
    else if(entry.played && !music)parts<<QObject::tr("Watched");
    if(entry.favorite)parts<<QObject::tr("Favourite");
    if(entry.durationMs>0)parts<<MediaControls::formatTime(entry.durationMs);
    return parts.join(QStringLiteral(" - "));
}
const QImage& Shell::pageBackdrop() const {
    static const QImage none;
    if(jellyfinBackdrop_.isNull() || pages_.isEmpty() || !pages_.last().jellyfin || mediaOpen_)return none;
    return jellyfinPageKey(pages_.size(),pages_.last())==jellyfinBackdropPage_?jellyfinBackdrop_:none;
}
// The focused Jellyfin row's backdrop identity: the page it is on plus the item id (set in id), or empty when the
// focus is not on an item of an open Jellyfin page (More, Search, Sign Out, a player, another area).
QString Shell::focusedJellyfinBackdropKey(QString& id) const {
    id.clear();
    if(pages_.isEmpty() || !pages_.last().jellyfin || mediaOpen_)return {};
    const auto& page=pages_.last();
    if(page.entries.isEmpty() || page.selection<0 || page.selection>=page.entries.size())return {};
    const auto& row=page.entries.at(page.selection);
    if(row.action!=Action::JellyfinPlay && row.action!=Action::JellyfinFolder)return {};
    id=row.value;
    return jellyfinPageKey(pages_.size(),page)+QLatin1Char('|')+id;
}
void Shell::applyJellyfinBackdrop(const QString& itemId,const QImage& image) {
    QString id;const QString key=focusedJellyfinBackdropKey(id);
    if(key.isEmpty() || id!=itemId)return;
    jellyfinBackdrop_=image;jellyfinBackdropId_=itemId;
    jellyfinBackdropPage_=jellyfinPageKey(pages_.size(),pages_.last());
    emit changed();
}
// Called on every change: when the focused item differs from the one the backdrop was last arranged for, show
// that item's picture. A cached picture is applied at once; otherwise the request waits out a short debounce so
// skimming down a list asks only for where the cursor settles. The previous picture stays until the new one is
// there (the view cross-fades), but an item with no backdrop, or one that fails to load, clears it, so art never
// stays attributed to the wrong film.
void Shell::syncJellyfinBackdrop() {
    QString id;const QString key=focusedJellyfinBackdropKey(id);
    if(key.isEmpty() || key==jellyfinBackdropWanted_)return;
    jellyfinBackdropWanted_=key;
    const QString tag=jellyfinBackdropTags_.value(id);
    if(tag.isEmpty()) {
        jellyfinBackdropDebounce_.stop();
        if(!jellyfinBackdrop_.isNull()){jellyfinBackdrop_={};jellyfinBackdropId_.clear();emit changed();}
        return;
    }
    if(const auto cached=jellyfinBackdropImages_.constFind(id);cached!=jellyfinBackdropImages_.constEnd()) {
        jellyfinBackdropDebounce_.stop();
        applyJellyfinBackdrop(id,cached.value());
        return;
    }
    jellyfinBackdropDebounce_.start();
}
void Shell::initJellyfin() {
    jellyfin_.setImageCache(QDir(dataDirectory_).filePath("jellyfin/images"));
    jellyfin_.setSessionStore(QDir(dataDirectory_).filePath("jellyfin/session.bin"));
    connect(&jellyfin_,&JellyfinClient::authenticated,this,[this]{
        if(!jellyfinSigningIn_)return;
        jellyfinSigningIn_=false;emit jellyfinSignInFinished(true,{});
        // The opt-in is the page's Remember row, and the stored record's existence is the only
        // durable form of it: turning the row off deletes the record on the way out.
        if(signInRemember_) {
            const bool saved=jellyfin_.saveSession();
            qInfo().noquote()<<(saved?"jellyfin: remembered sign-in stored (DPAPI, this Windows user; no password kept)"
                                      :"jellyfin: could not store the remembered sign-in; this session only");
        } else {
            jellyfin_.forgetSession();
            qInfo()<<"jellyfin: stay-signed-in is off; nothing was written";
        }
        refreshJellyfinMusicRow();
        if(jellyfinMusicWanted_){startJellyfinMusicRoot();return;}
        loadJellyfinPage({},jellyfinServiceTitle_,0,true);
    });
    connect(&jellyfin_,&JellyfinClient::failed,this,[this](quint64 request,const QString& error){
        if(request==0){if(jellyfinSigningIn_){jellyfinSigningIn_=false;emit jellyfinSignInFinished(false,error);}return;}
        if(request!=jellyfinRequest_)return;
        jellyfinRequest_=0;loading_=false;status_.clear();
        if(jellyfinPlayAll_.request==request)jellyfinPlayAll_={};
        // A remembered session the server no longer accepts: forget it and show the sign-in page,
        // because that is the surface that can fix it.
        if(jellyfinRestoring_) {
            jellyfinRestoring_=false;
            jellyfin_.signOut();
            jellyfin_.forgetSession();
            qWarning().noquote()<<"jellyfin: the remembered session was refused; it has been forgotten, signing in again";
            jellyfinMusicWanted_=!pages_.isEmpty() && !pages_.last().jellyfinMusic.isEmpty();
            jellyfinMusicLibraries_=-1;refreshJellyfinMusicRow();
            openJellyfinSignInPage(jellyfinServiceServer_);
            return;
        }
        if(jellyfinQueue_ && mediaOpen_){finishJellyfinPlayback();player_->stop();}
        showMessage(QStringLiteral("Jellyfin"),error,MessageKind::Page);
        if(!pages_.isEmpty() && pages_.last().jellyfin && pages_.last().entries.isEmpty() && pages_.size()==jellyfinRequestDepth_)pages_.removeLast();   // the list that failed to load is not a page to stay on
        emit changed();
    });
    connect(&jellyfin_,&JellyfinClient::pageReady,this,[this](quint64 request,const JellyfinPage& result){
        if(request!=jellyfinRequest_ || pages_.size()!=jellyfinRequestDepth_ || pages_.isEmpty() || !pages_.last().jellyfin)return;
        jellyfinRequest_=0;loading_=false;status_.clear();jellyfinRestoring_=false;auto& page=pages_.last();
        if(jellyfinPlayAll_.request==request){continueJellyfinPlayAll(result);emit changed();return;}   // Play All, not a page of rows
        if(!page.jellyfinMusic.isEmpty()){buildJellyfinMusicPage(result);emit changed();return;}
        const QString selected=page.entries.isEmpty()?QString{}:page.entries.value(page.selection).id;
        page.entries.clear();page.selection=0;
        int posters=0,backdrops=0;
        if(jellyfinBackdropTags_.size()>4000)jellyfinBackdropTags_.clear();
        int musicLibraries=0;
        // The server's own home lists lead the libraries page: what is half watched and what comes next.
        if(page.jellyfinViews) {
            auto resume=withGlyph(jfEntry("jellyfin:home-resume",tr("Continue Watching"),Action::JellyfinFolder,"home:resume",true),glyphs::play());
            resume.detail=tr("Titles you started on any Jellyfin device, at the server's saved position.");
            if(resume.id==selected)page.selection=int(page.entries.size());
            page.entries.push_back(std::move(resume));
            auto next=withGlyph(jfEntry("jellyfin:home-nextup",tr("Next Up"),Action::JellyfinFolder,"home:nextup",true),glyphs::check());
            next.detail=tr("The next episode of every show you are watching.");
            if(next.id==selected)page.selection=int(page.entries.size());
            page.entries.push_back(std::move(next));
            auto favorites=withGlyph(jfEntry("jellyfin:home-favorites",tr("Favourites"),Action::JellyfinFolder,"home:favorites",true),glyphs::likedSongs());
            favorites.detail=tr("Titles you flagged as favourites on the server.");
            if(favorites.id==selected)page.selection=int(page.entries.size());
            page.entries.push_back(std::move(favorites));
            // One Latest row per video library: its newest additions, from the server's own list.
            for(const auto& view:result.items) {
                if(!jfplayer::latestEligible(view.collectionType) || view.collectionType==QLatin1String("music"))continue;
                auto latest=withGlyph(jfEntry("jellyfin:home-latest:"+view.id,tr("Latest in %1").arg(view.title),Action::JellyfinFolder,"home:latest:"+view.id,true),glyphs::library(view.collectionType));
                latest.detail=tr("The newest additions to %1.").arg(view.title);
                if(latest.id==selected)page.selection=int(page.entries.size());
                page.entries.push_back(std::move(latest));
            }
        }
        for(const auto& media:result.items) {
            // Music libraries live in the Music column (jellyfin_music_shell.cpp); the TV page keeps the video ones.
            if(page.jellyfinViews && media.collectionType==QLatin1String("music")){++musicLibraries;continue;}
            // Every item's backdrop tag is kept so the focused row can ask for its own picture (syncJellyfinBackdrop).
            if(media.backdropTag.isEmpty())jellyfinBackdropTags_.remove(media.id);
            else{jellyfinBackdropTags_.insert(media.id,media.backdropTag);++backdrops;}
            const bool folder=page.jellyfinViews || media.folder;
            auto e=jfEntry("jellyfin:"+media.id,media.title,folder?Action::JellyfinFolder:playable(media.type)?Action::JellyfinPlay:Action::Unavailable,media.id,folder);
            e.mediaKind=folder?QString{}:media.type=="Audio"?QString("Music"):media.type=="Movie"?QString("Movie"):media.type=="Episode"?QString("Episode"):QString("Video");e.option=media.type=="Audio";
            if(!folder){e.resumeMs=media.resumeMs;e.durationMs=media.durationMs;e.played=media.played;e.favorite=media.favorite;e.seriesId=media.seriesId;e.detail=jellyfinRowDetail(e);}
            // A series, season or collection carries its own flags; a library (the root page) does not.
            else if(!page.jellyfinViews){e.played=media.played;e.favorite=media.favorite;if(e.played||e.favorite)e.detail=jellyfinRowDetail(e);}
            if(media.type=="Audio")e.texture="tex_music";
            // The home lists mix shows, so an episode names its show and its place in it.
            if(!page.jellyfinHome.isEmpty() && media.type=="Episode" && !media.seriesName.isEmpty()) {
                const QString place=media.seasonNumber>0 && media.episodeNumber>0?QStringLiteral(" S%1E%2").arg(media.seasonNumber).arg(media.episodeNumber):QString{};
                e.title=QStringLiteral("%1%2 - %3").arg(media.seriesName,place,media.title);
            }
            // A library wears its kind's glyph until (unless) the server's own library picture arrives.
            if(page.jellyfinViews)e=withGlyph(std::move(e),glyphs::library(media.collectionType));
            // Folders (libraries, series, seasons, albums) fetch their poster as items do.
            if(folder && !media.primaryTag.isEmpty() && posters<ArtworkRowsPerPage) {
                ++posters;
                jellyfin_.artwork(media.id,media.primaryTag,false);
            }
            if(!folder && e.action!=Action::Unavailable) {
                if(!media.primaryTag.isEmpty() && posters<ArtworkRowsPerPage) {
                    ++posters;
                    jellyfin_.artwork(media.id,media.primaryTag,false);
                }
            }
            if(e.id==selected)page.selection=int(page.entries.size());page.entries.push_back(std::move(e));
        }
        if(page.jellyfinViews){jellyfinMusicLibraries_=musicLibraries;refreshJellyfinMusicRow();}
        const qint64 next=qint64(page.jellyfinStart)+result.items.size();
        if(!page.jellyfinViews && !result.items.empty() && next<result.total && next<=1000000){auto more=withGlyph(jfEntry("jellyfin:more","More...",Action::JellyfinMore,page.jellyfinParent,true),glyphs::more());more.option=int(next);more.detail=page.title;page.entries.push_back(std::move(more));}
        if(page.jellyfinSearch) {
            auto again=withGlyph(jfEntry("jellyfin:search",tr("Search Again"),Action::JellyfinSearch),glyphs::search());
            again.detail=tr("Type another title, episode or album name.");
            page.entries.push_back(std::move(again));
        }
        if(page.jellyfinViews) {
            auto search=withGlyph(jfEntry("jellyfin:search",tr("Search Jellyfin"),Action::JellyfinSearch),glyphs::search());
            search.detail=tr("Type a title, an episode or an album name to search the server.");
            page.entries.push_back(std::move(search));
            auto out=withGlyph(jfEntry("jellyfin:signout","Sign Out of Jellyfin",Action::JellyfinSignOut),glyphs::signOut());page.entries.push_back(std::move(out));
        }
        if(page.entries.empty())status_=page.jellyfinSearch?"No Jellyfin items match that search.":
            page.jellyfinHome==QLatin1String("resume")?"Nothing to continue watching on Jellyfin.":
            page.jellyfinHome==QLatin1String("nextup")?"No next episodes to watch on Jellyfin.":
            page.jellyfinHome==QLatin1String("favorites")?"No favourites on Jellyfin yet.":
            page.jellyfinHome.startsWith(QLatin1String("latest"))?"Nothing new in this Jellyfin library.":"There are no supported items in this library.";
        qInfo().noquote()<<QString("jellyfin: %1 page with %2 row(s); %3 poster request(s)%4")
            .arg(page.jellyfinViews?QStringLiteral("library"):page.jellyfinSearch?QStringLiteral("search"):QStringLiteral("folder"))
            .arg(page.entries.size()).arg(posters)
            .arg(backdrops?QStringLiteral(" and %1 backdrop tag(s)").arg(backdrops):QString{});
        emit changed();
    });
    connect(&jellyfin_,&JellyfinClient::artworkReady,this,[this](quint64,const QString& itemId,bool backdrop,const QImage& image){
        if(backdrop) {
            // A completion for an item that is no longer focused is kept (a quick return to it is then free)
            // but never drawn; a failed load for the focused item clears the previous item's picture.
            QString focusedId;
            const bool focused=!focusedJellyfinBackdropKey(focusedId).isEmpty() && focusedId==itemId;
            if(image.isNull()){
                if(focused && !jellyfinBackdrop_.isNull()){jellyfinBackdrop_={};jellyfinBackdropId_.clear();emit changed();}
                return;
            }
            jellyfinBackdropImages_.insert(itemId,image);
            jellyfinBackdropOrder_.removeAll(itemId);jellyfinBackdropOrder_.push_back(itemId);
            while(jellyfinBackdropOrder_.size()>12)jellyfinBackdropImages_.remove(jellyfinBackdropOrder_.takeFirst());
            if(focused)applyJellyfinBackdrop(itemId,image);
            return;
        }
        if(image.isNull())return;
        bool updated=applyJellyfinMusicArt(itemId,image);   // music rows and Now Playing (jellyfin_music_shell.cpp)
        for(auto& page:pages_)if(page.jellyfin && page.jellyfinMusic.isEmpty())for(auto& row:page.entries) {
            if(row.value!=itemId || (row.action!=Action::JellyfinPlay && row.action!=Action::JellyfinFolder))continue;
            row.artwork=image;row.iconSized=false;updated=true;
        }
        if(updated)emit changed();
    });
    // The focused row owns the page backdrop: every change of focus (or of the open page) re-evaluates it.
    jellyfinBackdropDebounce_.setSingleShot(true);jellyfinBackdropDebounce_.setInterval(120);
    connect(&jellyfinBackdropDebounce_,&QTimer::timeout,this,[this]{
        QString id;const QString key=focusedJellyfinBackdropKey(id);
        if(key.isEmpty() || key!=jellyfinBackdropWanted_)return;   // focus moved on: that move restarted the timer
        const QString tag=jellyfinBackdropTags_.value(id);
        if(!tag.isEmpty())jellyfin_.artwork(id,tag,true);
    });
    connect(this,&Shell::changed,this,[this]{syncJellyfinBackdrop();});
    connect(&jellyfin_,&JellyfinClient::playbackReady,this,[this](quint64 request,const JellyfinPlayback& playback){
        if(request!=jellyfinRequest_ || !jellyfinQueue_ || !mediaOpen_)return;
        jellyfinRequest_=0;jellyfinPlayback_=playback;jellyfinStreamIssue_={};jellyfinProbe_=0;
        QStringList streams;
        for(const auto& stream:playback.streams)streams<<stream.type+":"+stream.title;
        qInfo().noquote()<<"jellyfin: playback"<<playback.itemId<<playback.playMethod<<playback.streams.size()
            <<"server stream(s)"<<streams.join('|');
        // A title with a remembered audio stream the server did not start with is re-requested with it, but only when
        // the player cannot switch tracks itself (a transcode); a direct play switches locally once it has loaded.
        if(!playback.audio && playback.audioStreamIndex<0 && playback.playMethod!=QLatin1String("DirectPlay")) {
            const auto remembered=recallChoice();
            const int pick=remembered.audio>=0?jfplayer::pickAudioStream(playback.streams,remembered.audio):-1;
            if(pick>=0 && pick!=jfplayer::pickAudioStream(playback.streams,-1)) {
                qInfo().noquote()<<"jellyfin: re-requesting with the remembered audio stream"<<pick<<"for"<<playback.itemId;
                jellyfinAudioReopen_.insert(playback.itemId,pick);
                const auto burn=jellyfinBurnIn_.value(playback.itemId,{-1,QString{}});
                jellyfinRequest_=jellyfin_.playback(playback.itemId,false,preferences_.value("jellyfin/maxBitrateKbps").toInt(),burn.first,burn.second,pick);
                return;
            }
        }
        // Chapter marks: from PlaybackInfo when it carried them, else asked for once (silently empty when none).
        QVector<jfplayer::Chapter> marks;
        for(const auto& chapter:playback.chapters)marks.push_back({chapter.startMs,chapter.name});
        mediaChapters_=jfplayer::normalizedChapters(marks);
        jellyfinChapterRequest_=0;
        if(mediaChapters_.size()<2 && !playback.audio)jellyfinChapterRequest_=jellyfin_.chapters(playback.itemId);
        else qInfo().noquote()<<"jellyfin:"<<mediaChapters_.size()<<"chapter mark(s) from PlaybackInfo";
        mediaLyrics_.clear();jellyfinLyricsRequest_=playback.audio?jellyfin_.lyrics(playback.itemId):0;   // silently none when the server has no lyrics
        requestJellyfinSubtitles();
        const auto local=jellyfinStream_.open(playback.url,jellyfin_.authorizationHeader());
        if(!local.isValid() || local.isEmpty()){advanceMedia(true,{});return;}
        player_->setSource(local);player_->play();emit changed();
    });
    connect(&jellyfin_,&JellyfinClient::chaptersReady,this,[this](quint64 request,const QString& itemId,const QVector<JellyfinChapter>& list){
        if(request!=jellyfinChapterRequest_ || itemId!=jellyfinPlayback_.itemId || !mediaOpen_ || !jellyfinQueue_)return;
        jellyfinChapterRequest_=0;
        QVector<jfplayer::Chapter> marks;
        for(const auto& chapter:list)marks.push_back({chapter.startMs,chapter.name});
        mediaChapters_=jfplayer::normalizedChapters(marks);
        qInfo().noquote()<<"jellyfin:"<<mediaChapters_.size()<<"chapter mark(s) from the item";
        refreshPlayerOptions();emit changed();
    });
    // Lyrics: only timed lines are kept, and only for the song that is still playing.
    connect(&jellyfin_,&JellyfinClient::lyricsReady,this,[this](quint64 request,const QString& itemId,const QVector<JellyfinLyricLine>& lines,bool synced){
        if(request!=jellyfinLyricsRequest_ || itemId!=jellyfinPlayback_.itemId || !mediaOpen_ || !jellyfinQueue_)return;
        jellyfinLyricsRequest_=0;
        if(lines.isEmpty() || !synced){qInfo().noquote()<<"jellyfin: no timed lyrics for"<<itemId<<(lines.isEmpty()?"(none)":"(not timed, not shown)");return;}
        mediaLyrics_=lines;
        qInfo().noquote()<<"jellyfin:"<<lines.size()<<"timed lyric line(s)";
        emit changed();
    });
    // The answer to a watched or favourite toggle: the row changes only when the server agreed (never before).
    connect(&jellyfin_,&JellyfinClient::userFlagChanged,this,[this](quint64 request,const QString& itemId,int flag,bool value,bool ok){
        if(!userFlagRequests_.contains(request))return;   // not a toggle of ours
        userFlagRequests_.remove(request);
        const bool favorite=flag==int(JellyfinClient::UserFlag::Favorite);
        if(!ok) {
            qWarning().noquote()<<"jellyfin:"<<(favorite?"favourite":"watched")<<"change for"<<itemId<<"was not accepted by the server; the row is unchanged";
            notify(QStringLiteral("Jellyfin"),favorite?tr("The server did not accept the favourite change."):tr("The server did not accept the watched change."));
            emit changed();
            return;
        }
        qInfo().noquote()<<"jellyfin:"<<(favorite?"favourite":"watched")<<(value?"set":"cleared")<<"for"<<itemId;
        applyJellyfinFlag(itemId,flag,value);
        notify(QStringLiteral("Jellyfin"),favorite?(value?tr("Added to favourites"):tr("Removed from favourites")):(value?tr("Marked as watched"):tr("Marked as unwatched")));
        emit changed();
    });
    // The server named the episode that follows the one that just ended: count down to it (Circle cancels), or, when it
    // named none (or could not be asked), carry on exactly as before - the next row of the page, or the end of the queue.
    connect(&jellyfin_,&JellyfinClient::nextEpisodeReady,this,[this](quint64 request,const JellyfinItem& item,bool found){
        if(request!=nextLookup_)return;
        nextLookup_=0;
        if(nextLookupMedia_!=mediaRequest_ || !mediaOpen_ || !jellyfinQueue_)return;   // the viewer has left or started something else
        status_.clear();
        if(found){startNextOffer(item);return;}
        qInfo().noquote()<<"jellyfin: no next episode offered by the server";
        advanceMediaQueue(false,{});
    });
    nextOfferTimer_.setInterval(250);
    connect(&nextOfferTimer_,&QTimer::timeout,this,[this]{
        if(!nextOffer_.active || !mediaOpen_ || !jellyfinQueue_){cancelNextOffer();return;}
        const int left=jfplayer::autoplaySecondsLeft(nextOfferClock_.elapsed());
        if(left<=0){playNextOffer();return;}
        if(left!=nextOffer_.left){nextOffer_.left=left;emit changed();}
    });
    // A server subtitle stream arrived: parse it with the sidecar parser, fill its track, and select it when
    // the server marks it default and this item has no session choice yet.
    connect(&jellyfin_,&JellyfinClient::subtitleReady,this,[this](quint64 request,int streamIndex,const QByteArray& bytes){
        if(!jellyfinSubtitleRequests_.contains(request))return;
        jellyfinSubtitleRequests_.remove(request);
        if(!jellyfinQueue_ || !mediaOpen_)return;
        const QString key=jellyfinSubtitleKey(streamIndex);
        int track=-1;
        for(int i=0;i<subtitleTracks_.size();++i)if(subtitleTracks_[i].path==key){track=i;break;}
        if(track<0)return;
        // A PGS stream's .sup: decoded on a worker like a sidecar. When the server would not deliver it, the
        // reason is kept and the row turns into the burn-in choice.
        if(subtitleTracks_[track].image) {
            subtitleTracks_[track].decoding=false;
            if(bytes.isEmpty()) {
                const QString reason=tr("The server did not deliver this PGS stream as .sup");
                jellyfinSubtitleErrors_.insert(streamIndex,reason);
                qWarning().noquote()<<"jellyfin: subtitle stream"<<streamIndex<<"unusable:"<<reason;
                if(track==subtitleTrackIndex_){subtitleTrackIndex_=-1;subtitleCueNumber_=0;}
            } else {
                qInfo().noquote()<<"jellyfin: PGS subtitle stream"<<streamIndex<<"arrived,"<<bytes.size()<<"bytes; decoding";
                startImageDecode(track,bytes);
            }
            refreshPlayerOptions();
            emit changed();
            return;
        }
        QString reason;
        if(bytes.isEmpty())reason=tr("The server did not deliver this subtitle stream.");
        else if(!subtitles::parse(bytes,QStringLiteral("srt"),subtitleTracks_[track],&reason))
            reason=tr("The server's subtitle stream has no usable cue (%1).").arg(reason);
        if(!reason.isEmpty()) {
            jellyfinSubtitleErrors_.insert(streamIndex,reason);
            qWarning().noquote()<<"jellyfin: subtitle stream"<<streamIndex<<"unusable:"<<reason;
            refreshPlayerOptions();
            return;
        }
        qInfo().noquote()<<"jellyfin: subtitle stream"<<streamIndex<<subtitleTracks_[track].name<<subtitleTracks_[track].cues.size()
            <<"cues"<<bytes.size()<<"bytes; skipped"<<subtitleTracks_[track].skipped;
        bool isDefault=false;
        for(const auto& stream:jellyfinPlayback_.streams)if(stream.index==streamIndex)isDefault=stream.isDefault;
        if(subtitleLog_)qInfo().noquote()<<"subtitle: stream"<<streamIndex<<"default"<<isDefault
            <<"choice" <<(subtitleChoice_.contains(subtitleMediaPath_)?subtitleChoice_.value(subtitleMediaPath_):QStringLiteral("<none>"));
        if(subtitleChoice_.contains(subtitleMediaPath_)) {
            if(subtitleChoice_.value(subtitleMediaPath_)==key)applySubtitleTrack(track);
        } else if(isDefault && subtitleTrackIndex_<0) {
            applySubtitleTrack(track);
        }
        refreshPlayerOptions();
        emit changed();
    });
    // What the stream saw: the server's status and the first bytes of its reply (scrubbed), or the stream's own
    // reason for refusing to relay. Logged at once, and kept for the message if the player then gives up.
    connect(&jellyfinStream_,&JellyfinStream::upstreamRejected,this,[this](int status,const QString& request,const QString& detail){
        qWarning().noquote()<<"jellyfin: stream request rejected:"<<request<<(status?QStringLiteral("-> HTTP %1").arg(status):QStringLiteral("(not relayed)"))
            <<(detail.isEmpty()?QString{}:QStringLiteral("-- \"%1\"").arg(detail));
        if(!jellyfinStreamIssue_.valid || (status>=500 && jellyfinStreamIssue_.status<500))
            jellyfinStreamIssue_={status,request,detail,true};
    });
    connect(&jellyfinStream_,&JellyfinStream::failed,this,[this](const QString& reason){
        if(!jellyfinQueue_ || !mediaOpen_)return;const auto request=mediaRequest_;
        QTimer::singleShot(0,this,[this,request,reason]{if(request==mediaRequest_ && mediaOpen_ && jellyfinQueue_)advanceMedia(true,reason);});
    });
    connect(&jellyfin_,&JellyfinClient::probeFinished,this,[this](quint64 request,int status,const QString& detail){
        if(request!=jellyfinProbe_)return;
        jellyfinProbe_=0;
        reportJellyfinFailure(status,detail,true);
    });
    connect(player_.get(),&QMediaPlayer::playbackStateChanged,this,[this]{
        if(jellyfinResumePending_>0)applyJellyfinResume();else reportJellyfinPlaybackState();
    });
    connect(player_.get(),&QMediaPlayer::seekableChanged,this,[this]{applyJellyfinResume();});
    connect(player_.get(),&QMediaPlayer::durationChanged,this,[this]{applyJellyfinResume();});
    connect(player_.get(),&QMediaPlayer::mediaStatusChanged,this,[this](QMediaPlayer::MediaStatus state){
        if(state!=QMediaPlayer::LoadedMedia && state!=QMediaPlayer::BufferedMedia)return;
        applyRememberedJellyfinAudio();
        applyJellyfinResume();
        if(jellyfinQueue_ && jellyfinResumePending_>0) {
            const auto request=mediaRequest_;
            QTimer::singleShot(1500,this,[this,request]{
                if(request!=mediaRequest_ || !jellyfinQueue_ || !mediaOpen_ || jellyfinResumePending_<=0)return;
                applyJellyfinResume();
                if(jellyfinResumePending_>0 && !player_->isSeekable()) {
                    qWarning().noquote()<<"jellyfin: this stream cannot seek; playing from the beginning instead of resuming at"
                        <<jellyfinResumePending_<<"ms";
                    jellyfinResumePending_=0;status_=tr("This stream cannot seek; playing from the beginning.");
                    reportJellyfinPlaybackState();emit changed();
                }
            });
        }
    });
    jellyfinProgress_.setInterval(10000);
    connect(&jellyfinProgress_,&QTimer::timeout,this,[this]{
        if(!jellyfinQueue_ || !mediaOpen_ || !jellyfinStarted_)return;
        qInfo().noquote()<<"jellyfin: progress report at"<<player_->position()<<"ms";
        jellyfin_.reportPlayback(jellyfinPlayback_,player_->position(),mediaPaused(),"progress");
    });
    jellyfinProgress_.start();
}
void Shell::openJellyfin(const Entry& entry) {
    const auto app=std::find_if(contentApps_.cbegin(),contentApps_.cend(),[&](const ContentApp& candidate){return candidate.id==entry.value && candidate.kind=="jellyfin";});
    if(app==contentApps_.cend()){showMessage(QStringLiteral("This Jellyfin registration could not be found."),MessageKind::Page);return;}
    jellyfinMusicWanted_=false;
    jellyfinServiceTitle_=app->title;jellyfinRootDepth_=int(pages_.size());
    const QUrl server(app->url);
    jellyfinServiceServer_=server;
    if(jellyfin_.signedIn() && sameService(server,jellyfin_.serverUrl()))loadJellyfinPage({},jellyfinServiceTitle_,0,true);
    // A stored session is the explicit "stay signed in" opt-in from the sign-in page; it is only
    // used for the registration it was made for.
    else if(jellyfin_.hasStoredSession() && jellyfin_.restoreSession() && sameService(server,jellyfin_.serverUrl())) {
        qInfo().noquote()<<"jellyfin: using the remembered session for"<<server.toString()<<"(DPAPI record, no password)";
        jellyfinRestoring_=true;
        loadJellyfinPage({},jellyfinServiceTitle_,0,true);
    } else {jellyfinRequest_=0;jellyfin_.signOut();emit requestJellyfinSignIn(server);}
}
void Shell::authenticateJellyfin(QUrl server,QString username,QString password){jellyfinSigningIn_=true;jellyfin_.signIn(server,std::move(username),std::move(password));}
void Shell::cancelJellyfinSignIn(){jellyfinSigningIn_=false;jellyfinMusicWanted_=false;jellyfinRequest_=0;jellyfin_.signOut();loading_=false;status_.clear();emit changed();}
void Shell::loadJellyfinPage(const QString& parent,const QString& title,int start,bool views,bool replace) {
    if(!jellyfin_.signedIn()){showMessage(QStringLiteral("Jellyfin"),QStringLiteral("Sign out and reopen the app to sign in again."),MessageKind::Page);emit changed();return;}
    if(!replace && pages_.size()>=32){showMessage(QStringLiteral("The maximum folder depth has been reached."),MessageKind::Page);emit changed();return;}
    if(!replace){Page page;page.title=title;pages_.push_back(std::move(page));}
    if(pages_.empty())return;
    auto& page=pages_.last();page.jellyfin=true;page.jellyfinViews=views;page.jellyfinParent=parent;page.jellyfinStart=start;
    jellyfinBackdrop_={};jellyfinBackdropId_.clear();jellyfinBackdropWanted_.clear();jellyfinBackdropDebounce_.stop();
    loading_=true;status_="Loading Jellyfin...";jellyfinRequestDepth_=int(pages_.size());
    jellyfinRequest_=views?jellyfin_.views():jellyfin_.items(parent,start);emit changed();
}
// The server's own lists behind the root page's Continue Watching and Next Up rows (audit/JELLYFIN-PLAYER.md). The rows
// of the answer carry the server's UserData, so a position saved by another Jellyfin client resumes here.
void Shell::loadJellyfinHomePage(const QString& kind,const QString& title) {
    // "resume", "nextup", "favorites", or "latest:<library id>".
    const QString listKind=kind.section(QLatin1Char(':'),0,0),library=kind.section(QLatin1Char(':'),1);
    if(listKind!=QLatin1String("resume") && listKind!=QLatin1String("nextup") && listKind!=QLatin1String("favorites") && listKind!=QLatin1String("latest"))return;
    if(!jellyfin_.signedIn()){showMessage(QStringLiteral("Jellyfin"),QStringLiteral("Sign out and reopen the app to sign in again."),MessageKind::Page);emit changed();return;}
    if(pages_.size()>=32){showMessage(QStringLiteral("The maximum folder depth has been reached."),MessageKind::Page);emit changed();return;}
    Page page;page.title=title;page.jellyfin=true;page.jellyfinHome=listKind;page.jellyfinParent=QStringLiteral("home:")+kind;
    pages_.push_back(std::move(page));
    jellyfinBackdrop_={};jellyfinBackdropId_.clear();jellyfinBackdropWanted_.clear();jellyfinBackdropDebounce_.stop();
    loading_=true;status_=tr("Loading Jellyfin...");jellyfinRequestDepth_=int(pages_.size());
    qInfo().noquote()<<"jellyfin: home list"<<kind;
    jellyfinRequest_=jellyfin_.home(listKind==QLatin1String("nextup")?JellyfinClient::HomeList::NextUp:listKind==QLatin1String("favorites")?JellyfinClient::HomeList::Favorites:
        listKind==QLatin1String("latest")?JellyfinClient::HomeList::Latest:JellyfinClient::HomeList::Resume,library);
    emit changed();
}
void Shell::openJellyfinSearch(const QString& term) {
    const QString query=term.trimmed();
    if(query.isEmpty())return;
    if(!jellyfin_.signedIn()){showMessage(QStringLiteral("Jellyfin"),QStringLiteral("Sign out and reopen the app to sign in again."),MessageKind::Page);emit changed();return;}
    if(pages_.size()>=32){showMessage(QStringLiteral("The maximum folder depth has been reached."),MessageKind::Page);emit changed();return;}
    // One search page at a time: a second search replaces the first result page instead of stacking.
    if(!pages_.isEmpty() && pages_.last().jellyfinSearch)pages_.removeLast();
    const QString title=tr("Search: %1").arg(query);
    Page page;page.title=title;
    page.jellyfin=true;page.jellyfinSearch=true;page.jellyfinParent=query;
    pages_.push_back(std::move(page));
    loadJellyfinSearchPage(query,title,0,true);
}
void Shell::loadJellyfinSearchPage(const QString& term,const QString& title,int start,bool replace) {
    if(!jellyfin_.signedIn()){showMessage(QStringLiteral("Jellyfin"),QStringLiteral("Sign out and reopen the app to sign in again."),MessageKind::Page);emit changed();return;}
    if(!replace){Page page;page.title=title;page.jellyfinSearch=true;pages_.push_back(std::move(page));}
    if(pages_.empty())return;
    auto& page=pages_.last();page.jellyfin=true;page.jellyfinSearch=true;page.jellyfinParent=term;page.jellyfinStart=start;
    jellyfinBackdrop_={};jellyfinBackdropId_.clear();jellyfinBackdropWanted_.clear();jellyfinBackdropDebounce_.stop();
    loading_=true;status_=tr("Searching Jellyfin...");jellyfinRequestDepth_=int(pages_.size());
    qInfo().noquote()<<"jellyfin: search"<<term<<"from"<<start;
    jellyfinRequest_=jellyfin_.search(term,start);emit changed();
}
void Shell::openJellyfinMedia(const Entry& entry,bool fromBeginning) {
    QVector<Entry> rows;int selected=-1;
    const bool homePage=!pages_.isEmpty() && !pages_.last().jellyfinHome.isEmpty();
    for(const auto& row:entries())if(row.action==Action::JellyfinPlay && (!homePage || row.id==entry.id)){if(row.id==entry.id)selected=int(rows.size());rows.push_back(row);}
    if(selected<0)return;
    qInfo().noquote()<<QString("jellyfin: %1 \"%2\" (server position %3 ms)")
        .arg(fromBeginning?QStringLiteral("play from the beginning"):QStringLiteral("play"))
        .arg(entry.title).arg(fromBeginning?0:entry.resumeMs);
    playJellyfinQueue(rows,selected,fromBeginning);
}
// Queue `rows` (Jellyfin playable rows) and start the one at `selected`: the page's rows for Cross on a track, the tracks of an album,
// an artist or a playlist for Play All.
void Shell::playJellyfinQueue(const QVector<Entry>& rows,int selected,bool fromBeginning) {
    if(selected<0 || selected>=rows.size())return;
    QVector<PlaybackQueue::Item> queue;jellyfinAudio_.clear();jellyfinResume_.clear();jellyfinSeries_.clear();jellyfinKinds_.clear();
    for(const auto& row:rows){queue.push_back({row.value,row.title});jellyfinAudio_.insert(row.value,row.option.toBool());jellyfinResume_.insert(row.value,row.resumeMs);jellyfinSeries_.insert(row.value,row.seriesId);jellyfinKinds_.insert(row.value,row.mediaKind);}
    if(fromBeginning)jellyfinResume_.insert(rows.at(selected).value,0);
    rememberJellyfinTracks(rows);
    jellyfinQueue_=true;mediaQueue_.clear();mediaQueue_.setRepeat(static_cast<PlaybackQueue::Repeat>(mediaRepeatMode()));
    mediaQueue_.setShuffle(preferences_.value("media/shuffle").toBool());mediaQueue_.reset(std::move(queue),selected);mediaFailures_=0;startJellyfinMedia();
}
QString Shell::mediaLyricCurrent() const {
    const int at=jfplayer::lyricIndexAt(mediaLyrics_,player_->position());
    return at<0?QString{}:mediaLyrics_.at(at).text;
}
QString Shell::mediaLyricNext() const {
    if(mediaLyrics_.isEmpty())return {};
    const int at=jfplayer::lyricIndexAt(mediaLyrics_,player_->position())+1;
    return at<mediaLyrics_.size()?mediaLyrics_.at(at).text:QString{};
}
// The streaming ceilings offered for Jellyfin, in kbit/s, with their labels.
const QVector<QPair<int,QString>>& Shell::jellyfinQualities() {
    static const QVector<QPair<int,QString>> list{
        {120000,QStringLiteral("Maximum (120 Mbps)")},{40000,QStringLiteral("40 Mbps")},{20000,QStringLiteral("20 Mbps")},
        {10000,QStringLiteral("10 Mbps")},{8000,QStringLiteral("8 Mbps")},{4000,QStringLiteral("4 Mbps")},
        {2000,QStringLiteral("2 Mbps")},{1000,QStringLiteral("1 Mbps")},{720,QStringLiteral("720 kbps")}};
    return list;
}
QString Shell::jellyfinQualityText() const {
    const int current=preferences_.value("jellyfin/maxBitrateKbps").toInt();
    for(const auto& q:jellyfinQualities())if(q.first==current)return q.second;
    return QStringLiteral("%1 kbps").arg(current);
}
void Shell::openJellyfinQualityList() {
    optionsParent_=options_;optionsNested_=true;
    optionsTitle_=tr("Streaming Quality");
    options_.clear();optionsSelection_=0;
    const int current=preferences_.value("jellyfin/maxBitrateKbps").toInt();
    for(int i=0;i<jellyfinQualities().size();++i) {
        MenuOption row{jellyfinQualities()[i].second,OptionCommand::JellyfinQualityTarget};
        row.index=i;row.active=jellyfinQualities()[i].first==current;
        row.note=i==0?tr("Direct play whenever the file allows it."):tr("The server converts anything above this down to fit.");
        if(row.active)optionsSelection_=i;
        options_.push_back(row);
    }
    interactionAudio_.play(InteractionAudio::Cue::Options);
    emit changed();
}
void Shell::chooseJellyfinQuality(int index) {
    if(index<0 || index>=jellyfinQualities().size())return;
    const auto choice=jellyfinQualities()[index];
    preferences_.setValue("jellyfin/maxBitrateKbps",choice.first);
    qInfo().noquote()<<"jellyfin: streaming quality set to"<<choice.second;
    resetOptions();
    notify(QStringLiteral("Jellyfin"),tr("Streaming quality: %1").arg(choice.second));
    // A title that is playing re-opens at the same position with the new ceiling.
    if(jellyfinQueue_ && mediaOpen_ && mediaQueue_.current()) {
        jellyfinResume_[mediaQueue_.current()->path]=player_->position();
        startJellyfinMedia();
    }
    interactionAudio_.play(InteractionAudio::Cue::Confirm);
    emit changed();
}
bool Shell::jellyfinVideoSelected() const {
    const auto* media=mediaQueue_.current();
    return mediaOpen_ && jellyfinQueue_ && media && !jellyfinAudio_.value(media->path);
}

void Shell::startJellyfinMedia() {
    const auto* current=mediaQueue_.current();if(!current)return;const auto media=*current;
    cancelNextOffer();nextLookup_=0;
    ++mediaRequest_;jellyfinRequest_=0;finishJellyfinPlayback();player_->stop();player_->setSource({});
    resumePath_.clear();resumePending_=0;
    jellyfinResumePending_=jellyfinResume_.value(media.path);
    showJellyfinTrack(media.path);
    mediaChapters_.clear();jellyfinChapterRequest_=0;jellyfinAudioApplyTries_=0;
    mediaOpen_=true;mediaTitle_=media.title;loading_=true;status_.clear();emit mediaFrame({});
    if(jellyfinResumePending_>0) {
        status_=tr("Resuming at %1").arg(MediaControls::formatTime(jellyfinResumePending_));
        qInfo().noquote()<<"jellyfin: resume pending at"<<jellyfinResumePending_<<"ms for"<<media.title;
    }
    for(int i=0;i<entries().size();++i)if(entries()[i].value==media.path && entries()[i].action==Action::JellyfinPlay){currentSelection()=i;break;}
    const auto burn=jellyfinBurnIn_.value(media.path,{-1,QString{}});
    jellyfinRequest_=jellyfin_.playback(media.path,jellyfinAudio_.value(media.path),preferences_.value("jellyfin/maxBitrateKbps").toInt(),
        burn.first,burn.second,jellyfinAudioReopen_.value(media.path,-1));emit changed();
}
void Shell::applyJellyfinResume() {
    if(!jellyfinQueue_ || !mediaOpen_ || jellyfinPlayback_.itemId.isEmpty() || jellyfinResumePending_<=0 || jellyfinApplyingResume_ || !player_->isSeekable())return;
    if(player_->mediaStatus()!=QMediaPlayer::LoadedMedia && player_->mediaStatus()!=QMediaPlayer::BufferedMedia)return;
    const qint64 reported=jellyfinResumePending_;
    const qint64 target=player_->duration()>0?std::min(reported,std::max<qint64>(0,player_->duration()-1)):reported;
    jellyfinResumePending_=0;jellyfinApplyingResume_=true;player_->setPosition(target);jellyfinApplyingResume_=false;
    status_.clear();
    qInfo().noquote()<<QString("jellyfin: resume applied at %1 ms (server reported %2 ms, duration %3 ms)")
        .arg(target).arg(reported).arg(player_->duration());
    reportJellyfinPlaybackState();emit changed();
}
void Shell::reportJellyfinPlaybackState() {
    if(!jellyfinQueue_ || !mediaOpen_ || jellyfinPlayback_.itemId.isEmpty() || jellyfinResumePending_>0 || jellyfinApplyingResume_)return;
    const auto state=player_->playbackState();
    if(state==QMediaPlayer::PlayingState && !jellyfinStarted_){
        jellyfinStarted_=true;
        qInfo().noquote()<<"jellyfin: start report at"<<player_->position()<<"ms";
        jellyfin_.reportPlayback(jellyfinPlayback_,player_->position(),false,"start");
    } else if(jellyfinStarted_ && state!=QMediaPlayer::StoppedState)
        jellyfin_.reportPlayback(jellyfinPlayback_,player_->position(),state==QMediaPlayer::PausedState,"progress");
}
void Shell::finishJellyfinPlayback(bool completed) {
    if(!jellyfinPlayback_.itemId.isEmpty()) {
        const auto id=jellyfinPlayback_.itemId;
        const qint64 duration=player_->duration()>0?player_->duration():jellyfinPlayback_.durationMs;
        // A completed title is reported at its end position even when the backend has already
        // rewound to zero, so the server can mark it played; an abandoned one is reported where it
        // actually stopped.
        const qint64 at=completed?(duration>0?duration:player_->position())
            :(jellyfinStarted_?player_->position():jellyfinResume_.value(id));
        // A finished title is not "paused": the stop is the completion report, not a pause.
        jellyfin_.reportPlayback(jellyfinPlayback_,at,completed?false:mediaPaused(),"stop");
        qInfo().noquote()<<QString("jellyfin: stop report item=%1 at %2 ms%3").arg(id).arg(at)
            .arg(completed?" (reached the end)":"");
        // Reaching the end or 90 percent is "watched": the server is told so explicitly (idempotent next to its own
        // rule on the stop report), and the resume point goes. A stop near the start keeps no resume point.
        const auto outcome=jellyfinPlayback_.audio?jfplayer::audioStopOutcome(at,duration,completed):jfplayer::stopOutcome(at,duration,completed);
        // Audio: a song's play count is the server's own rule on the stop report (a second mark could count it twice); only long
        // audio (audiobook, podcast) is marked explicitly, which also clears its resume point.
        if((jellyfinStarted_ || completed) && outcome.played && (!jellyfinPlayback_.audio || jfplayer::audioNeedsPlayedMark(duration,outcome.played))) {
            qInfo().noquote()<<QString("jellyfin: marking %1 watched (%2 of %3 ms)").arg(id).arg(at).arg(duration);
            jellyfin_.markPlayed(id);
        }
        if(jellyfinStarted_ || completed) {
            const qint64 resume=outcome.resumeMs;
            jellyfinResume_.insert(id,resume);
            for(auto& page:pages_)if(page.jellyfin)for(auto& row:page.entries)if(row.action==Action::JellyfinPlay && row.value==id) {
                row.resumeMs=resume;if(duration>0)row.durationMs=duration;
                if(outcome.played)row.played=true;else if(resume>0)row.played=false;
                row.detail=jellyfinRowDetail(row);
                if(outcome.played)qInfo().noquote()<<"jellyfin: row now reads"<<row.detail;
            }
        }
    }
    jellyfinPlayback_={};jellyfinStarted_=false;jellyfinResumePending_=0;jellyfinStream_.close();mediaChapters_.clear();jellyfinChapterRequest_=0;
    mediaLyrics_.clear();jellyfinLyricsRequest_=0;
    jellyfinSubtitleRequests_.clear();jellyfinSubtitleErrors_.clear();
}
// The screen text for a stream that would not open: what the server (or the XMB's own relay) actually said, in one
// or two sentences, instead of a generic hint. The numbers and the server's own words come from the stream's
// record of the failing request, else from the diagnostic probe of the same URL.
static QString jellyfinFailureText(const QString& playerError,int status,const QString& detail,bool relayRefused,bool answered) {
    QString words=detail.left(120);
    if(detail.size()>120)words+=QStringLiteral("...");
    const QString quote=words.isEmpty()?QString{}:QStringLiteral(": \"%1\"").arg(words);
    if(relayRefused)
        return Shell::tr("The XMB could not relay the stream: %1.").arg(words.isEmpty()?Shell::tr("unknown reason"):words);
    if(status>=500)
        return Shell::tr("Jellyfin failed to start the stream (HTTP %1%2). Check the server's log and its transcoding settings.").arg(status).arg(quote);
    if(status==401 || status==403)
        return Shell::tr("Jellyfin refused the stream (HTTP %1%2). Sign out of Jellyfin and sign in again.").arg(status).arg(quote);
    if(status==404)
        return Shell::tr("Jellyfin has no such stream (HTTP 404%1). The item may have been moved or removed.").arg(quote);
    if(status>=400)
        return Shell::tr("Jellyfin rejected the stream request (HTTP %1%2).").arg(status).arg(quote);
    if(answered)
        return Shell::tr("Jellyfin answers (HTTP %1) but the player could not open the stream: %2.").arg(status).arg(playerError.isEmpty()?Shell::tr("unknown error"):playerError.left(100));
    if(!detail.isEmpty())
        return Shell::tr("The server did not answer the stream request: %1.").arg(words);
    return playerError.isEmpty()?Shell::tr("This media could not be played."):Shell::tr("This media could not be played: %1.").arg(playerError.left(100));
}
// A Jellyfin playback failed. When the stream already named the failing request (an error status from the server,
// or its own refusal) that is reported at once; otherwise the XMB asks the server for the same URL itself
// (JellyfinClient::probeStream: same headers, a few KB, 8 s) so the real answer shows in the log and on screen.
void Shell::beginJellyfinFailureReport(const QString& playerError,const JellyfinStreamIssue& issue,const QUrl& url) {
    jellyfinPlayerError_=scrubDiagnostic(playerError.toUtf8(),160);
    jellyfinFailedIssue_=issue;
    jellyfinProbePath_=url.isValid()?url.path():QString{};
    jellyfinProbe_=0;
    if(issue.valid || !url.isValid() || !jellyfin_.signedIn()){reportJellyfinFailure(0,{},false);return;}
    status_=QStringLiteral("Playback stopped");
    jellyfinProbe_=jellyfin_.probeStream(url);
    // The probe has its own 8 s deadline; this only guards against the page being left open on a stuck state.
    const auto probe=jellyfinProbe_;
    QTimer::singleShot(12000,this,[this,probe]{if(jellyfinProbe_==probe && probe!=0){jellyfinProbe_=0;reportJellyfinFailure(0,QStringLiteral("no answer"),true);}});
}
void Shell::reportJellyfinFailure(int probeStatus,const QString& probeDetail,bool probed) {
    const auto issue=jellyfinFailedIssue_;
    int status=issue.status;QString detail=issue.detail;bool refused=issue.valid && issue.status==0;
    bool answered=false;
    if(!issue.valid && probed){
        status=probeStatus;detail=probeDetail;
        answered=probeStatus>=200 && probeStatus<400;
        if(answered)detail.clear();
    }
    qWarning().noquote()<<"jellyfin: stream open failed: player=\""+jellyfinPlayerError_+"\""
        <<(issue.valid?QStringLiteral("stream saw %1 %2%3").arg(issue.request,issue.status?QStringLiteral("HTTP %1").arg(issue.status):QStringLiteral("(not relayed)"),
                issue.detail.isEmpty()?QString{}:QStringLiteral(" \"%1\"").arg(issue.detail)):QString{})
        <<(probed?QStringLiteral("probe GET %1 -> %2%3").arg(jellyfinProbePath_,probeStatus?QStringLiteral("HTTP %1").arg(probeStatus):QStringLiteral("no answer"),
                probeDetail.isEmpty()?QString{}:QStringLiteral(" \"%1\"").arg(probeDetail)):QString{});
    showMessage("Jellyfin",jellyfinFailureText(jellyfinPlayerError_,status,detail,refused,answered),MessageKind::Page);
    emit changed();
}
// One track per text subtitle stream of the item about to play (at most eight), each fetched as SRT.
// The tracks start empty, so their rows read "Loading" until the bytes arrive (subtitleReady above).
void Shell::requestJellyfinSubtitles() {
    subtitleTracks_.clear();subtitleTrackIndex_=-1;subtitleCueNumber_=0;++subtitleGeneration_;
    jellyfinSubtitleRequests_.clear();jellyfinSubtitleErrors_.clear();
    subtitleMediaPath_=QStringLiteral("jellyfin:")+jellyfinPlayback_.itemId;
    int text=0,pgs=0,pgsDefault=-1;
    bool textDefault=false;
    for(const auto& stream:jellyfinPlayback_.streams) {
        if(stream.type!=QLatin1String("Subtitle") || stream.index<0)continue;
        if(!isEnglishSubtitle(stream))continue;   // only English subtitles, text and PGS alike
        if(subtitleTracks_.size()>=8)break;
        subtitles::Track track;
        track.path=jellyfinSubtitleKey(stream.index);track.name=stream.title;track.language=stream.language;
        if(stream.text) {
            track.format="srt";
            subtitleTracks_.push_back(track);
            jellyfinSubtitleRequests_.insert(jellyfin_.subtitle(jellyfinPlayback_.itemId,jellyfinPlayback_.mediaSourceId,stream.index),stream.index);
            ++text;textDefault=textDefault || stream.isDefault;
        } else if(jellyfinPgsCodec(stream.codec)) {
            // A whole film's PGS track can be tens of megabytes: it is fetched only when chosen.
            track.format="sup";track.image=true;
            if(stream.isDefault && pgsDefault<0)pgsDefault=int(subtitleTracks_.size());
            subtitleTracks_.push_back(track);
            ++pgs;
        }
    }
    if(text)qInfo().noquote()<<"jellyfin: requested"<<text<<"text subtitle stream(s)";
    if(pgs)qInfo().noquote()<<"jellyfin:"<<pgs<<"PGS subtitle stream(s), fetched as .sup when chosen";
    if(jellyfinPlayback_.burnSubtitleIndex>=0)
        qInfo().noquote()<<"jellyfin: subtitle stream"<<jellyfinPlayback_.burnSubtitleIndex<<"is burned in by the server ("
            <<jellyfinPlayback_.playMethod<<")";
    // A remembered choice of a PGS track (or a server default PGS stream with no text default) starts
    // its fetch now; text tracks are selected when their SRT arrives (subtitleReady).
    if(jellyfinPlayback_.burnSubtitleIndex>=0)return;
    // The subtitle this title was last played with (media/choices), when the server still lists that stream.
    if(!subtitleChoice_.contains(subtitleMediaPath_)) {
        const auto remembered=recallChoice();
        if(remembered.subtitle==-1)subtitleChoice_.insert(subtitleMediaPath_,QString{});
        else if(remembered.subtitle>=0) {
            const QString key=jellyfinSubtitleKey(remembered.subtitle);
            for(const auto& track:subtitleTracks_)if(track.path==key){subtitleChoice_.insert(subtitleMediaPath_,key);break;}
        }
    }
    int image=-1;
    if(subtitleChoice_.contains(subtitleMediaPath_)) {
        const QString chosen=subtitleChoice_.value(subtitleMediaPath_);
        for(int i=0;i<subtitleTracks_.size();++i)if(subtitleTracks_[i].image && subtitleTracks_[i].path==chosen)image=i;
    } else if(!textDefault)image=pgsDefault;
    if(image>=0)applySubtitleTrack(image);
}
bool Shell::jellyfinPgsCodec(const QString& codec) {
    const QString c=codec.trimmed().toLower();
    return c==QLatin1String("pgssub") || c==QLatin1String("pgs") || c==QLatin1String("hdmv_pgs_subtitle");
}
// The title re-opens with a different server audio stream (a transcode carries one, so the server picks it), at the
// same position; the choice is remembered for the title.
void Shell::chooseJellyfinAudio(int streamIndex) {
    if(!jellyfinQueue_ || !mediaOpen_ || !mediaQueue_.current() || streamIndex<0)return;
    const QString item=mediaQueue_.current()->path;
    if(streamIndex==jellyfinPlayback_.audioStreamIndex){resetOptions();emit changed();return;}
    jellyfinAudioReopen_.insert(item,streamIndex);
    rememberChoice(streamIndex,-2);
    qInfo().noquote()<<"jellyfin: audio stream"<<streamIndex<<"chosen; re-opening as a server stream";
    jellyfinResume_[item]=player_->position();
    resetOptions();
    notify(QStringLiteral("Jellyfin"),tr("Audio track changed by the server"));
    interactionAudio_.play(InteractionAudio::Cue::Confirm);
    startJellyfinMedia();
}
// A direct play carries every audio stream: once the player has loaded and reports the tracks, the remembered one is
// switched to locally (a few tries, because a backend may report its tracks a moment after LoadedMedia).
void Shell::applyRememberedJellyfinAudio() {
    if(!jellyfinQueue_ || !mediaOpen_ || jellyfinPlayback_.audio || jellyfinPlayback_.playMethod!=QLatin1String("DirectPlay") || jellyfinAudioApplyTries_>=3)return;
    const auto remembered=recallChoice();
    if(remembered.audio<0){jellyfinAudioApplyTries_=3;return;}
    const int ordinal=jfplayer::audioOrdinal(jellyfinPlayback_.streams,remembered.audio);
    if(ordinal<0){jellyfinAudioApplyTries_=3;return;}
    ++jellyfinAudioApplyTries_;
    refreshAudioTracks();
    if(ordinal>=audioTracks_.size())return;
    jellyfinAudioApplyTries_=3;
    if(ordinal!=activeAudioTrack_ && audioTracks_[ordinal].switchable)chooseAudioTrack(ordinal);
}
void Shell::openChapterList() {
    optionsParent_=options_;optionsNested_=true;optionsTitle_=tr("Chapters");
    options_.clear();optionsSelection_=0;
    appendChapterRows();
    for(int i=0;i<options_.size();++i)if(options_[i].active){optionsSelection_=i;break;}
    interactionAudio_.play(InteractionAudio::Cue::Options);
    emit changed();
}
// One row per server chapter mark: the numbered name, the start time as the note, the chapter the position is in
// marked as the current choice. Confirm seeks to its start (chooseChapter).
void Shell::appendChapterRows() {
    const int at=jfplayer::chapterIndexAt(mediaChapters_,player_->position());
    for(int i=0;i<mediaChapters_.size() && i<100;++i) {
        MenuOption row{jfplayer::chapterLabel(mediaChapters_[i],i),OptionCommand::PlayerChapterTarget};
        row.index=i;row.active=i==at;row.note=MediaControls::formatTime(mediaChapters_[i].startMs);
        options_.push_back(row);
    }
}
void Shell::chooseChapter(int index) {
    if(!videopanel::chapterTargetValid(index,int(mediaChapters_.size())) || !mediaOpen_ || !player_->isSeekable())return;
    const qint64 target=mediaChapters_[index].startMs;
    const QString label=jfplayer::chapterLabel(mediaChapters_[index],index);
    resetOptions();
    qInfo().noquote()<<"media: chapter jump to"<<label<<"at"<<target<<"ms";
    player_->setPosition(target);
    interactionAudio_.play(InteractionAudio::Cue::Confirm);
    emit changed();
}
// Burn-in: the title re-opens at the same position with the chosen image stream rendered into a server
// transcode (JellyfinClient::playback), or, with -1, back on the original stream without it.
void Shell::chooseJellyfinBurnIn(int streamIndex) {
    if(!jellyfinQueue_ || !mediaOpen_ || !mediaQueue_.current())return;
    const QString item=mediaQueue_.current()->path;
    if(streamIndex<0)jellyfinBurnIn_.remove(item);
    else {
        QString codec;
        for(const auto& stream:jellyfinPlayback_.streams)if(stream.index==streamIndex)codec=stream.codec;
        jellyfinBurnIn_.insert(item,{streamIndex,codec});
        rememberChoice(-2,streamIndex);
        subtitleChoice_.insert(subtitleMediaPath_,QString{});   // the client draws nothing over a burned-in stream
    }
    qInfo().noquote()<<(streamIndex<0?QStringLiteral("jellyfin: burn-in off; re-opening the original stream")
        :QStringLiteral("jellyfin: burn in subtitle stream %1; re-opening as a server transcode").arg(streamIndex));
    jellyfinResume_[item]=player_->position();
    resetOptions();
    notify(QStringLiteral("Jellyfin"),streamIndex<0?tr("Subtitles: original stream"):tr("Subtitles burned in by the server (transcode)"));
    interactionAudio_.play(InteractionAudio::Cue::Confirm);
    startJellyfinMedia();
}
bool Shell::autoplayNextEnabled() const {
    return preferences_.value("jellyfin/autoplayNext").toBool();
}
QString Shell::nextEpisodePrompt() const {
    if(!nextOffer_.active)return {};
    return jfplayer::autoplayPromptText(nextOffer_.title,std::max(nextOffer_.left,1));
}
// An episode just ended: ask the server for the next one (the watched mark first, then Next Up for the series). True when a
// lookup is under way and the queue must wait for it. Nothing is asked for a movie, a series-less item, music, Repeat One or
// when the preference is off; those carry on to the queue exactly as before.
bool Shell::beginNextEpisodeLookup(const QString& finishedId) {
    if(finishedId.isEmpty() || !jellyfin_.signedIn() || mediaRepeatMode()==1)return false;
    const QString series=jellyfinSeries_.value(finishedId);
    if(!jfplayer::offersNextEpisode(jellyfinKinds_.value(finishedId),series,jellyfinAudio_.value(finishedId),autoplayNextEnabled()))return false;
    nextLookupMedia_=mediaRequest_;
    nextLookup_=jellyfin_.nextEpisode(series,finishedId);
    status_=tr("Looking for the next episode...");
    qInfo().noquote()<<"jellyfin: episode"<<finishedId<<"ended; asking the server for the next one in its series";
    emit changed();
    return true;
}
void Shell::startNextOffer(const JellyfinItem& item) {
    const QString label=jfplayer::episodeLabel(item);
    nextOffer_=NextOffer{};
    nextOffer_.active=true;nextOffer_.id=item.id;nextOffer_.seriesId=item.seriesId.isEmpty()?jellyfinSeries_.value(item.id):item.seriesId;
    nextOffer_.kind=QStringLiteral("Episode");nextOffer_.resumeMs=item.resumeMs;
    nextOffer_.title=item.seriesName.isEmpty()?label:QStringLiteral("%1 %2").arg(item.seriesName,label);
    nextOffer_.left=jfplayer::kAutoplaySeconds;
    nextOfferClock_.start();nextOfferTimer_.start();
    qInfo().noquote()<<"jellyfin: Play Next Episode offered:"<<nextOffer_.title<<"in"<<jfplayer::kAutoplaySeconds<<"s";
    interactionAudio_.play(InteractionAudio::Cue::Options);
    emit changed();
}
void Shell::cancelNextOffer() {
    nextOfferTimer_.stop();
    if(nextOffer_.active)qInfo().noquote()<<"jellyfin: Play Next Episode countdown ended without playing";
    nextOffer_=NextOffer{};
}
// Cross, or the countdown reaching zero: the offered episode joins the queue (when its page does not already hold it) and plays.
void Shell::playNextOffer() {
    if(!nextOffer_.active)return;
    const NextOffer offer=nextOffer_;
    nextOfferTimer_.stop();nextOffer_=NextOffer{};
    if(!mediaOpen_ || !jellyfinQueue_)return;
    auto items=mediaQueue_.items();
    int index=-1;
    for(int i=0;i<items.size();++i)if(items[i].path==offer.id){index=i;break;}
    if(index<0){items.push_back({offer.id,offer.title});index=int(items.size())-1;}
    jellyfinAudio_.insert(offer.id,false);jellyfinResume_.insert(offer.id,offer.resumeMs);
    jellyfinSeries_.insert(offer.id,offer.seriesId);jellyfinKinds_.insert(offer.id,offer.kind);
    qInfo().noquote()<<"jellyfin: playing the next episode"<<offer.title;
    mediaQueue_.reset(std::move(items),index);mediaFailures_=0;
    startJellyfinMedia();
}
// The Triangle menu's watched and favourite rows: ask the server, and let the answer (applyJellyfinFlag) change the row.
void Shell::toggleJellyfinFlag(const Entry& entry,bool favorite) {
    if(entry.value.isEmpty() || !jellyfin_.signedIn())return;
    const bool value=favorite?!entry.favorite:!entry.played;
    const quint64 request=jellyfin_.setUserFlag(entry.value,favorite?JellyfinClient::UserFlag::Favorite:JellyfinClient::UserFlag::Played,value);
    userFlagRequests_.insert(request,entry.value);
    qInfo().noquote()<<"jellyfin: asking the server to"<<(favorite?(value?"add a favourite":"remove a favourite"):(value?"mark watched":"mark unwatched"))<<"for"<<entry.title;
}
void Shell::applyJellyfinFlag(const QString& itemId,int flag,bool value) {
    const bool favorite=flag==int(JellyfinClient::UserFlag::Favorite);
    if(!favorite && value)jellyfinResume_.insert(itemId,0);   // watched clears the server's resume point
    for(auto& page:pages_)if(page.jellyfin)for(auto& row:page.entries) {
        if(row.value!=itemId)continue;
        if(row.action==Action::JellyfinMusicNode){if(favorite && row.setting==QLatin1String("album")){row.favorite=value;row.detail=jellyfinMusicNodeDetail(row);}continue;}
        if(row.action!=Action::JellyfinPlay && row.action!=Action::JellyfinFolder)continue;
        if(favorite)row.favorite=value;
        else{row.played=value;if(value)row.resumeMs=0;}
        if(row.action==Action::JellyfinPlay || row.played || row.favorite)row.detail=jellyfinRowDetail(row);
        else row.detail.clear();
    }
}
void Shell::signOutJellyfin() {
    jellyfinRequest_=0;finishJellyfinPlayback();jellyfin_.signOut();jellyfin_.forgetSession();
    qInfo()<<"jellyfin: signed out; the remembered session was forgotten";
    notify(tr("Signed out."),QStringLiteral("Jellyfin"));
    jellyfinResume_.clear();jellyfinQueue_=false;loading_=false;status_.clear();
    jellyfinMusicLibraries_=-1;jellyfinMusicWanted_=false;refreshJellyfinMusicRow();
    while(pages_.size()>jellyfinRootDepth_)pages_.removeLast();emit changed();
}
bool Shell::jellyfinRemembered() const { return jellyfin_.hasStoredSession(); }
}
