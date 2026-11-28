// Tests for steam_owned.h: the Web API key lookup order (environment, then the key file), the "needs a key" path,
// the Valve GetOwnedGames response parsing (title sort, installed flag from local manifests, icon/header urls,
// unusable rows), every HTTP/body failure class, the request shape (the key travels in a header, never in the
// URL), cancellation, and the XMB_STEAM_OWNED_FILE fixture seam the render gate uses.
//
// The network path talks to a loopback server (tests/steam_http_fixture.h) through the XMB_STEAM_API_BASE test
// seam, which only accepts a plain-http loopback origin. Every id, title, key and response here is synthetic;
// the Steam root is a scratch directory named by XMB_STEAM_ROOT, the registry key is a dead scratch key, the key
// folder is redirected with XMB_API_KEY_DIR, and nothing real is read. The scratch directory is the first
// argument (QTemporaryDir is avoided: it fails on some machines for an environmental reason).

#include "steam_http_fixture.h"
#include "steam_owned.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace xmb;
using steamtest::HttpFixture;
using steamtest::HttpReply;

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

const char* IdA = "76561198000000001";
const char* FakeKey = "00112233445566778899AABBCCDDEEFF";   // 32 hex characters, obviously not a real key

QString scratchRoot;
QString steamRoot;
QString keyDir;

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size();
}

struct Loaded {
    OwnedGameList games;
    QString error;
    bool needsKey = false;
};

struct Probe {
    explicit Probe(SteamOwned& owned) {
        QObject::connect(&owned, &SteamOwned::loaded, &owned, [this](OwnedGameList games, QString error, bool needsKey) {
            results.push_back({games, error, needsKey});
        });
        QObject::connect(&owned, &SteamOwned::busyChanged, &owned, [this](bool busy) { busy_.push_back(busy); });
    }
    QVector<Loaded> results;
    QVector<bool> busy_;
    void reset() { results.clear(); busy_.clear(); }
};

QByteArray body(const QJsonObject& response) { return QJsonDocument(QJsonObject{{"response", response}}).toJson(QJsonDocument::Compact); }

QJsonObject game(int id, const QString& name, int playtime = 0, const QString& icon = QStringLiteral("deadbeef")) {
    QJsonObject o{{"appid", id}, {"name", name}, {"playtime_forever", playtime}};
    if (!icon.isEmpty()) o["img_icon_url"] = icon;
    return o;
}

bool refreshAndWait(SteamOwned& owned, Probe& probe, const QString& id) {
    probe.reset();
    owned.refresh(id);
    return steamtest::waitFor([&] { return !probe.results.isEmpty(); });
}

void buildSteamRoot() {
    QDir(steamRoot).removeRecursively();
    QDir().mkpath(steamRoot + "/steamapps/common/Dir2002");
    writeFile(steamRoot + "/config/loginusers.vdf",
              QByteArray("\"users\"\n{\n\t\"") + IdA + "\"\n\t{\n\t\t\"AccountName\"\t\t\"fixture_a\"\n\t\t\"RememberPassword\"\t\t\"1\"\n\t\t\"MostRecent\"\t\t\"1\"\n\t\t\"timestamp\"\t\t\"1790000000\"\n\t}\n}\n");
    writeFile(steamRoot + "/steamapps/appmanifest_2002.acf",
              "\"AppState\"\n{\n\t\"appid\"\t\t\"2002\"\n\t\"name\"\t\t\"Fixture Installed\"\n\t\"StateFlags\"\t\t\"4\"\n\t\"installdir\"\t\t\"Dir2002\"\n}\n");
}

void keyTests() {
    qunsetenv("STEAM_WEB_API_KEY");
    qputenv("XMB_API_KEY_DIR", QDir::toNativeSeparators(keyDir).toLocal8Bit());
    QDir(keyDir).removeRecursively();
    QDir().mkpath(keyDir);
    const QString file = SteamOwned::apiKeyFilePath();
    check(!file.isEmpty() && QDir::fromNativeSeparators(file).startsWith(keyDir), "the key file lives in XMB_API_KEY_DIR");
    check(SteamOwned::apiKey().isEmpty() && !SteamOwned::hasApiKey(), "no env and no file: no key");
    qputenv("XMB_STEAM_OWNED_FILE", "somewhere.json");
    check(SteamOwned::hasApiKey(), "the owned-file seam counts as 'has a key' so the shell does not show the setup hint");
    qunsetenv("XMB_STEAM_OWNED_FILE");

    writeFile(file, QByteArray(FakeKey) + "\r\nsecond line is ignored\r\n");
    checkText(SteamOwned::apiKey(), FakeKey, "the key file's first line is the key (CRLF tolerated)");
    check(SteamOwned::hasApiKey(), "a key file means there is a key");
    writeFile(file, QByteArray("   ") + FakeKey + "   ");
    checkText(SteamOwned::apiKey(), FakeKey, "surrounding whitespace is trimmed");
    writeFile(file, QByteArray(5000, 'a'));
    check(SteamOwned::apiKey().isEmpty(), "a key file over 4096 bytes is refused");
    writeFile(file, "");
    check(SteamOwned::apiKey().isEmpty(), "an empty key file is no key");

    writeFile(file, "FILE0011223344556677889900AABBCC");
    qputenv("STEAM_WEB_API_KEY", QByteArray("  ") + FakeKey + " ");
    checkText(SteamOwned::apiKey(), FakeKey, "the environment variable wins over the file and is trimmed");
    qputenv("STEAM_WEB_API_KEY", "   ");
    checkText(SteamOwned::apiKey(), "FILE0011223344556677889900AABBCC", "a blank environment variable falls through to the file");
    qunsetenv("STEAM_WEB_API_KEY");
    QFile::remove(file);
}

void staticTests() {
    checkText(SteamOwned::installUrl("220"), "steam://install/220", "installUrl");
    checkText(SteamOwned::launchUrl(" 220 "), "steam://rungameid/220", "launchUrl trims");
    checkText(SteamOwned::storeUrl("220"), "https://store.steampowered.com/app/220", "storeUrl");
    check(SteamOwned::installUrl("").isEmpty() && SteamOwned::launchUrl("0").isEmpty() && SteamOwned::storeUrl("-1").isEmpty() &&
              SteamOwned::launchUrl("12ab").isEmpty() && SteamOwned::installUrl("abc").isEmpty(),
          "the hand-off urls refuse empty, zero, negative and non-numeric ids");
}

void noKeyTests(SteamOwned& owned, Probe& probe, HttpFixture& server) {
    qunsetenv("STEAM_WEB_API_KEY");
    const int before = server.requestCount();
    probe.reset();
    owned.refresh(IdA);
    check(probe.results.size() == 1 && probe.results[0].needsKey && probe.results[0].games.isEmpty() && probe.results[0].error.contains("STEAM_WEB_API_KEY"),
          "without a key: loaded() with needsKey=true, no games, and no request");
    check(server.requestCount() == before && probe.busy_.isEmpty(), "without a key nothing is sent and busy never flips");
    checkText(owned.requestedId(), IdA, "requestedId() names the account that was asked for");

    // Empty id: the account scanSteam() selects (the fixture's single login user).
    probe.reset();
    owned.refresh("   ");
    checkText(owned.requestedId(), IdA, "an empty id falls back to the Steam account the scan selects");
    // No usable account at all: an explicit message that is not the key hint.
    const QString bareRoot = scratchRoot + "/bare-root";
    QDir().mkpath(bareRoot + "/steamapps");
    qputenv("XMB_STEAM_ROOT", QDir::toNativeSeparators(bareRoot).toLocal8Bit());
    probe.reset();
    owned.refresh("");
    check(probe.results.size() == 1 && !probe.results[0].needsKey && probe.results[0].error.contains("No Steam account") && owned.requestedId().isEmpty(),
          "no account found: 'No Steam account was found', needsKey=false");
    qputenv("XMB_STEAM_ROOT", QDir::toNativeSeparators(steamRoot).toLocal8Bit());
}

void networkTests(SteamOwned& owned, Probe& probe, HttpFixture& server) {
    qputenv("STEAM_WEB_API_KEY", FakeKey);
    QJsonArray rows{game(300, "Zeta Fixture", 90), game(2002, "Fixture Installed", 125, ""), game(301, "alpha Fixture", 0),
                    game(0, "Fixture Zero Id"), game(302, "   "), QJsonObject{{"appid", 303}},   // dropped: id 0, blank title, no title
                    QJsonValue(7), game(304, "  Fixture Padded  ", 5)};
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, body(QJsonObject{{"game_count", rows.size()}, {"games", rows}})}; });
    check(refreshAndWait(owned, probe, QString("  ") + IdA + "  "), "a valid response loads");
    if (probe.results.isEmpty()) return;
    const Loaded& r = probe.results.first();
    check(r.error.isEmpty() && !r.needsKey, "no error and no needsKey on success");
    QStringList titles;
    for (const OwnedGame& g : r.games) titles << g.title;
    check(titles == QStringList({"alpha Fixture", "Fixture Installed", "Fixture Padded", "Zeta Fixture"}), "games are sorted by title (locale-aware, case-insensitive); unusable rows are dropped");
    if (r.games.size() == 4) {
        const OwnedGame* installed = nullptr;
        const OwnedGame* zeta = nullptr;
        for (const OwnedGame& g : r.games) { if (g.appId == "2002") installed = &g; if (g.appId == "300") zeta = &g; }
        check(installed && installed->installed && installed->playtimeMinutes == 125, "a game with a local manifest is installed, playtime is carried");
        check(installed && installed->iconUrl.isEmpty() && installed->headerUrl == "https://cdn.cloudflare.steamstatic.com/steam/apps/2002/header.jpg", "no icon hash means no icon url; the header url is derived from the id");
        check(zeta && !zeta->installed && zeta->playtimeMinutes == 90 &&
                  zeta->iconUrl == "https://media.steampowered.com/steamcommunity/public/images/apps/300/deadbeef.jpg",
              "an owned game that is not installed says so; the icon url is built from the hash");
    }
    check(probe.busy_ == QVector<bool>({true, false}), "busyChanged true then false");
    checkText(owned.requestedId(), IdA, "the requested id is trimmed");

    // Request shape: the key is in a header, never in the URL.
    const QByteArray line = server.lastRequestLine();
    check(line.startsWith("GET /IPlayerService/GetOwnedGames/v1/?") && line.contains(QByteArray("steamid=") + IdA) &&
              line.contains("include_appinfo=1") && line.contains("include_played_free_games=1") && line.contains("format=json"),
          "request: GetOwnedGames with steamid, include_appinfo, include_played_free_games and format=json");
    check(!line.contains(FakeKey) && !line.toLower().contains("key="), "request: the API key is not in the URL");
    check(server.lastHeaders().toLower().contains(QByteArray("x-webapi-key: ").append(QByteArray(FakeKey).toLower())) ||
              server.lastHeaders().contains(QByteArray("x-webapi-key: ") + FakeKey),
          "request: the key is sent in the x-webapi-key header");
    check(server.lastHeaders().toLower().contains("user-agent: xmb-native"), "request: xmb-native user agent");

    // The list's own cap is 4096 rows.
    QJsonArray many;
    for (int i = 0; i < 4200; ++i) many.append(game(10000 + i, QString("Fixture Bulk %1").arg(i, 4, 10, QLatin1Char('0'))));
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, body(QJsonObject{{"games", many}})}; });
    refreshAndWait(owned, probe, IdA);
    check(probe.results.first().games.size() == 4096, "at most 4096 games are returned");
}

void failureTests(SteamOwned& owned, Probe& probe, HttpFixture& server) {
    qputenv("STEAM_WEB_API_KEY", FakeKey);
    const auto expect = [&](int status, const QByteArray& payload, const char* contains, bool needsKey, const char* what) {
        server.setHandler([=](const QByteArray&, const QByteArray&) { return HttpReply{status, payload}; });
        refreshAndWait(owned, probe, IdA);
        const bool ok = probe.results.size() == 1 && probe.results[0].games.isEmpty() && probe.results[0].needsKey == needsKey &&
                        probe.results[0].error.contains(contains);
        if (!ok && !probe.results.isEmpty()) std::fprintf(stderr, "  (%s) got error: %s\n", what, qPrintable(probe.results[0].error));
        check(ok, what);
        check(!probe.busy_.isEmpty() && probe.busy_.last() == false, "busy ends false after a failure");
    };
    expect(401, "{}", "rejected the API key", false, "HTTP 401: the key is rejected");
    expect(403, "{}", "rejected the API key", false, "HTTP 403: the key is rejected");
    expect(400, "{}", "account id", false, "HTTP 400: the SteamID64 is malformed");
    expect(500, "{}", "could not be reached", false, "HTTP 500: could not be reached");
    expect(404, "{}", "could not be reached", false, "HTTP 404: could not be reached");
    expect(429, "{}", "could not be reached", false, "HTTP 429: could not be reached");
    expect(200, "<html>login</html>", "unexpected response", false, "HTML 200: unexpected response");
    expect(200, "[]", "unexpected response", false, "JSON array 200: unexpected response");
    expect(200, "{\"response\":{}}", "owns no games, or its game details are private", false, "private profile ({}) is the owns-nothing message");
    expect(200, "{\"response\":{\"game_count\":0}}", "owns no games", false, "game_count 0 is the owns-nothing message");
    expect(200, "{}", "owns no games", false, "a missing response object is treated as an empty profile");
    expect(200, "{\"response\":{\"game_count\":3}}", "no game list", false, "game_count without games is 'returned no game list'");

    // An empty games array is a successful, empty library (no error).
    server.setHandler([](const QByteArray&, const QByteArray&) { return HttpReply{200, "{\"response\":{\"game_count\":0,\"games\":[]}}"}; });
    refreshAndWait(owned, probe, IdA);
    check(probe.results.size() == 1 && probe.results[0].games.isEmpty() && probe.results[0].error.isEmpty() && !probe.results[0].needsKey, "an empty games array is an empty library without an error");

    // A body over 8 MiB is refused before parsing, and reported once.
    const QByteArray huge = "{\"response\":{\"games\":[],\"pad\":\"" + QByteArray(8 * 1024 * 1024 + 1024, 'a') + "\"}}";
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, huge}; });
    refreshAndWait(owned, probe, IdA);
    check(probe.results.size() == 1 && probe.results[0].error.contains("larger than expected"), "a response over 8 MiB fails with 'larger than expected'");
    steamtest::settle();
    check(probe.results.size() == 1, "the oversize failure is reported once");

    // Connection refused.
    SteamOwned other;
    Probe otherProbe(other);
    QByteArray deadOrigin;
    {
        HttpFixture temporary;
        temporary.listen();
        deadOrigin = temporary.origin();
    }
    qputenv("XMB_STEAM_API_BASE", deadOrigin);
    other.refresh(IdA);
    check(steamtest::waitFor([&] { return !otherProbe.results.isEmpty(); }) && otherProbe.results[0].error.contains("could not be reached") && !otherProbe.results[0].needsKey,
          "a refused connection is 'could not be reached'");
    qputenv("XMB_STEAM_API_BASE", server.origin());
}

void cancelTests(SteamOwned& owned, Probe& probe, HttpFixture& server) {
    qputenv("STEAM_WEB_API_KEY", FakeKey);
    server.setHandler([&](const QByteArray&, const QByteArray&) { HttpReply r; r.neverAnswer = true; return r; });
    probe.reset();
    const int before = server.requestCount();
    owned.refresh(IdA);
    check(steamtest::waitFor([&] { return server.requestCount() > before; }), "the held request reaches the server");
    owned.cancel();
    server.closeHeldConnections();
    steamtest::settle();
    check(probe.results.isEmpty(), "cancel(): no loaded() for the abandoned request");
    check(probe.busy_ == QVector<bool>({true, false}), "cancel(): busyChanged(false) is emitted");

    // A newer refresh supersedes an older one still in flight: exactly one loaded(), for the newer account.
    int served = 0;
    server.setHandler([&](const QByteArray& line, const QByteArray&) {
        ++served;
        if (line.contains("steamid=76561198000000001")) { HttpReply r; r.neverAnswer = true; return r; }
        return HttpReply{200, body(QJsonObject{{"games", QJsonArray{game(400, "Fixture Newer")}}})};
    });
    probe.reset();
    owned.refresh("76561198000000001");
    steamtest::waitFor([&] { return served >= 1; });
    owned.refresh("76561198000000002");
    check(steamtest::waitFor([&] { return !probe.results.isEmpty(); }), "the newer account answers");
    server.closeHeldConnections();
    steamtest::settle();
    check(probe.results.size() == 1 && probe.results[0].games.size() == 1 && probe.results[0].games[0].appId == "400", "a superseded request never emits");
    checkText(owned.requestedId(), "76561198000000002", "requestedId() follows the latest refresh");
}

void fixtureSeamTests(SteamOwned& owned, Probe& probe, HttpFixture& server) {
    qunsetenv("STEAM_WEB_API_KEY");
    QFile::remove(SteamOwned::apiKeyFilePath());
    const QString fixture = scratchRoot + "/owned-fixture.json";
    writeFile(fixture, body(QJsonObject{{"games", QJsonArray{game(500, "Fixture Seam B", 10), game(2002, "Fixture Installed", 3), game(501, "Fixture Seam A")}}}));
    qputenv("XMB_STEAM_OWNED_FILE", QDir::toNativeSeparators(fixture).toLocal8Bit());
    const int before = server.requestCount();
    probe.reset();
    owned.refresh(IdA);
    check(probe.results.isEmpty(), "the fixture answer is asynchronous (never from inside refresh())");
    check(steamtest::waitFor([&] { return !probe.results.isEmpty(); }), "the fixture answers with no key and no network");
    const Loaded& r = probe.results.first();
    QStringList titles;
    for (const OwnedGame& g : r.games) titles << g.title;
    check(r.error.isEmpty() && !r.needsKey && titles == QStringList({"Fixture Installed", "Fixture Seam A", "Fixture Seam B"}), "fixture rows are parsed and sorted exactly like a live response");
    check(r.games.size() == 3 && r.games[0].installed && !r.games[1].installed, "fixture rows get the installed flag from the local manifests");
    check(server.requestCount() == before, "the fixture seam sends no request");

    // A refresh that is cancelled before the fixture timer fires never emits.
    probe.reset();
    owned.refresh(IdA);
    owned.cancel();
    steamtest::settle();
    check(probe.results.isEmpty(), "cancel() before the fixture answer drops it");

    // Unreadable / invalid fixtures.
    qputenv("XMB_STEAM_OWNED_FILE", QDir::toNativeSeparators(scratchRoot + "/does-not-exist.json").toLocal8Bit());
    refreshAndWait(owned, probe, IdA);
    check(probe.results.size() == 1 && probe.results[0].games.isEmpty() && probe.results[0].error.contains("fixture could not be read"), "a missing fixture file is reported, not invented");
    writeFile(fixture, "this is not json");
    qputenv("XMB_STEAM_OWNED_FILE", QDir::toNativeSeparators(fixture).toLocal8Bit());
    refreshAndWait(owned, probe, IdA);
    check(probe.results.size() == 1 && probe.results[0].games.isEmpty() && probe.results[0].error.contains("fixture could not be read"), "an invalid fixture file is reported");
    writeFile(fixture, "{\"response\":{}}");
    refreshAndWait(owned, probe, IdA);
    check(probe.results.size() == 1 && probe.results[0].games.isEmpty() && probe.results[0].error.isEmpty(), "a valid fixture without games is an empty list");
    qunsetenv("XMB_STEAM_OWNED_FILE");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    scratchRoot = QDir::fromNativeSeparators(argc > 1 ? QString::fromLocal8Bit(argv[1])
                                                       : QDir::current().filePath(QStringLiteral("steam-owned-scratch")));
    QDir(scratchRoot).removeRecursively();
    QDir().mkpath(scratchRoot);
    if (!QFileInfo(scratchRoot).isDir()) {
        std::fprintf(stderr, "cannot create the scratch directory %s\n", qPrintable(scratchRoot));
        return 2;
    }
    steamRoot = scratchRoot + "/steam-root";
    keyDir = scratchRoot + "/keys";
    buildSteamRoot();
    qputenv("XMB_STEAM_ROOT", QDir::toNativeSeparators(steamRoot).toLocal8Bit());
    qputenv("XMB_STEAM_REGISTRY_KEY", "HKEY_CURRENT_USER\\Software\\XmbTest\\SteamOwnedTests-dead");
    qunsetenv("XMB_STEAM_OWNED_FILE");
    qunsetenv("STEAM_WEB_API_KEY");

    HttpFixture server;
    if (!server.listen()) {
        std::fprintf(stderr, "cannot listen on the loopback interface\n");
        return 2;
    }
    qputenv("XMB_STEAM_API_BASE", server.origin());
    keyTests();
    staticTests();
    SteamOwned owned;
    Probe probe(owned);
    noKeyTests(owned, probe, server);
    networkTests(owned, probe, server);
    failureTests(owned, probe, server);
    cancelTests(owned, probe, server);
    fixtureSeamTests(owned, probe, server);
    QDir(scratchRoot).removeRecursively();
    std::printf("steam owned tests: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
