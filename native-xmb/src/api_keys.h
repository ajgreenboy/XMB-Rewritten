#pragma once
// API keys the user supplies themselves (Settings > Network Settings > API Keys).
//
// Pure helpers: key-format validation, where a key file lives, and the status a row shows. Nothing here logs,
// prints or returns a key except readKeyFile(), which the caller must keep out of every log and dump. The file
// helpers (atomic write, clear, user-only access) live in api_keys.cpp.
//
// Where the reading code looks is the single source of truth: SteamOwned::apiKey() reads
// STEAM_WEB_API_KEY, then <keyDirectory()>/steam_api_key; RawgClient reads RAWG_API_KEY, then
// <keyDirectory()>/rawg_api_key. Both take their directory from keyDirectory() below, so this page and the
// readers cannot disagree.
#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>

namespace xmb::apikeys {

enum class Id { SteamWeb, Rawg, SpotifyClient };
constexpr int kCount = 3;

struct Info {
    Id id;
    const char* slug;       // stable id used in row ids and field names
    const char* title;      // the row's title
    const char* envVar;     // the environment variable the reading code prefers
    const char* fileName;   // single-line file inside keyDirectory()
    const char* where;      // where the owner gets the key (text only)
    const char* help;       // one line shown as the row's help
    const char* format;     // what a valid key looks like, in words
};

inline const Info& info(Id id) {
    static const Info table[kCount] = {
        {Id::SteamWeb, "steam", "Steam Web API Key", "STEAM_WEB_API_KEY", "steam_api_key",
         "steamcommunity.com/dev/apikey",
         "Lists the games your Steam account owns. Get a key at steamcommunity.com/dev/apikey.",
         "32 hexadecimal characters (0-9, a-f)"},
        {Id::Rawg, "rawg", "RAWG API Key", "RAWG_API_KEY", "rawg_api_key",
         "rawg.io/apidocs",
         "Fills in artwork and details for games. Get a free key at rawg.io/apidocs.",
         "16 to 128 letters, digits, - or _"},
        // Spotify signs in with PKCE: a public Client ID and no secret, so there is no secret to store. The reader
        // (SpotifyClient::configure) takes SPOTIFY_CLIENT_ID, then "clientId" in spotify.json inside the DATA folder.
        {Id::SpotifyClient, "spotify", "Spotify Client ID", "SPOTIFY_CLIENT_ID", "spotify.json",
         "developer.spotify.com/dashboard",
         "Lets you sign in to Spotify. Create an app at developer.spotify.com/dashboard and register the redirect URI shown below.",
         "32 hexadecimal characters (0-9, a-f)"}};
    return table[id == Id::SteamWeb ? 0 : id == Id::Rawg ? 1 : 2];
}
inline const Info* infoForSlug(const QString& slug) {
    for (const Id id : {Id::SteamWeb, Id::Rawg, Id::SpotifyClient})
        if (slug == QLatin1String(info(id).slug)) return &info(id);
    return nullptr;
}

// Same rules the readers apply: steam_friends.cpp validApiKey (32 hex) and rawg_client.cpp validApiKey.
inline bool validSteamWebKey(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Fa-f0-9]{32}$"));
    return value.size() == 32 && pattern.match(value).hasMatch();
}
inline bool validRawgKey(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{16,128}$"));
    return value.size() <= 128 && pattern.match(value).hasMatch();
}
// A Spotify Client ID is the app's 32-character hexadecimal id (spotify_client.cpp validClientId).
inline bool validSpotifyClientId(const QString& value) { return validSteamWebKey(value); }
inline bool validKey(Id id, const QString& value) {
    return id == Id::SteamWeb ? validSteamWebKey(value) : id == Id::Rawg ? validRawgKey(value) : validSpotifyClientId(value);
}

// Where the key files live. XMB_API_KEY_DIR (tests and scripted runs only) redirects it so a run never touches
// the real application data folder; unset, it is the application data folder the readers always used.
inline QString keyDirectory() {
    const QString override = qEnvironmentVariable("XMB_API_KEY_DIR").trimmed();
    if (!override.isEmpty()) return QDir::cleanPath(override);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}
inline QString keyFilePath(Id id) {
    if (id == Id::SpotifyClient) return {};   // lives in the data folder (see the Spotify helpers), not the key folder
    const QString dir = keyDirectory();
    return dir.isEmpty() ? QString{} : QDir(dir).filePath(QLatin1String(info(id).fileName));
}

enum class Source { None, File, Environment };
struct Status {
    Source source = Source::None;  // where the key the app uses comes from
    bool fileSaved = false;        // a key file exists (Clear removes it, whatever the environment says)
    bool fileUsable = true;        // the saved file holds a key that passes the format check
    bool operator==(const Status& other) const {
        return source == other.source && fileSaved == other.fileSaved && fileUsable == other.fileUsable;
    }
};

// Status from plain values, so it is testable without a file or an environment. `envValue` is the raw
// environment variable; `fileKey` the first line of the saved file (trimmed), empty when absent or unreadable;
// `fileExists` whether the file is there at all. The environment wins exactly as the readers prefer it:
// Steam's reader takes any non-empty value, RAWG's only a valid one.
inline Status deriveStatus(Id id, const QString& envValue, const QString& fileKey, bool fileExists) {
    Status status;
    status.fileSaved = fileExists;
    status.fileUsable = fileKey.isEmpty() ? !fileExists : validKey(id, fileKey);
    const bool envCounts = id == Id::Rawg ? validRawgKey(envValue) : !envValue.trimmed().isEmpty();
    if (envCounts) status.source = Source::Environment;
    else if (fileExists && !fileKey.isEmpty()) status.source = Source::File;
    return status;
}
// What the row's value column says. Never the key, not even a part of it.
inline QString statusText(const Status& status) {
    if (status.source == Source::Environment) return QStringLiteral("Set by environment variable");
    if (status.source == Source::File) return status.fileUsable ? QStringLiteral("Set") : QStringLiteral("Set, not a valid key");
    return status.fileSaved ? QStringLiteral("Saved file is empty or unreadable") : QStringLiteral("Not set");
}

// --- the file helpers (api_keys.cpp) ----------------------------------------------------------------------
struct FileRead {
    bool exists = false;
    QString key;  // first line, trimmed; empty when missing, empty, or larger than 4096 bytes
};
FileRead readKeyFile(const QString& dir, const QString& fileName);
// Atomic: writes <file>.tmp in the same folder, restricts it to the current Windows user before the key goes in,
// then replaces the target in one step. The key is never part of `error`. `restricted` reports whether access
// was narrowed to the current user (it never widens: the folder's own inheritance is the floor).
bool writeKeyFile(const QString& dir, const QString& fileName, const QString& key, QString& error, bool* restricted = nullptr);
// Removes the saved file. Succeeds when there is nothing to remove.
bool clearKeyFile(const QString& dir, const QString& fileName, QString& error);

// --- Spotify Client ID: "clientId" inside <data folder>/spotify.json ---------------------------------------
// The reader's own file, so nothing else changes: other fields (redirectPort) are kept. Pass the data folder.
FileRead readSpotifyClientId(const QString& dataDir);
bool writeSpotifyClientId(const QString& dataDir, const QString& clientId, QString& error, bool* restricted = nullptr);
bool clearSpotifyClientId(const QString& dataDir, QString& error);
// Pure: the JSON bytes after setting (or, with an empty id, removing) "clientId", keeping every other field.
QByteArray spotifyJsonWithClientId(const QByteArray& existing, const QString& clientId);

}  // namespace xmb::apikeys
