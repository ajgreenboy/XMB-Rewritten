// Tests for the local-data Steam modules that have no network or UI:
//   - steam_activity.h: scanSteamActivity() over a synthetic localconfig.vdf (LastPlayed / Playtime per app, which
//     key paths count, the XMB_STEAM_ACTIVITY=0 gate) and steamActivityText() (the "12 h 5 min played - Last
//     played ..." line in the three date orders);
//   - steam_overrides.h: user artwork overrides under <data>/artwork (stems, extensions, fallbacks, ignored files);
//   - steam_artwork.h: the pure cache-path helpers SteamArtwork::cachePath / heroCachePath;
//   - steam_collections.h: the local overlay file (favorites, custom collections, launch options) load/save and
//     mergeSteamCollectionsWithOverlay(), the pure part of the collections menu.
//
// Every fixture is built here from fake ids ("Fixture ..." names, SteamID64 76561198000000001); no real Steam data
// is read. The scratch directory is the first argument (QTemporaryDir is avoided: it fails on some machines for an
// environmental reason).

#include "steam_activity.h"
#include "steam_artwork.h"
#include "steam_collections.h"
#include "steam_overrides.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
        std::fprintf(stderr, "FAIL: %s\n  want: %s\n  got:  %s\n", what, qPrintable(want), qPrintable(got));
        ++failures;
    }
}

const char* IdA = "76561198000000001";   // account id 39734273
const char* AcctA = "39734273";

QString scratchRoot;

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// ---- steam_activity ----------------------------------------------------------------------------------------------
void activityScanTests() {
    const QString root = scratchRoot + "/activity-root";
    QDir(root).removeRecursively();
    const QString path = root + QString("/userdata/%1/config/localconfig.vdf").arg(AcctA);
    writeFile(path,
              "\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n\t\t\t\t\"apps\"\n\t\t\t\t{\n"
              "\t\t\t\t\t\"1001\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"LastPlayed\"\t\t\"1790000000\"\n\t\t\t\t\t\t\"Playtime\"\t\t\"125\"\n\t\t\t\t\t\t\"Other\"\t\t\"ignored\"\n\t\t\t\t\t}\n"
              "\t\t\t\t\t\"1002\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"lastplayed\"\t\t\"1700000000\"\n\t\t\t\t\t}\n"                       // keys are case-insensitive
              "\t\t\t\t\t\"1003\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"PLAYTIME\"\t\t\"7\"\n\t\t\t\t\t\t// \"LastPlayed\" \"999\"\n\t\t\t\t\t}\n"   // comment line ignored
              "\t\t\t\t\t\"notanapp\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"Playtime\"\t\t\"50\"\n\t\t\t\t\t}\n"                                  // non-numeric app key
              "\t\t\t\t}\n\t\t\t}\n\t\t\t\"NotSteam\"\n\t\t\t{\n\t\t\t\t\"apps\"\n\t\t\t\t{\n\t\t\t\t\t\"2001\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"Playtime\"\t\t\"99\"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n"   // wrong parent
              "\t\t}\n\t}\n}\n");
    qunsetenv("XMB_STEAM_ACTIVITY");
    auto got = scanSteamActivity(root, IdA);
    check(got.size() == 3 && got.contains("1001") && got.contains("1002") && got.contains("1003"), "activity: the three numeric apps under ...Steam > apps are read");
    check(got.value("1001").lastPlayed == 1790000000 && got.value("1001").playtimeMinutes == 125, "activity: LastPlayed and Playtime");
    check(got.value("1002").lastPlayed == 1700000000 && got.value("1002").playtimeMinutes == 0, "activity: keys match case-insensitively; a missing Playtime is 0");
    check(got.value("1003").playtimeMinutes == 7 && got.value("1003").lastPlayed == 0, "activity: a commented-out LastPlayed is not read");
    check(!got.contains("notanapp"), "activity: a non-numeric app key is ignored");
    check(!got.contains("2001"), "activity: an apps block that is not under Steam is ignored");

    check(scanSteamActivity(root, "76561198000000002").isEmpty(), "activity: another account has no localconfig, so no records");
    check(scanSteamActivity(root, "12345").isEmpty() && scanSteamActivity(root, "").isEmpty() && scanSteamActivity(root, "abc").isEmpty(),
          "activity: an id that is not a SteamID64 reads nothing");
    check(scanSteamActivity("", IdA).isEmpty(), "activity: an empty Steam root reads nothing");
    check(scanSteamActivity(scratchRoot + "/no-such-root", IdA).isEmpty(), "activity: a missing root reads nothing");
    qputenv("XMB_STEAM_ACTIVITY", "0");
    check(scanSteamActivity(root, IdA).isEmpty(), "activity: XMB_STEAM_ACTIVITY=0 turns the play records off");
    qunsetenv("XMB_STEAM_ACTIVITY");
    writeFile(path, "");
    check(scanSteamActivity(root, IdA).isEmpty(), "activity: an empty localconfig yields nothing");
    writeFile(path, "\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n\t\t\t\t\"apps\"\n\t\t\t\t{\n\t\t\t\t\t\"1001\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"Playtime\"\t\t\"5\"\n");   // truncated
    got = scanSteamActivity(root, IdA);
    check(got.value("1001").playtimeMinutes == 5, "activity: a truncated file keeps what was complete before the cut");
}

void activityTextTests() {
    const qint64 noon = QDateTime(QDate(2026, 3, 11), QTime(12, 0)).toSecsSinceEpoch();   // local noon: no timezone edge
    SteamActivity none;
    check(steamActivityText(none, 0).isEmpty(), "text: nothing recorded is an empty line");
    SteamActivity a;
    a.playtimeMinutes = 5;
    checkText(steamActivityText(a, 0), "5 min played", "text: minutes only");
    a.playtimeMinutes = 60;
    checkText(steamActivityText(a, 0), "1 h played", "text: whole hours");
    a.playtimeMinutes = 125;
    checkText(steamActivityText(a, 0), "2 h 5 min played", "text: hours and minutes");
    a.lastPlayed = noon;
    checkText(steamActivityText(a, 0), "2 h 5 min played - Last played 11/03/2026", "text: DD/MM/YYYY");
    checkText(steamActivityText(a, 1), "2 h 5 min played - Last played 03/11/2026", "text: MM/DD/YYYY");
    checkText(steamActivityText(a, 2), "2 h 5 min played - Last played 2026/03/11", "text: YYYY/MM/DD");
    checkText(steamActivityText(a, 9), "2 h 5 min played - Last played 03/11/2026", "text: an unknown date order uses MM/DD/YYYY");
    a.playtimeMinutes = 0;
    checkText(steamActivityText(a, 0), "Last played 11/03/2026", "text: last played alone");
    a.lastPlayed = 86400;
    check(steamActivityText(a, 0).isEmpty(), "text: a timestamp within the first day of the epoch counts as 'never'");
    a.lastPlayed = -5;
    check(steamActivityText(a, 0).isEmpty(), "text: a negative timestamp counts as 'never'");
}

// ---- steam_overrides ---------------------------------------------------------------------------------------------
void overrideTests() {
    const QString data = scratchRoot + "/override-data";
    QDir(data).removeRecursively();
    const QString art = data + "/artwork";
    const QByteArray pixel("x");
    writeFile(art + "/123.png", pixel);
    writeFile(art + "/123.jpg", pixel);       // png wins over jpg
    writeFile(art + "/123_icon.jpg", pixel);
    writeFile(art + "/123_bg.webp", pixel);
    writeFile(art + "/200.jpeg", pixel);      // row only: icon and background fall back to it
    writeFile(art + "/300_icon.png", pixel);  // icon only: no row, no background
    writeFile(art + "/400_bg.png", pixel);
    writeFile(art + "/500.png", "");          // zero bytes: ignored
    writeFile(art + "/readme.png", pixel);    // not an app id
    writeFile(art + "/123_other.png", pixel); // unknown suffix
    writeFile(art + "/600.txt", pixel);       // not an image
    writeFile(art + "/0.png", pixel);         // app id 0
    writeFile(art + "/_icon.png", pixel);     // empty stem

    auto o = steamArtworkOverride(data, "123");
    check(QFileInfo(o.row).fileName() == "123.png" && QFileInfo(o.icon).fileName() == "123_icon.jpg" && QFileInfo(o.background).fileName() == "123_bg.webp", "override: row, icon and background come from their own stems; png beats jpg");
    check(QFileInfo(o.row).isAbsolute(), "override: paths are absolute");
    o = steamArtworkOverride(data, "200");
    check(QFileInfo(o.row).fileName() == "200.jpeg" && o.icon == o.row && o.background == o.row, "override: a bare row image also stands in as icon and background");
    o = steamArtworkOverride(data, "300");
    check(o.row.isEmpty() && QFileInfo(o.icon).fileName() == "300_icon.png" && o.background.isEmpty(), "override: an icon alone leaves row and background empty");
    o = steamArtworkOverride(data, "400");
    check(o.row.isEmpty() && o.icon.isEmpty() && QFileInfo(o.background).fileName() == "400_bg.png", "override: a background alone");
    o = steamArtworkOverride(data, "500");
    check(o.row.isEmpty() && o.icon.isEmpty() && o.background.isEmpty(), "override: a zero-byte file is ignored");
    o = steamArtworkOverride(data, " 200 ");
    check(QFileInfo(o.row).fileName() == "200.jpeg", "override: the app id is trimmed");
    o = steamArtworkOverride(data, "0200");
    check(QFileInfo(o.row).fileName() == "200.jpeg", "override: a zero-padded id is normalised to its number");
    for (const char* bad : {"", "abc", "0", "-5", "12 34", "1e3"}) {
        const auto b = steamArtworkOverride(data, bad);
        check(b.row.isEmpty() && b.icon.isEmpty() && b.background.isEmpty(), "override: an invalid app id yields nothing");
    }
    check(steamArtworkOverride("", "123").row.isEmpty(), "override: an empty data directory yields nothing");
    check(steamArtworkOverride(scratchRoot + "/no-such-data", "123").row.isEmpty(), "override: a missing artwork folder yields nothing");

    const auto entries = scanSteamArtworkOverrides(data);
    QHash<QString, SteamArtworkEntry> byId;
    for (const auto& e : entries) byId.insert(e.appId, e);
    check(entries.size() == byId.size(), "scan: one entry per app id");
    check(byId.contains("123") && byId.contains("200") && byId.contains("300") && byId.contains("400"), "scan: every app id with a usable file is listed");
    check(!byId.contains("500") && !byId.contains("600") && !byId.contains("0") && !byId.contains("readme") && !byId.contains(""), "scan: empty files, non-images, id 0, stray names and empty stems are skipped");
    check(byId.size() == 4, "scan: nothing else is listed");
    // For 123 both 123.jpg and 123.png exist; the scan keeps whichever sorts last by name (png), which matches the picker's png-first rule.
    check(QFileInfo(byId.value("123").row).suffix() == "png" && QFileInfo(byId.value("123").icon).fileName() == "123_icon.jpg" &&
              QFileInfo(byId.value("123").background).fileName() == "123_bg.webp", "scan: slots are filled from the stem suffixes");
    check(byId.value("300").row.isEmpty() && !byId.value("300").icon.isEmpty(), "scan: no fallbacks are applied in the report");
    check(scanSteamArtworkOverrides("").isEmpty() && scanSteamArtworkOverrides(scratchRoot + "/no-such-data").isEmpty(), "scan: no data directory or no artwork folder is an empty report");
}

// ---- steam_artwork pure helpers ----------------------------------------------------------------------------------
void artworkPathTests() {
    const QString cache = scratchRoot + "/art-cache";
    checkText(SteamArtwork::cachePath(cache, "220"), QDir::cleanPath(cache + "/220.image"), "artwork: cache path is <dir>/<id>.image");
    checkText(SteamArtwork::heroCachePath(cache, "220"), QDir::cleanPath(cache + "/220_hero.image"), "artwork: hero cache path is <dir>/<id>_hero.image");
    checkText(SteamArtwork::cachePath(cache + "/sub/../art-cache2", "7"), QDir::cleanPath(cache + "/art-cache2/7.image"), "artwork: the cache directory is cleaned");
    check(SteamArtwork::cachePath(cache, "").isEmpty() && SteamArtwork::cachePath(cache, "0").isEmpty() && SteamArtwork::cachePath(cache, "-1").isEmpty() &&
              SteamArtwork::cachePath(cache, "abc").isEmpty() && SteamArtwork::cachePath(cache, "12 3").isEmpty() && SteamArtwork::cachePath(cache, " 12").isEmpty() &&
              SteamArtwork::cachePath(cache, "1234567890123").isEmpty() && SteamArtwork::cachePath(cache, "../220").isEmpty(),
          "artwork: ids that are not 1-12 digits (or are 0) have no cache path, so a hostile id cannot escape the cache folder");
    check(!SteamArtwork::cachePath(cache, "123456789012").isEmpty(), "artwork: a 12-digit id is accepted");
    check(SteamArtwork::cachePath("", "220").isEmpty() && SteamArtwork::cachePath("   ", "220").isEmpty(), "artwork: an empty cache directory has no path");
    check(SteamArtwork::heroCachePath(cache, "x").isEmpty() && SteamArtwork::heroCachePath("", "1").isEmpty(), "artwork: the hero path is guarded the same way");
}

// ---- steam_collections overlay -----------------------------------------------------------------------------------
QStringList namesOf(const SteamCollectionList& list) {
    QStringList out;
    for (const auto& c : list) out << c.name;
    return out;
}
const SteamCollection* byName(const SteamCollectionList& list, const QString& name) {
    for (const auto& c : list)
        if (c.name == name) return &c;
    return nullptr;
}

void overlayFileTests() {
    const QString data = scratchRoot + "/overlay-data";
    QDir(data).removeRecursively();
    const QString file = data + "/steam-collections-local.json";
    check(loadSteamUserOverlay(data).isEmpty() && loadSteamUserOverlay("").isEmpty(), "overlay: a missing file or no data directory loads empty");
    SteamUserOverlay o;
    check(!saveSteamUserOverlay("", o) && !saveSteamUserOverlay("   ", o), "overlay: saving without a data directory fails");
    o.favorites = {"2", "10", "1"};
    o.customCollections.insert("Co-op", QStringList{"5", "3"});
    o.customCollections.insert("Empty One", QStringList{});
    o.launchOptions.insert("220", "-windowed -novid");
    o.launchOptions.insert("221", "   ");   // blank options are not saved
    check(saveSteamUserOverlay(data, o), "overlay: save creates the folder and writes the file");
    const QJsonObject root = QJsonDocument::fromJson(readFile(file)).object();
    check(root.value("version").toInt() == 1, "overlay: the file carries version 1");
    QStringList favs;
    for (const auto& v : root.value("favorites").toArray()) favs << v.toString();
    check(favs == QStringList({"1", "10", "2"}), "overlay: favorites are written sorted, so the file is stable");
    check(!root.value("launchOptions").toObject().contains("221"), "overlay: a blank launch option is dropped on save");
    const SteamUserOverlay back = loadSteamUserOverlay(data);
    check(back.favorites == o.favorites, "overlay: favorites round-trip");
    check(back.customCollections.value("Co-op") == QStringList({"5", "3"}), "overlay: collection member order round-trips");
    check(back.customCollections.contains("Empty One") && back.customCollections.value("Empty One").isEmpty(), "overlay: an empty collection survives");
    check(back.launchOptions.size() == 1 && back.launchOptions.value("220") == "-windowed -novid", "overlay: launch options round-trip");
    check(!back.isEmpty() && SteamUserOverlay().isEmpty(), "overlay: isEmpty()");
    const QByteArray first = readFile(file);
    saveSteamUserOverlay(data, back);
    check(readFile(file) == first, "overlay: save is deterministic (a load/save cycle changes no byte)");

    // Tolerant loading of a hand-edited file.
    writeFile(file, R"({"favorites":[" 7 ","","7",5,"8"],"collections":{"  Spaced  ":["1"," 1 ","2",""],"":["9"],"NotArray":3},"launchOptions":{"10":"  -x  ","":"y","11":""}})");
    const SteamUserOverlay t = loadSteamUserOverlay(data);
    check(t.favorites == QSet<QString>({"7", "8"}), "overlay load: favorites are trimmed, blanks and non-strings dropped");
    check(t.customCollections.size() == 1 && t.customCollections.value("Spaced") == QStringList({"1", "2"}), "overlay load: names trimmed, members trimmed and de-duplicated, blank/invalid collections dropped");
    check(t.launchOptions.size() == 1 && t.launchOptions.value("10") == "-x", "overlay load: launch options trimmed; empty ids or empty values dropped");
    writeFile(file, QByteArray("{\"launchOptions\":{\"12\":\"") + QByteArray(2000, 'z') + "\"}}");
    check(loadSteamUserOverlay(data).launchOptions.value("12").size() == 1024, "overlay load: launch options are capped at 1024 characters");
    writeFile(file, QByteArray("{\"collections\":{\"") + QByteArray(300, 'n') + "\":[\"1\"]}}");
    const auto longNames = loadSteamUserOverlay(data).customCollections.keys();
    check(longNames.size() == 1 && longNames[0].size() == 120, "overlay load: collection names are capped at 120 characters");
    writeFile(file, "not json at all");
    check(loadSteamUserOverlay(data).isEmpty(), "overlay load: invalid JSON is empty");
    writeFile(file, "[1,2,3]");
    check(loadSteamUserOverlay(data).isEmpty(), "overlay load: a non-object root is empty");
    writeFile(file, "");
    check(loadSteamUserOverlay(data).isEmpty(), "overlay load: an empty file is empty");
}

void mergeTests() {
    SteamCollection base1{"uc-b", "Beta", {"1", "2"}};
    SteamCollection base2{"uc-a", "alpha", {"3"}};
    const SteamCollectionList base{base1, base2};   // deliberately not sorted: the Steam order is kept when nothing merges
    SteamUserOverlay none;
    none.launchOptions.insert("1", "-x");
    const auto same = mergeSteamCollectionsWithOverlay(base, none);
    check(namesOf(same) == QStringList({"Beta", "alpha"}), "merge: an overlay with only launch options returns the base list untouched, in its own order");

    SteamUserOverlay o;
    o.favorites = {"9", "3", "1"};
    o.customCollections.insert("BETA", QStringList{"2", "7"});               // joins the existing "Beta" case-insensitively
    o.customCollections.insert("  Road Trip!  ", QStringList{"4", "4", ""});  // new collection
    o.customCollections.insert("   ", QStringList{"8"});                    // blank name: ignored
    const auto merged = mergeSteamCollectionsWithOverlay(base, o);
    check(namesOf(merged) == QStringList({"alpha", "Beta", "Favorites", "Road Trip!"}), "merge: the result is sorted by name (locale-aware) and the blank name is ignored");
    const auto* beta = byName(merged, "Beta");
    check(beta && beta->id == "uc-b" && beta->appIds == QVector<QString>({"1", "2", "7"}), "merge: members are appended to the matching Steam collection, without duplicates, keeping its id");
    const auto* fav = byName(merged, "Favorites");
    check(fav && fav->id == "xmb-favorites" && fav->appIds == QVector<QString>({"1", "3", "9"}), "merge: favorites become a 'Favorites' collection with sorted members");
    const auto* trip = byName(merged, "Road Trip!");
    check(trip && trip->id == "xmb-road-trip" && trip->appIds == QVector<QString>({"4"}), "merge: a new collection gets a slug id, trimmed name, de-duplicated members");

    // A Steam collection already called Favorites receives the favorites instead of a second one appearing.
    SteamCollectionList steamFav{SteamCollection{"uc-f", "favorites", {"1", "5"}}};
    SteamUserOverlay fo;
    fo.favorites = {"5", "6"};
    const auto m2 = mergeSteamCollectionsWithOverlay(steamFav, fo);
    check(m2.size() == 1 && m2[0].id == "uc-f" && m2[0].appIds == QVector<QString>({"1", "5", "6"}), "merge: favorites join an existing 'favorites' collection (case-insensitive)");

    // Slug rules, through the ids of new collections.
    SteamUserOverlay s;
    s.customCollections.insert("A  &  B", QStringList{"1"});
    s.customCollections.insert("!!!", QStringList{"1"});
    s.customCollections.insert("Ünï 2", QStringList{"1"});
    const auto sm = mergeSteamCollectionsWithOverlay({}, s);
    check(byName(sm, "A  &  B") && byName(sm, "A  &  B")->id == "xmb-a-b", "slug: runs of punctuation collapse to one dash");
    check(byName(sm, "!!!") && byName(sm, "!!!")->id == "xmb-collection", "slug: a name with no letters or digits gets the generic id");
    check(byName(sm, QString::fromUtf8("\xC3\x9C" "n\xC3\xAF 2")) && byName(sm, QString::fromUtf8("\xC3\x9C" "n\xC3\xAF 2"))->id == QString::fromUtf8("xmb-\xC3\xBCn\xC3\xAF-2"), "slug: letters outside ASCII are kept and lower-cased");

    // Names are capped at 120 characters.
    SteamUserOverlay l;
    l.customCollections.insert(QString(200, 'q'), QStringList{"1"});
    const auto lm = mergeSteamCollectionsWithOverlay({}, l);
    check(lm.size() == 1 && lm[0].name.size() == 120, "merge: a collection name is capped at 120 characters");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    scratchRoot = QDir::fromNativeSeparators(argc > 1 ? QString::fromLocal8Bit(argv[1])
                                                       : QDir::current().filePath(QStringLiteral("steam-local-scratch")));
    QDir(scratchRoot).removeRecursively();
    QDir().mkpath(scratchRoot);
    if (!QFileInfo(scratchRoot).isDir()) {
        std::fprintf(stderr, "cannot create the scratch directory %s\n", qPrintable(scratchRoot));
        return 2;
    }
    activityScanTests();
    activityTextTests();
    overrideTests();
    artworkPathTests();
    overlayFileTests();
    mergeTests();
    QDir(scratchRoot).removeRecursively();
    std::printf("steam local tests: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
