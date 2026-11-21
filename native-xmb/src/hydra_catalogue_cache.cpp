#include "hydra_catalogue_cache.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace xmb {
namespace {
constexpr int kSchemaVersion = 2;             // genres + version field (v1 was title/coverUrl only)
constexpr qint64 kDefaultTtlMs = 5 * 60 * 1000;   // five-minute freshness window
constexpr qint64 kMaxBytes = 256 * 1024;          // 256 KiB ceiling on the serialized entry
const QString kVersionKey = QStringLiteral("version");
const QString kEndpointKey = QStringLiteral("endpoint");
const QString kStoredAtKey = QStringLiteral("storedAtMs");
const QString kTitlesKey = QStringLiteral("titles");
const QString kGenresKey = QStringLiteral("genres");

// A short, stable, path-safe filename key for an endpoint; the endpoint itself is also stored in
// the file and re-checked on load, so the hash only exists to keep the name clean.
QString cacheKey(const QString& endpoint) {
    return QString::fromLatin1(
        QCryptographicHash::hash(endpoint.toUtf8(), QCryptographicHash::Sha1).toHex());
}
} // namespace

HydraCatalogueCache::HydraCatalogueCache(const QString& directory) : directory_(directory) {}

qint64 HydraCatalogueCache::ttlMs() { return kDefaultTtlMs; }
qint64 HydraCatalogueCache::maxBytes() { return kMaxBytes; }
int HydraCatalogueCache::schemaVersion() { return kSchemaVersion; }

QString HydraCatalogueCache::pathFor(const QString& endpoint) const {
    if (directory_.isEmpty()) return QString();
    return QDir(directory_).filePath(QStringLiteral("hydra-catalogue-") + cacheKey(endpoint) +
                                     QStringLiteral(".json"));
}

bool HydraCatalogueCache::load(const QString& endpoint, QVector<PopularTitle>* out, qint64 nowMs) const {
    if (out) out->clear();
    const QString path = pathFor(endpoint);
    if (path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    if (file.size() > maxBytes()) {   // bounded storage: an oversized entry is a miss, not an error
        file.close();
        QFile::remove(path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (document.isNull() || !document.isObject()) return false;   // corruption -> miss

    const QJsonObject root = document.object();
    if (root.value(kVersionKey).toInt() != schemaVersion()) return false;   // old/unknown schema -> miss
    if (root.value(kEndpointKey).toString() != endpoint) return false;      // source identity

    const qint64 storedAtMs = qint64(root.value(kStoredAtKey).toDouble());
    const qint64 now = nowMs != 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
    if (storedAtMs <= 0 || now - storedAtMs < 0 || now - storedAtMs > ttlMs()) return false; // stale/skewed

    QVector<PopularTitle> titles;
    const QJsonArray array = root.value(kTitlesKey).toArray();
    titles.reserve(array.size());
    for (const auto& value : array) {
        if (!value.isObject()) return false;
        const QJsonObject entry = value.toObject();
        const QString title = entry.value(QStringLiteral("title")).toString();
        if (title.isEmpty()) return false;   // an untitled member invalidates the whole entry
        PopularTitle item;
        item.title = title;
        item.coverUrl = entry.value(QStringLiteral("coverUrl")).toString();
        const QJsonValue genresValue = entry.value(kGenresKey);
        if (!genresValue.isUndefined() && !genresValue.isArray()) return false;   // corrupt schema
        if (genresValue.isArray()) {
            for (const auto& g : genresValue.toArray()) {
                if (!g.isString()) return false;   // a non-string genre member invalidates the entry
                item.genres.append(g.toString());  // already normalized at fetch time; kept verbatim
            }
        }
        titles.append(std::move(item));
    }
    if (titles.isEmpty()) return false;   // never serve an empty catalogue from cache
    if (out) *out = std::move(titles);
    return true;
}

bool HydraCatalogueCache::store(const QString& endpoint, const QVector<PopularTitle>& titles, qint64 nowMs) const {
    if (directory_.isEmpty() || titles.isEmpty()) return false;   // never persist an empty result

    QJsonArray array;
    for (const auto& item : titles) {
        QJsonObject entry;
        entry.insert(QStringLiteral("title"), item.title);
        entry.insert(QStringLiteral("coverUrl"), item.coverUrl);
        QJsonArray genres;
        for (const auto& g : item.genres) genres.append(g);
        entry.insert(kGenresKey, genres);
        array.append(entry);
    }
    QJsonObject root;
    root.insert(kVersionKey, schemaVersion());
    root.insert(kEndpointKey, endpoint);
    root.insert(kStoredAtKey, double(nowMs != 0 ? nowMs : QDateTime::currentMSecsSinceEpoch()));
    root.insert(kTitlesKey, array);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (qint64(bytes.size()) > maxBytes()) return false;   // bounded storage

    if (!QDir().mkpath(directory_)) return false;
    QSaveFile file(pathFor(endpoint));
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(bytes) != bytes.size()) { file.cancelWriting(); return false; }
    if (!file.commit()) return false;
    return true;
}
} // namespace xmb
