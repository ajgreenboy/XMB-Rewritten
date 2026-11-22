#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace xmb {

struct SteamGame {
    QString appId;
    QString title;
    QString installDirectory;
    QString artwork;
    QString backgroundArtwork;
};

struct SteamInstallation {
    QString root;
    QString steamId;
    QVector<SteamGame> games;
    QStringList warnings;
};

// Reads Steam's local configuration and installed-game manifests. This function
// performs no network access and makes no changes to Steam files.
SteamInstallation scanSteam(const QString& explicitRoot = {});

// Which remembered account is current, from <steam>/config/loginusers.vdf alone (the registry's live ActiveUser,
// which scanSteam() consults first, is not involved). Rule, in order: MostRecent=1 (older clients; newest timestamp
// among several), the account whose AccountName equals `autoLoginUser` (HKCU\Software\Valve\Steam\AutoLoginUser,
// case-insensitive; pass empty to skip), the newest timestamp ("timestamp" or "Timestamp"; a tie goes to the higher
// SteamID64), and finally the only remembered account. AllowAutoLogin and AutoLogin are NOT consulted: current
// clients set AllowAutoLogin=1 on every remembered account, so they cannot tell the accounts apart. Returns the
// SteamID64, or empty when nothing decides. `rule` (optional) names the step that decided, for logs and tests.
QString steamSelectAccountFromLoginUsers(const QByteArray& loginUsers, const QString& autoLoginUser,
                                         QString* rule = nullptr);

// The SteamID64 the running client reports as signed in (HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser),
// accepted only when the registry's SteamPath is `steamRoot` (or the XMB_STEAM_REGISTRY_KEY test seam is set).
// Empty when nobody is signed in or the registry describes another installation. Read-only and cheap: this is
// what the shell polls to notice an account switch made inside the Steam client.
QString steamActiveAccountFromRegistry(const QString& steamRoot);

enum class SteamAppStatus {
    Unavailable, // Steam's observation could not be read or is not trustworthy.
    Stopped,     // Steam was readable and confirmed the title is not running.
    Running,
    Updating
};

// Live state of one title as the running Steam client reports it in the registry
// (HKCU\Software\Valve\Steam). Read-only; always unavailable off Windows.
struct SteamAppState {
    SteamAppStatus status = SteamAppStatus::Unavailable;
    bool running = false;
    bool updating = false;
};
SteamAppState steamAppState(const QString& appId);

// The Steam URI hand-off that asks the running client to launch a title (steam://rungameid/<id>).
// This is the only launch channel where the client's own state is not observable (Linux: no registry),
// so the caller hands the URI to the OS and makes no claim about whether the title started or exited.
// Empty for a non-numeric id.
QString steamLaunchUri(const QString& appId);

// How long a launch handoff keeps watching before it gives up without a confirmed run. The shell
// and the tracking tests share this literal so the reducer's timeout behavior is exercised at the
// exact production value.
inline constexpr qint64 kSteamLaunchWaitMs = 5 * 60 * 1000;

// Pure lifecycle reducer used by the shell's pollSteamGame and the deterministic tracking tests.
// The shell dispatches on the returned action, so the timeout/update/interruption behavior tested
// here is exactly what runs in production. Unknown observations never contribute to the stopped
// streak; two confirmed stopped observations are required after a confirmed run. This keeps
// registry outages/restarts from becoming false exits.
struct SteamTrackState {
    bool seenRunning = false;    // Steam reported the title running at least once
    int consecutiveStopped = 0;  // consecutive stopped observations since it was last seen running
};
enum class SteamTrackAction {
    KeepWatching,      // no visible change; keep polling
    RestartWait,       // restart the launch-wait clock (a confirmed update before the title ran)
    ConfirmedRunning,  // the title transitioned into running (first time, or after a clear)
    Cleared,           // first stopped observation after a run: drop the overlay, keep watching
    Returned,          // the title was confirmed stopped after a run
    TimedOut,          // the launch wait expired without a confirmed run
};
struct SteamTrackTransition {
    SteamTrackState state;
    SteamTrackAction action = SteamTrackAction::KeepWatching;
    SteamAppStatus status = SteamAppStatus::Unavailable; // the observation that drove this transition
};
SteamTrackTransition steamTrackObservation(SteamTrackState current, SteamAppStatus status,
                                           qint64 elapsedMs, qint64 timeoutMs);

// Ask the running Steam client to stop a title through its console command line. The client executes
// "+<command>" arguments handed to steam.exe, so this is the supported-by-observation way to quit a
// Steam game without killing processes (audit/STEAM-CLI.md). Returns false when the client cannot be
// started or the id is not a number; the caller still has to wait for Apps\<id>\Running to clear.
// The forwarding process exits immediately with no stdout and no exit status, so a true return only
// means the command line was handed over - use steamVerifyStop() for the client's own record of it.
bool steamStopApp(const QString& appId,const QString& steamRoot = {}, QString* error = nullptr);

// The exact argument shape of the stop handoff: {"+app_stop", "<numeric app id>"}. Kept in one place
// so the command line, the log match and the audit cannot drift apart. Empty for an invalid id.
QStringList steamStopArguments(const QString& appId);

// ---- The Steam console log: the only return channel for a "+command" handoff ------------------
// A second "steam.exe +<command> ..." forwards to the running client and exits: no stdout, no exit
// code. The client writes the command line it received, and the command's own output, into
// <steamRoot>/logs/console_log.txt, so a bounded read of the region after a snapshot offset is the
// only evidence that a command was delivered and accepted (audit/STEAM-CLI.md section 3).
struct SteamConsoleLogSlice {
    bool available = false; // the log exists and the region could be read
    bool truncated = false; // the file is shorter than the requested offset (rotation or truncation)
    qint64 offset = 0;      // where this read started
    qint64 size = 0;        // the file's size at the moment of the read
    QByteArray bytes;       // at most the requested maximum of the region after `offset`
    QString path;           // the log file that was read, empty when none was found
    QString error;          // why the read failed, empty on success
};

// Read at most `maximumBytes` (default 64 KiB, capped at 1 MiB) of the log starting at `offset`.
// `offset` -1 reads the tail of the file. Read-only and bounded: nothing under the Steam
// installation is ever written, and only <root>/logs/console_log.txt is opened.
SteamConsoleLogSlice steamConsoleLogTail(const QString& steamRoot, qint64 offset,
                                        qsizetype maximumBytes = 64 * 1024);

// The log's current size in bytes, or -1 when no log was found. Take this before a handoff and pass
// it to steamVerifyStop() so earlier commands in the same file cannot be mistaken for this one.
qint64 steamConsoleLogSize(const QString& steamRoot);

// What the client's own log says about one stop handoff issued after `offset`.
struct SteamStopEvidence {
    bool logAvailable = false;
    bool delivered = false; // the client logged our exact "+app_stop <id>" command line
    bool rejected = false;  // the client answered "command not found" for app_stop
    QString commandLine;    // the line that was matched, for the log
    QString context;        // the lines the client wrote immediately after (bounded; may include
                            // adjacent client spew, so `delivered` is the signal, not this text)
    QString error;          // why no verdict could be read (missing log, unreadable region)
};
SteamStopEvidence steamVerifyStop(const QString& appId, qint64 offset,
                                  const QString& steamRoot = {});

}
