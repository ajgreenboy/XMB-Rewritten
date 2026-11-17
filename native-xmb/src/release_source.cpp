#include "release_source.h"
#include "real_debrid.h"
#include "flare_solverr.h"
#include "preferences.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <limits>
#include <atomic>

namespace xmb {
namespace {
constexpr qint64 kRefreshCooldownMs=15LL*60*1000;
void fail(QString* error,const QString& message) { if(error)*error=message; }
bool looksLikeHtml(const QByteArray& bytes) {
    const QString head=QString::fromLatin1(bytes.left(512)).trimmed().toLower();
    return head.startsWith(QStringLiteral("<!doctype html")) || head.startsWith(QStringLiteral("<html"));
}
QString safeName(QString text) {
    text.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")),QStringLiteral("_"));
    text=text.trimmed().left(100);
    while(text.endsWith(QLatin1Char('.')) || text.endsWith(QLatin1Char(' ')))text.chop(1);
    if(text.isEmpty() || text==QStringLiteral(".") || text==QStringLiteral(".."))text=QStringLiteral("release");
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"),QRegularExpression::CaseInsensitiveOption);
    if(reserved.match(text).hasMatch())text.prepend(QLatin1Char('_'));
    return text;
}
QString slugFor(const QString& name,const QUrl& url) {
    QString slug=name.toLower(); slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),QStringLiteral("-"));
    slug=slug.left(48); while(slug.endsWith(QLatin1Char('-')))slug.chop(1);
    if(slug.isEmpty())slug=QStringLiteral("source");
    return slug+QLatin1Char('-')+QString::fromLatin1(QCryptographicHash::hash(url.toEncoded(),QCryptographicHash::Sha256).toHex().left(12));
}
// Same streaming QNAM + event-loop + timer pattern as firmware_fetcher::fetch.
// file: is allowed only for importing a local index, never for resolved downloads.
bool fetch(const QUrl& url,QIODevice* output,qint64 limit,int timeoutMs,QString* error,
           const ReleaseProgress& progress={},bool localIndex=false,const std::atomic_bool* cancel=nullptr) {
    if(cancel && cancel->load()){fail(error,QStringLiteral("Download cancelled"));return false;}
    if(localIndex && url.isLocalFile()) {
        QFile file(url.toLocalFile());
        if(!file.open(QIODevice::ReadOnly)){fail(error,QStringLiteral("Cannot read local source: ")+file.errorString());return false;}
        if(file.size()>limit){fail(error,QStringLiteral("Source index too large"));return false;}
        while(!file.atEnd()) {
            if(cancel && cancel->load()){fail(error,QStringLiteral("Download cancelled"));return false;}
            const auto bytes=file.read(65536);
            if(file.error()!=QFileDevice::NoError || output->write(bytes)!=bytes.size()){fail(error,QStringLiteral("Cannot read or write source index"));return false;}
        }
        return true;
    }
    if(!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || (url.scheme()!=QStringLiteral("http") && url.scheme()!=QStringLiteral("https"))) {
        fail(error,QStringLiteral("Invalid HTTP(S) URL"));return false;
    }
    QNetworkAccessManager manager; QNetworkRequest request(url);
    request.setRawHeader("User-Agent","XMB-Release-Sources/0.2");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply=manager.get(request);
    QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    bool timedOut=false,failedWrite=false; qint64 received=0;
    const auto consume=[&] {
        if(cancel && cancel->load()) { reply->abort(); return; }
        if(reply->bytesAvailable()==0)return;
        const auto bytes=reply->readAll();
        if(bytes.size()>limit-received || output->write(bytes)!=bytes.size()){failedWrite=true;reply->abort();return;}
        received+=bytes.size();
    };
    QObject::connect(&timer,&QTimer::timeout,&loop,[&]{timedOut=true;reply->abort();});
    QObject::connect(reply,&QNetworkReply::readyRead,&loop,consume);
    QObject::connect(reply,&QNetworkReply::downloadProgress,&loop,[&](qint64 a,qint64 b){timer.start(timeoutMs);if(progress)progress(a,b);});
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    QTimer cancellation;
    QObject::connect(&cancellation,&QTimer::timeout,&loop,[&]{if(cancel->load())reply->abort();});
    if(cancel)cancellation.start(100);
    timer.start(timeoutMs);
    if(!reply->isFinished())loop.exec();
    consume();
    const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if(cancel && cancel->load())fail(error,QStringLiteral("Download cancelled"));
    else if(timedOut)fail(error,QStringLiteral("Download timed out"));
    else if(failedWrite)fail(error,QStringLiteral("Response too large or cannot write download"));
    else if(reply->error()!=QNetworkReply::NoError)
        fail(error,QStringLiteral("Network error: %1").arg(reply->errorString()));
    else if(status<200 || status>=300)
        fail(error,QStringLiteral("Server returned HTTP %1").arg(status));
    else return true;
    return false;
}

constexpr auto PartialFile = ".xmb-game.part";
constexpr auto PartialMetadata = ".xmb-game.resume.json";

QString transferIdentity(const QUrl& url) {
    return QString::fromLatin1(QCryptographicHash::hash(url.toEncoded(),QCryptographicHash::Sha256).toHex());
}

bool strongValidator(const QByteArray& value) {
    if(value.size()<2 || value.size()>512 || value.front()!='"' || value.back()!='"')return false;
    for(qsizetype i=1;i+1<value.size();++i)
        if(uchar(value[i])<0x21 || value[i]=='"' || value[i]==0x7f)return false;
    return true;
}

struct Checkpoint {
    QString resource;
    QByteArray validator;
    qint64 length=-1;
    qint64 written=0;
};

Checkpoint readCheckpoint(const QString& path,const QString& resource) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly) || file.size()>4096)return {};
    const auto data=QJsonDocument::fromJson(file.readAll()).object();
    const auto validator=QByteArray::fromBase64(data.value(QStringLiteral("etag")).toString().toLatin1());
    bool valid=false;
    const qint64 length=data.value(QStringLiteral("length")).toString().toLongLong(&valid);
    bool offsetValid=false;
    const qint64 written=data.value(QStringLiteral("written")).toString().toLongLong(&offsetValid);
    if(!valid || !offsetValid || written<=0 || length < -1 || length==0 ||
       (length>0 && written>=length) || !strongValidator(validator) ||
       data.value(QStringLiteral("resource")).toString()!=resource)return {};
    return {resource,validator,length,written};
}

bool writeCheckpoint(const QString& path,const Checkpoint& checkpoint) {
    const QJsonObject data{{QStringLiteral("resource"),checkpoint.resource},
        {QStringLiteral("etag"),QString::fromLatin1(checkpoint.validator.toBase64())},
        {QStringLiteral("length"),QString::number(checkpoint.length)},
        {QStringLiteral("written"),QString::number(checkpoint.written)}};
    const auto bytes=QJsonDocument(data).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit();
}

qint64 contentRangeTotal(const QByteArray& header,qint64 start) {
    static const QRegularExpression pattern(QStringLiteral("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$"));
    const auto match=pattern.match(QString::fromLatin1(header));
    if(!match.hasMatch())return -1;
    bool firstOk=false,lastOk=false,totalOk=false;
    const auto first=match.capturedView(1).toLongLong(&firstOk);
    const auto last=match.capturedView(2).toLongLong(&lastOk);
    const auto total=match.capturedView(3).toLongLong(&totalOk);
    if(!firstOk || !lastOk || !totalOk || first!=start || total<=start || last!=total-1)return -1;
    return total;
}

bool publishGameArchive(const QString& partial,const QString& path) {
    if(!QFile::exists(path))return QFile::rename(partial,path);
    QFile input(partial);
    QSaveFile replacement(path);
    replacement.setDirectWriteFallback(false);
    if(!input.open(QIODevice::ReadOnly) || !replacement.open(QIODevice::WriteOnly))return false;
    while(!input.atEnd()) {
        const auto block=input.read(256*1024);
        if(block.isEmpty() || replacement.write(block)!=block.size())return false;
    }
    input.close();
    if(!replacement.commit())return false;
    QFile::remove(partial);
    return true;
}

bool downloadGameArchive(const QUrl& url,const QString& path,QString* error,const ReleaseProgress& progress,
                         const std::atomic_bool* cancel,const std::atomic_bool* pausing) {
    if(!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
       (url.scheme()!=QLatin1String("http") && url.scheme()!=QLatin1String("https"))) {
        fail(error,QStringLiteral("Invalid HTTP(S) URL"));return false;
    }
    const QDir directory=QFileInfo(path).absoluteDir();
    const QString partial=directory.filePath(QLatin1String(PartialFile));
    const QString metadata=directory.filePath(QLatin1String(PartialMetadata));
    const QString resource=transferIdentity(url);
    const auto stopped=[&]{return cancel && cancel->load();};
    QNetworkAccessManager manager;
    for(int attempt=0;attempt<2;++attempt) {
        if(stopped())break;
        Checkpoint checkpoint=attempt==0?readCheckpoint(metadata,resource):Checkpoint{};
        const QFileInfo partialInfo(partial);
        if(partialInfo.isSymLink() || QFileInfo(metadata).isSymLink()) {
            fail(error,QStringLiteral("Invalid download staging file"));return false;
        }
        if(partialInfo.size()<checkpoint.written)checkpoint={};
        qint64 offset=checkpoint.written;
        const bool ranged=offset>0;
        QNetworkRequest request(url);
        request.setRawHeader("Accept-Encoding","identity");
        request.setTransferTimeout(60000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
        if(ranged) {
            request.setRawHeader("Range","bytes="+QByteArray::number(offset)+"-");
            request.setRawHeader("If-Range",checkpoint.validator);
        }
        auto* reply=manager.get(request);
        reply->setReadBufferSize(256*1024);
        QFile file(partial);
        QEventLoop loop;
        QTimer interrupt;
        bool restart=false;
        bool checked=false;
        QString failure;
        qint64 expected=-1;
        QElapsedTimer checkpointTimer;
        const auto flushCheckpoint=[&] {
            if(!file.isOpen() || checkpoint.validator.isEmpty() || offset<=0 ||
               (checkpoint.length>=0 && offset>=checkpoint.length))return false;
            if(!file.flush())return false;
            auto saved=checkpoint;
            saved.written=offset;
            if(!writeCheckpoint(metadata,saved))return false;
            checkpoint=saved;
            checkpointTimer.start();
            return true;
        };
        const auto consume=[&]{
            if(stopped())return;
            if(!checked) {
                const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if(status==0 || (status>=300 && status<400))return;
                checked=true;
                const QByteArray encoding=reply->rawHeader("Content-Encoding").trimmed().toLower();
                bool knownLength=false;
                const auto length=reply->rawHeader("Content-Length").toLongLong(&knownLength);
                const auto validator=reply->rawHeader("ETag");
                if(status==416 && ranged) {restart=true;reply->abort();return;}
                if(status==206 && ranged) {
                    expected=contentRangeTotal(reply->rawHeader("Content-Range"),offset);
                    if(reply->url()!=url || validator!=checkpoint.validator || expected<0 ||
                       (checkpoint.length>=0 && expected!=checkpoint.length) ||
                       (knownLength && length!=expected-offset)) {
                        restart=true;reply->abort();return;
                    }
                    checkpoint.length=expected;
                } else if(status==200) {
                    offset=0;
                    expected=knownLength?length:-1;
                    // A redirect can change representations between requests. Start fresh for such links.
                    checkpoint=reply->url()==url && strongValidator(validator)?Checkpoint{resource,validator,expected}:Checkpoint{};
                } else {
                    failure=QStringLiteral("Server returned HTTP %1").arg(status);reply->abort();return;
                }
                // Drop the old validator before truncating for a new response. A crash between
                // these operations can lose progress but cannot validate bytes of a different file.
                if(offset==0 && QFileInfo::exists(metadata) && !QFile::remove(metadata)) {
                    failure=QStringLiteral("Could not replace the old download checkpoint");reply->abort();return;
                }
                if((!encoding.isEmpty() && encoding!="identity") || expected==0 ||
                   !file.open(QIODevice::WriteOnly | (offset>0?QIODevice::Append:QIODevice::Truncate)) ||
                   (offset>0 && !file.resize(offset))) {
                    failure=QStringLiteral("Could not open a valid download stream");reply->abort();return;
                }
            }
            if(!file.isOpen())return;
            const auto bytes=reply->readAll();
            if((expected>=0 && bytes.size()>expected-offset) || file.write(bytes)!=bytes.size()) {
                failure=QStringLiteral("Download exceeds its reported size or could not be written");reply->abort();return;
            }
            offset+=bytes.size();
            if(!bytes.isEmpty() && (checkpoint.written==0 || !checkpointTimer.isValid() ||
               checkpointTimer.elapsed()>=2000 || offset-checkpoint.written>=4*1024*1024)) {
                if(!flushCheckpoint())checkpointTimer.start();
            }
            if(progress && !bytes.isEmpty())progress(offset,expected);
        };
        QObject::connect(reply,&QNetworkReply::readyRead,&loop,consume);
        QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
        QObject::connect(&interrupt,&QTimer::timeout,&loop,[&]{if(stopped())reply->abort();});
        if(cancel)interrupt.start(100);
        if(!reply->isFinished())loop.exec();
        consume();
        const bool stoppedForPause=stopped() && pausing && pausing->load() && failure.isEmpty();
        if(stoppedForPause)flushCheckpoint();
        const bool flushed=!file.isOpen() || file.flush();
        file.close();
        const auto networkError=reply->error();
        const auto networkMessage=reply->errorString();
        reply->deleteLater();
        if(stopped()) {
            if(stoppedForPause && checkpoint.written>0 && flushed) {
                fail(error,QStringLiteral("Download paused"));return false;
            }
            break;
        }
        if(restart)continue;
        if(!failure.isEmpty())fail(error,failure);
        else if(networkError!=QNetworkReply::NoError)fail(error,QStringLiteral("Network error: %1").arg(networkMessage));
        else if(!checked || !flushed || offset<=0 || (expected>=0 && offset!=expected))
            fail(error,QStringLiteral("The archive download was incomplete"));
        else if(publishGameArchive(partial,path)) {
            QFile::remove(metadata);
            return true;
        } else fail(error,QStringLiteral("Could not publish the archive to the game download folder"));
        ReleaseSources::discardGamePartial(directory.path());
        return false;
    }
    ReleaseSources::discardGamePartial(directory.path());
    fail(error,stopped()?QStringLiteral("Download stopped"):QStringLiteral("Server returned an invalid resume response"));
    return false;
}

void pause(int milliseconds) {
    QEventLoop loop;QTimer::singleShot(milliseconds,&loop,&QEventLoop::quit);loop.exec();
}
}
void ReleaseSources::discardGamePartial(const QString& destDir) {
    const QDir directory(destDir);
    QFile::remove(directory.filePath(QLatin1String(PartialFile)));
    QFile::remove(directory.filePath(QLatin1String(PartialMetadata)));
}

ReleaseSources::ReleaseSources(const QString& dataDirectory,const Preferences& preferences)
    :dataDirectory_(dataDirectory),preferences_(preferences) {}
bool ReleaseSources::fetchSourceJson(const QUrl& jsonUrl,QVector<IndexedRelease>* releases,QString* name,QString* error,const std::atomic_bool* cancel) const {
    if(error)error->clear();
    QByteArray bytes; QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
    QString directError;
    const bool fetched=fetch(jsonUrl,&buffer,64LL*1024*1024,30000,&directError,{},true,cancel);
    if(cancel && cancel->load()){fail(error,QStringLiteral("Download cancelled"));return false;}
    const QString solver=preferences_.value(QStringLiteral("debrid/flareSolverrUrl")).toString().trimmed();
    if(!solver.isEmpty() && (!fetched || looksLikeHtml(bytes))) {
        QString solverError;
        if(!FlareSolverr(solver).request(jsonUrl,&bytes,&solverError,cancel)) {
            fail(error,fetched?QStringLiteral("Source returned an HTML challenge (FlareSolverr: %1)").arg(solverError)
                               :QStringLiteral("%1 (FlareSolverr: %2)").arg(directError,solverError));
            return false;
        }
    } else if(!fetched) {
        fail(error,directError);return false;
    }
    QJsonParseError parse;
    const auto document=QJsonDocument::fromJson(QString::fromUtf8(bytes).toUtf8(),&parse);
    if(parse.error!=QJsonParseError::NoError){fail(error,QStringLiteral("Invalid source JSON: ")+parse.errorString());return false;}
    QString parseError; const auto parsed=parseReleaseIndex(document,&parseError);
    if(!parseError.isEmpty()){fail(error,parseError);return false;}
    if(releases)*releases=parsed;
    if(name) {
        QString sourceName=document.object().value(QStringLiteral("name")).toString().trimmed();
        if(sourceName.isEmpty())sourceName=parsed.isEmpty()?QStringLiteral("Source"):parsed.first().source;
        *name=sourceName;
    }
    return true;
}
bool ReleaseSources::importSource(const QUrl& jsonUrl,QString* error) {
    if(error)error->clear();
    QVector<IndexedRelease> releases; QString name;
    if(!fetchSourceJson(jsonUrl,&releases,&name,error))return false;
    const QString slug=slugFor(name,jsonUrl);
    const QDir directory(QDir(dataDirectory_).filePath(QStringLiteral("sources")));
    if(!QDir().mkpath(directory.path())){fail(error,QStringLiteral("Cannot create sources directory"));return false;}
    QJsonArray records;for(const auto& release:releases)records.append(releaseToJson(release));
    const QJsonObject root{{QStringLiteral("version"),1},{QStringLiteral("slug"),slug},{QStringLiteral("name"),name},
        {QStringLiteral("url"),jsonUrl.toString(QUrl::FullyEncoded)},{QStringLiteral("releases"),records},
        {QStringLiteral("refreshedAt"),QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()}};
    QSaveFile file(directory.filePath(slug+QStringLiteral(".json")));file.setDirectWriteFallback(false);
    const auto saved=QJsonDocument(root).toJson(QJsonDocument::Compact);
    if(!file.open(QIODevice::WriteOnly) || file.write(saved)!=saved.size() || !file.commit()) {
        fail(error,QStringLiteral("Cannot save source: ")+file.errorString());return false;
    }
    return true;
}
int ReleaseSources::refreshSources(QStringList* errors,bool force) {
    if(errors)errors->clear();
    const QDir directory(QDir(dataDirectory_).filePath(QStringLiteral("sources")));
    int refreshed=0;
    for(const auto& entry:directory.entryInfoList({QStringLiteral("*.json")},QDir::Files,QDir::Name)) {
        QFile stored(entry.filePath());
        if(!stored.open(QIODevice::ReadOnly) || stored.size()>256LL*1024*1024) {
            if(errors)errors->append(QStringLiteral("%1: cannot read stored source").arg(entry.fileName()));
            continue;
        }
        QJsonParseError parse; const auto oldDocument=QJsonDocument::fromJson(stored.readAll(),&parse);
        // The old handle must be closed before QSaveFile replaces the same path on Windows.
        stored.close();
        const QJsonObject old=oldDocument.object();
        if(parse.error!=QJsonParseError::NoError || old.value(QStringLiteral("version")).toInt()!=1 ||
           !old.value(QStringLiteral("releases")).isArray()) {
            if(errors)errors->append(QStringLiteral("%1: invalid stored source").arg(entry.fileName()));
            continue;
        }
        const QUrl url(old.value(QStringLiteral("url")).toString());
        if(url.isLocalFile())continue;
        const qint64 last=old.value(QStringLiteral("refreshedAt")).toVariant().toLongLong();
        const qint64 now=QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
        if(!force && last>0 && now-last<kRefreshCooldownMs)continue;
        QVector<IndexedRelease> releases; QString name,error;
        if(!fetchSourceJson(url,&releases,&name,&error)) {
            if(errors)errors->append(QStringLiteral("%1: %2").arg(entry.fileName(),error));
            continue;
        }
        // Keep the original slug and source URL; an upstream name change must not
        // orphan existing downloads or the shell's selected source.
        const QString savedName=name.isEmpty()?old.value(QStringLiteral("name")).toString():name;
        QJsonArray records; for(const auto& release:releases)records.append(releaseToJson(release));
        QJsonObject next=old;
        next.insert(QStringLiteral("name"),savedName);
        next.insert(QStringLiteral("releases"),records);
        next.insert(QStringLiteral("refreshedAt"),now);
        QSaveFile file(entry.filePath()); file.setDirectWriteFallback(false);
        const auto bytes=QJsonDocument(next).toJson(QJsonDocument::Compact);
        if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {
            if(errors)errors->append(QStringLiteral("%1: cannot save refreshed source: %2").arg(entry.fileName(),file.errorString()));
            continue;
        }
        ++refreshed;
    }
    return refreshed;
}
bool ReleaseSources::removeSource(const QString& slug,QString* error) {
    if(error)error->clear();
    const QDir directory(QDir(dataDirectory_).filePath(QStringLiteral("sources")));
    const QString safe=slugFor(slug,QUrl(QStringLiteral("https://removed.invalid"))).section(QLatin1Char('-'),0,0);
    for(const auto& entry:directory.entryInfoList({QStringLiteral("*.json")},QDir::Files)) {
        const QString name=entry.completeBaseName();
        if(name==slug || name.startsWith(safe+QLatin1Char('-'))) {
            if(!QFile::remove(entry.filePath())) { fail(error,QStringLiteral("Cannot remove source: ")+name); return false; }
            return true;
        }
    }
    return true; // already absent
}
QVector<ReleaseSource> ReleaseSources::listSources(QString* error) const {
    if(error)error->clear();
    QVector<ReleaseSource> sources;
    const QDir directory(QDir(dataDirectory_).filePath(QStringLiteral("sources")));
    for(const auto& entry:directory.entryInfoList({QStringLiteral("*.json")},QDir::Files,QDir::Name)) {
        QFile file(entry.filePath());
        if(!file.open(QIODevice::ReadOnly) || file.size()>256LL*1024*1024){fail(error,QStringLiteral("Cannot read stored source: ")+entry.fileName());continue;}
        QJsonParseError parse; const auto document=QJsonDocument::fromJson(file.readAll(),&parse); const auto root=document.object();
        if(parse.error!=QJsonParseError::NoError || root.value(QStringLiteral("version")).toInt()!=1 || !root.value(QStringLiteral("releases")).isArray()) {
            fail(error,QStringLiteral("Invalid stored source: ")+entry.fileName());continue;
        }
        ReleaseSource source;source.slug=entry.completeBaseName();source.name=root.value(QStringLiteral("name")).toString();source.url=QUrl(root.value(QStringLiteral("url")).toString());
        for(const auto& record:root.value(QStringLiteral("releases")).toArray())if(record.isObject())source.releases.append(releaseFromJson(record.toObject()));
        sources.append(source);
    }
    return sources;
}
QString ReleaseSources::downloadedPath(const QString& source,const QString& title) const {
    const QDir directory(QDir(dataDirectory_).filePath(QStringLiteral("downloads/")+safeName(source)));
    for(const auto& file:directory.entryInfoList(QDir::Files,QDir::Time))
        if(!file.isSymLink() && file.completeBaseName()==safeName(title))return file.absoluteFilePath();
    return {};
}
QString ReleaseSources::downloadResolved(const QVector<Download>& candidates,const QString& title,const QString& destDir,
                                         QString* error,const ReleaseProgress& progress,const std::atomic_bool* cancel,const std::atomic_bool* pausing) const {
    if(cancel && cancel->load()){fail(error,QStringLiteral("Download cancelled"));return {};}
    QString direct,magnet;
    for(const auto& download:candidates) {
        if(direct.isEmpty() && download.kind==QStringLiteral("direct"))direct=download.uri;
        if(magnet.isEmpty() && download.kind==QStringLiteral("magnet"))magnet=download.uri;
    }
    RealDebrid debrid(preferences_,cancel);QUrl resolved;
    QString debridError;
    if(!direct.isEmpty()) {
        // The spec permits ordinary HTTP downloads when no manager token was configured.
        resolved=debrid.hasToken()?debrid.unrestrict(QUrl(direct),&debridError):QUrl(direct);
    } else if(!magnet.isEmpty()) {
        const QString id=debrid.addMagnet(magnet,&debridError);
        if(id.isEmpty()){fail(error,debridError);return {};}
        QElapsedTimer polling;polling.start();bool selected=false;
        while(polling.elapsed()<10*60*1000 && !(cancel && cancel->load())) {
            const auto info=debrid.torrentInfo(id,&debridError);
            if(!debridError.isEmpty())break;
            if(info.status==QStringLiteral("waiting_files_selection") && !selected) {
                if(!debrid.selectFiles(id,QStringLiteral("all"),&debridError))break;
                selected=true;
            } else if(info.status==QStringLiteral("downloaded")) {
                if(info.links.isEmpty())debridError=QStringLiteral("Real-Debrid torrent has no download links");
                else resolved=debrid.unrestrict(QUrl(info.links.first()),&debridError);
                break;
            } else if(info.status==QStringLiteral("error") || info.status==QStringLiteral("magnet_error") || info.status==QStringLiteral("virus") || info.status==QStringLiteral("dead")) {
                debridError=QStringLiteral("Real-Debrid torrent failed: ")+info.status;break;
            }
            if(progress)progress(0,-1);pause(2000);
        }
        if(resolved.isEmpty() && debridError.isEmpty())debridError=QStringLiteral("Real-Debrid torrent timed out");
    } else {
        fail(error,QStringLiteral("No direct download or supported magnet for this release"));return {};
    }
    if(cancel && cancel->load()){fail(error,QStringLiteral("Download cancelled"));return {};}
    if(!debridError.isEmpty()){fail(error,debridError);return {};}
    QString extension=QFileInfo(safeName(resolved.fileName())).suffix();
    if(extension.isEmpty())extension=QStringLiteral("bin");
    const QDir directory(destDir);
    if(!QDir().mkpath(directory.path())){fail(error,QStringLiteral("Cannot create download directory"));return {};}
    const QString path=directory.filePath(safeName(title)+QLatin1Char('.')+extension);
    if(pausing)return downloadGameArchive(resolved,path,error,progress,cancel,pausing)?path:QString{};
    QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)){fail(error,QStringLiteral("Cannot open download: ")+file.errorString());return {};}
    if(!fetch(resolved,&file,std::numeric_limits<qint64>::max(),60000,error,progress,false,cancel))return {};
    if(cancel && cancel->load()){fail(error,QStringLiteral("Download cancelled"));return {};}
    if(!file.commit()){fail(error,QStringLiteral("Cannot save download: ")+file.errorString());return {};}
    return path;
}
QString ReleaseSources::downloadRelease(const QString& source,const QString& title,QString* error,const ReleaseProgress& progress) const {
    if(error)error->clear();
    QString storeError;const auto sources=listSources(&storeError);
    for(const auto& stored:sources) {
        if(stored.slug!=source && stored.name!=source)continue;
        for(const auto& release:stored.releases) {
            if(release.title!=title)continue;
            QVector<Download> candidates=release.downloads;
            for(const auto& mirror:release.mirrors)candidates+=mirror.parts;
            const QString destDir=QDir(dataDirectory_).filePath(QStringLiteral("downloads/")+safeName(stored.slug));
            return downloadResolved(candidates,title,destDir,error,progress);
        }
    }
    fail(error,storeError.isEmpty()?QStringLiteral("Release not found in imported sources"):storeError);return {};
}
QString ReleaseSources::downloadGameByUrl(const QString& title,const QUrl& sourceUrl,const QString& destDir,
                                          QString* error,const ReleaseProgress& progress) const {
    if(error)error->clear();
    QVector<IndexedRelease> releases; QString sourceName;
    if(!fetchSourceJson(sourceUrl,&releases,&sourceName,error))return {};
    const QString key=title.trimmed().toLower();
    const IndexedRelease* found=nullptr;
    for(const auto& release:releases) {
        const QString r=release.title.trimmed().toLower();
        if(r==key || r.contains(key) || key.contains(r)) { found=&release; break; }
    }
    if(!found){fail(error,QStringLiteral("Game not found in the selected source"));return {};}
    QVector<Download> candidates=found->downloads;
    for(const auto& mirror:found->mirrors)candidates+=mirror.parts;
    const QString titleForFile=found->title.isEmpty()?title:found->title;
    return downloadResolved(candidates,titleForFile,destDir,error,progress);
}
QString ReleaseSources::downloadGame(const QString& sourceSlug,const QUrl& sourceUrl,const QString& releaseTitle,
                                     const QVector<Download>& reviewed,const QString& destDir,QString* error,
                                     const ReleaseProgress& progress,const std::atomic_bool* cancel,const std::atomic_bool* pausing) const {
    if(error)error->clear();
    if(!reviewed.isEmpty()) {
        return downloadResolved(reviewed,releaseTitle,destDir,error,progress,cancel,pausing);
    }
    QVector<IndexedRelease> releases;
    if(!sourceSlug.isEmpty()) {
        QString storeError;
        for(const auto& stored:listSources(&storeError)) {
            if(stored.slug==sourceSlug){releases=stored.releases;break;}
        }
        if(releases.isEmpty() && !storeError.isEmpty()){fail(error,storeError);return {};}
    } else if(!fetchSourceJson(sourceUrl,&releases,nullptr,error,cancel)) {
        return {};
    }
    for(const auto& release:releases) {
        if(release.title.compare(releaseTitle,Qt::CaseInsensitive)!=0)continue;
        QVector<Download> candidates=release.downloads;
        for(const auto& mirror:release.mirrors) {
            if(mirror.parts.size()==1)candidates.append(mirror.parts.first());
        }
        return downloadResolved(candidates,releaseTitle,destDir,error,progress,cancel,pausing);
    }
    fail(error,QStringLiteral("Release not found in the selected source"));
    return {};
}

}
