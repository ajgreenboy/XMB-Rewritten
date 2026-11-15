#include "steam_quit.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QMap>
#include <algorithm>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace xmb::steamquit {

bool isSteamClientProcess(const QString& name) {
    static const QSet<QString> names{
        QStringLiteral("steam.exe"), QStringLiteral("steamwebhelper.exe"), QStringLiteral("steamservice.exe"),
        QStringLiteral("steamerrorreporter.exe"), QStringLiteral("steamerrorreporter64.exe"),
        QStringLiteral("gameoverlayui.exe"), QStringLiteral("gameoverlayui64.exe"),
        QStringLiteral("steam_monitor.exe"), QStringLiteral("streaming_client.exe"), QStringLiteral("vrmonitor.exe"),
        QStringLiteral("steamsysinfo.exe"), QStringLiteral("steamtours.exe")};
    return names.contains(name.trimmed().toLower());
}

QString normalizePath(const QString& path) {
    QString p = path.trimmed();
    if (p.startsWith(QStringLiteral("\\\\?\\"))) p = p.mid(4);
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (p.size() > 1 && p.endsWith(QLatin1Char('/'))) p.chop(1);
    return p.toLower();
}

bool pathInside(const QString& path, const QString& dir) {
    const QString p = normalizePath(path);
    const QString d = normalizePath(dir);
    if (p.isEmpty() || d.isEmpty()) return false;
    return p.size() > d.size() + 1 && p.startsWith(d) && p.at(d.size()) == QLatin1Char('/');
}

QList<ProcessInfo> selectGameProcesses(const QList<ProcessInfo>& processes, const QString& installDir,
                                       const QSet<quint32>& protectedPids) {
    QList<ProcessInfo> chosen;
    if (normalizePath(installDir).isEmpty()) return chosen;
    QHash<quint32, const ProcessInfo*> byPid;
    for (const auto& p : processes) if (p.pid) byPid.insert(p.pid, &p);
    const auto barred = [&](const ProcessInfo& p) {
        return !p.pid || protectedPids.contains(p.pid) || isSteamClientProcess(p.name);
    };
    QSet<quint32> in;
    for (const auto& p : processes)
        if (!barred(p) && pathInside(p.imagePath, installDir)) in.insert(p.pid);
    // Descendants, to a fixed point. A barred process is never added, so nothing below it is either.
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& p : processes) {
            if (in.contains(p.pid) || barred(p) || !in.contains(p.parentPid)) continue;
            const auto parent = byPid.value(p.parentPid);
            if (parent && parent->startTime && p.startTime && p.startTime < parent->startTime) continue;
            in.insert(p.pid);
            grew = true;
        }
    }
    for (const auto& p : processes) if (in.contains(p.pid)) chosen.push_back(p);
    std::sort(chosen.begin(), chosen.end(), [](const ProcessInfo& a, const ProcessInfo& b) { return a.pid < b.pid; });
    return chosen;
}

QList<ProcessInfo> stillAlive(const QList<ProcessInfo>& known, const QList<ProcessInfo>& snapshot) {
    QHash<quint32, const ProcessInfo*> byPid;
    for (const auto& p : snapshot) byPid.insert(p.pid, &p);
    QList<ProcessInfo> alive;
    for (const auto& k : known) {
        const auto it = byPid.constFind(k.pid);
        if (it == byPid.constEnd()) continue;
        const ProcessInfo& now = **it;
        if (now.name.compare(k.name, Qt::CaseInsensitive) != 0) continue;
        if (!k.imagePath.isEmpty() && !now.imagePath.isEmpty() && normalizePath(k.imagePath) != normalizePath(now.imagePath)) continue;
        if (k.startTime && now.startTime && k.startTime != now.startTime) continue;
        alive.push_back(now);
    }
    return alive;
}

QList<ProcessInfo> rootsOf(const QList<ProcessInfo>& set) {
    QSet<quint32> pids;
    for (const auto& p : set) pids.insert(p.pid);
    QList<ProcessInfo> roots;
    for (const auto& p : set) if (!pids.contains(p.parentPid)) roots.push_back(p);
    return roots;
}

QString describeNames(const QList<ProcessInfo>& set) {
    QMap<QString, int> counts;
    for (const auto& p : set) ++counts[p.name.isEmpty() ? QStringLiteral("?") : p.name];
    QStringList parts;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) parts << QStringLiteral("%1 x%2").arg(it.key()).arg(it.value());
    return parts.join(QStringLiteral(", "));
}

#ifdef Q_OS_WIN
QList<ProcessInfo> snapshotProcesses() {
    QList<ProcessInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e)) {
        ProcessInfo p;
        p.pid = e.th32ProcessID;
        p.parentPid = e.th32ParentProcessID;
        p.name = QString::fromWCharArray(e.szExeFile);
        if (!p.pid) continue;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid)) {
            wchar_t buffer[32768];
            DWORD size = DWORD(std::size(buffer));
            if (QueryFullProcessImageNameW(h, 0, buffer, &size)) p.imagePath = QString::fromWCharArray(buffer, int(size));
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(h, &created, &exited, &kernel, &user))
                p.startTime = (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            CloseHandle(h);
        }
        out.push_back(std::move(p));
    }
    CloseHandle(snap);
    return out;
}

namespace {
struct CloseCtx { QSet<quint32> pids; bool systemCommand; int posted; };
}
int postCloseToProcesses(const QList<ProcessInfo>& set, bool systemCommand) {
    if (set.isEmpty()) return 0;
    CloseCtx ctx{{}, systemCommand, 0};
    for (const auto& p : set) ctx.pids.insert(p.pid);
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<CloseCtx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (!c->pids.contains(quint32(pid)) || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        const BOOL ok = c->systemCommand ? PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0) : PostMessageW(h, WM_CLOSE, 0, 0);
        if (ok) ++c->posted;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.posted;
}

int terminateProcesses(const QList<ProcessInfo>& set) {
    int done = 0;
    for (const auto& p : set) {
        if (!p.pid || p.pid == quint32(QCoreApplication::applicationPid())) continue;
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid)) {
            // The pid must still be the process that was selected, not a recycled one.
            FILETIME created{}, exited{}, kernel{}, user{};
            bool same = true;
            if (p.startTime && GetProcessTimes(h, &created, &exited, &kernel, &user))
                same = ((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime) == p.startTime;
            if (same && TerminateProcess(h, 1)) ++done;
            CloseHandle(h);
        }
    }
    return done;
}
#else
QList<ProcessInfo> snapshotProcesses() { return {}; }
int postCloseToProcesses(const QList<ProcessInfo>&, bool) { return 0; }
int terminateProcesses(const QList<ProcessInfo>&) { return 0; }
#endif

Quitter::Quitter(QObject* parent) : QObject(parent) {
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, &Quitter::tick);
}

Quitter::Start Quitter::start(const QString& installDir, int graceMs, int terminateMs, int killMs) {
    Start result;
    cancel();
    const QSet<quint32> protectedPids{quint32(QCoreApplication::applicationPid())};
    const auto set = selectGameProcesses(snapshotProcesses(), installDir, protectedPids);
    if (set.isEmpty()) return result;
    installDir_ = installDir;
    known_ = set;
    grace_ = std::max(0, graceMs);
    terminateWait_ = std::max(0, terminateMs);
    killWait_ = std::max(0, killMs);
    stage_ = 1;
    active_ = true;
    begin_ = QDateTime::currentMSecsSinceEpoch();
    result.processes = int(set.size());
    result.names = describeNames(set);
    result.windows = postCloseToProcesses(set, false);
    timer_.start();
    return result;
}

void Quitter::cancel() {
    timer_.stop();
    active_ = false;
    stage_ = 0;
    known_.clear();
}

void Quitter::finish(int stage, const QString& survivors) {
    timer_.stop();
    active_ = false;
    known_.clear();
    emit finished(stage, survivors);
}

void Quitter::tick() {
    if (!active_) { timer_.stop(); return; }
    const auto snapshot = snapshotProcesses();
    const QSet<quint32> protectedPids{quint32(QCoreApplication::applicationPid())};
    // The fresh selection finds new children; stillAlive keeps orphans whose parent already exited.
    QList<ProcessInfo> alive = stillAlive(known_, snapshot);
    for (const auto& p : selectGameProcesses(snapshot, installDir_, protectedPids)) {
        const bool have = std::any_of(alive.cbegin(), alive.cend(), [&](const ProcessInfo& a) { return a.pid == p.pid; });
        if (!have) alive.push_back(p);
    }
    if (alive.isEmpty()) { finish(stage_, QString()); return; }
    known_ = alive;
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - begin_;
    if (stage_ == 1 && elapsed >= grace_) {
        stage_ = 2;
        const auto roots = rootsOf(alive);
        // Terminate the root processes only; their children usually go with them, and stage 3 takes any that stay.
        const int ended = terminateProcesses(roots);
        qInfo().noquote() << QStringLiteral("ingame: quit stage 2 terminate (the title did not exit within the grace period): %1 of %2 root process(es) ended (%3), %4 process(es) were still running")
            .arg(ended).arg(roots.size()).arg(describeNames(roots)).arg(alive.size());
    } else if (stage_ == 2 && elapsed >= qint64(grace_) + terminateWait_) {
        stage_ = 3;
        const int ended = terminateProcesses(alive);
        qInfo().noquote() << QStringLiteral("ingame: quit stage 3 kill: %1 of %2 process(es) ended (%3)")
            .arg(ended).arg(alive.size()).arg(describeNames(alive));
    } else if (stage_ == 3) {
        if (elapsed >= qint64(grace_) + terminateWait_ + killWait_) { finish(0, describeNames(alive)); return; }
        terminateProcesses(alive);   // a process that was protected or starting a moment ago gets another try
    }
}

}
