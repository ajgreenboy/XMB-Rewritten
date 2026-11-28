// Tests for the non-Steam shortcut reader (steam_shortcuts.h): the BINARY shortcuts.vdf parser, the shortcut id and
// launch-id arithmetic, the per-account scan and the grid-artwork lookup. Every file is built here from synthetic
// bytes with obviously fake names ("Fixture ..."); no real Steam data, process or registry value is read. The scratch
// directory is the first argument (QTemporaryDir is avoided: it fails on some machines for an environmental reason).

#include "steam_shortcuts.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <cstdio>

using namespace xmb;

namespace {

int failures = 0;
int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
void checkText(const QString& got, const QString& want, const char* what) {
    ++checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s (got \"%s\", want \"%s\")\n", what, qPrintable(got), qPrintable(want));
        ++failures;
    }
}

QString scratchRoot;

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    return file.write(bytes) == bytes.size();
}

// ---- a tiny writer for Steam's binary KeyValues ------------------------------------------------------------------------
QByteArray cstr(const QString& text) { return text.toUtf8() + QByteArray(1, '\0'); }
QByteArray strField(const QString& key, const QString& value) { return QByteArray(1, '\x01') + cstr(key) + cstr(value); }
QByteArray intField(const QString& key, quint32 value) {
    QByteArray out(1, '\x02');
    out += cstr(key);
    for (int i = 0; i < 4; ++i) out += char((value >> (8 * i)) & 0xFF);
    return out;
}
QByteArray mapField(const QString& key, const QByteArray& body) { return QByteArray(1, '\x00') + cstr(key) + body + QByteArray(1, '\x08'); }
QByteArray tagsField(const QStringList& tags) {
    QByteArray body;
    for (int i = 0; i < tags.size(); ++i) body += strField(QString::number(i), tags[i]);
    return mapField(QStringLiteral("tags"), body);
}
QByteArray fileOf(const QByteArray& entries) { return mapField(QStringLiteral("shortcuts"), entries) + QByteArray(1, '\x08'); }

QByteArray fullShortcut(int index, const QString& name, const QString& exe, quint32 appId, quint32 lastPlay, const QStringList& tags) {
    QByteArray body;
    body += intField(QStringLiteral("appid"), appId);
    body += strField(QStringLiteral("appname"), name);
    body += strField(QStringLiteral("exe"), exe);
    body += strField(QStringLiteral("StartDir"), QStringLiteral("\"C:\\Fixture\\Emu\\\""));
    body += strField(QStringLiteral("icon"), QStringLiteral("C:\\Fixture\\Emu\\icon.png"));
    body += strField(QStringLiteral("ShortcutPath"), QString());
    body += strField(QStringLiteral("LaunchOptions"), QStringLiteral("--fullscreen"));
    body += intField(QStringLiteral("IsHidden"), 0);
    body += intField(QStringLiteral("AllowDesktopConfig"), 1);
    body += intField(QStringLiteral("LastPlayTime"), lastPlay);
    body += tagsField(tags);
    return mapField(QString::number(index), body);
}

// ---- ids ---------------------------------------------------------------------------------------------------------------
void idTests() {
    check(steamCrc32(QByteArray("123456789")) == 0xCBF43926u, "CRC-32 of the standard check string is 0xCBF43926");
    check(steamCrc32(QByteArray()) == 0, "CRC-32 of nothing is 0");
    const QString exe = QStringLiteral("\"C:\\Fixture\\Emu\\emu.exe\"");
    const quint32 computed = steamShortcutComputedAppId(exe, QStringLiteral("Fixture Game"));
    check((computed & 0x80000000u) != 0, "a computed shortcut id has the top bit set");
    check(computed == (steamCrc32((exe + QStringLiteral("Fixture Game")).toUtf8()) | 0x80000000u), "computed id = crc32(exe + appname) | 0x80000000, quotes included");
    check(computed != steamShortcutComputedAppId(QStringLiteral("C:\\Fixture\\Emu\\emu.exe"), QStringLiteral("Fixture Game")), "the quotes around the executable are part of the checksummed text");
    check(steamShortcutGameId64(0x80000001u) == 0x8000000102000000ULL, "game id = (appId << 32) | 0x02000000");
    checkText(QString::number(steamShortcutGameId64(0xDEADBEEFu)), QString::number(0xDEADBEEF02000000ULL), "game id as decimal text");
    quint32 back = 0;
    check(steamShortcutAppIdFromGameId(QString::number(steamShortcutGameId64(0xC0FFEE01u)), &back) && back == 0xC0FFEE01u, "the app id is recovered from its game id");
    check(!isSteamShortcutGameId(QStringLiteral("620")) && !isSteamShortcutGameId(QStringLiteral("33554432")) && !isSteamShortcutGameId(QStringLiteral("")) &&
          !isSteamShortcutGameId(QStringLiteral("abc")) && !isSteamShortcutGameId(QStringLiteral("-1")) && !isSteamShortcutGameId(QString::number(0x8000000100000000ULL)) &&
          !isSteamShortcutGameId(QStringLiteral("99999999999999999999999")), "a Steam app id, zero-high ids, wrong tags and junk are not shortcut game ids");
    checkText(steamShortcutLaunchUri(QString::number(steamShortcutGameId64(0x80000001u))), QStringLiteral("steam://rungameid/9223372041183297536"), "launch uri carries the 64-bit id");
    check(steamShortcutLaunchUri(QStringLiteral("620")).isEmpty(), "no launch uri for a normal app id");
    checkText(steamShortcutRegistryAppId(QString::number(steamShortcutGameId64(0x80000001u))), QStringLiteral("2147483649"), "registry app id is the 32-bit shortcut id");
    check(steamShortcutRegistryAppId(QStringLiteral("620")).isEmpty(), "no registry shortcut id for a normal app id");
}

// ---- parsing -----------------------------------------------------------------------------------------------------------
void parseTests() {
    const QString exeA = QStringLiteral("\"C:\\Fixture\\Emu\\emu.exe\"");
    QByteArray entries;
    entries += fullShortcut(0, QStringLiteral("Fixture Game One"), exeA, 0xA0000001u, 1700000000u, {QStringLiteral("Emulator"), QStringLiteral("Retro")});
    // no appid stored, mixed-case keys, no tags, negative int32 on disk for the last play time
    {
        QByteArray body;
        body += strField(QStringLiteral("AppName"), QStringLiteral("Fixture Game Two"));
        body += strField(QStringLiteral("Exe"), QStringLiteral("\"C:\\Fixture\\two.exe\""));
        body += intField(QStringLiteral("LastPlayTime"), 0xFFFFFFF0u);
        entries += mapField(QStringLiteral("1"), body);
    }
    // only an executable (no name): the row is named after the file
    {
        QByteArray body;
        body += strField(QStringLiteral("exe"), QStringLiteral("\"C:\\Fixture\\Tools\\Viewer Tool.exe\""));
        entries += mapField(QStringLiteral("2"), body);
    }
    // nothing usable: skipped
    entries += mapField(QStringLiteral("3"), intField(QStringLiteral("appid"), 77));
    // hidden: skipped
    {
        QByteArray body;
        body += strField(QStringLiteral("appname"), QStringLiteral("Fixture Hidden"));
        body += strField(QStringLiteral("exe"), QStringLiteral("\"C:\\Fixture\\hidden.exe\""));
        body += intField(QStringLiteral("IsHidden"), 1);
        entries += mapField(QStringLiteral("4"), body);
    }
    // duplicate of the first id: dropped
    entries += fullShortcut(5, QStringLiteral("Fixture Game One Again"), exeA, 0xA0000001u, 0, {});
    const QByteArray file = fileOf(entries);
    const auto list = parseSteamShortcuts(file);
    check(list.error.isEmpty(), "a well-formed file reads without an error");
    check(list.shortcuts.size() == 3, "three shortcuts survive (one unusable, one hidden, one duplicate dropped)");
    check(list.skipped == 2 && list.duplicates == 1, "the dropped ones are counted");
    if (list.shortcuts.size() != 3) return;
    const auto& one = list.shortcuts[0];
    checkText(one.appName, QStringLiteral("Fixture Game One"), "name");
    checkText(one.exe, exeA, "the executable keeps Steam's quotes");
    checkText(one.exePath(), QStringLiteral("C:\\Fixture\\Emu\\emu.exe"), "exePath() strips them");
    checkText(one.startDir, QStringLiteral("\"C:\\Fixture\\Emu\\\""), "start directory");
    checkText(one.icon, QStringLiteral("C:\\Fixture\\Emu\\icon.png"), "icon");
    checkText(one.launchOptions, QStringLiteral("--fullscreen"), "launch options");
    check(one.tags == QStringList({QStringLiteral("Emulator"), QStringLiteral("Retro")}), "tags");
    check(one.appIdStored && one.appId == 0xA0000001u, "the stored app id is used");
    check(one.lastPlayTime == 1700000000u, "last play time");
    checkText(one.gameId(), QString::number(0xA000000102000000ULL), "stored id -> game id");
    checkText(one.rowId(), QStringLiteral("steam-shortcut:2684354561"), "row id");
    const auto& two = list.shortcuts[1];
    checkText(two.appName, QStringLiteral("Fixture Game Two"), "keys are matched without case");
    check(!two.appIdStored && two.appId == steamShortcutComputedAppId(QStringLiteral("\"C:\\Fixture\\two.exe\""), QStringLiteral("Fixture Game Two")),
          "a file without appid falls back to the computed id");
    check(two.lastPlayTime == 0xFFFFFFF0u, "last play time is read as unsigned");
    const auto& three = list.shortcuts[2];
    checkText(three.displayName(), QStringLiteral("Viewer Tool"), "a nameless shortcut is named after its executable");
    // The same file twice in a row (not a duplicate-in-file: two parses agree).
    check(parseSteamShortcuts(file).shortcuts.size() == 3, "parsing is repeatable");

    check(parseSteamShortcuts(QByteArray()).shortcuts.isEmpty() && parseSteamShortcuts(QByteArray()).error.isEmpty(), "an empty file is no shortcuts and no error");
    check(parseSteamShortcuts(fileOf(QByteArray())).shortcuts.isEmpty() && parseSteamShortcuts(fileOf(QByteArray())).error.isEmpty(), "an empty shortcuts map is no shortcuts and no error");
    check(!parseSteamShortcuts(QByteArray("this is not a shortcuts file")).error.isEmpty(), "text that is not the binary format is an error");
    check(!parseSteamShortcuts(mapField(QStringLiteral("other"), QByteArray()) + QByteArray(1, '\x08')).error.isEmpty(), "a file without a shortcuts entry says so");
    // A different top-level entry before "shortcuts" is stepped over.
    const auto prefixed = parseSteamShortcuts(mapField(QStringLiteral("other"), intField(QStringLiteral("x"), 1)) + fileOf(fullShortcut(0, QStringLiteral("Fixture P"), exeA, 0x81111111u, 0, {})));
    check(prefixed.error.isEmpty() && prefixed.shortcuts.size() == 1, "unrelated root entries are skipped");
}

void truncationTests() {
    QByteArray entries;
    entries += fullShortcut(0, QStringLiteral("Fixture Cut A"), QStringLiteral("\"C:\\Fixture\\a.exe\""), 0x81000001u, 5, {QStringLiteral("t")});
    entries += fullShortcut(1, QStringLiteral("Fixture Cut B"), QStringLiteral("\"C:\\Fixture\\b.exe\""), 0x81000002u, 6, {});
    entries += fullShortcut(2, QStringLiteral("Fixture Cut C"), QStringLiteral("\"C:\\Fixture\\c.exe\""), 0x81000003u, 7, {QStringLiteral("x"), QStringLiteral("y")});
    const QByteArray file = fileOf(entries);
    const auto whole = parseSteamShortcuts(file);
    check(whole.error.isEmpty() && whole.shortcuts.size() == 3, "the untruncated fixture reads fully");
    bool prefixOk = true, errorOk = true, neverMore = true;
    int previous = 0;
    for (qsizetype length = 0; length < file.size(); ++length) {
        const auto cut = parseSteamShortcuts(file.left(length));
        // Every shortcut returned from a prefix is a complete, unaltered earlier shortcut.
        for (int i = 0; i < cut.shortcuts.size(); ++i) {
            if (i >= whole.shortcuts.size() || cut.shortcuts[i].appName != whole.shortcuts[i].appName ||
                cut.shortcuts[i].exe != whole.shortcuts[i].exe || cut.shortcuts[i].appId != whole.shortcuts[i].appId || cut.shortcuts[i].tags != whole.shortcuts[i].tags)
                prefixOk = false;
        }
        if (length > 0 && cut.error.isEmpty() && cut.shortcuts.size() != whole.shortcuts.size()) errorOk = false;
        if (cut.shortcuts.size() < previous) neverMore = false;   // a longer prefix never loses a shortcut
        previous = int(cut.shortcuts.size());
    }
    check(prefixOk, "every truncation yields only complete, unaltered leading shortcuts");
    check(errorOk, "a truncation that loses a shortcut always reports an error");
    check(neverMore, "a longer prefix never yields fewer shortcuts");
    check(parseSteamShortcuts(file.left(file.size() - 1)).shortcuts.size() == 3, "losing only the final terminator keeps all three shortcuts");
    check(!parseSteamShortcuts(file.left(file.size() - 40)).error.isEmpty() && parseSteamShortcuts(file.left(file.size() - 40)).shortcuts.size() < 3, "a cut in the last shortcut drops it and reports");
}

void maliciousTests() {
    // A string that never ends.
    {
        QByteArray bad = QByteArray(1, '\x00') + cstr(QStringLiteral("shortcuts")) + QByteArray(1, '\x00') + cstr(QStringLiteral("0")) + QByteArray(1, '\x01') + cstr(QStringLiteral("appname")) + QByteArray(5000, 'A');
        const auto list = parseSteamShortcuts(bad);
        check(list.shortcuts.isEmpty() && !list.error.isEmpty(), "an unterminated string stops the read without reading past the buffer");
    }
    // A string longer than the cap, properly terminated.
    {
        QByteArray body = strField(QStringLiteral("appname"), QString(70000, QLatin1Char('A')));
        const auto list = parseSteamShortcuts(fileOf(mapField(QStringLiteral("0"), body)));
        check(list.shortcuts.isEmpty() && !list.error.isEmpty(), "a 70000-byte string is refused");
    }
    // An int32 with only two bytes left.
    {
        QByteArray bad = QByteArray(1, '\x00') + cstr(QStringLiteral("shortcuts")) + QByteArray(1, '\x00') + cstr(QStringLiteral("0")) + QByteArray(1, '\x02') + cstr(QStringLiteral("appid")) + QByteArray(2, '\x01');
        const auto list = parseSteamShortcuts(bad);
        check(list.shortcuts.isEmpty() && !list.error.isEmpty(), "a cut-off number is an error");
    }
    // An unknown type byte.
    {
        QByteArray body = QByteArray(1, '\x55') + cstr(QStringLiteral("weird")) + strField(QStringLiteral("appname"), QStringLiteral("Fixture After Weird"));
        const auto list = parseSteamShortcuts(fileOf(mapField(QStringLiteral("0"), body)));
        check(list.shortcuts.isEmpty() && !list.error.isEmpty(), "an unknown type stops the read (its length cannot be known)");
    }
    // Deep nesting.
    {
        QByteArray inner = strField(QStringLiteral("appname"), QStringLiteral("Fixture Deep"));
        for (int i = 0; i < 40; ++i) inner = mapField(QStringLiteral("n"), inner);
        const auto list = parseSteamShortcuts(fileOf(mapField(QStringLiteral("0"), inner)));
        check(list.shortcuts.isEmpty() && !list.error.isEmpty(), "nesting 40 deep is refused");
    }
    // Huge entry count: capped.
    {
        QByteArray many;
        for (int i = 0; i < 4200; ++i) many += fullShortcut(i, QStringLiteral("Fixture M%1").arg(i), QStringLiteral("\"C:\\Fixture\\m%1.exe\"").arg(i), 0x82000000u + quint32(i), 0, {});
        const auto list = parseSteamShortcuts(fileOf(many));
        check(list.shortcuts.size() == 4096 && !list.error.isEmpty(), "more than 4096 shortcuts are capped and reported");
    }
    // A key that is a huge string repeated: node cap.
    {
        QByteArray flood;
        for (int i = 0; i < 210000; ++i) flood += intField(QStringLiteral("k"), 1);
        const auto list = parseSteamShortcuts(fileOf(mapField(QStringLiteral("0"), flood)));
        check(list.shortcuts.isEmpty() && !list.error.isEmpty(), "an entry flood hits the node cap instead of the heap");
    }
    // Two shortcuts sharing one stored id.
    {
        QByteArray two = fullShortcut(0, QStringLiteral("Fixture Dup A"), QStringLiteral("\"C:\\a.exe\""), 0x83000001u, 1, {}) +
                         fullShortcut(1, QStringLiteral("Fixture Dup B"), QStringLiteral("\"C:\\b.exe\""), 0x83000001u, 2, {});
        const auto list = parseSteamShortcuts(fileOf(two));
        check(list.shortcuts.size() == 1 && list.duplicates == 1 && list.shortcuts[0].appName == QStringLiteral("Fixture Dup A"), "duplicate ids keep the first shortcut");
    }
    // Same name and exe twice with no stored id: the computed ids collide and the second is dropped.
    {
        QByteArray body;
        body += strField(QStringLiteral("appname"), QStringLiteral("Fixture Same"));
        body += strField(QStringLiteral("exe"), QStringLiteral("\"C:\\same.exe\""));
        const auto list = parseSteamShortcuts(fileOf(mapField(QStringLiteral("0"), body) + mapField(QStringLiteral("1"), body)));
        check(list.shortcuts.size() == 1 && list.duplicates == 1, "identical computed ids are deduplicated");
    }
    // Stored id of zero is treated as not stored.
    {
        QByteArray body = intField(QStringLiteral("appid"), 0) + strField(QStringLiteral("appname"), QStringLiteral("Fixture Zero")) + strField(QStringLiteral("exe"), QStringLiteral("\"C:\\z.exe\""));
        const auto list = parseSteamShortcuts(fileOf(mapField(QStringLiteral("0"), body)));
        check(list.shortcuts.size() == 1 && !list.shortcuts[0].appIdStored && list.shortcuts[0].appId != 0, "a stored id of 0 falls back to the computed id");
    }
    // Deterministic fuzz: random bytes and random single-byte damage never crash and never invent shortcuts.
    {
        QRandomGenerator rng(12345);
        const QByteArray base = fileOf(fullShortcut(0, QStringLiteral("Fixture Fuzz"), QStringLiteral("\"C:\\f.exe\""), 0x84000001u, 9, {QStringLiteral("t")}));
        bool alive = true;
        for (int i = 0; i < 4000; ++i) {
            QByteArray damaged = base;
            const int hits = 1 + int(rng.bounded(4));
            for (int h = 0; h < hits; ++h) damaged[int(rng.bounded(quint32(damaged.size())))] = char(rng.bounded(256));
            const auto list = parseSteamShortcuts(damaged);
            if (list.shortcuts.size() > 1) alive = false;
        }
        for (int i = 0; i < 2000; ++i) {
            QByteArray noise(int(rng.bounded(300)), '\0');
            for (auto& c : noise) c = char(rng.bounded(256));
            (void)parseSteamShortcuts(noise);
        }
        check(alive, "damaged and random input never crashes or fabricates extra shortcuts");
    }
}

// ---- the per-account scan and artwork ----------------------------------------------------------------------------------
void scanTests() {
    const QString root = scratchRoot + QStringLiteral("/root");
    const QString idA = QStringLiteral("76561198000000001");   // account 39734273
    const QString idB = QStringLiteral("76561198000000002");   // account 39734274
    const QString idNone = QStringLiteral("76561198000000099");
    writeFile(root + QStringLiteral("/userdata/39734273/config/shortcuts.vdf"),
              fileOf(fullShortcut(0, QStringLiteral("Fixture A One"), QStringLiteral("\"C:\\Fixture\\a1.exe\""), 0x85000001u, 100, {}) +
                     fullShortcut(1, QStringLiteral("Fixture A Two"), QStringLiteral("\"C:\\Fixture\\a2.exe\""), 0x85000002u, 200, {})));
    writeFile(root + QStringLiteral("/userdata/39734274/config/shortcuts.vdf"),
              fileOf(fullShortcut(0, QStringLiteral("Fixture B Only"), QStringLiteral("\"C:\\Fixture\\b1.exe\""), 0x86000001u, 300, {})));
    const auto a = scanSteamShortcuts(root, idA);
    const auto b = scanSteamShortcuts(root, idB);
    check(a.error.isEmpty() && a.shortcuts.size() == 2 && a.shortcuts[0].appName == QStringLiteral("Fixture A One"), "account A's file is A's");
    check(b.error.isEmpty() && b.shortcuts.size() == 1 && b.shortcuts[0].appName == QStringLiteral("Fixture B Only"), "account B's file is B's");
    const auto none = scanSteamShortcuts(root, idNone);
    check(none.shortcuts.isEmpty() && none.error.isEmpty(), "an account without a file has no shortcuts and no error");
    check(scanSteamShortcuts(root, QString()).shortcuts.isEmpty() && scanSteamShortcuts(QString(), idA).shortcuts.isEmpty() &&
          scanSteamShortcuts(root, QStringLiteral("123")).shortcuts.isEmpty() && scanSteamShortcuts(root, QStringLiteral("not a number")).shortcuts.isEmpty(),
          "no root, no account or a bad account id reads nothing");
    // An oversize file is refused, not read.
    writeFile(root + QStringLiteral("/userdata/39734275/config/shortcuts.vdf"), QByteArray(16 * 1024 * 1024 + 1, 'x'));
    const auto huge = scanSteamShortcuts(root, QStringLiteral("76561198000000003"));
    check(huge.shortcuts.isEmpty() && !huge.error.isEmpty(), "a file over 16 MiB is refused with an error");
    // A damaged file keeps what read before the damage.
    QByteArray good = fileOf(fullShortcut(0, QStringLiteral("Fixture Keep"), QStringLiteral("\"C:\\Fixture\\k.exe\""), 0x87000001u, 1, {}) +
                             fullShortcut(1, QStringLiteral("Fixture Lost"), QStringLiteral("\"C:\\Fixture\\l.exe\""), 0x87000002u, 1, {}));
    writeFile(root + QStringLiteral("/userdata/39734276/config/shortcuts.vdf"), good.left(good.size() - 60));
    const auto damaged = scanSteamShortcuts(root, QStringLiteral("76561198000000004"));
    check(damaged.shortcuts.size() == 1 && damaged.shortcuts[0].appName == QStringLiteral("Fixture Keep") && !damaged.error.isEmpty(), "a damaged file keeps the shortcuts before the damage and reports");
}

void artworkTests() {
    const QString root = scratchRoot + QStringLiteral("/art-root");
    const QString id = QStringLiteral("76561198000000001");
    const QString grid = root + QStringLiteral("/userdata/39734273/config/grid");
    SteamShortcut s;
    s.appName = QStringLiteral("Fixture Art");
    s.exe = QStringLiteral("\"C:\\Fixture\\art.exe\"");
    s.appId = 0x88000001u;
    check(steamShortcutGridArtwork(root, id, s).wide.isEmpty(), "no grid folder, no artwork");
    writeFile(grid + QStringLiteral("/%1.png").arg(s.appId), "wide");
    writeFile(grid + QStringLiteral("/%1p.jpg").arg(s.appId), "portrait");
    writeFile(grid + QStringLiteral("/%1_hero.webp").arg(s.appId), "hero");
    writeFile(grid + QStringLiteral("/%1_logo.png").arg(s.appId), "logo");
    writeFile(grid + QStringLiteral("/%1_icon.jpeg").arg(s.appId), "icon");
    writeFile(grid + QStringLiteral("/%1.png").arg(s.appId + 1), "someone else's");
    const auto art = steamShortcutGridArtwork(root, id, s);
    check(art.wide.endsWith(QStringLiteral("%1.png").arg(s.appId)) && art.portrait.endsWith(QStringLiteral("%1p.jpg").arg(s.appId)) &&
          art.hero.endsWith(QStringLiteral("_hero.webp")) && art.logo.endsWith(QStringLiteral("_logo.png")) && art.icon.endsWith(QStringLiteral("_icon.jpeg")),
          "grid files are found by the 32-bit shortcut id (every kind and extension)");
    SteamShortcut legacy = s;
    legacy.appId = 0x88000009u;
    writeFile(grid + QStringLiteral("/%1.png").arg(legacy.gameId()), "legacy wide");
    check(steamShortcutGridArtwork(root, id, legacy).wide.endsWith(legacy.gameId() + QStringLiteral(".png")), "the older 64-bit game id naming is accepted");
    SteamShortcut other = s;
    other.appId = 0x88000055u;
    check(steamShortcutGridArtwork(root, id, other).wide.isEmpty() && steamShortcutGridArtwork(root, QStringLiteral("76561198000000002"), s).wide.isEmpty(),
          "another shortcut's or another account's artwork is never returned");
    writeFile(grid + QStringLiteral("/%1.png").arg(other.appId), QByteArray());
    check(steamShortcutGridArtwork(root, id, other).wide.isEmpty(), "an empty file is not artwork");
}

void exeMatchTests() {
    SteamShortcut s;
    s.exe = QStringLiteral("\"C:\\Fixture\\Emu\\emu.exe\"");
    check(steamShortcutExeMatches(s, QStringLiteral("C:\\Fixture\\Emu\\emu.exe")), "the same image path matches");
    check(steamShortcutExeMatches(s, QStringLiteral("c:/fixture/emu/EMU.EXE")), "case and slash direction do not matter");
    check(!steamShortcutExeMatches(s, QStringLiteral("C:\\Fixture\\Emu\\other.exe")), "another program in the same folder does not match");
    check(!steamShortcutExeMatches(s, QStringLiteral("C:\\Fixture\\Emu")) && !steamShortcutExeMatches(s, QStringLiteral("C:\\Fixture\\Emu\\emu.exe.bak")), "a folder or a longer name does not match");
    check(!steamShortcutExeMatches(s, QString()) && !steamShortcutExeMatches(SteamShortcut(), QStringLiteral("C:\\x.exe")), "empty paths never match");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    scratchRoot = QDir::fromNativeSeparators(argc > 1 ? QString::fromLocal8Bit(argv[1])
                                                       : QDir::current().filePath(QStringLiteral("steam-shortcuts-scratch")));
    QDir(scratchRoot).removeRecursively();
    QDir().mkpath(scratchRoot);
    if (!QFileInfo(scratchRoot).isDir()) {
        std::fprintf(stderr, "cannot create the scratch directory %s\n", qPrintable(scratchRoot));
        return 2;
    }
    idTests();
    parseTests();
    truncationTests();
    maliciousTests();
    scanTests();
    artworkTests();
    exeMatchTests();
    QDir(scratchRoot).removeRecursively();
    std::printf("steam shortcut tests: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
