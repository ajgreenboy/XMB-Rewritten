#include "shell.h"
#include "seerr_client.h"
#include "seerr_config.h"
#include "seerr_jellyfin_match.h"
#include "sabnzbd_client.h"
#include "diagnostics.h"
#include "service_icons.h"

#include <QBuffer>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>
#include <algorithm>

namespace xmb {

struct SeerrUi {
    SeerrClient client;
    SeerrClient downloadClient;
    SabnzbdClient sabClient;
    seerrConfig::SabConfig sabEdit,sabActive;
    QHash<QString,QStringList> sabIds;
    QHash<QString,bool> sabStates;
    quint64 sabEpoch=0;
    bool sabBusy=false;
    QVector<Entry> downloadRows;
    QHash<QString,QImage> posters;
    QSet<QString> posterPending, downloadDetails;
    bool downloadPending=false;
    bool downloadLoaded=false;
    QUrl downloadServer;
    QString downloadApiKey;
    QString server;
    QString apiKey;
    // Seerr -> Jellyfin resolution state: for a request whose SABnzbd transfer has completed but whose media
    // is not yet marked Available, the matching Jellyfin library item (resolved by ProviderIds.Tmdb, title
    // fallback) becomes a Watch action. Keyed by the download row id (download:seerr:<requestId>). The match
    // decision itself is the pure resolveSeerrWatch() policy (seerr_jellyfin_match.h); this struct only keeps
    // the per-row inputs and the in-flight search bookkeeping.
    QHash<QString,QString> seerrTmdbId;
    QHash<QString,QString> seerrMediaType;    // row id -> "movie" / "tv"
    QHash<QString,bool> seerrIs4k;            // row id -> the request's is4k flag
    QHash<QString,JellyfinItem> jellyfinMatch;
    QHash<QString,QString> jellyfinTitleKey;   // row id -> search key (dedupes re-search)
    QHash<QString,QString> jellyfinTitle;      // row id -> title (title-match fallback)
    QSet<QString> jellyfinResolvePending;
    QHash<quint64,QString> jellyfinResolveRow; // search request id -> the row it resolves
    bool jellyfinResolveHooked=false;
    QString searchTerm;
    QString title;
    QString mediaType;
    int tmdbId=0;
    QJsonObject detail;
    QJsonObject searchResults;
    QJsonObject requestList;
    QHash<QString,QString> titles;
    QList<int> selectedSeasons;
    QString error;
    QString connection;
    quint64 generation=0;
    bool configDirty=false;
    bool submitting=false;
};

namespace {
Entry seerrItem(QString id,QString title,QString texture=QStringLiteral("23n"),
                QString container=QStringLiteral("qgl/icontex"),QString value={}) {
    Entry row;row.id=std::move(id);row.title=std::move(title);row.texture=std::move(texture);
    row.container=std::move(container);row.action=Action::Seerr;row.value=std::move(value);return row;
}
QString seerrMediaType(const QJsonObject& item) {
    const auto mediaType=item.value(QStringLiteral("mediaType")).toString();
    if(mediaType==QStringLiteral("tv") || mediaType==QStringLiteral("movie"))return mediaType;
    const auto media=item.value(QStringLiteral("media")).toObject();
    const auto nested=media.value(QStringLiteral("mediaType")).toString();
    if(nested==QStringLiteral("tv") || nested==QStringLiteral("movie"))return nested;
    return {};
}
QString seerrTitle(const QJsonObject& item) {
    auto title=item.value(QStringLiteral("title")).toString();
    if(title.isEmpty())title=item.value(QStringLiteral("name")).toString();
    if(title.isEmpty())title=item.value(QStringLiteral("media")).toObject().value(QStringLiteral("title")).toString();
    if(title.isEmpty())title=item.value(QStringLiteral("media")).toObject().value(QStringLiteral("name")).toString();
    return title.isEmpty()?QObject::tr("Untitled"):title;
}
QString seerrMediaStatus(int value) {
    switch(value) {
    case 1:return QObject::tr("Unknown");
    case 2:return QObject::tr("Pending");
    case 3:return QObject::tr("Processing");
    case 4:return QObject::tr("Partially available");
    case 5:return QObject::tr("Available");
    case 6:return QObject::tr("Blocked");
    case 7:return QObject::tr("Deleted");
    default:return QObject::tr("Status unavailable");
    }
}
QString seerrRequestStatus(int value) {
    switch(value) {
    case 1:return QObject::tr("Pending approval");
    case 2:return QObject::tr("Approved");
    case 3:return QObject::tr("Declined");
    case 4:return QObject::tr("Failed");
    case 5:return QObject::tr("Completed");
    default:return QObject::tr("Status unavailable");
    }
}
QJsonObject seerrMedia(const QJsonObject& object) {
    const auto nested=object.value(QStringLiteral("media")).toObject();
    return nested.isEmpty()?object:nested;
}
int seerrInt(const QJsonValue& value) { return value.toInt(-1); }
bool seerrHasNext(const QJsonObject& response,int pageNumber) {
    const auto info=response.value(QStringLiteral("pageInfo")).toObject();
    int pages=info.value(QStringLiteral("pages")).toInt(response.value(QStringLiteral("totalPages")).toInt(-1));
    if(pages>=0)return pageNumber<pages;
    return response.value(QStringLiteral("results")).toArray().size()>=20;
}
QJsonArray seerrTrackedItems(const QJsonObject& media,bool is4k) {
    const auto info=media.value(QStringLiteral("mediaInfo")).toObject();
    const auto& source=info.isEmpty()?media:info;
    return source.value(is4k?QStringLiteral("downloadStatus4k"):QStringLiteral("downloadStatus")).toArray();
}
QStringList seerrSabIds(const QJsonObject& media,const QList<int>& seasons,bool is4k,bool tv) {
    if(tv && seasons.isEmpty())return {};
    static const QRegularExpression sabId(QStringLiteral("^SABnzbd_nzo_[A-Za-z0-9_-]{1,80}$"));
    QStringList ids;
    for(const auto value:seerrTrackedItems(media,is4k)) {
        const auto queue=value.toObject();
        if(tv && !seasons.contains(queue.value(QStringLiteral("episode")).toObject().value(QStringLiteral("seasonNumber")).toInt(-1)))continue;
        const auto id=queue.value(QStringLiteral("downloadId")).toString();
        if(!sabId.match(id).hasMatch())return {};
        if(!ids.contains(id))ids.append(id);
        if(ids.size()>32)return {};
    }
    ids.sort();
    return ids;
}
QString seerrTrackedStatus(const QJsonObject& media,const QList<int>& seasons={},bool is4k=false) {
    const auto statuses=seerrTrackedItems(media,is4k);
    for(const auto value:statuses) {
        const auto queue=value.toObject();
        if(!seasons.isEmpty() && !seasons.contains(queue.value(QStringLiteral("episode")).toObject().value(QStringLiteral("seasonNumber")).toInt(-1)))continue;
        if(queue.value(QStringLiteral("status")).toString()!=QStringLiteral("downloading"))continue;
        const auto size=queue.value(QStringLiteral("size")).toDouble(-1);
        const auto left=queue.value(QStringLiteral("sizeLeft")).toDouble(-1);
        if(size<=0 || left<0 || left>size)continue;
        const int percent=int((1.0-left/size)*100.0+0.5);
        return QObject::tr("Tracked download · %1% complete").arg(percent);
    }
    return {};
}
}

QVector<Entry> Shell::seerrDownloadRows() const {
    return seerr_?seerr_->downloadRows:QVector<Entry>{};
}

void Shell::refreshSeerrDownloads() {
    if(diagnostics::safeMode())return;
    const auto config=seerrConfig::load(dataDirectory_);
    const auto server=SeerrClient::normalizeServer(config.server);
    if(server.isEmpty() || config.apiKey.isEmpty()) {
        if(seerr_){seerr_->downloadRows.clear();seerr_->sabIds.clear();seerr_->sabStates.clear();++seerr_->sabEpoch;}
        return;
    }
    if(!seerr_) {
        seerr_=std::make_shared<SeerrUi>();
        seerr_->server=config.server;
        seerr_->apiKey=config.apiKey;
        seerr_->client.configure(server,config.apiKey);
    }
    auto& ui=*seerr_;
    if(ui.downloadPending)return;
    if(!ui.downloadLoaded) {
        auto row=seerrItem(QStringLiteral("download:seerr:loading"),tr("Seerr"),QStringLiteral("tex_video"),QStringLiteral("explore_plugin_full"));
        row.artwork=serviceIcons::seerr();
        row.detail=tr("Checking for active downloads...");
        row.action=Action::Unavailable;
        ui.downloadRows={row};
    }
    ui.downloadPending=true;
    if(ui.downloadServer!=server || ui.downloadApiKey!=config.apiKey) {
        ui.downloadServer=server;
        ui.downloadApiKey=config.apiKey;
        ui.downloadDetails.clear();
        ui.downloadClient.configure(server,config.apiKey);
    }
    QPointer<Shell> self(this);
    ui.downloadClient.requests(0,[self](QJsonObject response,QString error){
        if(!self || !self->seerr_)return;
        auto& state=*self->seerr_;
        state.downloadPending=false;
        state.downloadLoaded=true;
        QVector<Entry> rows;
        QHash<QString,QStringList> sabIds;
        if(!error.isEmpty()) {
            auto row=seerrItem(QStringLiteral("download:seerr:error"),self->tr("Seerr Downloads Unavailable"));
            row.detail=error;
            row.value=error;
            row.action=Action::Information;
            rows.append(std::move(row));
        }
        for(const auto value:response.value(QStringLiteral("results")).toArray()) {
            const auto request=value.toObject();
            const int requestId=request.value(QStringLiteral("id")).toInt();
            if(requestId<=0 || request.value(QStringLiteral("status")).toInt()!=2)continue;
            const auto media=seerrMedia(request);
            const auto type=seerrMediaType(media);
            const int id=media.value(QStringLiteral("tmdbId")).toInt();
            if(type.isEmpty() || id<=0)continue;
            const bool is4k=request.value(QStringLiteral("is4k")).toBool();
            const int status=seerrInt(media.value(is4k?QStringLiteral("status4k"):QStringLiteral("status")));
            if(status>=5)continue;
            const QString key=type+QLatin1Char(':')+QString::number(id);
            const QString identifier=QStringLiteral("download:seerr:%1").arg(requestId);
            const QString title=state.titles.value(key,seerrTitle(request));
            auto row=seerrItem(identifier,title==self->tr("Untitled")?self->tr("Seerr Request #%1").arg(requestId):title,
                QStringLiteral("tex_dl_dst_video"),QStringLiteral("explore_category_network"));
            QList<int> seasons;
            for(const auto season:request.value(QStringLiteral("seasons")).toArray())
                seasons.push_back(season.toObject().value(QStringLiteral("seasonNumber")).toInt(-1));
            const auto ids=seerrSabIds(media,seasons,is4k,type==QLatin1String("tv"));
            if(!ids.isEmpty())sabIds.insert(identifier,ids);
            const auto tracked=seerrTrackedStatus(media,seasons,is4k);
            row.detail=tracked.isEmpty()?self->tr("Seerr · %1").arg(seerrMediaStatus(status)):self->tr("Seerr · %1").arg(tracked);
            row.value=row.title+QStringLiteral("\n\n")+row.detail+QStringLiteral("\n")+self->tr("Managed by Seerr and the configured download client.");
            row.action=Action::Information;
            row.artwork=state.posters.value(identifier);
            rows.append(std::move(row));
            // The transfer is out of SABnzbd (no live queue ids) and the media is not yet Available: try to
            // resolve the imported Jellyfin item so the row can offer Watch as soon as it is actually playable.
            if(ids.isEmpty() && self->jellyfin_.signedIn())
                self->resolveSeerrJellyfin(identifier,title,type,type==QLatin1String("movie")?QString::number(id):QString(),is4k);
            if(state.titles.contains(key) || state.downloadDetails.contains(key))continue;
            state.downloadDetails.insert(key);
            state.downloadClient.details(type,id,[self,key,identifier](QJsonObject details,QString error){
                if(!self || !self->seerr_)return;
                auto& state=*self->seerr_;
                state.downloadDetails.remove(key);
                if(!error.isEmpty())return;
                const QString name=seerrTitle(details);
                if(!name.isEmpty() && name!=self->tr("Untitled"))state.titles.insert(key,name);
                const QString path=details.value(QStringLiteral("posterPath")).toString();
                for(auto& row:state.downloadRows)if(row.id==identifier) {
                    row.title=name;
                    row.value=name+QStringLiteral("\n\n")+row.detail;
                }
                self->loadSeerrPoster(identifier,path);
                if(!self->pages_.isEmpty() && self->pages_.last().downloads && self->message_.isEmpty() && !self->optionsOpen_)
                    self->reloadSteamDownloads();
            });
        }
        if(rows.isEmpty()) {
            auto row=seerrItem(QStringLiteral("download:seerr:empty"),self->tr("Seerr"),QStringLiteral("tex_video"),QStringLiteral("explore_plugin_full"));
            row.artwork=serviceIcons::seerr();
            row.detail=self->tr("No active Seerr downloads");
            row.action=Action::Unavailable;
            rows.append(std::move(row));
        }
        state.downloadRows=std::move(rows);
        state.sabIds=std::move(sabIds);
        self->refreshSeerrSabStatus();
        if(!self->pages_.isEmpty() && self->pages_.last().downloads && self->message_.isEmpty() && !self->optionsOpen_)
            self->reloadSteamDownloads();
    },true);
}


void Shell::refreshSeerrSabStatus() {
    if(!seerr_ || diagnostics::safeMode())return;
    auto& state=*seerr_;
    if(state.sabBusy)return;
    const auto config=seerrConfig::loadSab(dataDirectory_);
    if(config!=state.sabActive) {
        state.sabActive=config;
        state.sabStates.clear();
        state.sabClient.configure(SabnzbdClient::normalizeServer(config.server),config.apiKey);
    }
    const auto epoch=++state.sabEpoch;
    if(config.server.isEmpty())return;
    QSet<QString> unique;
    for(const auto& ids:state.sabIds)for(const auto& id:ids)unique.insert(id);
    if(unique.isEmpty()){state.sabStates.clear();return;}
    QPointer<Shell> self(this);
    state.sabClient.status(unique.values(),[self,epoch](QHash<QString,bool> states,QString error){
        if(!self || !self->seerr_ || self->seerr_->sabEpoch!=epoch)return;
        auto& ui=*self->seerr_;
        ui.sabStates=error.isEmpty()?std::move(states):QHash<QString,bool>{};
        for(auto& row:ui.downloadRows) {
            const auto ids=ui.sabIds.value(row.id);
            const bool paused=!ids.isEmpty() && std::all_of(ids.cbegin(),ids.cend(),[&](const QString& id){
                return ui.sabStates.value(id,false);
            });
            if(!paused)continue;
            row.detail=self->tr("Seerr · Paused in SABnzbd");
            row.value=row.title+QStringLiteral("\n\n")+row.detail;
        }
        if(!self->pages_.isEmpty() && self->pages_.last().downloads && self->message_.isEmpty() && !self->optionsOpen_)
            self->reloadSteamDownloads();
    });
}

void Shell::addSeerrDownloadOptions(const Entry& entry) {
    if(!seerr_ || seerr_->sabBusy || diagnostics::safeMode())return;
    const auto& ui=*seerr_;
    const auto ids=ui.sabIds.value(entry.id);
    // A request whose transfer has left SABnzbd has no live controls, but a resolved Jellyfin item does:
    // Watch is the one action its menu offers while it waits to be marked Available.
    if(ids.isEmpty()) {
        if(ui.jellyfinMatch.contains(entry.id))
            addOption({"",tr("Watch")},OptionCommand::SeerrDownloadWatch,"seerr-jellyfin");
        return;
    }
    bool anyPaused=false,anyRunning=false;
    for(const auto& id:ids) {
        const auto state=ui.sabStates.constFind(id);
        if(state==ui.sabStates.cend())return;
        if(*state)anyPaused=true;else anyRunning=true;
    }
    if(anyRunning)addOption({"",tr("Pause")},OptionCommand::SeerrDownloadPause,"seerr-sabnzbd");
    if(anyPaused)addOption({"",tr("Resume")},OptionCommand::SeerrDownloadResume,"seerr-sabnzbd");
    addOption({"",tr("Cancel")},OptionCommand::SeerrDownloadCancel,"seerr-sabnzbd");
}

void Shell::controlSeerrDownload(const Entry& entry,OptionCommand command) {
    if(!seerr_ || seerr_->sabBusy || diagnostics::safeMode())return;
    auto& ui=*seerr_;
    if(command==OptionCommand::SeerrDownloadWatch) { watchSeerrJellyfin(entry.id); return; }
    const auto ids=ui.sabIds.value(entry.id);
    const auto config=seerrConfig::loadSab(dataDirectory_);
    if(ids.isEmpty() || config.server.isEmpty() || config!=ui.sabActive ||
       std::any_of(ids.cbegin(),ids.cend(),[&](const QString& id){return !ui.sabStates.contains(id);})) {
        notify(tr("Download Management"),tr("The transfer changed. Wait for its status to refresh."));
        return;
    }
    const auto perform=[this,entry,ids,config,command]{
        if(!seerr_ || seerr_->sabBusy || diagnostics::safeMode() ||
           seerr_->sabIds.value(entry.id)!=ids || seerrConfig::loadSab(dataDirectory_)!=config)return;
        auto& state=*seerr_;state.sabBusy=true;++state.sabEpoch;
        QPointer<Shell> self(this);
        const auto done=[self](QString error){
            if(!self || !self->seerr_)return;
            self->seerr_->sabBusy=false;
            self->seerr_->sabStates.clear();
            self->notify(self->tr("Download Management"),error.isEmpty()?self->tr("Download client updated"):error);
            self->refreshSeerrDownloads();
            emit self->changed();
        };
        if(command==OptionCommand::SeerrDownloadCancel)state.sabClient.cancel(ids,done);
        else state.sabClient.setPaused(ids,command==OptionCommand::SeerrDownloadPause,done);
    };
    if(command==OptionCommand::SeerrDownloadCancel) {
        const auto title=tr("Cancel %1?").arg(entry.title);
        showConfirm(title,title+QStringLiteral("\n\n")+tr("SABnzbd keeps downloaded files and may clear temporary data. Seerr keeps the request, and the server may grab a replacement."),perform);
    } else perform();
}

// The Seerr->Jellyfin resolution: find the imported Jellyfin item for a request whose transfer is done but whose
// media is not yet Available. The match itself is the pure resolveSeerrWatch() policy (seerr_jellyfin_match.h):
// movies only, exact TMDB equality (never a same-title id fallback), the 4K width rule, and no TV Watch in Stage 0.
void Shell::resolveSeerrJellyfin(const QString& rowId,const QString& title,const QString& mediaType,const QString& tmdbId,bool is4k) {
    if(!seerr_ || !jellyfin_.signedIn())return;
    auto& ui=*seerr_;
    if(ui.jellyfinResolvePending.size()>=16 || ui.jellyfinMatch.contains(rowId))return;
    // TV requests have no directly playable Watch in Stage 0: leave the row on its Seerr status, never claim
    // completion. Season/episode matching arrives in Stage 1.
    if(mediaType==QLatin1String("tv"))return;
    const QString key=title.trimmed().toCaseFolded();
    if(key.isEmpty() || ui.jellyfinTitleKey.value(rowId)==key)return;
    ui.jellyfinTitleKey.insert(rowId,key);
    ui.jellyfinTitle.insert(rowId,title.trimmed());
    ui.seerrMediaType.insert(rowId,mediaType);
    ui.seerrIs4k.insert(rowId,is4k);
    if(!tmdbId.isEmpty())ui.seerrTmdbId.insert(rowId,tmdbId);
    ui.jellyfinResolvePending.insert(rowId);
    if(!ui.jellyfinResolveHooked) {
        ui.jellyfinResolveHooked=true;
        connect(&jellyfin_,&JellyfinClient::pageReady,this,[this](quint64 request,const JellyfinPage& page){
            if(!seerr_)return;
            const QString rowId=seerr_->jellyfinResolveRow.take(request);
            if(rowId.isEmpty() || !seerr_->jellyfinResolvePending.contains(rowId))return;
            seerr_->jellyfinResolvePending.remove(rowId);
            seerr_->jellyfinTitleKey.remove(rowId);   // allow a later refresh to re-resolve an item still importing
            const SeerrWatchRequest watchRequest{
                seerr_->seerrMediaType.value(rowId),
                seerr_->seerrTmdbId.value(rowId),
                seerr_->jellyfinTitle.value(rowId),
                seerr_->seerrIs4k.value(rowId)};
            const SeerrWatchDecision decision=resolveSeerrWatch(watchRequest,page.items);
            if(decision.state==SeerrWatchState::Watch)applySeerrJellyfinMatch(rowId,decision.item);
        });
        // A failed search must release its row so the next poll retries; any other Jellyfin failure (a home
        // list, a playback, a sign-in) carries a request id that is not in the map and is left untouched.
        connect(&jellyfin_,&JellyfinClient::failed,this,[this](quint64 request,const QString&){
            if(!seerr_)return;
            const QString rowId=seerr_->jellyfinResolveRow.take(request);
            if(rowId.isEmpty())return;
            seerr_->jellyfinResolvePending.remove(rowId);
            seerr_->jellyfinTitleKey.remove(rowId);   // allow a later refresh to re-resolve
        });
    }
    const quint64 request=jellyfin_.search(title);
    ui.jellyfinResolveRow.insert(request,rowId);
}

void Shell::applySeerrJellyfinMatch(const QString& rowId,const JellyfinItem& item) {
    if(!seerr_)return;
    seerr_->jellyfinMatch.insert(rowId,item);
    for(auto& row:seerr_->downloadRows)if(row.id==rowId) {
        row.detail=tr("Ready to watch · Seerr · %1").arg(item.type==QLatin1String("Episode")?item.seriesName:item.title);
        row.value=row.title+QStringLiteral("\n\n")+row.detail+QStringLiteral("\n")+tr("Watch opens it in Jellyfin.");
    }
    emit changed();
}

void Shell::watchSeerrJellyfin(const QString& rowId) {
    if(!seerr_ || !seerr_->jellyfinMatch.contains(rowId))return;
    const JellyfinItem item=seerr_->jellyfinMatch.value(rowId);
    if(item.id.isEmpty() || !jellyfin_.signedIn())return;
    Entry entry;
    entry.id=QStringLiteral("jellyfin:")+item.id;
    entry.title=item.title;
    entry.value=item.id;
    entry.action=Action::JellyfinPlay;
    entry.mediaKind=item.type==QLatin1String("Movie")?QStringLiteral("Movie"):QStringLiteral("Episode");
    entry.option=item.type==QLatin1String("Audio");
    entry.resumeMs=item.resumeMs;entry.durationMs=item.durationMs;entry.played=item.played;entry.favorite=item.favorite;entry.seriesId=item.seriesId;
    QVector<Entry> rows{entry};
    playJellyfinQueue(rows,0,false);
}

void Shell::loadSeerrPoster(const QString& rowId,const QString& path) {
    static const QRegularExpression valid(QStringLiteral("^/[A-Za-z0-9_-]{1,100}\\.(?:jpg|jpeg|png)$"));
    if(!seerr_ || !dlnaNetwork_ || !valid.match(path).hasMatch())return;
    auto& state=*seerr_;
    if(state.posters.contains(rowId) || state.posterPending.contains(rowId) || state.posterPending.size()>=24)return;
    state.posterPending.insert(rowId);
    QNetworkRequest request(QUrl(QStringLiteral("https://image.tmdb.org/t/p/w185")+path));
    request.setTransferTimeout(8000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    auto* reply=dlnaNetwork_->get(request);
    constexpr qint64 limit=1024*1024;
    auto bytes=std::make_shared<QByteArray>();
    const auto consume=[reply,bytes]{
        bytes->append(reply->read(limit+1-bytes->size()));
        if(bytes->size()>limit)reply->abort();
    };
    connect(reply,&QNetworkReply::readyRead,this,consume);
    connect(reply,&QNetworkReply::finished,this,[this,reply,bytes,consume,rowId]{
        consume();
        reply->deleteLater();
        if(!seerr_)return;
        auto& state=*seerr_;state.posterPending.remove(rowId);
        if(reply->error()!=QNetworkReply::NoError || bytes->size()>limit ||
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()!=200)return;
        QBuffer buffer(bytes.get());
        if(!buffer.open(QIODevice::ReadOnly))return;
        QImageReader reader(&buffer);
        const auto size=reader.size();
        if(size.isEmpty() || size.width()>800 || size.height()>1200)return;
        reader.setScaledSize(size.scaled(192,288,Qt::KeepAspectRatio));
        const auto image=reader.read();
        if(image.isNull())return;
        if(state.posters.size()>=128)state.posters.erase(state.posters.begin());
        state.posters.insert(rowId,image);
        for(auto& page:pages_)for(auto& row:page.entries)
            if(row.id==rowId || row.id+page.seerr==rowId)row.artwork=image;
        for(auto& row:state.downloadRows)if(row.id==rowId)row.artwork=image;
        emit changed();
    });
}

void Shell::activateSeerr(const Entry& entry) {
    if(!seerr_) {
        seerr_=std::make_shared<SeerrUi>();
        const auto config=seerrConfig::load(dataDirectory_);
        seerr_->server=config.server;
        seerr_->apiKey=config.apiKey;
        seerr_->client.configure(SeerrClient::normalizeServer(seerr_->server),seerr_->apiKey);
    }
    auto& ui=*seerr_;
    const QString command=entry.value;
    if(command==QStringLiteral("open") || command==QStringLiteral("home")) {
        ++ui.generation;ui.error.clear();
        if(!ui.configDirty)ui.connection=ui.server.isEmpty()||ui.apiKey.isEmpty()?QString():tr("Saved configuration");
        bool hasPage=false;
        for(const auto& page:pages_)if(page.seerr.startsWith(QStringLiteral("home:")))hasPage=true;
        if(!hasPage) { Page page;page.title=tr("Seerr");page.seerr=QStringLiteral("home:%1").arg(++ui.generation);pages_.push_back(std::move(page)); }
        refreshSeerrPage();emit changed();return;
    }
    if(pages_.isEmpty() || pages_.last().seerr.isEmpty())return;
    const QString active=pages_.last().seerr;
    if(command==QLatin1String("sab")) {
        if(pages_.size()>=32)return;
        ui.sabEdit=seerrConfig::loadSab(dataDirectory_);ui.error.clear();
        Page page;page.title=tr("Download Client");page.seerr=QStringLiteral("sab:config");
        pages_.push_back(std::move(page));refreshSeerrPage();emit changed();return;
    }
    if(command.startsWith(QLatin1String("sab-field:"))) {
        const auto field=command.section(':',1,1);
        const bool secret=field==QLatin1String("api-key");
        emit requestJellyfinFieldEdit(QStringLiteral("seerr:sab-")+field,entry.title,
            secret?QString():ui.sabEdit.server,secret);
        return;
    }
    if(command==QLatin1String("sab-save") || command==QLatin1String("sab-disable")) {
        const bool disable=command==QLatin1String("sab-disable");
        if(seerrConfig::saveSab(dataDirectory_,disable?seerrConfig::SabConfig{}:ui.sabEdit,ui.error)) {
            if(disable)ui.sabEdit={};
            ++ui.sabEpoch;ui.sabStates.clear();
            ui.error=disable?tr("Download client disabled"):tr("Download client saved");
        }
        refreshSeerrPage();emit changed();return;
    }
    if(command.startsWith(QStringLiteral("field:"))) {
        const auto field=command.section(':',1,1);
        const bool secret=field==QStringLiteral("apiKey");
        emit requestJellyfinFieldEdit(QStringLiteral("seerr:")+field,
            field==QStringLiteral("server")?tr("Seerr Server URL"):tr("Seerr API Key"),
            field==QStringLiteral("server")?ui.server:QString(),secret);
        return;
    }
    if(command==QStringLiteral("search")) {
        emit requestJellyfinFieldEdit(QStringLiteral("seerr:search"),tr("Search Media"),ui.searchTerm,false);return;
    }
    if(command==QStringLiteral("requests")) { openSeerrRequests(0);return; }
    if(command==QStringLiteral("refresh")) {
        if(active.startsWith(QStringLiteral("requests:"))) { openSeerrRequests(active.section(':',1,1).toInt());return; }
        if(active.startsWith(QStringLiteral("detail:"))) { loadSeerrDetails(ui.mediaType,ui.tmdbId,active);return; }
        refreshSeerrPage();emit changed();return;
    }
    if(command==QStringLiteral("test")) { testSeerrConnection();return; }
    if(command==QStringLiteral("save")) { saveSeerrConfig();return; }
    if(command.startsWith(QStringLiteral("result:"))) {
        const auto parts=command.split(':');if(parts.size()<3)return;
        ui.mediaType=parts[1];ui.tmdbId=parts[2].toInt();ui.title=entry.title;
        ui.selectedSeasons.clear();ui.error.clear();
        if(pages_.size()<32) { Page page;page.title=ui.title;page.seerr=QStringLiteral("detail:%1:%2").arg(ui.mediaType).arg(ui.tmdbId);pages_.push_back(std::move(page)); }
        loadSeerrDetails(ui.mediaType,ui.tmdbId,pages_.last().seerr);return;
    }
    if(command.startsWith(QStringLiteral("season:"))) {
        const int season=command.section(':',1,1).toInt();
        if(ui.selectedSeasons.contains(season))ui.selectedSeasons.removeAll(season);else ui.selectedSeasons.push_back(season);
        refreshSeerrPage();emit changed();return;
    }
    if(command==QStringLiteral("request")) {
        if(ui.submitting || ui.detail.isEmpty() || (ui.mediaType==QStringLiteral("tv") && ui.selectedSeasons.isEmpty()))return;
        const QString type=ui.mediaType,title=ui.title;const int id=ui.tmdbId;const QList<int> seasons=ui.selectedSeasons;
        const QString target=seasons.isEmpty()?tr("Send this request to Seerr?"):tr("Request %1 selected season(s) from Seerr?").arg(seasons.size());
        showConfirm(tr("Request %1?").arg(title),target+QStringLiteral("\n\n")+tr("Seerr will use its configured default server, quality, and notification settings."),
            [this,type,id,seasons]{submitSeerrRequest(type,id,seasons);});
        return;
    }
    if(command.startsWith(QStringLiteral("request-page:"))) { openSeerrRequests(command.section(':',1,1).toInt());return; }
    if(command.startsWith(QStringLiteral("search-page:"))) { loadSeerrSearch(ui.searchTerm,command.section(':',1,1).toInt());return; }
    refreshSeerrPage();emit changed();
}

void Shell::seerrField(const QString& field,const QString& value) {
    if(!seerr_)activateSeerr(Entry{.value=QStringLiteral("open")});
    auto& ui=*seerr_;
    const QString key=field.startsWith(QStringLiteral("seerr:"))?field.mid(6):field;
    if(key.startsWith(QLatin1String("sab-"))) {
        if(key==QLatin1String("sab-server")) {
            const auto url=SabnzbdClient::normalizeServer(value);
            if(!value.trimmed().isEmpty() && url.isEmpty())ui.error=tr("Enter a SABnzbd address without credentials or query parameters.");
            else {
                if(url!=SabnzbdClient::normalizeServer(ui.sabEdit.server))ui.sabEdit.apiKey.clear();
                ui.sabEdit.server=url.toString(QUrl::FullyEncoded);
                ui.error.clear();
            }
        } else if(key==QLatin1String("sab-api-key")) {ui.sabEdit.apiKey=value.trimmed();ui.error.clear();}
        refreshSeerrPage();emit changed();return;
    }
    if(key==QStringLiteral("search")) {
        const auto term=value.trimmed();if(term.isEmpty())return;
        ui.searchTerm=term;ui.error.clear();loadSeerrSearch(term,1);return;
    }
    if(key==QStringLiteral("server")) {
        const QUrl normalized=SeerrClient::normalizeServer(value);
        if(!value.trimmed().isEmpty() && normalized.isEmpty()) {
            ui.error=tr("Enter a server address without a username, password, or query parameters.");
            refreshSeerrPage();emit changed();return;
        }
        if(normalized!=SeerrClient::normalizeServer(ui.server)){ui.apiKey.clear();ui.titles.clear();}
        ui.server=normalized.toString(QUrl::FullyEncoded);ui.configDirty=true;ui.submitting=false;ui.connection.clear();ui.error.clear();ui.client.configure({},{});
    } else if(key==QStringLiteral("apiKey")) {
        ui.apiKey=value.trimmed();ui.configDirty=true;ui.submitting=false;ui.connection.clear();ui.error.clear();
        ui.client.configure({},{});
    } else return;
    refreshSeerrPage();emit changed();
}

void Shell::refreshSeerrPage() {
    if(!seerr_)return;
    auto& ui=*seerr_;
    if(pages_.isEmpty())return;
    auto& page=pages_.last();const auto token=page.seerr;
    const int selection=page.selection;const QString focused=page.entries.value(selection).id;page.entries.clear();
    const auto add=[&](QString id,QString title,QString value,QString detail={}) {
        auto row=seerrItem(id,title,QStringLiteral("23n"),QStringLiteral("qgl/icontex"),value);
        if(value.isEmpty()) {
            row.action=Action::Information;
            row.value=detail.isEmpty()?title:title+QStringLiteral("\n\n")+detail;
        }
        row.detail=detail;page.entries.push_back(std::move(row));
    };
    if(token==QLatin1String("sab:config")) {
        page.title=tr("Download Client");
        add(QStringLiteral("seerr:sab-server"),tr("SABnzbd Web UI"),QStringLiteral("sab-field:server"));
        page.entries.last().fieldValue=ui.sabEdit.server;
        add(QStringLiteral("seerr:sab-api-key"),tr("API Key"),QStringLiteral("sab-field:api-key"));
        page.entries.last().fieldValue=ui.sabEdit.apiKey.isEmpty()?tr("Not set"):tr("Set");
        add(QStringLiteral("seerr:sab-save"),tr("Save"),QStringLiteral("sab-save"),
            tr("Enable Pause, Resume, and Cancel for matching SABnzbd transfers."));
        if(!seerrConfig::loadSab(dataDirectory_).server.isEmpty())
            add(QStringLiteral("seerr:sab-disable"),tr("Disable"),QStringLiteral("sab-disable"));
        if(!ui.error.isEmpty())add(QStringLiteral("seerr:sab-status"),tr("Status"),QString(),ui.error);
    } else if(token.startsWith(QStringLiteral("home:"))) {
        page.title=tr("Seerr");
        add(QStringLiteral("seerr:search"),tr("Search"),QStringLiteral("search"),tr("Find a movie or TV series."));
        page.entries.last().disabled=ui.configDirty || ui.server.isEmpty() || ui.apiKey.isEmpty();
        add(QStringLiteral("seerr:requests"),tr("My Requests"),QStringLiteral("requests"),tr("View request and media availability states."));
        page.entries.last().disabled=ui.configDirty || ui.server.isEmpty() || ui.apiKey.isEmpty();
        add(QStringLiteral("seerr:server"),tr("Server URL"),QStringLiteral("field:server"),tr("Cross to edit the Seerr server address."));
        page.entries.last().fieldValue=ui.server;
        add(QStringLiteral("seerr:api-key"),tr("API Key"),QStringLiteral("field:apiKey"),tr("Cross to enter the Seerr API key. The value is masked."));
        page.entries.last().fieldValue=ui.apiKey.isEmpty()?tr("Not set"):tr("Set");
        add(QStringLiteral("seerr:test"),tr("Test Connection"),QStringLiteral("test"),tr("Check that this server accepts the configured API key."));
        add(QStringLiteral("seerr:save"),tr("Save Configuration"),QStringLiteral("save"),tr("Save the server and API key securely on this device."));
        if(!ui.connection.isEmpty())add(QStringLiteral("seerr:connection"),tr("Connection"),QString(),ui.connection);
        if(!ui.error.isEmpty())add(QStringLiteral("seerr:error"),tr("Status"),QString(),ui.error);
        add(QStringLiteral("seerr:download-client"),tr("Download Client"),QStringLiteral("sab"),tr("Optional SABnzbd controls for Seerr transfers."));
    } else if(token.startsWith(QStringLiteral("search:"))) {
        page.title=tr("Search: %1").arg(ui.searchTerm);
        add(QStringLiteral("seerr:search-again"),tr("Search Again"),QStringLiteral("search"));
        const auto items=ui.searchResults.value(QStringLiteral("results")).toArray();
        for(const auto value:items) {
            const auto object=value.toObject();const QString type=seerrMediaType(object);const int id=object.value(QStringLiteral("id")).toInt();
            if(id<=0 || type.isEmpty())continue;
            const auto mediaInfo=object.value(QStringLiteral("mediaInfo")).toObject();
            auto title=seerrTitle(object);auto row=seerrItem(QStringLiteral("seerr:result:%1:%2").arg(type).arg(id),title,QStringLiteral("tex_video"),QStringLiteral("explore_plugin_full"),
                QStringLiteral("result:%1:%2").arg(type).arg(id));
            const QString date=object.value(type==QStringLiteral("tv")?QStringLiteral("firstAirDate"):QStringLiteral("releaseDate")).toString();
            row.detail=type==QStringLiteral("tv")?tr("TV"):tr("Movie");
            if(!date.isEmpty())row.detail+=QStringLiteral("  •  ")+date.left(4);
            if(!mediaInfo.isEmpty())row.detail+=QStringLiteral("  •  ")+seerrMediaStatus(seerrInt(mediaInfo.value(QStringLiteral("status"))));
            row.artwork=ui.posters.value(row.id);
            loadSeerrPoster(row.id,object.value(QStringLiteral("posterPath")).toString());
            page.entries.push_back(std::move(row));
        }
        const int pageNumber=token.section(':',1,1).toInt();
        if(seerrHasNext(ui.searchResults,pageNumber))add(QStringLiteral("seerr:search-next"),tr("Next Page"),QStringLiteral("search-page:%1").arg(pageNumber+1));
        if(pageNumber>1)add(QStringLiteral("seerr:search-previous"),tr("Previous Page"),QStringLiteral("search-page:%1").arg(pageNumber-1));
        if(!ui.error.isEmpty())add(QStringLiteral("seerr:error"),tr("Search Status"),QString(),ui.error);
    } else if(token.startsWith(QStringLiteral("detail:"))) {
        page.title=ui.title;
        const auto mediaInfo=ui.detail.value(QStringLiteral("mediaInfo")).toObject();
        const QString tracked=seerrTrackedStatus(ui.detail);
        add(QStringLiteral("seerr:media-status"),ui.title.isEmpty()?tr("Media Status"):ui.title,QString(),
            tracked.isEmpty()?seerrMediaStatus(seerrInt(mediaInfo.value(QStringLiteral("status")))):tracked);
        const auto overview=ui.detail.value(QStringLiteral("overview")).toString();
        auto& mediaRow=page.entries.last();
        mediaRow.artwork=ui.posters.value(mediaRow.id+token);
        loadSeerrPoster(mediaRow.id+token,ui.detail.value(QStringLiteral("posterPath")).toString());
        if(!overview.isEmpty()) {
            QStringList metadata;
            const QString date=ui.detail.value(ui.mediaType==QStringLiteral("tv")?QStringLiteral("firstAirDate"):QStringLiteral("releaseDate")).toString();
            if(date.size()>=4)metadata.append(date.left(4));
            const int runtime=ui.detail.value(QStringLiteral("runtime")).toInt();
            if(runtime>0)metadata.append(tr("%1 min").arg(runtime));
            for(const auto genre:ui.detail.value(QStringLiteral("genres")).toArray()) {
                const QString name=genre.toObject().value(QStringLiteral("name")).toString();
                if(!name.isEmpty())metadata.append(name);
            }
            const QString description=metadata.isEmpty()?overview:metadata.join(QStringLiteral(" · "))+QStringLiteral("\n\n")+overview;
            add(QStringLiteral("seerr:overview"),tr("Overview"),QString(),description);
        }
        if(ui.mediaType==QStringLiteral("tv")) {
            const auto seasons=ui.detail.value(QStringLiteral("seasons")).toArray();
            QHash<int,int> statuses;
            for(const auto value:mediaInfo.value(QStringLiteral("seasons")).toArray()) {
                const auto season=value.toObject();
                statuses.insert(season.value(QStringLiteral("seasonNumber")).toInt(-1),season.value(QStringLiteral("status")).toInt(-1));
            }
            for(const auto seasonValue:seasons) {
                const auto season=seasonValue.toObject();const int number=season.value(QStringLiteral("seasonNumber")).toInt(-1);
                if(number<0)continue;
                const bool selected=ui.selectedSeasons.contains(number);
                auto row=seerrItem(QStringLiteral("seerr:season:%1").arg(number),tr("Season %1%2").arg(number).arg(selected?tr("  ✓"):QString()),QStringLiteral("tex_video"),QStringLiteral("explore_plugin_full"),
                    QStringLiteral("season:%1").arg(number));
                const QString name=season.value(QStringLiteral("name")).toString();
                const int seasonStatus=statuses.value(number,mediaInfo.value(QStringLiteral("status")).toInt()==5?5:-1);
                row.disabled=seasonStatus==2 || seasonStatus==3 || seasonStatus==5 || seasonStatus==6;
                row.detail=name.isEmpty()?QString():name;
                const auto seasonTracked=seerrTrackedStatus(ui.detail,{number});
                row.detail+=row.detail.isEmpty()?QString():QStringLiteral("  •  ");
                row.detail+=!seasonTracked.isEmpty()?seasonTracked:(seasonStatus>=0?seerrMediaStatus(seasonStatus):tr("Status unavailable"));
                page.entries.push_back(std::move(row));
            }
        }
        const QString label=ui.mediaType==QStringLiteral("tv")&&!ui.selectedSeasons.isEmpty()?tr("Request Selected Seasons"):tr("Request");
        add(QStringLiteral("seerr:request"),label,QStringLiteral("request"),tr("Cross opens a confirmation before sending this request."));
        page.entries.last().disabled=ui.detail.isEmpty() || ui.submitting ||
            (ui.mediaType==QStringLiteral("tv") && ui.selectedSeasons.isEmpty()) ||
            (ui.mediaType==QStringLiteral("movie") && mediaInfo.value(QStringLiteral("status")).toInt()>=2 && mediaInfo.value(QStringLiteral("status")).toInt()<=6);
        add(QStringLiteral("seerr:refresh"),tr("Refresh Details"),QStringLiteral("refresh"));
        if(!ui.error.isEmpty())add(QStringLiteral("seerr:error"),tr("Request Status"),QString(),ui.error);
    } else if(token.startsWith(QStringLiteral("requests:"))) {
        page.title=tr("My Requests");
        const auto items=ui.requestList.value(QStringLiteral("results")).toArray();
        for(const auto value:items) {
            const auto request=value.toObject();const auto media=seerrMedia(request);const auto requester=request.value(QStringLiteral("requestedBy")).toObject();
            const int reqStatus=seerrInt(request.value(QStringLiteral("status")));
            const bool is4k=request.value(QStringLiteral("is4k")).toBool();
            const int mediaStatus=seerrInt(media.value(is4k?QStringLiteral("status4k"):QStringLiteral("status")));
            QString mediaState=seerrMediaStatus(mediaStatus);
            QList<int> seasons;
            for(const auto seasonValue:request.value(QStringLiteral("seasons")).toArray())
                seasons.push_back(seasonValue.toObject().value(QStringLiteral("seasonNumber")).toInt(-1));
            const QString tracked=seerrTrackedStatus(media,seasons,is4k);
            if(!tracked.isEmpty())mediaState=tracked;
            const QString status=QStringLiteral("%1 · %2").arg(seerrRequestStatus(reqStatus),mediaState);
            const QString type=seerrMediaType(media);
            const int id=media.value(QStringLiteral("tmdbId")).toInt();
            const int requestId=request.value(QStringLiteral("id")).toInt();
            const QString name=ui.titles.value(type+QLatin1Char(':')+QString::number(id));
            add(QStringLiteral("seerr:request:%1").arg(requestId),name.isEmpty()?tr("Request #%1").arg(requestId):name,QString(),status);
            auto& row=page.entries.last();
            row.container=QStringLiteral("explore_plugin_full");
            row.texture=QStringLiteral("tex_video");
            const QString user=requester.value(QStringLiteral("displayName")).toString();
            if(!user.isEmpty())row.detail+=QStringLiteral(" · ")+user;
        }
        const int skip=token.section(':',1,1).toInt();
        add(QStringLiteral("seerr:requests-refresh"),tr("Refresh"),QStringLiteral("refresh"));
        if(skip>0)add(QStringLiteral("seerr:requests-previous"),tr("Previous Page"),QStringLiteral("request-page:%1").arg(std::max(0,skip-20)));
        if(seerrHasNext(ui.requestList,skip/20+1))add(QStringLiteral("seerr:requests-next"),tr("Next Page"),QStringLiteral("request-page:%1").arg(skip+20));
        if(!ui.error.isEmpty())add(QStringLiteral("seerr:error"),tr("Request List Status"),QString(),ui.error);
    }
    for(int i=0;i<page.entries.size();++i)if(page.entries[i].id==focused){page.selection=i;break;}
    page.selection=std::clamp(page.selection,0,std::max(0,int(page.entries.size())-1));
}

void Shell::loadSeerrSearch(const QString& term,int pageNumber) {
    if(!seerr_)return;auto& ui=*seerr_;ui.error.clear();ui.searchResults={};
    const QString requestToken=QStringLiteral("search:%1:%2").arg(pageNumber).arg(++ui.generation);
    if(!pages_.isEmpty()) {
        if(pages_.last().seerr.startsWith(QStringLiteral("search:"))) {
            pages_.last().seerr=requestToken;
            pages_.last().title=tr("Searching...");
        } else {Page page;page.title=tr("Searching...");page.seerr=requestToken;pages_.push_back(std::move(page));}
    }
    refreshSeerrPage();emit changed();
    const auto token=pages_.last().seerr;QPointer<Shell> self(this);
    ui.client.search(term,pageNumber,[self,token](QJsonObject data,QString error){
        if(!self || !self->seerr_)return;auto& state=*self->seerr_;
        const bool pageStillExists=std::any_of(self->pages_.cbegin(),self->pages_.cend(),[&](const auto& page){return page.seerr==token;});
        if(!pageStillExists)return;
        state.searchResults=std::move(data);state.error=std::move(error);
        for(const auto value:state.searchResults.value(QStringLiteral("results")).toArray()) {
            const auto hit=value.toObject();
            const QString type=seerrMediaType(hit);
            const int id=hit.value(QStringLiteral("id")).toInt();
            if(!type.isEmpty() && id>0 && state.titles.size()<1024)
                state.titles.insert(type+QLatin1Char(':')+QString::number(id),seerrTitle(hit));
        }
        if(!self->pages_.isEmpty() && self->pages_.last().seerr==token){self->refreshSeerrPage();emit self->changed();}
    });
}

void Shell::loadSeerrDetails(const QString& type,int tmdbId,const QString& token) {
    if(!seerr_)return;auto& ui=*seerr_;ui.error.clear();ui.detail={};
    Q_UNUSED(token);
    const QString requestToken=QStringLiteral("detail:%1:%2:%3").arg(type).arg(tmdbId).arg(++ui.generation);
    if(!pages_.isEmpty())pages_.last().seerr=requestToken;
    refreshSeerrPage();emit changed();QPointer<Shell> self(this);
    ui.client.details(type,tmdbId,[self,requestToken,type,tmdbId](QJsonObject data,QString error){
        if(!self || !self->seerr_)return;
        const bool pageStillExists=std::any_of(self->pages_.cbegin(),self->pages_.cend(),[&](const auto& page){return page.seerr==requestToken;});
        if(!pageStillExists)return;
        auto& state=*self->seerr_;state.detail=std::move(data);state.error=std::move(error);
        if(state.title.isEmpty())state.title=seerrTitle(state.detail);
        if(!self->pages_.isEmpty() && self->pages_.last().seerr==requestToken){self->refreshSeerrPage();emit self->changed();}
    });
}

void Shell::openSeerrRequests(int skip) {
    if(!seerr_)return;auto& ui=*seerr_;ui.error.clear();ui.requestList={};
    const bool initial=pages_.isEmpty() || !pages_.last().seerr.startsWith(QStringLiteral("requests:"));
    const QString requestToken=QStringLiteral("requests:%1:%2").arg(skip).arg(++ui.generation);
    if(!pages_.isEmpty()) {
        if(pages_.last().seerr.startsWith(QStringLiteral("requests:"))) {
            pages_.last().seerr=requestToken;
            pages_.last().title=tr("Loading Requests...");
        } else {Page page;page.title=tr("Loading Requests...");page.seerr=requestToken;pages_.push_back(std::move(page));}
    }
    refreshSeerrPage();emit changed();const auto token=pages_.last().seerr;QPointer<Shell> self(this);
    ui.client.requests(skip,[self,token,initial](QJsonObject data,QString error){
        if(!self || !self->seerr_)return;
        const bool pageStillExists=std::any_of(self->pages_.cbegin(),self->pages_.cend(),[&](const auto& page){return page.seerr==token;});
        if(!pageStillExists)return;
        auto& state=*self->seerr_;state.requestList=std::move(data);state.error=std::move(error);
        if(!self->pages_.isEmpty() && self->pages_.last().seerr==token){
            if(initial){self->pages_.last().entries.clear();self->pages_.last().selection=0;}
            self->refreshSeerrPage();emit self->changed();
        }
        for(const auto value:state.requestList.value(QStringLiteral("results")).toArray()) {
            const auto media=seerrMedia(value.toObject());
            const QString type=seerrMediaType(media);
            const int id=media.value(QStringLiteral("tmdbId")).toInt();
            const QString key=type+QLatin1Char(':')+QString::number(id);
            if(type.isEmpty() || id<=0 || state.titles.contains(key) || state.titles.size()>=1024)continue;
            state.client.details(type,id,[self,token,key](QJsonObject detail,QString error){
                if(!self || !self->seerr_ || !error.isEmpty())return;
                self->seerr_->titles.insert(key,seerrTitle(detail));
                if(!self->pages_.isEmpty() && self->pages_.last().seerr==token){self->refreshSeerrPage();emit self->changed();}
            });
        }
    });
}

void Shell::testSeerrConnection() {
    if(!seerr_)return;auto& ui=*seerr_;ui.error.clear();ui.connection=tr("Testing connection...");
    ui.client.configure(SeerrClient::normalizeServer(ui.server),ui.apiKey);refreshSeerrPage();emit changed();
    const auto token=pages_.isEmpty()?QString():pages_.last().seerr;QPointer<Shell> self(this);
    ui.client.check([self,token](QJsonObject data,QString error){
        if(error.isEmpty() && data.value(QStringLiteral("id")).toInt()<=0)error=tr("The server did not return a Seerr user.");
        if(!self || !self->seerr_)return;
        const bool pageStillExists=std::any_of(self->pages_.cbegin(),self->pages_.cend(),[&](const auto& page){return page.seerr==token;});
        if(!pageStillExists)return;
        auto& state=*self->seerr_;
        state.connection=error.isEmpty()?tr("Connected"):tr("Connection failed: %1").arg(error);
        state.error=std::move(error);
        if(!self->pages_.isEmpty() && self->pages_.last().seerr==token){self->refreshSeerrPage();emit self->changed();}
    });
}

void Shell::saveSeerrConfig() {
    if(!seerr_)return;auto& ui=*seerr_;QString error;
    const auto normalized=SeerrClient::normalizeServer(ui.server);
    if(normalized.isEmpty() || ui.apiKey.trimmed().isEmpty() || ui.connection!=tr("Connected")) {ui.error=tr("Enter the server URL and API key, test the connection, then save.");refreshSeerrPage();emit changed();return;}
    const auto config=seerrConfig::Config{normalized.toString(),ui.apiKey};
    if(!seerrConfig::save(dataDirectory_,config,error)){ui.error=error;refreshSeerrPage();emit changed();return;}
    ui.server=normalized.toString();ui.connection=tr("Configuration saved");ui.error.clear();
    ui.configDirty=false;ui.client.configure(normalized,ui.apiKey);refreshSeerrPage();emit changed();
}

void Shell::submitSeerrRequest(const QString& type,int tmdbId,const QList<int>& seasons) {
    if(!seerr_ || seerr_->submitting)return;
    auto& ui=*seerr_;ui.submitting=true;ui.error.clear();refreshSeerrPage();emit changed();
    const auto token=pages_.isEmpty()?QString():pages_.last().seerr;QPointer<Shell> self(this);
    ui.client.submit(type,tmdbId,seasons,[self,token](QJsonObject data,QString error){
        if(!self || !self->seerr_)return;
        auto& state=*self->seerr_;state.submitting=false;
        const bool pageStillExists=std::any_of(self->pages_.cbegin(),self->pages_.cend(),[&](const auto& page){return page.seerr==token;});
        if(!pageStillExists)return;
        if(error.isEmpty() && data.value(QStringLiteral("id")).toInt()<=0)error=tr("The server did not confirm the request. Check My Requests before retrying.");
        state.error=error.isEmpty()?tr("Request sent. Check My Requests for its service status."):tr("Request failed: %1").arg(error);
        if(error.isEmpty())state.detail={};
        if(!self->pages_.isEmpty() && self->pages_.last().seerr==token){self->refreshSeerrPage();emit self->changed();}
    });
}

}
