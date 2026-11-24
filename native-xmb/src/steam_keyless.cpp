#include "steam_keyless.h"

#include "steam_activity.h"
#include "steam_appinfo.h"
#include "steam_collections.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <algorithm>

namespace xmb {
namespace {

constexpr int MaximumCandidates = 20000;   // ids looked up in appinfo before the type filter
constexpr int MaximumCacheEntries = 8;     // accounts kept

bool isAppId(const QString& text) {
    if (text.isEmpty() || text.size() > 10) return false;
    for (const QChar c : text)
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
    bool ok = false;
    const qulonglong value = text.toULongLong(&ok);
    return ok && value != 0 && value <= 0xffffffffULL;
}

// A cheap fingerprint of one input file: size and modification time (missing files fingerprint as "-").
QString fingerprint(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists()) return QStringLiteral("-");
    return QStringLiteral("%1@%2").arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
}

struct CacheEntry {
    QString signature;
    SteamKeylessLibrary library;
};
QMutex cacheMutex;
QHash<QString, CacheEntry>& cache() {
    static QHash<QString, CacheEntry> entries;
    return entries;
}

QString headerUrlFor(const QString& appId) {
    return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/header.jpg").arg(appId);
}

struct Candidate {
    bool played = false;
    bool collected = false;
    bool cached = false;
    qint64 lastPlayed = 0;
    int playtime = 0;
};

} // namespace

QString steamKeylessSourceText(const QString& persona) {
    const QString name = persona.trimmed().isEmpty() ? QObject::tr("this account") : persona.trimmed();
    return QObject::tr("From this PC's Steam data for %1: games you have played or collected. "
                       "Add a Steam Web API key in Settings for the full list.").arg(name);
}

void clearSteamKeylessCache() {
    const QMutexLocker lock(&cacheMutex);
    cache().clear();
}

SteamKeylessLibrary scanSteamKeylessLibrary(const QString& steamRoot, const QString& steamId64,
                                            const QHash<QString, QString>& installedTitles) {
    SteamKeylessLibrary library;
    library.steamId64 = steamId64.trimmed();
    QString account;
    // The switch deterministic captures use to turn every live Steam record off turns this one off too.
    if (qEnvironmentVariable("XMB_STEAM_ACTIVITY") == QLatin1String("0")) return library;
    if (steamRoot.trimmed().isEmpty() || !steamAccountId(library.steamId64, &account)) return library;

    const QDir root(steamRoot);
    const QString config = root.filePath(QStringLiteral("userdata/%1/config").arg(account));
    if (!QFileInfo(config).isDir()) {
        library.warnings.append(QStringLiteral("No local Steam data for this account on this PC."));
        return library;
    }
    const QString localConfig = QDir(config).filePath(QStringLiteral("localconfig.vdf"));
    const QString cloud = QDir(config).filePath(QStringLiteral("cloudstorage/cloud-storage-namespace-1.json"));
    const QString libraryCache = QDir(config).filePath(QStringLiteral("librarycache"));
    const QString appInfoPath = root.filePath(QStringLiteral("appcache/appinfo.vdf"));

    QStringList installedKeys;
    for (auto it = installedTitles.cbegin(); it != installedTitles.cend(); ++it) installedKeys.append(it.key() + it.value());
    installedKeys.sort();
    const QString signature = fingerprint(localConfig) + QLatin1Char('|') + fingerprint(cloud) + QLatin1Char('|') +
        fingerprint(libraryCache) + QLatin1Char('|') + fingerprint(appInfoPath) + QLatin1Char('|') +
        QString::number(qHash(installedKeys.join(QLatin1Char('\n')))) + QLatin1Char('|') + QDir::cleanPath(steamRoot);
    {
        const QMutexLocker lock(&cacheMutex);
        const auto found = cache().constFind(library.steamId64);
        if (found != cache().constEnd() && found->signature == signature) {
            SteamKeylessLibrary hit = found->library;
            hit.fromCache = true;
            return hit;
        }
    }

    QHash<QString, Candidate> candidates;

    // 1. Play records.
    const auto activity = scanSteamActivity(steamRoot, library.steamId64);
    for (auto it = activity.cbegin(); it != activity.cend(); ++it) {
        if (!isAppId(it.key())) continue;
        if (it->playtimeMinutes <= 0 && it->lastPlayed <= 86400) continue;   // no play evidence: noise
        Candidate& candidate = candidates[it.key()];
        candidate.played = true;
        candidate.lastPlayed = it->lastPlayed;
        candidate.playtime = it->playtimeMinutes;
    }
    // 2. Static collections.
    QString collectionsError;
    for (const auto& collection : scanSteamCollectionsIn(steamRoot, library.steamId64, &collectionsError))
        for (const auto& appId : collection.appIds)
            if (isAppId(appId)) candidates[appId].collected = true;
    // 3. The client's library cache: <appid>.json, one per app the library showed this account.
    const auto cacheFiles = QDir(libraryCache).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::Readable | QDir::NoSymLinks);
    for (const auto& file : cacheFiles) {
        const QString id = file.completeBaseName();
        if (isAppId(id)) candidates[id].cached = true;
    }
    if (candidates.isEmpty()) {
        const QMutexLocker lock(&cacheMutex);
        cache().insert(library.steamId64, {signature, library});
        return library;
    }

    // Bound the appinfo lookup: when there are more candidates than the limit, the most recently played go first.
    QStringList ids = candidates.keys();
    std::sort(ids.begin(), ids.end(), [&](const QString& left, const QString& right) {
        const Candidate& a = candidates[left];
        const Candidate& b = candidates[right];
        if (a.played != b.played) return a.played;
        if (a.lastPlayed != b.lastPlayed) return a.lastPlayed > b.lastPlayed;
        return left.toULongLong() < right.toULongLong();
    });
    if (ids.size() > MaximumCandidates) {
        ids = ids.mid(0, MaximumCandidates);
        library.truncated = true;
    }

    QString appInfoError;
    const auto info = readSteamAppInfo(appInfoPath, QSet<QString>(ids.cbegin(), ids.cend()), &appInfoError);
    if (!appInfoError.isEmpty()) library.warnings.append(appInfoError);

    struct Row { OwnedGame game; const Candidate* candidate; };
    QVector<Row> rows;
    for (const QString& id : ids) {
        QString title;
        const auto entry = info.constFind(id);
        const bool typed = entry != info.constEnd() && !entry->type.isEmpty();
        if (typed && !steamAppTypeIsGame(entry->type)) continue;   // tools, servers, DLC, music, demos, ...
        if (entry != info.constEnd()) title = entry->title;
        if (title.isEmpty()) title = installedTitles.value(id).trimmed();
        if (title.isEmpty()) continue;                             // never list an unnamed id
        if (!typed && !installedTitles.contains(id)) continue;     // unknown type: only when the PC names it
        OwnedGame game;
        game.appId = id;
        game.title = title;
        game.headerUrl = headerUrlFor(id);
        game.playtimeMinutes = std::max(0, candidates[id].playtime);
        game.installed = installedTitles.contains(id);
        rows.push_back({game, &candidates[id]});
    }

    if (rows.size() > kSteamKeylessMaximumGames) {
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.candidate->played != b.candidate->played) return a.candidate->played;
            if (a.candidate->lastPlayed != b.candidate->lastPlayed) return a.candidate->lastPlayed > b.candidate->lastPlayed;
            return a.game.appId.toULongLong() < b.game.appId.toULongLong();
        });
        rows.resize(kSteamKeylessMaximumGames);
        library.truncated = true;
    }
    for (const auto& row : rows) {
        if (row.candidate->played) ++library.playedCount;
        if (row.candidate->collected) ++library.collectedCount;
        if (row.candidate->cached) ++library.libraryCacheCount;
        library.games.push_back(row.game);
    }
    std::sort(library.games.begin(), library.games.end(), [](const OwnedGame& a, const OwnedGame& b) {
        const int order = a.title.localeAwareCompare(b.title);
        return order == 0 ? a.appId.toULongLong() < b.appId.toULongLong() : order < 0;
    });

    const QMutexLocker lock(&cacheMutex);
    if (cache().size() >= MaximumCacheEntries && !cache().contains(library.steamId64)) cache().clear();
    cache().insert(library.steamId64, {signature, library});
    return library;
}

} // namespace xmb
