#include "launch_cover.h"
#include "game_capture.h"
#include <QDateTime>
#include <QGuiApplication>
#include <QDebug>
#include <QPainter>
#include <QScreen>
#include <QSet>
#include <cmath>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#endif

namespace xmb {
namespace {
qint64 epochMs() { return QDateTime::currentMSecsSinceEpoch(); }
launchfade::WindowMode modeOf(GameWindowMode m) {
    switch(m) {
    case GameWindowMode::Minimized: return launchfade::WindowMode::Minimized;
    case GameWindowMode::Windowed: return launchfade::WindowMode::Windowed;
    case GameWindowMode::Borderless: return launchfade::WindowMode::Borderless;
    case GameWindowMode::Exclusive: return launchfade::WindowMode::Exclusive;
    default: return launchfade::WindowMode::None;
    }
}
const char* modeName(launchfade::WindowMode m) {
    switch(m) {
    case launchfade::WindowMode::Minimized: return "minimized";
    case launchfade::WindowMode::Windowed: return "windowed";
    case launchfade::WindowMode::Borderless: return "borderless";
    case launchfade::WindowMode::Exclusive: return "exclusive";
    default: return "none";
    }
}
#ifdef Q_OS_WIN
// The documented way through the foreground lock (the same as View's takeForeground): attach to the current foreground
// thread's input queue for the call. Nothing is injected into any process.
bool giveForeground(HWND window) {
    if(!window || !IsWindow(window))return false;
    const HWND foreground=GetForegroundWindow();
    if(foreground==window)return true;
    const DWORD foregroundThread=foreground?GetWindowThreadProcessId(foreground,nullptr):0;
    const DWORD thisThread=GetCurrentThreadId();
    const bool attached=foregroundThread && foregroundThread!=thisThread && AttachThreadInput(thisThread,foregroundThread,TRUE);
    SetForegroundWindow(window);BringWindowToTop(window);
    if(attached)AttachThreadInput(thisThread,foregroundThread,FALSE);
    return GetForegroundWindow()==window;
}
#endif
}

LaunchCover::LaunchCover():
    QWidget(nullptr,Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint|Qt::Tool|Qt::WindowDoesNotAcceptFocus|Qt::WindowTransparentForInput) {
    setWindowTitle(QStringLiteral("XMB launch cover"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    frame_.setInterval(16);
    frame_.setTimerType(Qt::PreciseTimer);
    connect(&frame_,&QTimer::timeout,this,&LaunchCover::tick);
    watchdog_.setSingleShot(true);
    connect(&watchdog_,&QTimer::timeout,this,[this]{
        // Independent of the state machine: whatever went wrong, the user is never left behind black.
        if(!isVisible() && !machine_.active())return;
        qWarning().noquote()<<QStringLiteral("launch-cover: watchdog removed the cover (phase %1) at=%2")
            .arg(QLatin1String(launchfade::Machine::phaseName(machine_.phase()))).arg(epochMs());
        if(start_){auto start=std::move(start_);start_=nullptr;start();}   // never lose a launch
        machine_=launchfade::Machine(machine_.timing());
        finish();
    });
}

LaunchCover::~LaunchCover()=default;

bool LaunchCover::enabled() {
    const QString forced=qEnvironmentVariable("XMB_LAUNCH_COVER");
    if(forced==QLatin1String("0"))return false;
    if(forced==QLatin1String("1"))return true;
    return qEnvironmentVariableIntValue("XMB_NO_GAMEBOOT")==0;
}

QList<quint32> LaunchCover::processTree(quint32 root) {
    QList<quint32> tree;
    if(!root)return tree;
    tree.push_back(root);
#ifdef Q_OS_WIN
    QList<QPair<quint32,quint32>> links;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE)return tree;
    PROCESSENTRY32W e{};e.dwSize=sizeof(e);
    for(BOOL ok=Process32FirstW(snap,&e);ok;ok=Process32NextW(snap,&e))links.push_back({quint32(e.th32ProcessID),quint32(e.th32ParentProcessID)});
    CloseHandle(snap);
    QSet<quint32> in{root};
    for(bool grew=true;grew;) {
        grew=false;
        for(const auto& [pid,parent]:links)
            if(pid && pid!=parent && !in.contains(pid) && in.contains(parent)){in.insert(pid);tree.push_back(pid);grew=true;}
    }
#endif
    return tree;
}

void LaunchCover::begin(const QString& title,bool steam,bool alreadyBlack,std::function<bool()> start,std::function<Watch()> watch) {
    if(machine_.active()) {
        // One cover at a time; a second launch while it is up (not reachable through the shell) just starts.
        qInfo().noquote()<<"launch-cover: already up; starting"<<title<<"uncovered";
        if(start)start();
        return;
    }
    bool numeric=false;
    const int override=qEnvironmentVariable("XMB_LAUNCH_COVER_TIMEOUT_MS").toInt(&numeric);   // scripted-run seam
    machine_=launchfade::Machine(launchfade::Timing::forLaunch(steam,numeric && override>=500 && override<=600000?override:0));
    title_=title;steam_=steam;start_=std::move(start);watch_=std::move(watch);
    lastSample_=-1000;lastTopmost_=-1000;seenWindow_=0;opacity_=-1;loggedPhase_=launchfade::Phase::Idle;
    clock_.start();
    machine_.begin(0,alreadyBlack);
    QWidget* anchor=anchor_?anchor_():nullptr;
    if(QScreen* screen=anchor && anchor->screen()?anchor->screen():QGuiApplication::primaryScreen())setGeometry(screen->geometry());
    apply();
    show();
    fitToMonitor();
#ifdef Q_OS_WIN
    const HWND hwnd=reinterpret_cast<HWND>(winId());
    SetWindowLongPtrW(hwnd,GWL_EXSTYLE,GetWindowLongPtrW(hwnd,GWL_EXSTYLE)|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT);
    SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_FRAMECHANGED);
#endif
    const auto& t=machine_.timing();
    const QRect g=frameGeometry();
    qInfo().noquote()<<QStringLiteral("launch-cover: begin title=\"%1\" steam=%2 alreadyBlack=%3 fadeToBlackMs=%4 stableMs=%5 fadeFromBlackMs=%6 "
        "timeoutMs=%7 hardCapMs=%8 rect=%9,%10 %11x%12 at=%13")
        .arg(title).arg(steam?1:0).arg(alreadyBlack?1:0).arg(alreadyBlack?0:t.fadeToBlackMs).arg(t.stableMs).arg(t.fadeFromBlackMs)
        .arg(t.noWindowTimeoutMs).arg(t.hardCap()).arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height()).arg(epochMs());
    watchdog_.start(int(t.hardCap()+1500));
    frame_.start();
    tick();   // after a game boot the cover is opaque now, so the title starts in this same turn
}

void LaunchCover::titleEnded(const QString& why) {
    if(!machine_.active())return;
    qInfo().noquote()<<QStringLiteral("launch-cover: title ended (%1) at=%2").arg(why).arg(epochMs());
    machine_.processExited(now());
    tick();
}

void LaunchCover::press(const QString& why) {
    if(machine_.phase()!=launchfade::Phase::Hold)return;
    qInfo().noquote()<<QStringLiteral("launch-cover: press (%1) at=%2").arg(why).arg(epochMs());
    machine_.press(now());
    tick();
}

void LaunchCover::withdraw(const QString& why) {
    if(!machine_.active())return;
    qInfo().noquote()<<QStringLiteral("launch-cover: withdrawn (%1) at=%2").arg(why).arg(epochMs());
    machine_.abort(now());
    tick();
}

void LaunchCover::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(),Qt::black);
}

launchfade::WindowSample LaunchCover::sampleWindow(quint64* pidOut) {
    launchfade::WindowSample best;
#ifdef Q_OS_WIN
    if(!watch_)return best;
    const Watch w=watch_();
    if(!w.alive){*pidOut=quint64(-1);return best;}
    qint64 bestArea=0;quintptr bestHwnd=0;quint32 bestPid=0;
    for(const quint32 pid:w.pids) {
        const quintptr hwnd=findMainWindow(pid);
        if(!hwnd)continue;
        RECT r{};
        if(!GetWindowRect(reinterpret_cast<HWND>(hwnd),&r))continue;
        const qint64 area=qint64(r.right-r.left)*(r.bottom-r.top);
        if(area>bestArea){bestArea=area;bestHwnd=hwnd;bestPid=pid;}
    }
    if(!bestHwnd)return best;
    const GameWindowInfo info=classifyGameWindow(bestHwnd);
    best.present=info.mode!=GameWindowMode::Invalid;
    best.mode=modeOf(info.mode);
    best.x=info.windowRect.x();best.y=info.windowRect.y();best.w=info.windowRect.width();best.h=info.windowRect.height();
    best.coversMonitor=info.coversMonitor;
    best.id=bestHwnd;
    *pidOut=bestPid;
#else
    Q_UNUSED(pidOut);
#endif
    return best;
}

void LaunchCover::tick() {
    using launchfade::Phase;
    machine_.tick(now());
    if(machine_.takeLaunchDue()) {
        apply();
        if(isVisible() && machine_.active()) {
            repaint();
#ifdef Q_OS_WIN
            // Two compositor frames: the black cover is on the glass before the title's process exists.
            DwmFlush();DwmFlush();
#endif
        }
        qInfo().noquote()<<QStringLiteral("launch-cover: opaque=%1; starting the title at=%2").arg(machine_.active()?1:0).arg(epochMs());
        auto start=std::move(start_);start_=nullptr;
        const bool started=start?start():false;
        if(!started) {
            qInfo().noquote()<<QStringLiteral("launch-cover: the title did not start at=%1").arg(epochMs());
            machine_.startFailed(now());
        } else qInfo().noquote()<<QStringLiteral("launch-cover: title started at=%1").arg(epochMs());
    }
    const qint64 t=now();
    if(machine_.phase()==Phase::Hold && t-lastSample_>=100) {
        lastSample_=t;
        quint64 pid=0;
        const auto s=sampleWindow(&pid);
        if(pid==quint64(-1)) {
            qInfo().noquote()<<QStringLiteral("launch-cover: the title's process is gone at=%1").arg(epochMs());
            machine_.processExited(now());
        } else {
            if(s.usable() && s.id!=seenWindow_) {
                seenWindow_=s.id;
                qInfo().noquote()<<QStringLiteral("launch-cover: window seen hwnd=0x%1 pid=%2 mode=%3 rect=%4,%5 %6x%7 covers=%8 at=%9")
                    .arg(s.id,0,16).arg(pid).arg(QLatin1String(modeName(s.mode))).arg(s.x).arg(s.y).arg(s.w).arg(s.h).arg(s.coversMonitor?1:0).arg(epochMs());
            }
            machine_.sample(now(),s);
        }
    }
    if(machine_.takeHandoffDue()) {
        const auto& w=machine_.window();
        bool given=false;
#ifdef Q_OS_WIN
        given=giveForeground(reinterpret_cast<HWND>(quintptr(w.id)));
#endif
        qInfo().noquote()<<QStringLiteral("launch-cover: foreground handed to hwnd=0x%1 ok=%2 at=%3").arg(w.id,0,16).arg(given?1:0).arg(epochMs());
    }
    if(machine_.phase()!=loggedPhase_) {
        const Phase phase=machine_.phase();
        if(phase==Phase::FadeFromBlack || (phase==Phase::Done && loggedPhase_==Phase::Hold)) {
            const auto& w=machine_.window();
            qInfo().noquote()<<QStringLiteral("launch-cover: fade-out reason=%1 fadeMs=%2 mode=%3 rect=%4,%5 %6x%7 after=%8 ms at=%9")
                .arg(QLatin1String(launchfade::Machine::reasonName(machine_.reason()))).arg(machine_.fadeOutMs())
                .arg(QLatin1String(modeName(machine_.reason()==launchfade::Reason::WindowStable?w.mode:launchfade::WindowMode::None)))
                .arg(w.x).arg(w.y).arg(w.w).arg(w.h).arg(now()).arg(epochMs());
        }
        loggedPhase_=phase;
    }
    if(machine_.active() && machine_.phase()!=Phase::FadeFromBlack)fitToMonitor();
    apply();
    if(!machine_.active() && (isVisible() || frame_.isActive())) {
        qInfo().noquote()<<QStringLiteral("launch-cover: done reason=%1 after=%2 ms at=%3")
            .arg(QLatin1String(launchfade::Machine::reasonName(machine_.reason()))).arg(now()).arg(epochMs());
        finish();
    }
}

void LaunchCover::apply() {
    const double o=machine_.active()?machine_.opacity(now()):0.;
    if(std::abs(o-opacity_)<.002)return;
    opacity_=o;
    setWindowOpacity(o);
}

void LaunchCover::fitToMonitor() {
#ifdef Q_OS_WIN
    if(!isVisible())return;
    const HWND hwnd=reinterpret_cast<HWND>(winId());
    QWidget* anchor=anchor_?anchor_():nullptr;
    const HWND anchorHwnd=anchor?reinterpret_cast<HWND>(anchor->window()->winId()):nullptr;
    const HMONITOR monitor=anchorHwnd?MonitorFromWindow(anchorHwnd,MONITOR_DEFAULTTOPRIMARY):MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};mi.cbSize=sizeof(mi);
    if(!GetMonitorInfoW(monitor,&mi))return;
    RECT current{};GetWindowRect(hwnd,&current);
    const qint64 t=now();
    // A display-mode switch resizes the monitor under the cover: follow it. The topmost band is re-asserted a few
    // times a second, since a title's own topmost window created later would otherwise sit above the cover.
    if(!EqualRect(&current,&mi.rcMonitor)) {
        SetWindowPos(hwnd,HWND_TOPMOST,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_NOACTIVATE);
        lastTopmost_=t;
        if(lastTopmost_>=0 && current.right>current.left)
            qInfo().noquote()<<QStringLiteral("launch-cover: monitor is now %1,%2 %3x%4; cover resized at=%5").arg(mi.rcMonitor.left).arg(mi.rcMonitor.top)
                .arg(mi.rcMonitor.right-mi.rcMonitor.left).arg(mi.rcMonitor.bottom-mi.rcMonitor.top).arg(epochMs());
    } else if(t-lastTopmost_>=250) {
        SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        lastTopmost_=t;
    }
#endif
}

void LaunchCover::finish() {
    frame_.stop();
    watchdog_.stop();
    watch_=nullptr;
    opacity_=-1;
    hide();
}

}  // namespace xmb
