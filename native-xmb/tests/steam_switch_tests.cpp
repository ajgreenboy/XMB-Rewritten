// Tests for the credential-free Steam account switch (src/steam_switch.*) and the prompt text built on it
// (src/steam_accounts.*): the pure loginusers.vdf rewrite, and the whole asynchronous switch against a scratch
// Steam root, a scratch registry key (HKCU\Software\XmbTest\...; never the real Steam key) and a fake process
// control. Nothing here touches a real Steam installation, process or registry value.

#include "steam_accounts.h"
#include "steam_refresh.h"
#include "steam_switch.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
void checkEq(const QByteArray& got, const QByteArray& want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s\n--- want ---\n%s\n--- got ---\n%s\n", what, want.constData(), got.constData());
        ++failures;
    }
}

const char* IdA = "76561198000000001"; // account id 39734273
const char* IdB = "76561198000000002"; // account id 39734274
const char* IdC = "76561198000000003";

QByteArray user(const char* id, const char* name, const char* persona, const char* remember, const char* autoLogin,
                const char* mostRecent, const char* eol = "\n", bool offlineKeys = true) {
    QByteArray b;
    const QByteArray e(eol);
    b += "\t\"" + QByteArray(id) + "\"" + e + "\t{" + e;
    b += "\t\t\"AccountName\"\t\t\"" + QByteArray(name) + "\"" + e;
    b += "\t\t\"PersonaName\"\t\t\"" + QByteArray(persona) + "\"" + e;
    b += "\t\t\"RememberPassword\"\t\t\"" + QByteArray(remember) + "\"" + e;
    if (offlineKeys) b += "\t\t\"WantsOfflineMode\"\t\t\"0\"" + e + "\t\t\"SkipOfflineModeWarning\"\t\t\"0\"" + e;
    if (autoLogin) b += "\t\t\"AllowAutoLogin\"\t\t\"" + QByteArray(autoLogin) + "\"" + e;
    if (mostRecent) b += "\t\t\"MostRecent\"\t\t\"" + QByteArray(mostRecent) + "\"" + e;
    b += "\t\t\"Timestamp\"\t\t\"1700000000\"" + e + "\t}" + e;
    return b;
}

QByteArray file(const QList<QByteArray>& users, const char* eol = "\n") {
    const QByteArray e(eol);
    QByteArray b = "\"users\"" + e + "{" + e;
    for (const QByteArray& u : users) b += u;
    b += "}" + e;
    return b;
}

// ---- the pure rewrite -------------------------------------------------------------------------------
void rewriteTests() {
    // 1. Two accounts: the target gets MostRecent 1 and AllowAutoLogin 1, the other MostRecent 0; nothing else moves.
    {
        const QByteArray in = file({user(IdA, "alicea", "Al", "1", "1", "1"), user(IdB, "bobbyb", "Cee", "1", "0", "0")});
        const auto r = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdB));
        check(r.ok && r.changed, "two accounts: rewritten");
        check(r.accountName == QLatin1String("bobbyb") && r.rememberPassword && r.usersSeen == 2, "two accounts: target facts");
        const QByteArray want = file({user(IdA, "alicea", "Al", "1", "1", "0"), user(IdB, "bobbyb", "Cee", "1", "1", "1")});
        checkEq(r.bytes, want, "two accounts: exact output");
        // Idempotent.
        const auto again = rewriteLoginUsersForSwitch(r.bytes, QString::fromLatin1(IdB));
        check(again.ok && !again.changed && again.bytes == r.bytes, "rewriting the output changes nothing");
    }
    // 2. Unknown keys, nested blocks, comments and odd spacing survive byte for byte.
    {
        const QByteArray in =
            "// leading comment\n\"users\"\n{\n"
            "\t\"76561198000000001\"\n\t{\n\t\t\"AccountName\"\t\t\"a\"\n\t\t\"RememberPassword\"\t\t\"1\"\n"
            "\t\t\"Extra\"\n\t\t{\n\t\t\t\"deep\"\t\"1\"\n\t\t\t\"MostRecent\"\t\"1\"\n\t\t}\n"
            "\t\t\"Unknown\"\t\t\"keep me\" // trailing comment\n\t\t\"MostRecent\"\t\t\"1\"\n\t}\n"
            "\t\"76561198000000002\"\n\t{\n\t\t\"AccountName\"\t\t\"b\"\n\t\t\"RememberPassword\"\t\t\"1\"\n"
            "\t\t\"Custom\"\t\t\"x\"\n\t\t\"MostRecent\"\t\t\"0\"\n\t}\n}\n\"other\"\n{\n\t\"k\"\t\"v\"\n}\n";
        const auto r = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdB));
        check(r.ok && r.changed, "unknown keys: rewritten");
        QByteArray want = in;
        // account A: the direct MostRecent goes 1 -> 0 (the nested one inside "Extra" is not an account key);
        want.replace("\"Unknown\"\t\t\"keep me\" // trailing comment\n\t\t\"MostRecent\"\t\t\"1\"",
                     "\"Unknown\"\t\t\"keep me\" // trailing comment\n\t\t\"MostRecent\"\t\t\"0\"");
        // account B: MostRecent 0 -> 1 and AllowAutoLogin inserted before the closing brace.
        want.replace("\t\t\"Custom\"\t\t\"x\"\n\t\t\"MostRecent\"\t\t\"0\"\n\t}\n",
                     "\t\t\"Custom\"\t\t\"x\"\n\t\t\"MostRecent\"\t\t\"1\"\n\t\t\"AllowAutoLogin\"\t\t\"1\"\n\t}\n");
        checkEq(r.bytes, want, "unknown keys: only the intended bytes changed");
    }
    // 3. CRLF stays CRLF: no bare LF is introduced.
    {
        const QByteArray in = file({user(IdA, "a", "A", "1", "1", "1", "\r\n"), user(IdB, "b", "B", "1", nullptr, nullptr, "\r\n")}, "\r\n");
        const auto r = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdB));
        check(r.ok && r.changed, "CRLF: rewritten");
        bool bareLf = false;
        for (qsizetype i = 0; i < r.bytes.size(); ++i)
            if (r.bytes.at(i) == '\n' && (i == 0 || r.bytes.at(i - 1) != '\r')) bareLf = true;
        check(!bareLf, "CRLF: no bare LF");
        const QByteArray want = file({user(IdA, "a", "A", "1", "1", "0", "\r\n"), user(IdB, "b", "B", "1", "1", "1", "\r\n")}, "\r\n");
        // The inserted lines follow the last existing key, so compare as a set of lines instead of exact order.
        check(r.bytes.contains("\t\t\"AllowAutoLogin\"\t\t\"1\"\r\n") && r.bytes.contains("\t\t\"MostRecent\"\t\t\"1\"\r\n"),
              "CRLF: both keys inserted with the file's separator");
        const auto reread = rewriteLoginUsersForSwitch(r.bytes, QString::fromLatin1(IdB));
        check(reread.ok && !reread.changed, "CRLF: idempotent");
        Q_UNUSED(want);
    }
    // 4. Missing MostRecent and AllowAutoLogin on the target are inserted in the block's own style; other accounts
    //    without MostRecent are not touched.
    {
        const QByteArray in = file({user(IdA, "a", "A", "1", nullptr, nullptr), user(IdB, "b", "B", "1", nullptr, nullptr)});
        const auto r = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdA));
        check(r.ok && r.changed, "missing keys: rewritten");
        const QByteArray want = file({user(IdA, "a", "A", "1", nullptr, nullptr)});
        const QByteArray expectA =
            "\t\t\"Timestamp\"\t\t\"1700000000\"\n\t\t\"MostRecent\"\t\t\"1\"\n\t\t\"AllowAutoLogin\"\t\t\"1\"\n\t}\n";
        check(r.bytes.contains(expectA), "missing keys: inserted after the last key, same indent and separator");
        // Account B is byte-identical.
        check(r.bytes.endsWith(user(IdB, "b", "B", "1", nullptr, nullptr) + "}\n"), "missing keys: the other account is untouched");
        Q_UNUSED(want);
    }
    // 5. One account only.
    {
        const QByteArray in = file({user(IdA, "solo", "Solo", "1", "0", "0")});
        const auto r = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdA));
        checkEq(r.bytes, file({user(IdA, "solo", "Solo", "1", "1", "1")}), "one account: exact output");
        check(r.usersSeen == 1, "one account: count");
    }
    // 6. Many accounts: exactly one MostRecent 1 afterwards, on the target.
    {
        QList<QByteArray> users;
        for (int i = 0; i < 30; ++i) {
            const QByteArray id = QByteArray::number(76561198000000100ULL + static_cast<unsigned>(i));
            const QByteArray name = "acct" + QByteArray::number(i);
            users << user(id.constData(), name.constData(), "P", "1", "0", i == 5 ? "1" : "0");
        }
        const QByteArray in = file(users);
        const QString target = QString::number(76561198000000100ULL + 17);
        const auto r = rewriteLoginUsersForSwitch(in, target);
        check(r.ok && r.usersSeen == 30 && r.accountName == QLatin1String("acct17"), "many accounts: target found");
        check(r.bytes.count("\"MostRecent\"\t\t\"1\"") == 1, "many accounts: exactly one MostRecent 1");
        check(r.bytes.count("\"AllowAutoLogin\"\t\t\"1\"") == 1, "many accounts: only the target gets AllowAutoLogin 1");
        check(r.bytes.size() == in.size(), "many accounts: same size (values replaced in place)");
    }
    // 7. Refusals leave the bytes alone.
    {
        const QByteArray in = file({user(IdA, "a", "A", "0", "1", "1"), user(IdB, "b", "B", "1", "0", "0")});
        const auto notRemembered = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdA));
        check(!notRemembered.ok && !notRemembered.changed && notRemembered.bytes == in, "no saved sign-in: refused");
        const auto allowed = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdA), false);
        check(allowed.ok && !allowed.rememberPassword, "no saved sign-in: allowed only when not required");
        const auto missing = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdC));
        check(!missing.ok && missing.bytes == in, "unknown account: refused");
        const auto empty = rewriteLoginUsersForSwitch(in, QString());
        check(!empty.ok, "empty id: refused");
        const auto garbage = rewriteLoginUsersForSwitch("\"users\" { \"x\" { \"AccountName\" ", QString::fromLatin1(IdA));
        check(!garbage.ok, "truncated file: refused");
        const auto noUsers = rewriteLoginUsersForSwitch("\"other\" { \"a\" \"b\" }", QString::fromLatin1(IdA));
        check(!noUsers.ok, "no users block: refused");
        const QByteArray twice = file({user(IdA, "a", "A", "1", "1", "1"), user(IdA, "a", "A", "1", "1", "1")});
        check(!rewriteLoginUsersForSwitch(twice, QString::fromLatin1(IdA)).ok, "duplicate account: refused");
        const QByteArray noName = file({"\t\"76561198000000001\"\n\t{\n\t\t\"RememberPassword\"\t\"1\"\n\t}\n"});
        check(!rewriteLoginUsersForSwitch(noName, QString::fromLatin1(IdA)).ok, "no AccountName: refused");
    }
    // 8. Quoting: bare words and quoted values keep their own style; escapes inside other values survive.
    {
        const QByteArray in =
            "users\n{\n\t76561198000000001\n\t{\n\t\tAccountName a\n\t\tRememberPassword 1\n\t\tMostRecent 0\n\t}\n"
            "\t\"76561198000000002\"\n\t{\n\t\t\"AccountName\"\t\"b\"\n\t\t\"PersonaName\"\t\"say \\\"hi\\\" \\\\ ok\"\n"
            "\t\t\"RememberPassword\"\t\"1\"\n\t\t\"MostRecent\"\t\"true\"\n\t}\n}\n";
        const auto r = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdA));
        check(r.ok, "quoting: rewritten");
        check(r.bytes.contains("\t\tMostRecent 1\n"), "quoting: a bare value stays bare");
        check(r.bytes.contains("\t\tAllowAutoLogin 1\n"), "quoting: an inserted key follows the bare style");
        check(r.bytes.contains("\"MostRecent\"\t\"0\"\n"), "quoting: a truthy 'true' on another account becomes 0, quoted");
        check(r.bytes.contains("\"say \\\"hi\\\" \\\\ ok\""), "quoting: escapes in other values survive");
    }
    // 9. A byte-order mark survives.
    {
        const QByteArray in = QByteArray("\xEF\xBB\xBF") + file({user(IdA, "a", "A", "1", "0", "0")});
        const auto r = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdA));
        check(r.ok && r.bytes.startsWith("\xEF\xBB\xBF"), "BOM kept");
    }
    // 10. A compact one-line block.
    {
        const QByteArray in = "\"users\" { \"76561198000000001\" { \"AccountName\" \"a\" \"RememberPassword\" \"1\" } "
                              "\"76561198000000002\" { \"AccountName\" \"b\" \"RememberPassword\" \"1\" \"MostRecent\" \"1\" } }";
        const auto r = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdA));
        check(r.ok && r.changed, "compact: rewritten");
        check(r.bytes.contains("\"MostRecent\" \"1\" \"AllowAutoLogin\" \"1\" }"), "compact: inserted inside the braces");
        check(r.bytes.contains("\"MostRecent\" \"0\" }"), "compact: the other account goes to 0");
        const auto again = rewriteLoginUsersForSwitch(r.bytes, QString::fromLatin1(IdA));
        check(again.ok && !again.changed, "compact: idempotent");
    }
    // 11. WantsOfflineMode: mirrored as 0 when others have it and the target does not; an existing 1 is left alone.
    {
        QByteArray b = "\"users\"\n{\n";
        b += user(IdA, "a", "A", "1", "1", "1", "\n", true);
        b += user(IdB, "b", "B", "1", "0", "0", "\n", false);
        b += "}\n";
        const auto r = rewriteLoginUsersForSwitch(b, QString::fromLatin1(IdB));
        check(r.ok && r.bytes.count("\"WantsOfflineMode\"") == 2 && r.bytes.count("\"SkipOfflineModeWarning\"") == 2,
              "offline keys: added to the target as 0 when the file uses them");
        QByteArray offline = file({user(IdA, "a", "A", "1", "1", "1"), user(IdB, "b", "B", "1", "0", "0")});
        offline.replace("\"WantsOfflineMode\"\t\t\"0\"", "\"WantsOfflineMode\"\t\t\"1\"");
        const auto o = rewriteLoginUsersForSwitch(offline, QString::fromLatin1(IdB));
        check(o.ok && o.wantsOfflineMode, "offline: reported");
        check(o.bytes.contains("\"WantsOfflineMode\"\t\t\"1\""), "offline: the existing value is not changed");
    }
}

// The format a current Steam client writes (seen on the HTPC): no MostRecent or AllowAutoLogin, an AutoLogin flag per
// account and a lowercase "timestamp".
QByteArray modernUser(const char* id, const char* name, const char* remember, const char* autoLogin) {
    return "\t\"" + QByteArray(id) + "\"\r\n\t{\r\n\t\t\"AccountName\"\t\t\"" + QByteArray(name) +
           "\"\r\n\t\t\"PersonaName\"\t\t\"P\"\r\n\t\t\"RememberPassword\"\t\t\"" + QByteArray(remember) +
           "\"\r\n\t\t\"WantsOfflineMode\"\t\t\"0\"\r\n\t\t\"SkipOfflineModeWarning\"\t\t\"0\"\r\n\t\t\"AutoLogin\"\t\t\"" +
           QByteArray(autoLogin) + "\"\r\n\t\t\"timestamp\"\t\t\"1794677089\"\r\n\t}\r\n";
}

void modernSchemaTests() {
    const QByteArray in = "\"users\"\r\n{\r\n" + modernUser(IdA, "alicea", "1", "1") + modernUser(IdB, "bobbyb", "1", "0") + "}\r\n";
    const auto r = rewriteLoginUsersForSwitch(in, QString::fromLatin1(IdB));
    check(r.ok && r.changed, "modern: rewritten");
    QByteArray want = "\"users\"\r\n{\r\n" + modernUser(IdA, "alicea", "1", "0") + modernUser(IdB, "bobbyb", "1", "1") + "}\r\n";
    // The target also gets the keys the task names, appended after its last key in the block's own style.
    want.replace("\t\t\"timestamp\"\t\t\"1794677089\"\r\n\t}\r\n}\r\n",
                 "\t\t\"timestamp\"\t\t\"1794677089\"\r\n\t\t\"MostRecent\"\t\t\"1\"\r\n\t\t\"AllowAutoLogin\"\t\t\"1\"\r\n\t}\r\n}\r\n");
    checkEq(r.bytes, want, "modern: AutoLogin moves to the target, MostRecent and AllowAutoLogin are added");
    const auto again = rewriteLoginUsersForSwitch(r.bytes, QString::fromLatin1(IdB));
    check(again.ok && !again.changed, "modern: idempotent");
    check(r.bytes.count("\"AutoLogin\"\t\t\"1\"") == 1, "modern: exactly one account has AutoLogin 1");
}

// ---- the whole switch -------------------------------------------------------------------------------
struct FakeProcess : SteamProcessControl {
    bool running = false;
    bool neverExits = false;
    bool launchOk = true;
    bool exitOk = true;
    int exitAfterPolls = 2;
    qulonglong signInAs = 0; // ActiveUser written when Steam is "started"
    bool launchedRunning = true;
    QString registryKey;
    QStringList ops;
    bool exitRequested = false;
    int polls = 0;

    bool steamRunning(QString* which) override {
        if (exitRequested && !neverExits && ++polls >= exitAfterPolls) {
            running = false;
            exitRequested = false;
        }
        if (running && which) *which = QStringLiteral("steam.exe");
        return running;
    }
    bool requestExit(const QString&, QString* error) override {
        ops << QStringLiteral("exit");
        if (!exitOk) { if (error) *error = QStringLiteral("no"); return false; }
        exitRequested = true;
        return true;
    }
    bool launch(const QString&, QString* error) override {
        ops << QStringLiteral("launch");
        if (!launchOk) { if (error) *error = QStringLiteral("launch refused"); return false; }
        running = launchedRunning;
        QSettings active(registryKey + QStringLiteral("\\ActiveProcess"), QSettings::NativeFormat);
        active.setValue(QStringLiteral("ActiveUser"), static_cast<uint>(signInAs));
        active.sync();
        return true;
    }
};

struct World {
    QTemporaryDir dir;
    QString root;
    QString vdf;
    QString exe;
    QString key;
    QByteArray original;
    bool init(const QString& registryKey, const QByteArray& content) {
        if (!dir.isValid()) return false;
        root = QDir::fromNativeSeparators(dir.path());
        QDir(root).mkpath(QStringLiteral("config"));
        vdf = root + QStringLiteral("/config/loginusers.vdf");
        exe = root + QStringLiteral("/steam.exe");
        key = registryKey;
        original = content;
        QFile f(vdf);
        if (!f.open(QIODevice::WriteOnly) || f.write(content) != content.size()) return false;
        f.close();
        QFile e(exe);
        if (!e.open(QIODevice::WriteOnly)) return false;
        e.close();
        return true;
    }
    QByteArray read() const {
        QFile f(vdf);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
    }
    QStringList backups() const {
        return QDir(root + QStringLiteral("/config")).entryList({QStringLiteral("loginusers.vdf.xmb-backup-*")}, QDir::Files, QDir::Name);
    }
};

void clearKey(const QString& key) {
#ifdef _WIN32
    const QString sub = key.mid(QStringLiteral("HKEY_CURRENT_USER\\").size());
    RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(sub.utf16()));
#else
    Q_UNUSED(key);
#endif
}

SteamSwitchOptions optionsFor(const World& w, const char* id, const char* name) {
    SteamSwitchOptions o;
    o.steamRoot = w.root;
    o.steamExe = w.exe;
    o.steamId64 = QString::fromLatin1(id);
    o.accountName = QString::fromLatin1(name);
    o.personaName = QStringLiteral("Persona ") + o.accountName;
    o.registryKey = w.key;
    o.exitTimeoutMs = 600;
    o.settleMs = 20;
    o.verifyTimeoutMs = 700;
    o.pollMs = 20;
    return o;
}

SteamSwitchResult run(const SteamSwitchOptions& options, FakeProcess* fake, QStringList* progressLines = nullptr) {
    fake->registryKey = options.registryKey;
    // The switcher owns its process control, so a non-owning forwarder keeps the fake inspectable afterwards.
    struct Forward : SteamProcessControl {
        FakeProcess* f;
        explicit Forward(FakeProcess* p) : f(p) {}
        bool steamRunning(QString* w) override { return f->steamRunning(w); }
        bool requestExit(const QString& e, QString* err) override { return f->requestExit(e, err); }
        bool launch(const QString& e, QString* err) override { return f->launch(e, err); }
    };
    SteamAccountSwitcher switcher(options, std::make_unique<Forward>(fake));
    SteamSwitchResult result;
    bool done = false;
    QEventLoop loop;
    QObject::connect(&switcher, &SteamAccountSwitcher::progress, [&](const QString& t) { if (progressLines) progressLines->append(t); });
    QObject::connect(&switcher, &SteamAccountSwitcher::finished, [&](const SteamSwitchResult& r) { result = r; done = true; loop.quit(); });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    QTimer::singleShot(0, &switcher, &SteamAccountSwitcher::start);
    loop.exec();
    check(done, "the switch finished within the failsafe");
    return result;
}

QString regString(const QString& key, const char* name, bool* present = nullptr) {
    QSettings s(key, QSettings::NativeFormat);
    if (present) *present = s.contains(QLatin1String(name));
    return s.value(QLatin1String(name)).toString();
}

void switchTests(const QString& baseKey) {
    const QByteArray content = file({user(IdA, "alicea", "Al", "1", "1", "1"), user(IdB, "bobbyb", "Cee", "1", "0", "0")});
    const qulonglong idB = 39734274ULL;
    int n = 0;
    const auto freshKey = [&]() { return baseKey + QStringLiteral("\\case") + QString::number(++n); };

    // A. Steam running: it is asked to exit, waited for, the selection is written, Steam is started, ActiveUser
    //    names the account -> SignedIn.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "A: scratch root");
        QSettings pre(key, QSettings::NativeFormat);
        pre.setValue(QStringLiteral("AutoLoginUser"), QStringLiteral("alicea"));
        pre.setValue(QStringLiteral("RememberPassword"), 0);
        pre.sync();
        FakeProcess fake;
        fake.running = true;
        fake.signInAs = idB;
        QStringList progress;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake, &progress);
        check(r.outcome == SteamSwitchResult::Outcome::SignedIn && r.ok && r.changed, "A: signed in");
        check(r.message.contains(QStringLiteral("Signed in as Persona bobbyb")), "A: message names the persona");
        check((fake.ops == QStringList{QStringLiteral("exit"), QStringLiteral("launch")}), "A: exit requested once, then launch");
        checkEq(w.read(), file({user(IdA, "alicea", "Al", "1", "1", "0"), user(IdB, "bobbyb", "Cee", "1", "1", "1")}),
                "A: loginusers.vdf rewritten");
        check(regString(key, "AutoLoginUser") == QLatin1String("bobbyb"), "A: AutoLoginUser is the login name");
        QSettings after(key, QSettings::NativeFormat);
        check(after.value(QStringLiteral("RememberPassword")).toInt() == 1 && after.value(QStringLiteral("RememberPassword")).typeId() != QMetaType::QString,
              "A: RememberPassword is a DWORD 1");
        const QStringList backups = w.backups();
        check(backups.size() == 1, "A: one backup");
        if (backups.size() == 1) {
            QFile b(w.root + QStringLiteral("/config/") + backups.first());
            check(b.open(QIODevice::ReadOnly) && b.readAll() == content, "A: the backup is the original, byte for byte");
        }
        check(progress.contains(QStringLiteral("Closing Steam")) && progress.contains(QStringLiteral("Starting Steam")),
              "A: progress reported");
        // End to end through the reader the XMB uses: with the scratch key as Steam's registry the account is active.
        QSettings steamKey(key, QSettings::NativeFormat);
        steamKey.setValue(QStringLiteral("SteamPath"), w.root);
        steamKey.sync();
        qputenv("XMB_STEAM_REGISTRY_KEY", key.toUtf8());
        const auto accounts = scanSteamAccounts(w.root);
        check(accounts.size() == 2 && accounts.first().accountName == QLatin1String("bobbyb") && accounts.first().active,
              "A: scanSteamAccounts agrees the account is active");
        clearKey(key);
    }
    // B. Steam not running: no exit request.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "B: scratch root");
        FakeProcess fake;
        fake.running = false;
        fake.signInAs = idB;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake);
        check(r.outcome == SteamSwitchResult::Outcome::SignedIn, "B: signed in");
        check(fake.ops == QStringList{QStringLiteral("launch")}, "B: no exit request when Steam is not running");
        check(regString(key, "AutoLoginUser") == QLatin1String("bobbyb"), "B: registry written");
        clearKey(key);
    }
    // C. Steam does not exit: bounded wait, then nothing changed.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "C: scratch root");
        FakeProcess fake;
        fake.running = true;
        fake.neverExits = true;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake);
        check(r.outcome == SteamSwitchResult::Outcome::Failed && !r.ok && !r.changed, "C: failed");
        check(r.message.contains(QStringLiteral("did not close")), "C: says Steam did not close");
        check(w.read() == content && w.backups().isEmpty(), "C: the file and the folder are untouched");
        bool present = true;
        regString(key, "AutoLoginUser", &present);
        check(!present, "C: the registry is untouched");
        check(fake.ops == QStringList{QStringLiteral("exit")}, "C: no launch");
        clearKey(key);
    }
    // D. Launch fails after the writes: everything is put back (previous values present).
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "D: scratch root");
        QSettings pre(key, QSettings::NativeFormat);
        pre.setValue(QStringLiteral("AutoLoginUser"), QStringLiteral("alicea"));
        pre.setValue(QStringLiteral("RememberPassword"), 0);
        pre.sync();
        FakeProcess fake;
        fake.launchOk = false;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake);
        check(r.outcome == SteamSwitchResult::Outcome::Failed && r.restored && !r.changed, "D: failed and restored");
        check(w.read() == content, "D: loginusers.vdf restored byte for byte");
        check(regString(key, "AutoLoginUser") == QLatin1String("alicea"), "D: AutoLoginUser restored");
        QSettings after(key, QSettings::NativeFormat);
        check(after.value(QStringLiteral("RememberPassword")).toInt() == 0, "D: RememberPassword restored");
        clearKey(key);
    }
    // D2. Same, with no previous values: they are removed again.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "D2: scratch root");
        FakeProcess fake;
        fake.launchOk = false;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake);
        check(r.restored && w.read() == content, "D2: restored");
        bool a = true, b = true;
        regString(key, "AutoLoginUser", &a);
        regString(key, "RememberPassword", &b);
        check(!a && !b, "D2: values that did not exist are removed again");
        clearKey(key);
    }
    // E. The account file cannot be replaced (read-only): nothing changed, a clear message.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "E: scratch root");
        QFile::setPermissions(w.vdf, QFileDevice::ReadOwner);
        FakeProcess fake;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake);
        QFile::setPermissions(w.vdf, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        check(r.outcome == SteamSwitchResult::Outcome::Failed && !r.changed, "E: failed");
        check(w.read() == content, "E: the file is unchanged");
        bool present = true;
        regString(key, "AutoLoginUser", &present);
        check(!present, "E: the registry was never written");
        check(!fake.ops.contains(QStringLiteral("launch")), "E: Steam was not started");
        clearKey(key);
    }
    // F. Steam shows its login window: ActiveUser stays 0.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "F: scratch root");
        FakeProcess fake;
        fake.signInAs = 0;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake);
        check(r.outcome == SteamSwitchResult::Outcome::LoginWindow && r.ok && r.changed, "F: login window reported");
        check(r.message.contains(QStringLiteral("sign-in window")) && r.message.contains(QStringLiteral("Steam Guard")), "F: message explains it");
        clearKey(key);
    }
    // G. Steam signs in as the other account.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "G: scratch root");
        FakeProcess fake;
        fake.signInAs = 39734273ULL;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake);
        check(r.outcome == SteamSwitchResult::Outcome::OtherAccount && r.message.contains(QStringLiteral("not Persona bobbyb")),
              "G: other account named in the message");
        clearKey(key);
    }
    // G2. Steam is not running at the end.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "G2: scratch root");
        FakeProcess fake;
        fake.launchedRunning = false;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake);
        check(r.outcome == SteamSwitchResult::Outcome::NotRunning, "G2: not running reported");
        clearKey(key);
    }
    // H. An account without a saved sign-in is refused before anything is closed.
    {
        const QByteArray noRemember = file({user(IdA, "a", "A", "1", "1", "1"), user(IdB, "bobbyb", "Cee", "0", "0", "0")});
        World w;
        const QString key = freshKey();
        check(w.init(key, noRemember), "H: scratch root");
        FakeProcess fake;
        fake.running = true;
        const auto r = run(optionsFor(w, IdB, "bobbyb"), &fake);
        check(r.outcome == SteamSwitchResult::Outcome::Failed && fake.ops.isEmpty(), "H: refused, Steam was never touched");
        check(w.read() == noRemember && w.backups().isEmpty(), "H: file untouched");
        // A login name that does not match the file is refused too.
        World w2;
        check(w2.init(freshKey(), content), "H: second root");
        FakeProcess fake2;
        const auto r2 = run(optionsFor(w2, IdB, "someone-else"), &fake2);
        check(r2.outcome == SteamSwitchResult::Outcome::Failed && fake2.ops.isEmpty() && w2.read() == content, "H: name mismatch refused");
        clearKey(key);
        clearKey(w2.key);
    }
    // I. Hold: the plan is logged and nothing changes.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "I: scratch root");
        FakeProcess fake;
        fake.running = true;
        SteamSwitchOptions o = optionsFor(w, IdB, "bobbyb");
        o.hold = true;
        const auto r = run(o, &fake);
        check(r.outcome == SteamSwitchResult::Outcome::Held && fake.ops.isEmpty() && w.read() == content && w.backups().isEmpty(),
              "I: hold changes nothing and starts nothing");
        bool present = true;
        regString(key, "AutoLoginUser", &present);
        check(!present, "I: no registry value written");
        clearKey(key);
    }
    // J. select=false (no saved sign-in): Steam is closed and started, nothing is written.
    {
        const QByteArray noRemember = file({user(IdA, "a", "A", "1", "1", "1"), user(IdB, "bobbyb", "Cee", "0", "0", "0")});
        World w;
        const QString key = freshKey();
        check(w.init(key, noRemember), "J: scratch root");
        FakeProcess fake;
        fake.running = true;
        SteamSwitchOptions o = optionsFor(w, IdB, "bobbyb");
        o.select = false;
        const auto r = run(o, &fake);
        check(r.outcome == SteamSwitchResult::Outcome::Started && r.ok && !r.changed, "J: started");
        check((fake.ops == QStringList{QStringLiteral("exit"), QStringLiteral("launch")}), "J: exit then launch");
        check(w.read() == noRemember && w.backups().isEmpty(), "J: file untouched, no backup");
        bool present = true;
        regString(key, "AutoLoginUser", &present);
        check(!present, "J: registry untouched");
        check(r.message.contains(QStringLiteral("Remember my password")), "J: tells the user to sign in once");
        clearKey(key);
    }
    // K. Old backups are pruned to five.
    {
        World w;
        const QString key = freshKey();
        check(w.init(key, content), "K: scratch root");
        for (int i = 0; i < 8; ++i) {
            QFile old(w.root + QStringLiteral("/config/loginusers.vdf.xmb-backup-2020010%1-000000").arg(i + 1));
            check(old.open(QIODevice::WriteOnly), "K: old backup");
            old.write("x");
        }
        FakeProcess fake;
        fake.signInAs = idB;
        run(optionsFor(w, IdB, "bobbyb"), &fake);
        check(w.backups().size() == 5, "K: five backups kept");
        check(!w.backups().isEmpty() && !w.backups().last().contains(QStringLiteral("2020")), "K: the newest is kept");
        clearKey(key);
    }
}

// ---- the registry seam and the prompt text ----------------------------------------------------------
void seamAndPromptTests(const QString& baseKey) {
    qunsetenv("XMB_STEAM_REGISTRY_KEY");
    check(steamRegistryKeyPath() == QLatin1String("HKEY_CURRENT_USER\\Software\\Valve\\Steam") && !steamRegistryKeyIsTestSeam(),
          "seam: the real key by default");
    // A rejected override must never fall back to the real key (a scratch run that mistyped its key once wrote the
    // real Steam registry): it becomes a dead key under XmbTest.
    const QString dead = QStringLiteral("HKEY_CURRENT_USER\\Software\\XmbTest\\RejectedOverride");
    for (const char* bad : {"HKEY_CURRENT_USER\\Software\\Valve\\Other", "HKCU\\Software\\XmbTest\\Steam",
                            "HKEY_LOCAL_MACHINE\\Software\\XmbTest\\x", "HKEY_CURRENT_USER\\Software\\XmbTest\\..\\Valve\\Steam",
                            "HKEY_CURRENT_USER\\Software\\XmbTest\\", "HKEY_CURRENT_USER\\Software\\Valve\\Steam"}) {
        qputenv("XMB_STEAM_REGISTRY_KEY", bad);
        check(steamRegistryKeyPath() == dead, bad);
    }
    qputenv("XMB_STEAM_REGISTRY_KEY", baseKey.toUtf8());
    check(steamRegistryKeyIsTestSeam() && steamRegistryKeyPath() == baseKey, "seam: a key under XmbTest is accepted");

    const QByteArray content = file({user(IdA, "alicea", "Al", "1", "1", "1"), user(IdB, "bobbyb", "Cee", "1", "0", "0"),
                                     user(IdC, "nosaved", "NoSaved", "0", "0", "0")});
    World w;
    check(w.init(baseKey + QStringLiteral("\\prompt"), content), "prompt: scratch root");
    QSettings key(w.key, QSettings::NativeFormat);
    key.setValue(QStringLiteral("SteamPath"), w.root);
    key.setValue(QStringLiteral("AutoLoginUser"), QStringLiteral("alicea"));
    key.sync();
    qputenv("XMB_STEAM_REGISTRY_KEY", w.key.toUtf8());
    const auto accounts = scanSteamAccounts(w.root);
    check(accounts.size() == 3, "prompt: three accounts");
    SteamAccount remembered, notRemembered, active;
    for (const auto& a : accounts) {
        if (a.accountName == QLatin1String("bobbyb")) remembered = a;
        else if (a.accountName == QLatin1String("nosaved")) notRemembered = a;
        else active = a;
    }
    check(active.active && !remembered.active, "prompt: AutoLoginUser marks the active account");

    const auto auto1 = steamSwitchPlanFor(remembered, w.root);
    check(auto1.possible && auto1.automatic && auto1.steps.size() == 2, "prompt: remembered account is automatic");
    check(auto1.steps.at(1).body.contains(QStringLiteral("AutoLoginUser = bobbyb")) &&
              auto1.steps.at(1).body.contains(QStringLiteral("loginusers.vdf")) &&
              auto1.steps.at(1).body.contains(QStringLiteral("backed up")),
          "prompt: step 2 names the registry value, the file and the backup");
    check(auto1.steps.at(0).body.contains(QStringLiteral("Steam signs in by itself")), "prompt: step 1 says Steam signs in itself");
    const auto manual = steamSwitchPlanFor(notRemembered, w.root);
    check(manual.possible && !manual.automatic && manual.steps.size() == 2, "prompt: an account with no saved sign-in is not automatic");
    check(manual.steps.at(0).body.contains(QStringLiteral("changes nothing")) && manual.steps.at(1).body.contains(QStringLiteral("Remember my password")),
          "prompt: the manual path tells the user to sign in once");
    const auto already = steamSwitchPlanFor(active, w.root);
    check(!already.possible || already.steps.isEmpty(), "prompt: the active account has no steps");
    check(already.blocked.body.contains(QStringLiteral("already")) && already.blocked.body.contains(QStringLiteral("nothing was changed")),
          "prompt: the active account says so");

    const auto dry = steamSwitchDryRun(remembered, w.root);
    check(dry.possible && !dry.executed && dry.automatic && dry.commands.size() == 2, "dry run: two commands, not executed");
    check(dry.commands.at(0).endsWith(QStringLiteral("steam.exe -shutdown")) && dry.commands.at(1).endsWith(QStringLiteral("steam.exe")),
          "dry run: exactly -shutdown, then no arguments");
    check(dry.actions.size() == 4 && dry.actions.at(2).endsWith(QStringLiteral("AutoLoginUser=bobbyb")) &&
              dry.actions.at(3).contains(QStringLiteral("RememberPassword=1 (DWORD)")),
          "dry run: the exact registry writes are listed");
    check(dry.actions.at(1).contains(QStringLiteral("config/loginusers.vdf")), "dry run: the file path is listed");
    QString all = dry.transcript.join(QLatin1Char('\n')) + dry.actions.join(QLatin1Char('\n')) + dry.commands.join(QLatin1Char('\n'));
    check(!all.contains(QStringLiteral("-login"), Qt::CaseInsensitive), "dry run: no -login anywhere");
    const auto dryManual = steamSwitchDryRun(notRemembered, w.root);
    check(!dryManual.automatic && dryManual.actions.size() == 1 && dryManual.actions.first().contains(QStringLiteral("no file or registry value")),
          "dry run: nothing is written for an account with no saved sign-in");
    clearKey(w.key);
}

} // namespace

// Pure decisions behind the library refresh after a switch (src/steam_refresh.h).
void refreshDecisionTests() {
    using O = SteamSwitchResult::Outcome;
    check(steamSwitchRefreshesLibrary(O::SignedIn), "refresh: SignedIn refreshes");
    check(steamSwitchRefreshesLibrary(O::OtherAccount), "refresh: OtherAccount refreshes");
    for (O o : {O::LoginWindow, O::NotRunning, O::Started, O::Held, O::Failed})
        check(!steamSwitchRefreshesLibrary(o), "refresh: no library refresh for the other outcomes");
    check(steamSwitchWatchesActiveUser(O::LoginWindow), "refresh: LoginWindow waits for ActiveUser");
    for (O o : {O::SignedIn, O::OtherAccount, O::NotRunning, O::Started, O::Held, O::Failed})
        check(!steamSwitchWatchesActiveUser(o), "refresh: only LoginWindow watches ActiveUser");

    check(ownedResponseIsStale(IdA, IdB), "stale: a response for A while B is active is stale");
    check(!ownedResponseIsStale(IdB, IdB), "stale: same account is current");
    check(!ownedResponseIsStale(QString(), IdB), "stale: unknown requested id is not stale");
    check(!ownedResponseIsStale(IdA, QString()), "stale: no current account is not stale");
    check(!ownedResponseIsStale(QString(), QString()), "stale: nothing known is not stale");

    check(steamIdFromActiveUser(39734273) == QLatin1String(IdA), "ActiveUser 39734273 is IdA");
    check(steamIdFromActiveUser(0).isEmpty(), "ActiveUser 0 is nobody");
    check(steamIdFromActiveUser(0x100000000ULL).isEmpty(), "ActiveUser above 32 bits is rejected");
    check(steamIdFromActiveUser(0xffffffffULL) == QString::number(76561197960265728ULL + 0xffffffffULL), "ActiveUser max");

    check(steamRefreshName(" Reported ", "Persona", "Label") == QLatin1String("Reported"), "name: reported wins and is trimmed");
    check(steamRefreshName({}, "Persona", "Label") == QLatin1String("Persona"), "name: persona next");
    check(steamRefreshName({}, "  ", "Label") == QLatin1String("Label"), "name: blank persona falls to label");
    check(steamRefreshName({}, {}, {}) == QLatin1String("Steam"), "name: fallback");

    ActiveUserWatch watch;
    check(!watch.observe(39734273), "watch: first observation only primes");
    check(!watch.observe(39734273), "watch: unchanged is quiet");
    check(!watch.observe(0), "watch: signed out is remembered, not reported");
    check(watch.observe(39734274), "watch: a different account is reported");
    check(!watch.observe(39734274), "watch: reported once");
    check(watch.observe(39734273), "watch: switching back is reported");
    watch.reset();
    check(!watch.observe(0), "watch: after reset, primes at 0 (login window)");
    check(watch.observe(39734274), "watch: login window then sign-in is reported");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString baseKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\XmbTest\\SteamSwitchTests-%1").arg(QCoreApplication::applicationPid());
    qputenv("XMB_STEAM_REGISTRY_KEY", baseKey.toUtf8());
    if (!steamRegistryKeyIsTestSeam() || steamRegistryKeyPath() != baseKey) {
        std::fprintf(stderr, "refusing to run: the registry seam is not in effect\n");
        return 2;
    }
    rewriteTests();
    modernSchemaTests();
    switchTests(baseKey);
    seamAndPromptTests(baseKey);
    refreshDecisionTests();
    clearKey(baseKey);
    std::printf("steam switch tests: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
