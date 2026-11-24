#pragma once

// Reads app names and types from the Steam client's own metadata cache, <steam>/appcache/appinfo.vdf. This is the
// client's authoritative local source for "what is app 12345 called, and is it a game or a tool?", so the keyless
// library (steam_keyless.h) can name titles it has no manifest for. Read-only, bounded, never throws.
//
// Binary layout (documented here because it is not published by Valve; measured against a real v29 file
// 2026-11-23 and cross-checked with the SteamDatabase appinfo readers). Little endian throughout.
//
//   header   uint32 magic      0x07564427 (v27) / 0x07564428 (v28) / 0x07564429 (v29): "DV\x07" plus a version byte
//            uint32 universe
//            [v29 only] int64 stringTableOffset   absolute file offset of the key string table
//   entries  repeated until appid == 0:
//            uint32 appid
//            uint32 size                bytes that follow, up to the next entry
//            uint32 infoState
//            uint32 lastUpdated
//            uint64 accessToken
//            byte[20] sha1 of the text form
//            uint32 changeNumber
//            [v28+] byte[20] sha1 of the binary form
//            byte[size - 40 (v27) / size - 60 (v28, v29)] binary KeyValues: the app's "appinfo" object
//   v29 string table, at stringTableOffset:   uint32 count, then `count` NUL-terminated UTF-8 strings.
//
// Binary KeyValues: a type byte, then (unless it is the end marker) the key, then the value.
//   0x00 object (children follow until a 0x08)   0x01 string (NUL-terminated UTF-8)   0x02 int32
//   0x03 float32   0x04 pointer (4 bytes)   0x05 wide string (UTF-16LE, NUL-terminated)   0x06 colour (4 bytes)
//   0x07 uint64   0x08 end of the enclosing object   0x0A int64
// A key is a NUL-terminated string in v27/v28, and a uint32 index into the string table in v29.
// The two values this module reads are appinfo > common > name and appinfo > common > type.

#include <QHash>
#include <QSet>
#include <QString>

namespace xmb {

struct SteamAppInfoEntry {
    QString title; // appinfo.common.name; empty when the entry has none
    QString type;  // appinfo.common.type as Steam spells it ("Game", "Tool", "DLC", "Music", ...); empty when absent
};

// Looks up only the app ids in `wanted`; every other entry is skipped without being read, so a 100 MB file costs a
// sequential pass of 8-byte headers. A malformed entry ends the pass at the damage (what was read so far is kept) and
// sets `error`. Bounds: a single entry above 16 MiB, a string table above 32 MiB or a key/value above 64 KiB is
// refused rather than allocated. Apps with no entry are simply absent from the result.
QHash<QString, SteamAppInfoEntry> readSteamAppInfo(const QString& path, const QSet<QString>& wanted,
                                                    QString* error = nullptr);

// True when `type` is what Steam calls a game ("Game", any case). Demos, DLC, tools, music, video, servers and
// redistributables are not, which keeps Steamworks Common Redistributables and dedicated servers out of a library.
bool steamAppTypeIsGame(const QString& type);

} // namespace xmb
