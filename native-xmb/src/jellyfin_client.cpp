#include "jellyfin_client.h"
#include "jellyfin_stream.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif
#endif

namespace xmb {
namespace {
constexpr int RequestTimeoutMs = 15000;
constexpr qsizetype MaxResponseBytes = 4 * 1024 * 1024;
// An image subtitle stream (a film's whole PGS track) is far larger than any JSON or SRT reply, and the
// server may first have to demux it out of the container, so it gets the decoder's own 64 MB input cap
// (image_subtitles.h) and a longer deadline.
constexpr qsizetype MaxImageSubtitleBytes = 64 * 1024 * 1024;
constexpr qsizetype MaxTrickplayBytes = 12 * 1024 * 1024;
constexpr int ImageSubtitleTimeoutMs = 180000;   // the server extracts every subtitle stream of the file before it answers (measured: 64 s on a 1080p remux)
constexpr int ProbeTimeoutMs = 8000;
constexpr qsizetype ProbeBytes = 2048;
constexpr qsizetype MaxImageBytes = 4 * 1024 * 1024;
constexpr int PageLimit = 200;
// Artwork is bounded on both sides of the network: the server is asked for a small picture, and
// whatever arrives is refused before decode if its declared size is unreasonable.
constexpr int PosterMaxHeight = 480;
constexpr int BackdropMaxWidth = 640;
constexpr int ImageDecodeLimit = 1024;
constexpr int ImageCacheFiles = 400;

bool validImageKey(const QString& value) {
    if (value.isEmpty() || value.size() > 128) return false;
    for (const QChar ch : value) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-') && ch != QLatin1Char('_')) return false;
    }
    return true;
}

// Bounded decode: the byte length is already capped by the caller; this refuses an absurd pixel
// count before allocating it and scales anything over the limit down before reading.
QImage decodeBoundedArtwork(const QByteArray& bytes) {
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0 ||
        size.width() > 4096 || size.height() > 4096) return {};
    if (size.width() > ImageDecodeLimit || size.height() > ImageDecodeLimit)
        reader.setScaledSize(size.scaled(ImageDecodeLimit, ImageDecodeLimit, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull() || image.width() > ImageDecodeLimit || image.height() > ImageDecodeLimit) return {};
    return image;
}

QImage decodeBoundedTrickplay(const QByteArray& bytes) {
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0 ||
        size.width() > 4096 || size.height() > 4096 || qint64(size.width()) * size.height() > 16 * 1024 * 1024) return {};
    const QImage image = reader.read();
    return image.isNull() ? QImage{} : image;
}

#ifdef Q_OS_WIN
QByteArray protectForUser(const QByteArray& plain) {
    const QByteArray entropy = QByteArrayLiteral("xmb-native-jellyfin-session-v1");
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    DATA_BLOB extra{static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy.constData()))};
    DATA_BLOB out{0, nullptr};
    if (!CryptProtectData(&in, L"XMB Jellyfin session", &extra, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

QByteArray unprotectForUser(const QByteArray& sealed) {
    const QByteArray entropy = QByteArrayLiteral("xmb-native-jellyfin-session-v1");
    DATA_BLOB in{static_cast<DWORD>(sealed.size()), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.constData()))};
    DATA_BLOB extra{static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy.constData()))};
    DATA_BLOB out{0, nullptr};
    if (!CryptUnprotectData(&in, nullptr, &extra, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}
#else
QByteArray protectForUser(const QByteArray&) { return {}; }
QByteArray unprotectForUser(const QByteArray&) { return {}; }
#endif

bool safeToken(const QString& token) {
    if (token.isEmpty() || token.size() > 4096) return false;
    for (const QChar ch : token) {
        if (ch.isNull() || ch.category() == QChar::Other_Control ||
            ch == QLatin1Char('"') || ch == QLatin1Char('\\')) return false;
    }
    return true;
}

QByteArray authValue(const QByteArray& token, const QString& deviceId) {
    QByteArray value = QByteArrayLiteral("MediaBrowser Client=\"XMB\", Device=\"XMB Native\", DeviceId=\"") +
                       deviceId.toUtf8() + QByteArrayLiteral("\", Version=\"1.0\"");
    if (!token.isEmpty()) value += QByteArrayLiteral(", Token=\"") + token + QByteArrayLiteral("\"");
    return value;
}

// The server's own transcode URL with every credential-looking key removed. The query is filtered pair by pair
// in its ENCODED form and put back untouched: decoding and re-adding values would turn %2B into a literal plus
// (a space to the server), drop the encoding of an ampersand inside a value, and so change what the server reads.
QUrl withoutSecrets(QUrl url) {
    const QString raw = url.query(QUrl::FullyEncoded);
    QStringList kept;
    for (const QString& pair : raw.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
        const QString key = QUrl::fromPercentEncoding(pair.section(QLatin1Char('='), 0, 0).toUtf8().replace('+', ' ')).toLower();
        if (key == QLatin1String("apikey") || key == QLatin1String("api_key") || key == QLatin1String("token") ||
            key == QLatin1String("access_token")) continue;
        kept.push_back(pair);
    }
    if (kept.isEmpty()) url.setQuery(QString{});
    else url.setQuery(kept.join(QLatin1Char('&')), QUrl::StrictMode);
    url.setFragment({});
    return url;
}

bool validId(const QString& value) {
    if (value.isEmpty() || value.size() > 128) return false;
    for (const QChar ch : value) {
        if (ch.isNull() || ch.category() == QChar::Other_Control ||
            ch == QLatin1Char('/') || ch == QLatin1Char('\\') ||
            ch == QLatin1Char('%') || ch == QLatin1Char('?') || ch == QLatin1Char('#')) return false;
    }
    return value != QLatin1String(".") && value != QLatin1String("..");
}

QString typeName(const QJsonObject& item) { return item.value(QStringLiteral("Type")).toString(); }

qint64 millisecondsFromTicks(const QJsonValue& value) {
    if (!value.isDouble()) return 0;
    const double ticks = value.toDouble(-1.0);
    // JSON numbers are doubles; reject non-finite, negative, and values outside
    // qint64 before converting, then truncate sub-millisecond ticks.
    if (!std::isfinite(ticks) || ticks < 0.0 ||
        ticks >= static_cast<double>(std::numeric_limits<qint64>::max())) return 0;
    return static_cast<qint64>(ticks / 10000.0);
}

// MediaSources[].Chapters or an item's Chapters: [{"StartPositionTicks": n, "Name": "..."}]. Bounded; an unnamed mark
// keeps an empty name (the UI numbers it).
QVector<JellyfinChapter> parseChapters(const QJsonArray& values) {
    QVector<JellyfinChapter> out;
    for (const auto& value : values) {
        if (!value.isObject() || out.size() >= 200) continue;
        const QJsonObject chapter = value.toObject();
        JellyfinChapter parsed;
        parsed.startMs = millisecondsFromTicks(chapter.value(QStringLiteral("StartPositionTicks")));
        parsed.name = chapter.value(QStringLiteral("Name")).toString().trimmed().left(80);
        out.push_back(parsed);
    }
    return out;
}

JellyfinItem parseItem(const QJsonObject& json) {
    JellyfinItem result;
    result.id = json.value(QStringLiteral("Id")).toString();
    result.title = json.value(QStringLiteral("Name")).toString();
    result.type = typeName(json);
    result.overview = json.value(QStringLiteral("Overview")).toString();
    result.folder = json.value(QStringLiteral("IsFolder")).toBool();
    result.collectionType = json.value(QStringLiteral("CollectionType")).toString().left(32);
    result.durationMs = millisecondsFromTicks(json.value(QStringLiteral("RunTimeTicks")));
    const QJsonObject userData = json.value(QStringLiteral("UserData")).toObject();
    result.played = userData.value(QStringLiteral("Played")).toBool(false);
    result.favorite = userData.value(QStringLiteral("IsFavorite")).toBool(false);
    result.playCount = std::clamp(userData.value(QStringLiteral("PlayCount")).toInt(0), 0, 1000000);
    const QString seriesId = json.value(QStringLiteral("SeriesId")).toString();
    if (validId(seriesId)) result.seriesId = seriesId;
    // External provider ids are optional and bounded; only the two cross-service keys the shell resolves
    // against (TMDB and IMDb) are kept, and an id must look like the provider's own (numeric for TMDB).
    const QJsonObject providerIds = json.value(QStringLiteral("ProviderIds")).toObject();
    const QString tmdbId = providerIds.value(QStringLiteral("Tmdb")).toString().trimmed();
    if (validId(tmdbId) && tmdbId.size() <= 32 && std::all_of(tmdbId.cbegin(), tmdbId.cend(), [](const QChar ch) { return ch.isDigit(); }))
        result.tmdbId = tmdbId;
    const QString imdbId = providerIds.value(QStringLiteral("Imdb")).toString().trimmed();
    if (validId(imdbId) && imdbId.size() <= 32) result.imdbId = imdbId;
    result.resumeMs = millisecondsFromTicks(userData.value(QStringLiteral("PlaybackPositionTicks")));
    if (result.played || (result.durationMs > 0 && result.resumeMs >= result.durationMs))
        result.resumeMs = 0;
    // Image tags are the server's own version stamp for the picture: they key the local cache, so
    // replacing a poster on the server replaces it here instead of replaying the old file.
    const QJsonObject tags = json.value(QStringLiteral("ImageTags")).toObject();
    const QString primary = tags.value(QStringLiteral("Primary")).toString();
    if (validImageKey(primary)) result.primaryTag = primary;
    const QJsonArray backdrops = json.value(QStringLiteral("BackdropImageTags")).toArray();
    const QString backdrop = backdrops.isEmpty() ? QString{} : backdrops.first().toString();
    if (validImageKey(backdrop)) result.backdropTag = backdrop;
    // Music: the performer, the album and the tag of the album's cover, the track's place on its disc.
    const QJsonArray artists = json.value(QStringLiteral("Artists")).toArray();
    result.artist = json.value(QStringLiteral("AlbumArtist")).toString().left(200);
    if (result.artist.isEmpty() && !artists.isEmpty()) result.artist = artists.first().toString().left(200);
    if (result.type == QLatin1String("Audio") && !artists.isEmpty()) {
        QStringList names;
        for (const auto& name : artists) { if (names.size() >= 4) break; if (!name.toString().isEmpty()) names << name.toString().left(120); }
        if (!names.isEmpty()) result.artist = names.join(QStringLiteral(", "));
    }
    result.album = json.value(QStringLiteral("Album")).toString().left(200);
    const QString albumId = json.value(QStringLiteral("AlbumId")).toString();
    if (validId(albumId)) result.albumId = albumId;
    const QString albumTag = json.value(QStringLiteral("AlbumPrimaryImageTag")).toString();
    if (validImageKey(albumTag)) result.albumTag = albumTag;
    result.trackNumber = std::clamp(json.value(QStringLiteral("IndexNumber")).toInt(0), 0, 9999);
    result.discNumber = std::clamp(json.value(QStringLiteral("ParentIndexNumber")).toInt(0), 0, 999);
    result.year = std::clamp(json.value(QStringLiteral("ProductionYear")).toInt(0), 0, 9999);
    if (result.type == QLatin1String("Episode")) {
        result.seriesName = json.value(QStringLiteral("SeriesName")).toString().left(200);
        result.seasonNumber = std::clamp(json.value(QStringLiteral("ParentIndexNumber")).toInt(0), 0, 999);
        result.episodeNumber = std::clamp(json.value(QStringLiteral("IndexNumber")).toInt(0), 0, 9999);
    }
    // The source resolution, for the 4K rule: the widest MediaSources[].Width (a multi-version item reports every
    // source, so the largest one is what "is this item 4K" asks). A few routes answer a top-level Width instead of
    // MediaSources; honour it only when no source width was seen. 0 = the server reported none (unknown, not "SD").
    result.width = 0;
    {
        const QJsonArray sources = json.value(QStringLiteral("MediaSources")).toArray();
        for (const auto& sourceValue : sources) {
            if (!sourceValue.isObject()) continue;
            const int w = sourceValue.toObject().value(QStringLiteral("Width")).toInt(0);
            if (w > 0 && w <= 20000) result.width = std::max(result.width, w);
        }
        if (result.width == 0) {
            const int w = json.value(QStringLiteral("Width")).toInt(0);
            if (w > 0 && w <= 20000) result.width = w;
        }
    }
    return result;
}

JellyfinTrickplayInfo parseTrickplayInfo(const QByteArray& bytes, const QString& mediaSourceId) {
    JellyfinTrickplayInfo best;
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    const QJsonObject allSources = root.value(QStringLiteral("Trickplay")).toObject();
    QJsonObject sourceWidths = allSources.value(mediaSourceId).toObject();
    // A source id is required: even a lone different key can describe an alternate media version.
    if (sourceWidths.isEmpty()) return best;
    QVector<QPair<int, JellyfinTrickplayInfo>> candidates;
    for (auto it = sourceWidths.constBegin(); it != sourceWidths.constEnd(); ++it) {
        bool keyOk = false;
        const int keyWidth = it.key().toInt(&keyOk);
        if (!keyOk || keyWidth < 32 || keyWidth > 2048 || !it.value().isObject()) continue;
        const QJsonObject entry = it.value().toObject();
        JellyfinTrickplayInfo info;
        info.width = entry.value(QStringLiteral("Width")).toInt(keyWidth);
        info.height = entry.value(QStringLiteral("Height")).toInt();
        info.tileWidth = entry.value(QStringLiteral("TileWidth")).toInt();
        info.tileHeight = entry.value(QStringLiteral("TileHeight")).toInt();
        info.thumbnailCount = entry.value(QStringLiteral("ThumbnailCount")).toInt();
        info.intervalMs = entry.value(QStringLiteral("Interval")).toInt();
        const qint64 capacity = qint64(info.tileWidth) * info.tileHeight;
        if (info.width < 32 || info.width > 2048 || info.height < 16 || info.height > 2048 ||
            info.tileWidth < 1 || info.tileWidth > 100 || info.tileHeight < 1 || info.tileHeight > 100 ||
            capacity > 10000 || info.thumbnailCount < 1 || info.thumbnailCount > 100000 ||
            info.intervalMs < 100 || info.intervalMs > 600000) continue;
        if (qint64(info.width)*info.tileWidth>4096 || qint64(info.height)*info.tileHeight>4096 ||
            (qint64(info.thumbnailCount)+capacity-1)/capacity>10001) continue;
        candidates.push_back({keyWidth, info});
    }
    if (candidates.isEmpty()) return best;
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        const int da = a.first >= 240 ? a.first - 240 : 10000 + (240 - a.first);
        const int db = b.first >= 240 ? b.first - 240 : 10000 + (240 - b.first);
        return da < db;
    });
    return candidates.first().second;
}
}

JellyfinClient::JellyfinClient(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)),
      deviceId_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    qRegisterMetaType<xmb::JellyfinItem>();
    qRegisterMetaType<xmb::JellyfinPage>();
    qRegisterMetaType<xmb::JellyfinPlayback>();
    qRegisterMetaType<xmb::JellyfinTrickplayInfo>();
}

JellyfinClient::~JellyfinClient() {
    destroying_=true;
    ++generation_;
    cancelRequests(true);
    accessToken_.fill('\0');
}

bool JellyfinClient::signedIn() const { return !accessToken_.isEmpty() && !userId_.isEmpty(); }
QUrl JellyfinClient::serverUrl() const { return server_; }
QByteArray JellyfinClient::authorizationHeader() const {
    return authValue(safeToken(QString::fromUtf8(accessToken_)) ? accessToken_ : QByteArray{}, deviceId_);
}
QString JellyfinClient::userId() const { return userId_; }
bool JellyfinClient::isCurrent(quint64 generation) const { return generation == generation_; }

void JellyfinClient::cancelRequests(bool includeReports) {
    QVector<QNetworkReply*> keep;
    const auto pending = replies_;
    for (auto* reply : pending) {
        if (!reply) continue;
        if (!includeReports && reportReplies_.contains(reply)) {
            keep.push_back(reply);
            continue;
        }
        reply->abort();
    }
    replies_ = std::move(keep);
    if (includeReports) reportReplies_.clear();
    flagAlias_.clear(); nextWait_.clear(); nextAlias_.clear(); homeAlias_.clear();
}

QUrl JellyfinClient::apiUrl(const QString& route) const {
    QUrl result = server_;
    QString path = result.path();
    if (path.isEmpty()) path = QStringLiteral("/");
    if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
    QString suffix = route;
    while (suffix.startsWith(QLatin1Char('/'))) suffix.remove(0, 1);
    result.setPath(path + suffix);
    result.setQuery(QString{});
    result.setFragment({});
    return result;
}

QNetworkReply* JellyfinClient::get(const QUrl& url, int transferTimeoutMs) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "XMB/1.0");
    request.setRawHeader("Authorization", authorizationHeader());
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(transferTimeoutMs);
    return network_->get(request);
}

QNetworkReply* JellyfinClient::getImage(const QUrl& url, int transferTimeoutMs) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "XMB/1.0");
    request.setRawHeader("Authorization", authorizationHeader());
    request.setRawHeader("Accept", "image/jpeg");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(transferTimeoutMs);
    return network_->get(request);
}

QNetworkReply* JellyfinClient::post(const QUrl& url, const QByteArray& body) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "XMB/1.0");
    request.setRawHeader("Authorization", authorizationHeader());
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(RequestTimeoutMs);
    return network_->post(request, body);
}

QNetworkReply* JellyfinClient::deleteResource(const QUrl& url) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "XMB/1.0");
    request.setRawHeader("Authorization", authorizationHeader());
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(RequestTimeoutMs);
    return network_->deleteResource(request);
}

quint64 JellyfinClient::send(Operation operation, QNetworkReply* reply, QString context, bool audio) {
    const quint64 request = ++nextRequest_;
    const quint64 generation = generation_;
    replies_.push_back(reply);
    if (operation == Operation::Report) reportReplies_.insert(reply);
    const qsizetype limit = operation == Operation::SubtitleImage ? MaxImageSubtitleBytes
        : operation == Operation::TrickplaySheet ? MaxTrickplayBytes : MaxResponseBytes;
    reply->setReadBufferSize(limit + 1);
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(operation == Operation::SubtitleImage || operation == Operation::Subtitle ? ImageSubtitleTimeoutMs : operation == Operation::Probe ? ProbeTimeoutMs : RequestTimeoutMs);
    connect(reply, &QNetworkReply::downloadProgress, this, [reply, limit](qint64 received, qint64) {
        if (received > limit) reply->abort();
    });
    connect(reply, &QIODevice::readyRead, this, [reply, limit] {
        if (reply->bytesAvailable() > limit) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation, request, generation,
                                                      context = std::move(context), audio]() mutable {
        finish(reply, operation, request, generation, std::move(context), audio);
    });
    return request;
}

void JellyfinClient::signIn(QUrl server, QString user, QString password) {
    // A fresh connection identity keeps a delayed old-session logout from revoking a new sign-in.
    deviceId_=QUuid::createUuid().toString(QUuid::WithoutBraces);
    ++generation_;
    cancelRequests(false);
    accessToken_.fill('\0');
    accessToken_.clear();
    userId_.clear();
    server_ = {};
    server = server.adjusted(QUrl::NormalizePathSegments);
    const bool hasQueryOrFragment = server.hasQuery() || server.hasFragment();
    while (server.path().endsWith(QLatin1Char('/')) && server.path().size() > 1)
        server.setPath(server.path().left(server.path().size() - 1));
    const auto scheme = server.scheme().toLower();
    if (!server.isValid() || server.host().isEmpty() ||
        (scheme != QLatin1String("http") && scheme != QLatin1String("https")) ||
        !server.userName().isEmpty() || !server.password().isEmpty() || hasQueryOrFragment ||
        user.trimmed().isEmpty()) {
        password.fill(QChar(0)); user.fill(QChar(0));
        fail(0, QStringLiteral("Enter a valid Jellyfin server and username."));
        return;
    }
    server.setQuery(QString{});
    server.setFragment(QString{});
    server_ = server;
    const quint64 generation = generation_;
    QJsonObject payload{{QStringLiteral("Username"), user}, {QStringLiteral("Pw"), password}};
    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    payload.insert(QStringLiteral("Pw"), QString());
    password.fill(QChar(0)); user.fill(QChar(0));
    QNetworkRequest request(apiUrl(QStringLiteral("Users/AuthenticateByName")));
    request.setRawHeader("User-Agent", "XMB/1.0");
    request.setRawHeader("Authorization", authValue({}, deviceId_));
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(RequestTimeoutMs);
    auto* reply = network_->post(request, body);
    body.fill('\0');
    const quint64 requestId = ++nextRequest_;
    replies_.push_back(reply);
    reply->setReadBufferSize(MaxResponseBytes + 1);
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(RequestTimeoutMs);
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64) {
        if (received > MaxResponseBytes) reply->abort();
    });
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > MaxResponseBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId, generation] {
        replies_.removeAll(reply);
        if (!isCurrent(generation)) { reply->deleteLater(); return; }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray bytes = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300 &&
                        bytes.size() <= MaxResponseBytes;
        reply->deleteLater();
        QJsonParseError parse{};
        const auto document = QJsonDocument::fromJson(bytes, &parse);
        const QJsonObject root = document.object();
        const QString token = root.value(QStringLiteral("AccessToken")).toString();
        const QString id = root.value(QStringLiteral("User")).toObject().value(QStringLiteral("Id")).toString();
        if (!ok || parse.error != QJsonParseError::NoError || !safeToken(token) || !validId(id)) {
            fail(0, QStringLiteral("Jellyfin sign-in failed. Check the server, username, and password."));
            return;
        }
        accessToken_ = token.toUtf8();
        userId_ = id;
        Q_UNUSED(requestId)
        emit authenticated();
    });
}

void JellyfinClient::signOut() {
    if(signedIn())logouts_.push_back({apiUrl(QStringLiteral("Sessions/Logout")),authorizationHeader()});
    ++generation_;
    cancelRequests(false);
    accessToken_.fill('\0');
    accessToken_.clear();
    userId_.clear();
    server_ = {};
    flushLogouts();
}

quint64 JellyfinClient::views() {
    if (!signedIn()) { const quint64 request = ++nextRequest_; fail(request, QStringLiteral("Sign in to Jellyfin first.")); return request; }
    QUrl url = apiUrl(QStringLiteral("UserViews"));
    QUrlQuery query;query.addQueryItem(QStringLiteral("userId"),userId_);url.setQuery(query);
    return send(Operation::Views, get(url));
}

quint64 JellyfinClient::items(QString parentId, int startIndex) {
    if (!signedIn()) { const quint64 request = ++nextRequest_; fail(request, QStringLiteral("Sign in to Jellyfin first.")); return request; }
    if (startIndex < 0 || !validId(parentId)) { const quint64 request = ++nextRequest_; fail(request, QStringLiteral("Choose a valid Jellyfin folder.")); return request; }
    QUrl url = apiUrl(QStringLiteral("Items"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ParentId"), parentId);
    query.addQueryItem(QStringLiteral("UserId"), userId_);
    query.addQueryItem(QStringLiteral("StartIndex"), QString::number(startIndex));
    query.addQueryItem(QStringLiteral("Limit"), QString::number(PageLimit));
    query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("SortName"));
    query.addQueryItem(QStringLiteral("SortOrder"), QStringLiteral("Ascending"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("Fields"), QStringLiteral("Overview,ProviderIds"));
    url.setQuery(query);
    return send(Operation::Items, get(url), QString::number(startIndex));
}

quint64 JellyfinClient::search(QString term, int startIndex) {
    if (!signedIn()) { const quint64 request = ++nextRequest_; fail(request, QStringLiteral("Sign in to Jellyfin first.")); return request; }
    term = term.trimmed();
    bool printable = !term.isEmpty() && term.size() <= 200;
    for (const QChar ch : term) if (ch.isNull() || ch.category() == QChar::Other_Control) printable = false;
    if (!printable || startIndex < 0) {
        const quint64 request = ++nextRequest_;
        fail(request, QStringLiteral("Enter a search term."));
        return request;
    }
    QUrl url = apiUrl(QStringLiteral("Items"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("UserId"), userId_);
    query.addQueryItem(QStringLiteral("SearchTerm"), term);
    query.addQueryItem(QStringLiteral("StartIndex"), QString::number(startIndex));
    query.addQueryItem(QStringLiteral("Limit"), QString::number(PageLimit));
    query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("IncludeItemTypes"),
                       QStringLiteral("Movie,Episode,Video,MusicVideo,Trailer,Recording,Series,Season"));
    query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("SortName"));
    query.addQueryItem(QStringLiteral("SortOrder"), QStringLiteral("Ascending"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    // MediaSources rides with Overview/ProviderIds so the Seerr->Jellyfin Watch resolution can read the source
    // width for its 4K rule (the same search results serve both the player's own search and the resolver).
    query.addQueryItem(QStringLiteral("Fields"), QStringLiteral("Overview,ProviderIds,MediaSources"));
    url.setQuery(query);
    return send(Operation::Search, get(url), QString::number(startIndex));
}

quint64 JellyfinClient::episodes(QString seriesId, int season) {
    if (!signedIn()) { const quint64 request = ++nextRequest_; fail(request, QStringLiteral("Sign in to Jellyfin first.")); return request; }
    if (!validId(seriesId) || season < 0) {
        const quint64 request = ++nextRequest_;
        fail(request, QStringLiteral("Choose a valid Jellyfin series."));
        return request;
    }
    QUrl url = apiUrl(QStringLiteral("Shows/") + seriesId + QStringLiteral("/Episodes"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), userId_);
    query.addQueryItem(QStringLiteral("season"), QString::number(season));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    // MediaSources rides with ProviderIds so the TV 4K rule can read the source width; the episode's SeriesId,
    // ParentIndexNumber (season) and IndexNumber are core fields, always present.
    query.addQueryItem(QStringLiteral("Fields"), QStringLiteral("ProviderIds,MediaSources"));
    url.setQuery(query);
    return send(Operation::Episodes, get(url), QString::number(season));
}

quint64 JellyfinClient::music(MusicList kind, QString libraryId, QString id, int startIndex) {
    if (!signedIn()) { const quint64 request = ++nextRequest_; fail(request, QStringLiteral("Sign in to Jellyfin first.")); return request; }
    const bool needsId = kind == MusicList::ArtistAlbums || kind == MusicList::AlbumTracks || kind == MusicList::PlaylistTracks || kind == MusicList::ArtistTracks;
    bool idOk = needsId ? validId(id) : true;
    if (kind == MusicList::Search) {
        id = id.trimmed();
        idOk = !id.isEmpty() && id.size() <= 200;
        for (const QChar ch : id) if (ch.isNull() || ch.category() == QChar::Other_Control) idOk = false;
    }
    if (startIndex < 0 || !idOk || (!libraryId.isEmpty() && !validId(libraryId))) {
        const quint64 request = ++nextRequest_;
        fail(request, QStringLiteral("Choose a valid Jellyfin music list."));
        return request;
    }
    QUrl url;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("UserId"), userId_);
    query.addQueryItem(QStringLiteral("StartIndex"), QString::number(startIndex));
    query.addQueryItem(QStringLiteral("Limit"), QString::number(kind == MusicList::Random ? 50 : PageLimit));
    query.addQueryItem(QStringLiteral("EnableImageTypes"), QStringLiteral("Primary"));
    query.addQueryItem(QStringLiteral("ImageTypeLimit"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    const auto items = [&](const QString& types, const QString& sort, const QString& order) {
        url = apiUrl(QStringLiteral("Items"));
        query.addQueryItem(QStringLiteral("IncludeItemTypes"), types);
        query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
        query.addQueryItem(QStringLiteral("SortBy"), sort);
        query.addQueryItem(QStringLiteral("SortOrder"), order);
        if (!libraryId.isEmpty()) query.addQueryItem(QStringLiteral("ParentId"), libraryId);
    };
    switch (kind) {
    case MusicList::Artists:
        url = apiUrl(QStringLiteral("Artists/AlbumArtists"));
        query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("SortName"));
        query.addQueryItem(QStringLiteral("SortOrder"), QStringLiteral("Ascending"));
        if (!libraryId.isEmpty()) query.addQueryItem(QStringLiteral("ParentId"), libraryId);
        break;
    case MusicList::Albums: items(QStringLiteral("MusicAlbum"), QStringLiteral("SortName"), QStringLiteral("Ascending")); break;
    case MusicList::Songs: items(QStringLiteral("Audio"), QStringLiteral("SortName"), QStringLiteral("Ascending")); break;
    case MusicList::Recent: items(QStringLiteral("MusicAlbum"), QStringLiteral("DateCreated,SortName"), QStringLiteral("Descending")); break;
    case MusicList::Random: items(QStringLiteral("MusicAlbum"), QStringLiteral("Random"), QStringLiteral("Ascending")); break;
    case MusicList::Playlists:
        items(QStringLiteral("Playlist"), QStringLiteral("SortName"), QStringLiteral("Ascending"));
        query.removeAllQueryItems(QStringLiteral("ParentId"));   // playlists live in their own view, not in a music library
        query.addQueryItem(QStringLiteral("MediaTypes"), QStringLiteral("Audio"));
        break;
    case MusicList::ArtistAlbums:
        items(QStringLiteral("MusicAlbum"), QStringLiteral("ProductionYear,SortName"), QStringLiteral("Ascending"));
        query.addQueryItem(QStringLiteral("ArtistIds"), id);
        break;
    case MusicList::Favorites:
        items(QStringLiteral("Audio,MusicAlbum"), QStringLiteral("SortName"), QStringLiteral("Ascending"));
        query.addQueryItem(QStringLiteral("Filters"), QStringLiteral("IsFavorite"));
        break;
    case MusicList::RecentlyPlayed:
        items(QStringLiteral("Audio"), QStringLiteral("DatePlayed"), QStringLiteral("Descending"));
        query.addQueryItem(QStringLiteral("Filters"), QStringLiteral("IsPlayed"));
        break;
    case MusicList::MostPlayed:
        items(QStringLiteral("Audio"), QStringLiteral("PlayCount"), QStringLiteral("Descending"));
        query.addQueryItem(QStringLiteral("Filters"), QStringLiteral("IsPlayed"));
        break;
    case MusicList::ArtistTracks:
        items(QStringLiteral("Audio"), QStringLiteral("Album,ParentIndexNumber,IndexNumber,SortName"), QStringLiteral("Ascending"));
        query.addQueryItem(QStringLiteral("ArtistIds"), id);
        break;
    case MusicList::AlbumTracks:
        items(QStringLiteral("Audio"), QStringLiteral("ParentIndexNumber,IndexNumber,SortName"), QStringLiteral("Ascending"));
        query.removeAllQueryItems(QStringLiteral("ParentId"));
        query.addQueryItem(QStringLiteral("ParentId"), id);
        break;
    case MusicList::PlaylistTracks:
        url = apiUrl(QStringLiteral("Playlists/") + id + QStringLiteral("/Items"));   // the playlist's own order
        break;
    case MusicList::Search:
        items(QStringLiteral("MusicArtist,MusicAlbum,Audio"), QStringLiteral("SortName"), QStringLiteral("Ascending"));
        query.addQueryItem(QStringLiteral("SearchTerm"), id);
        break;
    }
    url.setQuery(query);
    return send(Operation::Items, get(url), QString::number(startIndex));
}

quint64 JellyfinClient::playback(QString itemId, bool audio, int maxBitrateKbps, int burnSubtitleIndex, QString burnCodec,
                                 int audioStreamIndex) {
    if (!signedIn()) { const quint64 request = ++nextRequest_; fail(request, QStringLiteral("Sign in to Jellyfin first.")); return request; }
    if (!validId(itemId)) { const quint64 request = ++nextRequest_; fail(request, QStringLiteral("Choose a valid Jellyfin item.")); return request; }
    const QJsonObject videoDirect{{QStringLiteral("Container"), QStringLiteral("mp4,mkv,webm")},
                                  {QStringLiteral("Type"), QStringLiteral("Video")},
                                  {QStringLiteral("VideoCodec"), QStringLiteral("h264,hevc")},
                                  {QStringLiteral("AudioCodec"), QStringLiteral("aac,ac3,mp3,opus")}};
    const QJsonObject audioDirect{{QStringLiteral("Container"), QStringLiteral("mp3,aac,m4a,flac,ogg,oga,opus,wav,webm,mka")},
                                  {QStringLiteral("Type"), QStringLiteral("Audio")},
                                  {QStringLiteral("AudioCodec"), QStringLiteral("aac,mp3,opus,flac,vorbis,alac,pcm_s16le,pcm_s24le")}};
    const QJsonObject videoTranscode{{QStringLiteral("Container"), QStringLiteral("ts")},
        {QStringLiteral("Type"), QStringLiteral("Video")}, {QStringLiteral("VideoCodec"), QStringLiteral("h264")},
        {QStringLiteral("AudioCodec"), QStringLiteral("aac")}, {QStringLiteral("Protocol"), QStringLiteral("hls")},
        {QStringLiteral("Context"), QStringLiteral("Streaming")}, {QStringLiteral("MaxAudioChannels"), QStringLiteral("2")}};
    const QJsonObject audioTranscode{{QStringLiteral("Container"), QStringLiteral("ts")},
        {QStringLiteral("Type"), QStringLiteral("Audio")}, {QStringLiteral("AudioCodec"), QStringLiteral("aac")},
        {QStringLiteral("Protocol"), QStringLiteral("hls")}, {QStringLiteral("Context"), QStringLiteral("Streaming")}};
    const qint64 maxBitrate = qint64(std::clamp(maxBitrateKbps, 500, 200000)) * 1000;
    qInfo().noquote() << "jellyfin: PlaybackInfo with a" << maxBitrate / 1000 << "kbit/s ceiling";
    QJsonObject profile{{QStringLiteral("Name"), QStringLiteral("XMB Qt FFmpeg")},
        {QStringLiteral("MaxStreamingBitrate"), maxBitrate}, {QStringLiteral("MaxStaticBitrate"), maxBitrate},
        {QStringLiteral("MusicStreamingTranscodingBitrate"), 192000},
        {QStringLiteral("DeviceId"), deviceId_},
        {QStringLiteral("DirectPlayProfiles"), QJsonArray{audio ? audioDirect : videoDirect}},
        {QStringLiteral("TranscodingProfiles"), QJsonArray{audio ? audioTranscode : videoTranscode}}};
    // Burn-in of an image subtitle the client cannot draw (VobSub, DVB, or a PGS stream the server
    // would not deliver): the stream is selected and its only delivery method is Encode, which the
    // server can honour only in a transcode, so direct play and direct stream are switched off.
    const bool burn = !audio && burnSubtitleIndex >= 0;
    QString codec = burnCodec.trimmed().toLower();
    for (const QChar ch : codec) if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_'))) { codec.clear(); break; }
    if (burn) {
        profile.insert(QStringLiteral("SubtitleProfiles"), QJsonArray{QJsonObject{
            {QStringLiteral("Format"), codec.isEmpty() ? QStringLiteral("pgssub") : codec},
            {QStringLiteral("Method"), QStringLiteral("Encode")}}});
        qInfo().noquote() << "jellyfin: PlaybackInfo burns in subtitle stream" << burnSubtitleIndex << codec;
    }
    // An audio stream the player cannot switch to by itself is the server's to select: that cannot be a static direct
    // play, which carries every stream and leaves the choice to the player.
    const bool pickAudio = !audio && audioStreamIndex >= 0;
    QJsonObject bodyObject{{QStringLiteral("DeviceId"), deviceId_},
        {QStringLiteral("DeviceProfile"), profile},
        {QStringLiteral("UserId"), userId_}, {QStringLiteral("EnableDirectPlay"), !burn && !pickAudio},
        {QStringLiteral("EnableDirectStream"), !burn}, {QStringLiteral("EnableTranscoding"), true},
        {QStringLiteral("IsPlayback"), true}, {QStringLiteral("AutoOpenLiveStream"), true},
        {QStringLiteral("MaxStreamingBitrate"), maxBitrate},
        {QStringLiteral("StartTimeTicks"), 0}};
    if (burn) bodyObject.insert(QStringLiteral("SubtitleStreamIndex"), burnSubtitleIndex);
    if (pickAudio) bodyObject.insert(QStringLiteral("AudioStreamIndex"), audioStreamIndex);
    QByteArray body = QJsonDocument(bodyObject).toJson(QJsonDocument::Compact);
    auto* reply = post(apiUrl(QStringLiteral("Items/") + itemId + QStringLiteral("/PlaybackInfo")), body);
    body.fill('\0');
    // The context carries what the answer needs: "<item>#<burn stream>#<audio stream>" (-1 = none).
    return send(Operation::Playback, reply,
                itemId + QLatin1Char('#') + QString::number(burn ? burnSubtitleIndex : -1) + QLatin1Char('#') +
                    QString::number(pickAudio ? audioStreamIndex : -1), audio);
}

QUrl JellyfinClient::homeUrl(HomeList kind, bool legacyRoute, const QString& libraryId) const {
    QUrl url;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), userId_);
    query.addQueryItem(QStringLiteral("Limit"), kind == HomeList::Resume ? QStringLiteral("50") : kind == HomeList::Favorites ? QStringLiteral("100") :
                                                  kind == HomeList::Latest ? QStringLiteral("30") : QStringLiteral("40"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("Fields"), QStringLiteral("Overview"));
    query.addQueryItem(QStringLiteral("EnableImageTypes"), QStringLiteral("Primary,Backdrop"));
    if (kind == HomeList::Resume) {
        // Jellyfin 10.9+: /UserItems/Resume. Before that: /Users/{id}/Items/Resume (removed in 12).
        url = apiUrl(legacyRoute ? QStringLiteral("Users/") + userId_ + QStringLiteral("/Items/Resume") : QStringLiteral("UserItems/Resume"));
        query.addQueryItem(QStringLiteral("MediaTypes"), QStringLiteral("Video"));
        query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    } else if (kind == HomeList::Latest) {
        // 10.9+: /Items/Latest. Before that: /Users/{id}/Items/Latest. The answer is a bare JSON array.
        url = apiUrl(legacyRoute ? QStringLiteral("Users/") + userId_ + QStringLiteral("/Items/Latest") : QStringLiteral("Items/Latest"));
        if (validId(libraryId)) query.addQueryItem(QStringLiteral("ParentId"), libraryId);
    } else if (kind == HomeList::Favorites) {
        if (legacyRoute) return QUrl();   // /Items has the same shape on every version: nothing older to try
        url = apiUrl(QStringLiteral("Items"));
        query.removeQueryItem(QStringLiteral("userId"));
        query.addQueryItem(QStringLiteral("UserId"), userId_);
        query.addQueryItem(QStringLiteral("Filters"), QStringLiteral("IsFavorite"));
        query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
        query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Movie,Series,Episode,Video,MusicVideo"));
        query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("SortName"));
        query.addQueryItem(QStringLiteral("SortOrder"), QStringLiteral("Ascending"));
    } else {
        if (legacyRoute) return QUrl();   // /Shows/NextUp has no older route
        url = apiUrl(QStringLiteral("Shows/NextUp"));
        query.addQueryItem(QStringLiteral("DisableFirstEpisode"), QStringLiteral("false"));
    }
    url.setQuery(query);
    return url;
}

quint64 JellyfinClient::home(HomeList kind, QString libraryId) {
    if (!signedIn()) { const quint64 request = ++nextRequest_; fail(request, QStringLiteral("Sign in to Jellyfin first.")); return request; }
    if (!libraryId.isEmpty() && !validId(libraryId)) libraryId.clear();
    // "<list code>#<attempt>#<library>": '#' cannot occur in a valid id.
    return send(Operation::Home, get(homeUrl(kind, false, libraryId)),
                QString::number(int(kind)) + QLatin1String("#0#") + libraryId);
}

QUrl JellyfinClient::userFlagUrl(UserFlag flag, const QString& itemId, bool legacyRoute) const {
    const bool played = flag == UserFlag::Played;
    QUrl url;
    if (legacyRoute) {
        url = apiUrl(QStringLiteral("Users/") + userId_ + (played ? QStringLiteral("/PlayedItems/") : QStringLiteral("/FavoriteItems/")) + itemId);
        return url;
    }
    url = apiUrl((played ? QStringLiteral("UserPlayedItems/") : QStringLiteral("UserFavoriteItems/")) + itemId);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), userId_);
    url.setQuery(query);
    return url;
}

quint64 JellyfinClient::setUserFlag(QString itemId, UserFlag flag, bool value) {
    if (!signedIn() || !validId(itemId)) {
        const quint64 request = ++nextRequest_;
        QTimer::singleShot(0, this, [this, request, itemId, flag, value] { emit userFlagChanged(request, itemId, int(flag), value, false); });
        return request;
    }
    const QUrl url = userFlagUrl(flag, itemId);
    auto* reply = value ? post(url, QByteArray()) : deleteResource(url);
    return send(Operation::UserFlag, reply,
                QString::number(int(flag)) + QLatin1Char('#') + (value ? QLatin1Char('1') : QLatin1Char('0')) + QLatin1String("#0#") + itemId);
}

QUrl JellyfinClient::nextUpUrl(const QString& seriesId) const {
    QUrl url = homeUrl(HomeList::NextUp);
    QUrlQuery query(url);
    query.removeQueryItem(QStringLiteral("Limit"));
    query.addQueryItem(QStringLiteral("Limit"), QStringLiteral("3"));
    query.addQueryItem(QStringLiteral("seriesId"), seriesId);
    url.setQuery(query);
    return url;
}

quint64 JellyfinClient::nextEpisode(QString seriesId, QString currentId) {
    const quint64 request = ++nextRequest_;
    if (!signedIn() || !validId(seriesId) || !validId(currentId)) {
        QTimer::singleShot(0, this, [this, request] { emit nextEpisodeReady(request, JellyfinItem{}, false); });
        return request;
    }
    // The watched mark goes first: NextUp answers "the next unwatched episode", which is the finished one until the
    // server has been told. Its answer (flag request id) releases the NextUp request.
    const quint64 flag = setUserFlag(currentId, UserFlag::Played, true);
    nextWait_.insert(flag, NextWait{request, seriesId, currentId});
    return request;
}

void JellyfinClient::markPlayed(QString itemId) {
    if (!signedIn() || !validId(itemId)) return;
    QUrl url = apiUrl(QStringLiteral("UserPlayedItems/") + itemId);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), userId_);
    url.setQuery(query);
    auto* reply = post(url, QByteArray());
    send(Operation::Report, reply);
    // Before Jellyfin 10.9 the route was /Users/{id}/PlayedItems/{item}; a 404 retries it once, anything else is final.
    const QUrl legacy = apiUrl(QStringLiteral("Users/") + userId_ + QStringLiteral("/PlayedItems/") + itemId);
    connect(reply, &QNetworkReply::finished, this, [this, reply, legacy] {
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404 && signedIn())
            send(Operation::Report, post(legacy, QByteArray()));
    });
}

quint64 JellyfinClient::lyrics(QString itemId) {
    if (!signedIn() || !validId(itemId)) {
        const quint64 request = ++nextRequest_;
        QTimer::singleShot(0, this, [this, request, itemId] { emit lyricsReady(request, itemId, {}, false); });
        return request;
    }
    return send(Operation::Lyrics, get(apiUrl(QStringLiteral("Audio/") + itemId + QStringLiteral("/Lyrics"))), itemId);
}

quint64 JellyfinClient::chapters(QString itemId) {
    if (!signedIn() || !validId(itemId)) {
        const quint64 request = ++nextRequest_;
        QTimer::singleShot(0, this, [this, request, itemId] { emit chaptersReady(request, itemId, {}); });
        return request;
    }
    QUrl url = apiUrl(QStringLiteral("Items"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), userId_);
    query.addQueryItem(QStringLiteral("ids"), itemId);
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("Chapters"));
    url.setQuery(query);
    return send(Operation::Chapters, get(url), itemId);
}

void JellyfinClient::reportPlayback(const JellyfinPlayback& playbackInfo, qint64 positionMs,
                                    bool paused, QString event) {
    if (!signedIn() || !validId(playbackInfo.itemId) || positionMs < 0) return;
    event = event.trimmed().toLower();
    QString endpoint;
    if (event == QLatin1String("start")) endpoint = QStringLiteral("Sessions/Playing");
    else if (event == QLatin1String("progress")) endpoint = QStringLiteral("Sessions/Playing/Progress");
    else if (event == QLatin1String("stop")) endpoint = QStringLiteral("Sessions/Playing/Stopped");
    else return;
    const qint64 ticks = positionMs > std::numeric_limits<qint64>::max() / 10000
        ? std::numeric_limits<qint64>::max() : positionMs * 10000;
    QJsonObject body{{QStringLiteral("ItemId"), playbackInfo.itemId},
        {QStringLiteral("DeviceId"), deviceId_},
        {QStringLiteral("MediaSourceId"), playbackInfo.mediaSourceId},
        {QStringLiteral("PlaySessionId"), playbackInfo.playSessionId},
        {QStringLiteral("PlayMethod"), playbackInfo.playMethod},
        {QStringLiteral("PositionTicks"), ticks}, {QStringLiteral("IsPaused"), paused},
        {QStringLiteral("CanSeek"), true}, {QStringLiteral("IsMuted"), false},
        {QStringLiteral("VolumeLevel"), 100}, {QStringLiteral("PlaybackRate"), 1},
        {QStringLiteral("RepeatMode"), QStringLiteral("RepeatNone")}};
    if (playbackInfo.audioStreamIndex >= 0) body.insert(QStringLiteral("AudioStreamIndex"), playbackInfo.audioStreamIndex);
    if (playbackInfo.burnSubtitleIndex >= 0) body.insert(QStringLiteral("SubtitleStreamIndex"), playbackInfo.burnSubtitleIndex);
    QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto* reply = post(apiUrl(endpoint), bytes);
    bytes.fill('\0');
    send(Operation::Report, reply);
    if (event == QLatin1String("stop") && !playbackInfo.playSessionId.isEmpty()) {
        QUrl cleanup = apiUrl(QStringLiteral("Videos/ActiveEncodings"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("deviceId"), deviceId_);
        query.addQueryItem(QStringLiteral("playSessionId"), playbackInfo.playSessionId);
        cleanup.setQuery(query);
        send(Operation::Report, deleteResource(cleanup));
    }
}

quint64 JellyfinClient::probeStream(const QUrl& url) {
    if (!signedIn() || !trustedServerUrl(url)) {
        const quint64 request = ++nextRequest_;
        QTimer::singleShot(0, this, [this, request] {
            emit probeFinished(request, 0, QStringLiteral("not signed in, or the address is not the signed-in server"));
        });
        return request;
    }
    auto* reply = get(url, ProbeTimeoutMs);
    const quint64 request = send(Operation::Probe, reply);
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() >= ProbeBytes) reply->abort();   // a few KB are all the diagnostic needs
    });
    return request;
}

void JellyfinClient::setImageCache(QString directory) {
    imageCache_ = std::move(directory);
    if (!imageCache_.isEmpty()) QDir().mkpath(imageCache_);
}

QString JellyfinClient::cachedImagePath(const QString& itemId, const QString& tag, bool backdrop) const {
    if (imageCache_.isEmpty() || !validId(itemId)) return {};
    const QByteArray stamp = QCryptographicHash::hash(tag.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    // The server chooses ids; only plain id characters may reach a file name ("C:x" would be a drive-relative path and
    // "a:b" an NTFS stream). Anything else is replaced by a hash of the id.
    bool plain = true;
    for (const QChar ch : itemId) if (!(ch.isLetterOrNumber() && ch.unicode() < 128) && ch != QLatin1Char('-') && ch != QLatin1Char('_')) { plain = false; break; }
    const QString base = plain ? itemId
        : QString::fromLatin1(QCryptographicHash::hash(itemId.toUtf8(), QCryptographicHash::Sha1).toHex().left(24));
    const QString name = base + (backdrop ? QStringLiteral("-b-") : QStringLiteral("-p-")) +
                         QString::fromLatin1(stamp) + QStringLiteral(".img");
    return QDir(imageCache_).filePath(name);
}

void JellyfinClient::writeImageCache(const QString& path, const QByteArray& bytes) const {
    if (path.isEmpty() || bytes.isEmpty() || bytes.size() > MaxImageBytes) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        QFile::remove(path);
        return;
    }
    trimImageCache();
}

// A poster per item accumulates for as long as the library is browsed. The cap is generous and
// the oldest files go first, so the cache stays a cache rather than a second library.
void JellyfinClient::trimImageCache() const {
    if (imageCache_.isEmpty()) return;
    QDir directory(imageCache_);
    const auto files = directory.entryInfoList({QStringLiteral("*.img")}, QDir::Files, QDir::Time);
    if (files.size() <= ImageCacheFiles) return;
    for (qsizetype i = ImageCacheFiles; i < files.size(); ++i) QFile::remove(files[i].absoluteFilePath());
}

quint64 JellyfinClient::artwork(QString itemId, QString tag, bool backdrop) {
    if (!signedIn() || !validId(itemId)) {
        const quint64 request = ++nextRequest_;
        QTimer::singleShot(0, this, [this, request, itemId, backdrop] {
            emit artworkReady(request, itemId, backdrop, QImage{});
        });
        return request;
    }
    if (!validImageKey(tag)) tag.clear();
    const QString cached = cachedImagePath(itemId, tag, backdrop);
    if (!cached.isEmpty()) {
        QFile file(cached);
        if (file.exists() && file.size() > 0 && file.size() <= MaxImageBytes && file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.readAll();
            file.close();
            const QImage image = decodeBoundedArtwork(bytes);
            if (!image.isNull()) {
                const quint64 request = ++nextRequest_;
                QTimer::singleShot(0, this, [this, request, itemId, backdrop, image] {
                    emit artworkReady(request, itemId, backdrop, image);
                });
                return request;
            }
            file.close(); // a truncated or corrupt cache entry is dropped and fetched again
            QFile::remove(cached);
        }
    }
    QUrl url = apiUrl(QStringLiteral("Items/") + itemId + QStringLiteral("/Images/") +
                      (backdrop ? QStringLiteral("Backdrop") : QStringLiteral("Primary")));
    QUrlQuery query;
    if (backdrop) query.addQueryItem(QStringLiteral("maxWidth"), QString::number(BackdropMaxWidth));
    else query.addQueryItem(QStringLiteral("maxHeight"), QString::number(PosterMaxHeight));
    query.addQueryItem(QStringLiteral("quality"), QStringLiteral("80"));
    if (!tag.isEmpty()) query.addQueryItem(QStringLiteral("tag"), tag);
    url.setQuery(query);
    return send(Operation::Artwork, get(url), itemId + QLatin1Char('#') + (backdrop ? QLatin1Char('1') : QLatin1Char('0')) +
                QLatin1Char('#') + tag);
}

quint64 JellyfinClient::subtitle(QString itemId, QString mediaSourceId, int streamIndex) {
    if (!signedIn() || !validId(itemId) || !validId(mediaSourceId) || streamIndex < 0) {
        const quint64 request = ++nextRequest_;
        QTimer::singleShot(0, this, [this, request, streamIndex] { emit subtitleReady(request, streamIndex, QByteArray{}); });
        return request;
    }
    const QUrl url = apiUrl(QStringLiteral("Videos/") + itemId + QLatin1Char('/') + mediaSourceId +
                            QStringLiteral("/Subtitles/") + QString::number(streamIndex) + QStringLiteral("/Stream.srt"));
    return send(Operation::Subtitle, get(url, ImageSubtitleTimeoutMs), QString::number(streamIndex));
}

quint64 JellyfinClient::subtitleImage(QString itemId, QString mediaSourceId, int streamIndex) {
    if (!signedIn() || !validId(itemId) || !validId(mediaSourceId) || streamIndex < 0) {
        const quint64 request = ++nextRequest_;
        QTimer::singleShot(0, this, [this, request, streamIndex] { emit subtitleReady(request, streamIndex, QByteArray{}); });
        return request;
    }
    // The same route as the SRT conversion, asked for the PGS stream's own container: the server
    // extracts the stream and returns the .sup, which the client decodes itself (image_subtitles.h).
    const QUrl url = apiUrl(QStringLiteral("Videos/") + itemId + QLatin1Char('/') + mediaSourceId +
                            QStringLiteral("/Subtitles/") + QString::number(streamIndex) + QStringLiteral("/Stream.sup"));
    return send(Operation::SubtitleImage, get(url, ImageSubtitleTimeoutMs), QString::number(streamIndex));
}

quint64 JellyfinClient::trickplayInfo(QString itemId, QString mediaSourceId) {
    if (!signedIn() || !validId(itemId) || !validId(mediaSourceId)) {
        const quint64 request = ++nextRequest_;
        QTimer::singleShot(0, this, [this, request, itemId, mediaSourceId] {
            emit trickplayInfoReady(request, itemId, mediaSourceId, JellyfinTrickplayInfo{});
        });
        return request;
    }
    QUrl url = apiUrl(QStringLiteral("Items/") + itemId);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("UserId"), userId_);
    url.setQuery(query);
    return send(Operation::TrickplayInfo, get(url), itemId + QLatin1Char('#') + mediaSourceId);
}

quint64 JellyfinClient::trickplaySheet(QString itemId, QString mediaSourceId, int width, int sheetIndex) {
    if (!signedIn() || !validId(itemId) || !validId(mediaSourceId) || width < 32 || width > 2048 || sheetIndex < 0 || sheetIndex > 10000) {
        const quint64 request = ++nextRequest_;
        QTimer::singleShot(0, this, [this, request, itemId, mediaSourceId, width, sheetIndex] {
            emit trickplaySheetReady(request, itemId, mediaSourceId, width, sheetIndex, QImage{});
        });
        return request;
    }
    QUrl url = apiUrl(QStringLiteral("Videos/") + itemId + QStringLiteral("/Trickplay/") +
                      QString::number(width) + QLatin1Char('/') + QString::number(sheetIndex) + QStringLiteral(".jpg"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("MediaSourceId"), mediaSourceId);
    url.setQuery(query);
    const QString context = itemId + QLatin1Char('#') + mediaSourceId + QLatin1Char('#') +
                            QString::number(width) + QLatin1Char('#') + QString::number(sheetIndex);
    return send(Operation::TrickplaySheet, getImage(url), context);
}

void JellyfinClient::setSessionStore(QString path) { sessionPath_ = std::move(path); }

bool JellyfinClient::hasStoredSession() const {
    if (sessionPath_.isEmpty()) return false;
    const QFileInfo info(sessionPath_);
    return info.isFile() && info.size() > 9 && info.size() <= 16 * 1024;
}

bool JellyfinClient::saveSession() {
    if (sessionPath_.isEmpty() || !signedIn()) return false;
    if (accessToken_.isEmpty() || accessToken_.size() > 4096) return false;
    QJsonObject record{{QStringLiteral("v"), 1},
                       {QStringLiteral("server"), server_.toString()},
                       {QStringLiteral("userId"), userId_},
                       {QStringLiteral("token"), QString::fromUtf8(accessToken_)}};
    QByteArray plain = QJsonDocument(record).toJson(QJsonDocument::Compact);
    record.insert(QStringLiteral("token"), QString{});
    const QByteArray sealed = protectForUser(plain);
    plain.fill('\0');
    if (sealed.isEmpty()) return false;
    const QFileInfo info(sessionPath_);
    QDir().mkpath(info.absolutePath());
    QSaveFile file(sessionPath_);
    if (!file.open(QIODevice::WriteOnly)) return false;
    static const QByteArray magic = QByteArrayLiteral("XMBJF1\n");
    if (file.write(magic) != magic.size() || file.write(sealed) != sealed.size() || !file.commit()) {
        QFile::remove(sessionPath_);
        return false;
    }
    return true;
}

bool JellyfinClient::restoreSession() {
    if (sessionPath_.isEmpty() || signedIn()) return false;
    QFile file(sessionPath_);
    if (!file.exists() || file.size() <= 9 || file.size() > 16 * 1024 || !file.open(QIODevice::ReadOnly)) return false;
    QByteArray bytes = file.readAll();
    file.close();
    static const QByteArray magic = QByteArrayLiteral("XMBJF1\n");
    if (!bytes.startsWith(magic)) { forgetSession(); return false; }
    QByteArray sealed = bytes.mid(magic.size());
    bytes.fill('\0');
    const QByteArray plain = unprotectForUser(sealed);
    sealed.fill('\0');
    if (plain.isEmpty()) return false; // the record belongs to another user or machine: leave it alone
    const auto document = QJsonDocument::fromJson(plain);
    const auto record = document.object();
    const QString token = record.value(QStringLiteral("token")).toString();
    const QString id = record.value(QStringLiteral("userId")).toString();
    const QUrl server(record.value(QStringLiteral("server")).toString());
    const auto scheme = server.scheme().toLower();
    if (!safeToken(token) || !validId(id) || !server.isValid() || server.host().isEmpty() ||
        (scheme != QLatin1String("http") && scheme != QLatin1String("https")) ||
        !server.userName().isEmpty() || server.hasQuery() || server.hasFragment()) {
        forgetSession();
        return false;
    }
    deviceId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ++generation_;
    cancelRequests(false);
    accessToken_.fill('\0');
    accessToken_ = token.toUtf8();
    userId_ = id;
    server_ = server;
    return true;
}

void JellyfinClient::forgetSession() {
    if (sessionPath_.isEmpty()) return;
    QFile::remove(sessionPath_);
}

bool JellyfinClient::trustedServerUrl(const QUrl& target) const {
    if (!target.isValid() || target.scheme().toLower() != server_.scheme().toLower() ||
        target.host().compare(server_.host(), Qt::CaseInsensitive) != 0 ||
        target.port(target.scheme()=="https"?443:80) != server_.port(server_.scheme()=="https"?443:80) || !target.userName().isEmpty() || !target.password().isEmpty()) return false;
    QString prefix = server_.path();
    if (!prefix.isEmpty() && prefix != QLatin1String("/")) {
        while (prefix.endsWith(QLatin1Char('/'))) prefix.chop(1);
        if (target.path() != prefix && !target.path().startsWith(prefix + QLatin1Char('/'))) return false;
    }
    return true;
}

void JellyfinClient::finish(QNetworkReply* reply, Operation operation, quint64 request,
                            quint64 generation, QString context, bool audio) {
    replies_.removeAll(reply);
    reportReplies_.remove(reply);
    if (operation != Operation::Report && !isCurrent(generation)) { reply->deleteLater(); return; }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray bytes = reply->readAll();
    const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300 &&
                    bytes.size() <= (operation == Operation::SubtitleImage ? MaxImageSubtitleBytes :
                                     operation == Operation::TrickplaySheet ? MaxTrickplayBytes : MaxResponseBytes);
    reply->deleteLater();
    if (operation == Operation::Report) {flushLogouts();return;}
    if (operation == Operation::Probe) {
        // The digest is the server's own words (scrubbed), or the network error when nothing answered.
        const QString detail = status ? scrubDiagnostic(bytes) : scrubDiagnostic(reply->errorString().toUtf8());
        emit probeFinished(request, status, detail);
        return;
    }
    // Artwork missing or broken is not an error the viewer needs: the row keeps its built-in
    // glyph and the report is a null image.
    if (operation == Operation::Artwork) {
        const auto parts = context.split(QLatin1Char('#'));
        const QString itemId = parts.value(0);
        const bool backdrop = parts.value(1) == QLatin1String("1");
        const QString tag = parts.value(2);
        QImage image;
        if (ok && bytes.size() <= MaxImageBytes) {
            image = decodeBoundedArtwork(bytes);
            if (!image.isNull()) writeImageCache(cachedImagePath(itemId, tag, backdrop), bytes);
        }
        emit artworkReady(request, itemId, backdrop, image);
        return;
    }
    if (operation == Operation::TrickplayInfo) {
        const auto parts = context.split(QLatin1Char('#'));
        const QString itemId = parts.value(0), sourceId = parts.value(1);
        const JellyfinTrickplayInfo info = ok ? parseTrickplayInfo(bytes, sourceId) : JellyfinTrickplayInfo{};
        emit trickplayInfoReady(request, itemId, sourceId, info);
        return;
    }
    if (operation == Operation::TrickplaySheet) {
        const auto parts = context.split(QLatin1Char('#'));
        const QString itemId = parts.value(0), sourceId = parts.value(1);
        const int width = parts.value(2).toInt(), sheetIndex = parts.value(3).toInt();
        if(!ok) {
            emit trickplaySheetReady(request,itemId,sourceId,width,sheetIndex,QImage{});
            return;
        }
        auto* watcher=new QFutureWatcher<QImage>(this);
        connect(watcher,&QFutureWatcher<QImage>::finished,this,[this,watcher,request,itemId,sourceId,width,sheetIndex,generation]{
            const QImage image=watcher->result();
            watcher->deleteLater();
            if(isCurrent(generation))emit trickplaySheetReady(request,itemId,sourceId,width,sheetIndex,image);
        });
        watcher->setFuture(QtConcurrent::run([bytes]{return decodeBoundedTrickplay(bytes);}));
        return;
    }
    if (operation == Operation::Lyrics) {
        // {"Metadata":{...},"Lyrics":[{"Text":"...","Start":ticks},...]}. Synced only when every line has a Start; the lines are
        // kept in start order, at most 2000 of 300 characters, so a runaway answer cannot grow the page.
        QVector<JellyfinLyricLine> lines;
        bool synced = false;
        if (ok) {
            const QJsonArray values = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("Lyrics")).toArray();
            synced = !values.isEmpty();
            for (const auto& value : values) {
                if (!value.isObject() || lines.size() >= 2000) continue;
                const QJsonObject line = value.toObject();
                JellyfinLyricLine out;
                out.text = line.value(QStringLiteral("Text")).toString().section(QChar(10), 0, 0).trimmed().left(300);
                if (line.contains(QStringLiteral("Start"))) out.startMs = millisecondsFromTicks(line.value(QStringLiteral("Start")));
                else synced = false;
                lines.push_back(std::move(out));
            }
            if (synced) std::stable_sort(lines.begin(), lines.end(), [](const JellyfinLyricLine& a, const JellyfinLyricLine& b) { return a.startMs < b.startMs; });
        }
        emit lyricsReady(request, context, lines, synced);
        return;
    }
    if (operation == Operation::Chapters) {
        QVector<JellyfinChapter> list;
        if (ok) {
            const QJsonArray values = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("Items")).toArray();
            if (!values.isEmpty() && values.first().isObject())
                list = parseChapters(values.first().toObject().value(QStringLiteral("Chapters")).toArray());
        }
        emit chaptersReady(request, context, list);
        return;
    }
    if (operation == Operation::Home) {
        // context = "<list code>#<attempt>#<library>". The current route is tried first; a 404/405 retries the older user-scoped
        // route once (its answer is delivered under the original request id), and a server with neither answers an
        // empty list: having no Continue Watching route is not an error the viewer needs.
        const QStringList parts = context.split(QLatin1Char('#'));
        if (homeAlias_.contains(request)) request = homeAlias_.take(request);
        if (!ok && (status == 404 || status == 405 || status == 501)) {
            const int code = std::clamp(parts.value(0).toInt(), 0, 3);
            const auto kind = static_cast<HomeList>(code);
            const QUrl legacy = homeUrl(kind, true, parts.value(2));
            if (parts.value(1) == QLatin1String("0") && legacy.isValid() && !legacy.isEmpty()) {
                const quint64 retry = send(Operation::Home, get(legacy), parts.value(0) + QLatin1String("#1#") + parts.value(2));
                homeAlias_.insert(retry, request);
                return;
            }
            emit pageReady(request, JellyfinPage{});
            return;
        }
    }
    // A watched or favourite flag: 10.9+ route first, the older user-scoped route once on a 404/405/501, then the answer.
    // "<flag>#<value>#<attempt>#<item>". A next-episode lookup waiting behind a watched mark is released here.
    if (operation == Operation::UserFlag) {
        const QStringList parts = context.split(QLatin1Char('#'));
        const auto flag = parts.value(0) == QLatin1String("1") ? UserFlag::Favorite : UserFlag::Played;
        const bool value = parts.value(1) == QLatin1String("1");
        const QString item = parts.mid(3).join(QLatin1Char('#'));
        if (flagAlias_.contains(request)) request = flagAlias_.take(request);
        if (!ok && (status == 404 || status == 405 || status == 501) && parts.value(2) == QLatin1String("0") && signedIn()) {
            const QUrl legacy = userFlagUrl(flag, item, true);
            auto* retry = value ? post(legacy, QByteArray()) : deleteResource(legacy);
            const quint64 again = send(Operation::UserFlag, retry, parts.value(0) + QLatin1Char('#') + parts.value(1) + QLatin1String("#1#") + item);
            flagAlias_.insert(again, request);
            return;
        }
        if (nextWait_.contains(request)) {
            const NextWait wait = nextWait_.take(request);
            // Whether or not the mark was accepted, NextUp is asked: the stop report may have marked it already.
            const quint64 lookup = send(Operation::NextEpisode, get(nextUpUrl(wait.seriesId)), wait.currentId);
            nextAlias_.insert(lookup, wait.request);
            return;
        }
        emit userFlagChanged(request, item, int(flag), value, ok);
        return;
    }
    if (operation == Operation::NextEpisode) {
        if (nextAlias_.contains(request)) request = nextAlias_.take(request);
        JellyfinItem found;
        bool have = false;
        if (ok) {
            const QJsonDocument parsed = QJsonDocument::fromJson(bytes);
            const QJsonArray values = parsed.isArray() ? parsed.array() : parsed.object().value(QStringLiteral("Items")).toArray();
            for (const auto& value : values) {
                if (!value.isObject()) continue;
                auto item = parseItem(value.toObject());
                if (!validId(item.id) || item.id == context || item.type != QLatin1String("Episode") || item.played) continue;
                found = std::move(item);
                have = true;
                break;
            }
        }
        emit nextEpisodeReady(request, found, have);
        return;
    }

    // A subtitle stream is text, not JSON: the bytes go to the parser as they are, or empty on failure.
    if (operation == Operation::Subtitle || operation == Operation::SubtitleImage) {
        emit subtitleReady(request, context.toInt(), ok ? bytes : QByteArray{});
        return;
    }
    if (!ok) {
        // The real reason: the status and the server's own words (scrubbed, short), or the network's error when nothing answered.
        // A JSON error body ({"Message": "..."}, or ASP.NET's problem details) is read for its text; anything else is shown as it is.
        QByteArray shown = bytes;
        if (status) {
            const QJsonObject error = QJsonDocument::fromJson(bytes.left(4096)).object();
            for (const char* key : {"Message", "message", "detail", "title", "error"}) {
                const QString text = error.value(QLatin1String(key)).toString().trimmed();
                if (!text.isEmpty()) { shown = text.toUtf8(); break; }
            }
        }
        QString words = status ? scrubDiagnostic(shown, 120) : scrubDiagnostic(reply->errorString().toUtf8(), 120);
        const QString quote = words.isEmpty() ? QString{} : QStringLiteral(": \"%1\"").arg(words);
        QString text;
        if (status == 401 || status == 403) text = QStringLiteral("Jellyfin refused the request (HTTP %1%2). Sign out of Jellyfin and sign in again.").arg(status).arg(quote);
        else if (status == 404) text = QStringLiteral("Jellyfin has no such page (HTTP 404%1). The item may have been moved or removed.").arg(quote);
        else if (status >= 500) text = QStringLiteral("Jellyfin failed to answer (HTTP %1%2). Check the server's log.").arg(status).arg(quote);
        else if (status >= 400) text = QStringLiteral("Jellyfin rejected the request (HTTP %1%2).").arg(status).arg(quote);
        else if (status == 0) text = QStringLiteral("The Jellyfin server did not answer%1.").arg(quote.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(words));
        else text = QStringLiteral("Jellyfin sent a reply this build cannot read (HTTP %1, %2 bytes): the list may be too large.").arg(status).arg(bytes.size());
        qWarning().noquote() << "jellyfin: request failed:" << text;
        fail(request, text);
        return;
    }
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError) {
        fail(request, QStringLiteral("Jellyfin returned an unexpected response.")); return;
    }
    if (operation == Operation::Views || operation == Operation::Items || operation == Operation::Search || operation == Operation::Episodes || operation == Operation::Home) {
        JellyfinPage page;
        page.startIndex = operation == Operation::Items || operation == Operation::Search ? context.toInt() : 0;
        QJsonArray values;
        if (document.isArray()) values = document.array();
        else if (document.isObject()) {
            const auto root = document.object();
            values = root.value(QStringLiteral("Items")).toArray();
            page.startIndex = root.value(QStringLiteral("StartIndex")).toInt(page.startIndex);
            page.total = root.value(QStringLiteral("TotalRecordCount")).toInt(values.size());
        }
        if (page.total == 0) page.total = values.size();
        page.items.reserve(std::min<qsizetype>(values.size(), PageLimit));
        for (const auto& value : values) if (value.isObject() && page.items.size() < PageLimit) {
            auto item = parseItem(value.toObject());
            if (!validId(item.id)) continue;
            page.items.push_back(std::move(item));
        }
        emit pageReady(request, page);
        return;
    }
    if (operation == Operation::Playback) {
        // "<item>#<subtitle stream to burn in>#<audio stream asked for>" (playback()); -1 = none.
        int burn = -1, wantedAudio = -1;
        if (const int hash = context.indexOf(QLatin1Char('#')); hash > 0) {
            const QStringList parts = context.mid(hash + 1).split(QLatin1Char('#'));
            burn = parts.value(0, QStringLiteral("-1")).toInt();
            wantedAudio = parts.value(1, QStringLiteral("-1")).toInt();
            context.truncate(hash);
        }
        const auto root = document.object();
        const QString session = root.value(QStringLiteral("PlaySessionId")).toString();
        const auto sources = root.value(QStringLiteral("MediaSources")).toArray();
        for (const auto& sourceValue : sources) {
            if (!sourceValue.isObject()) continue;
            const auto source = sourceValue.toObject();
            const QString sourceId = source.value(QStringLiteral("Id")).toString();
            // A burned-in subtitle exists only in the server's transcode, so a burn request never takes
            // the static file even when the server would allow it.
            const bool directPlay = burn < 0 && wantedAudio < 0 && source.value(QStringLiteral("SupportsDirectPlay")).toBool();
            const bool directStream = burn < 0 && wantedAudio < 0 && source.value(QStringLiteral("SupportsDirectStream")).toBool();
            QString method;
            QUrl stream;
            if (directPlay) {
                method = QStringLiteral("DirectPlay");
                stream = apiUrl((audio ? QStringLiteral("Audio/") : QStringLiteral("Videos/")) + context +
                                QStringLiteral("/stream"));
                QUrlQuery query;
                query.addQueryItem(QStringLiteral("static"), QStringLiteral("true"));
                if (!sourceId.isEmpty()) query.addQueryItem(QStringLiteral("MediaSourceId"), sourceId);
                stream.setQuery(query);
            } else {
                method = directStream ? QStringLiteral("DirectStream") : QStringLiteral("Transcode");
                stream = QUrl(source.value(QStringLiteral("TranscodingUrl")).toString());
                if (stream.isEmpty() || !stream.isValid()) continue;
                if (stream.isRelative()) {
                    QUrl base = server_;
                    QString basePath = base.path();
                    if (!basePath.endsWith(QLatin1Char('/'))) basePath += QLatin1Char('/');
                    base.setPath(basePath);
                    stream = base.resolved(stream);
                }
                if (stream.isValid() && stream.scheme().compare(server_.scheme(), Qt::CaseInsensitive) == 0 &&
                    stream.host().compare(server_.host(), Qt::CaseInsensitive) == 0 &&
                    stream.port(-1) == server_.port(-1)) {
                    QString prefix = server_.path();
                    while (prefix.endsWith(QLatin1Char('/'))) prefix.chop(1);
                    if (!prefix.isEmpty() && prefix != QLatin1String("/") &&
                        stream.path() != prefix && !stream.path().startsWith(prefix + QLatin1Char('/'))) {
                        stream.setPath(prefix + (stream.path().startsWith(QLatin1Char('/'))
                            ? stream.path() : QLatin1Char('/') + stream.path()));
                    }
                }
                // The transcode URL normally carries the burn parameters itself; they are added when
                // the server left them out, so the request cannot silently fall back to no subtitle.
                if (burn >= 0) {
                    QUrlQuery query(stream);
                    if (!query.hasQueryItem(QStringLiteral("SubtitleStreamIndex")))
                        query.addQueryItem(QStringLiteral("SubtitleStreamIndex"), QString::number(burn));
                    if (!query.hasQueryItem(QStringLiteral("SubtitleMethod")))
                        query.addQueryItem(QStringLiteral("SubtitleMethod"), QStringLiteral("Encode"));
                    stream.setQuery(query);
                }
                if (wantedAudio >= 0) {
                    QUrlQuery query(stream);
                    if (!query.hasQueryItem(QStringLiteral("AudioStreamIndex")))
                        query.addQueryItem(QStringLiteral("AudioStreamIndex"), QString::number(wantedAudio));
                    stream.setQuery(query);
                }
            }
            stream = withoutSecrets(stream);
            if (!trustedServerUrl(stream)) continue;
            JellyfinPlayback result;
            result.url = stream;
            result.itemId = context;
            result.mediaSourceId = sourceId;
            result.playSessionId = session;
            result.playMethod = method;
            result.audio = audio;
            result.burnSubtitleIndex = burn;
            result.audioStreamIndex = wantedAudio;
            result.durationMs = millisecondsFromTicks(source.value(QStringLiteral("RunTimeTicks")));
            result.chapters = parseChapters(source.value(QStringLiteral("Chapters")).toArray());
            // The server's own stream list. What the player can actually switch to is decided
            // later against QMediaPlayer::audioTracks(), so this is a catalogue, not a promise.
            const QJsonArray streams = source.value(QStringLiteral("MediaStreams")).toArray();
            for (const auto& streamValue : streams) {
                if (!streamValue.isObject()) continue;
                const auto entry = streamValue.toObject();
                JellyfinTrack track;
                track.index = entry.value(QStringLiteral("Index")).toInt(-1);
                track.type = entry.value(QStringLiteral("Type")).toString();
                track.codec = entry.value(QStringLiteral("Codec")).toString();
                track.language = entry.value(QStringLiteral("Language")).toString();
                track.channels = entry.value(QStringLiteral("Channels")).toInt(0);
                track.external = entry.value(QStringLiteral("IsExternal")).toBool(false);
                track.isDefault = entry.value(QStringLiteral("IsDefault")).toBool(false);
                track.text = entry.value(QStringLiteral("IsTextSubtitleStream")).toBool(false);
                track.title = entry.value(QStringLiteral("DisplayTitle")).toString().trimmed();
                if (track.title.isEmpty()) track.title = track.language;
                if (track.title.isEmpty()) track.title = track.codec;
                if (track.type.isEmpty() || track.title.isEmpty()) continue;
                result.streams.push_back(track);
            }
            emit playbackReady(request, result);
            return;
        }
        fail(request, QStringLiteral("Jellyfin did not provide a compatible stream."));
    }
}

void JellyfinClient::flushLogouts() {
    if(destroying_ || !reportReplies_.empty() || logouts_.empty())return;
    const auto logout=logouts_.takeFirst();
    QNetworkRequest request(logout.url);request.setRawHeader("Authorization",logout.authorization);
    request.setHeader(QNetworkRequest::ContentTypeHeader,QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(RequestTimeoutMs);
    send(Operation::Report,network_->post(request,QByteArray("{}")));
}

void JellyfinClient::fail(quint64 request, const QString& message) {
    if (request == 0) { emit failed(request, message); return; }
    const quint64 generation = generation_;
    QTimer::singleShot(0, this, [this, request, message, generation] {
        if (isCurrent(generation)) emit failed(request, message);
    });
}

} // namespace xmb

