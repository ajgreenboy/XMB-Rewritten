#pragma once
// Non-Steam shortcuts: the games and apps the owner added to Steam with "Add a Non-Steam Game". Steam keeps them per
// account in userdata/<accountid>/config/shortcuts.vdf, a BINARY KeyValues file (not the text VDF steam_vdf.h reads):
//
//   0x00 "shortcuts" 0x00            a nested map; every entry below is one map keyed "0", "1", ...
//     0x00 "0" 0x00                  one shortcut
//       0x02 "appid" 0x00 <int32>    the shortcut's 32-bit app id (older files may not store it)
//       0x01 "AppName" 0x00 <text> 0x00      0x01 = NUL-terminated UTF-8 string
//       0x01 "Exe" 0x00 <text> 0x00          (Steam stores the path quoted: "C:\...\x.exe")
//       0x01 "StartDir" / "icon" / "LaunchOptions" ...
//       0x02 "LastPlayTime" 0x00 <int32>     Unix seconds
//       0x00 "tags" 0x00 { 0x01 "0" 0x00 <text> 0x00 ... 0x08 }
//     0x08                           end of the shortcut
//   0x08                             end of "shortcuts"
// 0x08                               end of the file
// Type bytes: 0x00 map, 0x01 string, 0x02 int32 (little endian), 0x08 end; this reader also steps over 0x03 float,
// 0x04 pointer, 0x06 colour (4 bytes), 0x07 uint64 and 0x0A int64 (8 bytes) and refuses anything else, because the
// length of an unknown type cannot be known.
//
// The file is untrusted input: it is read with a size cap, every length is bounds-checked, nesting, entry counts and
// string sizes are limited, and the first structural error stops the read. A shortcut is accepted only when its whole
// map was read, so a truncated file yields the complete shortcuts before the damage and never a half-read one.
// Read only. See audit/STEAM-SHORTCUTS.md.
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

namespace xmb {

struct SteamShortcut {
    QString appName;
    QString exe;            // as Steam stores it (usually wrapped in double quotes)
    QString startDir;
    QString icon;           // path of the shortcut's own icon image; may be empty
    QString launchOptions;
    QStringList tags;
    quint32 appId = 0;      // the 32-bit shortcut app id in effect (stored, or computed when the file has none)
    bool appIdStored = false;
    quint32 lastPlayTime = 0;   // Unix seconds, 0 = never
    bool hidden = false;        // IsHidden = 1 in the file
    // The id Steam launches the shortcut by: (appId << 32) | 0x02000000, as a decimal string (never 0).
    QString gameId() const;
    // The row id the Game column uses, "steam-shortcut:<appId>".
    QString rowId() const;
    // The executable without Steam's surrounding quotes.
    QString exePath() const;
    // The name the row shows: AppName, else the executable's base name.
    QString displayName() const;
};

struct SteamShortcutList {
    QVector<SteamShortcut> shortcuts;
    QString error;            // empty on a clean read; the first problem otherwise (the list still holds what was read before it)
    int duplicates = 0;       // shortcuts dropped because an earlier one had the same app id
    int skipped = 0;          // shortcuts dropped for having no name and no executable, or being hidden
};

// Standard CRC-32 (IEEE 802.3, the zlib one) that Steam's shortcut ids are built on.
quint32 steamCrc32(const QByteArray& bytes);
// crc32(exe + appName) | 0x80000000, with both strings exactly as stored in the file (the executable keeps its quotes).
quint32 steamShortcutComputedAppId(const QString& exe, const QString& appName);
// (appId << 32) | 0x02000000
quint64 steamShortcutGameId64(quint32 appId);
// The shortcut's app id from a game id built by steamShortcutGameId64; false for any other number.
bool steamShortcutAppIdFromGameId(const QString& gameId, quint32* appId);
// True for a decimal string that is a shortcut game id (a 64-bit value whose low 32 bits are 0x02000000).
bool isSteamShortcutGameId(const QString& gameId);
// "steam://rungameid/<gameId>"; empty for anything that is not a shortcut game id.
QString steamShortcutLaunchUri(const QString& gameId);
// The registry key Steam would keep the shortcut's flags under (Apps\<32-bit id>); empty for a non-shortcut id.
QString steamShortcutRegistryAppId(const QString& gameId);

SteamShortcutList parseSteamShortcuts(const QByteArray& bytes);
// Reads userdata/<account>/config/shortcuts.vdf for `steamId64`. A missing file is simply no shortcuts (no error).
SteamShortcutList scanSteamShortcuts(const QString& steamRoot, const QString& steamId64);

// Artwork the owner (or SteamGridDB through Steam) placed in userdata/<account>/config/grid for a shortcut. Files are
// named by the 32-bit shortcut id (<id>.png wide capsule, <id>p portrait, <id>_hero, <id>_logo, <id>_icon); the older
// 64-bit game id naming is accepted too. png, jpg, jpeg and webp. Every field is a path or empty.
struct SteamShortcutArt {
    QString logo, wide, portrait, hero, icon;
};
SteamShortcutArt steamShortcutGridArtwork(const QString& steamRoot, const QString& steamId64, const SteamShortcut& shortcut);

// True when `imagePath` (a running process's image) is the shortcut's own executable: same file, compared without
// case and with either slash. Nothing else - not the folder, not the launcher - counts.
bool steamShortcutExeMatches(const SteamShortcut& shortcut, const QString& imagePath);

} // namespace xmb
