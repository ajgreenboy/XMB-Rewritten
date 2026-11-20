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
    if(timedOut)return QStringLiteral("Hydra %1 request timed out; retry").arg(service);
    const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if(status>=400)return QStringLiteral("Hydra %1 request failed (HTTP %2); retry").arg(service).arg(status);
    return QStringLiteral("Hydra %1 request failed; retry").arg(service);
}

bool fetchJson(const QUrl& url, QJsonObject* out, QString* error) {
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent","XMB-Release-Sources/0.2");
    QNetworkReply* reply=manager.get(request);
    QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    bool timedOut=false;
    QObject::connect(&timer,&QTimer::timeout,&loop,[&]{timedOut=true;reply->abort();});
    QObject::connect(reply,&QNetworkReply::downloadProgress,&loop,[&](qint64 received,qint64){if(received>16*1024*1024){timedOut=true;reply->abort();}}); // a source cannot stream an unbounded body into memory
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    timer.start(30000); loop.exec();
    if(timedOut || reply->error()!=QNetworkReply::NoError ||
       reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()>=400) {
        const QString message=responseError(QStringLiteral("library"),reply,timedOut); reply->deleteLater();
        if(error)*error=message; return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document=QJsonDocument::fromJson(reply->readAll(),&parseError);
    reply->deleteLater();
    if(document.isNull() || !document.isObject()) {
        if(error)*error=QStringLiteral("Hydra library returned invalid JSON; retry");
        return false;
    }
    if(out)*out=document.object(); return true;
}
// The Hydra Cloud catalogue endpoints (catalogue/hot, catalogue/featured) answer a bare JSON array.
bool fetchJsonArray(const QUrl& url, QJsonArray* out, QString* error) {
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent","XMB-Release-Sources/0.2");
    QNetworkReply* reply=manager.get(request);
    QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    bool timedOut=false;
    QObject::connect(&timer,&QTimer::timeout,&loop,[&]{timedOut=true;reply->abort();});
    QObject::connect(reply,&QNetworkReply::downloadProgress,&loop,[&](qint64 received,qint64){if(received>16*1024*1024){timedOut=true;reply->abort();}}); // a source cannot stream an unbounded body into memory
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    timer.start(30000); loop.exec();
    if(timedOut || reply->error()!=QNetworkReply::NoError ||
       reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()>=400) {
        const QString message=responseError(QStringLiteral("catalogue"),reply,timedOut); reply->deleteLater();
        if(error)*error=message; return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document=QJsonDocument::fromJson(reply->readAll(),&parseError);
    reply->deleteLater();
    if(document.isNull() || !document.isArray()) { if(error)*error=QStringLiteral("Hydra catalogue returned invalid JSON; retry"); return false; }
    if(out)*out=document.array(); return true;
}
// The catalogue's dedup key. It must agree with the shell's gameSearchKey() so a game merged here
// matches the imported/online merge in runGameSearch(): Unicode-normalise, collapse whitespace, then
// case-fold. Editions that differ in this key stay apart; identical names merge across sources.
QString catalogNameKey(const QString& name) {
    return name.normalized(QString::NormalizationForm_KC).simplified().toCaseFolded();
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
            // One option per source: identify a source by its release-index URL (unique), falling back
            // to its display name when the URL is absent.
            auto& dst=results[existing.value()];
            const QString identity=option.sourceUrl.isEmpty()?option.source:option.sourceUrl;
            bool have=false;
            for(const auto& old:dst.options) {
                const QString oldIdentity=old.sourceUrl.isEmpty()?old.source:old.sourceUrl;
                if(!identity.isEmpty() && identity==oldIdentity){have=true;break;}
            }
            if(!have)dst.options.append(option);
        }
    }
    std::sort(results.begin(),results.end(),[&installs](const GameResult& a,const GameResult& b){
        int pa=0,pb=0;
        for(const auto& o:a.options)pa=std::max(pa,installs.value(o.source,0));
        for(const auto& o:b.options)pb=std::max(pb,installs.value(o.source,0));
        return pa>pb;
    });
    return results;
}
bool HydraLibrary::listSources(QVector<HydraSource>* out, QString* error) const {
    if(out)out->clear();
    QVector<HydraSource> all;
    int pageNumber=1;
    for(;;) {
        QJsonObject root;
        if(!fetchJson(QUrl(QStringLiteral("https://api.hydralibrary.com/sources?page=%1&limit=100").arg(pageNumber)),&root,error))return false;
        if(!root.contains(QStringLiteral("sources")) || !root.value(QStringLiteral("sources")).isArray()) {
            if(error)*error=QStringLiteral("Hydra library returned an invalid sources response; retry");
            return false;
        }
        const SourcesPage page=parseSourcesPage(root);
        all+=page.sources;
        if(!hasMoreSources(pageNumber,all.size(),page))break;
        ++pageNumber;
    }
    if(out)*out=all; return true;
}
bool HydraLibrary::preinstalledSources(QStringList* out, QString* error) const {
    QVector<HydraSource> sources;
    if(!listSources(&sources,error))return false;
    QStringList names;
    for(const auto& source:sources)if(isPreinstalled(source))names.append(source.title);
    if(out)*out=names; return true;
}
bool HydraLibrary::searchGames(const QString& query, QVector<GameResult>* out, QString* error) const {
    if(out)out->clear();
    QVector<HydraSource> sources;
    if(!listSources(&sources,error))return false;
    QVector<GameRecord> records;
    QSet<QString> names;   // distinct game names, so pagination stops at the API's filteredCount
    for(int page=1;page<=5;++page) {
        QJsonObject root;
        const QUrl url(QStringLiteral("https://api.hydralibrary.com/games?q=%1&limit=100&page=%2")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(query))).arg(page));
        if(!fetchJson(url,&root,error))return false;
        if(!root.contains(QStringLiteral("games")) || !root.value(QStringLiteral("games")).isArray()) {
            if(error)*error=QStringLiteral("Hydra library returned an invalid games response; retry");
            return false;
        }
        const QJsonArray games=root.value(QStringLiteral("games")).toArray();
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
bool HydraLibrary::popularGames(QVector<GameResult>* out, QString* error) const {
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
    if(out)out->clear();
    QJsonArray hot;
    const QString url=loopbackEndpoint("XMB_HYDRA_HOT_URL",
        QStringLiteral("https://api.hydralauncher.gg/catalogue/hot?take=40&skip=0"));
    if(!fetchJsonArray(QUrl(url),&hot,error))return false;
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
            const QString identity=option.sourceUrl.isEmpty()?option.source:option.sourceUrl;
            if(seen.contains(identity))continue;
            seen.insert(identity);merged.options.append(option);
        }
    }
    return merged;
}

bool HydraLibrary::popularGame(const PopularTitle& title,GameResult* out,QString* error) const {
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
    GameResult result;result.name=title.title;result.title=title.title;result.coverUrl=title.coverUrl;
    QSet<QString> seenSource;
    for(const auto& gv:root.value(QStringLiteral("games")).toArray()) {
        const auto game=gv.toObject();
        GameSourceOption option;
        option.source=game.value(QStringLiteral("source")).toString();
        option.sourceUrl=game.value(QStringLiteral("sourceUrl")).toString();
        const QString identity=option.sourceUrl.isEmpty()?option.source:option.sourceUrl;
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
