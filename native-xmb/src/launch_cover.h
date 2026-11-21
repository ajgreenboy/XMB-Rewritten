#pragma once
// The launch cover (audit/GAME-LAUNCH-FADE.md): a separate black, topmost, never-activated, click-through window over the
// monitor the title will use. It is opaque before the title's process is created, so the display-mode switch, the
// title's window creation and the compositor's window animations all happen behind black; it fades off once the
// title's window has settled. The timing is launch_fade.h (pure, unit-tested); this class only samples windows, paints
// black and logs. Every path ends with the window hidden: the state machine's hard cap plus an independent watchdog.
#include "launch_fade.h"
#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <functional>

namespace xmb {

class LaunchCover : public QWidget {
    Q_OBJECT
public:
    // What the cover watches once the title has been started.
    struct Watch {
        bool alive = true;          // false: the tracked process is gone (treated as an exit)
        QList<quint32> pids;        // processes whose windows count as the title's
    };
    LaunchCover();
    ~LaunchCover() override;

    // On unless XMB_LAUNCH_COVER=0. XMB_NO_GAMEBOOT=1 (the scripted-run seam the pinned gate's launch recipes use) also
    // turns it off unless XMB_LAUNCH_COVER=1 asks for it, so those runs behave exactly as before.
    static bool enabled();

    // The XMB's own top-level window: the cover spans the monitor it is on (the monitor the display mode is applied to).
    void setAnchor(std::function<QWidget*()> anchor) { anchor_ = std::move(anchor); }
    // `root` and every process descended from it (one Toolhelp snapshot; parent links only, nothing is opened).
    static QList<quint32> processTree(quint32 root);

    // Covers the monitor, then runs start() once the cover is opaque (at once when alreadyBlack). start() returns false
    // when the title did not start (its own error surface is already up). watch() is polled while the cover holds.
    void begin(const QString& title, bool steam, bool alreadyBlack, std::function<bool()> start, std::function<Watch()> watch);
    bool active() const { return machine_.active(); }

    // Shell events. Each is ignored when the cover is not up.
    void titleEnded(const QString& why);
    void press(const QString& why);
    void withdraw(const QString& why);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void tick();
    void apply();
    void fitToMonitor();
    void finish();
    qint64 now() const { return clock_.elapsed(); }
    launchfade::WindowSample sampleWindow(quint64* pidOut);

    launchfade::Machine machine_;
    QElapsedTimer clock_;
    QTimer frame_, watchdog_;
    std::function<QWidget*()> anchor_;
    std::function<bool()> start_;
    std::function<Watch()> watch_;
    QString title_;
    bool steam_ = false;
    qint64 lastSample_ = -1000, lastTopmost_ = -1000;
    launchfade::Phase loggedPhase_ = launchfade::Phase::Idle;
    quint64 seenWindow_ = 0;
    double opacity_ = -1;
};

}  // namespace xmb
