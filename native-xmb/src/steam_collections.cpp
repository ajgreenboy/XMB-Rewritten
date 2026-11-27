#include "steam_collections.h"

#include "steam_library.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

namespace xmb {
namespace {
constexpr qint64 MaxFileBytes = 8 * 1024 * 1024;
constexpr int MaxCollections = 512;
constexpr int MaxAppsPerCollection = 20000;
constexpr int MaxNameLength = 120;

// SteamID64 is a 64-bit value whose low 32 bits are the account id.
constexpr qint64 SteamId64Base = 76561197960265728LL;
} // namespace

bool steamAccountId(const QString& steamId64, QString* accountId) {
    bool ok = false;
    const qint64 value = steamId64.trimmed().toLongLong(&ok);
    if (!ok || value <= SteamId64Base) return false;
    if (accountId) *accountId = QString::number(value - SteamId64Base);
    return true;
}

SteamCollectionList scanSteamCollectionsIn(const QString& steamRoot, const QString& steamId64, QString* error) {
    if (error) error->clear();
    QString id = steamId64.trimmed();
    if (id.isEmpty()) id = scanSteam().steamId;
    QString account;
    if (!steamAccountId(id, &account)) {
        if (error) *error = QStringLiteral("No Steam account was found; set XMB_STEAM_ID to a SteamID64.");
        return {};
    }

    // Steam is installed where the library scan found it; fall back to the
    // conventional location so a collections-only read still works.
    // An explicit root (the keyless library names the installation it already resolved) is the only place read;
    // without one, the library scan's root and the conventional locations are tried, as before.
    const QStringList roots = !steamRoot.trimmed().isEmpty()
        ? QStringList{steamRoot}
        : QStringList{scanSteam().root, QStringLiteral("C:/Program Files (x86)/Steam"), QStringLiteral("C:/Program Files/Steam")};
    for (const QString& root : roots) {
        if (root.isEmpty()) continue;
        const QDir dir(QDir(root).filePath(QStringLiteral("userdata/%1/config/cloudstorage").arg(account)));
        const QString path = dir.filePath(QStringLiteral("cloud-storage-namespace-1.json"));
        QFile file(path);
        if (!file.exists()) continue;
        if (file.size() > MaxFileBytes) {
            if (error) *error = QStringLiteral("The Steam collection file is larger than expected.");
            return {};
        }
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = file.errorString();
            return {};
        }
        QJsonParseError parseError{};
        const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();
        if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
            if (error) *error = QStringLiteral("The Steam collection file was not valid JSON.");
            return {};
        }

        SteamCollectionList collections;
        for (const auto& entry : document.array()) {
            if (collections.size() >= MaxCollections) break;
            if (!entry.isArray()) continue;
            const auto pair = entry.toArray();
            if (pair.size() != 2 || !pair.at(1).isObject()) continue;
            const auto record = pair.at(1).toObject();
            if (record.value(QLatin1String("is_deleted")).toBool(false)) continue;
            const QString key = record.value(QLatin1String("key")).toString();
            if (!key.startsWith(QLatin1String("user-collections."))) continue;
            // The payload is a JSON document carried as a string.
            const QString payload = record.value(QLatin1String("value")).toString();
            if (payload.isEmpty()) continue;
            QJsonParseError innerError{};
            const auto collection = QJsonDocument::fromJson(payload.toUtf8(), &innerError);
            if (innerError.error != QJsonParseError::NoError || !collection.isObject()) continue;
            const auto object = collection.object();
            SteamCollection item;
            item.id = key.mid(int(std::strlen("user-collections.")));
            item.name = object.value(QLatin1String("name")).toString().trimmed();
            if (item.name.isEmpty()) item.name = item.id;
            if (item.name.size() > MaxNameLength) item.name.truncate(MaxNameLength);
            QSet<QString> seen;
            for (const auto& appId : object.value(QLatin1String("added")).toArray()) {
                if (item.appIds.size() >= MaxAppsPerCollection) break;
                if (!appId.isDouble()) continue;
                const QString text = QString::number(appId.toInt());
                if (text == QLatin1String("0") || seen.contains(text)) continue;
                seen.insert(text);
                item.appIds.push_back(text);
            }
            collections.push_back(std::move(item));
        }
        std::sort(collections.begin(), collections.end(),
                  [](const SteamCollection& a, const SteamCollection& b) {
                      return a.name.localeAwareCompare(b.name) < 0;
                  });
        return collections;
    }
    if (error) *error = QStringLiteral("Steam's userdata folder was not found for this account.");
    return {};
}

SteamCollectionList scanSteamCollections(const QString& steamId64, QString* error) {
    return scanSteamCollectionsIn({}, steamId64, error);
}

namespace {
QString overlayFilePath(const QString& dataDirectory) {
    return dataDirectory.trimmed().isEmpty()
        ? QString{}
        : QDir(dataDirectory).filePath(QStringLiteral("steam-collections-local.json"));
}

QString slugifyCollectionId(const QString& name) {
    QString slug;
    for (const QChar c : name.trimmed().toLower()) {
        if (c.isLetterOrNumber()) slug.append(c);
        else if (!slug.endsWith(QLatin1Char('-'))) slug.append(QLatin1Char('-'));
    }
    while (slug.startsWith(QLatin1Char('-'))) slug.remove(0, 1);
    while (slug.endsWith(QLatin1Char('-'))) slug.chop(1);
    return QStringLiteral("xmb-") + (slug.isEmpty() ? QStringLiteral("collection") : slug);
}
} // namespace

SteamUserOverlay loadSteamUserOverlay(const QString& dataDirectory) {
    SteamUserOverlay overlay;
    const QString path = overlayFilePath(dataDirectory);
    if (path.isEmpty()) return overlay;
    QFile file(path);
    if (!file.exists() || file.size() <= 0 || file.size() > MaxFileBytes) return overlay;
    if (!file.open(QIODevice::ReadOnly)) return overlay;
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(file.readAll(), &err);
    file.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return overlay;
    const QJsonObject root = doc.object();
    for (const auto& val : root.value(QLatin1String("favorites")).toArray()) {
        const QString id = val.toString().trimmed();
        if (!id.isEmpty()) overlay.favorites.insert(id);
    }
    const QJsonObject cols = root.value(QLatin1String("collections")).toObject();
    for (auto it = cols.begin(); it != cols.end(); ++it) {
        const QString name = it.key().trimmed().left(MaxNameLength);
        if (name.isEmpty() || !it.value().isArray()) continue;
        QStringList ids;
        QSet<QString> seen;
        for (const auto& v : it.value().toArray()) {
            const QString id = v.toString().trimmed();
            if (!id.isEmpty() && !seen.contains(id)) {
                seen.insert(id);
                ids.append(id);
            }
        }
        overlay.customCollections.insert(name, ids);
    }
    const QJsonObject opts = root.value(QLatin1String("launchOptions")).toObject();
    for (auto it = opts.begin(); it != opts.end(); ++it) {
        const QString id = it.key().trimmed();
        const QString args = it.value().toString().trimmed();
        if (!id.isEmpty() && !args.isEmpty()) overlay.launchOptions.insert(id, args.left(1024));
    }
    return overlay;
}

bool saveSteamUserOverlay(const QString& dataDirectory, const SteamUserOverlay& overlay) {
    const QString path = overlayFilePath(dataDirectory);
    if (path.isEmpty()) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    QStringList favList = overlay.favorites.values();
    favList.sort();
    QJsonArray favArray;
    for (const QString& id : favList) favArray.append(id);
    root.insert(QStringLiteral("favorites"), favArray);

    QJsonObject colsObj;
    QStringList colNames = overlay.customCollections.keys();
    colNames.sort();
    for (const QString& name : colNames) {
        QJsonArray arr;
        for (const QString& id : overlay.customCollections.value(name)) arr.append(id);
        colsObj.insert(name, arr);
    }
    root.insert(QStringLiteral("collections"), colsObj);

    QJsonObject optsObj;
    QStringList optKeys = overlay.launchOptions.keys();
    optKeys.sort();
    for (const QString& id : optKeys) {
        const QString args = overlay.launchOptions.value(id).trimmed();
        if (!args.isEmpty()) optsObj.insert(id, args);
    }
    root.insert(QStringLiteral("launchOptions"), optsObj);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.commit();
}

SteamCollectionList mergeSteamCollectionsWithOverlay(const SteamCollectionList& base,
                                                     const SteamUserOverlay& overlay) {
    if (overlay.favorites.isEmpty() && overlay.customCollections.isEmpty()) return base;
    SteamCollectionList out = base;
    const auto ensureCollection = [&](const QString& name, const QStringList& addIds) {
        const QString trimmed = name.trimmed().left(MaxNameLength);
        if (trimmed.isEmpty()) return;
        auto it = std::find_if(out.begin(), out.end(), [&](const SteamCollection& c) {
            return c.name.compare(trimmed, Qt::CaseInsensitive) == 0;
        });
        if (it == out.end()) {
            SteamCollection created;
            created.id = slugifyCollectionId(trimmed);
            created.name = trimmed;
            for (const QString& id : addIds) {
                if (!id.isEmpty() && !created.appIds.contains(id)) created.appIds.push_back(id);
            }
            out.push_back(std::move(created));
        } else {
            for (const QString& id : addIds) {
                if (!id.isEmpty() && !it->appIds.contains(id)) it->appIds.push_back(id);
            }
        }
    };

    if (!overlay.favorites.isEmpty()) {
        QStringList favs = overlay.favorites.values();
        favs.sort();
        ensureCollection(QStringLiteral("Favorites"), favs);
    }
    QStringList customNames = overlay.customCollections.keys();
    customNames.sort();
    for (const QString& name : customNames) {
        ensureCollection(name, overlay.customCollections.value(name));
    }
    std::sort(out.begin(), out.end(), [](const SteamCollection& a, const SteamCollection& b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return out;
}

} // namespace xmb
