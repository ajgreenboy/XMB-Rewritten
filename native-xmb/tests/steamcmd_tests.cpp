#include "steamcmd.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <cstdio>
#include <functional>

// SteamCMD client tests (audit/STEAMCMD.md). Pure parts first (prompt matching on partial buffers, progress, command
// lines, zip check, manifests), then the real Run class against the stand-in steamcmd (tests/steamcmd_standin.cpp),
// which never touches Steam. Credentials used here are fake constants that exist only in this file and in the
// stand-in; the runner scans every captured argument, environment variable, log line and scratch file for them.
// Usage: xmb-steamcmd-tests [<stand-in exe> <scratch folder>]; without arguments only the pure tests run.
using namespace xmb::steamcmd;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failed; } } while (0)

static const QString kPassword = QStringLiteral("Tr0ub4dor&3-standin");
static const QString kEmailCode = QStringLiteral("AB12C");
static const QString kAppCode = QStringLiteral("7K3PQ");
static constexpr int kWaitMs = 30000;   // CI runners are slow

static QStringList g_log;   // every Qt log line the run produced
static void captureHandler(QtMsgType, const QMessageLogContext&, const QString& message) { g_log << message; }

static QString lineFor(const char* text) { return QString::fromUtf8(text); }

// ---- pure ---------------------------------------------------------------------------------------------------
static void pureTests() {
    CHECK(validAccountName(QStringLiteral("some_user-1.x")));
    CHECK(!validAccountName(QStringLiteral("ab")));
    CHECK(!validAccountName(QStringLiteral("+quit")));
    CHECK(!validAccountName(QStringLiteral("a b c")));
    CHECK(!validAccountName(QStringLiteral("user\"x")));
    CHECK(!validAccountName(QStringLiteral("user\n+quit")));
    CHECK(validAppId(QStringLiteral("480")));
    CHECK(!validAppId(QString()));
    CHECK(!validAppId(QStringLiteral("48a")));
    CHECK(!validAppId(QStringLiteral("12345678901")));
    CHECK(safeFolderName(QStringLiteral("Half-Life: Alyx"), QStringLiteral("546560")) == QStringLiteral("Half-Life Alyx"));
    CHECK(safeFolderName(QStringLiteral("CON"), QStringLiteral("7")) == QStringLiteral("app_7"));
    CHECK(safeFolderName(QStringLiteral("..\\.."), QStringLiteral("7")) == QStringLiteral("app_7"));
    CHECK(safeFolderName(QStringLiteral("a/b\\c"), QStringLiteral("7")) == QStringLiteral("a b c"));

    // Prompts have no newline: they are matched on the unterminated tail, even split over several reads.
    CHECK(OutputScanner::classifyTail(QStringLiteral("password: ")) == Prompt::Password);
    CHECK(OutputScanner::classifyTail(QStringLiteral("Steam Guard code:")) == Prompt::GuardEmail);
    CHECK(OutputScanner::classifyTail(QStringLiteral("Two-factor code: ")) == Prompt::GuardApp);
    CHECK(OutputScanner::classifyTail(QStringLiteral("Waiting for confirmation...")) == Prompt::None);
    CHECK(OutputScanner::classifyTail(QString()) == Prompt::None);
    {
        OutputScanner s;
        s.feed("Logging in user 'x' to Steam Public...\r\npass");
        CHECK(OutputScanner::classifyTail(s.tail()) == Prompt::None);
        CHECK(s.takeLines() == QStringList{QStringLiteral("Logging in user 'x' to Steam Public...")});
        s.feed("word: ");
        CHECK(OutputScanner::classifyTail(s.tail()) == Prompt::Password);
        s.clearTail();
        CHECK(s.tail().isEmpty());
    }
    {
        // Bare CR redraws, CRLF as one terminator, ANSI colour sequences and NULs.
        OutputScanner s;
        s.feed("\x1b[1;32m Update state (0x61) downloading, progress: 5.00 (50 / 1000)\r Update state (0x61) downloading, progress: 6.00 (60 / 1000)\r\nline two\r\n");
        s.feed(QByteArray("a\0b\n", 4));
        const auto lines = s.takeLines();
        CHECK(lines.size() == 4);
        CHECK(lines.value(0).startsWith(QStringLiteral("Update state")));
        CHECK(lines.value(2) == QStringLiteral("line two"));
        CHECK(lines.value(3) == QStringLiteral("ab"));
        CHECK(s.tail().isEmpty());
    }
    {
        // A multi-byte character split across two reads is decoded whole.
        OutputScanner s;
        s.feed("caf\xc3");
        s.feed("\xa9\n");
        CHECK(s.takeLines() == QStringList{QString::fromUtf8("caf\xc3\xa9")});
    }

    const auto p = OutputScanner::parseProgress(QStringLiteral("Update state (0x61) downloading, progress: 12.34 (1234 / 10000)"));
    CHECK(p.valid && p.stateCode == 0x61 && p.label == QStringLiteral("downloading") && qAbs(p.percent - 12.34) < 1e-6 && p.done == 1234 && p.total == 10000);
    CHECK(OutputScanner::parseProgress(QStringLiteral("Update state (0x5) validating, progress: 13.48 (1554089956 / 11530459441)")).total == 11530459441LL);
    CHECK(!OutputScanner::parseProgress(QStringLiteral("progress: 1")).valid);

    CHECK(OutputScanner::classifyLine(lineFor("Logging in user 'x' to Steam Public...FAILED (Invalid Password)")) == Line::BadPassword);
    CHECK(OutputScanner::classifyLine(lineFor("FAILED (Rate Limit Exceeded)")) == Line::RateLimited);
    CHECK(OutputScanner::classifyLine(lineFor("FAILED (Two-factor code mismatch)")) == Line::BadGuardCode);
    CHECK(OutputScanner::classifyLine(lineFor("FAILED (Invalid Login Auth Code)")) == Line::BadGuardCode);
    CHECK(OutputScanner::classifyLine(lineFor("Please confirm the login in the Steam Mobile app on your phone.")) == Line::MobileConfirm);
    CHECK(OutputScanner::classifyLine(lineFor("Waiting for confirmation...")) == Line::MobileWaiting);
    CHECK(OutputScanner::classifyLine(lineFor("Timed out waiting for confirmation.")) == Line::MobileTimedOut);
    CHECK(OutputScanner::classifyLine(lineFor("Waiting for user info...OK")) == Line::LoginOk);
    CHECK(OutputScanner::classifyLine(lineFor("Success! App '480' fully installed.")) == Line::JobOk);
    CHECK(OutputScanner::classifyLine(lineFor("ERROR! Failed to install app '481' (No subscription)")) == Line::NoSubscription);
    CHECK(OutputScanner::classifyLine(lineFor("Error! App '483' state is 0x202 after update job.")) == Line::JobFailed);
    CHECK(OutputScanner::classifyLine(lineFor("Redirecting stderr to 'x'")) == Line::Other);
    CHECK(OutputScanner::failureText(lineFor("Logging in user 'x' to Steam Public...FAILED (Invalid Password)")) == QStringLiteral("Invalid Password"));

    // Command lines: the account name only; force_install_dir before login; nothing secret can be in them.
    const auto signIn = signInArguments(QStringLiteral("someone"));
    CHECK(signIn.contains(QStringLiteral("+login")) && signIn.indexOf(QStringLiteral("someone")) == signIn.indexOf(QStringLiteral("+login")) + 1);
    CHECK(signIn.last() == QStringLiteral("+quit"));
    const auto job = jobArguments(QStringLiteral("someone"), QStringLiteral("480"), QStringLiteral("D:/Lib/steamapps/common/Game"), true);
    CHECK(job.indexOf(QStringLiteral("+force_install_dir")) < job.indexOf(QStringLiteral("+login")));
    CHECK(job.indexOf(QStringLiteral("+@NoPromptForPassword")) < job.indexOf(QStringLiteral("+login")));
    CHECK(job.indexOf(QStringLiteral("+@sSteamCmdForcePlatformType")) < job.indexOf(QStringLiteral("+login")));
    CHECK(job.contains(QStringLiteral("validate")));
    CHECK(!jobArguments(QStringLiteral("someone"), QStringLiteral("480"), QStringLiteral("D:/x"), false).contains(QStringLiteral("validate")));
    CHECK(job.indexOf(QStringLiteral("+login")) + 2 == job.indexOf(QStringLiteral("+app_update")));   // no password slot between them

    // Secret
    {
        Secret s(kPassword);
        CHECK(s.size() == kPassword.toUtf8().size());
        Secret moved(std::move(s));
        CHECK(s.isEmpty() && !moved.isEmpty());
        moved.wipe();
        CHECK(moved.isEmpty());
    }
    CHECK(Run::scrub(QStringLiteral("you typed AB12C ok"), kEmailCode.toUtf8()) == QStringLiteral("you typed <hidden> ok"));
}

static QByteArray makeZip(const QStringList& names) {
    QByteArray body, central;
    quint32 offset = 0;
    const auto le16 = [](QByteArray& b, quint16 v) { b.append(char(v & 0xff)); b.append(char(v >> 8)); };
    const auto le32 = [](QByteArray& b, quint32 v) { for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xff)); };
    for (const QString& n : names) {
        const QByteArray name = n.toUtf8();
        const QByteArray data = QByteArray("data");
        body.append("PK\x03\x04", 4); le16(body, 20); le16(body, 0); le16(body, 0); le16(body, 0); le16(body, 0);
        le32(body, 0); le32(body, quint32(data.size())); le32(body, quint32(data.size())); le16(body, quint16(name.size())); le16(body, 0);
        body.append(name); body.append(data);
        central.append("PK\x01\x02", 4); le16(central, 20); le16(central, 20); le16(central, 0); le16(central, 0); le16(central, 0); le16(central, 0);
        le32(central, 0); le32(central, quint32(data.size())); le32(central, quint32(data.size())); le16(central, quint16(name.size()));
        le16(central, 0); le16(central, 0); le16(central, 0); le16(central, 0); le32(central, 0); le32(central, offset);
        central.append(name);
        offset = quint32(body.size());
    }
    QByteArray eocd;
    eocd.append("PK\x05\x06", 4); le16(eocd, 0); le16(eocd, 0); le16(eocd, quint16(names.size())); le16(eocd, quint16(names.size()));
    le32(eocd, quint32(central.size())); le32(eocd, quint32(body.size())); le16(eocd, 0);
    return body + central + eocd;
}

static void fileTests(const QString& scratch) {
    QString error;
    CHECK(zipHoldsSteamcmd(makeZip({QStringLiteral("steamcmd.exe")}), &error));
    CHECK(zipHoldsSteamcmd(makeZip({QStringLiteral("readme.txt"), QStringLiteral("SteamCMD.EXE")}), &error));
    CHECK(!zipHoldsSteamcmd(makeZip({QStringLiteral("readme.txt")}), &error) && error.contains(QStringLiteral("steamcmd.exe")));
    CHECK(!zipHoldsSteamcmd(makeZip({QStringLiteral("sub/steamcmd.exe")}), &error));
    CHECK(!zipHoldsSteamcmd(makeZip({QStringLiteral("steamcmd.exe"), QStringLiteral("../evil.dll")}), &error) && error.contains(QStringLiteral("outside")));
    CHECK(!zipHoldsSteamcmd(makeZip({QStringLiteral("steamcmd.exe"), QStringLiteral("C:/evil.dll")}), &error));
    CHECK(!zipHoldsSteamcmd(makeZip({QStringLiteral("steamcmd.exe"), QStringLiteral("/evil.dll")}), &error));
    CHECK(!zipHoldsSteamcmd(QByteArray("<html>Making sure you are not a bot</html>").repeated(10), &error) && error.contains(QStringLiteral("not a zip")));
    CHECK(!zipHoldsSteamcmd(QByteArray(), &error));
    {
        QByteArray damaged = makeZip({QStringLiteral("steamcmd.exe")});
        damaged.chop(30);
        CHECK(!zipHoldsSteamcmd(damaged, &error));
    }

    const QString root = QDir(scratch).filePath(QStringLiteral("files"));
    QDir(root).removeRecursively();
    QDir().mkpath(root + QStringLiteral("/tool"));
    QDir().mkpath(root + QStringLiteral("/clientfolder"));
    const auto touch = [](const QString& p) { QFile f(p); (void)f.open(QIODevice::WriteOnly); f.write("x"); };
    touch(root + QStringLiteral("/tool/steamcmd.exe"));
    touch(root + QStringLiteral("/clientfolder/steamcmd.exe"));
    touch(root + QStringLiteral("/clientfolder/steam.exe"));
    CHECK(resolveExecutable(root + QStringLiteral("/tool")) == QDir::cleanPath(root + QStringLiteral("/tool/steamcmd.exe")));
    CHECK(resolveExecutable(root + QStringLiteral("/tool/steamcmd.exe")) == QDir::cleanPath(root + QStringLiteral("/tool/steamcmd.exe")));
    CHECK(resolveExecutable(root + QStringLiteral("/nope")).isEmpty());
    CHECK(resolveExecutable(root).isEmpty());
    CHECK(looksLikeSteamClientFolder(root + QStringLiteral("/clientfolder")));
    const auto found = candidateExecutables(root + QStringLiteral("/clientfolder"), {}, {root + QStringLiteral("/tool")});
    CHECK(!found.isEmpty() && found.first().endsWith(QStringLiteral("tool/steamcmd.exe")));   // the client's own folder is never a candidate
    for (const QString& f : found) CHECK(!looksLikeSteamClientFolder(QFileInfo(f).absolutePath()));

    // Libraries from a scratch libraryfolders.vdf
    const QString steam = root + QStringLiteral("/Steam");
    QDir().mkpath(steam + QStringLiteral("/steamapps"));
    QDir().mkpath(root + QStringLiteral("/LibB"));
    {
        QFile f(steam + QStringLiteral("/steamapps/libraryfolders.vdf"));
        (void)f.open(QIODevice::WriteOnly);
        f.write(QStringLiteral("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"%1\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"%2\"\n\t}\n\t\"2\"\n\t{\n\t\t\"path\"\t\t\"%3\"\n\t}\n}\n")
                    .arg(steam, root + QStringLiteral("/LibB"), root + QStringLiteral("/Gone")).toUtf8());
    }
    const auto libs = steamLibraries(steam);
    CHECK(libs.size() == 2 && libs.value(1).endsWith(QStringLiteral("LibB")));   // a missing drive is left out, the root is not repeated
    CHECK(steamLibraries(QString()).isEmpty());

    // Manifest integration: files forced into <lib>/steamapps/common/<Game>, SteamCMD's manifest beside them.
    const QString lib = root + QStringLiteral("/LibB");
    QDir().mkpath(lib + QStringLiteral("/steamapps/common/Game"));
    QDir().mkpath(lib + QStringLiteral("/steamapps"));
    const QString forced = installFolderFor(lib, QStringLiteral("Game"));
    CHECK(forced.endsWith(QStringLiteral("LibB/steamapps/common/Game")));
    CHECK(integrateManifest(lib, forced, QStringLiteral("480")).kind == Integration::Kind::NoManifest);
    QDir().mkpath(forced + QStringLiteral("/steamapps"));
    const auto writeAcf = [&](const QString& dir, const QString& installdir) {
        QFile f(dir + QStringLiteral("/steamapps/appmanifest_480.acf"));
        (void)f.open(QIODevice::WriteOnly);
        f.write(QStringLiteral("\"AppState\"\n{\n\t\"appid\"\t\"480\"\n\t\"installdir\"\t\"%1\"\n}\n").arg(installdir).toUtf8());
    };
    writeAcf(forced, QStringLiteral("Other"));
    CHECK(integrateManifest(lib, forced, QStringLiteral("480")).kind == Integration::Kind::Mismatch);
    CHECK(!QFileInfo::exists(lib + QStringLiteral("/steamapps/appmanifest_480.acf")));
    writeAcf(forced, QStringLiteral("game"));   // case-insensitive, as Windows folders are
    CHECK(integrateManifest(lib, forced, QStringLiteral("480")).kind == Integration::Kind::Linked);
    CHECK(QFileInfo::exists(lib + QStringLiteral("/steamapps/appmanifest_480.acf")));
    CHECK(integrateManifest(lib, forced, QStringLiteral("480")).kind == Integration::Kind::AlreadyKnown);   // never overwritten
    CHECK(integrateManifest(lib, forced, QStringLiteral("4x")).kind != Integration::Kind::Linked);
}

// ---- processes ----------------------------------------------------------------------------------------------
struct Result {
    int outcome = -1;
    QString message;
    QVector<int> prompts, states;
    QStringList lines;
    int progressEvents = 0;
    double maxPercent = 0;
    bool timedOut = false;
};

// Runs `exe` with `args`. `onPrompt` answers a prompt (return false to leave it unanswered), `onProgress` can cancel.
// `cancelAfterMs` > 0 calls run.cancel() that long after start (for the cancel-vs-prompt-timeout regression).
static Result drive(Run& run, Run::Mode mode, const QString& exe, const QStringList& args,
                    const std::function<bool(Run&, int prompt, int count)>& onPrompt = {},
                    const std::function<void(Run&)>& onProgress = {},
                    int cancelAfterMs = 0) {
    Result r;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    int promptCount = 0;
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { r.timedOut = true; loop.quit(); });
    QObject::connect(&run, &Run::promptRequested, &loop, [&](int p) {
        r.prompts.push_back(p);
        if (onPrompt) onPrompt(run, p, ++promptCount);
    });
    QObject::connect(&run, &Run::stateChanged, &loop, [&](int s) { r.states.push_back(s); });
    QObject::connect(&run, &Run::lineSeen, &loop, [&](QString l) { r.lines << l; });
    QObject::connect(&run, &Run::progressChanged, &loop, [&] {
        ++r.progressEvents;
        r.maxPercent = std::max(r.maxPercent, run.lastProgress().percent);
        if (onProgress) onProgress(run);
    });
    QObject::connect(&run, &Run::finished, &loop, [&](int o, QString m) { r.outcome = o; r.message = m; loop.quit(); });
    timeout.start(kWaitMs);
    if (!run.start(mode, exe, args)) return r;
    if (cancelAfterMs > 0) QTimer::singleShot(cancelAfterMs, &loop, [&run] { run.cancel(); });
    loop.exec();
    timeout.stop();
    return r;
}

static bool answer(Run& run, const QString& text) { Secret s(text); return run.provide(s); }

static void processTests(const QString& exe, const QString& scratch) {
    const QString capture = QDir(scratch).filePath(QStringLiteral("capture.txt"));
    QFile::remove(capture);
    qputenv("XMB_STEAMCMD_STANDIN_CAPTURE", capture.toUtf8());
    const auto O = [](Run::Outcome o) { return int(o); };
    const QVector<int> states;

    // 1. password only, prompt arrives split over two reads with no newline
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("ok_user")),
            [](Run& rn, int prompt, int) { return prompt == int(Prompt::Password) && answer(rn, kPassword); });
        CHECK(!r.timedOut);
        CHECK(r.outcome == O(Run::Outcome::SignedIn));
        CHECK(r.prompts == QVector<int>{int(Prompt::Password)});
        CHECK(run.state() == Run::State::Done);
        CHECK(!run.running());
    }
    // 2. wrong password
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("ok_user")),
            [](Run& rn, int, int) { return answer(rn, QStringLiteral("not the password")); });
        CHECK(r.outcome == O(Run::Outcome::BadPassword));
        CHECK(r.message.contains(QStringLiteral("Invalid Password")));
    }
    // 3. email Steam Guard: password then code
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("guard_user")),
            [](Run& rn, int prompt, int) { return answer(rn, prompt == int(Prompt::Password) ? kPassword : kEmailCode); });
        CHECK(r.outcome == O(Run::Outcome::SignedIn));
        CHECK(r.prompts == (QVector<int>{int(Prompt::Password), int(Prompt::GuardEmail)}));
    }
    // 4. wrong guard code ends the run with the child's own reason
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("guard_user")),
            [](Run& rn, int prompt, int) { return answer(rn, prompt == int(Prompt::Password) ? kPassword : QStringLiteral("WRONG")); });
        CHECK(r.outcome == O(Run::Outcome::BadGuardCode));
        CHECK(r.message.contains(QStringLiteral("Invalid Login Auth Code")));
    }
    // 5. wrong code that SteamCMD asks for again: a second prompt, then success
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("guard_retry_user")),
            [](Run& rn, int prompt, int count) { return answer(rn, prompt == int(Prompt::Password) ? kPassword : count == 2 ? QStringLiteral("WRONG") : kEmailCode); });
        CHECK(r.outcome == O(Run::Outcome::SignedIn));
        CHECK(r.prompts.size() == 3);
    }
    // 6. mobile app code
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("app_user")),
            [](Run& rn, int prompt, int) { return answer(rn, prompt == int(Prompt::Password) ? kPassword : kAppCode); });
        CHECK(r.outcome == O(Run::Outcome::SignedIn));
        CHECK(r.prompts.value(1) == int(Prompt::GuardApp));
    }
    // 7. approve on the phone: no code prompt, the waiting state is reported, then signed in
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("mobile_user")),
            [](Run& rn, int, int) { return answer(rn, kPassword); });
        CHECK(r.outcome == O(Run::Outcome::SignedIn));
        CHECK(r.prompts == QVector<int>{int(Prompt::Password)});
        CHECK(r.states.contains(int(Run::State::WaitingMobile)));
    }
    // 8. the phone approval times out
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("mobile_timeout_user")),
            [](Run& rn, int, int) { return answer(rn, kPassword); });
        CHECK(r.outcome == O(Run::Outcome::MobileTimedOut));
    }
    // 9. rate limit before any prompt
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("ratelimit_user")));
        CHECK(r.outcome == O(Run::Outcome::RateLimited));
        CHECK(r.prompts.isEmpty());
        CHECK(r.message.contains(QStringLiteral("Rate Limit")));
    }
    // 10. a child that echoes the password: the echo never leaves Run
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("echo_user")),
            [](Run& rn, int, int) { return answer(rn, kPassword); });
        CHECK(r.outcome == O(Run::Outcome::SignedIn));
        bool sawEcho = false;
        for (const QString& l : r.lines) {
            CHECK(!l.contains(kPassword));
            if (l.contains(QStringLiteral("<hidden>"))) sawEcho = true;
        }
        CHECK(sawEcho);
    }
    // 11. a prompt that is never answered, then cancel: the child is gone
    {
        Run run;
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("ok_user")),
            [](Run& rn, int, int) { rn.cancel(); return true; });
        CHECK(r.outcome == O(Run::Outcome::Cancelled));
        CHECK(!run.running());
    }
    // 12. providing without a pending prompt writes nothing and wipes the secret
    {
        Run run;
        Secret s(kPassword);
        CHECK(!run.provide(s));
        CHECK(s.isEmpty());
    }
    // 13. a prompt that never arrives (SteamCMD's own buffer swallows it): bounded by the prompt timeout
    {
        Run run;
        run.setPromptTimeout(2000);
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("buffered_user")));
        CHECK(!r.timedOut);                       // Run ended on its own, not the harness 30 s guard
        CHECK(r.outcome == O(Run::Outcome::TimedOut));
        CHECK(r.prompts.isEmpty());               // no prompt was ever delivered
        CHECK(!run.running());
        CHECK(r.message.contains(QStringLiteral("pipe")));
    }
    // 14. cancel just before the prompt timeout still ends Cancelled, not TimedOut. The child ignores terminate
    //     (a console app with no message loop), so only the 1.5 s kill timer would end it; without cancel()
    //     stopping the prompt timer, the 1 s prompt timeout would fire first and win with TimedOut.
    {
        Run run;
        run.setPromptTimeout(1000);
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("buffered_user")), {}, {}, 600);
        CHECK(!r.timedOut);
        CHECK(r.outcome == O(Run::Outcome::Cancelled));
        CHECK(!run.running());
    }
    // 15. a guard prompt that never arrives after the password: the re-armed prompt timeout ends the run. Only the
    //     password prompt is ever delivered; the buffered guard code prompt never reaches the reader.
    {
        Run run;
        run.setPromptTimeout(2000);
        const auto r = drive(run, Run::Mode::SignIn, exe, signInArguments(QStringLiteral("buffered_guard_user")),
            [](Run& rn, int prompt, int) { return prompt == int(Prompt::Password) && answer(rn, kPassword); });
        CHECK(!r.timedOut);                       // Run ended on its own, not the harness 30 s guard
        CHECK(r.outcome == O(Run::Outcome::TimedOut));
        CHECK(r.prompts == (QVector<int>{int(Prompt::Password)}));   // the guard prompt never arrived
        CHECK(!run.running());
        CHECK(r.message.contains(QStringLiteral("pipe")));
    }

    // Jobs -----------------------------------------------------------------------------------------------------
    const QString library = QDir(scratch).filePath(QStringLiteral("Library"));
    QDir(library).removeRecursively();
    QDir().mkpath(library + QStringLiteral("/steamapps/common"));
    {
        Run run;
        const QString forced = installFolderFor(library, QStringLiteral("Test Game"));
        QDir().mkpath(forced);
        const auto r = drive(run, Run::Mode::Job, exe, jobArguments(QStringLiteral("cached_user"), QStringLiteral("480"), forced, true));
        CHECK(!r.timedOut);
        CHECK(r.outcome == O(Run::Outcome::JobDone));
        CHECK(r.prompts.isEmpty());
        CHECK(r.progressEvents >= 4);
        CHECK(r.maxPercent >= 99.9);
        CHECK(integrateManifest(library, forced, QStringLiteral("480")).kind == Integration::Kind::Linked);
        CHECK(QFileInfo::exists(library + QStringLiteral("/steamapps/appmanifest_480.acf")));
    }
    {
        Run run;
        const auto r = drive(run, Run::Mode::Job, exe, jobArguments(QStringLiteral("cached_user"), QStringLiteral("481"), library + QStringLiteral("/x"), true));
        CHECK(r.outcome == O(Run::Outcome::Failed));
        CHECK(r.message.contains(QStringLiteral("No subscription")));
    }
    {
        Run run;
        const auto r = drive(run, Run::Mode::Job, exe, jobArguments(QStringLiteral("cached_user"), QStringLiteral("483"), library + QStringLiteral("/x"), false));
        CHECK(r.outcome == O(Run::Outcome::Failed));
        CHECK(r.message.contains(QStringLiteral("0x202")));
    }
    {
        Run run;   // a stale cached login: the child refuses instead of asking
        const auto r = drive(run, Run::Mode::Job, exe, jobArguments(QStringLiteral("stale_user"), QStringLiteral("480"), library + QStringLiteral("/x"), true));
        CHECK(r.outcome == O(Run::Outcome::NeedsSignIn));
    }
    {
        Run run;   // a child that asks for a password anyway is stopped, never answered
        QElapsedTimer t;
        t.start();
        const auto r = drive(run, Run::Mode::Job, exe, jobArguments(QStringLiteral("prompt_user"), QStringLiteral("480"), library + QStringLiteral("/x"), true));
        CHECK(r.outcome == O(Run::Outcome::NeedsSignIn));
        CHECK(r.prompts.isEmpty());
        CHECK(!run.running());
    }
    {
        Run run;
        const auto r = drive(run, Run::Mode::Job, exe, jobArguments(QStringLiteral("ratelimit_user"), QStringLiteral("480"), library + QStringLiteral("/x"), true));
        CHECK(r.outcome == O(Run::Outcome::RateLimited));
    }
    {
        Run run;   // a download that never ends is cancelled from its progress
        bool cancelled = false;
        const auto r = drive(run, Run::Mode::Job, exe, jobArguments(QStringLiteral("cached_user"), QStringLiteral("482"), library + QStringLiteral("/x"), true), {},
            [&](Run& rn) { if (!cancelled) { cancelled = true; rn.cancel(); } });
        CHECK(!r.timedOut);
        CHECK(r.outcome == O(Run::Outcome::Cancelled));
        CHECK(!run.running());
    }
    {
        Run run;
        const auto r = drive(run, Run::Mode::Job, QDir(scratch).filePath(QStringLiteral("missing.exe")), jobArguments(QStringLiteral("cached_user"), QStringLiteral("480"), QStringLiteral("x"), true));
        CHECK(r.outcome == O(Run::Outcome::NotStarted));
    }

    // The credential audit: the secrets are in no argument, no environment variable, no log line and no file ---
    QFile cap(capture);
    CHECK(cap.open(QIODevice::ReadOnly));
    const QString captured = QString::fromUtf8(cap.readAll());
    CHECK(captured.contains(QStringLiteral("arg: +login")));
    CHECK(captured.contains(QStringLiteral("env: ")));
    CHECK(captured.contains(QStringLiteral("stdin: password length=%1 match=1").arg(kPassword.toUtf8().size())));   // it did arrive, on stdin
    CHECK(captured.contains(QStringLiteral("stdin: code length=5 match=1")));
    for (const QString& secret : {kPassword, kEmailCode, kAppCode}) {
        CHECK(!captured.contains(secret));                         // no argument, no environment variable
        for (const QString& l : g_log) CHECK(!l.contains(secret)); // no log line
    }
    int scanned = 0;
    QDirIterator it(scratch, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile f(it.next());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        ++scanned;
        for (const QString& secret : {kPassword, kEmailCode, kAppCode}) CHECK(!bytes.contains(secret.toUtf8()));
    }
    CHECK(scanned > 0);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qInstallMessageHandler(captureHandler);
    pureTests();
    if (argc >= 3) {
        const QString scratch = QDir::cleanPath(QString::fromLocal8Bit(argv[2]));
        QDir(scratch).removeRecursively();
        QDir().mkpath(scratch);
        fileTests(scratch);
        processTests(QDir::cleanPath(QString::fromLocal8Bit(argv[1])), scratch);
    }
    if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
    std::printf("steamcmd tests passed\n");
    return 0;
}
