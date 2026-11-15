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
// The row's one detail line: what it is, where it stands, how long it is. Kept to printable ASCII
// because the label renderer is the firmware's and the evidence crops are ASCII text.
QString progressDetail(const Entry& entry) {
    QStringList parts;
    if(!entry.mediaKind.isEmpty())parts<<entry.mediaKind;
    if(entry.resumeMs>0)parts<<QObject::tr("Resume at %1").arg(MediaControls::formatTime(entry.resumeMs));
    else if(entry.played)parts<<QObject::tr("Watched");
    if(entry.durationMs>0)parts<<MediaControls::formatTime(entry.durationMs);
    return parts.join(QStringLiteral(" - "));
}
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
        loadJellyfinPage({},jellyfinServiceTitle_,0,true);
    });
    connect(&jellyfin_,&JellyfinClient::failed,this,[this](quint64 request,const QString& error){
        if(request==0){if(jellyfinSigningIn_){jellyfinSigningIn_=false;emit jellyfinSignInFinished(false,error);}return;}
        if(request!=jellyfinRequest_)return;
        jellyfinRequest_=0;loading_=false;status_.clear();
        // A remembered session the server no longer accepts: forget it and show the sign-in page,
        // because that is the surface that can fix it.
        if(jellyfinRestoring_) {
            jellyfinRestoring_=false;
            jellyfin_.signOut();
            jellyfin_.forgetSession();
            qWarning().noquote()<<"jellyfin: the remembered session was refused; it has been forgotten, signing in again";
            openJellyfinSignInPage(jellyfinServiceServer_);
            return;
        }
        if(jellyfinQueue_ && mediaOpen_){finishJellyfinPlayback();player_->stop();}
        message_="Jellyfin\n\n"+error;emit changed();
    });
    connect(&jellyfin_,&JellyfinClient::pageReady,this,[this](quint64 request,const JellyfinPage& result){
        if(request!=jellyfinRequest_ || pages_.size()!=jellyfinRequestDepth_ || pages_.isEmpty() || !pages_.last().jellyfin)return;
        jellyfinRequest_=0;loading_=false;status_.clear();jellyfinRestoring_=false;auto& page=pages_.last();
        const QString selected=page.entries.isEmpty()?QString{}:page.entries.value(page.selection).id;
        page.entries.clear();page.selection=0;
        int posters=0,backdrops=0;
        if(jellyfinBackdropTags_.size()>4000)jellyfinBackdropTags_.clear();
        for(const auto& media:result.items) {
            // Every item's backdrop tag is kept so the focused row can ask for its own picture (syncJellyfinBackdrop).
            if(media.backdropTag.isEmpty())jellyfinBackdropTags_.remove(media.id);
            else{jellyfinBackdropTags_.insert(media.id,media.backdropTag);++backdrops;}
            const bool folder=page.jellyfinViews || media.folder;
            auto e=jfEntry("jellyfin:"+media.id,media.title,folder?Action::JellyfinFolder:playable(media.type)?Action::JellyfinPlay:Action::Unavailable,media.id,folder);
            e.mediaKind=folder?QString{}:media.type=="Audio"?QString("Music"):media.type=="Movie"?QString("Movie"):media.type=="Episode"?QString("Episode"):QString("Video");e.option=media.type=="Audio";
            if(!folder){e.resumeMs=media.resumeMs;e.durationMs=media.durationMs;e.played=media.played;e.detail=progressDetail(e);}
            if(media.type=="Audio")e.texture="tex_music";
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
        if(page.entries.empty())status_=page.jellyfinSearch?"No Jellyfin items match that search.":"There are no supported items in this library.";
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
        bool updated=false;
        for(auto& page:pages_)if(page.jellyfin)for(auto& row:page.entries) {
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
        requestJellyfinSubtitles();
        const auto local=jellyfinStream_.open(playback.url,jellyfin_.authorizationHeader());
        if(!local.isValid() || local.isEmpty()){advanceMedia(true,{});return;}
        player_->setSource(local);player_->play();emit changed();
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
    if(app==contentApps_.cend()){message_="This Jellyfin registration could not be found.";return;}
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
void Shell::cancelJellyfinSignIn(){jellyfinSigningIn_=false;jellyfinRequest_=0;jellyfin_.signOut();loading_=false;status_.clear();emit changed();}
void Shell::loadJellyfinPage(const QString& parent,const QString& title,int start,bool views,bool replace) {
    if(!jellyfin_.signedIn()){message_="Jellyfin\n\nSign out and reopen the app to sign in again.";emit changed();return;}
    if(!replace && pages_.size()>=32){message_="The maximum folder depth has been reached.";emit changed();return;}
    if(!replace){Page page;page.title=title;pages_.push_back(std::move(page));}
    if(pages_.empty())return;
    auto& page=pages_.last();page.jellyfin=true;page.jellyfinViews=views;page.jellyfinParent=parent;page.jellyfinStart=start;
    jellyfinBackdrop_={};jellyfinBackdropId_.clear();jellyfinBackdropWanted_.clear();jellyfinBackdropDebounce_.stop();
    loading_=true;status_="Loading Jellyfin...";jellyfinRequestDepth_=int(pages_.size());
    jellyfinRequest_=views?jellyfin_.views():jellyfin_.items(parent,start);emit changed();
}
void Shell::openJellyfinSearch(const QString& term) {
    const QString query=term.trimmed();
    if(query.isEmpty())return;
    if(!jellyfin_.signedIn()){message_="Jellyfin\n\nSign out and reopen the app to sign in again.";emit changed();return;}
    if(pages_.size()>=32){message_="The maximum folder depth has been reached.";emit changed();return;}
    // One search page at a time: a second search replaces the first result page instead of stacking.
    if(!pages_.isEmpty() && pages_.last().jellyfinSearch)pages_.removeLast();
    const QString title=tr("Search: %1").arg(query);
    Page page;page.title=title;
    page.jellyfin=true;page.jellyfinSearch=true;page.jellyfinParent=query;
    pages_.push_back(std::move(page));
    loadJellyfinSearchPage(query,title,0,true);
}
void Shell::loadJellyfinSearchPage(const QString& term,const QString& title,int start,bool replace) {
    if(!jellyfin_.signedIn()){message_="Jellyfin\n\nSign out and reopen the app to sign in again.";emit changed();return;}
    if(!replace){Page page;page.title=title;page.jellyfinSearch=true;pages_.push_back(std::move(page));}
    if(pages_.empty())return;
    auto& page=pages_.last();page.jellyfin=true;page.jellyfinSearch=true;page.jellyfinParent=term;page.jellyfinStart=start;
    jellyfinBackdrop_={};jellyfinBackdropId_.clear();jellyfinBackdropWanted_.clear();jellyfinBackdropDebounce_.stop();
    loading_=true;status_=tr("Searching Jellyfin...");jellyfinRequestDepth_=int(pages_.size());
    qInfo().noquote()<<"jellyfin: search"<<term<<"from"<<start;
    jellyfinRequest_=jellyfin_.search(term,start);emit changed();
}
void Shell::openJellyfinMedia(const Entry& entry,bool fromBeginning) {
    QVector<PlaybackQueue::Item> queue;int selected=-1;jellyfinAudio_.clear();jellyfinResume_.clear();
    for(const auto& row:entries())if(row.action==Action::JellyfinPlay){if(row.id==entry.id)selected=int(queue.size());queue.push_back({row.value,row.title});jellyfinAudio_.insert(row.value,row.option.toBool());jellyfinResume_.insert(row.value,row.resumeMs);}
    if(selected<0)return;
    if(fromBeginning)jellyfinResume_.insert(entry.value,0);
    qInfo().noquote()<<QString("jellyfin: %1 \"%2\" (server position %3 ms)")
        .arg(fromBeginning?QStringLiteral("play from the beginning"):QStringLiteral("play"))
        .arg(entry.title).arg(fromBeginning?0:entry.resumeMs);
    jellyfinQueue_=true;mediaQueue_.clear();mediaQueue_.setRepeat(static_cast<PlaybackQueue::Repeat>(mediaRepeatMode()));
    mediaQueue_.setShuffle(preferences_.value("media/shuffle").toBool());mediaQueue_.reset(std::move(queue),selected);mediaFailures_=0;startJellyfinMedia();
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
void Shell::startJellyfinMedia() {
    const auto* current=mediaQueue_.current();if(!current)return;const auto media=*current;
    ++mediaRequest_;jellyfinRequest_=0;finishJellyfinPlayback();player_->stop();player_->setSource({});
    resumePath_.clear();resumePending_=0;
    jellyfinResumePending_=jellyfinResume_.value(media.path);
    mediaOpen_=true;mediaTitle_=media.title;loading_=true;status_.clear();emit mediaFrame({});
    if(jellyfinResumePending_>0) {
        status_=tr("Resuming at %1").arg(MediaControls::formatTime(jellyfinResumePending_));
        qInfo().noquote()<<"jellyfin: resume pending at"<<jellyfinResumePending_<<"ms for"<<media.title;
    }
    for(int i=0;i<entries().size();++i)if(entries()[i].value==media.path && entries()[i].action==Action::JellyfinPlay){currentSelection()=i;break;}
    const auto burn=jellyfinBurnIn_.value(media.path,{-1,QString{}});
    jellyfinRequest_=jellyfin_.playback(media.path,jellyfinAudio_.value(media.path),preferences_.value("jellyfin/maxBitrateKbps").toInt(),
        burn.first,burn.second);emit changed();
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
        const qint64 duration=player_->duration();
        // A completed title is reported at its end position even when the backend has already
        // rewound to zero, so the server can mark it played; an abandoned one is reported where it
        // actually stopped.
        const qint64 at=completed?(duration>0?duration:player_->position())
            :(jellyfinStarted_?player_->position():jellyfinResume_.value(id));
        // A finished title is not "paused": the stop is the completion report, not a pause.
        jellyfin_.reportPlayback(jellyfinPlayback_,at,completed?false:mediaPaused(),"stop");
        qInfo().noquote()<<QString("jellyfin: stop report item=%1 at %2 ms%3").arg(id).arg(at)
            .arg(completed?" (reached the end)":"");
        if(jellyfinStarted_ || completed) {
            const qint64 resume=completed?0:at;
            jellyfinResume_.insert(id,resume);
            for(auto& page:pages_)if(page.jellyfin)for(auto& row:page.entries)if(row.action==Action::JellyfinPlay && row.value==id) {
                row.resumeMs=resume;if(duration>0)row.durationMs=duration;row.played=completed;row.detail=progressDetail(row);
                if(completed)qInfo().noquote()<<"jellyfin: row now reads"<<row.detail;
            }
        }
    }
    jellyfinPlayback_={};jellyfinStarted_=false;jellyfinResumePending_=0;jellyfinStream_.close();
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
void Shell::signOutJellyfin() {
    jellyfinRequest_=0;finishJellyfinPlayback();jellyfin_.signOut();jellyfin_.forgetSession();
    qInfo()<<"jellyfin: signed out; the remembered session was forgotten";
    jellyfinResume_.clear();jellyfinQueue_=false;loading_=false;status_.clear();
    while(pages_.size()>jellyfinRootDepth_)pages_.removeLast();emit changed();
}
bool Shell::jellyfinRemembered() const { return jellyfin_.hasStoredSession(); }
}
