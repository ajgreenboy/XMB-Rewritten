#include "installer_panel.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

// The Installing panel's logic: the installer log parser, the truthful status, the exit classification, the close
// escalation state machine and the process-tree selection (pure), then the same pieces against the synthetic
// installer stand-in (tests/installer_standin.cpp). No real installer is ever started.
// Usage: xmb-installer-panel-tests [<stand-in exe> <scratch folder>]; without arguments only the pure tests run.
using namespace xmb::installerpanel;
using xmb::steamquit::ProcessInfo;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failed; } } while (0)

static bool hasLine(const StatusView& v, const char* label) {
    for (const auto& l : v.lines) if (l.label == QLatin1String(label)) return true;
    return false;
}
static QString lineValue(const StatusView& v, const char* label) {
    for (const auto& l : v.lines) if (l.label == QLatin1String(label)) return l.value;
    return {};
}
static ProcessInfo proc(quint32 pid, quint32 parent, quint64 start, const char* name = "x.exe") {
    ProcessInfo p;
    p.pid = pid; p.parentPid = parent; p.startTime = start; p.name = QString::fromUtf8(name);
    return p;
}

static void logTests() {
    InnoLogTracker t;
    CHECK(!t.state().seen);
    // Split across small chunks: a partial line is held until its newline arrives.
    const QByteArray text = QByteArray("\xEF\xBB\xBF") +
        "2026-11-22 10:00:00.100   Log opened. (Time zone: UTC+01:00)\r\n"
        "2026-11-22 10:00:00.200   Setup version: Inno Setup version 6.2.2\r\n"
        "2026-11-22 10:00:01.000   Starting the installation process.\r\n"
        "2026-11-22 10:00:01.100   Creating directory: D:\\Games\\Foo\r\n"
        "2026-11-22 10:00:01.200   -- File entry --\r\n"
        "2026-11-22 10:00:01.300   Dest filename: D:\\Games\\Foo\\data\\pack_0.bin\r\n"
        "2026-11-22 10:00:02.000   Extracting data: 42%\r\n";
    for (int i = 0; i < text.size(); i += 7) t.feed(text.mid(i, 7));
    const auto s = t.state();
    CHECK(s.seen && s.installing && !s.succeeded && !s.failed);
    CHECK(s.step == QLatin1String("Copying files"));
    CHECK(s.item == QLatin1String("pack_0.bin"));
    CHECK(s.fileEntries == 1);
    CHECK(s.percent == 42);

    t.feed("2026-11-22 10:00:03.000   Installing 1");   // no newline yet
    CHECK(t.state().percent == 42);                    // a partial line changes nothing
    t.feed("0% done\n");
    CHECK(t.state().percent == 10);                    // ... until it completes; the latest percentage wins

    // Lines that merely contain a number and a percent sign in an unrelated message never become progress.
    InnoLogTracker u;
    u.feed("2026-11-22 10:00:00.000   Setup version: Inno Setup version 6.2.2\n"
           "2026-11-22 10:00:00.000   Dest filename: C:\\x\\100%.dat\n"
           "2026-11-22 10:00:00.000   Disk space: 250 GB free\n");
    CHECK(u.state().percent == -1);                    // no progress line, no percentage: never estimated
    u.parseLine(QStringLiteral("Extracting: 140%"));
    CHECK(u.state().percent == -1);                    // out of range is ignored
    u.parseLine(QStringLiteral("Extracting: 3.5%"));
    CHECK(u.state().percent == -1);                    // a decimal fraction is not a whole percentage

    InnoLogTracker ok;
    ok.parseLine(QStringLiteral("2026-11-22 10:05:00.000   Installation process succeeded."));
    ok.parseLine(QStringLiteral("2026-11-22 10:05:00.100   Log closed."));
    CHECK(ok.state().succeeded && ok.state().closed && !ok.state().failed);

    InnoLogTracker bad;
    bad.parseLine(QStringLiteral("Exception message: Disk full"));
    bad.parseLine(QStringLiteral("Rolling back changes."));
    CHECK(bad.state().failed && bad.state().rollingBack && !bad.state().succeeded);
    CHECK(bad.state().error == QLatin1String("Disk full"));

    // A giant line without a newline cannot grow the buffer forever.
    InnoLogTracker big;
    big.feed(QByteArray(2 << 20, 'a'));
    big.feed("Installation process succeeded.\n");
    CHECK(big.state().succeeded);
}

static void statusTests() {
    CHECK(formatElapsed(0) == QLatin1String("0:00"));
    CHECK(formatElapsed(7400) == QLatin1String("0:07"));
    CHECK(formatElapsed(723000) == QLatin1String("12:03"));
    CHECK(formatElapsed(3723000) == QLatin1String("1:02:03"));
    CHECK(formatElapsed(-5) == QLatin1String("0:00"));
    CHECK(formatBytes(0) == QLatin1String("0 B"));
    CHECK(formatBytes(1536) == QLatin1String("1.50 KB"));
    CHECK(formatBytes(5ll * 1024 * 1024 * 1024) == QLatin1String("5.00 GB"));
    CHECK(formatBytes(-1).isEmpty());

    // Nothing observed but the process itself: only the elapsed time, and a headline that claims no step.
    StatusInputs none;
    none.running = true;
    none.elapsedMs = 5000;
    auto v = deriveStatus(none);
    CHECK(v.headline == QLatin1String("The installer is running"));
    CHECK(v.lines.size() == 1 && hasLine(v, "Elapsed"));
    CHECK(!hasLine(v, "Progress") && !hasLine(v, "Written to destination") && !hasLine(v, "Step"));

    // A log that exists but has no percentage never produces one; a folder that is not there produces no size.
    StatusInputs logged = none;
    logged.logAvailable = true;
    logged.log.step = QStringLiteral("Copying files");
    logged.log.item = QStringLiteral("a.bin");
    logged.log.fileEntries = 3;
    v = deriveStatus(logged);
    CHECK(v.headline == QLatin1String("Copying files"));
    CHECK(!hasLine(v, "Step"));                       // the headline already says it
    CHECK(lineValue(v, "Current file") == QLatin1String("a.bin"));
    CHECK(lineValue(v, "Files in log") == QLatin1String("3"));
    CHECK(!hasLine(v, "Progress"));
    CHECK(!hasLine(v, "Written to destination"));     // bytesWritten stays -1

    logged.log.percent = 61;
    logged.bytesWritten = 3 * 1024 * 1024;
    logged.bytesLowerBound = true;
    v = deriveStatus(logged);
    CHECK(lineValue(v, "Progress").startsWith(QLatin1String("61%")));
    CHECK(lineValue(v, "Written to destination") == QLatin1String("at least 3.00 MB"));

    // A destination that exists but is empty is a real 0 B, not an unknown.
    logged.bytesWritten = 0;
    logged.bytesLowerBound = false;
    CHECK(lineValue(deriveStatus(logged), "Written to destination") == QLatin1String("0 B"));

    // With no readable log the log-derived lines are all absent even if a stale LogState carries values.
    StatusInputs nolog = logged;
    nolog.logAvailable = false;
    v = deriveStatus(nolog);
    CHECK(v.headline == QLatin1String("The installer is running"));
    CHECK(!hasLine(v, "Progress") && !hasLine(v, "Current file"));

    StatusInputs fail = logged;
    fail.log.failed = true;
    fail.log.rollingBack = true;
    CHECK(deriveStatus(fail).headline == QLatin1String("The installer reports it is rolling back"));
    StatusInputs closing = none;
    closing.closeRequested = true;
    CHECK(deriveStatus(closing).headline.startsWith(QLatin1String("Close requested")));
    closing.awaitingForceConfirm = true;
    CHECK(deriveStatus(closing).headline.contains(QLatin1String("waiting for your answer")));
    closing.terminating = true;
    CHECK(deriveStatus(closing).headline == QLatin1String("Force closing the installer"));
    StatusInputs gone = none;
    gone.running = false;
    CHECK(deriveStatus(gone).headline == QLatin1String("The installer has exited"));
}

static void exitTests() {
    ExitInputs in;
    in.destinationHasFiles = true;
    CHECK(classifyExit(in) == Outcome::Succeeded);
    in.destinationHasFiles = false;
    CHECK(classifyExit(in) == Outcome::ExitedUnverified);       // exit 0 with nothing written is NOT a success
    in.destinationHasFiles = true;
    in.logAvailable = true;
    in.log.failed = true;
    CHECK(classifyExit(in) == Outcome::ExitedUnverified);       // exit 0 but the log reports a failure
    in.logAvailable = false;
    in.exitCode = 5;
    CHECK(classifyExit(in) == Outcome::Failed);
    in.closeRequested = true;
    CHECK(classifyExit(in) == Outcome::Cancelled);
    in.exitCode = 0;
    in.logAvailable = false;
    CHECK(classifyExit(in) == Outcome::Succeeded);              // it finished anyway before the close landed
    in.forced = true;
    CHECK(classifyExit(in) == Outcome::Cancelled);              // a terminated process never counts as installed
    ExitInputs crash;
    crash.crashed = true;
    crash.exitCode = 0xC0000005;
    CHECK(classifyExit(crash) == Outcome::Crashed);

    ExitInputs f;
    f.exitCode = 2;
    f.logAvailable = true;
    f.log.error = QStringLiteral("Disk full");
    const QString failed = outcomeText(Outcome::Failed, f, QStringLiteral("D:\\G"), QStringLiteral("C:\\log.txt"), 10);
    CHECK(failed.contains(QLatin1String("exit code 2")) && failed.contains(QLatin1String("C:\\log.txt")) && failed.contains(QLatin1String("Disk full")));
    ExitInputs z;
    const QString unverified = outcomeText(Outcome::ExitedUnverified, z, QStringLiteral("D:\\G"), QString(), 0);
    CHECK(unverified.contains(QLatin1String("not treated as installed")));
    const QString success = outcomeText(Outcome::Succeeded, z, QStringLiteral("D:\\G"), QString(), 2048);
    CHECK(success.contains(QLatin1String("exit")) && success.contains(QLatin1String("2.00 KB")));
    ExitInputs c;
    c.forced = true;
    const QString cancelled = outcomeText(Outcome::Cancelled, c, QStringLiteral("D:\\G"), QString(), -1);
    CHECK(cancelled.contains(QLatin1String("half-finished")) && cancelled.contains(QLatin1String("nothing was deleted")) && cancelled.contains(QLatin1String("force closed")));
}

static void escalationTests() {
    using A = CloseEscalation::Action;
    using S = CloseEscalation::Stage;
    CloseEscalation e(5000, 3000);
    CHECK(e.stage() == S::Idle && !e.closeRequested());
    CHECK(e.tick(0, true) == A::None);
    CHECK(e.confirmForce(0) == A::None);                 // never terminates without the question having been asked
    CHECK(!e.forced());

    CHECK(e.requestClose(1000) == A::SendClose);
    CHECK(e.stage() == S::PoliteSent && e.closeRequested());
    CHECK(e.requestClose(1500) == A::None);              // a second press inside the grace period is not a second close
    CHECK(e.tick(5999, true) == A::None);                // grace is measured from the first request
    CHECK(e.tick(6000, true) == A::AskForce);
    CHECK(e.stage() == S::AwaitingConfirm);
    CHECK(e.tick(7000, true) == A::None);                // asked once; no repeated prompts
    CHECK(e.requestClose(7100) == A::AskForce);          // pressing Close again re-asks
    e.declineForce();
    CHECK(e.stage() == S::Idle && !e.closeRequested() && !e.forced());   // declined: nothing was done, nothing claimed
    CHECK(e.requestClose(8000) == A::SendClose);         // and a new Close starts again politely
    CHECK(e.tick(13000, true) == A::AskForce);
    CHECK(e.confirmForce(13100) == A::Terminate);
    CHECK(e.stage() == S::Terminating && e.forced());
    CHECK(e.confirmForce(13200) == A::None);             // one terminate per confirmation
    CHECK(e.tick(14000, true) == A::None);
    CHECK(e.tick(16100, true) == A::None && e.stage() == S::TerminateFailed);   // survivor after the wait: reported, never retried
    CHECK(e.requestClose(17000) == A::SendClose);        // the owner may try again
    e.processExited();
    CHECK(e.stage() == S::Done);
    CHECK(e.requestClose(18000) == A::None && e.tick(19000, true) == A::None);

    // A polite close that works never asks about force.
    CloseEscalation p(5000);
    CHECK(p.requestClose(0) == A::SendClose);
    CHECK(p.tick(900, false) == A::None);
    CHECK(p.stage() == S::Done && !p.forced());
    // The installer ending while the question is open ends the sequence.
    CloseEscalation q(1000);
    q.requestClose(0);
    CHECK(q.tick(1000, true) == A::AskForce);
    CHECK(q.tick(1100, false) == A::None && q.stage() == S::Done);
    CHECK(q.confirmForce(1200) == A::None);
}

static void treeTests() {
    QList<ProcessInfo> all;
    all << proc(4, 0, 1) << proc(100, 4, 10, "xmb.exe") << proc(200, 100, 20, "setup.exe")
        << proc(300, 200, 30, "unpack.tmp") << proc(400, 300, 40, "helper.exe") << proc(500, 100, 25, "bystander.exe")
        << proc(600, 200, 5, "recycled-child.exe");   // claims parent 200 but started before it: a recycled id
    const auto tree = installerTree(all, 200, 20);
    QList<quint32> pids;
    for (const auto& p : tree) pids.push_back(p.pid);
    std::sort(pids.begin(), pids.end());
    CHECK((pids == QList<quint32>{200, 300, 400}));
    CHECK(installerTree(all, 200, 99).isEmpty());          // same pid, different creation time: another process
    CHECK(installerTree(all, 999, 0).isEmpty());           // the root is gone
    CHECK(installerTree(all, 200, 0).size() == 3);         // unknown start time still works
}

static void folderTests() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    CHECK(directoryBytes(dir.filePath(QStringLiteral("missing")), 100, nullptr) == -1);
    CHECK(!directoryHasFiles(dir.filePath(QStringLiteral("missing"))));
    bool lower = true;
    CHECK(directoryBytes(dir.path(), 100, &lower) == 0 && !lower);   // an existing empty folder is a real zero
    CHECK(!directoryHasFiles(dir.path()));
    QDir(dir.path()).mkpath(QStringLiteral("a/b"));
    QDir(dir.path()).mkpath(QStringLiteral("empty"));
    for (const char* name : {"a/one.bin", "a/b/two.bin"}) {
        QFile f(dir.filePath(QString::fromLatin1(name)));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(1000, 'x'));
    }
    CHECK(directoryBytes(dir.path(), 100, &lower) == 2000 && !lower);
    CHECK(directoryHasFiles(dir.path()));
    directoryBytes(dir.path(), 2, &lower);
    CHECK(lower);                                       // a capped scan says so
}

#ifdef Q_OS_WIN
static bool waitFor(int ms, const std::function<bool()>& ok) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (ok()) return true;
        QCoreApplication::processEvents();
        QThread::msleep(30);
    }
    return ok();
}

struct Run {
    std::unique_ptr<QProcess> process;
    QString dir, log;
    InnoLogTracker tracker;
    qint64 logOffset = 0;
    void poll() {
        QFile f(log);
        if (!f.open(QIODevice::ReadOnly)) return;
        f.seek(logOffset);
        const QByteArray bytes = f.readAll();
        logOffset += bytes.size();
        tracker.feed(bytes);
    }
    quint64 start() const {
        for (const auto& p : xmb::steamquit::snapshotProcesses()) if (p.pid == quint32(process->processId())) return p.startTime;
        return 0;
    }
    QList<ProcessInfo> tree() const { return installerTree(xmb::steamquit::snapshotProcesses(), quint32(process->processId()), start()); }
};

static Run launch(const QString& exe, const QString& scratch, const QString& name, const QString& mode, int seconds) {
    Run r;
    r.dir = QDir(scratch).filePath(name + QStringLiteral(" dest"));   // a space on purpose
    r.log = QDir(scratch).filePath(name + QStringLiteral(".log"));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XMB_STANDIN_MODE"), mode);
    env.insert(QStringLiteral("XMB_STANDIN_SECONDS"), QString::number(seconds));
    r.process = std::make_unique<QProcess>();
    r.process->setProcessEnvironment(env);
    r.process->setProgram(exe);
    // The exact command line shape the shell builds (Inno parses /DIR="..." itself; Qt's argument quoting would mangle it).
    r.process->setNativeArguments(QStringLiteral("/DIR=\"%1\" /LANG=en /NORESTART /SP- /LOG=\"%2\"")
                                      .arg(QDir::toNativeSeparators(r.dir), QDir::toNativeSeparators(r.log)));
    r.process->start();
    CHECK(r.process->waitForStarted(5000));
    return r;
}

static void liveTests(const QString& exe, const QString& scratch) {
    // 1. A normal run to the end: log steps and percentage were seen, the folder grew, exit 0 is a success.
    {
        Run r = launch(exe, scratch, QStringLiteral("ok"), QStringLiteral("success"), 3);
        CHECK(waitFor(5000, [&] { r.poll(); return r.tracker.state().step == QLatin1String("Copying files"); }));
        CHECK(waitFor(5000, [&] { return directoryBytes(r.dir, 1000, nullptr) > 0; }));
        const qint64 early = directoryBytes(r.dir, 1000, nullptr);
        CHECK(waitFor(8000, [&] { return r.process->state() == QProcess::NotRunning; }));
        r.poll();
        CHECK(r.process->exitCode() == 0);
        const auto s = r.tracker.state();
        CHECK(s.succeeded && s.closed && s.percent == 100 && s.fileEntries >= 2 && !s.item.isEmpty());
        CHECK(directoryBytes(r.dir, 1000, nullptr) > early);              // the destination grew while it ran
        ExitInputs in;
        in.exitCode = r.process->exitCode();
        in.destinationHasFiles = directoryHasFiles(r.dir);
        in.logAvailable = true;
        in.log = s;
        CHECK(classifyExit(in) == Outcome::Succeeded);
    }
    // 2. A failure: non-zero exit, the installer's own message, never a success.
    {
        Run r = launch(exe, scratch, QStringLiteral("fail"), QStringLiteral("fail"), 4);
        CHECK(waitFor(10000, [&] { return r.process->state() == QProcess::NotRunning; }));
        r.poll();
        CHECK(r.process->exitCode() == 2);
        CHECK(r.tracker.state().failed && r.tracker.state().error == QLatin1String("Synthetic failure (fixture)"));
        ExitInputs in;
        in.exitCode = 2;
        in.destinationHasFiles = directoryHasFiles(r.dir);
        in.logAvailable = true;
        in.log = r.tracker.state();
        CHECK(classifyExit(in) == Outcome::Failed);
    }
    // 3. Exit 0 with nothing written: not a success.
    {
        Run r = launch(exe, scratch, QStringLiteral("nofiles"), QStringLiteral("nofiles"), 3);
        CHECK(waitFor(8000, [&] { return r.process->state() == QProcess::NotRunning; }));
        r.poll();
        CHECK(r.process->exitCode() == 0 && !directoryHasFiles(r.dir) && !r.tracker.state().succeeded);
        ExitInputs in;
        in.destinationHasFiles = directoryHasFiles(r.dir);
        in.logAvailable = true;
        in.log = r.tracker.state();
        CHECK(classifyExit(in) == Outcome::ExitedUnverified);
    }
    // 4. A cooperative installer: the polite close (WM_CLOSE to the tree's windows) ends it; nothing is deleted.
    {
        Run r = launch(exe, scratch, QStringLiteral("polite"), QStringLiteral("success"), 30);
        CHECK(waitFor(5000, [&] { r.poll(); return r.tracker.state().step == QLatin1String("Copying files") && !r.tree().isEmpty(); }));
        const QString someFile = QDir(r.dir).filePath(QStringLiteral("data/pack_0.bin"));
        CHECK(waitFor(5000, [&] { return QFileInfo::exists(someFile); }));
        CloseEscalation esc(5000);
        QElapsedTimer clock;
        clock.start();
        CHECK(esc.requestClose(clock.elapsed()) == CloseEscalation::Action::SendClose);
        CHECK(xmb::steamquit::postCloseToProcesses(r.tree(), false) >= 1);
        CHECK(waitFor(5000, [&] { return r.process->state() == QProcess::NotRunning; }));
        CHECK(esc.tick(clock.elapsed(), false) == CloseEscalation::Action::None && esc.stage() == CloseEscalation::Stage::Done);
        CHECK(!esc.forced());
        r.poll();
        CHECK(r.process->exitCode() == 5 && r.tracker.state().rollingBack);
        CHECK(QFileInfo::exists(someFile));                               // the half-finished destination is left alone
        ExitInputs in;
        in.exitCode = 5;
        in.closeRequested = true;
        in.destinationHasFiles = directoryHasFiles(r.dir);
        in.logAvailable = true;
        in.log = r.tracker.state();
        CHECK(classifyExit(in) == Outcome::Cancelled);
    }
    // 5. A stubborn installer with a helper child: polite does nothing, the escalation asks, a confirmation terminates
    //    the whole tree, and nothing in the destination is removed.
    {
        Run r = launch(exe, scratch, QStringLiteral("stubborn"), QStringLiteral("stubborn"), 60);
        CHECK(waitFor(5000, [&] { r.poll(); return r.tree().size() >= 2; }));       // the installer and its helper
        const auto before = r.tree();
        CloseEscalation esc(1200, 4000);
        QElapsedTimer clock;
        clock.start();
        CHECK(esc.requestClose(clock.elapsed()) == CloseEscalation::Action::SendClose);
        xmb::steamquit::postCloseToProcesses(r.tree(), false);
        QThread::msleep(500);
        CHECK(r.process->state() == QProcess::Running);                         // it ignored WM_CLOSE
        CloseEscalation::Action action = CloseEscalation::Action::None;
        CHECK(waitFor(4000, [&] { action = esc.tick(clock.elapsed(), r.process->state() == QProcess::Running); return action == CloseEscalation::Action::AskForce; }));
        CHECK(r.process->state() == QProcess::Running);                         // asked; nothing terminated yet
        CHECK(esc.confirmForce(clock.elapsed()) == CloseEscalation::Action::Terminate);
        CHECK(xmb::steamquit::terminateProcesses(r.tree()) >= 2);
        CHECK(waitFor(5000, [&] { return r.process->state() == QProcess::NotRunning; }));
        CHECK(waitFor(5000, [&] { esc.tick(clock.elapsed(), r.process->state() == QProcess::Running); return esc.stage() == CloseEscalation::Stage::Done; }));
        CHECK(esc.forced());
        CHECK(waitFor(5000, [&] {                                                // the helper child went with the tree
            const auto snap = xmb::steamquit::snapshotProcesses();
            for (const auto& b : before) for (const auto& p : snap) if (p.pid == b.pid && p.startTime == b.startTime) return false;
            return true;
        }));
        CHECK(QFileInfo::exists(r.dir));                                          // the destination was never deleted
        ExitInputs in;
        in.exitCode = 1;
        in.closeRequested = true;
        in.forced = true;
        in.destinationHasFiles = directoryHasFiles(r.dir);
        CHECK(classifyExit(in) == Outcome::Cancelled);
    }
}
#endif

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    logTests();
    statusTests();
    exitTests();
    escalationTests();
    treeTests();
    folderTests();
    std::printf("pure tests: %s\n", g_failed ? "FAILED" : "ok");
#ifdef Q_OS_WIN
    if (argc >= 3) {
        const QString scratch = QDir::cleanPath(QString::fromLocal8Bit(argv[2]));
        QDir(scratch).removeRecursively();
        QDir().mkpath(scratch);
        liveTests(QString::fromLocal8Bit(argv[1]), scratch);
        QDir(scratch).removeRecursively();
    } else {
        std::printf("live tests skipped (no stand-in exe argument)\n");
    }
#endif
    std::printf("%s (%d failure(s))\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
