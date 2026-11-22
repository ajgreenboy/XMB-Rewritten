// Deterministic Linux Steam discovery and launch hand-off tests.
//
// These pin the two parts of the Linux Steam milestone without touching a real Steam install, a real
// Steam client, or the desktop:
//
//   * scanSteam() read-only discovery of native Linux installs through the normal locations
//     (XMB_STEAM_ROOT override, ~/.steam/steam, ~/.local/share/Steam) and the configured secondary
//     libraries, with the missing-drive diagnostic, dedup, path bounds and installed-only filtering.
//   * steamLaunchUri(), the captured launch hand-off (steam://rungameid/<id>): the exact URI the shell
//     hands to the OS on Linux, asserted as a pure string so no URI is ever opened here.
//
// The fixtures are synthetic: a temporary "Steam root" with libraryfolders.vdf and appmanifest_*.acf
// files written by this test. Build with -DXMB_STEAM_LINUX_BUILD_TESTS=ON.

#include "steam_library.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtGlobal>

#include <cstdio>

using namespace xmb;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    else { std::printf("ok: %s\n", what); }
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(data) == data.size();
}

bool ensureDir(const QString& path) {
    return QDir().mkpath(path);
}

// A Valve KeyValues document. The library scanner's dialect (parseKeyValues in steam_library.cpp)
// reads quoted keys and string values, keys case-insensitively; this text matches the on-disk shape
// of a real libraryfolders.vdf / appmanifest_*.acf.
QByteArray libraryfolders(const QStringList& paths) {
    QString out = QStringLiteral("\"libraryfolders\"\n{\n");
    for (int i = 0; i < paths.size(); ++i) {
        out += QStringLiteral("\t\"%1\"\n\t{\n").arg(i);
        out += QStringLiteral("\t\t\"path\"\t\t\"%1\"\n").arg(paths.at(i));
        out += QStringLiteral("\t\t\"label\"\t\t\"\"\n\t}\n");
    }
    out += QStringLiteral("}\n");
    return out.toUtf8();
}

QByteArray appmanifest(const QString& id, const QString& name, const QString& installdir,
                       const QString& stateFlags = QStringLiteral("4")) {
    QString out = QStringLiteral("\"AppState\"\n{\n");
    out += QStringLiteral("\t\"appid\"\t\t\"%1\"\n").arg(id);
    out += QStringLiteral("\t\"Universe\"\t\t\"1\"\n");
    out += QStringLiteral("\t\"name\"\t\t\"%1\"\n").arg(name);
    out += QStringLiteral("\t\"StateFlags\"\t\t\"%1\"\n").arg(stateFlags);
    out += QStringLiteral("\t\"installdir\"\t\t\"%1\"\n").arg(installdir);
    out += QStringLiteral("}\n");
    return out.toUtf8();
}

// Write one installed game into <library>/steamapps (manifest + common/<installdir>/ directory).
bool addGame(const QString& library, const QString& id, const QString& name, const QString& installdir) {
    const QString steamapps = QDir(library).filePath(QStringLiteral("steamapps"));
    if (!ensureDir(QDir(steamapps).filePath(QStringLiteral("common")))) return false;
    if (!ensureDir(QDir(steamapps).filePath(QStringLiteral("common/") + installdir))) return false;
    return writeFile(QDir(steamapps).filePath(QStringLiteral("appmanifest_%1.acf").arg(id)),
                     appmanifest(id, name, installdir));
}

bool hasGame(const SteamInstallation& scan, const QString& id) {
    for (const auto& g : scan.games)
        if (g.appId == id) return true;
    return false;
}

// ---- launch hand-off URI (captured; never opened) ---------------------------------------------

void testLaunchUri() {
    check(steamLaunchUri(QStringLiteral("400")) == QStringLiteral("steam://rungameid/400"),
          "launch uri for a numeric id");
    check(steamLaunchUri(QStringLiteral(" 440 ")) == QStringLiteral("steam://rungameid/440"),
          "launch uri trims the id");
    check(steamLaunchUri(QStringLiteral("4294967295")) == QStringLiteral("steam://rungameid/4294967295"),
          "launch uri accepts the 32-bit maximum");
    check(steamLaunchUri(QString()).isEmpty(), "launch uri rejects empty id");
    check(steamLaunchUri(QStringLiteral("0")).isEmpty(), "launch uri rejects zero");
    check(steamLaunchUri(QStringLiteral("-1")).isEmpty(), "launch uri rejects negative");
    check(steamLaunchUri(QStringLiteral("abc")).isEmpty(), "launch uri rejects non-numeric");
    check(steamLaunchUri(QStringLiteral("400a")).isEmpty(), "launch uri rejects trailing junk");
    check(steamLaunchUri(QStringLiteral("4294967296")).isEmpty(), "launch uri rejects id above 32 bits");
    check(steamLaunchUri(QStringLiteral("12345678901")).isEmpty(), "launch uri rejects id above 10 digits");
}

// ---- discovery through the explicit Steam-root override ---------------------------------------

void testRootOverride() {
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir for root-override fixture");
    const QString root = dir.filePath(QStringLiteral("root"));
    const QString secondary = dir.filePath(QStringLiteral("secondary"));
    check(addGame(root, QStringLiteral("100"), QStringLiteral("Game One"), QStringLiteral("g100")),
          "root game fixture written");
    check(addGame(secondary, QStringLiteral("200"), QStringLiteral("Game Two"), QStringLiteral("g200")),
          "secondary game fixture written");
    check(writeFile(QDir(root).filePath(QStringLiteral("steamapps/libraryfolders.vdf")),
                    libraryfolders({root, secondary})),
          "root override libraryfolders.vdf written");

    qputenv("XMB_STEAM_ROOT", root.toUtf8());
    const SteamInstallation scan = scanSteam();
    qunsetenv("XMB_STEAM_ROOT");

    check(scan.warnings.isEmpty(), "root override scan has no warnings");
    check(scan.root == QDir::cleanPath(root), "root override names the configured root");
    check(scan.games.size() == 2, "root override discovers the primary and separate library");
    check(hasGame(scan, QStringLiteral("100")) && hasGame(scan, QStringLiteral("200")),
          "root override finds both games (no dedup collapse)");
}

// ---- discovery through the normal ~/.local/share/Steam location ------------------------------

void testHomeLocalShare() {
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir for local-share fixture");
    const QString home = dir.filePath(QStringLiteral("home"));
    const QString steam = QDir(home).filePath(QStringLiteral(".local/share/Steam"));
    check(addGame(steam, QStringLiteral("300"), QStringLiteral("Game Three"), QStringLiteral("g300")),
          "local-share game fixture written");

    qunsetenv("XMB_STEAM_ROOT");
    qputenv("HOME", home.toUtf8());
    const SteamInstallation scan = scanSteam();
    qunsetenv("HOME");

    check(scan.warnings.isEmpty(), "local-share scan has no warnings");
    check(scan.games.size() == 1 && hasGame(scan, QStringLiteral("300")),
          "~/.local/share/Steam is discovered");
}

// ---- discovery through the normal ~/.steam/steam location ------------------------------------

void testHomeDotSteam() {
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir for dot-steam fixture");
    const QString home = dir.filePath(QStringLiteral("home"));
    const QString steam = QDir(home).filePath(QStringLiteral(".steam/steam"));
    check(addGame(steam, QStringLiteral("400"), QStringLiteral("Game Four"), QStringLiteral("g400")),
          "dot-steam game fixture written");

    qunsetenv("XMB_STEAM_ROOT");
    qputenv("HOME", home.toUtf8());
    const SteamInstallation scan = scanSteam();
    qunsetenv("HOME");

    check(scan.warnings.isEmpty(), "dot-steam scan has no warnings");
    check(scan.games.size() == 1 && hasGame(scan, QStringLiteral("400")),
          "~/.steam/steam is discovered");
}

// ---- absent Steam ----------------------------------------------------------------------------

void testAbsentSteam() {
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir for absent-steam fixture");
    const QString home = dir.filePath(QStringLiteral("home"));
    check(ensureDir(home), "empty fake home created");

    qunsetenv("XMB_STEAM_ROOT");
    qputenv("HOME", home.toUtf8());
    const SteamInstallation scan = scanSteam();
    qunsetenv("HOME");

    check(scan.root.isEmpty(), "absent Steam reports no root");
    check(scan.games.isEmpty(), "absent Steam discovers no games");
    check(scan.warnings.size() == 1 &&
              scan.warnings.constFirst().contains(QStringLiteral("Steam was not found")),
          "absent Steam emits the not-found diagnostic");
}

// ---- bad / stale library ---------------------------------------------------------------------

void testBadStaleLibrary() {
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir for bad/stale fixture");
    const QString root = dir.filePath(QStringLiteral("root"));
    const QString gone = dir.filePath(QStringLiteral("gone")); // never created
    check(addGame(root, QStringLiteral("100"), QStringLiteral("Game One"), QStringLiteral("g100")),
          "healthy game fixture written");

    // A stale manifest: installed flag clear (never/not-installed) -> excluded by installed-only filter.
    check(writeFile(QDir(root).filePath(QStringLiteral("steamapps/appmanifest_500.acf")),
                    appmanifest(QStringLiteral("500"), QStringLiteral("Not Installed"),
                                QStringLiteral("g500"), QStringLiteral("0"))),
          "stale (not installed) manifest written");
    // A manifest whose install directory is absent -> excluded by path bounds.
    check(writeFile(QDir(root).filePath(QStringLiteral("steamapps/appmanifest_600.acf")),
                    appmanifest(QStringLiteral("600"), QStringLiteral("Broken Path"),
                                QStringLiteral("missing-dir"))),
          "broken-path manifest written");
    // A configured secondary library that does not exist -> diagnostic, not a hard failure.
    check(writeFile(QDir(root).filePath(QStringLiteral("steamapps/libraryfolders.vdf")),
                    libraryfolders({root, gone})),
          "bad/stale libraryfolders.vdf written");

    qputenv("XMB_STEAM_ROOT", root.toUtf8());
    const SteamInstallation scan = scanSteam();
    qunsetenv("XMB_STEAM_ROOT");

    check(hasGame(scan, QStringLiteral("100")), "healthy game remains visible next to a missing library");
    check(!hasGame(scan, QStringLiteral("500")), "not-installed manifest is filtered out");
    check(!hasGame(scan, QStringLiteral("600")), "manifest outside the common path is filtered out");
    check(scan.games.size() == 1, "bad/stale library yields exactly the healthy game");
    check(scan.warnings.size() == 1 && scan.warnings.constFirst().contains(QStringLiteral("unavailable")),
          "missing library emits the unavailable diagnostic");
    check(!scan.warnings.constFirst().contains(gone), "missing-library diagnostic does not leak the path");
}

// ---- valid separate (secondary) library ------------------------------------------------------

void testSeparateLibrary() {
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir for separate-library fixture");
    const QString root = dir.filePath(QStringLiteral("root"));
    const QString secondary = dir.filePath(QStringLiteral("secondary"));
    check(addGame(root, QStringLiteral("100"), QStringLiteral("Game One"), QStringLiteral("g100")),
          "primary game fixture written");
    check(addGame(secondary, QStringLiteral("200"), QStringLiteral("Game Two"), QStringLiteral("g200")),
          "secondary game fixture written");
    check(addGame(secondary, QStringLiteral("201"), QStringLiteral("Game Two B"), QStringLiteral("g201")),
          "second secondary game fixture written");
    check(writeFile(QDir(root).filePath(QStringLiteral("steamapps/libraryfolders.vdf")),
                    libraryfolders({root, secondary})),
          "separate-library libraryfolders.vdf written");

    qputenv("XMB_STEAM_ROOT", root.toUtf8());
    const SteamInstallation scan = scanSteam();
    qunsetenv("XMB_STEAM_ROOT");

    check(scan.warnings.isEmpty(), "separate library scan has no warnings");
    check(scan.games.size() == 3, "separate library discovers both libraries' games");
    check(hasGame(scan, QStringLiteral("100")) && hasGame(scan, QStringLiteral("200")) &&
              hasGame(scan, QStringLiteral("201")),
          "separate library lists every game once (dedup intact)");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    testLaunchUri();
    testRootOverride();
    testHomeLocalShare();
    testHomeDotSteam();
    testAbsentSteam();
    testBadStaleLibrary();
    testSeparateLibrary();

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
