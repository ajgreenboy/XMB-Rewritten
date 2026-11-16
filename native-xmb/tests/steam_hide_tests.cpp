#include "steam_hidden.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <cstdio>
#include <functional>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Steam background mode (audit/STEAM-HIDDEN.md): the exact start switches, the kill switch, the readiness state machine,
// the window rules (pure), and the live start/wait/hide path against tests/steam_hide_standin.cpp, a stand-in steam.exe
// that only ever touches a registry key under HKEY_CURRENT_USER\Software\XmbTest\. The real Steam is never started,
// stopped or read. The live window tests open small windows on the desktop for a moment: run them under the gate lock.
// Usage: xmb-steam-hide-tests [<stand-in exe> <scratch folder>]; without arguments only the pure tests run.
using namespace xmb::steamhide;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failed; } } while (0)

static ClientState state(bool alive, qulonglong user) {
    ClientState s;
    s.pid = alive ? 4242 : 0;
    s.alive = alive;
    s.activeUser = user;
    return s;
}

static void switchTests() {
    const QStringList on = startArguments(true);
    CHECK(on == (QStringList{QStringLiteral("-silent"), QStringLiteral("-nofriendsui"), QStringLiteral("-nochatui")}));
    CHECK(startArguments(false).isEmpty());
    for (const char* forbidden : {"-no-browser", "-noreactlogin", "-noverifyfiles", "-cef-disable-gpu", "-tenfoot", "-bigpicture",
                                  "-login", "-nobigpicture", "-console", "-dev", "-applaunch", "-shutdown"})
        CHECK(!on.contains(QLatin1String(forbidden)));

    qunsetenv("XMB_STEAM_HIDE");
    CHECK(!killSwitchEngaged());
    CHECK(effectiveEnabled(true));
    CHECK(!effectiveEnabled(false));
    qputenv("XMB_STEAM_HIDE", "0");
    CHECK(killSwitchEngaged());
    CHECK(!effectiveEnabled(true));
    qputenv("XMB_STEAM_HIDE", "1");
    CHECK(!killSwitchEngaged());
    CHECK(effectiveEnabled(true));
    qputenv("XMB_STEAM_HIDE", "");
    CHECK(!killSwitchEngaged());
    qunsetenv("XMB_STEAM_HIDE");
}

static void readinessTests() {
    const Timing t{20000, 90000, 600000};
    // Not running: starting until the timeout, then failed with the real reason.
    CHECK(evaluate(state(false, 0), 0, t).phase == Phase::Starting);
    CHECK(evaluate(state(false, 0), 89999, t).phase == Phase::Starting);
    CHECK(evaluate(state(false, 0), 90000, t).phase == Phase::Failed);
    CHECK(evaluate(state(false, 0), 90000, t).text.contains(QStringLiteral("90 seconds")));
    // A stale ActiveUser with no live client is not a running Steam.
    CHECK(evaluate(state(false, 77), 1000, t).phase == Phase::Starting);
    CHECK(evaluate(state(false, 77), 95000, t).phase == Phase::Failed);
    // Running and signed in: ready at once, at any time.
    CHECK(evaluate(state(true, 77), 0, t).phase == Phase::Ready);
    CHECK(evaluate(state(true, 77), 500000, t).phase == Phase::Ready);
    // Running, nobody signed in: starting, then "needs you to sign in", never failed before the give-up time.
    CHECK(evaluate(state(true, 0), 5000, t).phase == Phase::Starting);
    CHECK(evaluate(state(true, 0), 20000, t).phase == Phase::SignInNeeded);
    CHECK(evaluate(state(true, 0), 20000, t).text.contains(QStringLiteral("needs you to sign in")));
    CHECK(evaluate(state(true, 0), 120000, t).phase == Phase::SignInNeeded);   // past the start timeout: the client exists
    CHECK(evaluate(state(true, 0), 599999, t).phase == Phase::SignInNeeded);
    CHECK(evaluate(state(true, 0), 600000, t).phase == Phase::Failed);
    CHECK(state(true, 5).signedIn() && !state(true, 0).signedIn() && !state(false, 5).signedIn());
}

static WindowFacts win(const char* title, const char* proc = "steam.exe", bool signedIn = true) {
    WindowFacts f;
    f.title = QString::fromUtf8(title);
    f.processName = QString::fromUtf8(proc);
    f.clientProcess = true;
    f.signedIn = signedIn;
    return f;
}

static void windowRuleTests() {
    // Hidden: the windows a signed-in client puts up on its own.
    CHECK(classifyWindow(win("Steam")) == WindowVerdict::Hide);
    CHECK(classifyWindow(win("Steam Big Picture Mode")) == WindowVerdict::Hide);
    CHECK(classifyWindow(win("Steam - News (2 of 3)")) == WindowVerdict::Hide);
    CHECK(classifyWindow(win("Friends List")) == WindowVerdict::Hide);
    CHECK(classifyWindow(win("Steam - Updating")) == WindowVerdict::Hide);
    CHECK(classifyWindow(win("Steam", "steamwebhelper.exe")) == WindowVerdict::Hide);
    CHECK(classifyWindow(win("STEAM")) == WindowVerdict::Hide);
    // Kept, as sign-in windows: by title, in any state.
    CHECK(classifyWindow(win("Sign in to Steam")) == WindowVerdict::KeepSignIn);
    CHECK(classifyWindow(win("Steam Guard")) == WindowVerdict::KeepSignIn);
    CHECK(classifyWindow(win("Steam Guard - Computer Authorization Required")) == WindowVerdict::KeepSignIn);
    CHECK(classifyWindow(win("Steam - Login")) == WindowVerdict::KeepSignIn);
    CHECK(classifyWindow(win("Enter your security code")) == WindowVerdict::KeepSignIn);
    // Signed out: whatever the title (it may be in another language), nothing is hidden.
    CHECK(classifyWindow(win("Steam", "steam.exe", false)) == WindowVerdict::KeepSignIn);
    CHECK(classifyWindow(win("Anmelden bei Steam", "steam.exe", false)) == WindowVerdict::KeepSignIn);
    CHECK(classifyWindow(win("Friends List", "steamwebhelper.exe", false)) == WindowVerdict::KeepSignIn);
    // Kept, as messages: errors and warnings the user must read.
    CHECK(classifyWindow(win("Steam - Error")) == WindowVerdict::KeepMessage);
    CHECK(classifyWindow(win("Steam - Not enough disk space")) == WindowVerdict::KeepMessage);
    CHECK(classifyWindow(win("Failed to launch game")) == WindowVerdict::KeepMessage);
    // Never touched: not the client, the overlay, hidden windows, owned dialogs, tool windows, untitled plumbing.
    WindowFacts game = win("Steam");
    game.clientProcess = false;
    game.processName = QStringLiteral("TheQuarry.exe");
    CHECK(classifyWindow(game) == WindowVerdict::Ignore);
    CHECK(classifyWindow(win("Steam", "gameoverlayui.exe")) == WindowVerdict::Ignore);
    CHECK(classifyWindow(win("Steam", "gameoverlayui64.exe")) == WindowVerdict::Ignore);
    CHECK(classifyWindow(win("Steam", "TheQuarry.exe")) == WindowVerdict::Ignore);
    WindowFacts f = win("Steam");
    f.visible = false;
    CHECK(classifyWindow(f) == WindowVerdict::Ignore);
    f = win("Steam"); f.owned = true;
    CHECK(classifyWindow(f) == WindowVerdict::Ignore);
    f = win("Steam"); f.toolWindow = true;
    CHECK(classifyWindow(f) == WindowVerdict::Ignore);
    CHECK(classifyWindow(win("")) == WindowVerdict::Ignore);
    CHECK(classifyWindow(win("   ")) == WindowVerdict::Ignore);
    // The XMB itself is not a Steam window, even when it is called Steam.
    f = win("Steam"); f.clientProcess = false; f.processName = QStringLiteral("re-ps3-native.exe");
    CHECK(classifyWindow(f) == WindowVerdict::Ignore);
    CHECK(titleLooksLikeSignIn(QStringLiteral("Sign in to Steam")));
    CHECK(!titleLooksLikeSignIn(QStringLiteral("Steam")));
    CHECK(titleLooksLikeMessage(QStringLiteral("Steam - Error")));
    CHECK(!titleLooksLikeMessage(QStringLiteral("Steam - News")));
}

#ifdef Q_OS_WIN
namespace {
QString g_standin, g_scratch;
int g_keyCounter = 0;

QString newKey() {
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\XmbTest\\HideTests\\run%1_%2").arg(GetCurrentProcessId()).arg(++g_keyCounter);
}
void killClient(const QString& key) {
    const ClientState s = readClientState(key);
    if (s.pid) if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, s.pid)) { TerminateProcess(h, 1); WaitForSingleObject(h, 5000); CloseHandle(h); }
}
void dropKey(const QString& key) {
    QSettings k(key, QSettings::NativeFormat);
    k.remove(QString());
    k.sync();
}
void setEnvironment(const QString& key, const QString& record, const char* ready, const char* user, const char* signIn, const QString& windows) {
    qputenv("XMB_STEAM_REGISTRY_KEY", key.toUtf8());
    qputenv("XMB_STANDIN_RECORD", record.toUtf8());
    qputenv("XMB_STANDIN_READY_MS", ready);
    qputenv("XMB_STANDIN_USER", user);
    if (signIn) qputenv("XMB_STANDIN_SIGNIN_MS", signIn); else qunsetenv("XMB_STANDIN_SIGNIN_MS");
    qputenv("XMB_STANDIN_WINDOWS", windows.toUtf8());
    qputenv("XMB_STANDIN_MAX_SECONDS", "40");
}
QStringList lines(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
}
struct Outcome { bool ready = false; bool failed = false; bool signIn = false; QStringList statuses; QString failure; };
// Runs the starter until it ends (generous 30 s wait: CI runners are slow).
Outcome runStarter(const StartOptions& options) {
    ClientStarter starter;
    Outcome out;
    QEventLoop loop;
    QObject::connect(&starter, &ClientStarter::status, [&](const QString& t) { out.statuses << t; });
    QObject::connect(&starter, &ClientStarter::needsSignIn, [&](const QString&) { out.signIn = true; });
    QObject::connect(&starter, &ClientStarter::ready, [&] { out.ready = true; loop.quit(); });
    QObject::connect(&starter, &ClientStarter::failed, [&](const QString& t) { out.failed = true; out.failure = t; loop.quit(); });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    starter.begin(options);
    if (starter.running()) loop.exec();
    return out;
}
StartOptions optionsFor(const QString& key, bool hidden) {
    StartOptions o;
    o.steamExe = QDir(g_scratch).filePath(QStringLiteral("steam.exe"));
    o.hidden = hidden;
    o.registryKey = key;
    o.pollMs = 100;
    o.elevatedOverride = 0;
    return o;
}
bool windowVisible(const wchar_t* title) {
    const HWND h = FindWindowW(L"XmbSteamHideStandIn", title);
    return h && IsWindowVisible(h);
}
bool windowExists(const wchar_t* title) { return FindWindowW(L"XmbSteamHideStandIn", title) != nullptr; }
}  // namespace

static void liveTests() {
    QDir().mkpath(g_scratch);
    const QString steamExe = QDir(g_scratch).filePath(QStringLiteral("steam.exe"));
    QFile::remove(steamExe);
    CHECK(QFile::copy(g_standin, steamExe));
    const QString record = QDir(g_scratch).filePath(QStringLiteral("record.txt"));

    // 1. Hidden start: exact switch set on the command line, status text, ready from Steam's own registry values.
    {
        QFile::remove(record);
        const QString key = newKey();
        setEnvironment(key, record, "300", "1234", nullptr, QString());
        const Outcome o = runStarter(optionsFor(key, true));
        CHECK(o.ready && !o.failed);
        CHECK(o.statuses.contains(QStringLiteral("Starting Steam...")));
        const auto recorded = lines(record);
        CHECK(recorded.size() == 1);
        CHECK(recorded.value(0).contains(QStringLiteral("-silent -nofriendsui -nochatui")));
        CHECK(!recorded.value(0).contains(QStringLiteral("-no-browser")));
        // 2. Already running: ready at once, no second start.
        const Outcome again = runStarter(optionsFor(key, true));
        CHECK(again.ready && !again.failed);
        CHECK(lines(record).size() == 1);
        killClient(key);
        dropKey(key);
    }
    // 3. Background mode off: the start has no switches at all.
    {
        QFile::remove(record);
        const QString key = newKey();
        setEnvironment(key, record, "200", "1234", nullptr, QString());
        const Outcome o = runStarter(optionsFor(key, false));
        CHECK(o.ready);
        const auto recorded = lines(record);
        CHECK(recorded.size() == 1);
        CHECK(!recorded.value(0).contains(QStringLiteral("-silent")));
        CHECK(!recorded.value(0).contains(QStringLiteral("-nofriendsui")));
        killClient(key);
        dropKey(key);
    }
    // 4. Signed out at first: the sign-in state is reported, then ready once the user signs in.
    {
        QFile::remove(record);
        const QString key = newKey();
        setEnvironment(key, record, "100", "555", "2500", QString());
        StartOptions o = optionsFor(key, true);
        o.timing = Timing{800, 20000, 60000};
        const Outcome out = runStarter(o);
        CHECK(out.signIn);
        CHECK(out.ready && !out.failed);
        killClient(key);
        dropKey(key);
    }
    // 5. Never signs in: gives up with the honest text instead of hanging.
    {
        QFile::remove(record);
        const QString key = newKey();
        setEnvironment(key, record, "100", "0", nullptr, QString());
        StartOptions o = optionsFor(key, true);
        o.timing = Timing{400, 20000, 2000};
        const Outcome out = runStarter(o);
        CHECK(out.signIn && out.failed && !out.ready);
        CHECK(out.failure.contains(QStringLiteral("nobody signed in")));
        killClient(key);
        dropKey(key);
    }
    // 6. A client that never registers: times out with the real reason.
    {
        QFile::remove(record);
        const QString key = newKey();
        setEnvironment(key, record, "3500", "1234", nullptr, QString());
        qputenv("XMB_STANDIN_MAX_SECONDS", "1");
        StartOptions o = optionsFor(key, true);
        o.timing = Timing{20000, 1500, 60000};
        const Outcome out = runStarter(o);
        CHECK(out.failed && !out.ready);
        CHECK(out.failure.contains(QStringLiteral("did not start")));
        // The stand-in registers itself late and leaves at once; let it finish before the key is dropped.
        QEventLoop late;
        QTimer::singleShot(5500, &late, &QEventLoop::quit);
        late.exec();
        dropKey(key);
    }
    // 7. steam.exe missing, and an elevated token: refused before anything is started.
    {
        QFile::remove(record);
        const QString key = newKey();
        setEnvironment(key, record, "100", "1234", nullptr, QString());
        StartOptions o = optionsFor(key, true);
        o.steamExe = QDir(g_scratch).filePath(QStringLiteral("nothing-here.exe"));
        Outcome out = runStarter(o);
        CHECK(out.failed && out.failure.contains(QStringLiteral("steam.exe was not found")));
        o = optionsFor(key, true);
        o.elevatedOverride = 1;
        out = runStarter(o);
        CHECK(out.failed && out.failure.contains(QStringLiteral("administrator")));
        CHECK(lines(record).isEmpty());
        dropKey(key);
    }
    // 8. Window rules against live windows of the stand-in client (signed in): the main window goes, the sign-in
    //    and error windows stay, and a window that is not the client's is never touched.
    {
        QFile::remove(record);
        const QString key = newKey();
        setEnvironment(key, record, "200", "1234", nullptr, QStringLiteral("Steam|Sign in to Steam|Steam - Error|Steam - News"));
        const Outcome o = runStarter(optionsFor(key, true));
        CHECK(o.ready);
        QEventLoop settle;
        QTimer::singleShot(600, &settle, &QEventLoop::quit);
        settle.exec();
        CHECK(windowVisible(L"Steam") && windowVisible(L"Sign in to Steam"));
        const auto procs = clientProcesses(key);
        CHECK(procs.size() >= 1);
        // A map without the stand-in's pid (a game, the XMB) leaves every window alone.
        const SweepResult none = sweepWindows(QHash<quint32, QString>{{1, QStringLiteral("steam.exe")}}, true, true);
        CHECK(none.hidden == 0 && none.keepWindow == 0);
        CHECK(windowVisible(L"Steam"));
        // Classify only.
        const SweepResult dry = sweepWindows(procs, true, false);
        CHECK(dry.hidden == 2);   // "Steam" and "Steam - News"
        CHECK(dry.keepWindow != 0 && dry.keepIsSignIn && dry.keepTitle == QStringLiteral("Sign in to Steam"));
        CHECK(windowVisible(L"Steam"));
        // Apply.
        const SweepResult done = sweepWindows(procs, true, true);
        CHECK(done.hidden == 2);
        CHECK(windowExists(L"Steam") && !windowVisible(L"Steam"));
        CHECK(!windowVisible(L"Steam - News"));
        CHECK(windowVisible(L"Sign in to Steam"));
        CHECK(windowVisible(L"Steam - Error"));
        // A second pass has nothing left to hide.
        CHECK(sweepWindows(procs, true, true).hidden == 0);
        killClient(key);
        dropKey(key);
    }
    // 9. Signed out: nothing is hidden, the main window counts as the sign-in window and is to be brought forward.
    {
        QFile::remove(record);
        const QString key = newKey();
        setEnvironment(key, record, "100", "0", nullptr, QStringLiteral("Steam"));
        StartOptions o = optionsFor(key, true);
        o.timing = Timing{300, 20000, 1000};
        runStarter(o);
        QEventLoop settle;
        QTimer::singleShot(400, &settle, &QEventLoop::quit);
        settle.exec();
        const auto procs = clientProcesses(key);
        const SweepResult r = sweepWindows(procs, false, true);
        CHECK(r.hidden == 0);
        CHECK(r.keepWindow != 0 && r.keepIsSignIn);
        CHECK(windowVisible(L"Steam"));
        killClient(key);
        dropKey(key);
    }
}
#endif

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    switchTests();
    readinessTests();
    windowRuleTests();
#ifdef Q_OS_WIN
    if (argc >= 3) {
        g_standin = QString::fromLocal8Bit(argv[1]);
        g_scratch = QString::fromLocal8Bit(argv[2]);
        CHECK(QFileInfo(g_standin).isFile());
        if (QFileInfo(g_standin).isFile()) liveTests();
    }
#else
    Q_UNUSED(argc); Q_UNUSED(argv);
#endif
    std::printf(g_failed ? "FAILED: %d check(s)\n" : "OK: steam hide tests passed\n", g_failed);
    return g_failed ? 1 : 0;
}
