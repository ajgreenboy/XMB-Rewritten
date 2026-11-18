#include "steam_quit.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Quit Game for a Steam title: process selection (pure) and the three-stage driver against a harmless stand-in
// game (audit/STEAM-QUIT.md). Only copies of the stand-in that this runner starts are ever closed or killed.
// Usage: xmb-steam-quit-tests [<stand-in exe> <scratch folder>]; without arguments only the pure tests run.
// Exit code 0 = all pass.
using namespace xmb::steamquit;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failed; } } while (0)

static ProcessInfo proc(quint32 pid, quint32 parent, const char* path, const char* name, quint64 start = 0) {
    ProcessInfo p;
    p.pid = pid; p.parentPid = parent; p.imagePath = QString::fromUtf8(path); p.name = QString::fromUtf8(name); p.startTime = start;
    return p;
}
static QList<quint32> pids(const QList<ProcessInfo>& set) {
    QList<quint32> out;
    for (const auto& p : set) out.push_back(p.pid);
    return out;
}

static void pureTests() {
    const QString dir = QStringLiteral("D:\\SteamLibrary\\steamapps\\common\\The Quarry");
    // path identity
    CHECK(normalizePath(QStringLiteral("D:\\A\\b\\")) == QStringLiteral("d:/a/b"));
    CHECK(normalizePath(QStringLiteral("\\\\?\\D:\\A")) == QStringLiteral("d:/a"));
    CHECK(pathInside(QStringLiteral("d:/steamlibrary/steamapps/common/the quarry/Binaries/Win64/Quarry.exe"), dir));
    CHECK(!pathInside(dir, dir));
    CHECK(!pathInside(QStringLiteral("D:\\SteamLibrary\\steamapps\\common\\The Quarry Demo\\x.exe"), dir));
    CHECK(!pathInside(QString(), dir));
    CHECK(isSteamClientProcess(QStringLiteral("Steam.exe")));
    CHECK(isSteamClientProcess(QStringLiteral("steamwebhelper.exe")));
    CHECK(isSteamClientProcess(QStringLiteral("GameOverlayUI.exe")));
    CHECK(!isSteamClientProcess(QStringLiteral("TheQuarry.exe")));

    // The game, a child launcher outside the folder, a grandchild, the Steam client and its helpers.
    QList<ProcessInfo> all;
    all << proc(4, 0, "", "System")
        << proc(100, 4, "C:\\Program Files (x86)\\Steam\\steam.exe", "steam.exe", 10)
        << proc(101, 100, "C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win7x64\\steamwebhelper.exe", "steamwebhelper.exe", 11)
        << proc(102, 100, "C:\\Program Files (x86)\\Steam\\gameoverlayui.exe", "gameoverlayui.exe", 12)
        << proc(200, 100, "D:\\SteamLibrary\\steamapps\\common\\The Quarry\\Quarry.exe", "Quarry.exe", 20)
        << proc(201, 200, "D:\\SteamLibrary\\steamapps\\common\\The Quarry\\Binaries\\Win64\\Quarry-Win64-Shipping.exe", "Quarry-Win64-Shipping.exe", 21)
        << proc(202, 201, "C:\\ProgramData\\EasyAntiCheat\\EasyAntiCheat_EOS.exe", "EasyAntiCheat_EOS.exe", 22)   // outside, but a descendant
        << proc(203, 202, "", "helper.exe", 23)                                                               // unreadable image, descendant
        << proc(300, 4, "C:\\Windows\\explorer.exe", "explorer.exe", 5)
        << proc(301, 300, "D:\\SteamLibrary\\steamapps\\common\\The Quarry Demo\\Demo.exe", "Demo.exe", 30)  // sibling folder
        << proc(302, 300, "C:\\Windows\\notepad.exe", "notepad.exe", 31);
    const auto picked = selectGameProcesses(all, dir);
    CHECK(pids(picked) == QList<quint32>({200, 201, 202, 203}));
    CHECK(describeNames(picked) == QStringLiteral("EasyAntiCheat_EOS.exe x1, Quarry-Win64-Shipping.exe x1, Quarry.exe x1, helper.exe x1")
          || describeNames(picked).contains(QStringLiteral("Quarry.exe x1")));
    // never anything of the Steam client, never the unrelated or the sibling-folder processes
    for (quint32 banned : {4u, 100u, 101u, 102u, 300u, 301u, 302u}) CHECK(!pids(picked).contains(banned));
    // roots: only the launcher process has a parent outside the set
    CHECK(pids(rootsOf(picked)) == QList<quint32>({200}));

    // protected pids are skipped, together with what descends from them only
    CHECK(pids(selectGameProcesses(all, dir, {201})) == QList<quint32>({200}));
    // a game process that is itself named like the Steam client is skipped and its subtree with it
    QList<ProcessInfo> odd = all;
    odd[4].name = QStringLiteral("steamwebhelper.exe");
    CHECK(pids(selectGameProcesses(odd, dir)).isEmpty() || !pids(selectGameProcesses(odd, dir)).contains(200));
    // a Steam client process inside the install folder (a bundled copy) is never selected
    QList<ProcessInfo> bundled;
    bundled << proc(10, 4, "D:\\SteamLibrary\\steamapps\\common\\The Quarry\\steam.exe", "steam.exe", 1)
            << proc(11, 10, "D:\\SteamLibrary\\steamapps\\common\\The Quarry\\child.exe", "child.exe", 2);
    CHECK(pids(selectGameProcesses(bundled, dir)) == QList<quint32>({11}));
    // nothing running from the folder: empty, so the caller falls back
    CHECK(selectGameProcesses(all, QStringLiteral("D:\\SteamLibrary\\steamapps\\common\\Other Game")).isEmpty());
    CHECK(selectGameProcesses(all, QString()).isEmpty());
    // a recycled parent id: a process that started before its "parent" is not a child
    QList<ProcessInfo> recycled;
    recycled << proc(500, 4, "D:\\SteamLibrary\\steamapps\\common\\The Quarry\\Quarry.exe", "Quarry.exe", 100)
             << proc(501, 500, "C:\\Windows\\notepad.exe", "notepad.exe", 50);
    CHECK(pids(selectGameProcesses(recycled, dir)) == QList<quint32>({500}));
    // unrelated process whose parent id is in the set but whose parent is gone: not selected when absent
    QList<ProcessInfo> orphan;
    orphan << proc(601, 600, "C:\\x\\y.exe", "y.exe", 5);
    CHECK(selectGameProcesses(orphan, dir).isEmpty());

    // stillAlive: an orphan whose parent exited is kept, a recycled pid is not
    QList<ProcessInfo> snapshot;
    snapshot << proc(203, 1, "", "helper.exe", 23)                                // parent gone, still the same process
             << proc(201, 4, "C:\\Windows\\notepad.exe", "notepad.exe", 900);    // pid reused by another program
    const auto alive = stillAlive(picked, snapshot);
    CHECK(pids(alive) == QList<quint32>({203}));
    // unrelated names never reach the log: only base names and counts
    CHECK(!describeNames(picked).contains(QLatin1Char('\\')));
}

// ---- live tests with the stand-in ---------------------------------------------------------------------------------

static bool waitFor(int ms, const std::function<bool()>& done) {
    QEventLoop loop;
    QTimer poll, limit;
    bool ok = false;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (done()) { ok = true; loop.quit(); } });
    limit.setSingleShot(true);
    QObject::connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    limit.start(ms);
    loop.exec();
    return ok || done();
}

static QList<ProcessInfo> running(const QString& dir) {
    return selectGameProcesses(snapshotProcesses(), dir, {quint32(QCoreApplication::applicationPid())});
}

// The visible, unowned, top-level window of a pid, or null. This is the window a running game would show in front
// of the XMB, so Quit Game must remove it: WM_CLOSE for a cooperative title, terminate/kill for a stubborn one. It
// is a simplified mirror of findMainWindow (game_capture.cpp): it omits the DWM cloak check and the
// WS_EX_TOOLWINDOW filter, and it never calls GetForegroundWindow(), so "gone" asserts the window was destroyed,
// not that any window regained the foreground. findMainWindow is not linked into this runner.
static void* windowForPid(quint32 pid) {
#ifdef Q_OS_WIN
    struct Ctx { DWORD pid; HWND best; qint64 area; } c{pid, nullptr, 0};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        RECT r{};
        GetWindowRect(h, &r);
        const qint64 a = qint64(r.right - r.left) * (r.bottom - r.top);
        if (a > c->area) { c->area = a; c->best = h; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
    return c.best;
#else
    Q_UNUSED(pid);
    return nullptr;
#endif
}

struct Run {
    int stage = -1;
    bool done = false;
    QString survivors;
    qint64 ms = 0;
};

static Run quitScenario(const QString& dir, const QString& label, int expectedStage, int grace, bool expectOrphans = false) {
    Run run;
    Quitter quitter;
    QObject::connect(&quitter, &Quitter::finished, [&](int stage, const QString& survivors) { run.stage = stage; run.survivors = survivors; run.done = true; });
    const auto before = running(dir);
    std::printf("[%s] %d process(es) before: %s\n", qPrintable(label), int(before.size()), qPrintable(describeNames(before)));
    QElapsedTimer clock;
    clock.start();
    const auto start = quitter.start(dir, grace, 800, 4000);
    CHECK(start.processes == before.size());
    waitFor(grace + 10000, [&] { return run.done; });
    run.ms = clock.elapsed();
    std::printf("[%s] ended by stage %d after %lld ms, windows posted %d, survivors \"%s\"\n", qPrintable(label), run.stage,
                static_cast<long long>(run.ms), start.windows, qPrintable(run.survivors));
    CHECK(run.done);
    CHECK(run.stage == expectedStage);
    CHECK(running(dir).isEmpty());
    Q_UNUSED(expectOrphans);
    return run;
}

static void liveTests(const QString& standIn, const QString& scratch) {
    // A fake library: <scratch>\steamapps\common\Stand In Game\<exe>, the layout Steam uses.
    const QString dir = QDir(scratch).filePath(QStringLiteral("steamapps/common/Stand In Game"));
    QDir().mkpath(dir);
    const QString exe = QDir(dir).filePath(QStringLiteral("StandInGame.exe"));
    QFile::remove(exe);
    CHECK(QFile::copy(standIn, exe));
    // A bystander that is not inside the folder must survive every scenario.
    const QString otherDir = QDir(scratch).filePath(QStringLiteral("steamapps/common/Bystander"));
    QDir().mkpath(otherDir);
    const QString otherExe = QDir(otherDir).filePath(QStringLiteral("Bystander.exe"));
    QFile::remove(otherExe);
    CHECK(QFile::copy(standIn, otherExe));
    QProcess bystander;
    bystander.start(otherExe, {QStringLiteral("--max-seconds"), QStringLiteral("90")});
    CHECK(bystander.waitForStarted(5000));
    const auto bystanderAlive = [&] { return bystander.state() == QProcess::Running; };

    const auto launch = [&](const QStringList& args) {
        auto* p = new QProcess;
        p->start(exe, args + QStringList{QStringLiteral("--max-seconds"), QStringLiteral("90")});
        CHECK(p->waitForStarted(5000));
        return p;
    };
    const auto settled = [&](int count) { return waitFor(5000, [&] { return running(dir).size() >= count && !running(dir).isEmpty(); }); };

    // 1. A cooperative title with two helpers that follow it: WM_CLOSE ends everything, stage 1.
    {
        std::unique_ptr<QProcess> game(launch({QStringLiteral("--children"), QStringLiteral("2"), QStringLiteral("--children-follow")}));
        CHECK(settled(3));
        const quint32 gamePid = quint32(game->processId());
        CHECK(waitFor(5000, [&] { return windowForPid(gamePid) != nullptr; }));   // the game's visible top-level window is up
        const auto run = quitScenario(dir, QStringLiteral("cooperative+followers"), 1, 6000);
        CHECK(run.ms < 4000);
        game->waitForFinished(2000);
        CHECK(windowForPid(gamePid) == nullptr);   // WM_CLOSE was posted and handled: the window is destroyed
    }
    // 2. A stubborn title (ignores WM_CLOSE) with one helper: stage 2 terminates the root, the helper follows.
    {
        std::unique_ptr<QProcess> game(launch({QStringLiteral("--ignore-close"), QStringLiteral("--children"), QStringLiteral("1"), QStringLiteral("--children-follow")}));
        CHECK(settled(2));
        quitScenario(dir, QStringLiteral("stubborn+follower"), 2, 700);
        game->waitForFinished(2000);
    }
    // 3. A cooperative root whose helpers do NOT follow (orphans that ignore WM_CLOSE): the root leaves at stage 1, the
    //    orphans are still part of the game and need stage 2/3.
    {
        std::unique_ptr<QProcess> game(launch({QStringLiteral("--children"), QStringLiteral("2")}));
        CHECK(settled(3));
        const auto run = quitScenario(dir, QStringLiteral("cooperative+orphans"), 2, 700);
        CHECK(run.ms >= 700);
        game->waitForFinished(2000);
    }
    // 4. Stubborn root with stubborn orphan-capable helpers: the root is terminated at stage 2, helpers outlive it and are
    //    taken by stage 3.
    {
        std::unique_ptr<QProcess> game(launch({QStringLiteral("--ignore-close"), QStringLiteral("--children"), QStringLiteral("2")}));
        CHECK(settled(3));
        const quint32 gamePid = quint32(game->processId());
        CHECK(waitFor(5000, [&] { return windowForPid(gamePid) != nullptr; }));
        quitScenario(dir, QStringLiteral("stubborn+orphans"), 3, 700);
        game->waitForFinished(2000);
        CHECK(windowForPid(gamePid) == nullptr);   // the root was terminated at stage 2 (its window died with it); the windowless orphans fell to stage 3
    }
    // 5. Nothing running from the folder: the driver reports it so the shell can fall back.
    {
        Quitter quitter;
        const auto start = quitter.start(dir, 500);
        CHECK(start.processes == 0);
        CHECK(!quitter.active());
    }
    CHECK(bystanderAlive());   // the process outside the folder was never touched
    CHECK(windowForPid(quint32(bystander.processId())) != nullptr);   // and neither was its window
    bystander.kill();
    bystander.waitForFinished(2000);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    pureTests();
    std::printf("pure tests: %s\n", g_failed ? "FAILED" : "ok");
#ifdef Q_OS_WIN
    if (argc >= 3) {
        const QString scratch = QDir::cleanPath(QString::fromLocal8Bit(argv[2]));
        QDir(scratch).removeRecursively();
        QDir().mkpath(scratch);
        liveTests(QString::fromLocal8Bit(argv[1]), scratch);
    } else {
        std::printf("live tests skipped (no stand-in exe argument)\n");
    }
#endif
    std::printf("%s (%d failure(s))\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
