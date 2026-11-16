#include "steam_hidden.h"

#include "steam_quit.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QTimer>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xmb::steamhide {

QStringList startArguments(bool hidden) {
    if (!hidden) return {};
    return {QStringLiteral("-silent"), QStringLiteral("-nofriendsui"), QStringLiteral("-nochatui")};
}

bool killSwitchEngaged() {
    return qEnvironmentVariable("XMB_STEAM_HIDE").trimmed() == QLatin1String("0");
}
bool effectiveEnabled(bool preference) { return preference && !killSwitchEngaged(); }

// ---- Registry and process state ---------------------------------------------------------------------------
namespace {
#ifdef Q_OS_WIN
QString imageBaseName(quint32 pid) {
    QString name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buffer[32768];
        DWORD size = DWORD(std::size(buffer));
        if (QueryFullProcessImageNameW(h, 0, buffer, &size))
            name = QFileInfo(QString::fromWCharArray(buffer, int(size))).fileName();
        CloseHandle(h);
    }
    return name;
}
#endif
}  // namespace

ClientState readClientState(const QString& registryKey) {
    ClientState state;
#ifdef Q_OS_WIN
    const QSettings active(registryKey + QStringLiteral("\\ActiveProcess"), QSettings::NativeFormat);
    if (active.status() != QSettings::NoError) return state;
    bool valid = false;
    const qulonglong pid = active.value(QStringLiteral("pid")).toULongLong(&valid);
    if (valid && pid > 0 && pid <= 0xffffffffULL) {
        state.pid = quint32(pid);
        // A pid left behind by a client that exited (or recycled by another program) is not a running client.
        state.alive = imageBaseName(state.pid).compare(QStringLiteral("steam.exe"), Qt::CaseInsensitive) == 0;
    }
    valid = false;
    const qulonglong user = active.value(QStringLiteral("ActiveUser")).toULongLong(&valid);
    if (valid && user <= 0xffffffffULL) state.activeUser = user;
#else
    Q_UNUSED(registryKey);
#endif
    return state;
}

Verdict evaluate(const ClientState& state, qint64 elapsedMs, const Timing& timing) {
    Verdict v;
    if (state.alive && state.activeUser != 0) {
        v.phase = Phase::Ready;
        v.text = QObject::tr("Steam is running.");
    } else if (state.alive) {
        if (elapsedMs >= timing.signInGiveUpMs) {
            v.phase = Phase::Failed;
            v.text = QObject::tr("Steam is running but nobody signed in. Sign in to Steam, then try again.");
        } else if (elapsedMs >= timing.signInAfterMs) {
            v.phase = Phase::SignInNeeded;
            v.text = QObject::tr("Steam needs you to sign in. Its sign-in window is on screen; the game starts once you are signed in.");
        } else {
            v.phase = Phase::Starting;
            v.text = QObject::tr("Starting Steam...");
        }
    } else if (elapsedMs >= timing.timeoutMs) {
        v.phase = Phase::Failed;
        v.text = QObject::tr("Steam did not start: no Steam client registered itself within %1 seconds.").arg(timing.timeoutMs / 1000);
    } else {
        v.phase = Phase::Starting;
        v.text = QObject::tr("Starting Steam...");
    }
    return v;
}

bool processIsElevated() {
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
#else
    return false;
#endif
}

// ---- ClientStarter ------------------------------------------------------------------------------------------
ClientStarter::ClientStarter(QObject* parent) : QObject(parent) {
    connect(&timer_, &QTimer::timeout, this, &ClientStarter::poll);
}

void ClientStarter::cancel() {
    timer_.stop();
    running_ = false;
}

void ClientStarter::begin(const StartOptions& options) {
    cancel();
    options_ = options;
    signInAnnounced_ = false;
    lastStatus_.clear();
    running_ = true;
    clock_.start();
    const ClientState now = readClientState(options_.registryKey);
    if (now.alive) {
        qInfo().noquote() << QStringLiteral("steam hide: client already registered (pid %1, signed in %2); not starting another")
                                 .arg(now.pid).arg(now.activeUser ? 1 : 0);
        timer_.start(std::max(50, options_.pollMs));
        QTimer::singleShot(0, this, &ClientStarter::poll);
        return;
    }
    const bool elevated = options_.elevatedOverride >= 0 ? options_.elevatedOverride != 0 : processIsElevated();
    if (elevated) {
        finishFailed(QObject::tr("Steam was not started: the XMB is running as administrator, and Steam would inherit that. Start the XMB without administrator rights."));
        return;
    }
    if (!QFileInfo(options_.steamExe).isFile()) {
        finishFailed(QObject::tr("Steam was not started: steam.exe was not found."));
        return;
    }
    const QStringList args = startArguments(options_.hidden);
    emit status(QObject::tr("Starting Steam..."));
    bool started = false;
#ifdef Q_OS_WIN
    {
        QProcess process;
        process.setProgram(options_.steamExe);
        process.setArguments(args);
        process.setWorkingDirectory(QFileInfo(options_.steamExe).absolutePath());
        // Steam is a GUI program, so it never gets a console; CREATE_NO_WINDOW is belt and braces. The start
        // hint (minimised, not activated) is only a request: -silent is what keeps the main window away.
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
            a->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            a->startupInfo->wShowWindow = SW_SHOWMINNOACTIVE;
        });
        qint64 pid = 0;
        started = process.startDetached(&pid);
    }
#else
    started = QProcess::startDetached(options_.steamExe, args, QFileInfo(options_.steamExe).absolutePath());
#endif
    if (!started) {
        finishFailed(QObject::tr("Steam was not started: steam.exe could not be launched."));
        return;
    }
    qInfo().noquote() << QStringLiteral("steam hide: started steam.exe with [%1]; waiting for ActiveProcess\\pid").arg(args.join(QLatin1Char(' ')));
    timer_.start(std::max(50, options_.pollMs));
}

void ClientStarter::finishReady() {
    timer_.stop();
    running_ = false;
    qInfo().noquote() << QStringLiteral("steam hide: client ready after %1 ms").arg(clock_.elapsed());
    emit ready();
}
void ClientStarter::finishFailed(const QString& text) {
    timer_.stop();
    running_ = false;
    qWarning().noquote() << "steam hide: failed:" << text;
    emit failed(text);
}

void ClientStarter::poll() {
    if (!running_) return;
    const Verdict verdict = evaluate(readClientState(options_.registryKey), clock_.elapsed(), options_.timing);
    switch (verdict.phase) {
    case Phase::Ready: finishReady(); return;
    case Phase::Failed: finishFailed(verdict.text); return;
    case Phase::SignInNeeded:
        if (!signInAnnounced_) {
            signInAnnounced_ = true;
            qInfo().noquote() << "steam hide: client is up but nobody is signed in; waiting for the user";
            emit needsSignIn(verdict.text);
        }
        break;
    case Phase::Starting:
        if (verdict.text != lastStatus_) { lastStatus_ = verdict.text; emit status(verdict.text); }
        break;
    }
}

// ---- Window rules ---------------------------------------------------------------------------------------------
bool titleLooksLikeSignIn(const QString& title) {
    const QString t = title.toLower();
    for (const char* word : {"sign in", "sign-in", "signin", "log in", "login", "logon", "steam guard", "verification",
                             "verify", "security code", "two-factor", "authenticat", "password"})
        if (t.contains(QLatin1String(word))) return true;
    return false;
}
bool titleLooksLikeMessage(const QString& title) {
    const QString t = title.toLower();
    for (const char* word : {"error", "warning", "failed", "unable", "cannot", "can't", "problem", "required", "not enough",
                             "disk space", "corrupt", "locked", "must be"})
        if (t.contains(QLatin1String(word))) return true;
    return false;
}

WindowVerdict classifyWindow(const WindowFacts& f) {
    // 1. Only windows of the Steam client itself, never the game, the XMB, or Steam's in-game overlay.
    if (!f.clientProcess) return WindowVerdict::Ignore;
    const QString proc = f.processName.toLower();
    if (proc != QLatin1String("steam.exe") && proc != QLatin1String("steamwebhelper.exe")) return WindowVerdict::Ignore;
    // 2. Windows that are not on screen, are owned dialogs (they follow their owner) or tool windows are left alone.
    if (!f.visible || f.owned || f.toolWindow) return WindowVerdict::Ignore;
    // 3. A window without a title is Chromium plumbing or the overlay's surface: never touched.
    if (f.title.trimmed().isEmpty()) return WindowVerdict::Ignore;
    // 4. Anything the user must act on or read is kept and brought forward.
    if (titleLooksLikeSignIn(f.title)) return WindowVerdict::KeepSignIn;
    // 5. Signed out: the client is showing its login window and the title could be in any language, so nothing is hidden.
    if (!f.signedIn) return WindowVerdict::KeepSignIn;
    if (titleLooksLikeMessage(f.title)) return WindowVerdict::KeepMessage;
    // 6. Everything else a signed-in client puts up (main window, Big Picture, News, update and friends windows).
    return WindowVerdict::Hide;
}

#ifdef Q_OS_WIN
namespace {
struct SweepCtx {
    const QHash<quint32, QString>* procs;
    bool signedIn;
    bool apply;
    SweepResult result;
};
}
SweepResult sweepWindows(const QHash<quint32, QString>& clientProcs, bool signedIn, bool apply) {
    SweepCtx ctx{&clientProcs, signedIn, apply, {}};
    if (clientProcs.isEmpty()) return ctx.result;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<SweepCtx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        const auto it = c->procs->constFind(quint32(pid));
        WindowFacts f;
        f.clientProcess = it != c->procs->constEnd();
        if (!f.clientProcess) return TRUE;
        f.processName = it.value();
        wchar_t title[256]{};
        GetWindowTextW(h, title, 255);
        f.title = QString::fromWCharArray(title);
        wchar_t cls[128]{};
        GetClassNameW(h, cls, 127);
        f.className = QString::fromWCharArray(cls);
        f.visible = IsWindowVisible(h) != FALSE;
        f.owned = GetWindow(h, GW_OWNER) != nullptr;
        f.toolWindow = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
        f.signedIn = c->signedIn;
        switch (classifyWindow(f)) {
        case WindowVerdict::Hide:
            if (c->apply) ShowWindow(h, SW_HIDE);
            ++c->result.hidden;
            c->result.hiddenTitles << QStringLiteral("%1 [%2]").arg(f.title, f.className);
            break;
        case WindowVerdict::KeepSignIn:
            if (!c->result.keepWindow || !c->result.keepIsSignIn) {
                c->result.keepWindow = reinterpret_cast<quintptr>(h);
                c->result.keepIsSignIn = true;
                c->result.keepTitle = f.title;
            }
            break;
        case WindowVerdict::KeepMessage:
            if (!c->result.keepWindow) {
                c->result.keepWindow = reinterpret_cast<quintptr>(h);
                c->result.keepIsSignIn = false;
                c->result.keepTitle = f.title;
            }
            break;
        case WindowVerdict::Ignore: break;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

QHash<quint32, QString> clientProcesses(const QString& registryKey) {
    QHash<quint32, QString> out;
    const ClientState state = readClientState(registryKey);
    if (!state.alive) return out;
    out.insert(state.pid, QStringLiteral("steam.exe"));
    const auto snapshot = steamquit::snapshotProcesses();
    // The helper processes the client spawns (steamwebhelper.exe, a second steam.exe) hold some of its windows.
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& p : snapshot) {
            if (out.contains(p.pid) || !out.contains(p.parentPid)) continue;
            const QString n = p.name.toLower();
            if (n != QLatin1String("steamwebhelper.exe") && n != QLatin1String("steam.exe")) continue;
            out.insert(p.pid, n);
            grew = true;
        }
    }
    return out;
}

bool raiseWindow(quintptr handle) {
    HWND window = reinterpret_cast<HWND>(handle);
    if (!window || !IsWindow(window)) return false;
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    else ShowWindow(window, SW_SHOW);
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD thisThread = GetCurrentThreadId();
    const bool attached = foregroundThread && foregroundThread != thisThread && AttachThreadInput(thisThread, foregroundThread, TRUE);
    SetForegroundWindow(window);
    BringWindowToTop(window);
    if (attached) AttachThreadInput(thisThread, foregroundThread, FALSE);
    return true;
}
#else
SweepResult sweepWindows(const QHash<quint32, QString>&, bool, bool) { return {}; }
QHash<quint32, QString> clientProcesses(const QString&) { return {}; }
bool raiseWindow(quintptr) { return false; }
#endif

}  // namespace xmb::steamhide
