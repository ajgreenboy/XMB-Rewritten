#pragma once
// XMB_FRAME_TRACE=1: opt-in frame-time log for hitch hunting (audit/PS-MENU-HITCH.md).
//
// Every frame whose CPU time in View::paintGL, or whose gap since the previous swap, passes 20 ms is logged with
// labels for what that frame did (a surface opening for the first time, an RCO name index build, a first image
// decode, glyph-cache misses, an atlas page, the PS menu's bloom), and one summary line per second reports the
// average and worst frame. Off by default: every call site tests on() first, so a normal run pays one load and a
// branch per site and builds no strings. Everything here runs on the GUI thread.
//
// (XMB_FRAME_TRACE=<path> keeps its older meaning: a binary-ish paint/swap dump written at exit. Only the value
// "1" selects this log.)
#include <algorithm>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QDebug>
#include <QProcessEnvironment>
#include <QtGlobal>

namespace xmb::frametrace {
inline bool g_on=false;
inline bool on() { return g_on; }
inline QStringList g_pending;   // labels noted since the last finished paint (GUI thread only)
inline void note(const QString& label) { if(g_on)g_pending<<label; }

class Recorder {
public:
    // Called by the View once, from the environment. XMB_FRAME_TRACE_MS=<n> moves the 20 ms line (to look at 8 ms
    // frames, say, when a 60 Hz display has only 16.7 ms to give).
    void enable(bool value) {
        g_on=value;on_=value;
        if(!value)return;
        clock_.start();
        bool ok=false;const double wanted=qEnvironmentVariable("XMB_FRAME_TRACE_MS").toDouble(&ok);
        if(ok && wanted>0.)thresholdMs=wanted;
    }
    bool enabled() const { return on_; }
    // End of a paintGL: the CPU time the frame took, taken with the labels noted during it.
    void paintDone(double cpuMs,bool rebuilt) {
        if(!on_)return;
        cpuMs_=cpuMs;rebuilt_=rebuilt;labels_=g_pending;g_pending.clear();havePaint_=true;
        // Early-returning paints (the intro, the gallery) never swap-log a stale value.
    }
    // frameSwapped: the swap gap, then the log decision for the frame that just finished.
    void swapped() {
        if(!on_)return;
        const double now=clock_.nsecsElapsed()/1e6;
        const double gap=lastSwap_<0?0.:now-lastSwap_;lastSwap_=now;
        const double cpu=havePaint_?cpuMs_:0.;
        const bool slow=cpu>=thresholdMs || gap>=thresholdMs;
        if(slow) {
            ++slow_;
            qInfo().noquote()<<QString("frame-trace: SLOW t=%1 ms cpu=%2 ms gap=%3 ms rebuild=%4 [%5]")
                .arg(now,0,'f',0).arg(cpu,0,'f',1).arg(gap,0,'f',1).arg(rebuilt_?1:0).arg(labels_.join(QStringLiteral("; ")));
        }
        if(havePaint_){sumCpu_+=cpu;worstCpu_=std::max(worstCpu_,cpu);++n_;}
        worstGap_=std::max(worstGap_,gap);
        havePaint_=false;labels_.clear();
        if(now-windowStart_>=1000.) {
            if(n_>0)qInfo().noquote()<<QString("frame-trace: 1s frames=%1 avg-cpu=%2 ms worst-cpu=%3 ms worst-gap=%4 ms slow=%5")
                .arg(n_).arg(sumCpu_/n_,0,'f',1).arg(worstCpu_,0,'f',1).arg(worstGap_,0,'f',1).arg(slow_);
            windowStart_=now;n_=0;slow_=0;sumCpu_=worstCpu_=worstGap_=0;
        }
    }
private:
    bool on_=false,havePaint_=false,rebuilt_=false;
    double thresholdMs=20.;
    QElapsedTimer clock_;
    double cpuMs_=0,lastSwap_=-1,windowStart_=0,sumCpu_=0,worstCpu_=0,worstGap_=0;
    int n_=0,slow_=0;
    QStringList labels_;
};
}
