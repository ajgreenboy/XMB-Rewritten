#include "imported_art.h"
#include "imported_art_match.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

namespace xmb {
namespace {

constexpr int SearchTimeoutMs = 12000;
constexpr int ImageTimeoutMs = 8000;
constexpr qsizetype MaxSearchBytes = 1024 * 1024;
constexpr qsizetype MaxImageBytes = 5 * 1024 * 1024;
constexpr int MaxRedirects = 3;
constexpr int MaxLookupsPerRun = 48;      // titles looked up per library scan; the rest wait for the next scan
constexpr int TitleIntervalMs = 400;      // one title at a time, never a burst
constexpr qint64 FailureBackoffSeconds = 60 * 60;          // store unreachable: one attempt per hour at most
constexpr qint64 NoMatchRetrySeconds = 7 * 24 * 60 * 60;   // a confident "no match" is not asked again for a week
constexpr qint64 DetailsRetrySeconds = 24 * 60 * 60;       // matched, but no header image anywhere: look again tomorrow
constexpr int MaxImageDimension = 8192;
constexpr quint64 MaxImagePixels = 64ULL * 1024ULL * 1024ULL;

struct Stored {
    bool ok = false;
    QImage preview;
};

// Verify the bytes decode, then persist them unchanged (a truncated download never becomes a cache entry).
Stored storeImage(const QByteArray& bytes, const QString& path, bool wantPreview) {
    Stored result;
    if (bytes.isEmpty() || bytes.size() > MaxImageBytes || path.isEmpty()) return result;
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return result;
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > MaxImageDimension || size.height() > MaxImageDimension ||
        quint64(size.width()) * quint64(size.height()) > MaxImagePixels) return result;
    const QSize limit = wantPreview ? QSize(512, 288) : QSize(3840, 2160);
    if (size.width() > limit.width() || size.height() > limit.height())
        reader.setScaledSize(size.scaled(limit, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull()) return result;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return result;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return result;
    result.ok = true;
    if (wantPreview) result.preview = image;
    return result;
}

QImage readPreview(const QString& path) {
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > MaxImageDimension || size.height() > MaxImageDimension) return {};
    reader.setScaledSize(size.scaled(QSize(512, 288), Qt::KeepAspectRatio));
    return reader.read();
}

} // namespace

ImportedArt::ImportedArt(QString cacheRoot, QObject* parent, QNetworkAccessManager* network)
    : QObject(parent), cacheRoot_(std::move(cacheRoot)),
      network_(network ? network : new QNetworkAccessManager(this)), timer_(new QTimer(this)) {
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &ImportedArt::pump);
}

ImportedArt::~ImportedArt() {
    ++generation_;
    if (reply_) {
        auto* reply = reply_;
        reply_ = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

bool ImportedArt::networkEnabled() {
    return qEnvironmentVariable("XMB_STEAM_ARTWORK") != QLatin1String("0") &&
           qEnvironmentVariable("XMB_IMPORTED_ART") != QLatin1String("0");
}

void ImportedArt::refresh(const QVector<Target>& targets) {
    pending_.clear();
    needsNetwork_.clear();
    lookupsThisRun_ = 0;
    QSet<QString> seen;
    bool keepActive = false;
    for (const Target& target : targets) {
        if (target.first.isEmpty() || target.second.trimmed().isEmpty() || seen.contains(target.first)) continue;
        seen.insert(target.first);
        if (target == active_) keepActive = true;
        else pending_.append(target);
    }
    if (!keepActive && !active_.first.isEmpty()) {
        ++generation_;
        active_ = {};
        if (reply_) {
            auto* reply = reply_;
            reply_ = nullptr;
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }
    if (!keepActive && !pending_.isEmpty()) timer_->start(0);
}

void ImportedArt::resetBackoff() {
    networkRetryAfter_ = 0;
    retryAfter_.clear();
    heroTried_.clear();
    lastStatus_.clear();
    force_ = true;
    if (active_.first.isEmpty()) timer_->start(0);
}

void ImportedArt::pump() {
    if (!active_.first.isEmpty()) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QElapsedTimer slice;
    slice.start();
    while (!pending_.isEmpty()) {
        if (slice.elapsed() >= 3) {
            timer_->start(1);
            return;
        }
        const Target target = pending_.takeFirst();
        if (retryAfter_.value(target) > now) continue;
        const auto meta = importedArt::loadMeta(cacheRoot_, target.second);
        if (!meta.key.isEmpty() && !meta.noMatch) {
            const QString header = importedArt::cachedHeader(cacheRoot_, target.second);
            if (header.isEmpty()) {
                needsNetwork_.append(target);   // matched earlier, image missing: download it, no new search
                continue;
            }
            const QString hero = importedArt::cachedHero(cacheRoot_, target.second);
            if (hero.isEmpty() && !meta.heroMissing && !heroTried_.contains(meta.appId)) needsNetwork_.append(target);
            if (!delivered_.contains(target)) {
                deliver(target, header, hero);
                return;
            }
            continue;
        }
        if (meta.noMatch && !force_ && meta.retryAfter > now) continue;
        needsNetwork_.append(target);
    }
    force_ = false;
    if (!networkEnabled()) {
        needsNetwork_.clear();
        return;
    }
    if (needsNetwork_.isEmpty()) return;
    if (networkRetryAfter_ > now) {
        timer_->start(int(qMin<qint64>(FailureBackoffSeconds, networkRetryAfter_ - now)) * 1000);
        return;
    }
    if (lookupsThisRun_ >= MaxLookupsPerRun) return;
    startNetwork(needsNetwork_.takeFirst());
}

void ImportedArt::deliver(const Target& target, const QString& headerPath, const QString& heroPath) {
    active_ = target;
    const quint64 generation = generation_;
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, generation, target, headerPath, heroPath] {
        const QImage preview = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        active_ = {};
        timer_->start(0);
        if (preview.isNull()) {
            needsNetwork_.append(target);
            return;
        }
        delivered_.insert(target);
        emit ready(target.first, target.second, headerPath, heroPath, preview);
    });
    watcher->setFuture(QtConcurrent::run([headerPath] { return readPreview(headerPath); }));
}

void ImportedArt::startNetwork(const Target& target) {
    active_ = target;
    ++lookupsThisRun_;
    const auto meta = importedArt::loadMeta(cacheRoot_, target.second);
    if (!meta.key.isEmpty() && !meta.noMatch) {
        if (importedArt::cachedHeader(cacheRoot_, target.second).isEmpty()) downloadHeader(target, meta.appId);
        else downloadHero(target, meta.appId);
        return;
    }
    search(target);
}

void ImportedArt::finishTarget(bool failed) {
    const Target target = active_;
    active_ = {};
    if (failed && !target.first.isEmpty()) {
        networkRetryAfter_ = QDateTime::currentSecsSinceEpoch() + FailureBackoffSeconds;
        needsNetwork_.prepend(target);
    }
    timer_->start(TitleIntervalMs);
}

void ImportedArt::search(const Target& target) {
    const quint64 generation = generation_;
    get(importedArt::searchUrl(target.second), false, [this, generation, target](QByteArray bytes, int status, int error) {
        if (generation != generation_) return;
        QJsonParseError parseError{};
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (error != int(QNetworkReply::NoError) || status < 200 || status >= 300 ||
            parseError.error != QJsonParseError::NoError || !document.isObject() ||
            !document.object().contains(QStringLiteral("items"))) {
            qInfo().noquote() << QStringLiteral("artwork: %1 store search failed (http %2, error %3); retry later")
                .arg(target.second).arg(status).arg(error);
            report(tr("Steam store search unreachable; imported game artwork will retry later."));
            finishTarget(true);
            return;
        }
        const auto match = importedArt::selectMatch(target.second, importedArt::parseStoreSearch(bytes));
        importedArt::Meta meta;
        meta.title = target.second;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (!match.ok) {
            meta.noMatch = true;
            meta.retryAfter = now + NoMatchRetrySeconds;
            meta.reason = match.reason;
            importedArt::saveMeta(cacheRoot_, meta);
            qInfo().noquote() << QStringLiteral("artwork: %1 no confident match (%2)").arg(target.second, match.reason);
            finishTarget(false);
            return;
        }
        meta.appId = match.appId;
        meta.name = match.name;
        meta.reason = match.reason;
        meta.matchedAt = now;
        importedArt::saveMeta(cacheRoot_, meta);
        qInfo().noquote() << QStringLiteral("artwork: %1 -> steam app %2 (%3)").arg(target.second, match.appId, match.reason);
        downloadHeader(target, match.appId);
    });
}

void ImportedArt::headerUnavailable(const Target& target, const QString& appId) {
    importedArt::Meta meta;
    meta.title = target.second;
    meta.noMatch = true;
    meta.retryAfter = QDateTime::currentSecsSinceEpoch() + DetailsRetrySeconds;
    meta.reason = QStringLiteral("steam app %1 has no header image").arg(appId);
    importedArt::saveMeta(cacheRoot_, meta);
    qInfo().noquote() << QStringLiteral("artwork: %1 no confident match (%2)").arg(target.second, meta.reason);
    finishTarget(false);
}

void ImportedArt::headerFromDetails(const Target& target, const QString& appId) {
    const quint64 generation = generation_;
    QUrl url(QStringLiteral("https://store.steampowered.com/api/appdetails"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("appids"), appId);
    query.addQueryItem(QStringLiteral("filters"), QStringLiteral("basic"));
    url.setQuery(query);
    get(url, false, [this, generation, target, appId](QByteArray bytes, int status, int error) {
        if (generation != generation_) return;
        if (error != int(QNetworkReply::NoError) || status < 200 || status >= 300) {
            finishTarget(true);
            return;
        }
        const QJsonObject data = QJsonDocument::fromJson(bytes).object().value(appId).toObject()
            .value(QStringLiteral("data")).toObject();
        const QUrl header(data.value(QStringLiteral("header_image")).toString());
        if (!importedArt::allowedImageUrl(header)) {
            headerUnavailable(target, appId);
            return;
        }
        downloadHeader(target, appId, header, true);
    });
}

void ImportedArt::downloadHeader(const Target& target, const QString& appId, const QUrl& url, bool viaDetails) {
    const quint64 generation = generation_;
    get(url.isEmpty() ? importedArt::assetUrl(appId, importedArt::Asset::Header) : url, true,
        [this, generation, target, appId, viaDetails](QByteArray bytes, int status, int error) {
        if (generation != generation_) return;
        if (status == 404) {
            if (viaDetails) headerUnavailable(target, appId);
            else headerFromDetails(target, appId);
            return;
        }
        if (error != int(QNetworkReply::NoError) || status < 200 || status >= 300 || bytes.isEmpty()) {
            report(tr("Steam artwork download failed; imported game artwork will retry later."));
            finishTarget(true);
            return;
        }
        const QString path = importedArt::assetPath(cacheRoot_, appId, importedArt::Asset::Header);
        auto* watcher = new QFutureWatcher<Stored>(this);
        connect(watcher, &QFutureWatcher<Stored>::finished, this, [this, watcher, generation, target, appId, path] {
            const Stored stored = watcher->result();
            watcher->deleteLater();
            if (generation != generation_) return;
            if (!stored.ok) {
                finishTarget(true);
                return;
            }
            delivered_.insert(target);
            emit ready(target.first, target.second, path,
                       importedArt::cachedHero(cacheRoot_, target.second), stored.preview);
            downloadHero(target, appId);
        });
        watcher->setFuture(QtConcurrent::run([bytes, path] { return storeImage(bytes, path, true); }));
    });
}

void ImportedArt::downloadHero(const Target& target, const QString& appId) {
    auto meta = importedArt::loadMeta(cacheRoot_, target.second);
    if (meta.key.isEmpty() || meta.noMatch || meta.heroMissing || heroTried_.contains(appId) ||
        !importedArt::cachedHero(cacheRoot_, target.second).isEmpty()) {
        finishTarget(false);
        return;
    }
    heroTried_.insert(appId);   // once per session: a flaky CDN never turns into a retry loop
    const quint64 generation = generation_;
    get(importedArt::assetUrl(appId, importedArt::Asset::Hero), true,
        [this, generation, target, appId, meta](QByteArray bytes, int status, int error) mutable {
        if (generation != generation_) return;
        if (status == 404) {
            meta.heroMissing = true;
            importedArt::saveMeta(cacheRoot_, meta);
            finishTarget(false);
            return;
        }
        if (error != int(QNetworkReply::NoError) || status < 200 || status >= 300 || bytes.isEmpty()) {
            finishTarget(false);
            return;
        }
        const QString path = importedArt::assetPath(cacheRoot_, appId, importedArt::Asset::Hero);
        auto* watcher = new QFutureWatcher<Stored>(this);
        connect(watcher, &QFutureWatcher<Stored>::finished, this, [this, watcher, generation, target, path] {
            const Stored stored = watcher->result();
            watcher->deleteLater();
            if (generation != generation_) return;
            if (stored.ok) emit heroReady(target.first, path);
            finishTarget(false);
        });
        watcher->setFuture(QtConcurrent::run([bytes, path] { return storeImage(bytes, path, false); }));
    });
}

void ImportedArt::get(const QUrl& url, bool image, std::function<void(QByteArray, int, int)> done, int redirects) {
    if (image ? !importedArt::allowedImageUrl(url) : !importedArt::allowedSearchUrl(url)) {
        done({}, 0, int(QNetworkReply::ProtocolFailure));
        return;
    }
    const qsizetype maximumBytes = image ? MaxImageBytes : MaxSearchBytes;
    QNetworkRequest request(url);
    request.setTransferTimeout(image ? ImageTimeoutMs : SearchTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("xmb-native/1.0"));
    request.setRawHeader("Accept", image ? "image/*" : "application/json");
    QNetworkReply* reply = network_->get(request);
    reply_ = reply;
    reply->setReadBufferSize(maximumBytes + 1);
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes, maximumBytes] {
        const qint64 remaining = qint64(maximumBytes) + 1 - bytes->size();
        if (remaining <= 0) {
            reply->abort();
            return;
        }
        bytes->append(reply->read(remaining));
        if (bytes->size() > maximumBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, bytes, url, image, redirects, maximumBytes, done = std::move(done)]() mutable {
        if (reply_ == reply) reply_ = nullptr;
        const QByteArray tail = reply->readAll();
        const bool tooLarge = bytes->size() > maximumBytes || tail.size() > qint64(maximumBytes) - bytes->size();
        if (!tooLarge) bytes->append(tail);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const int error = int(reply->error());
        const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        reply->deleteLater();
        if (tooLarge) {
            done({}, status, int(QNetworkReply::UnknownNetworkError));
            return;
        }
        if (redirect.isValid() && !redirect.isEmpty()) {
            const QUrl target = url.resolved(redirect);
            if (redirects >= MaxRedirects) {
                done({}, status, int(QNetworkReply::ProtocolFailure));
                return;
            }
            get(target, image, std::move(done), redirects + 1);
            return;
        }
        done(*bytes, status, error);
    });
}

void ImportedArt::report(const QString& message) {
    if (lastStatus_ == message) return;
    lastStatus_ = message;
    emit status(message);
}

} // namespace xmb
