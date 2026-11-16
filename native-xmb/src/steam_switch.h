#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>
#include <memory>

namespace xmb {

// The credential-free Steam account switch.
//
// What it does, and the only things it writes (audit/STEAM-ACCOUNT-SWITCH.md):
//   1. <steam>/config/loginusers.vdf : the target account gets MostRecent "1" and AllowAutoLogin "1",
//      every other account MostRecent "0". Every other key, the ordering and the file's formatting are
//      kept. The file is copied to loginusers.vdf.xmb-backup-<time> first and replaced atomically.
//   2. HKCU\Software\Valve\Steam : AutoLoginUser = the account's LOGIN NAME, RememberPassword = 1 (DWORD).
//   3. Steam is started again with no account argument; Steam itself signs in with the login it already
//      saved for that account.
// Only an account that already has a remembered login (RememberPassword 1 in loginusers.vdf) is ever
// selected. The XMB never reads or writes a password, token, cookie, ssfn* file, ConnectCache or the
// credentials in config.vdf, and never passes -login to Steam.

// ---- The pure rewrite of loginusers.vdf -----------------------------------------------------------
struct LoginUsersRewrite {
    bool ok = false;
    bool changed = false;       // `bytes` differs from the input
    QByteArray bytes;           // the rewritten file (the input when nothing changed)
    QString error;              // why ok is false; never contains file contents
    QString accountName;        // the target's AccountName (its login name) as the file spells it
    bool rememberPassword = false;  // the target already has RememberPassword 1
    bool wantsOfflineMode = false;  // the target has WantsOfflineMode 1 (left as it is)
    int usersSeen = 0;          // account blocks under "users"
};

// Selects `steamId64` as Steam's automatic account inside the text of loginusers.vdf:
//  - the target: MostRecent "1" and AllowAutoLogin "1" (set when present, inserted when missing);
//    WantsOfflineMode and SkipOfflineModeWarning are added as "0" only when missing and the file's other
//    accounts use them (an existing value, including WantsOfflineMode "1", is never changed);
//  - when the file uses the key "AutoLogin" (current Steam clients; they write no MostRecent), the target
//    gets AutoLogin "1" and every other account's truthy AutoLogin becomes "0";
//  - every other account: a truthy MostRecent is set to "0", never inserted;
//  - every other byte is kept: unknown keys, nested blocks, comments, quoting, indentation, the line ending
//    style (CRLF or LF) and the byte-order mark. Edits are splices into the original bytes.
// Refuses (ok=false, nothing changed) when the file does not parse, has no "users" block, has no account
// with that id (or two of them), the account has no AccountName, or `requireRemembered` is true and the
// account does not have RememberPassword 1. Running it on its own output changes nothing (changed=false).
LoginUsersRewrite rewriteLoginUsersForSwitch(const QByteArray& original, const QString& steamId64,
                                             bool requireRemembered = true);

// ---- Processes (the seam) -------------------------------------------------------------------------
class SteamProcessControl {
public:
    virtual ~SteamProcessControl() = default;
    // True while steam.exe or steamwebhelper.exe is running; `which` names what is.
    virtual bool steamRunning(QString* which) = 0;
    // Asks the client to exit (steam.exe -shutdown). Does not wait.
    virtual bool requestExit(const QString& steamExe, QString* error) = 0;
    // Starts the client detached with no arguments. Does not wait.
    virtual bool launch(const QString& steamExe, QString* error) = 0;
};
// The real one: a process snapshot plus QProcess::startDetached.
// `startArguments` are added to the relaunch command (Steam background mode passes -silent and friends so the
// restarted client stays in the tray, audit/STEAM-HIDDEN.md); empty keeps the start exactly as it was.
std::unique_ptr<SteamProcessControl> makeSystemSteamProcessControl(const QStringList& startArguments = {});
// XMB_STEAM_SWITCH_NOPROC=1: Steam is never running, nothing is started or stopped, and the launch is only
// logged. A scripted-run seam like XMB_POWER_DRY_RUN; it lets a run exercise the file and registry writes
// against a scratch Steam root without touching any real Steam process.
std::unique_ptr<SteamProcessControl> makeNullSteamProcessControl();

// ---- The switch itself ----------------------------------------------------------------------------
struct SteamSwitchOptions {
    QString steamRoot;
    QString steamExe;
    QString steamId64;
    QString accountName;       // the login name expected in the file (checked against it)
    QString personaName;       // for the messages
    QString registryKey;       // empty: steamRegistryKeyPath()
    bool select = true;        // false: the account has no saved sign-in; close and start Steam, change nothing
    bool hold = false;         // XMB_STEAM_SWITCH_HOLD: log the plan and change nothing
    int exitTimeoutMs = 45000; // how long to wait for steam.exe and steamwebhelper.exe to be gone
    int settleMs = 1500;       // after they are gone, before the files are touched
    int verifyTimeoutMs = 60000;
    int pollMs = 500;
    int keptBackups = 5;
};

struct SteamSwitchResult {
    enum class Outcome {
        SignedIn,     // Steam's ActiveUser names the account
        LoginWindow,  // Steam is running but nobody signed in (its login window, Steam Guard, expired login)
        OtherAccount, // Steam signed in as another account
        NotRunning,   // Steam was started but is not running at the end of the wait
        Started,      // select=false: Steam was restarted, nothing was changed, the user signs in inside Steam
        Held,         // hold mode: nothing was changed
        Failed        // nothing (or, after a late failure, nothing net) was changed; see message
    };
    Outcome outcome = Outcome::Failed;
    bool ok = false;           // the switch was carried out and Steam was started (not the same as signed in)
    bool changed = false;      // the new selection is in place when the result arrives
    bool restored = false;     // a failure after the first write put the file and the registry back
    QString message;           // the user-facing text, plain words
    QString backupPath;        // the loginusers.vdf copy that was made (also when it was restored from)
    QString signedInAs;        // persona name Steam reported, when known
};

class SteamAccountSwitcher : public QObject {
    Q_OBJECT
public:
    SteamAccountSwitcher(SteamSwitchOptions options, std::unique_ptr<SteamProcessControl> process,
                         QObject* parent = nullptr);
    ~SteamAccountSwitcher() override;
    // Starts the asynchronous run. Everything after this call is driven by timers on the owner's thread;
    // nothing blocks. Call once.
    void start();
    bool running() const { return stage_ != Stage::Idle && stage_ != Stage::Done; }

signals:
    // One short line for the status page; the same text is logged.
    void progress(const QString& text);
    void finished(const xmb::SteamSwitchResult& result);

private:
    enum class Stage { Idle, Closing, Settling, Verifying, Done };
    void finish(SteamSwitchResult::Outcome outcome, bool ok, const QString& message);
    void tick();
    void pollClosing();
    void writeSelection();
    void pollVerify();
    void rollback(const QString& why);
    void pruneBackups();
    QString loginUsersPath() const;
    QString registryKey() const;

    SteamSwitchOptions options_;
    std::unique_ptr<SteamProcessControl> process_;
    Stage stage_ = Stage::Idle;
    QTimer timer_;
    QElapsedTimer clock_;
    qint64 phaseStart_ = 0;
    SteamSwitchResult result_;
    QByteArray originalBytes_;
    bool fileWritten_ = false;
    bool registryWritten_ = false;
    bool hadAutoLogin_ = false;
    bool hadRemember_ = false;
    QString oldAutoLogin_;
    int oldRemember_ = 0;
};

// Steam's own record of who is signed in: HKCU\...\Steam\ActiveProcess ActiveUser as an account id, 0 when
// the client reports nobody (login window, or not running).
qulonglong steamActiveUserId(const QString& registryKey);

} // namespace xmb

Q_DECLARE_METATYPE(xmb::SteamSwitchResult)
