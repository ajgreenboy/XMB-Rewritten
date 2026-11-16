#pragma once
#include <QHash>
#include <QObject>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>

// Steam background mode (audit/STEAM-HIDDEN.md): the XMB is a living-room front end, so the full Steam client runs
// hidden behind it (games keep Steamworks, cloud saves, Steam Input and friends data) and the XMB starts it, waits
// for it, and keeps its stray windows off the TV. Nothing here touches Steam's files; the registry is only read.
namespace xmb::steamhide {

// ---- Switch set --------------------------------------------------------------------------------------------
// The exact arguments the hidden start passes to steam.exe: "-silent" (tray start, no main window) plus
// "-nofriendsui" and "-nochatui" (no friends or chat window on start). Empty when background mode is off, so the
// start is exactly what it always was. Never -no-browser (deprecated, and it removes the overlay browser),
// -noverifyfiles, -cef-disable-gpu, -tenfoot or -login; see the audit for why.
QStringList startArguments(bool hidden);

// XMB_STEAM_HIDE=0 is the kill switch: background mode is off whatever the setting says.
bool killSwitchEngaged();
bool effectiveEnabled(bool preference);

// ---- What Steam says about itself ---------------------------------------------------------------------------
// HKCU\Software\Valve\Steam\ActiveProcess: "pid" is the running client's process id and "ActiveUser" the account
// id it is signed in as (0 while its sign-in window is up). `registryKey` is steamRegistryKeyPath() (the
// XMB_STEAM_REGISTRY_KEY test seam, accepted only under HKEY_CURRENT_USER\Software\XmbTest\).
struct ClientState {
    quint32 pid = 0;            // ActiveProcess\pid, 0 when absent
    bool alive = false;         // that pid is a live process whose image is steam.exe (a stale value is not "running")
    qulonglong activeUser = 0;  // ActiveProcess\ActiveUser, 0 = nobody signed in
    bool signedIn() const { return alive && activeUser != 0; }
};
ClientState readClientState(const QString& registryKey);

// ---- Readiness state machine (pure) ---------------------------------------------------------------------------
enum class Phase { Starting, Ready, SignInNeeded, Failed };
struct Timing {
    int signInAfterMs = 20000;   // client running but nobody signed in after this long: it is waiting for the user
    int timeoutMs = 90000;       // no client at all after this long: the start failed
    int signInGiveUpMs = 600000; // stop waiting for a sign-in after this long
};
struct Verdict {
    Phase phase = Phase::Starting;
    QString text;                // the honest on-screen sentence for this state
};
Verdict evaluate(const ClientState& state, qint64 elapsedMs, const Timing& timing);

// ---- Starting the client -------------------------------------------------------------------------------------
bool processIsElevated();        // true when the XMB runs with an elevated token (Steam would inherit it)

struct StartOptions {
    QString steamExe;
    bool hidden = true;          // pass startArguments(true)
    QString registryKey;         // steamRegistryKeyPath()
    Timing timing;
    int pollMs = 500;
    int elevatedOverride = -1;   // tests: 0/1 replaces processIsElevated()
};

// Starts the client when none is registered and waits for it. Every state becomes a status() line; the result is
// exactly one of ready() / failed(). needsSignIn() fires (once) when the client is up but nobody is signed in, and
// the wait goes on until the user signs in or signInGiveUpMs passes.
class ClientStarter : public QObject {
    Q_OBJECT
public:
    explicit ClientStarter(QObject* parent = nullptr);
    void begin(const StartOptions& options);
    bool running() const { return running_; }
    void cancel();
signals:
    void status(const QString& text);
    void needsSignIn(const QString& text);
    void ready();
    void failed(const QString& text);
private:
    void poll();
    void finishReady();
    void finishFailed(const QString& text);
    StartOptions options_;
    QTimer timer_;
    QElapsedTimer clock_;
    bool running_ = false;
    bool signInAnnounced_ = false;
    QString lastStatus_;
};

// ---- Stray Steam windows -------------------------------------------------------------------------------------
struct WindowFacts {
    QString title;
    QString className;
    QString processName;         // image base name of the owner, e.g. steam.exe
    bool visible = true;
    bool owned = false;          // has an owner window (a child dialog)
    bool toolWindow = false;     // WS_EX_TOOLWINDOW
    bool clientProcess = false;  // belongs to the Steam client (steam.exe, or steamwebhelper.exe under it)
    bool signedIn = false;       // ActiveUser != 0
};
enum class WindowVerdict {
    Ignore,        // not ours to touch
    Hide,          // a Steam window the XMB hides
    KeepSignIn,    // the user must use it: bring it forward
    KeepMessage    // a Steam error or warning the user must read: bring it forward
};
WindowVerdict classifyWindow(const WindowFacts& facts);
bool titleLooksLikeSignIn(const QString& title);   // sign-in, Steam Guard, verification windows
bool titleLooksLikeMessage(const QString& title);  // error, warning, failed windows

struct SweepResult {
    int hidden = 0;
    QStringList hiddenTitles;
    quintptr keepWindow = 0;       // a window to bring forward (sign-in first, then message), 0 = none
    bool keepIsSignIn = false;
    QString keepTitle;
};
// One pass over the top-level windows. `clientProcs` maps pid -> image name for the Steam client's processes only
// (clientProcesses() in production, a stand-in in tests); every other window is ignored. `apply` false only
// classifies.
SweepResult sweepWindows(const QHash<quint32, QString>& clientProcs, bool signedIn, bool apply = true);
// The client's processes: the registered ActiveProcess pid and the steam*.exe processes under it.
QHash<quint32, QString> clientProcesses(const QString& registryKey);
// Brings one window to the front (restores it when minimised). Windows only.
bool raiseWindow(quintptr hwnd);

}  // namespace xmb::steamhide
