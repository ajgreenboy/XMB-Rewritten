// Tests for "switch Steam accounts fully" and "library without a key":
//   - the account-selection rule over loginusers.vdf (steamSelectAccountFromLoginUsers) and its registry-first order,
//   - the appinfo.vdf reader (steam_appinfo.h) against hand-built v27/v28/v29 files,
//   - the keyless per-account library (steam_keyless.h) and the per-account snapshot (steam_account_state.h) against a
//     synthetic Steam root holding two accounts,
//   - the pure refresh decisions in steam_refresh.h.
// Every fixture is built here from obviously fake ids and names (SteamID64 7656119800000000x, "Fixture ..." titles);
// no real Steam data, process or registry value is read or written. The scratch directory is the first argument
// (QTemporaryDir is avoided: it fails on some machines for an environmental reason).

#include "steam_account_state.h"
#include "steam_appinfo.h"
#include "steam_collections.h"
#include "steam_keyless.h"
#include "steam_library.h"
#include "steam_refresh.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSet>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <utility>

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

const char* IdA = "76561198000000001"; // account id 39734273
const char* IdB = "76561198000000002"; // account id 39734274
const char* AcctA = "39734273";
const char* AcctB = "39734274";

QString scratchRoot;

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size();
}

// ---- loginusers.vdf --------------------------------------------------------------------------------------------
struct LoginUser {
    const char* id;
    const char* name;
    const char* persona = "";
    const char* timestampKey = "Timestamp";   // current clients write lower-case "timestamp"; older ones "Timestamp"
    const char* timestamp = "";
    const char* mostRecent = "";
    const char* allowAutoLogin = "";
    const char* autoLogin = "";
};
QByteArray loginUsers(const QVector<LoginUser>& users) {
    QByteArray b = "\"users\"\n{\n";
    for (const auto& u : users) {
        b += "\t\"" + QByteArray(u.id) + "\"\n\t{\n";
        b += "\t\t\"AccountName\"\t\t\"" + QByteArray(u.name) + "\"\n";
        if (*u.persona) b += "\t\t\"PersonaName\"\t\t\"" + QByteArray(u.persona) + "\"\n";
        b += "\t\t\"RememberPassword\"\t\t\"1\"\n";
        if (*u.allowAutoLogin) b += "\t\t\"AllowAutoLogin\"\t\t\"" + QByteArray(u.allowAutoLogin) + "\"\n";
        if (*u.autoLogin) b += "\t\t\"AutoLogin\"\t\t\"" + QByteArray(u.autoLogin) + "\"\n";
        if (*u.mostRecent) b += "\t\t\"MostRecent\"\t\t\"" + QByteArray(u.mostRecent) + "\"\n";
        if (*u.timestamp) b += "\t\t\"" + QByteArray(u.timestampKey) + "\"\t\t\"" + QByteArray(u.timestamp) + "\"\n";
        b += "\t}\n";
    }
    return b + "}\n";
}

void selectionRuleTests() {
    QString rule;
    // The HTPC case: no MostRecent, no AllowAutoLogin, RememberPassword=1 on both, capital-T Timestamp, and the
    // lower SteamID64 is the newer login. The highest id must NOT win.
    {
        const auto bytes = loginUsers({{IdA, "fixture_a", "Aye", "Timestamp", "1790000000"},
                                       {IdB, "fixture_b", "Bee", "Timestamp", "1780000000"}});
        checkText(steamSelectAccountFromLoginUsers(bytes, {}, &rule), IdA, "HTPC data: newest timestamp wins, not highest id");
        checkText(rule, QStringLiteral("timestamp"), "HTPC data: rule is timestamp");
        // The same data with the registry's AutoLoginUser naming the older account: that name outranks the timestamp.
        checkText(steamSelectAccountFromLoginUsers(bytes, QStringLiteral("fixture_b"), &rule), IdB, "AutoLoginUser outranks timestamp");
        checkText(rule, QStringLiteral("AutoLoginUser"), "rule names AutoLoginUser");
        checkText(steamSelectAccountFromLoginUsers(bytes, QStringLiteral("FIXTURE_B"), nullptr), IdB, "AutoLoginUser compares case-insensitively");
        checkText(steamSelectAccountFromLoginUsers(bytes, QStringLiteral("nobody"), nullptr), IdA, "an AutoLoginUser no account has falls through to timestamp");
    }
    // The old bug: AllowAutoLogin=1 on every remembered account made both "most recent", then the highest id won.
    {
        const auto bytes = loginUsers({{IdA, "fixture_a", "Aye", "timestamp", "1790000000", "", "1", "1"},
                                       {IdB, "fixture_b", "Bee", "timestamp", "1780000000", "", "1", "1"}});
        checkText(steamSelectAccountFromLoginUsers(bytes, {}, &rule), IdA, "AllowAutoLogin/AutoLogin on both accounts says nothing");
        checkText(rule, QStringLiteral("timestamp"), "that falls to the timestamp rule");
    }
    // MostRecent=1 (older clients) wins over a newer timestamp.
    {
        const auto bytes = loginUsers({{IdA, "fixture_a", "Aye", "Timestamp", "1700000000", "1"},
                                       {IdB, "fixture_b", "Bee", "Timestamp", "1780000000", "0"}});
        checkText(steamSelectAccountFromLoginUsers(bytes, QStringLiteral("fixture_b"), &rule), IdA, "MostRecent=1 outranks AutoLoginUser and timestamp");
        checkText(rule, QStringLiteral("MostRecent"), "rule names MostRecent");
    }
    // Two MostRecent: the newer login, not the higher id.
    {
        const auto bytes = loginUsers({{IdA, "fixture_a", "Aye", "timestamp", "1790000000", "1"},
                                       {IdB, "fixture_b", "Bee", "timestamp", "1780000000", "1"}});
        checkText(steamSelectAccountFromLoginUsers(bytes, {}, nullptr), IdA, "two MostRecent: newest timestamp among them");
    }
    // Ties and gaps.
    {
        const auto tie = loginUsers({{IdA, "fixture_a", "", "timestamp", "1790000000"}, {IdB, "fixture_b", "", "timestamp", "1790000000"}});
        checkText(steamSelectAccountFromLoginUsers(tie, {}, nullptr), IdB, "equal timestamps: the higher SteamID64");
        const auto oneStamp = loginUsers({{IdA, "fixture_a"}, {IdB, "fixture_b", "", "timestamp", "5"}});
        checkText(steamSelectAccountFromLoginUsers(oneStamp, {}, nullptr), IdB, "an account with a timestamp beats one without");
        const auto sole = loginUsers({{IdA, "fixture_a"}});
        checkText(steamSelectAccountFromLoginUsers(sole, {}, &rule), IdA, "the sole account is selected");
        checkText(rule, QStringLiteral("sole account"), "rule names the sole account");
        const auto none = loginUsers({{IdA, "fixture_a"}, {IdB, "fixture_b"}});
        check(steamSelectAccountFromLoginUsers(none, {}, nullptr).isEmpty(), "two accounts and no evidence: no guess");
    }
    check(steamSelectAccountFromLoginUsers("not vdf at all {{{", {}, nullptr).isEmpty(), "garbage selects nothing");
    check(steamSelectAccountFromLoginUsers("\"users\"\n{\n\t\"abc\"\n\t{\n\t}\n}\n", {}, nullptr).isEmpty(), "a non-SteamID key is not an account");
}

// ---- the registry-first order through scanSteam() ----------------------------------------------------------------
void setRegistry(const QString& key, const QString& name, const QVariant& value) {
    QSettings s(key, QSettings::NativeFormat);
    s.setValue(name, value);
    s.sync();
}
void registryOrderTests(const QString& key) {
    const QString root = scratchRoot + QStringLiteral("/reg-root");
    QDir(root).removeRecursively();
    writeFile(root + QStringLiteral("/config/loginusers.vdf"),
              loginUsers({{IdA, "fixture_a", "Aye", "Timestamp", "1790000000"}, {IdB, "fixture_b", "Bee", "Timestamp", "1780000000"}}));
    QDir().mkpath(root + QStringLiteral("/steamapps"));
    qputenv("XMB_STEAM_REGISTRY_KEY", key.toUtf8());

    // Nothing in the registry: the loginusers rule decides (newest timestamp).
    checkText(scanSteam(root).steamId, IdA, "no registry state: the loginusers rule (newest timestamp)");
    // The running client's ActiveUser outranks everything in loginusers.vdf.
    setRegistry(key + QStringLiteral("\\ActiveProcess"), QStringLiteral("ActiveUser"), AcctB);
    checkText(scanSteam(root).steamId, IdB, "registry ActiveUser outranks the file");
    checkText(steamActiveAccountFromRegistry(root), IdB, "steamActiveAccountFromRegistry reports the signed-in account");
    // A switch: the same call now follows the new ActiveUser (this is what the shell's poll sees).
    setRegistry(key + QStringLiteral("\\ActiveProcess"), QStringLiteral("ActiveUser"), AcctA);
    checkText(scanSteam(root).steamId, IdA, "ActiveUser changed: the selection follows");
    // Signed out (ActiveUser 0): back to the file, with AutoLoginUser naming the older account.
    setRegistry(key + QStringLiteral("\\ActiveProcess"), QStringLiteral("ActiveUser"), 0);
    check(steamActiveAccountFromRegistry(root).isEmpty(), "ActiveUser 0 is nobody");
    setRegistry(key, QStringLiteral("AutoLoginUser"), QStringLiteral("fixture_b"));
    checkText(scanSteam(root).steamId, IdB, "signed out: AutoLoginUser decides");
    // An out-of-range ActiveUser is ignored.
    setRegistry(key + QStringLiteral("\\ActiveProcess"), QStringLiteral("ActiveUser"), QStringLiteral("99999999999"));
    check(steamActiveAccountFromRegistry(root).isEmpty(), "an impossible ActiveUser is ignored");
    QSettings(key, QSettings::NativeFormat).remove(QString());
    qunsetenv("XMB_STEAM_REGISTRY_KEY");
}

// ---- appinfo.vdf -------------------------------------------------------------------------------------------------
void put32(QByteArray& b, quint32 v) { b.append(reinterpret_cast<const char*>(&v), 4); }
void put64(QByteArray& b, quint64 v) { b.append(reinterpret_cast<const char*>(&v), 8); }

struct Kv {
    QByteArray out;
    QVector<QByteArray>* table = nullptr;   // v29 key table; null = inline keys
    void key(const QByteArray& k) {
        if (!table) { out += k; out.append('\0'); return; }
        int index = table->indexOf(k);
        if (index < 0) { table->append(k); index = int(table->size()) - 1; }
        put32(out, quint32(index));
    }
    void object(const QByteArray& k) { out.append('\0'); key(k); }
    void end() { out.append(char(8)); }
    void str(const QByteArray& k, const QByteArray& v) { out.append(char(1)); key(k); out += v; out.append('\0'); }
    void i32(const QByteArray& k, qint32 v) { out.append(char(2)); key(k); put32(out, quint32(v)); }
    void f32(const QByteArray& k) { out.append(char(3)); key(k); put32(out, 0x3f800000u); }
    void u64(const QByteArray& k, quint64 v) { out.append(char(7)); key(k); put64(out, v); }
    void i64(const QByteArray& k, qint64 v) { out.append(char(10)); key(k); put64(out, quint64(v)); }
    void wide(const QByteArray& k, const QString& v) {
        out.append(char(5)); key(k);
        for (const QChar c : v) { out.append(char(c.unicode() & 0xff)); out.append(char(c.unicode() >> 8)); }
        out.append(char(0)); out.append(char(0));
    }
};

struct AppSpec {
    quint32 appId;
    const char* name;   // empty: the entry has no name
    const char* type;   // empty: the entry has no type
    bool noise = false; // objects and every value type before "common", and an unrelated nested "name"
    bool commonLast = false;
};

QByteArray appEntry(int version, const AppSpec& app, QVector<QByteArray>* table) {
    Kv kv;
    kv.table = table;
    kv.object("appinfo");
    kv.i32("appid", qint32(app.appId));
    auto common = [&] {
        kv.object("common");
        if (*app.name) kv.str("name", app.name);
        if (*app.type) kv.str("type", app.type);
        kv.str("oslist", "windows");
        kv.object("name_localized"); kv.str("german", "wrong place"); kv.end();
        kv.end();
    };
    if (app.noise) {
        kv.object("depots");
        kv.str("name", "not the app name");
        kv.object("1"); kv.str("name", "depot one"); kv.u64("manifest", 123456789012ULL); kv.end();
        kv.f32("ratio"); kv.i64("big", -5); kv.wide("note", QStringLiteral("wide text"));
        kv.out.append(char(4)); kv.key("ptr"); put32(kv.out, 7);
        kv.out.append(char(6)); kv.key("colour"); put32(kv.out, 0xff00ff00u);
        kv.end();
    }
    if (!app.commonLast) common();
    if (app.commonLast) {
        kv.object("extended"); kv.str("name", "extended name"); kv.end();
        common();
    }
    kv.end();   // appinfo
    kv.end();   // root terminator some builds write
    QByteArray e;
    put32(e, app.appId);
    put32(e, quint32(40 + (version >= 0x28 ? 20 : 0) + kv.out.size()));
    put32(e, 2);                  // infoState
    put32(e, 1700000000);         // lastUpdated
    put64(e, 0);                  // token
    e += QByteArray(20, '\x11');  // sha1 text
    put32(e, 77);                 // change number
    if (version >= 0x28) e += QByteArray(20, '\x22');   // sha1 binary
    return e + kv.out;
}

QByteArray makeAppInfo(int version, const QVector<AppSpec>& apps) {
    QByteArray file;
    put32(file, 0x07564400u | quint32(version));
    put32(file, 1);   // universe
    QVector<QByteArray> table;
    if (version >= 0x29) put64(file, 0);   // patched below
    for (const auto& app : apps) file += appEntry(version, app, version >= 0x29 ? &table : nullptr);
    put32(file, 0);   // end marker
    if (version >= 0x29) {
        const quint64 offset = quint64(file.size());
        std::memcpy(file.data() + 8, &offset, 8);
        put32(file, quint32(table.size()));
        for (const auto& s : table) { file += s; file.append('\0'); }
    }
    return file;
}

QSet<QString> idSet(std::initializer_list<const char*> ids) {
    QSet<QString> set;
    for (const char* id : ids) set.insert(QString::fromLatin1(id));
    return set;
}

void appInfoTests() {
    const QVector<AppSpec> apps{
        {100, "Fixture Game One", "Game"},
        {200, "Fixture Tool", "Tool"},
        {300, "Fixture Soundtrack", "music"},
        {400, "Fixture Noisy Game", "game", true},
        {500, "Fixture Late Common", "Game", true, true},
        {600, "", "Game"},
        {700, "Fixture Untyped", ""},
        {800, "Fixture Demo", "Demo"},
    };
    for (const int version : {0x27, 0x28, 0x29}) {
        const QString path = scratchRoot + QStringLiteral("/appinfo-v%1.vdf").arg(version, 0, 16);
        check(writeFile(path, makeAppInfo(version, apps)), "write appinfo fixture");
        QString error;
        const auto all = readSteamAppInfo(path, idSet({"100", "200", "300", "400", "500", "600", "700", "800", "999"}), &error);
        const std::string label = "appinfo v" + std::to_string(version);
        check(error.isEmpty(), (label + ": no error").c_str());
        check(all.size() == 8, (label + ": the eight present apps are found, the absent one is not").c_str());
        check(all.value("100").title == "Fixture Game One" && all.value("100").type == "Game", (label + ": name and type").c_str());
        check(all.value("400").title == "Fixture Noisy Game" && all.value("400").type == "game",
              (label + ": every value type before common is stepped over, and a nested name is not the app name").c_str());
        check(all.value("500").title == "Fixture Late Common", (label + ": common after other objects").c_str());
        check(all.value("600").title.isEmpty() && all.value("600").type == "Game", (label + ": a nameless entry keeps its type").c_str());
        check(all.value("700").title == "Fixture Untyped" && all.value("700").type.isEmpty(), (label + ": an untyped entry keeps its name").c_str());
        check(steamAppTypeIsGame(all.value("100").type) && steamAppTypeIsGame(all.value("400").type), (label + ": Game/game are games").c_str());
        check(!steamAppTypeIsGame(all.value("200").type) && !steamAppTypeIsGame(all.value("300").type) &&
              !steamAppTypeIsGame(all.value("800").type) && !steamAppTypeIsGame(QString{}), (label + ": tool, music, demo and no type are not games").c_str());
        // Only the wanted ids are read.
        const auto some = readSteamAppInfo(path, idSet({"500"}), &error);
        check(some.size() == 1 && some.contains("500"), (label + ": only the wanted id").c_str());
    }

    // Damage: each is refused or contained, never a crash, and what was read before the damage is kept.
    const QString good = scratchRoot + QStringLiteral("/appinfo-v29.vdf");
    QFile goodFile(good);
    check(goodFile.open(QIODevice::ReadOnly), "open the v29 fixture");
    const QByteArray goodBytes = goodFile.readAll();
    goodFile.close();
    QString error;
    check(readSteamAppInfo(scratchRoot + QStringLiteral("/missing.vdf"), idSet({"100"}), &error).isEmpty() && !error.isEmpty(), "a missing file is reported");
    {
        QByteArray bad = goodBytes;
        bad[3] = 0x08;   // wrong signature byte
        const QString path = scratchRoot + QStringLiteral("/appinfo-badmagic.vdf");
        writeFile(path, bad);
        check(readSteamAppInfo(path, idSet({"100"}), &error).isEmpty() && !error.isEmpty(), "a wrong signature is refused");
        QByteArray future = goodBytes;
        future[0] = 0x2b;   // version 0x2b
        writeFile(path, future);
        check(readSteamAppInfo(path, idSet({"100"}), &error).isEmpty() && !error.isEmpty(), "an unknown version is refused");
    }
    {
        // Truncation at every length: no crash, no entry that was not fully present, and nothing past the file.
        const QString path = scratchRoot + QStringLiteral("/appinfo-cut.vdf");
        bool sane = true;
        for (int cut = 0; cut < goodBytes.size(); cut += 7) {
            writeFile(path, goodBytes.left(cut));
            const auto got = readSteamAppInfo(path, idSet({"100", "200", "400", "500"}), &error);
            for (auto it = got.cbegin(); it != got.cend(); ++it)
                if (it.key() == "100" && it->title != "Fixture Game One" && !it->title.isEmpty()) sane = false;
        }
        check(sane, "truncated files never yield a wrong title");
    }
    {
        // An entry whose size field exceeds the file; the string table index out of range; an unknown KV type byte.
        QByteArray huge = makeAppInfo(0x28, {{100, "Fixture Game One", "Game"}});
        const quint32 big = 0x7fffffffu;
        std::memcpy(huge.data() + 12, &big, 4);   // first entry's size (after magic, universe, appid)
        const QString path = scratchRoot + QStringLiteral("/appinfo-huge.vdf");
        writeFile(path, huge);
        check(readSteamAppInfo(path, idSet({"100"}), &error).isEmpty() && !error.isEmpty(), "an impossible entry size is refused");

        QByteArray badIndex = makeAppInfo(0x29, {{100, "Fixture Game One", "Game"}});
        // Point the first key at a table slot that does not exist: the first KV byte after the 60 fixed bytes is the
        // object type, followed by the 4-byte key index.
        const int kvStart = 16 + 8 + 60;
        const quint32 wrongIndex = 9999;
        std::memcpy(badIndex.data() + kvStart + 1, &wrongIndex, 4);
        writeFile(path, badIndex);
        check(readSteamAppInfo(path, idSet({"100"}), &error).isEmpty(), "a key index outside the string table yields no entry");

        QByteArray badType = makeAppInfo(0x28, {{100, "Fixture Game One", "Game"}});
        badType[8 + 8 + 60] = char(0x55);   // first KV type byte
        writeFile(path, badType);
        check(readSteamAppInfo(path, idSet({"100"}), &error).isEmpty(), "an unknown KeyValues type stops the entry");
    }
    check(readSteamAppInfo(good, {}, &error).isEmpty(), "nothing wanted reads nothing");
}

// ---- a synthetic Steam root with two accounts --------------------------------------------------------------------
QByteArray localConfig(const QVector<std::tuple<int, qint64, int>>& apps, const char* familyGroup) {
    QByteArray b = "\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n\t\t\t\t\"apps\"\n\t\t\t\t{\n";
    for (const auto& [id, last, minutes] : apps) {
        b += "\t\t\t\t\t\"" + QByteArray::number(id) + "\"\n\t\t\t\t\t{\n";
        if (last >= 0) b += "\t\t\t\t\t\t\"LastPlayed\"\t\t\"" + QByteArray::number(last) + "\"\n";
        if (minutes >= 0) b += "\t\t\t\t\t\t\"Playtime\"\t\t\"" + QByteArray::number(minutes) + "\"\n";
        b += "\t\t\t\t\t}\n";
    }
    b += "\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n";
    if (*familyGroup)
        b += "\t\"FamilyGroup\"\n\t{\n\t\t\"groupid\"\t\t\"" + QByteArray(familyGroup) + "\"\n\t\t\"name\"\t\t\"Fixture Family\"\n\t}\n";
    return b + "}\n";
}

QByteArray cloudCollections(const QVector<std::pair<QByteArray, QVector<int>>>& staticCollections, bool withDynamic) {
    QByteArray b = "[";
    bool first = true;
    auto sep = [&] { if (!first) b += ","; first = false; };
    for (const auto& [name, ids] : staticCollections) {
        QByteArray added = "[";
        for (int i = 0; i < ids.size(); ++i) added += (i ? "," : "") + QByteArray::number(ids[i]);
        added += "]";
        QByteArray inner = "{\"id\":\"uc-" + name + "\",\"name\":\"" + name + "\",\"added\":" + added + ",\"removed\":[]}";
        inner.replace("\"", "\\\"");
        sep();
        b += "[\"user-collections.uc-" + name + "\",{\"key\":\"user-collections.uc-" + name + "\",\"timestamp\":1,\"value\":\"" + inner + "\",\"version\":\"1\",\"conflictResolutionMethod\":\"custom\",\"strMethodId\":\"union-collections\"}]";
    }
    if (withDynamic) {
        // A dynamic collection has a filter and no member list.
        QByteArray inner = "{\"id\":\"uc-dyn\",\"name\":\"Fixture Dynamic\",\"added\":[],\"filterSpec\":{\"nFormatVersion\":2}}";
        inner.replace("\"", "\\\"");
        sep();
        b += "[\"user-collections.uc-dyn\",{\"key\":\"user-collections.uc-dyn\",\"timestamp\":1,\"value\":\"" + inner + "\"}]";
    }
    return b + "]";
}

QByteArray libraryCacheJson() {
    return "[[\"achievements\",{\"version\":2,\"data\":{\"vecHighlight\":[],\"vecUnachieved\":[],\"nTotal\":0}}],[\"descriptions\",{\"version\":1,\"data\":{}}]]";
}

QByteArray manifest(int id, const char* name) {
    return "\"AppState\"\n{\n\t\"appid\"\t\t\"" + QByteArray::number(id) + "\"\n\t\"name\"\t\t\"" + QByteArray(name) +
           "\"\n\t\"StateFlags\"\t\t\"4\"\n\t\"installdir\"\t\t\"Dir" + QByteArray::number(id) + "\"\n}\n";
}

// Account A: 1001 (played), 1002 (played, LastPlayed only), 1003 (a stub with neither: noise), 1004 (a tool, played),
//            2001 (collected only), 3001 (library cache only), 4001 (played, appinfo has no entry; installed on the PC),
//            5001 (played, appinfo has no entry and the PC has no manifest: never listed), 6001 (played, a demo).
// Account B: 1001 (shared title, different play record), 7001, 7002 (played), 8001 (collected).
// The PC has 7001 and 4001 installed. 7001 belongs to B's data only and must never appear for A.
SteamInstallation makeRoot(const QString& root, const QString& dataDir) {
    QDir(root).removeRecursively();
    QDir(dataDir).removeRecursively();
    QDir().mkpath(dataDir);
    writeFile(root + QStringLiteral("/config/loginusers.vdf"),
              loginUsers({{IdA, "fixture_a", "Persona Aye", "timestamp", "1790000000"},
                          {IdB, "fixture_b", "Persona Bee", "timestamp", "1780000000"}}));
    writeFile(root + QStringLiteral("/steamapps/appmanifest_7001.acf"), manifest(7001, "Fixture B Installed"));
    writeFile(root + QStringLiteral("/steamapps/appmanifest_4001.acf"), manifest(4001, "Fixture Installed Fallback"));
    QDir().mkpath(root + QStringLiteral("/steamapps/common/Dir7001"));
    QDir().mkpath(root + QStringLiteral("/steamapps/common/Dir4001"));
    writeFile(root + QStringLiteral("/appcache/appinfo.vdf"), makeAppInfo(0x29, {
        {1001, "Fixture Alpha", "Game"}, {1002, "Fixture Bravo", "Game"}, {1003, "Fixture Stub", "Game"},
        {1004, "Fixture Editor Tool", "Tool"}, {2001, "Fixture Collected", "Game", true}, {3001, "Fixture Cached", "Game"},
        {6001, "Fixture Demo", "Demo"}, {7001, "Fixture B Installed", "Game"}, {7002, "Fixture B Played", "Game"},
        {8001, "Fixture B Collected", "Game"}}));
    writeFile(root + QStringLiteral("/userdata/%1/config/localconfig.vdf").arg(AcctA),
              localConfig({{1001, 1790000000, 120}, {1002, 1700000000, -1}, {1003, 0, 0}, {1004, 1700000500, 30},
                           {4001, 1700000100, 15}, {5001, 1700000200, 10}, {6001, 1700000300, 5}}, "fixture-group-a"));
    writeFile(root + QStringLiteral("/userdata/%1/config/cloudstorage/cloud-storage-namespace-1.json").arg(AcctA),
              cloudCollections({{"Fixture Favourites", {2001, 1001}}}, true));
    writeFile(root + QStringLiteral("/userdata/%1/config/librarycache/3001.json").arg(AcctA), libraryCacheJson());
    writeFile(root + QStringLiteral("/userdata/%1/config/librarycache/1001.json").arg(AcctA), libraryCacheJson());
    writeFile(root + QStringLiteral("/userdata/%1/config/librarycache/notanapp.json").arg(AcctA), libraryCacheJson());
    writeFile(root + QStringLiteral("/userdata/%1/config/localconfig.vdf").arg(AcctB),
              localConfig({{1001, 1750000000, 5}, {7001, 1780000000, 300}, {7002, 1760000000, 60}}, "fixture-group-b"));
    writeFile(root + QStringLiteral("/userdata/%1/config/cloudstorage/cloud-storage-namespace-1.json").arg(AcctB),
              cloudCollections({{"Fixture Bee Group", {8001}}}, false));
    return scanSteam(root);
}

QStringList ids(const OwnedGameList& games) {
    QStringList out;
    for (const auto& g : games) out << g.appId;
    return out;
}
QHash<QString, QString> installedOf(const SteamInstallation& s) {
    QHash<QString, QString> h;
    for (const auto& g : s.games) h.insert(g.appId, g.title);
    return h;
}

void keylessTests() {
    const QString root = scratchRoot + QStringLiteral("/keyless-root");
    const QString data = scratchRoot + QStringLiteral("/keyless-data");
    qputenv("XMB_STEAM_REGISTRY_KEY", "HKEY_CURRENT_USER\\Software\\XmbTest\\SteamAccountTests-dead");   // no ActiveUser: loginusers decides
    const SteamInstallation installation = makeRoot(root, data);
    checkText(installation.steamId, IdA, "fixture: account A (newest timestamp) is the selected account");
    clearSteamKeylessCache();
    const auto installed = installedOf(installation);
    check(installed.contains("7001") && installed.contains("4001"), "fixture: the PC has both installs");

    const auto a = scanSteamKeylessLibrary(installation.root, IdA, installed);
    // 1001/1002 played, 2001 collected, 3001 library cache, 4001 played + manifest-only. Not: 1003 (stub), 1004 (tool),
    // 5001 (unnamed), 6001 (demo), 7001/7002/8001 (account B), and the non-numeric cache file.
    QStringList got = ids(a.games);
    got.sort();
    check(got == QStringList({"1001", "1002", "2001", "3001", "4001"}), "A's keyless list is exactly its own games");
    check(!got.contains("7001") && !got.contains("7002") && !got.contains("8001"), "none of account B's apps appear for A");
    check(!got.contains("1003"), "a localconfig stub with no playtime and no last-played is noise");
    check(!got.contains("1004") && !got.contains("6001"), "tools and demos are not games");
    check(!got.contains("5001"), "an unnamed, untyped id is never listed");
    QHash<QString, OwnedGame> byId;
    for (const auto& g : a.games) byId.insert(g.appId, g);
    check(byId.value("1001").title == "Fixture Alpha", "title from appinfo");
    check(byId.value("4001").title == "Fixture Installed Fallback" && byId.value("4001").installed, "no appinfo entry: the manifest names it, and it is installed");
    check(!byId.value("1001").installed, "installed is set only from this PC's manifests");
    check(byId.value("1001").playtimeMinutes == 120, "playtime is carried for the owned-row model (the row line does not show it)");
    check(a.playedCount == 3 && a.collectedCount == 2 && a.libraryCacheCount == 2, "source counts: played 3 (1001 1002 4001), collected 2 (2001 1001), cache 2 (3001 1001)");
    check(a.games.first().title.localeAwareCompare(a.games.last().title) <= 0, "title order");
    check(!a.fromCache, "the first scan reads the files");

    const auto again = scanSteamKeylessLibrary(installation.root, IdA, installed);
    check(again.fromCache && ids(again.games) == ids(a.games), "the second scan is answered from the per-account cache");

    // Account B: its own data only.
    const auto b = scanSteamKeylessLibrary(installation.root, IdB, installed);
    QStringList gotB = ids(b.games);
    gotB.sort();
    check(gotB == QStringList({"1001", "7001", "7002", "8001"}), "B's keyless list is exactly its own games");
    check(!gotB.contains("2001") && !gotB.contains("3001") && !gotB.contains("4001"), "none of account A's apps appear for B");
    check(!b.fromCache, "B has its own cache entry");
    check(scanSteamKeylessLibrary(installation.root, IdA, installed).fromCache, "A's cache entry survived B's scan");

    // A played game shows up on the next scan: the cache validates the file, not a timer.
    writeFile(root + QStringLiteral("/userdata/%1/config/localconfig.vdf").arg(AcctA),
              localConfig({{1001, 1790000000, 120}, {1002, 1700000000, -1}, {3001, 1795000000, 45}, {4001, 1700000100, 15}}, "fixture-group-a"));
    const auto changed = scanSteamKeylessLibrary(installation.root, IdA, installed);
    check(!changed.fromCache && changed.playedCount == 4, "a changed localconfig.vdf invalidates the cache entry");

    // Deterministic captures turn it off; a missing account folder and a bad id are empty.
    qputenv("XMB_STEAM_ACTIVITY", "0");
    check(scanSteamKeylessLibrary(installation.root, IdA, installed).games.isEmpty(), "XMB_STEAM_ACTIVITY=0: no keyless data");
    qunsetenv("XMB_STEAM_ACTIVITY");
    check(scanSteamKeylessLibrary(installation.root, "76561198000000099", installed).games.isEmpty(), "an account with no userdata folder has no games");
    check(scanSteamKeylessLibrary(installation.root, "garbage", installed).games.isEmpty(), "a bad account id has no games");
    check(scanSteamKeylessLibrary({}, IdA, installed).games.isEmpty(), "no root, no games");

    // No appinfo.vdf at all: only manifest-named apps survive, and the reason is a warning, not a crash.
    QFile::remove(root + QStringLiteral("/appcache/appinfo.vdf"));
    clearSteamKeylessCache();
    const auto noInfo = scanSteamKeylessLibrary(installation.root, IdA, installed);
    check(ids(noInfo.games) == QStringList({"4001"}) && !noInfo.warnings.isEmpty(), "without appinfo.vdf only manifest-named apps remain");
}

void capTest() {
    const QString root = scratchRoot + QStringLiteral("/cap-root");
    QDir(root).removeRecursively();
    writeFile(root + QStringLiteral("/config/loginusers.vdf"), loginUsers({{IdA, "fixture_a", "Aye", "timestamp", "1"}}));
    const int total = kSteamKeylessMaximumGames + 120;
    QVector<AppSpec> apps;
    QVector<QByteArray> names;
    QVector<std::tuple<int, qint64, int>> played;
    names.reserve(total);
    for (int i = 0; i < total; ++i) {
        names.push_back("Fixture Cap " + QByteArray::number(i));
        apps.push_back({quint32(10000 + i), names.last().constData(), "Game"});
        played.push_back({10000 + i, qint64(1700000000 + i), 10});   // a higher index was played later
    }
    writeFile(root + QStringLiteral("/appcache/appinfo.vdf"), makeAppInfo(0x29, apps));
    writeFile(root + QStringLiteral("/userdata/%1/config/localconfig.vdf").arg(AcctA), localConfig(played, ""));
    clearSteamKeylessCache();
    const auto library = scanSteamKeylessLibrary(root, IdA, {});
    check(library.games.size() == kSteamKeylessMaximumGames && library.truncated, "the list is capped at kSteamKeylessMaximumGames and says so");
    QSet<QString> have;
    for (const auto& g : library.games) have.insert(g.appId);
    check(have.contains(QString::number(10000 + total - 1)) && !have.contains(QStringLiteral("10000")), "the cap keeps the most recently played");
}

// One binary shortcuts.vdf holding a single shortcut (steam_shortcuts.h documents the format).
QByteArray shortcutsFile(const char* name, quint32 appId) {
    QByteArray out;
    out += QByteArray("\0shortcuts\0", 11) + QByteArray("\0" "0\0", 3);
    out += QByteArray("\x02" "appid\0", 7);
    for (int i = 0; i < 4; ++i) out += char((appId >> (8 * i)) & 0xFF);
    out += QByteArray("\x01" "appname\0", 9) + QByteArray(name) + QByteArray(1, '\0');
    out += QByteArray("\x01" "exe\0", 5) + QByteArray("\"C:\\Fixture\\x.exe\"") + QByteArray(1, '\0');
    out += QByteArray("\x08\x08\x08", 3);
    return out;
}

// ---- the per-account snapshot ----------------------------------------------------------------------------------------
void snapshotTests() {
    const QString root = scratchRoot + QStringLiteral("/snapshot-root");
    const QString data = scratchRoot + QStringLiteral("/snapshot-data");
    qputenv("XMB_STEAM_REGISTRY_KEY", "HKEY_CURRENT_USER\\Software\\XmbTest\\SteamAccountTests-dead");
    SteamInstallation steam = makeRoot(root, data);
    clearSteamKeylessCache();
    writeFile(root + QStringLiteral("/userdata/%1/config/shortcuts.vdf").arg(AcctA), shortcutsFile("Fixture Shortcut A", 0x90000001u));
    writeFile(root + QStringLiteral("/userdata/%1/config/shortcuts.vdf").arg(AcctB), shortcutsFile("Fixture Shortcut B", 0x90000002u));

    const auto a = loadSteamAccountSnapshot(steam, data);
    checkText(a.steamId, IdA, "snapshot A: account");
    checkText(a.persona, QStringLiteral("Persona Aye"), "snapshot A: persona");
    check(a.activity.contains("1001") && a.activity.value("1001").playtimeMinutes == 120 && !a.activity.contains("7001"), "snapshot A: play records are A's");
    check(a.collections.size() == 2 && a.collections.first().name == "Fixture Dynamic" && a.collections.first().appIds.isEmpty() &&
          a.collections.last().name == "Fixture Favourites" && a.collections.last().appIds.size() == 2, "snapshot A: collections (the dynamic one has no member list)");
    check(a.accounts.size() == 2, "snapshot A: both remembered accounts");
    check(a.shortcuts.size() == 1 && a.shortcuts.first().appName == "Fixture Shortcut A" && a.shortcuts.first().appId == 0x90000001u && a.shortcutsError.isEmpty(),
          "snapshot A: the non-Steam shortcuts are A's");
    check(!ids(a.keyless.games).contains(QString::number(0x90000001u)), "snapshot A: a shortcut is never part of the owned games");
    check(!a.keyless.games.isEmpty() && !ids(a.keyless.games).contains("7001"), "snapshot A: keyless list is A's");

    // The same installation, the account changed: nothing of A may be in B's snapshot.
    SteamInstallation asB = steam;
    asB.steamId = IdB;
    const auto b = loadSteamAccountSnapshot(asB, data);
    checkText(b.steamId, IdB, "snapshot B: account");
    checkText(b.persona, QStringLiteral("Persona Bee"), "snapshot B: persona");
    check(b.activity.contains("7001") && !b.activity.contains("1002") && b.activity.value("1001").playtimeMinutes == 5, "snapshot B: play records are B's");
    check(b.collections.size() == 1 && b.collections.first().name == "Fixture Bee Group", "snapshot B: collections are B's");
    check(b.shortcuts.size() == 1 && b.shortcuts.first().appName == "Fixture Shortcut B", "snapshot B: A's shortcuts are not shown to B");
    check(ids(b.keyless.games).contains("7002") && !ids(b.keyless.games).contains("2001"), "snapshot B: keyless list is B's");

    // No account at all: every account-specific list is empty (nothing leaks from a previous snapshot).
    SteamInstallation none = steam;
    none.steamId.clear();
    const auto empty = loadSteamAccountSnapshot(none, data);
    check(empty.steamId.isEmpty() && empty.activity.isEmpty() && empty.collections.isEmpty() && empty.keyless.games.isEmpty() && empty.shortcuts.isEmpty(), "no account: nothing account-specific is loaded");
    qunsetenv("XMB_STEAM_REGISTRY_KEY");
}

void collectionsInTests() {
    const QString root = scratchRoot + QStringLiteral("/snapshot-root");
    QString error;
    const auto a = scanSteamCollectionsIn(root, IdA, &error);
    check(a.size() == 2 && a.last().appIds.contains("2001") && a.last().appIds.contains("1001") && error.isEmpty(), "scanSteamCollectionsIn reads the named root's account file (static collections only)");
    const auto none = scanSteamCollectionsIn(root, "76561198000000099", &error);
    check(none.isEmpty() && !error.isEmpty(), "an account with no cloud storage reports why");
}

void decisionTests() {
    checkText(steamPollAccountChange(IdB, IdA, {}), IdB, "poll: Steam reports another account");
    check(steamPollAccountChange(IdA, IdA, {}).isEmpty(), "poll: same account, nothing to do");
    check(steamPollAccountChange({}, IdA, {}).isEmpty(), "poll: nobody signed in is not a change");
    check(steamPollAccountChange(IdB, IdA, IdB).isEmpty(), "poll: a refresh already started for that account is not restarted");
    checkText(steamPollAccountChange(IdA, IdB, IdA), QString{}, "poll: the id a refresh was started for is not triggered again");
    check(steamKeylessReplacesOwned(false, false) && steamKeylessReplacesOwned(false, true), "no key: the keyless list replaces what is shown, even an old Valve list");
    check(steamKeylessReplacesOwned(true, false), "a key whose list has not arrived: the keyless list stands in");
    check(!steamKeylessReplacesOwned(true, true), "a key and Valve's list on screen: Valve's list wins");
    const QString text = steamKeylessSourceText(QStringLiteral("Persona Aye"));
    checkText(text, QStringLiteral("From this PC's Steam data for Persona Aye: games you have played or collected. Add a Steam Web API key in Settings for the full list."), "source text");
    check(steamKeylessSourceText({}).contains(QStringLiteral("this account")), "source text without a persona");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    scratchRoot = QDir::fromNativeSeparators(argc > 1 ? QString::fromLocal8Bit(argv[1])
                                                       : QDir::current().filePath(QStringLiteral("steam-account-scratch")));
    QDir(scratchRoot).removeRecursively();
    QDir().mkpath(scratchRoot);
    if (!QFileInfo(scratchRoot).isDir()) {
        std::fprintf(stderr, "cannot create the scratch directory %s\n", qPrintable(scratchRoot));
        return 2;
    }
    const QString registryKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\XmbTest\\SteamAccountTests-%1").arg(QCoreApplication::applicationPid());
    selectionRuleTests();
    registryOrderTests(registryKey);
    appInfoTests();
    keylessTests();
    capTest();
    snapshotTests();
    collectionsInTests();
    decisionTests();
    QDir(scratchRoot).removeRecursively();
    std::printf("steam account tests: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
