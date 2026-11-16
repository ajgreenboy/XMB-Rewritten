#pragma once
// Valve's SteamCMD as the XMB's installer for Steam games (audit/STEAMCMD.md).
//
// Layers, from the bottom:
//   pure helpers   account/app id rules, prompt matching on partial output, progress parsing, the command lines,
//                  locating steamcmd.exe, the zip check, folder choice and manifest integration;
//   Secret         a byte buffer that is zeroed on destruction and on wipe();
//   Run            one steamcmd child process driven through QProcess: the sign-in exchange or an install/update
//                  job, a state machine over what the child prints.
//
// Credential rules (non-negotiable, tests/steamcmd_tests.cpp scans for every one of them):
//   * a password or Steam Guard code is written ONLY to the child's stdin, once, and is zeroed straight after;
//   * it is never in a command line, an environment variable, a file, a log line, the settings or a crash dump
//     (nothing here ever puts it in a QString that outlives the call);
//   * every line the child prints is scrubbed of any held secret before it leaves this class;
//   * log lines name states only ("prompt: password", "sent").
#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <functional>

namespace xmb::steamcmd {

// Valve's official download of the Windows SteamCMD (https://developer.valvesoftware.com/wiki/SteamCMD names it).
// Valve publishes no checksum or signature for it, so the app can only check that the file is a zip that holds
// steamcmd.exe; the confirmation page says so.
inline const char* const kDownloadUrl = "https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip";
constexpr qint64 kMaxZipBytes = 8 * 1024 * 1024;

// ---- secrets ------------------------------------------------------------------------------------------------
class Secret {
public:
    Secret() = default;
    explicit Secret(const QString& text);        // UTF-8, copied once
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;
    ~Secret() { wipe(); }
    bool isEmpty() const { return bytes_.isEmpty(); }
    qsizetype size() const { return bytes_.size(); }
    void wipe();
    const QByteArray& bytes() const { return bytes_; }
private:
    QByteArray bytes_;
};

// ---- rules for what may reach a command line ----------------------------------------------------------------
// A Steam account name: letters, digits and _ - . only, 3..64 characters. Anything else (a leading '+', a space, a
// quote) could change what SteamCMD runs, so it is refused instead of escaped.
bool validAccountName(const QString& name);
bool validAppId(const QString& appId);           // digits only, 1..10
QString safeFolderName(const QString& title, const QString& appId);   // one path component, never empty

// ---- prompts and lines from the child -----------------------------------------------------------------------
enum class Prompt { None, Password, GuardEmail, GuardApp };
enum class Line {
    Other, LoginOk, MobileConfirm, MobileWaiting, MobileTimedOut, GuardWanted,
    BadPassword, BadGuardCode, RateLimited, LoginFailed, NoCachedLogin,
    JobOk, JobFailed, NoSubscription,
};
struct Progress {
    bool valid = false;
    int stateCode = 0;           // the 0x.. in "Update state (0x61)"
    QString label;               // downloading, validating, committing...
    double percent = 0;
    qint64 done = 0, total = 0;
};

// Splits arriving bytes into complete lines (terminated by LF, CR or CRLF; SteamCMD redraws its progress with
// bare CRs) and keeps the unterminated tail, which is where prompts live ("password: " has no newline).
class OutputScanner {
public:
    // Appends bytes. Terminal escape sequences are removed and NUL bytes dropped.
    void feed(const QByteArray& bytes);
    QStringList takeLines();          // completed lines since the last call, empty lines dropped
    QString tail() const;
    void clearTail() { tailBytes_.clear(); }
    static Prompt classifyTail(const QString& tail);
    static Line classifyLine(const QString& line);
    static Progress parseProgress(const QString& line);
    // The text after "FAILED (" / "ERROR (" or a known reason, for a failure line shown to the person.
    static QString failureText(const QString& line);
private:
    QByteArray tailBytes_;
    QStringList lines_;
    bool previousWasCr_ = false;
    int escape_ = 0;                  // 0 none, 1 saw ESC, 2 inside a CSI sequence
};

// ---- command lines ------------------------------------------------------------------------------------------
// Sign-in: +login <user> with no password, so SteamCMD asks for it on stdin. The password is never an argument.
QStringList signInArguments(const QString& username);
// A job (app_update): +force_install_dir must come before +login (Valve's wiki says so), +@NoPromptForPassword 1
// makes a missing cached login fail instead of asking, +@ShutdownOnFailedCommand 1 ends the run on a failure.
// `validate` adds the validate switch (always used for a first install and for Validate).
QStringList jobArguments(const QString& username, const QString& appId, const QString& forceInstallDir, bool validate);

// ---- finding steamcmd.exe -----------------------------------------------------------------------------------
// Where to look, in order: the configured path (file or folder), then <dataDir>/steamcmd, then the usual places.
// `roots` are extra folders (for tests); nothing under the Steam client's own folder is ever a candidate.
QStringList candidateExecutables(const QString& configured, const QString& dataDir, const QStringList& extraFolders = {});
QString findExecutable(const QString& configured, const QString& dataDir, const QStringList& extraFolders = {});
// A folder whose steamcmd.exe is the answer, or `path` itself when it is the exe.
QString resolveExecutable(const QString& pathOrFolder);
// True when the folder looks like the full Steam client (steam.exe present): SteamCMD's own files are never
// removed from there.
bool looksLikeSteamClientFolder(const QString& folder);

// ---- download verification ----------------------------------------------------------------------------------
// The bytes are a zip (local file header at the start, an end-of-central-directory record) whose central directory
// names steamcmd.exe at its top level. Says why not in `error`.
bool zipHoldsSteamcmd(const QByteArray& zip, QString* error = nullptr);

// ---- libraries and manifests --------------------------------------------------------------------------------
// The library roots in <steamRoot>/steamapps/libraryfolders.vdf plus the Steam root itself (deduplicated, only
// those that exist). Read-only.
QStringList steamLibraries(const QString& steamRoot);
// The install folder a job uses: <library>/steamapps/common/<folder>.
QString installFolderFor(const QString& library, const QString& folderName);
// After a job: SteamCMD wrote steamapps/appmanifest_<id>.acf under the forced install folder. If its installdir is
// exactly the folder's name (so the client's <library>/steamapps/common/<installdir> resolves to the same files) and
// the library has no manifest for the app yet, copy it to <library>/steamapps/. Never overwrites a manifest.
struct Integration {
    enum class Kind { Linked, AlreadyKnown, NoManifest, Mismatch, CopyFailed } kind = Kind::NoManifest;
    QString detail;
};
Integration integrateManifest(const QString& library, const QString& forcedFolder, const QString& appId);

// ---- one steamcmd process -----------------------------------------------------------------------------------
class Run final : public QObject {
    Q_OBJECT
public:
    enum class Mode { SignIn, Job };
    enum class State { Idle, Starting, WantPassword, WantGuard, WaitingMobile, Working, Done };
    enum class Outcome {
        None, SignedIn, JobDone, NeedsSignIn, BadPassword, BadGuardCode, RateLimited, MobileTimedOut,
        Failed, Cancelled, NotStarted, ExitedEarly,
    };
    explicit Run(QObject* parent = nullptr);
    ~Run() override;
    // Starts the child. Nothing about the account but its name is in `arguments`; the environment is the app's own
    // (no variable is added). Returns false when the process cannot start (Outcome::NotStarted follows at once).
    bool start(Mode mode, const QString& exe, const QStringList& arguments, const QString& workDir = {});
    // Writes one line to the child's stdin, then wipes the secret. Only valid for the prompt that was asked for.
    bool provide(Secret& secret);
    // Ends the child: terminate, a short wait, then kill. finished(Cancelled) follows.
    void cancel();
    State state() const { return state_; }
    Mode mode() const { return mode_; }
    Prompt pendingPrompt() const { return pending_; }
    Progress lastProgress() const { return progress_; }
    QString lastMessage() const { return message_; }     // the child's own words for the last failure
    bool running() const { return process_.state() != QProcess::NotRunning; }
    // The secrets the scrubber knows about right now (kept only until provide() wipes them); for tests.
    static QString scrub(const QString& line, const QByteArray& secret);
signals:
    void stateChanged(int state);
    void promptRequested(int prompt);         // a Prompt
    void progressChanged();
    void lineSeen(QString line);              // scrubbed; for the XMB's trace, which logs states not lines
    void finished(int outcome, QString message);
private:
    void onOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void handleLine(const QString& line);
    void checkTail();
    void setState(State state);
    void finish(Outcome outcome, const QString& message);
    QProcess process_;
    OutputScanner scanner_;
    State state_ = State::Idle;
    Mode mode_ = Mode::SignIn;
    Prompt pending_ = Prompt::None;
    Prompt lastAnswered_ = Prompt::None;
    Progress progress_;
    QString message_;
    QByteArray held_;                         // the one secret currently being sent, for scrubbing, wiped after
    QTimer killTimer_;
    bool cancelling_ = false, sawLoginOk_ = false, finishedOnce_ = false, sawMobile_ = false;
    Outcome lineOutcome_ = Outcome::None;
    QString lineMessage_;
};

}   // namespace xmb::steamcmd
