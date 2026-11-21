#include "hydra_library.h"
#include <QDateTime>
#include <QElapsedTimer>
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
// Retry-After (RFC 7231 §7.1.3) is either delay-seconds = 1*DIGIT (decimal digits only: no sign,
// no fraction, no unit suffix) or an HTTP-date. parseRetryAfterSeconds returns the digit-seconds
// (>= 0), or one of the two sentinels below for an absent or unparseable header.
constexpr qint64 kRetryAfterAbsent=-1;     // no Retry-After header (or an empty value)
constexpr qint64 kRetryAfterMalformed=-2;  // present but neither a digit run nor an HTTP-date

// The result of one blocking GET, kept separate from the error string so the retry loop can
// classify a failure instead of guessing from prose. `status` is the final HTTP status (0 when
// none), and `networkError` the reply's own error (which aborts turn into OperationCanceledError,
// so `timedOut`/`invalidRedirect`/`overflow` carry those cases instead).
struct FetchOutcome {
    bool ok=false;
    bool invalidUrl=false;
    QByteArray body;
    int status=0;
    QNetworkReply::NetworkError networkError=QNetworkReply::NoError;
    bool timedOut=false;
    bool invalidRedirect=false;
    bool overflow=false;
    qint64 retryAfterSeconds=kRetryAfterAbsent;   // parsed Retry-After (see sentinels above)
};
bool fetchBody(const QUrl& url,const QString& service,QByteArray* out,QString* error);

// Parses a Retry-After header value into seconds, honouring the strict 1*DIGIT grammar for the
// delay-seconds form (leading/trailing optional whitespace is field framing, not part of the value).
qint64 parseRetryAfterSeconds(const QByteArray& value) {
    const QByteArray field=value.trimmed();
    if(field.isEmpty())return kRetryAfterAbsent;
    bool allDigits=!field.isEmpty();
    for(const char c:field)if(c<'0'||c>'9'){allDigits=false;break;}
    if(allDigits) {
        bool ok=false;
        const qlonglong seconds=QString::fromLatin1(field).toLongLong(&ok);
        if(ok && seconds>=0)return seconds;
        return kRetryAfterMalformed;   // a digit run that overflows qlonglong
    }
    const QDateTime when=QDateTime::fromString(QString::fromLatin1(field),Qt::RFC2822Date);
    if(when.isValid()) {
        const qint64 delta=QDateTime::currentDateTimeUtc().secsTo(when.toUTC());
        return delta>0?delta:0;
    }
    return kRetryAfterMalformed;
}

// A curated set of genuinely temporary network conditions worth one bounded retry. Permanent or
// security failures (host not found, TLS handshake, insecure redirect, content/protocol errors)
// are deliberately excluded: replaying those cannot succeed and would only delay the error.
bool isTransientNetworkError(QNetworkReply::NetworkError error) {
    switch(error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        return true;
    default:
        return false;
    }
}

// Retry only an idempotent GET on a transient failure: HTTP 500 and the temporary
// gateway/unavailable statuses (502/503/504), timeouts, and the curated network set above.
// Permanent HTTP errors (any 4xx, 429 rate-limits, and unlisted 5xx), unsafe redirects, TLS
// failures and oversized bodies are never retried.
bool fetchRetryable(const FetchOutcome& outcome) {
    if(outcome.invalidUrl || outcome.invalidRedirect || outcome.overflow)return false;
    if(outcome.timedOut)return true;
    if(outcome.status>0)return outcome.status==500 || outcome.status==502 || outcome.status==503 || outcome.status==504;
    if(outcome.networkError!=QNetworkReply::NoError)return isTransientNetworkError(outcome.networkError);
    return false;
}

QString fetchErrorString(const QString& service,const FetchOutcome& outcome) {
    if(outcome.overflow)return QStringLiteral("Hydra %1 response exceeds 16 MiB; retry").arg(service);
    if(outcome.invalidRedirect)return QStringLiteral("Hydra %1 redirect is not safe HTTP(S); retry").arg(service);
    if(outcome.invalidUrl)return QStringLiteral("Hydra %1 URL must be HTTP(S); retry").arg(service);
    if(outcome.timedOut)return QStringLiteral("Hydra %1 request timed out; retry").arg(service);
    if(outcome.status>=400)return QStringLiteral("Hydra %1 request failed (HTTP %2); retry").arg(service).arg(outcome.status);
    return QStringLiteral("Hydra %1 request failed; retry").arg(service);
}

bool fetchJson(const QUrl& url, QJsonObject* out, QString* error,const QString& service=QStringLiteral("library")) {
    QByteArray body; if(!fetchBody(url,service,&body,error))return false;
    QJsonParseError parseError;
    const QJsonDocument document=QJsonDocument::fromJson(body,&parseError);
    if(document.isNull() || !document.isObject()) {
        if(error)*error=QStringLiteral("Hydra %1 returned invalid JSON; retry").arg(service);
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

// One blocking GET. The body, status and failure reason are returned through FetchOutcome.
// `idleMs` is an idle deadline that restarts on every body chunk (0 disables it), so a slowly
// progressing feed is never cut off while a stalled one is aborted and can be retried; `deadlineMs`
// is an absolute, non-rearmed deadline that always bounds the attempt, so a drip-fed body cannot
// outlive the overall retry budget. Catalogue callers pass idleMs=0 and the historical 30s deadline.
FetchOutcome fetchBodyOnce(const QUrl& url,int idleMs,int deadlineMs) {
    FetchOutcome outcome;
    if((url.scheme()!=QLatin1String("http") && url.scheme()!=QLatin1String("https")) || url.host().isEmpty()) {
        outcome.invalidUrl=true; return outcome;
    }
    constexpr qint64 kMaxBody=16*1024*1024;
    QNetworkAccessManager manager; QNetworkRequest request(url);
    request.setRawHeader("User-Agent","XMB-Release-Sources/0.2");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply=manager.get(request); reply->setReadBufferSize(kMaxBody+1);
    QEventLoop loop; QTimer idleTimer,deadlineTimer; idleTimer.setSingleShot(true); deadlineTimer.setSingleShot(true);
    QByteArray body; bool timedOut=false,invalidRedirect=false,overflow=false;
    const auto abort=[&]{reply->abort();};
    const auto expire=[&]{timedOut=true;abort();};
    QObject::connect(&idleTimer,&QTimer::timeout,&loop,expire);
    QObject::connect(&deadlineTimer,&QTimer::timeout,&loop,expire);
    QObject::connect(reply,&QNetworkReply::redirected,&loop,[&](const QUrl& target){
        if(!safeRedirect(reply->url(),target)){invalidRedirect=true;abort();}
    });
    QObject::connect(reply,&QNetworkReply::readyRead,&loop,[&]{
        if(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong()>kMaxBody){overflow=true;abort();return;}
        const qint64 remaining=kMaxBody-body.size();
        const QByteArray chunk=reply->read(remaining+1);
        if(chunk.size()>remaining){overflow=true;abort();return;}
        body.append(chunk);
        if(idleMs>0)idleTimer.start(idleMs);   // re-arm the idle deadline on progress
    });
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    if(idleMs>0)idleTimer.start(idleMs);
    deadlineTimer.start(deadlineMs);   // absolute: never re-armed, bounds even a progressing body
    loop.exec();
    if(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong()>kMaxBody)overflow=true;
    const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError=reply->error();
    const bool failed=timedOut || invalidRedirect || overflow || networkError!=QNetworkReply::NoError || status>=400;
    if(!failed) {
        outcome.ok=true; outcome.body=std::move(body);
    } else {
        outcome.status=status;
        outcome.networkError=networkError;
        outcome.timedOut=timedOut;
        outcome.invalidRedirect=invalidRedirect;
        outcome.overflow=overflow;
        if(status>=400)outcome.retryAfterSeconds=parseRetryAfterSeconds(reply->rawHeader("Retry-After"));
    }
    reply->deleteLater(); return outcome;
}

bool fetchBody(const QUrl& url,const QString& service,QByteArray* out,QString* error) {
    const FetchOutcome outcome=fetchBodyOnce(url,0,30000);
    if(!outcome.ok) { if(error)*error=fetchErrorString(service,outcome); return false; }
    if(out)*out=outcome.body; return true;
}

void sleepFor(int msecs) {
    if(msecs<=0)return;
    QEventLoop loop; QTimer::singleShot(msecs,&loop,&QEventLoop::quit); loop.exec();
}

// Retry timing policy. The defaults are fixed for production; the loopback fixture may shrink them
// (only while XMB_HYDRA_TEST_FIXTURE=1) so the stall, Retry-After and bound behaviours can be
// exercised deterministically without a multi-second test run. Test-only overrides are never read
// in production.
struct RetryPolicy {
    int maxAttempts=3;
    int budgetMs=20000;      // absolute ceiling on the whole sequence (enforced per attempt)
    int backoffBaseMs=400;   // short backoff before the first retry (doubles per retry)
    int inactivityMs=8000;   // per-attempt idle deadline, reset on each body chunk
};
RetryPolicy retryPolicy() {
    RetryPolicy policy;
    if(qEnvironmentVariableIntValue("XMB_HYDRA_TEST_FIXTURE")!=1)return policy;
    const auto readPositive=[&](const char* name,int fallback) {
        const int value=qEnvironmentVariableIntValue(name);
        return value>0?value:fallback;
    };
    policy.maxAttempts=readPositive("XMB_HYDRA_RETRY_MAX_ATTEMPTS",policy.maxAttempts);
    policy.budgetMs=readPositive("XMB_HYDRA_RETRY_BUDGET_MS",policy.budgetMs);
    policy.backoffBaseMs=readPositive("XMB_HYDRA_RETRY_BACKOFF_MS",policy.backoffBaseMs);
    policy.inactivityMs=readPositive("XMB_HYDRA_RETRY_INACTIVITY_MS",policy.inactivityMs);
    return policy;
}

// Bounded automatic retry of the idempotent selected-feed GET (the Browse Sources path). Each
// attempt runs under the idle deadline (re-armed on progress, so a slowly progressing feed is not
// hard-capped) AND an absolute deadline of the remaining budget (never re-armed, so a drip-fed body
// cannot outlive the budget). A transient failure gets a short, growing backoff that honours
// Retry-After — never repeating before the server says it is safe, never waiting past the remaining
// budget, and never retrying at all when a Retry-After is present but unparseable. The whole sequence
// is capped by maxAttempts and budgetMs.
//
// `sharedBudget` lets one user action that makes several requests (the paginated source directory)
// run under ONE budget: every page and every backoff draws on the same clock, so a long listing cannot
// stretch the wait past the budget by giving each page a fresh one. Without it the call owns its budget.
bool fetchBodyWithRetry(const QUrl& url,const QString& service,QByteArray* out,QString* error,QElapsedTimer* sharedBudget=nullptr) {
    const RetryPolicy policy=retryPolicy();
    QElapsedTimer ownBudget;
    if(!sharedBudget)ownBudget.start();
    QElapsedTimer& budget=sharedBudget?*sharedBudget:ownBudget;
    FetchOutcome outcome;
    outcome.timedOut=true;   // what a budget already spent before the first attempt reports
    for(int attempt=1;;++attempt) {
        const int elapsed=int(budget.elapsed());
        if(elapsed>=policy.budgetMs)break;
        outcome=fetchBodyOnce(url,policy.inactivityMs,policy.budgetMs-elapsed);
        if(outcome.ok) { if(out)*out=outcome.body; return true; }
        if(attempt>=policy.maxAttempts || !fetchRetryable(outcome))break;
        if(outcome.retryAfterSeconds==kRetryAfterMalformed)break;   // unparseable Retry-After: no premature retry
        int wait=policy.backoffBaseMs<<(attempt-1);
        if(outcome.retryAfterSeconds>0) {
            const qint64 secs=outcome.retryAfterSeconds;
            const qint64 retryAfterMs=(secs>=qint64(policy.budgetMs)/1000)?qint64(policy.budgetMs):secs*1000;
            if(retryAfterMs>wait)wait=int(retryAfterMs);
        }
        const int remaining=policy.budgetMs-int(budget.elapsed());
        if(wait>=remaining)break;   // never wait past the budget (no unbounded wait)
        sleepFor(wait);
    }
    if(error)*error=fetchErrorString(service,outcome);
    return false;
}
// One page of the Hydra source directory (Browse Sources), under the directory action's shared budget.
// A transient 500/502/503/504 or timeout is retried inside that budget; 4xx (403, 429 included), unsafe
// redirects and oversized bodies are terminal and are not replayed. The error text names the source
// directory (not just "library") so the screen shows which request failed.
bool fetchDirectoryPage(const QUrl& url,QElapsedTimer* budget,QJsonObject* out,QString* error) {
    const QString service=QStringLiteral("source directory");
    QByteArray body; if(!fetchBodyWithRetry(url,service,&body,error,budget))return false;
    QJsonParseError parseError;
    const QJsonDocument document=QJsonDocument::fromJson(body,&parseError);
    if(document.isNull() || !document.isObject()) {
        if(error)*error=QStringLiteral("Hydra %1 returned invalid JSON; retry").arg(service);
        return false;
    }
    if(out)*out=document.object(); return true;
}
bool fetchDocumentWithRetry(const QUrl& url, QJsonDocument* out, QString* error) {
    QByteArray body; if(!fetchBodyWithRetry(url,QStringLiteral("source"),&body,error))return false;
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
            result.coverUrl=record.coverUrl;
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
void rankGameResults(QVector<GameResult>* results,const QString& query) {
    if(!results)return;
    const QString needle=catalogNameKey(query);
    const QStringList tokens=needle.split(QLatin1Char(' '),Qt::SkipEmptyParts);
    auto score=[&](const GameResult& game) {
        const QString name=catalogNameKey(game.name);
        if(name==needle)return 0;
        if(!needle.isEmpty() && name.startsWith(needle))return 1;
        if(!needle.isEmpty() && name.contains(needle))return 2;
        int matched=0;for(const auto& token:tokens)if(name.contains(token))++matched;
        if(matched>0)return 3+int(tokens.size())-matched;
        return 100;
    };
    std::stable_sort(results->begin(),results->end(),[&](const GameResult& left,const GameResult& right){return score(left)<score(right);});
}
bool HydraLibrary::listSources(QVector<HydraSource>* out, QString* error) const {
    if(error)error->clear();
    if(out)out->clear();
    QVector<HydraSource> all;
    int pageNumber=1;
    const QString endpoint=loopbackEndpoint("XMB_HYDRA_SOURCES_URL",
        QStringLiteral("https://api.hydralibrary.com/sources"));
    // One budget for the whole directory action (all pages and every backoff), started when the action
    // starts. A page that fails for good fails the listing: `all` is local and `out` is only written on
    // success, so a partial source list can never reach the screen.
    QElapsedTimer directoryBudget; directoryBudget.start();
    for(;;) {
        QJsonObject root;
        if(!fetchDirectoryPage(QUrl(endpoint+QStringLiteral("?page=%1&limit=100").arg(pageNumber)),&directoryBudget,&root,error))return false;
        if(!root.contains(QStringLiteral("sources")) || !root.value(QStringLiteral("sources")).isArray()) {
            if(error)*error=QStringLiteral("Hydra source directory returned an invalid sources response; retry");
            return false;
        }
        if(!validSourceArray(root.value(QStringLiteral("sources")).toArray())) {
            if(error)*error=QStringLiteral("Hydra source directory returned an invalid source entry; retry");
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
        if(!fetchJson(url,&root,error,QStringLiteral("game search")))return false;
        if(!root.contains(QStringLiteral("games")) || !root.value(QStringLiteral("games")).isArray()) {
            if(error)*error=QStringLiteral("Hydra game search returned an invalid games response; retry");
            return false;
        }
        const QJsonArray games=root.value(QStringLiteral("games")).toArray();
        if(!validGameArray(games,true)) {
            if(error)*error=QStringLiteral("Hydra game search returned an invalid game entry; retry");
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
            record.coverUrl=game.value(QStringLiteral("coverUrl")).toString();
            const QString key=catalogNameKey(record.name);
            if(key.isEmpty())continue;
            names.insert(key);
            records.append(record);
        }
        const int filtered=root.value(QStringLiteral("filteredCount")).toInt();
        if(names.size()>=filtered || names.size()>=500)break;
    }
    QVector<GameResult> results=buildGameCatalog(records,sources); rankGameResults(&results,query);
    if(out)*out=results; return true;
}
bool HydraLibrary::browseSource(const HydraSource& source, QVector<GameResult>* out, QString* error) const {
    if(error)error->clear(); if(out)out->clear();
    if(!source.url.isValid()) { if(error)*error=QStringLiteral("Hydra source has an invalid URL; retry"); return false; }
    QJsonDocument document;
    if(!fetchDocumentWithRetry(source.url,&document,error))return false;
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
        option.fileSize=game.value(QStringLiteral("fileSize")).toString();
        option.sourceStatus=game.value(QStringLiteral("sourceStatus")).toString();
        option.releaseTitle=game.value(QStringLiteral("originalTitle")).toString();
        if(option.releaseTitle.isEmpty())option.releaseTitle=game.value(QStringLiteral("title")).toString();
        const QString identity=optionIdentity(option);
        if(seenSource.contains(identity))continue;
        seenSource.insert(identity);
        if(result.originalTitle.isEmpty())result.originalTitle=game.value(QStringLiteral("originalTitle")).toString();
        result.options.append(option);
    }
    if(out)*out=std::move(result);
    return true;
}
} // namespace xmb
