#pragma once
#include "steam_quit.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>

// The Installing panel's pure logic (audit-free, no GUI): what a visible repack installer's own log says, what the
// panel may truthfully show, how a finished run is classified, and the polite-then-confirmed close sequence.
//
// Rule for every value here: it comes from a source that can be observed (the process state, the installer's /LOG
// file, the destination folder's size). A value that cannot be observed stays unknown (empty / -1) and is never
// shown; there is no guessed percentage and no success without a signal.
namespace xmb::installerpanel {

// ---- The installer's /LOG file ---------------------------------------------------------------------------------
// Inno Setup writes one line per event: "<date> <time>   <message>". The tracker keeps the latest step, the file it
// last named, a count of "File entry" records and the outcome lines. It is fed the file incrementally.
struct LogState {
    bool seen = false;            // at least one line was parsed
    bool installing = false;      // "Starting the installation process."
    bool succeeded = false;       // "Installation process succeeded."
    bool failed = false;          // an Exception line, "Installation process failed" or "Rolling back changes."
    bool rollingBack = false;
    bool closed = false;          // "Log closed."
    QString step;                 // e.g. "Copying files"; empty = no step line seen yet
    QString item;                 // base name of the last "Dest filename:" / extraction line; empty = none
    QString error;                // the installer's own "Exception message:" text; empty = none
    int percent = -1;             // 0..100 only when a log line carried one; -1 = unknown (never estimated)
    int fileEntries = 0;          // "-- File entry --" records seen
};

class InnoLogTracker {
public:
    // Appends raw bytes (UTF-8 with or without a BOM). A trailing partial line is held until its newline arrives.
    void feed(const QByteArray& chunk);
    // Parses one complete line (without its line break). Exposed for tests.
    void parseLine(const QString& line);
    const LogState& state() const { return state_; }
    void reset();
private:
    LogState state_;
    QByteArray carry_;
    bool first_ = true;
    bool expectDest_ = false;
};

// ---- Status ----------------------------------------------------------------------------------------------------
struct StatusInputs {
    bool running = false;
    qint64 elapsedMs = 0;
    qint64 bytesWritten = -1;          // destination folder size; -1 = not measurable (folder missing / unreadable)
    bool bytesLowerBound = false;      // the scan stopped early: "at least"
    bool logAvailable = false;         // the /LOG file exists and was read
    LogState log;
    bool closeRequested = false;
    bool awaitingForceConfirm = false;
    bool terminating = false;
};
struct StatusLine { QString label, value; };
struct StatusView {
    QString headline;                  // one line, always present
    QVector<StatusLine> lines;         // only known values, in display order
};
StatusView deriveStatus(const StatusInputs& in);

QString formatElapsed(qint64 ms);       // "0:07", "12:03", "1:02:03"
QString formatBytes(qint64 bytes);      // "0 B", "1.5 MB" (binary units)

// ---- After exit ------------------------------------------------------------------------------------------------
enum class Outcome {
    Succeeded,            // exit 0 AND the destination holds files AND the log does not report a failure
    ExitedUnverified,     // exit 0 but the destination is empty/missing, or the log reports a failure: not a success
    Failed,               // non-zero exit code
    Crashed,              // the process crashed
    Cancelled,            // the owner closed it (polite or forced) and it ended without a success signal
};
struct ExitInputs {
    int exitCode = 0;
    bool crashed = false;
    bool destinationHasFiles = false;
    bool closeRequested = false;
    bool forced = false;               // it had to be terminated
    LogState log;
    bool logAvailable = false;
};
Outcome classifyExit(const ExitInputs& in);
// The panel's result sentence. Never claims more than the inputs show.
QString outcomeText(Outcome outcome, const ExitInputs& in, const QString& destination, const QString& logPath,
                    qint64 bytesWritten);

// ---- Close escalation ------------------------------------------------------------------------------------------
// Polite first (WM_CLOSE to the installer's windows); if it is still alive after `graceMs`, ask the owner before
// anything is force-terminated; only a confirmation produces the Terminate action. A declined force-close goes back to
// idle (the installer keeps running, nothing is claimed).
class CloseEscalation {
public:
    enum class Stage { Idle, PoliteSent, AwaitingConfirm, Terminating, TerminateFailed, Done };
    enum class Action { None, SendClose, AskForce, Terminate };
    explicit CloseEscalation(qint64 graceMs = 5000, qint64 terminateWaitMs = 5000)
        : grace_(graceMs), terminateWait_(terminateWaitMs) {}
    Action requestClose(qint64 nowMs);
    Action tick(qint64 nowMs, bool alive);
    Action confirmForce(qint64 nowMs);
    void declineForce();
    void processExited();
    Stage stage() const { return stage_; }
    bool closeRequested() const { return requested_; }
    bool forced() const { return forced_; }
private:
    qint64 grace_, terminateWait_;
    Stage stage_ = Stage::Idle;
    qint64 stamp_ = 0;
    bool requested_ = false, forced_ = false;
};

// ---- Process tree and folder helpers ---------------------------------------------------------------------------
// The installer process (matched on pid and, when known, creation time) and every descendant that started no earlier
// than its parent, so a recycled pid never pulls in an unrelated process. Empty when the root is gone.
QList<steamquit::ProcessInfo> installerTree(const QList<steamquit::ProcessInfo>& snapshot, quint32 rootPid, quint64 rootStart);
// Total size in bytes of the regular files under `path`, not following links. -1 when the folder does not exist.
// Stops after `maxEntries` directory entries and sets *lowerBound.
qint64 directoryBytes(const QString& path, int maxEntries, bool* lowerBound);
// True when at least one regular file exists under `path`.
bool directoryHasFiles(const QString& path);

}
