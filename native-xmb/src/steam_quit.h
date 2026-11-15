#pragma once
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

// Quit Game for a Steam title (audit/STEAM-QUIT.md). "steam.exe +app_stop" does not stop a running game on this
// client build, so the XMB closes the game's own processes with the same three stages a non-Steam title gets:
// WM_CLOSE with a grace period, then terminate, then kill. Only processes that run from the game's install folder,
// and the children those start, are ever touched; the Steam client (steam.exe, steamwebhelper, Steam services, the
// overlay) never is.
namespace xmb::steamquit {

struct ProcessInfo {
    quint32 pid = 0;
    quint32 parentPid = 0;
    QString imagePath;        // full image path; empty when the process could not be opened
    QString name;             // base name, e.g. TheQuarry.exe
    quint64 startTime = 0;    // creation time (FILETIME ticks); 0 = unknown
};

// The Steam client's own processes by base name (case-insensitive). These are never selected, and neither is
// anything that only descends from one of them.
bool isSteamClientProcess(const QString& name);

// Folder identity used for the comparison: forward slashes, no trailing slash, lower-case, no \\?\ prefix.
QString normalizePath(const QString& path);
// True when `path` is strictly inside `dir` (a sibling folder that merely shares a prefix is not).
bool pathInside(const QString& path, const QString& dir);

// The processes to close or kill for a game installed in `installDir`: every process whose image lives inside
// the folder, plus every descendant of those (launcher and anti-cheat helper children), minus the Steam
// client's processes (and their subtrees), minus `protectedPids`. Sorted by pid. A child counts only when it
// started no earlier than its parent, so a recycled parent id cannot pull in an unrelated process.
QList<ProcessInfo> selectGameProcesses(const QList<ProcessInfo>& processes, const QString& installDir,
                                       const QSet<quint32>& protectedPids = {});
// Members of `known` that are still in `snapshot` (same pid, same name and image path): a child whose parent
// already exited is still part of the game, and a recycled pid is not.
QList<ProcessInfo> stillAlive(const QList<ProcessInfo>& known, const QList<ProcessInfo>& snapshot);
// The processes of `set` whose parent is not in `set`.
QList<ProcessInfo> rootsOf(const QList<ProcessInfo>& set);
// "TheQuarry.exe x1, Helper.exe x2": names and counts only, never a path or a command line.
QString describeNames(const QList<ProcessInfo>& set);

// Windows only (empty or zero elsewhere).
QList<ProcessInfo> snapshotProcesses();
int postCloseToProcesses(const QList<ProcessInfo>& set, bool systemCommand = false);   // windows that were sent WM_CLOSE (or SC_CLOSE)
int terminateProcesses(const QList<ProcessInfo>& set);                         // processes TerminateProcess accepted

// The three-stage driver the shell uses. It runs on the event loop, so the XMB keeps painting.
class Quitter : public QObject {
    Q_OBJECT
public:
    explicit Quitter(QObject* parent = nullptr);
    struct Start {
        int processes = 0;       // 0 = nothing was found; nothing was started
        int windows = 0;         // windows sent WM_CLOSE
        QString names;           // describeNames of the set
    };
    // Finds the game's processes and posts stage 1. `graceMs` is the wait before stage 2.
    Start start(const QString& installDir, int graceMs, int terminateMs = 3000, int killMs = 5000);
    bool active() const { return active_; }
    void cancel();
signals:
    // stage 1..3 = the stage after which the last process was gone; 0 = survivors remain after every stage.
    void finished(int stage, const QString& survivors);
private slots:
    void tick();
private:
    void finish(int stage, const QString& survivors);
    QString installDir_;
    QList<ProcessInfo> known_;
    QTimer timer_;
    qint64 begin_ = 0;
    int stage_ = 0, grace_ = 10000, terminateWait_ = 3000, killWait_ = 5000;
    bool active_ = false;
};

}
