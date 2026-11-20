#include "steam_library.h"
#include "steam_vdf.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QVector>
#include <algorithm>

namespace xmb {
namespace {

constexpr qsizetype MaximumConfigBytes = 2 * 1024 * 1024;
constexpr qsizetype MaximumManifestBytes = 1024 * 1024;
constexpr qsizetype MaximumTokens = 100000;
constexpr qsizetype MaximumEntries = 20000;
constexpr int MaximumDepth = 32;
constexpr int MaximumLibraries = 64;
constexpr qsizetype MaximumGames = 4096;
constexpr int MaximumManifestsPerLibrary = 4096;

using KvEntry = steamvdf::Entry;
using KvDocument = steamvdf::Document;

// The KeyValues dialect of the library scanner, read by the shared parser (steam_vdf.h): a leading
// BOM is not skipped, a quoted "{" or "}" acts as a brace, and keys are looked up case-insensitively.
// Every input is at most MaximumConfigBytes (readBounded), so the token-size limit cannot be hit by a
// bare word.
KvDocument parseKeyValues(const QByteArray& bytes) {
    steamvdf::Options options;
    options.skipBom = false;
    options.quotedBracesAreStructural = true;
    options.maxTokenBytes = MaximumConfigBytes;
    options.maxTokens = MaximumTokens;
    options.maxEntries = MaximumEntries;
    options.maxDepth = MaximumDepth;
    return steamvdf::parse(bytes, options);
}

bool readBounded(const QString& path, qsizetype maximum, QByteArray* bytes) {
    return steamvdf::readBounded(path, maximum, bytes, nullptr, false);
}

const KvEntry* findEntry(const QVector<KvEntry>& entries, const QString& key) {
    return steamvdf::findEntry(entries, key, Qt::CaseInsensitive);
}

QString stringValue(const QVector<KvEntry>& entries, const QString& key) {
    return steamvdf::stringValue(entries, key, Qt::CaseInsensitive);
}

QString canonicalDirectory(const QString& path) {
    const QFileInfo info(QDir::cleanPath(path));
    return info.isDir() ? QDir::fromNativeSeparators(info.canonicalFilePath()) : QString{};
}

QString normalizedPath(const QString& path) {
    QString result = QDir::fromNativeSeparators(QDir::cleanPath(path.trimmed()));
#ifdef Q_OS_WIN
    if (result.size() >= 2 && result.at(1) == QLatin1Char(':')) result[0] = result.at(0).toUpper();
#endif
    return result;
}

bool isSafeAppId(const QString& appId) {
    if (appId.isEmpty() || appId.size() > 12) return false;
    return std::all_of(appId.cbegin(), appId.cend(), [](QChar c) {
        return c >= QLatin1Char('0') && c <= QLatin1Char('9');
    });
}

bool isSafeNumericKey(const QString& key) {
    if (key.isEmpty() || key.size() > 20) return false;
    return std::all_of(key.cbegin(), key.cend(), [](QChar c) {
        return c >= QLatin1Char('0') && c <= QLatin1Char('9');
    });
}

bool isSafeInstallDirectory(const QString& directory) {
    if (directory.isEmpty() || directory.size() > 180 || directory == QStringLiteral(".") ||
        directory == QStringLiteral("..")) return false;
    return std::none_of(directory.cbegin(), directory.cend(), [](QChar c) {
        return c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') ||
               c == QLatin1Char('<') || c == QLatin1Char('>') || c == QLatin1Char('"') ||
               c == QLatin1Char('|') || c == QLatin1Char('?') || c == QLatin1Char('*') || c.isNull() ||
               c.category() == QChar::Other_Control;
    });
}

bool isInside(const QString& child, const QString& parent) {
    QString normalizedChild = normalizedPath(child);
    QString normalizedParent = normalizedPath(parent);
#ifdef Q_OS_WIN
    normalizedChild = normalizedChild.toCaseFolded();
    normalizedParent = normalizedParent.toCaseFolded();
#endif
    if (!normalizedParent.endsWith(QLatin1Char('/'))) normalizedParent.append(QLatin1Char('/'));
    return normalizedChild.startsWith(normalizedParent);
}

QString existingArtwork(const QStringList& candidates) {
    for (const auto& path : candidates) {
        const QFileInfo info(path);
        if (info.isFile() && info.size() > 0 && info.size() <= 20 * 1024 * 1024)
            return QDir::fromNativeSeparators(info.absoluteFilePath());
    }
    return {};
}

void appendArtworkMatches(QStringList& candidates, const QString& directory, const QStringList& patterns) {
    constexpr qsizetype MaximumMatchesPerGroup = 16;
    const QDir dir(directory);
    const auto matches = dir.entryInfoList(patterns, QDir::Files | QDir::Readable | QDir::NoSymLinks, QDir::Name);
    const qsizetype count = std::min<qsizetype>(matches.size(), MaximumMatchesPerGroup);
    for (qsizetype i = 0; i < count; ++i) candidates.append(matches.at(i).absoluteFilePath());
}

// Current Steam clients keep an app's library art in appcache/librarycache/<appid>/ under plain names
// (header.jpg, library_header.jpg, library_hero.jpg, library_600x900.jpg, library_capsule.jpg), and newer
// titles put them one level deeper in a hashed folder (<appid>/<sha1>/library_header.jpg). The older
// prefixed names (<appid>_header.jpg, *_header.jpg) are still tried by the callers. Measured on a real
// client 2026-11-16: of 1856 cache folders, 1149 had a plain header.jpg and 455 kept their art only in a
// hashed sub-folder, so neither was found and those rows drew the placeholder.
void appendCacheArtwork(QStringList& candidates, const QString& nested, const QStringList& names) {
    constexpr qsizetype MaximumSubfolders = 8;
    for (const auto& name : names) candidates.append(QDir(nested).filePath(name));
    const auto subfolders = QDir(nested).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable | QDir::NoSymLinks, QDir::Name);
    const qsizetype count = std::min<qsizetype>(subfolders.size(), MaximumSubfolders);
    for (qsizetype i = 0; i < count; ++i)
        for (const auto& name : names) candidates.append(QDir(subfolders.at(i).absoluteFilePath()).filePath(name));
}

QString artworkFor(const QString& steamRoot, const QString& appId, const QString& steamId) {
    const QString cache = QDir(steamRoot).filePath(QStringLiteral("appcache/librarycache"));
    QStringList candidates;
    QStringList userGrids;
    if (!steamId.isEmpty()) {
        QStringList userIds;
        bool converted = false;
        constexpr qulonglong SteamIdBase = 76561197960265728ULL;
        const qulonglong steamIdNumber = steamId.toULongLong(&converted);
        if (converted && steamIdNumber >= SteamIdBase)
            userIds.append(QString::number(steamIdNumber - SteamIdBase));
        userIds.append(steamId);
        for (const auto& userId : userIds)
            userGrids.append(QDir(steamRoot).filePath(QStringLiteral("userdata/%1/config/grid").arg(userId)));

        for (const auto& grid : userGrids)
            candidates << QDir(grid).filePath(appId + QStringLiteral(".png"))
                       << QDir(grid).filePath(appId + QStringLiteral(".jpg"));
        for (const auto& grid : userGrids)
            candidates << QDir(grid).filePath(appId + QStringLiteral("_hero.png"))
                       << QDir(grid).filePath(appId + QStringLiteral("_hero.jpg"))
                       << QDir(grid).filePath(appId + QStringLiteral("_library_hero.png"))
                       << QDir(grid).filePath(appId + QStringLiteral("_library_hero.jpg"));
    }

    const QString nested = QDir(cache).filePath(appId);
    // Wide header art contains the game's title/logo, so prefer it over hero art.
    candidates << QDir(cache).filePath(appId + QStringLiteral("_header_2x.jpg"))
               << QDir(cache).filePath(appId + QStringLiteral("_header_2x.png"))
               << QDir(cache).filePath(appId + QStringLiteral("_header.jpg"))
               << QDir(cache).filePath(appId + QStringLiteral("_header.png"));
    appendArtworkMatches(candidates, nested, {QStringLiteral("*_header_2x.jpg"), QStringLiteral("*_header_2x.png")});
    appendArtworkMatches(candidates, nested, {QStringLiteral("*_header.jpg"), QStringLiteral("*_header.png")});
    appendCacheArtwork(candidates, nested, {QStringLiteral("header.jpg"), QStringLiteral("library_header.jpg"),
                                            QStringLiteral("library_header_2x.jpg")});

    candidates << QDir(cache).filePath(appId + QStringLiteral("_hero.jpg"))
               << QDir(cache).filePath(appId + QStringLiteral("_hero.png"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_hero.jpg"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_hero.png"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_hero_2x.jpg"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_hero_2x.png"));
    appendArtworkMatches(candidates, nested, {QStringLiteral("*_library_hero_2x.jpg"), QStringLiteral("*_library_hero_2x.png")});
    appendArtworkMatches(candidates, nested, {QStringLiteral("*_library_hero.jpg"), QStringLiteral("*_library_hero.png")});
    appendArtworkMatches(candidates, nested, {QStringLiteral("*_hero.jpg"), QStringLiteral("*_hero.png")});

    // Portrait artwork and legacy icon files are last-resort fallbacks for the wide slot.
    candidates << QDir(cache).filePath(appId + QStringLiteral("_library_600x900_2x.jpg"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_600x900_2x.png"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_600x900.jpg"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_600x900.png"));
    appendArtworkMatches(candidates, nested, {QStringLiteral("*_library_600x900_2x.jpg"), QStringLiteral("*_library_600x900_2x.png")});
    appendArtworkMatches(candidates, nested, {QStringLiteral("*_library_600x900.jpg"), QStringLiteral("*_library_600x900.png")});
    appendCacheArtwork(candidates, nested, {QStringLiteral("library_hero.jpg"), QStringLiteral("library_600x900.jpg"),
                                            QStringLiteral("library_capsule.jpg")});
    for (const auto& grid : userGrids)
        candidates << QDir(grid).filePath(appId + QStringLiteral("p.png"))
                   << QDir(grid).filePath(appId + QStringLiteral("p.jpg"));
    const QString legacy = QDir(steamRoot).filePath(QStringLiteral("steam/games"));
    candidates << QDir(legacy).filePath(appId + QStringLiteral(".jpg"))
               << QDir(legacy).filePath(appId + QStringLiteral(".png"));
    return existingArtwork(candidates);
}

QString backgroundArtworkFor(const QString& steamRoot, const QString& appId, const QString& steamId) {
    const QString cache = QDir(steamRoot).filePath(QStringLiteral("appcache/librarycache"));
    QStringList candidates;
    QStringList grids;
    if (!steamId.isEmpty()) {
        QStringList ids;
        bool ok = false;
        constexpr qulonglong base = 76561197960265728ULL;
        const qulonglong number = steamId.toULongLong(&ok);
        if (ok && number >= base) ids.append(QString::number(number - base));
        ids.append(steamId);
        for (const auto& id : ids) grids.append(QDir(steamRoot).filePath(QStringLiteral("userdata/%1/config/grid").arg(id)));
    }
    for (const auto& grid : grids)
        candidates << QDir(grid).filePath(appId + QStringLiteral("_hero.jpg"))
                   << QDir(grid).filePath(appId + QStringLiteral("_hero.png"))
                   << QDir(grid).filePath(appId + QStringLiteral("_library_hero.jpg"))
                   << QDir(grid).filePath(appId + QStringLiteral("_library_hero.png"));
    candidates << QDir(cache).filePath(appId + QStringLiteral("_library_hero_2x.jpg"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_hero_2x.png"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_hero.jpg"))
               << QDir(cache).filePath(appId + QStringLiteral("_library_hero.png"));
    const QString nested = QDir(cache).filePath(appId);
    appendArtworkMatches(candidates, nested, {QStringLiteral("*_library_hero_2x.jpg"), QStringLiteral("*_library_hero_2x.png")});
    appendArtworkMatches(candidates, nested, {QStringLiteral("*_library_hero.jpg"), QStringLiteral("*_library_hero.png")});
    appendCacheArtwork(candidates, nested, {QStringLiteral("library_hero.jpg"), QStringLiteral("library_hero_2x.jpg")});
    return existingArtwork(candidates);
}

struct RootInfo {
    QString path;
    QString steamId;
};

struct LibraryInfo {
    QString path;
    RootInfo owner;
};

constexpr qulonglong SteamIdBase = 76561197960265728ULL;
constexpr qulonglong MaximumAccountId = 0xffffffffULL;

bool steamIdForAccountKey(const QString& key, qulonglong* steamId) {
    if (!isSafeNumericKey(key)) return false;
    bool valid = false;
    const qulonglong parsed = key.toULongLong(&valid);
    if (!valid || parsed < SteamIdBase || parsed - SteamIdBase == 0 ||
        parsed - SteamIdBase > MaximumAccountId) return false;
    if (steamId) *steamId = parsed;
    return true;
}

QString activeRegistrySteamId(const QString& steamRoot) {
#ifdef Q_OS_WIN
    QSettings steamRegistry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"), QSettings::NativeFormat);
    const QString registryPath = steamRegistry.value(QStringLiteral("SteamPath")).toString().trimmed();
    const QString canonicalRegistryPath = canonicalDirectory(registryPath);
    const QString canonicalSteamRoot = canonicalDirectory(steamRoot);
    if (canonicalRegistryPath.isEmpty() || canonicalSteamRoot.isEmpty() ||
        canonicalRegistryPath.compare(canonicalSteamRoot, Qt::CaseInsensitive) != 0) return {};

    QSettings activeProcess(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam\\ActiveProcess"),
                            QSettings::NativeFormat);
    bool valid = false;
    const qulonglong accountId = activeProcess.value(QStringLiteral("ActiveUser")).toULongLong(&valid);
    if (!valid || accountId == 0 || accountId > MaximumAccountId) return {};
    return QString::number(SteamIdBase + accountId);
#else
    Q_UNUSED(steamRoot);
    return {};
#endif
}

QString selectedSteamId(const QString& steamRoot, bool allowActiveRegistry) {
    if (allowActiveRegistry) {
        const QString active = activeRegistrySteamId(steamRoot);
        if (!active.isEmpty()) return active;
    }

    const QString path = QDir(steamRoot).filePath(QStringLiteral("config/loginusers.vdf"));
    QByteArray bytes;
    if (!readBounded(path, MaximumConfigBytes, &bytes)) return {};
    const auto document = parseKeyValues(bytes);
    if (!document.error.isEmpty()) return {};
    const auto* users = findEntry(document.entries, QStringLiteral("users"));
    if (!users || !users->block) return {};

    struct Account {
        qulonglong steamId = 0;
        bool mostRecent = false;
        bool hasTimestamp = false;
        qulonglong timestamp = 0;
    };
    QVector<Account> accounts;
    for (const auto& user : users->children) {
        qulonglong steamId = 0;
        if (!user.block || !steamIdForAccountKey(user.key, &steamId)) continue;
        Account account;
        account.steamId = steamId;
        // The client writes lower-case "timestamp" today and has dropped MostRecent; accept the older
        // spellings so a different Steam version still resolves the active account.
        account.mostRecent = stringValue(user.children, QStringLiteral("MostRecent")) == QStringLiteral("1") ||
                             stringValue(user.children, QStringLiteral("AutoLogin")) == QStringLiteral("1") ||
                             stringValue(user.children, QStringLiteral("AllowAutoLogin")) == QStringLiteral("1");
        bool timestampValid = false;
        QString stamp = stringValue(user.children, QStringLiteral("timestamp"));
        if (stamp.isEmpty()) stamp = stringValue(user.children, QStringLiteral("Timestamp"));
        account.timestamp = stamp.toULongLong(&timestampValid);
        account.hasTimestamp = timestampValid;
        accounts.append(account);
    }

    for (const auto& account : accounts) {
        if (!account.mostRecent) continue;
        qulonglong selected = account.steamId;
        for (const auto& candidate : accounts)
            if (candidate.mostRecent && candidate.steamId > selected) selected = candidate.steamId;
        return QString::number(selected);
    }
    if (accounts.size() == 1) return QString::number(accounts.constFirst().steamId);

    const Account* selected = nullptr;
    for (const auto& account : accounts) {
        if (!account.hasTimestamp) continue;
        if (!selected || account.timestamp > selected->timestamp ||
            (account.timestamp == selected->timestamp && account.steamId > selected->steamId))
            selected = &account;
    }
    return selected ? QString::number(selected->steamId) : QString{};
}

QStringList rootsToTry(const QString& explicitRoot) {
    if (!explicitRoot.trimmed().isEmpty()) return {explicitRoot};
    QStringList candidates;
    const QString overrideRoot = qEnvironmentVariable("XMB_STEAM_ROOT").trimmed();
    if (!overrideRoot.isEmpty()) return {overrideRoot};

#ifdef Q_OS_WIN
    QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"), QSettings::NativeFormat);
    const QString registryRoot = registry.value(QStringLiteral("SteamPath")).toString().trimmed();
    if (!registryRoot.isEmpty()) candidates.append(registryRoot);
    const QString programFilesX86 = qEnvironmentVariable("ProgramFiles(x86)").trimmed();
    if (!programFilesX86.isEmpty()) candidates.append(QDir(programFilesX86).filePath(QStringLiteral("Steam")));
    else candidates.append(QStringLiteral("C:/Program Files (x86)/Steam"));
#else
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    candidates << QDir(home).filePath(QStringLiteral(".steam/steam"))
               << QDir(home).filePath(QStringLiteral(".local/share/Steam"));
#endif

    QStringList unique;
    for (const auto& candidate : candidates) {
        const QString cleaned = normalizedPath(candidate);
        bool found = false;
        for (const auto& prior : unique) {
#ifdef Q_OS_WIN
            found = prior.compare(cleaned, Qt::CaseInsensitive) == 0;
#else
            found = prior == cleaned;
#endif
            if (found) break;
        }
        if (!found) unique.append(cleaned);
    }
    return unique;
}

bool samePath(const QString& left, const QString& right) {
#ifdef Q_OS_WIN
    return normalizedPath(left).compare(normalizedPath(right), Qt::CaseInsensitive) == 0;
#else
    return normalizedPath(left) == normalizedPath(right);
#endif
}

void addLibraries(const RootInfo& root, QVector<LibraryInfo>& libraries, QStringList& warnings) {
    int unavailable = 0;
    QStringList unavailablePaths;
    auto add = [&](const QString& path) {
        const QString canonical = canonicalDirectory(path);
        if (canonical.isEmpty() || !QFileInfo(QDir(canonical).filePath(QStringLiteral("steamapps"))).isDir()) {
            // The root itself is a normal first-install state until Steam has created steamapps;
            // only report additional libraries explicitly named by a real library list.
            if (!samePath(path, root.path) && !path.trimmed().isEmpty()) {
                const QString normalized = normalizedPath(path);
                bool duplicate = false;
                for (const auto& prior : unavailablePaths) {
#ifdef Q_OS_WIN
                    duplicate = prior.compare(normalized, Qt::CaseInsensitive) == 0;
#else
                    duplicate = prior == normalized;
#endif
                    if (duplicate) break;
                }
                if (!duplicate && unavailablePaths.size() < MaximumLibraries) {
                    unavailablePaths.append(normalized);
                    ++unavailable;
                }
            }
            return;
        }
        for (const auto& previous : libraries) {
#ifdef Q_OS_WIN
            if (previous.path.compare(canonical, Qt::CaseInsensitive) == 0) return;
#else
            if (previous.path == canonical) return;
#endif
        }
        if (libraries.size() >= MaximumLibraries) {
            if (!warnings.contains(QStringLiteral("Steam library count exceeds safety limit.")))
                warnings.append(QStringLiteral("Steam library count exceeds safety limit."));
            return;
        }
        libraries.append({canonical, root});
    };

    add(root.path);
    // Steam has used both locations. The steamapps form is canonical when present; do not
    // silently prefer a stale alternate file when the canonical file is corrupt.
    const QString canonicalConfig = QDir(root.path).filePath(QStringLiteral("steamapps/libraryfolders.vdf"));
    const QString alternateConfig = QDir(root.path).filePath(QStringLiteral("config/libraryfolders.vdf"));
    QString config;
    if (QFileInfo(canonicalConfig).isFile()) config = canonicalConfig;
    else if (QFileInfo(alternateConfig).isFile()) config = alternateConfig;
    else return; // The optional library list has not been written yet.

    QByteArray bytes;
    if (!readBounded(config, MaximumConfigBytes, &bytes)) {
        warnings.append(QStringLiteral("A Steam library configuration could not be read."));
        return;
    }
    const auto document = parseKeyValues(bytes);
    if (!document.error.isEmpty()) {
        warnings.append(QStringLiteral("A Steam library configuration could not be parsed."));
        return;
    }
    const auto* folders = findEntry(document.entries, QStringLiteral("libraryfolders"));
    if (!folders || !folders->block) return;
    for (const auto& folder : folders->children) {
        if (!isSafeAppId(folder.key)) continue;
        const QString path = folder.block ? stringValue(folder.children, QStringLiteral("path")) : folder.value;
        if (!path.trimmed().isEmpty()) add(path);
    }
    if (unavailable > 0)
        warnings.append(QStringLiteral("%1 configured Steam librar%2 unavailable; healthy libraries remain visible.")
                            .arg(unavailable).arg(unavailable == 1 ? QStringLiteral("y is") : QStringLiteral("ies are")));
}

void scanLibrary(const QString& steamRoot, const QString& steamId, const QString& library,
                 SteamInstallation& result, QSet<QString>& seenApps) {
    const QString steamApps = QDir(library).filePath(QStringLiteral("steamapps"));
    const QFileInfo commonInfo(QDir(steamApps).filePath(QStringLiteral("common")));
    const QString common = canonicalDirectory(commonInfo.absoluteFilePath());
    if (common.isEmpty()) return;
    const QDir directory(steamApps);
    const auto manifests = directory.entryInfoList({QStringLiteral("appmanifest_*.acf")},
        QDir::Files | QDir::Readable | QDir::NoSymLinks, QDir::Name);
    const qsizetype count = std::min<qsizetype>(manifests.size(), MaximumManifestsPerLibrary);
    if (manifests.size() > MaximumManifestsPerLibrary)
        result.warnings.append(QStringLiteral("A Steam library has more manifests than the scan limit."));
    for (qsizetype i = 0; i < count; ++i) {
        if (result.games.size() >= MaximumGames) return;
        const QFileInfo& manifest = manifests.at(i);
        const QString expectedId = manifest.completeBaseName().mid(QStringLiteral("appmanifest_").size());
        if (!isSafeAppId(expectedId)) continue;
        QByteArray bytes;
        if (!readBounded(manifest.absoluteFilePath(), MaximumManifestBytes, &bytes)) continue;
        const auto document = parseKeyValues(bytes);
        if (!document.error.isEmpty()) continue;
        const auto* state = findEntry(document.entries, QStringLiteral("AppState"));
        if (!state || !state->block) continue;
        const QString appId = stringValue(state->children, QStringLiteral("appid"));
        if (appId != expectedId || !isSafeAppId(appId) || seenApps.contains(appId)) continue;
        if (appId == QStringLiteral("228980")) continue; // Steamworks Common Redistributables
        bool stateOk = false;
        const qulonglong flags = stringValue(state->children, QStringLiteral("StateFlags")).toULongLong(&stateOk);
        if (!stateOk || (flags & 4ULL) == 0) continue;
        const QString installDir = stringValue(state->children, QStringLiteral("installdir"));
        if (!isSafeInstallDirectory(installDir)) continue;
        const QString installPath = QDir(common).filePath(installDir);
        const QFileInfo installInfo(installPath);
        if (!installInfo.isDir() || installInfo.isSymLink()) continue;
        const QString actualDirectory = canonicalDirectory(installPath);
        if (actualDirectory.isEmpty() || !isInside(actualDirectory, common)) continue;

        SteamGame game;
        game.appId = appId;
        game.title = stringValue(state->children, QStringLiteral("name")).trimmed();
        if (game.title.isEmpty()) game.title = appId;
        game.installDirectory = actualDirectory;
        game.artwork = artworkFor(steamRoot, appId, steamId);
        game.backgroundArtwork = backgroundArtworkFor(steamRoot, appId, steamId);
        result.games.append(std::move(game));
        seenApps.insert(appId);
    }
}

} // namespace

SteamInstallation scanSteam(const QString& explicitRoot) {
    SteamInstallation result;
    QVector<RootInfo> roots;
    const bool allowActiveRegistry = explicitRoot.trimmed().isEmpty() &&
        qEnvironmentVariable("XMB_STEAM_ROOT").trimmed().isEmpty();
    for (const auto& candidate : rootsToTry(explicitRoot)) {
        const QString canonical = canonicalDirectory(candidate);
        if (canonical.isEmpty() || !QDir(canonical).exists()) continue;
        bool duplicate = false;
        for (const auto& root : roots) {
#ifdef Q_OS_WIN
            duplicate = root.path.compare(canonical, Qt::CaseInsensitive) == 0;
#else
            duplicate = root.path == canonical;
#endif
            if (duplicate) break;
        }
        if (!duplicate) roots.append({canonical, selectedSteamId(canonical, allowActiveRegistry)});
    }

    if (roots.isEmpty()) {
        result.warnings.append(QStringLiteral("Steam was not found in the configured locations."));
        return result;
    }
    result.root = roots.constFirst().path;
    result.steamId = roots.constFirst().steamId;

    QVector<LibraryInfo> libraries;
    for (const auto& root : roots) addLibraries(root, libraries, result.warnings);
    QSet<QString> seenApps;
    for (const auto& library : libraries) {
        scanLibrary(library.owner.path, library.owner.steamId, library.path, result, seenApps);
        if (result.games.size() >= MaximumGames) {
            result.warnings.append(QStringLiteral("Steam game scan reached the result limit."));
            break;
        }
    }
    std::sort(result.games.begin(), result.games.end(), [](const SteamGame& left, const SteamGame& right) {
        const int titleOrder = QString::compare(left.title, right.title, Qt::CaseInsensitive);
        return titleOrder == 0 ? left.appId < right.appId : titleOrder < 0;
    });
    return result;
}

namespace {

// Steam app ids are 32-bit decimal; anything else is not an id this client can name, and the
// console takes arguments verbatim, so validation is the allow-list (audit/STEAM-CLI.md section 3.4).
bool validSteamAppId(const QString& appId) {
    if (appId.isEmpty() || appId.size() > 10) return false;
    for (const QChar character : appId) {
        if (!character.isDigit() || character.unicode() > 0x7f) return false;
    }
    bool valid = false;
    const qulonglong numeric = appId.toULongLong(&valid, 10);
    return valid && numeric != 0 && numeric <= 0xffffffffULL;
}

QString consoleLogPath(const QString& steamRoot) {
    const QStringList roots = rootsToTry(steamRoot);
    for (const QString& root : roots) {
        const QString path = QDir(root).filePath(QStringLiteral("logs/console_log.txt"));
        if (QFileInfo(path).isFile()) return path;
    }
    return {};
}

// Every line the client writes is prefixed with "[2026-10-26 20:39:50] ".
QString stripLogTimestamp(const QString& line) {
    if (!line.startsWith(QLatin1Char('['))) return line;
    const int close = line.indexOf(QStringLiteral("] "));
    if (close < 0) return line;
    return line.mid(close + 2);
}

// The single argument after "+<command>" on a logged command line, with the client's quoting
// removed: '"...\steam.exe" +app_stop 999999999"' must yield "999999999".
QString commandArgument(const QString& line, const QString& command) {
    const int at = line.indexOf(command);
    if (at < 0) return {};
    QString rest = line.mid(at + command.size()).trimmed();
    while (rest.startsWith(QLatin1Char('"'))) rest.remove(0, 1);
    int end = 0;
    while (end < rest.size() && !rest.at(end).isSpace() && rest.at(end) != QLatin1Char('"')) ++end;
    return rest.left(end);
}

} // namespace

bool steamStopApp(const QString& appId,const QString& steamRoot,QString* error) {
    if (error) error->clear();
#ifdef Q_OS_WIN
    const QString id = appId.trimmed();
    if (!validSteamAppId(id)) { if (error) *error = QStringLiteral("Not a Steam app id."); return false; }
    QString executable;
    for (const QString& root : rootsToTry(steamRoot)) {
        const QString candidate = QDir(root).filePath(QStringLiteral("steam.exe"));
        if (QFileInfo(candidate).isFile()) { executable = candidate; break; }
    }
    if (executable.isEmpty()) { if (error) *error = QStringLiteral("Steam installation not found."); return false; }
    // The console command is undocumented and prints nothing; the registry Running flag is the proof.
    if (!QProcess::startDetached(executable, steamStopArguments(id))) {
        if (error) *error = QStringLiteral("Steam could not be started."); return false;
    }
    return true;
#else
    Q_UNUSED(appId); Q_UNUSED(steamRoot);
    if (error) *error = QStringLiteral("Steam is only supported on Windows.");
    return false;
#endif
}

QStringList steamStopArguments(const QString& appId) {
    const QString id = appId.trimmed();
    if (!validSteamAppId(id)) return {};
    return {QStringLiteral("+app_stop"), id};
}

SteamConsoleLogSlice steamConsoleLogTail(const QString& steamRoot, qint64 offset,
                                         qsizetype maximumBytes) {
    SteamConsoleLogSlice slice;
    const qsizetype cap =
        std::clamp<qsizetype>(maximumBytes > 0 ? maximumBytes : 64 * 1024, 1, 1024 * 1024);
    const QString path = consoleLogPath(steamRoot);
    if (path.isEmpty()) {
        slice.error = QStringLiteral("Steam's console log was not found.");
        return slice;
    }
    slice.path = path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        slice.error = file.errorString();
        return slice;
    }
    const qint64 size = file.size();
    slice.size = size;
    if (size < 0) {
        slice.error = QStringLiteral("Steam's console log has no readable size.");
        return slice;
    }
    qint64 start = offset;
    if (offset < 0 || offset > size) {
        // The file was truncated, rotated or never snapshotted: read the tail instead of nothing and
        // say so, so a caller never treats a rotated log as "command not delivered".
        slice.truncated = offset > size;
        start = std::max<qint64>(0, size - qint64(cap));
    }
    slice.offset = start;
    if (size > start && !file.seek(start)) {
        slice.error = QStringLiteral("Steam's console log could not be positioned.");
        return slice;
    }
    slice.bytes = file.read(std::min<qint64>(qint64(cap), size - start));
    if (file.error() != QFileDevice::NoError) {
        slice.error = file.errorString();
        slice.bytes.clear();
        return slice;
    }
    slice.available = true;
    return slice;
}

qint64 steamConsoleLogSize(const QString& steamRoot) {
    const QString path = consoleLogPath(steamRoot);
    if (path.isEmpty()) return -1;
    return QFileInfo(path).size();
}

SteamStopEvidence steamVerifyStop(const QString& appId, qint64 offset,
                                  const QString& steamRoot) {
    SteamStopEvidence evidence;
    const QString id = appId.trimmed();
    if (!validSteamAppId(id)) {
        evidence.error = QStringLiteral("Not a Steam app id.");
        return evidence;
    }
    const SteamConsoleLogSlice slice = steamConsoleLogTail(steamRoot, offset);
    evidence.logAvailable = slice.available;
    if (!slice.available) {
        evidence.error = slice.error;
        return evidence;
    }
    if (slice.truncated)
        evidence.error = QStringLiteral(
            "Steam's console log is shorter than the snapshot, so the command cannot be attributed.");

    const QStringList lines = QString::fromUtf8(slice.bytes).split(QLatin1Char('\n'));
    int matched = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines.at(i);
        if (!line.contains(QStringLiteral("ExecCommandLine:"))) continue;
        if (!line.contains(QStringLiteral("+app_stop"))) continue;
        if (commandArgument(line, QStringLiteral("+app_stop")) != id) continue;
        matched = i;
        evidence.delivered = true;
        evidence.commandLine = stripLogTimestamp(line).trimmed();
        break;
    }
    if (!evidence.delivered) {
        if (evidence.error.isEmpty())
            evidence.error = QStringLiteral(
                "The client did not record this stop command after the snapshot offset.");
        return evidence;
    }
    // A successful app_stop prints nothing, so this text is only the client's neighbouring spew; the
    // "command not found" line is what turns delivery into a rejection.
    QStringList context;
    for (int i = matched + 1; i < lines.size() && context.size() < 8; ++i) {
        const QString& line = lines.at(i);
        if (line.contains(QStringLiteral("ExecCommandLine:"))) break;
        const QString cleaned = stripLogTimestamp(line).trimmed();
        if (cleaned.isEmpty()) continue;
        if (cleaned.contains(QStringLiteral("command not found"))) evidence.rejected = true;
        context.append(cleaned);
        if (cleaned.size() > 256) break;
    }
    evidence.context = context.join(QLatin1Char('\n'));
    if (evidence.context.size() > 2048) evidence.context.truncate(2048);
    return evidence;
}

SteamAppState steamAppState(const QString& appId) {
    SteamAppState state;
#ifdef Q_OS_WIN
    bool numeric = false;
    appId.toULongLong(&numeric);
    if (!numeric) return state;
    // Steam keeps these live while it runs: RunningAppID names the title in the foreground of its
    // lifecycle, Apps\<id>\Running stays 1 from launch (including any launcher) until the last
    // process exits, and Apps\<id>\Updating is 1 while a pre-launch update downloads.
    // Same test seam as steamRegistryKeyPath() (steam_accounts.cpp, which this file does not link): a scripted run may name
    // a scratch key under HKEY_CURRENT_USER\Software\XmbTest\; a value that is set but not acceptable reads a dead
    // scratch key, never the real one. Read only.
    QString base = QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam");
    {
        const QString seam = qEnvironmentVariable("XMB_STEAM_REGISTRY_KEY").trimmed();
        const QString allowed = QStringLiteral("HKEY_CURRENT_USER\\Software\\XmbTest\\");
        if (seam.size() > allowed.size() && seam.startsWith(allowed, Qt::CaseInsensitive) && !seam.contains(QStringLiteral("..")) && !seam.contains(QLatin1Char('/')))
            base = seam;
        else if (!seam.isEmpty())
            base = allowed + QStringLiteral("RejectedOverride");
    }
    const QSettings steam(base, QSettings::NativeFormat);
    const QSettings app(base + QStringLiteral("\\Apps\\") + appId, QSettings::NativeFormat);
    // QSettings reports an empty value for a missing key, and can also mask a registry
    // read/parse failure. Do not turn either case into "stopped". ActiveProcess is Steam's
    // own liveness/sign-in record: while it is absent, malformed, or signed out, the
    // Apps values are not a reliable observation (Steam is commonly rewriting them while
    // restarting).
    const QSettings active(base + QStringLiteral("\\ActiveProcess"), QSettings::NativeFormat);
    bool validPid = false;
    bool validUser = false;
    const qulonglong pid = active.value(QStringLiteral("pid")).toULongLong(&validPid);
    const qulonglong user = active.value(QStringLiteral("ActiveUser")).toULongLong(&validUser);
    if (steam.status() != QSettings::NoError || app.status() != QSettings::NoError ||
        active.status() != QSettings::NoError || !active.contains(QStringLiteral("pid")) ||
        !active.contains(QStringLiteral("ActiveUser")) || !validPid || pid == 0 ||
        !validUser || user == 0)
        return state;

    const QString runningId = steam.value(QStringLiteral("RunningAppID")).toString().trimmed();
    const QString runningValue = app.value(QStringLiteral("Running")).toString().trimmed();
    const QString updatingValue = app.value(QStringLiteral("Updating")).toString().trimmed();
    const bool runningFlag = runningValue == QStringLiteral("1");
    const bool updatingFlag = updatingValue == QStringLiteral("1");
    const bool foreground = runningId == appId;
    bool validRunningId = runningId.isEmpty();
    if (!validRunningId) {
        bool numericRunningId = false;
        runningId.toULongLong(&numericRunningId);
        validRunningId = numericRunningId;
    }
    // A present app record or RunningAppID with a non-boolean/non-numeric value is
    // malformed, not stopped. With a readable signed-in client, no app flag means
    // the client has confirmed this title is stopped.
    if ((!runningValue.isEmpty() && runningValue != QStringLiteral("0") && !runningFlag) ||
        (!updatingValue.isEmpty() && updatingValue != QStringLiteral("0") && !updatingFlag) ||
        !validRunningId)
        return state;
    state.running = foreground || runningFlag;
    state.updating = updatingFlag;
    state.status = state.updating ? SteamAppStatus::Updating :
                   state.running ? SteamAppStatus::Running : SteamAppStatus::Stopped;
#else
    Q_UNUSED(appId);
#endif
    return state;
}

SteamTrackTransition steamTrackObservation(SteamTrackState current, SteamAppStatus status,
                                           qint64 elapsedMs, qint64 timeoutMs) {
    SteamTrackTransition result;
    result.state = current;
    result.status = status;
    switch (status) {
    case SteamAppStatus::Unavailable:
        // A restarting/signed-out client and a transient read failure are not evidence that the title
        // stopped: reset the exit debounce. Before a confirmed run, a client that never becomes readable
        // must still recover within the launch wait, so the unavailable clock is deliberately NOT reset.
        result.state.consecutiveStopped = 0;
        if (!current.seenRunning && elapsedMs >= timeoutMs)
            result.action = SteamTrackAction::TimedOut;
        break;
    case SteamAppStatus::Updating:
        // Known-but-not-stopped; also interrupts the clear streak. Before a confirmed run a download or
        // update can take arbitrarily long, so the launch wait restarts on every updating observation.
        result.state.consecutiveStopped = 0;
        if (!current.seenRunning)
            result.action = SteamTrackAction::RestartWait;
        break;
    case SteamAppStatus::Running:
        result.state.consecutiveStopped = 0;
        if (!current.seenRunning || current.consecutiveStopped != 0) {
            result.state.seenRunning = true;
            result.action = SteamTrackAction::ConfirmedRunning;
        }
        break;
    case SteamAppStatus::Stopped:
        if (!current.seenRunning) {
            if (elapsedMs >= timeoutMs)
                result.action = SteamTrackAction::TimedOut;
        } else if (++result.state.consecutiveStopped >= 2) {
            result.state.seenRunning = false;
            result.state.consecutiveStopped = 0;
            result.action = SteamTrackAction::Returned;
        } else {
            result.action = SteamTrackAction::Cleared;
        }
        break;
    }
    return result;
}

} // namespace xmb
