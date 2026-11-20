#include "hydra_library.h"
#include <QEventLoop>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace xmb {
namespace {
QString responseError(const QString& service, QNetworkReply* reply, bool timedOut) {
    // Retry is explicit: callers surface this to the Store's Retry action rather than hammering a
    // rate-limited endpoint. We intentionally do not sleep or replay requests from this blocking client.
    if(timedOut)return QStringLiteral("Hydra %1 request timed out; retry").arg(service);
    const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if(status>=400)return QStringLiteral("Hydra %1 request failed (HTTP %2); retry").arg(service).arg(status);
    return QStringLiteral("Hydra %1 request failed; retry").arg(service);
}
bool fetchBody(const QUrl& url,const QString& service,QByteArray* out,QString* error);

bool fetchJson(const QUrl& url, QJsonObject* out, QString* error) {
    QByteArray body; if(!fetchBody(url,QStringLiteral("library"),&body,error))return false;
    QJsonParseError parseError;
    const QJsonDocument document=QJsonDocument::fromJson(body,&parseError);
    if(document.isNull() || !document.isObject()) {
        if(error)*error=QStringLiteral("Hydra library returned invalid JSON; retry");
        return false;
    }
    if(out)*out=document.object(); return true;
}
// The Hydra Cloud catalogue endpoints (catalogue/hot, catalogue/featured) answer a bare JSON array.
bool fetchJsonArray(const QUrl& url, QJsonArray* out, QString* error) {
    QByteArray body; if(!fetchBody(url,QStringLiteral("catalogue"),&body,error))return false;
    QJsonParseError parseError;
    const QJsonDocument document=QJsonDocument::fromJson(body,&parseError);
    if(document.isNull() || !document.isArray()) { if(error)*error=QStringLiteral("Hydra catalogue returned invalid JSON; retry"); return false; }
    if(out)*out=document.array(); return true;
}
bool safeRedirect(const QUrl& from, const QUrl& to) {
    const bool valid=(to.scheme()==QLatin1String("http") || to.scheme()==QLatin1String("https")) && !to.host().isEmpty();
    if(!valid)return false;
    return !(from.scheme()==QLatin1String("https") && to.scheme()==QLatin1String("http"));
}

bool fetchBody(const QUrl& url,const QString& service,QByteArray* out,QString* error) {
    if((url.scheme()!=QLatin1String("http") && url.scheme()!=QLatin1String("https")) || url.host().isEmpty()) {
        if(error)*error=QStringLiteral("Hydra %1 URL must be HTTP(S); retry").arg(service); return false;
    }
    constexpr qint64 kMaxBody=16*1024*1024;
    QNetworkAccessManager manager; QNetworkRequest request(url);
    request.setRawHeader("User-Agent","XMB-Release-Sources/0.2");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply=manager.get(request); reply->setReadBufferSize(kMaxBody+1);
    QEventLoop loop; QTimer timer; timer.setSingleShot(true); QByteArray body; bool timedOut=false,invalidRedirect=false,overflow=false;
    const auto abort=[&]{reply->abort();};
    QObject::connect(&timer,&QTimer::timeout,&loop,[&]{timedOut=true;abort();});
    QObject::connect(reply,&QNetworkReply::redirected,&loop,[&](const QUrl& target){
        if(!safeRedirect(reply->url(),target)){invalidRedirect=true;abort();}
    });
    QObject::connect(reply,&QNetworkReply::readyRead,&loop,[&]{
        if(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong()>kMaxBody){overflow=true;abort();return;}
        const qint64 remaining=kMaxBody-body.size();
        const QByteArray chunk=reply->read(remaining+1);
        if(chunk.size()>remaining){overflow=true;abort();return;}
        body.append(chunk);
    });
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    timer.start(30000); loop.exec();
    if(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong()>kMaxBody)overflow=true;
    if(timedOut || invalidRedirect || overflow || reply->error()!=QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()>=400) {
        const QString message=overflow?QStringLiteral("Hydra %1 response exceeds 16 MiB; retry").arg(service)
            :(invalidRedirect?QStringLiteral("Hydra %1 redirect is not safe HTTP(S); retry").arg(service):responseError(service,reply,timedOut));
        reply->deleteLater(); if(error)*error=message; return false;
    }
    if(out)*out=body; reply->deleteLater(); return true;
}
bool fetchDocument(const QUrl& url, QJsonDocument* out, QString* error) {
    QByteArray body; if(!fetchBody(url,QStringLiteral("source"),&body,error))return false;
    QJsonParseError parseError; const auto document=QJsonDocument::fromJson(body,&parseError);
    if(document.isNull()) { if(error)*error=QStringLiteral("Hydra source returned invalid JSON; retry"); return false; }
    if(out)*out=document; return true;
}
// The catalogue's dedup key. It must agree with the shell's gameSearchKey() so a game merged here
// matches the imported/online merge in runGameSearch(): Unicode-normalise, collapse whitespace, then
// case-fold. Editions that differ in this key stay apart; identical names merge across sources.
QString catalogNameKey(const QString& name) {
    return name.normalized(QString::NormalizationForm_KC).simplified().toCaseFolded();
}
QString optionIdentity(const GameSourceOption& option) {
    QString key=option.source+QLatin1Char('\x1f')+option.sourceUrl+QLatin1Char('\x1f')+option.releaseTitle+QLatin1Char('\x1f')+option.fileSize;
    for(const auto& download:option.downloads)key+=QLatin1Char('\x1f')+download.kind+QLatin1Char(':')+download.uri+QLatin1Char(':')+download.infohash;
    return key;
}
bool isPreinstalled(const HydraSource& source) {
    const QString desc=source.description.toLower();
    const QString title=source.title.toLower();
    bool direct=false,torrent=false;
    for(const auto& option:source.topDownloadOption) {
        const QString name=option.toLower();
        if(name==QStringLiteral("torrent"))torrent=true;
        else if(!name.isEmpty())direct=true;
    }
    return desc.contains(QStringLiteral("pre-installed")) || desc.contains(QStringLiteral("preinstalled")) ||
           title.contains(QStringLiteral("portable")) || (direct && !torrent);
}

QString loopbackEndpoint(const char* variable,const QString& fallback) {
    if(qEnvironmentVariableIntValue("XMB_HYDRA_TEST_FIXTURE")!=1)return fallback;
    const QString configured=qEnvironmentVariable(variable);
    if(configured.isEmpty())return fallback;
    const QUrl url(configured);
    const QString host=url.host().toLower();
    if((url.scheme()==QLatin1String("http") || url.scheme()==QLatin1String("https")) &&
       (host==QLatin1String("127.0.0.1") || host==QLatin1String("localhost") || host==QLatin1String("::1")))
        return configured;
    return fallback;
}

bool validSourceArray(const QJsonArray& values) {
    for(const auto& value:values) {
        if(!value.isObject())return false;
        const auto object=value.toObject();
        const QString title=object.value(QStringLiteral("title")).toString().trimmed();
        const QUrl url(object.value(QStringLiteral("url")).toString().trimmed());
        if(title.isEmpty() || !url.isValid() || (url.scheme()!=QLatin1String("http") && url.scheme()!=QLatin1String("https")) || url.host().isEmpty())return false;
    }
    return true;
}

bool validGameArray(const QJsonArray& values, bool requireName) {
    for(const auto& value:values) {
        if(!value.isObject())return false;
        const auto object=value.toObject();
        if(requireName && object.value(QStringLiteral("name")).toString().trimmed().isEmpty())return false;
        if(object.value(QStringLiteral("source")).toString().trimmed().isEmpty())return false;
        const QUrl sourceUrl(object.value(QStringLiteral("sourceUrl")).toString().trimmed());
        if(!sourceUrl.isValid() || sourceUrl.scheme().isEmpty() || sourceUrl.host().isEmpty())return false;
    }
    return true;
}

bool validPopularTitleArray(const QJsonArray& values) {
    for(const auto& value:values) {
        if(!value.isObject() || value.toObject().value(QStringLiteral("title")).toString().trimmed().isEmpty())return false;
    }
    return true;
}

struct PopularLookup {
    bool ok=false;
    GameResult game;
    QString error;
};
}

SourcesPage parseSourcesPage(const QJsonObject& root) {
    SourcesPage page;
    page.total=root.value(QStringLiteral("total")).toInt();
    page.limit=root.value(QStringLiteral("limit")).toInt();
    for(const auto& value:root.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject object=value.toObject();
        HydraSource source;
        source.title=object.value(QStringLiteral("title")).toString();
        source.url=QUrl(object.value(QStringLiteral("url")).toString());
        source.gamesCount=object.value(QStringLiteral("gamesCount")).toInt();
        source.description=object.value(QStringLiteral("description")).toString();
        source.installs=object.value(QStringLiteral("stats")).toObject().value(QStringLiteral("installs")).toInt();
        for(const auto& status:object.value(QStringLiteral("status")).toArray())source.status.append(status.toString());
        for(const auto& option:object.value(QStringLiteral("topDownloadOption")).toArray())
            source.topDownloadOption.append(option.toObject().value(QStringLiteral("name")).toString());
        page.sources.append(source);
    }
    return page;
}

bool hasMoreSources(int pageNumber, int accumulated, const SourcesPage& page) {
    return page.limit>0 && accumulated<page.total && !page.sources.isEmpty() && pageNumber<100;
}

QVector<GameResult> buildGameCatalog(const QVector<GameRecord>& records, const QVector<HydraSource>& sources) {
    QHash<QString,int> installs;
    for(const auto& source:sources)installs.insert(source.title,source.installs);
    QVector<GameResult> results;
    QHash<QString,int> byName;
    for(const auto& record:records) {
        const QString name=record.name.trimmed();
        const QString key=catalogNameKey(name);
        if(key.isEmpty())continue;
        GameSourceOption option;
        option.source=record.source;
        option.sourceUrl=record.sourceUrl;
        option.fileSize=record.fileSize;
        option.sourceStatus=record.sourceStatus;
        option.releaseTitle=record.originalTitle;
        if(option.releaseTitle.isEmpty())option.releaseTitle=record.title;
        const auto existing=byName.find(key);
        if(existing==byName.end()) {
            GameResult result;
            result.name=name;
            result.title=record.title;
            result.originalTitle=record.originalTitle;
            result.options.append(option);
            byName.insert(key,results.size());
            results.append(result);
        } else {
            auto& dst=results[existing.value()];
            bool have=false;
            for(const auto& old:dst.options) {
                if(optionIdentity(old)==optionIdentity(option)){have=true;break;}
            }
            if(!have)dst.options.append(option);
        }
    }
    std::stable_sort(results.begin(),results.end(),[&installs](const GameResult& a,const GameResult& b){
        int pa=0,pb=0;
        for(const auto& o:a.options)pa=std::max(pa,installs.value(o.source,0));
        for(const auto& o:b.options)pb=std::max(pb,installs.value(o.source,0));
        return pa>pb;
    });
    return results;
}
bool HydraLibrary::listSources(QVector<HydraSource>* out, QString* error) const {
    if(error)error->clear();
    if(out)out->clear();
    QVector<HydraSource> all;
    int pageNumber=1;
    const QString endpoint=loopbackEndpoint("XMB_HYDRA_SOURCES_URL",
        QStringLiteral("https://api.hydralibrary.com/sources"));
    for(;;) {
        QJsonObject root;
        if(!fetchJson(QUrl(endpoint+QStringLiteral("?page=%1&limit=100").arg(pageNumber)),&root,error))return false;
        if(!root.contains(QStringLiteral("sources")) || !root.value(QStringLiteral("sources")).isArray()) {
            if(error)*error=QStringLiteral("Hydra library returned an invalid sources response; retry");
            return false;
        }
        if(!validSourceArray(root.value(QStringLiteral("sources")).toArray())) {
            if(error)*error=QStringLiteral("Hydra library returned an invalid source entry; retry");
            return false;
        }
        const SourcesPage page=parseSourcesPage(root);
        all+=page.sources;
        if(!hasMoreSources(pageNumber,all.size(),page)) {
            if(page.limit>0 && all.size()<page.total && pageNumber>=100) {
                if(error)*error=QStringLiteral("Hydra sources response is truncated after 100 pages; retry");
                if(out)out->clear(); return false;
            }
            break;
        }
        ++pageNumber;
    }
    if(out)*out=all; return true;
}
bool HydraLibrary::preinstalledSources(QStringList* out, QString* error) const {
    if(error)error->clear();
    if(out)out->clear();
    QVector<HydraSource> sources;
    if(!listSources(&sources,error))return false;
    QStringList names;
    for(const auto& source:sources)if(isPreinstalled(source))names.append(source.title);
    if(out)*out=names; return true;
}
bool HydraLibrary::searchGames(const QString& query, QVector<GameResult>* out, QString* error) const {
    if(error)error->clear();
    if(out)out->clear();
    QVector<HydraSource> sources;
    if(!listSources(&sources,error))return false;
    QVector<GameRecord> records;
    const QString endpoint=loopbackEndpoint("XMB_HYDRA_SEARCH_URL",
        QStringLiteral("https://api.hydralibrary.com/games"));
    QSet<QString> names;   // distinct game names, so pagination stops at the API's filteredCount
    for(int page=1;page<=5;++page) {
        QJsonObject root;
        const QUrl url(endpoint+QStringLiteral("?q=%1&limit=100&page=%2")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(query))).arg(page));
        if(!fetchJson(url,&root,error))return false;
        if(!root.contains(QStringLiteral("games")) || !root.value(QStringLiteral("games")).isArray()) {
            if(error)*error=QStringLiteral("Hydra library returned an invalid games response; retry");
            return false;
        }
        const QJsonArray games=root.value(QStringLiteral("games")).toArray();
        if(!validGameArray(games,true)) {
            if(error)*error=QStringLiteral("Hydra library returned an invalid game entry; retry");
            return false;
        }
        if(games.isEmpty())break;
        for(const auto& value:games) {
            const QJsonObject game=value.toObject();
            GameRecord record;
            record.name=game.value(QStringLiteral("name")).toString();
            record.title=game.value(QStringLiteral("title")).toString();
            record.originalTitle=game.value(QStringLiteral("originalTitle")).toString();
            record.source=game.value(QStringLiteral("source")).toString();
            record.sourceUrl=game.value(QStringLiteral("sourceUrl")).toString();
            record.fileSize=game.value(QStringLiteral("fileSize")).toString();
            record.sourceStatus=game.value(QStringLiteral("sourceStatus")).toString();
            const QString key=catalogNameKey(record.name);
            if(key.isEmpty())continue;
            names.insert(key);
            records.append(record);
        }
        const int filtered=root.value(QStringLiteral("filteredCount")).toInt();
        if(names.size()>=filtered || names.size()>=500)break;
    }
    const QVector<GameResult> results=buildGameCatalog(records,sources);
    if(out)*out=results; return true;
}
bool HydraLibrary::browseSource(const HydraSource& source, QVector<GameResult>* out, QString* error) const {
    if(error)error->clear(); if(out)out->clear();
    if(!source.url.isValid()) { if(error)*error=QStringLiteral("Hydra source has an invalid URL; retry"); return false; }
    QJsonDocument document;
    if(!fetchDocument(source.url,&document,error))return false;
    QString parseError; const auto releases=parseReleaseIndex(document,&parseError);
    if(!parseError.isEmpty()) { if(error)*error=QStringLiteral("Hydra source returned invalid release JSON; retry"); return false; }
    QVector<GameResult> games; QHash<QString,int> gameIndexes;
    for(const auto& release:releases) {
        QString name=release.raw.value(QStringLiteral("gameTitle")).toString().trimmed();
        if(name.isEmpty())name=release.raw.value(QStringLiteral("name")).toString().trimmed();
        if(name.isEmpty())name=release.title.trimmed();
        if(name.isEmpty())continue;
        GameSourceOption option; option.source=source.title; option.sourceUrl=source.url.toString(); option.fileSize=release.sizeLabel; option.releaseTitle=release.title;
        for(const auto& download:release.downloads)
            if(download.kind==QLatin1String("direct") || download.kind==QLatin1String("magnet"))option.downloads.append(download);
        for(const auto& mirror:release.mirrors)if(mirror.parts.size()==1 &&
            (mirror.parts.first().kind==QLatin1String("direct") || mirror.parts.first().kind==QLatin1String("magnet")))option.downloads.append(mirror.parts.first());
        option.mirror=release.mirrors.isEmpty()?QString{}:release.mirrors.first().host;
        option.sourceStatus=option.downloads.isEmpty()?QStringLiteral("Only this source is known; no supported direct or magnet download is available"):QStringLiteral("Only this source is known; other sources unavailable");
        const QString key=catalogNameKey(name); int index=gameIndexes.value(key,-1);
        if(index<0) { GameResult game; game.name=name; game.title=name; game.originalTitle=release.title; game.options.append(option); gameIndexes.insert(key,games.size()); games.append(std::move(game)); }
        else {
            bool duplicate=false; for(const auto& old:games[index].options)if(optionIdentity(old)==optionIdentity(option)){duplicate=true;break;}
            if(!duplicate)games[index].options.append(option);
        }
    }
    if(out)*out=std::move(games); return true;
}
bool HydraLibrary::popularGames(QVector<GameResult>* out, QString* error) const {
    if(error)error->clear();
    if(out)out->clear();

    QVector<PopularTitle> titles;
    if(!popularTitles(&titles,error))return false;

    // Each /games lookup is independent. Keep the pool deliberately small: this is a
    // catalogue refresh, not an invitation to open 40 sockets at once. Futures are
    // consumed in hot-catalogue order below, so completion timing can never reorder
    // the Store tiles.
    QThreadPool pool;
    pool.setMaxThreadCount(6);
    QVector<QFuture<PopularLookup>> lookups;
    lookups.reserve(titles.size());
    for(const auto& title:titles) {
        lookups.append(QtConcurrent::run(&pool,[title] {
            PopularLookup lookup;
            lookup.ok=HydraLibrary().popularGame(title,&lookup.game,&lookup.error);
            return lookup;
        }));
    }

    QVector<GameResult> results;
    QString firstFailure;
    const auto groups=groupPopularTitles(titles);
    int rawIndex=0;
    for(const auto& group:groups) {
        QVector<GameResult> parts;
        bool failed=false;
        parts.reserve(group.entries.size());
        for(int i=0;i<group.entries.size();++i,++rawIndex) {
            const auto& lookup=lookups.at(rawIndex).result();
            if(!lookup.ok) { failed=true;if(firstFailure.isEmpty())firstFailure=lookup.error;break; }
            parts.append(lookup.game);
        }
        // A group is all-or-nothing. Never publish a partial source union when one
        // duplicate hot entry failed; the Store's per-title retry handles recovery.
        if(!failed) {
            auto merged=mergePopularGameResults(group.name,group.coverUrl,parts);
            if(!merged.options.isEmpty())results.append(std::move(merged));
        }
        if(failed)rawIndex+=group.entries.size()-parts.size();
    }
    if(!firstFailure.isEmpty()) {
        if(out)out->clear();
        if(error)*error=firstFailure;
        return false;
    }
    if(out)*out=results;
    return true;
}

bool HydraLibrary::popularTitles(QVector<PopularTitle>* out,QString* error) const {
    if(error)error->clear();
    if(out)out->clear();
    QJsonArray hot;
    const QString url=loopbackEndpoint("XMB_HYDRA_HOT_URL",
        QStringLiteral("https://api.hydralauncher.gg/catalogue/hot?take=40&skip=0"));
    if(!fetchJsonArray(QUrl(url),&hot,error))return false;
    if(!validPopularTitleArray(hot)) {
        if(error)*error=QStringLiteral("Hydra catalogue returned an invalid popular title entry; retry");
        return false;
    }
    for(const auto& value:hot) {
        const auto object=value.toObject();
        PopularTitle title;
        title.title=object.value(QStringLiteral("title")).toString().trimmed();
        title.coverUrl=object.value(QStringLiteral("libraryImageUrl")).toString();
        if(!title.title.isEmpty() && out)out->append(title);
    }
    return true;
}

QVector<PopularTitleGroup> groupPopularTitles(const QVector<PopularTitle>& titles) {
    QVector<PopularTitleGroup> groups;
    QHash<QString,int> indexes;
    for(const auto& title:titles) {
        const QString key=catalogNameKey(title.title);
        if(key.isEmpty())continue;
        const auto found=indexes.constFind(key);
        if(found==indexes.cend()) {
            indexes.insert(key,groups.size());
            PopularTitleGroup group;group.name=title.title;group.coverUrl=title.coverUrl;group.entries.append(title);
            groups.append(std::move(group));
        } else groups[found.value()].entries.append(title);
    }
    return groups;
}

GameResult mergePopularGameResults(const QString& name,const QString& coverUrl,
                                   const QVector<GameResult>& results) {
    GameResult merged;merged.name=name;merged.title=name;merged.coverUrl=coverUrl;
    QSet<QString> seen;
    for(const auto& result:results) {
        if(merged.originalTitle.isEmpty())merged.originalTitle=result.originalTitle;
        for(const auto& option:result.options) {
            const QString identity=optionIdentity(option);
            if(seen.contains(identity))continue;
            seen.insert(identity);merged.options.append(option);
        }
    }
    return merged;
}

bool HydraLibrary::popularGame(const PopularTitle& title,GameResult* out,QString* error) const {
    if(error)error->clear();
    if(out)*out=GameResult{};
    const QString endpoint=loopbackEndpoint("XMB_HYDRA_GAMES_URL",
        QStringLiteral("https://api.hydralibrary.com/games"));
    const QString searchUrl=endpoint+QStringLiteral("?q=%1&limit=100&page=1")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(title.title)));
    QJsonObject root;
    if(!fetchJson(QUrl(searchUrl),&root,error))return false;
    if(!root.contains(QStringLiteral("games")) || !root.value(QStringLiteral("games")).isArray()) {
        if(error)*error=QStringLiteral("Hydra library returned an invalid games response; retry");
        return false;
    }
    if(!validGameArray(root.value(QStringLiteral("games")).toArray(),false)) {
        if(error)*error=QStringLiteral("Hydra library returned an invalid game entry; retry");
        return false;
    }
    GameResult result;result.name=title.title;result.title=title.title;result.coverUrl=title.coverUrl;
    QSet<QString> seenSource;
    for(const auto& gv:root.value(QStringLiteral("games")).toArray()) {
        const auto game=gv.toObject();
        GameSourceOption option;
        option.source=game.value(QStringLiteral("source")).toString();
        option.sourceUrl=game.value(QStringLiteral("sourceUrl")).toString();
        const QString identity=optionIdentity(option);
        if(seenSource.contains(identity))continue;
        seenSource.insert(identity);
        option.fileSize=game.value(QStringLiteral("fileSize")).toString();
        option.sourceStatus=game.value(QStringLiteral("sourceStatus")).toString();
        option.releaseTitle=game.value(QStringLiteral("originalTitle")).toString();
        if(option.releaseTitle.isEmpty())option.releaseTitle=game.value(QStringLiteral("title")).toString();
        if(result.originalTitle.isEmpty())result.originalTitle=game.value(QStringLiteral("originalTitle")).toString();
        result.options.append(option);
    }
    if(out)*out=std::move(result);
    return true;
}
} // namespace xmb
