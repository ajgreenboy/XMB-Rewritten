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
    if(timedOut || reply->error()!=QNetworkReply::NoError) {
        const QString message=reply->errorString(); reply->deleteLater();
        if(error)*error=QStringLiteral("Hydra library: %1").arg(message); return false;
    }
    const QJsonObject root=QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    if(out)*out=root; return true;
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
    if(timedOut || reply->error()!=QNetworkReply::NoError) {
        const QString message=reply->errorString(); reply->deleteLater();
        if(error)*error=QStringLiteral("Hydra catalogue: %1").arg(message); return false;
    }
    const QJsonDocument document=QJsonDocument::fromJson(reply->readAll());
    reply->deleteLater();
    if(!document.isArray()) { if(error)*error=QStringLiteral("Hydra catalogue returned an unexpected response"); return false; }
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

struct PopularLookup {
    bool ok=false;
    QJsonObject root;
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
        const SourcesPage page=parseSourcesPage(root);
        all+=page.sources;
        if(!hasMoreSources(pageNumber,all.size(),page))break;
        ++pageNumber;
    }
    if(all.isEmpty()) { if(error)*error=QStringLiteral("Hydra library returned no sources"); return false; }
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

    // The Hydra Cloud "hot" catalogue is the real popularity ranking. take=40 keeps the worker's
    // round-trips bounded (one search per title).
    QJsonArray hot;
    if(!fetchJsonArray(QUrl(QStringLiteral("https://api.hydralauncher.gg/catalogue/hot?take=40&skip=0")),&hot,error))
        return false;

    // Each /games lookup is independent. Keep the pool deliberately small: this is a
    // catalogue refresh, not an invitation to open 40 sockets at once. Futures are
    // consumed in hot-catalogue order below, so completion timing can never reorder
    // the Store tiles.
    QThreadPool pool;
    pool.setMaxThreadCount(6);
    QVector<QFuture<PopularLookup>> lookups;
    lookups.reserve(hot.size());
    for(const auto& value:hot) {
        const QString title=value.toObject().value(QStringLiteral("title")).toString().trimmed();
        const QString searchUrl=QStringLiteral("https://api.hydralibrary.com/games?q=%1&limit=100&page=1")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(title)));
        lookups.append(QtConcurrent::run(&pool,[searchUrl] {
            QJsonObject root;
            PopularLookup lookup;
            lookup.ok=fetchJson(QUrl(searchUrl),&root,nullptr);
            lookup.root=std::move(root);
            return lookup;
        }));
    }

    QVector<GameResult> results;
    QHash<QString,int> byName;
    for(int hotIndex=0;hotIndex<hot.size();++hotIndex) {
        const auto& value=hot.at(hotIndex);
        const QJsonObject entry=value.toObject();
        const QString title=entry.value(QStringLiteral("title")).toString().trimmed();
        if(title.isEmpty())continue;
        const QString cover=entry.value(QStringLiteral("libraryImageUrl")).toString();

        // Match this popular title against the download library (single page; the query is specific).
        const PopularLookup lookup=lookups.at(hotIndex).result();
        if(!lookup.ok)continue; // one dead source/title must not erase other hot tiles
        const QJsonObject& root=lookup.root;

        GameResult result;
        result.name=title;
        result.title=title;
        result.coverUrl=cover;
        QSet<QString> seenSource;   // one download option per source
        for(const auto& gv:root.value(QStringLiteral("games")).toArray()) {
            const QJsonObject game=gv.toObject();
            const QString sourceName=game.value(QStringLiteral("source")).toString();
            const QString sourceUrl=game.value(QStringLiteral("sourceUrl")).toString();
            const QString sourceIdentity=sourceUrl.isEmpty()?sourceName:sourceUrl;
            if(seenSource.contains(sourceIdentity))continue;
            seenSource.insert(sourceIdentity);
            GameSourceOption option;
            option.source=sourceName;
            option.sourceUrl=game.value(QStringLiteral("sourceUrl")).toString();
            option.fileSize=game.value(QStringLiteral("fileSize")).toString();
            option.sourceStatus=game.value(QStringLiteral("sourceStatus")).toString();
            option.releaseTitle=game.value(QStringLiteral("originalTitle")).toString();
            if(option.releaseTitle.isEmpty())option.releaseTitle=game.value(QStringLiteral("title")).toString();
            if(result.originalTitle.isEmpty())
                result.originalTitle=game.value(QStringLiteral("originalTitle")).toString();
            result.options.append(option);
        }
        if(result.options.isEmpty())continue;   // not downloadable: drop it

        const QString key=catalogNameKey(title);
        const auto existing=byName.find(key);
        if(existing==byName.end()) {
            byName.insert(key,results.size());
            results.append(result);
        } else {
            auto& dst=results[existing.value()];
            if(dst.coverUrl.isEmpty())dst.coverUrl=cover;
            QSet<QString> have;
            for(const auto& o:dst.options)have.insert(o.sourceUrl.isEmpty()?o.source:o.sourceUrl);
            for(const auto& o:result.options) {
                const QString identity=o.sourceUrl.isEmpty()?o.source:o.sourceUrl;
                if(!have.contains(identity)) { have.insert(identity); dst.options.append(o); }
            }
        }
    }
    if(out)*out=results;
    return !results.isEmpty();
}
} // namespace xmb
