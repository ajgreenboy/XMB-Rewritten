#include "shell.h"
#include "rawg_artwork.h"
#include "diagnostics.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace xmb {
namespace {
QString releaseId(const QString& source,const QString& title) {
    return QStringLiteral("release:item:")+source+QLatin1Char(':')+
        QString::fromLatin1(QCryptographicHash::hash(title.toUtf8(),QCryptographicHash::Sha256).toHex());
}
Entry releaseRow(const QString& id,const QString& title,Action action,const QString& value={}) {
    Entry entry;
    entry.id=id;entry.title=title;entry.action=action;entry.value=value;
    entry.container=QStringLiteral("explore_plugin_full");
    entry.texture=action==Action::ReleaseDownload?QStringLiteral("game_tex_default_ps3"):QStringLiteral("item_tex_plain_folder");
    entry.source=QStringLiteral("native");
    return entry;
}
QString coverUrl(const IndexedRelease& release) {
    for(const char* key:{"artwork","thumbnail","cover","image","poster","cover_url","image_url"}) {
        const auto value=release.raw.value(QLatin1String(key));
        const QString url=value.isObject()?value.toObject().value(QStringLiteral("url")).toString():value.toString();
        if(!url.isEmpty())return url;
    }
    return {};
}
QImage readCover(const QByteArray& bytes) {
    QBuffer buffer;buffer.setData(bytes);buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QSize size=reader.size();
    if(size.isEmpty() || size.width()>8192 || size.height()>8192)return {};
    reader.setScaledSize(size.scaled(512,512,Qt::KeepAspectRatio));
    return reader.read();
}
QImage fetchCover(const QUrl& url,const QString& path) {
    QImage cached(path);
    if(!cached.isNull())return cached;
    if(url.scheme()!=QLatin1String("https") && url.scheme()!=QLatin1String("http"))return {};
    QNetworkAccessManager network;
    QNetworkRequest request(url);request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply=network.get(request);
    QEventLoop loop;
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    QByteArray bytes;
    QObject::connect(reply,&QNetworkReply::readyRead,&loop,[&]{
        bytes+=reply->readAll();
        if(bytes.size()>8*1024*1024)reply->abort();
    });
    loop.exec();
    if(reply->error()!=QNetworkReply::NoError)return {};
    bytes+=reply->readAll();
    if(bytes.size()>8*1024*1024)return {};
    QImage image=readCover(bytes);
    if(!image.isNull()) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if(file.open(QIODevice::WriteOnly) && image.save(&file,"PNG"))file.commit();
    }
    return image;
}
QString gameSearchKey(const QString& title) {
    return title.normalized(QString::NormalizationForm_KC).simplified().toCaseFolded();
}
QString coverCachePath(const QString& url,const QString& cacheRoot) {
    return cacheRoot+QLatin1Char('/')+QStringLiteral("hot-")+
        QString::fromLatin1(QCryptographicHash::hash(url.toUtf8(),QCryptographicHash::Sha256).toHex().left(16))+QStringLiteral(".png");
}
// Fetch each game's cover into the same cache the browse page uses, keyed by its list
// position. Online Browse and search share this so a search result carries its box art the
// moment the row lands instead of waiting for a second round-trip.
QHash<int,QImage> fetchCovers(const QVector<GameResult>& games,const QString& cacheRoot) {
    QHash<int,QImage> covers;
    for(int i=0;i<games.size();++i) {
        const QString url=games[i].coverUrl;
        if(url.isEmpty())continue;
        const QImage image=fetchCover(QUrl(url),coverCachePath(url,cacheRoot));
        if(!image.isNull())covers.insert(i,image);
    }
    return covers;
}
// The local cache path `fetchCover` wrote for a URL, or empty when the cover could not be
// cached. Used to surface game art behind the source list without a second network round-trip.
QString cachedCoverPath(const QString& url,const QString& cacheRoot) {
    if(url.isEmpty())return {};
    const QString path=coverCachePath(url,cacheRoot);
    if(!QFileInfo::exists(path)) {
        const QImage image=fetchCover(QUrl(url),path);
        if(image.isNull())return {};
    }
    return path;
}

GameResult importedGame(const ReleaseSource& source,const IndexedRelease& release) {
    GameResult game;
    game.name=release.raw.value(QStringLiteral("gameTitle")).toString().trimmed();
    if(game.name.isEmpty())game.name=release.raw.value(QStringLiteral("name")).toString().trimmed();
    if(game.name.isEmpty())game.name=release.title;
    const QString cover=coverUrl(release);
    if(!cover.isEmpty())game.coverUrl=source.url.resolved(QUrl(cover)).toString();
    QSet<QString> multipart, seen;
    for(const auto& mirror:release.mirrors)
        if(mirror.parts.size()>1)
            for(const auto& part:mirror.parts)multipart.insert(part.uri);
    const auto add=[&](const Download& download,const QString& host) {
        if(download.kind!=QLatin1String("direct") && download.kind!=QLatin1String("magnet"))return;
        if(download.uri.isEmpty() || multipart.contains(download.uri) || seen.contains(download.uri) || game.options.size()>=24)return;
        seen.insert(download.uri);
        GameSourceOption option;
        option.source=source.name;
        option.sourceSlug=source.slug;
        option.sourceUrl=source.url.toString();
        option.releaseTitle=release.title;
        option.fileSize=release.sizeLabel;
        option.mirror=host.isEmpty()?QUrl(download.uri).host():host;
        if(option.mirror.isEmpty() && download.kind==QLatin1String("magnet"))option.mirror=QObject::tr("Magnet");
        option.downloads={download};
        game.options.append(std::move(option));
    };
    for(const auto& mirror:release.mirrors)
        if(mirror.parts.size()==1)add(mirror.parts.first(),mirror.host);
    for(const auto& download:release.downloads)add(download,{});
    return game;
}

QString optionCount(const GameResult& game) {
    QSet<QString> sources;
    for(const auto& option:game.options)sources.insert(option.source);
    if(sources.size()==1 && game.options.size()==1)return game.options.first().source;
    if(sources.size()>1)return QObject::tr("%n source(s)",nullptr,int(sources.size()));
    return QObject::tr("%n download option(s)",nullptr,int(game.options.size()));
}

QVector<Entry> downloadHubRows(const QVector<ReleaseSource>& sources) {
    QVector<Entry> rows;
    rows.append(releaseRow(QStringLiteral("game:search"),QObject::tr("Search Games"),Action::SearchGames));
    rows.append(releaseRow(QStringLiteral("game:browse"),QObject::tr("Browse Popular"),Action::BrowseGames));
    rows.append(releaseRow(QStringLiteral("release:import"),QObject::tr("Import Source"),Action::ImportSource));
    for(const auto& source:sources) {
        auto row=releaseRow(QStringLiteral("release:source:")+source.slug,source.name,Action::ReleaseFolder,source.slug);
        row.detail=QObject::tr("%n release(s)",nullptr,int(source.releases.size()));
        rows.append(std::move(row));
    }
    if(sources.isEmpty()) {
        auto row=releaseRow(QStringLiteral("release:none"),QObject::tr("No sources imported yet."),Action::Unavailable);
        row.detail=QObject::tr("Import Source adds a release index to download from.");
        rows.append(std::move(row));
    }
    return rows;
}

// Browse worker result: the popular games plus their fetched box art (indexed by list position).
struct BrowseOutcome { QVector<GameResult> games; QHash<int,QImage> covers; QString error; };
}

void Shell::refreshReleaseSources(const QUrl& importUrl) {
    const bool importing=!importUrl.isEmpty();
    if(importing && busy())return;
    if(importing)++releaseSourcesGeneration_;
    struct Result { QVector<ReleaseSource> sources; QHash<QString,QString> paths; QString error; };
    auto* watcher=new QFutureWatcher<Result>(this);
    connect(watcher,&QFutureWatcher<Result>::finished,this,[this,watcher,importing]{
        const auto result=watcher->result();watcher->deleteLater();
        releaseSources_=result.sources;releaseLocalPaths_=result.paths;
        rebuildReleaseSources();
        if(importing){busy_=false;status_.clear();busyProgress_=-1.;busySpinnerPhase_=-1.;}
        if(!result.error.isEmpty())showMessage(tr("Release Sources"),result.error,MessageKind::Page);
        else if(importing)showMessage(tr("Import Source"),tr("Source imported. Open its folder in Download Games to browse releases."),MessageKind::Page);
        if(!importing)refreshGameFeeds();
        emit changed();
    });
    const QString data=dataDirectory_;
    watcher->setFuture(QtConcurrent::run([data,importUrl,importing]{
        Preferences preferences(data);ReleaseSources store(data,preferences);Result result;
        if(importing)store.importSource(importUrl,&result.error);
        QString listError;result.sources=store.listSources(&listError);
        if(result.error.isEmpty())result.error=listError;
        for(const auto& source:result.sources)for(const auto& release:source.releases)
            result.paths.insert(releaseId(source.slug,release.title),store.downloadedPath(source.slug,release.title));
        return result;
    }));
    if(importing) {
        clearMessage();busy_=true;status_=tr("Importing source...");
        busyProgress_=-1.;busySpinnerPhase_=0.;busyClock_.start();emit changed();
    }
}
void Shell::refreshGameFeeds() {
    if(releaseFeedsRefreshing_ || releaseSources_.isEmpty() || diagnostics::safeMode())return;
    releaseFeedsRefreshing_=true;
    const auto generation=releaseSourcesGeneration_;
    auto* watcher=new QFutureWatcher<QVector<ReleaseSource>>(this);
    connect(watcher,&QFutureWatcher<QVector<ReleaseSource>>::finished,this,[this,watcher,generation]{
        auto sources=watcher->result();watcher->deleteLater();
        releaseFeedsRefreshing_=false;
        if(generation!=releaseSourcesGeneration_ || sources.isEmpty())return;
        releaseSources_=std::move(sources);
        rebuildReleaseSources();
        emit changed();
    });
    const QString data=dataDirectory_;
    watcher->setFuture(QtConcurrent::run([data]{
        Preferences prefs(data);
        ReleaseSources store(data,prefs);
        if(store.refreshSources()==0)return QVector<ReleaseSource>{};
        return store.listSources();
    }));
}

void Shell::rebuildReleaseSources() {
    for(auto& page:pages_)if(page.downloadHub) {
        const QString focus=page.entries.value(page.selection).id;
        page.entries=downloadHubRows(releaseSources_);
        page.selection=0;
        for(int i=0;i<page.entries.size();++i)
            if(page.entries[i].id==focus){page.selection=i;break;}
    }
    // The imported source folders no longer sit in the column: they are listed inside the Download Games
    // hub (openDownloadHub). This just keeps the hub folder's detail line honest about how many sources
    // are imported, so the column itself stays a fixed set of folders.
    for(auto& category:categories_)if(category.id==QLatin1String("game")) {
        for(auto& entry:category.entries)if(entry.action==Action::DownloadHub) {
            entry.detail=releaseSources_.isEmpty()?tr("Browse and import")
                :tr("%1 source%2").arg(releaseSources_.size()).arg(releaseSources_.size()==1?QString():QStringLiteral("s"));
        }
    }
}
void Shell::openReleaseSource(const QString& slug) {
    if(pages_.size()>=32)return;
    for(const auto& source:releaseSources_)if(source.slug==slug) {
        Page page;page.title=source.name;
        for(const auto& release:source.releases) {
            Entry entry=releaseRow(releaseId(slug,release.title),release.title,Action::ReleaseDownload,slug);
            entry.setting=release.title;entry.artwork=releaseArt_.value(entry.id);entry.iconSized=true;
            entry.rawgArtworkVisible=releaseRawgArt_.contains(entry.id);
            entry.artworkUrl=coverUrl(release);
            entry.detail=release.sizeLabel;
            const QString local=releaseLocalPaths_.value(entry.id);
            entry.fieldValue=local.isEmpty()?tr("Download"):tr("Downloaded");
            if(!local.isEmpty())entry.detail=QDir::toNativeSeparators(local);
            page.entries.append(entry);
        }
        pages_.push_back(std::move(page));
        if(source.releases.isEmpty())showMessage(tr("There are no content items."),MessageKind::Inline);
        for(int i=0;i<source.releases.size();++i)requestReleaseArt(pages_.last().entries[i],source.releases[i],source.url);
        emit changed();return;
    }
}
void Shell::requestReleaseArt(const Entry& entry,const IndexedRelease& release,const QUrl& sourceUrl) {
    if(!entry.artwork.isNull())return;
    if(!releaseArtwork_) {
        releaseArtwork_=std::make_unique<RawgArtwork>(QDir(dataDirectory_).filePath(QStringLiteral("cache/release-art")),this);
        connect(releaseArtwork_.get(),&RawgArtwork::ready,this,[this](const QString& id,const QString&,const QString&,const QImage& image){
            releaseArt_.insert(id,image);
            releaseRawgArt_.insert(id);
            for(auto& page:pages_)for(auto& row:page.entries)if(row.id==id){row.artwork=image;row.rawgArtworkVisible=true;}
            emit changed();
        });
    }
    const QString imageUrl=coverUrl(release);
    const auto fallback=[this]{
        QVector<QPair<QString,QString>> targets;
        for(const auto& page:pages_)for(const auto& row:page.entries)
            if(row.action==Action::ReleaseDownload && row.artwork.isNull() && row.artworkUrl.isEmpty() && !releaseArtRequested_.contains(row.id))
                targets.append(qMakePair(row.id,row.title));
        releaseArtwork_->refresh(targets);
    };
    if(imageUrl.isEmpty()){fallback();return;}
    if(releaseArtRequested_.contains(entry.id))return;
    releaseArtRequested_.insert(entry.id);
    const QString cache=QDir(dataDirectory_).filePath(QStringLiteral("cache/release-covers/")+
        QString::fromLatin1(QCryptographicHash::hash((entry.id+imageUrl).toUtf8(),QCryptographicHash::Sha256).toHex())+QStringLiteral(".png"));
    auto* watcher=new QFutureWatcher<QImage>(this);
    connect(watcher,&QFutureWatcher<QImage>::finished,this,[this,watcher,id=entry.id,fallback]{
        const QImage image=watcher->result();watcher->deleteLater();releaseArtRequested_.remove(id);
        if(image.isNull()) {
            for(auto& page:pages_)for(auto& row:page.entries)if(row.id==id)row.artworkUrl.clear();
            fallback();return;
        }
        releaseArt_.insert(id,image);
        for(auto& page:pages_)for(auto& row:page.entries)if(row.id==id)row.artwork=image;
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([url=sourceUrl.resolved(QUrl(imageUrl)),cache]{return fetchCover(url,cache);}));
}
void Shell::activateRelease(const Entry& entry) {
    for(const auto& source:releaseSources_) {
        if(source.slug!=entry.value)continue;
        for(const auto& release:source.releases)
            if(release.title==entry.setting) {
                openGameSources(importedGame(source,release));
                return;
            }
    }
    showMessage(tr("Release Sources"),tr("This release is no longer in the feed. Go back and open the source again."),MessageKind::Page);
}

void Shell::openDownloadManager() {
    if(pages_.size()>=32)return;
    Page page;page.title=tr("Download Manager");
    Entry entry=releaseRow(QStringLiteral("debrid:token"),tr("Real-Debrid API Key"),Action::DebridToken);
    entry.fieldValue=preferences_.value(QStringLiteral("debrid/realDebridToken")).toString().isEmpty()?tr("Not set"):QStringLiteral("********");
    entry.detail=tr("Enter a replacement API key, or clear the field to remove it. Stored for this profile; never shown in plain text.");
    page.entries.append(entry);
    Entry solver=releaseRow(QStringLiteral("flaresolverr:url"),tr("FlareSolverr Address"),Action::FlareSolverrUrl);
    solver.fieldValue=preferences_.value(QStringLiteral("debrid/flareSolverrUrl")).toString();
    if(solver.fieldValue.isEmpty())solver.fieldValue=tr("Not set");
    solver.detail=tr("Base URL of a local FlareSolverr instance, e.g. http://localhost:8191. Used to fetch sources behind Cloudflare bot protection.");
    page.entries.append(solver);
    pages_.push_back(std::move(page));emit changed();
}
void Shell::editDebridToken() {
    // An empty, masked editor never exposes the existing token to the view or its capture diagnostics.
    emit requestJellyfinFieldEdit(QStringLiteral("debrid:token"),tr("Real-Debrid API Key"),{},true);
}
void Shell::saveDebridToken(const QString& token) {
    if(!preferences_.setValue(QStringLiteral("debrid/realDebridToken"),token.trimmed()))return;
    for(auto& page:pages_)for(auto& row:page.entries)if(row.id==QStringLiteral("debrid:token"))
        row.fieldValue=token.trimmed().isEmpty()?tr("Not set"):QStringLiteral("********");
    setupRefresh();emit changed();
}
void Shell::editFlareSolverrUrl() {
    emit requestJellyfinFieldEdit(QStringLiteral("flaresolverr:url"),tr("FlareSolverr Address"),
                                  preferences_.value(QStringLiteral("debrid/flareSolverrUrl")).toString(),false);
}
void Shell::saveFlareSolverrUrl(const QString& url) {
    if(!preferences_.setValue(QStringLiteral("debrid/flareSolverrUrl"),url.trimmed()))return;
    const QString shown=url.trimmed().isEmpty()?tr("Not set"):url.trimmed();
    for(auto& page:pages_)for(auto& row:page.entries)if(row.id==QStringLiteral("flaresolverr:url"))row.fieldValue=shown;
    setupRefresh();emit changed();
}
void Shell::openGameSearch(const QString& scope) {
    if(!scope.isEmpty()) {
        emit requestJellyfinFieldEdit(QStringLiteral("game:search:")+scope,tr("Search Games"),{},false);
        return;
    }
    if(pages_.size()>=32)return;
    Page page;page.title=tr("Search Games");
    page.entries.append(releaseRow(QStringLiteral("game:scope:all"),tr("All sources"),Action::SearchGames,QStringLiteral("all")));
    page.entries.append(releaseRow(QStringLiteral("game:scope:imported"),tr("Imported feeds"),Action::SearchGames,QStringLiteral("imported")));
    page.entries.append(releaseRow(QStringLiteral("game:scope:online"),tr("Online Browse"),Action::SearchGames,QStringLiteral("online")));
    pages_.push_back(std::move(page));
    emit changed();
}
void Shell::openDownloadHub() {
    if(pages_.size()>=32)return;
    Page page;page.title=tr("Download Games");page.downloadHub=true;
    page.entries=downloadHubRows(releaseSources_);
    pages_.push_back(std::move(page));
    refreshGameFeeds();
    emit changed();
}

void Shell::openBrowseGames() {
    if(busy())return;
    busy_=true;status_=tr("Loading popular games...");
    busyProgress_=-1.;busySpinnerPhase_=0.;busyClock_.start();
    auto* watcher=new QFutureWatcher<BrowseOutcome>(this);
    connect(watcher,&QFutureWatcher<BrowseOutcome>::finished,this,[this,watcher]{
        const auto result=watcher->result();watcher->deleteLater();
        busy_=false;status_.clear();busyProgress_=-1.;busySpinnerPhase_=-1.;
        if(pages_.size()<32) {
            Page page;page.title=tr("Browse");
            page.downloadResults=result.games;
            page.entries.append(releaseRow(QStringLiteral("game:search-row"),tr("Search"),Action::SearchGames));
            if(!result.error.isEmpty()) {
                Entry error=releaseRow(QStringLiteral("game:error"),tr("Could not load games"),Action::Unavailable);
                error.detail=result.error;page.entries.append(error);
            } else if(page.downloadResults.isEmpty()) {
                page.entries.append(releaseRow(QStringLiteral("game:empty"),tr("No popular games are downloadable right now."),Action::Unavailable));
            } else {
                const int shown=qMin(100,page.downloadResults.size());
                for(int i=0;i<shown;++i) {
                    const auto& game=page.downloadResults[i];
                    Entry entry=releaseRow(QStringLiteral("game:result:")+QString::number(i),game.name,Action::GameSearchResult,QString::number(i));
                    entry.detail=game.options.size()>1?tr("%1 sources").arg(game.options.size()):game.options.first().source;
                    entry.fieldValue=game.options.first().fileSize;
                    entry.gameTile=true;
                    entry.artwork=result.covers.value(i);
                    entry.artworkUrl=game.coverUrl;
                    page.entries.append(entry);
                }
            }
            pages_.push_back(std::move(page));
        }
        emit changed();
    });
    const QString cache=QDir(dataDirectory_).filePath(QStringLiteral("cache/release-art"));
    watcher->setFuture(QtConcurrent::run([cache]{
        BrowseOutcome result;
        HydraLibrary().popularGames(&result.games,&result.error);
        result.covers=fetchCovers(result.games,cache);
        return result;
    }));
    emit changed();
}
void Shell::runGameSearch(const QString& query,const QString& scope) {
    const QString term=query.trimmed();
    if(term.isEmpty() || busy() || pages_.size()>=32)return;
    const bool online=scope==QLatin1String("all") || scope==QLatin1String("online");
    const bool imported=scope==QLatin1String("all") || scope==QLatin1String("imported");
    if(!online && !imported)return;
    busy_=true;status_=tr("Searching games...");
    busyProgress_=-1.;busySpinnerPhase_=0.;busyClock_.start();
    auto* watcher=new QFutureWatcher<BrowseOutcome>(this);
    const auto depth=pages_.size();
    connect(watcher,&QFutureWatcher<BrowseOutcome>::finished,this,[this,watcher,term,depth]{
        auto result=watcher->result();watcher->deleteLater();
        busy_=false;status_.clear();busyProgress_=-1.;busySpinnerPhase_=-1.;
        if(pages_.size()==depth && depth<32) {
            Page page;page.title=tr("Search: %1").arg(term);
            page.downloadResults=std::move(result.games);
            const int shown=qMin(100,page.downloadResults.size());
            for(int i=0;i<shown;++i) {
                const auto& game=page.downloadResults[i];
                const QString index=QString::number(i);
                Entry entry=releaseRow(QStringLiteral("game:result:")+index,game.name,Action::GameSearchResult,index);
                entry.detail=optionCount(game);
                entry.gameTile=true;
                entry.artwork=result.covers.value(i);
                entry.artworkUrl=game.coverUrl;
                page.entries.append(std::move(entry));
            }
            if(shown==0)page.entries.append(releaseRow(QStringLiteral("game:empty"),tr("No games found for \"%1\"").arg(term),Action::Unavailable));
            if(!result.error.isEmpty()) {
                auto error=releaseRow(QStringLiteral("game:error"),tr("Online Browse unavailable"),Action::Unavailable);
                error.detail=result.error;
                page.entries.append(std::move(error));
            }
            pages_.push_back(std::move(page));
        }
        emit changed();
    });
    const auto sources=imported?releaseSources_:QVector<ReleaseSource>{};
    const QString cache=QDir(dataDirectory_).filePath(QStringLiteral("cache/release-art"));
    watcher->setFuture(QtConcurrent::run([term,online,sources,cache]{
        BrowseOutcome result;
        QHash<QString,int> indexes;
        const auto add=[&](GameResult game) {
            if(game.options.isEmpty())return;
            const QString key=gameSearchKey(game.name);
            if(key.isEmpty())return;
            const auto found=indexes.constFind(key);
            if(found!=indexes.cend()) {
                auto& existing=result.games[*found];
                if(existing.coverUrl.isEmpty())existing.coverUrl=game.coverUrl;
                for(auto& option:game.options) {
                    const auto duplicate=std::any_of(existing.options.cbegin(),existing.options.cend(),[&](const GameSourceOption& old){
                        const QString oldUri=old.downloads.isEmpty()?QString{}:old.downloads.first().uri;
                        const QString uri=option.downloads.isEmpty()?QString{}:option.downloads.first().uri;
                        return old.sourceUrl==option.sourceUrl && old.releaseTitle==option.releaseTitle && oldUri==uri;
                    });
                    if(!duplicate && existing.options.size()<48)existing.options.append(std::move(option));
                }
            } else if(result.games.size()<100) {
                indexes.insert(key,int(result.games.size()));
                result.games.append(std::move(game));
            }
        };
        const QString normalizedTerm=gameSearchKey(term);
        for(const auto& source:sources)
            for(const auto& release:source.releases) {
                auto game=importedGame(source,release);
                if(gameSearchKey(game.name).contains(normalizedTerm) || gameSearchKey(release.title).contains(normalizedTerm))add(std::move(game));
            }
        if(online) {
            QVector<GameResult> results;
            if(HydraLibrary().searchGames(term,&results,&result.error))
                for(auto& game:results)add(std::move(game));
        }
        result.covers=fetchCovers(result.games,cache);
        return result;
    }));
    emit changed();
}

void Shell::pickGameResult(int index) {
    if(pages_.isEmpty() || index<0 || index>=pages_.last().downloadResults.size())return;
    const GameResult game=pages_.last().downloadResults[index];
    openGameSources(game);
}

void Shell::openGameSources(const GameResult& game) {
    if(pages_.size()>=32)return;
    Page page;page.title=game.name;
    page.downloadResults={game};
    QHash<QString,int> mirrorCounts, mirrorIndex;
    for(const auto& option:game.options)
        if(!option.mirror.isEmpty())++mirrorCounts[option.source+QLatin1Char('/')+option.mirror];
    for(int i=0;i<game.options.size();++i) {
        auto& option=page.downloadResults.first().options[i];
        const QString index=QString::number(i);
        Entry entry=releaseRow(QStringLiteral("game:source:")+index,option.source,Action::GameSourcePick,index);
        if(!option.mirror.isEmpty()) {
            const QString key=option.source+QLatin1Char('/')+option.mirror;
            if(mirrorCounts.value(key)>1)option.mirror+=tr(" · Mirror %1").arg(++mirrorIndex[key]);
            entry.title+=QStringLiteral(" / ")+option.mirror;
        }
        entry.detail=option.releaseTitle.isEmpty()?game.name:option.releaseTitle;
        entry.fieldValue=option.fileSize.isEmpty()?tr("Size unknown"):option.fileSize;
        page.entries.append(std::move(entry));
    }
    if(page.entries.isEmpty())
        page.entries.append(releaseRow(QStringLiteral("game:no-links"),tr("No supported single-file download"),Action::Unavailable));
    pages_.push_back(std::move(page));
    requestGamePageArt();
    emit changed();
}

void Shell::requestGamePageArt() {
    if(pages_.isEmpty() || pages_.last().downloadResults.isEmpty())return;
    const QString url=pages_.last().downloadResults.first().coverUrl;
    if(url.isEmpty())return;
    const int depth=pages_.size();
    const QString cache=QDir(dataDirectory_).filePath(QStringLiteral("cache/release-art"));
    auto* watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,depth]{
        const QString path=watcher->result();watcher->deleteLater();
        // Only the page this fetch was started for: a later back/pick must not repaint an older page.
        if(path.isEmpty() || pages_.size()!=depth)return;
        for(auto& row:pages_.last().entries)
            if(row.action==Action::GameSourcePick)row.backgroundPath=path;
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([url,cache]{return cachedCoverPath(url,cache);}));
}

void Shell::pickGameSource(const QString& value) {
    if(pages_.isEmpty() || pages_.last().downloadResults.isEmpty())return;
    const auto game=pages_.last().downloadResults.first();
    bool valid=false;
    const int option=value.toInt(&valid);
    if(!valid || option<0 || option>=game.options.size())return;
    beginGameDownload(game.name,game.options[option]);
}

// The destinations the drive picker offers. XMB_GAME_LIBRARY_ROOT (local verification only,
// like XMB_STEAM_ROOT) replaces the real drive list with one scratch root, so a replay can install
// into a temporary folder without touching any real library. Its value is the folder whose child
// "XMB Games" receives the install.
QVector<QPair<QString,QString>> gameLibraryRoots() {
    const QString override=qEnvironmentVariable("XMB_GAME_LIBRARY_ROOT");
    if(!override.isEmpty()) {
        const QString root=QDir::cleanPath(override);
        const QFileInfo info(root);
        if(info.isDir() && QDir::isAbsolutePath(root))
            return {qMakePair(QDir::toNativeSeparators(root),root)};
    }
    QVector<QPair<QString,QString>> roots;
    for(const auto& drive:QDir::drives())roots.append(qMakePair(QDir::toNativeSeparators(drive.absolutePath()),drive.absolutePath()));
    return roots;
}

void Shell::beginGameDownload(const QString& title,const GameSourceOption& option) {
    if(pages_.size()>=32)return;
    Page page;page.title=tr("Choose a drive");
    GameResult game;
    game.name=title;
    game.options={option};
    page.downloadResults={std::move(game)};
    for(const auto& root:gameLibraryRoots()) {
        Entry entry=releaseRow(QStringLiteral("drive:")+root.second,QDir::toNativeSeparators(root.second),Action::GameDownloadDrive,root.second);
        entry.detail=tr("Install in %1").arg(QDir::toNativeSeparators(QDir(root.second).filePath(QStringLiteral("XMB Games"))));
        page.entries.append(std::move(entry));
    }
    if(page.entries.isEmpty())
        page.entries.append(releaseRow(QStringLiteral("drive:none"),tr("No drives found"),Action::Unavailable));
    // Preselect the last-used install drive when it is still offered. The persisted value is only a
    // focus default (the console remembers the last destination); enqueue re-validates the volume, so a
    // stale or removed drive simply falls back to the first row.
    const QString last=preferences_.value(QStringLiteral("downloads/lastGameDrive")).toString();
    for(int i=0;i<page.entries.size();++i)
        if(page.entries[i].action==Action::GameDownloadDrive && page.entries[i].value==last){page.selection=i;break;}
    pages_.push_back(std::move(page));
    emit changed();
}

void Shell::completeGameDownload(const QString& drive) {
    if(pages_.isEmpty() || pages_.last().downloadResults.isEmpty())return;
    const auto game=pages_.last().downloadResults.first();
    if(game.options.size()!=1)return;
    const auto option=game.options.first();
    const QString directory=QDir(drive).filePath(QStringLiteral("XMB Games"));
    QString source=option.source;
    if(!option.mirror.isEmpty())source+=QStringLiteral(" / ")+option.mirror;
    const QString size=option.fileSize.isEmpty()?tr("Unknown"):option.fileSize;
    const QString question=tr("Download and install %1?").arg(game.name);
    const QString release=option.releaseTitle.isEmpty()?game.name:option.releaseTitle;
    const QString body=question+QStringLiteral("\n\n")+tr("Release: %1\nSource: %2\nSize: %3\nDestination: %4")
        .arg(release,source,size,QDir::toNativeSeparators(directory));
    showConfirm(question,body,[this,game,option,directory,drive]{
        pages_.removeLast();
        // Remember this drive only when the enqueue actually succeeded: a refused download (duplicate,
        // full queue, invalid volume) must not become the preselected "last used" location.
        if(enqueueReleaseGame(game.name,option.releaseTitle.isEmpty()?game.name:option.releaseTitle,
                              QUrl(option.sourceUrl),option.sourceSlug,directory,option.downloads))
            preferences_.setValue(QStringLiteral("downloads/lastGameDrive"),drive);
        emit changed();
    });
}

void Shell::fetchGameArtwork(const QString& id,const QString& title) {
    if(!releaseArtwork_) {
        releaseArtwork_=std::make_unique<RawgArtwork>(QDir(dataDirectory_).filePath(QStringLiteral("cache/release-art")),this);
        connect(releaseArtwork_.get(),&RawgArtwork::ready,this,[this](const QString& rid,const QString&,const QString&,const QImage& image){
            releaseArt_.insert(rid,image);
            for(auto& page:pages_)for(auto& row:page.entries)if(row.id==rid){row.artwork=image;row.rawgArtworkVisible=true;}
            emit changed();
        });
    }
    if(!gameArtHooked_) {
        gameArtHooked_=true;
        connect(releaseArtwork_.get(),&RawgArtwork::ready,this,[this](const QString& rid,const QString&,const QString& path,const QImage&){
            const QString registryPath=QDir(dataDirectory_).filePath(QStringLiteral("apps.json"));
            QVector<ContentApp> apps; QString error;
            if(!ContentRegistry::load(registryPath,apps,error))return;
            for(const auto& app:apps)if(app.id==rid && app.artwork!=path) {
                ContentApp updated=app; updated.artwork=path;
                if(ContentRegistry::upsert(registryPath,updated,error))refreshContent();
                break;
            }
        });
    }
    releaseArtwork_->refresh(QVector<QPair<QString,QString>>{{id,title}});
}
} // namespace xmb
