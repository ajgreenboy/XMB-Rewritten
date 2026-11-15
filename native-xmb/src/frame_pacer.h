#pragma once
// Frame pacing: the animation clock follows when frames are DISPLAYED, not when paintGL happens to start
// (audit/FRAME-PACING.md).
//
// The paint thread used to advance every animation by the wall-clock gap between two paintGL entries. With vsync
// on, each paint starts a variable time after the vblank that released the previous swap (queued signal, event
// queue order, pad poll timer, GPU back-pressure), so the gaps jitter by a millisecond or three around the
// refresh period while the frames themselves reach the screen at exact refresh multiples. The result is a motion
// step that is uneven from frame to frame even at a steady 60 fps.
//
// FramePacer instead timestamps every swap (frameSwapped, delivered right after the vblank-locked buffer swap),
// estimates the refresh period from the median swap gap, and quantises each gap to a whole number of periods.
// A frame that was painted after swap N is advanced by the displayed interval swap N-1 -> swap N: exactly one
// period in steady state, two after a missed vblank (so the animation catches up to wall time in one frame
// instead of running slow). Where the swaps are not vblank locked (vsync off, no swaps, a stall or a pause) it
// falls back to the old wall-clock step, clamped to 100 ms, so nothing regresses there.
//
// Pure logic with no Qt GUI dependency, so work/pace-selftest.ts style checks and unit tests can drive it.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace xmb {
class FramePacer {
public:
    struct Step { double dt;      // seconds to advance every animation by
                  double contentSeconds; // the paced clock after this step (== wall clock in the fallback)
                  bool paced; };  // true when the step came from the swap timeline
    // frameSwapped, at the moment it is emitted (not through a queued connection, which adds the event queue wait).
    void swapped(int64_t ns) {
        if(last_>0) {
            const int64_t gap=ns-last_;
            if(gap>500'000 && gap<200'000'000) {
                gaps_[head_++%gaps_.size()]=gap;count_=std::min<int>(count_+1,int(gaps_.size()));
                refit();
                // A gap well under one period means the estimate is stale (another display, a rate change): follow the wall.
                const bool stall=!locked_ || gap<static_cast<int64_t>(.75*period_) || gap>static_cast<int64_t>(8*period_) || gap>100'000'000;
                const int64_t quantised=stall?std::min<int64_t>(gap,100'000'000)
                    :static_cast<int64_t>(std::max<int64_t>(1,std::llround(double(gap)/period_)))*static_cast<int64_t>(period_+.5);
                pending_+=quantised;++pendingSwaps_;
            } else if(gap>=200'000'000) { // a long pause: resume without a jump
                pending_+=100'000'000;++pendingSwaps_;
            }
        }
        last_=ns;
    }
    // Called once per paintGL with the wall clock. pacing=false (XMB_PACING=0, or a pinned capture clock) always
    // takes the wall-clock step.
    Step step(int64_t nowNs,bool pacing) {
        const double wallDt=std::clamp(double(nowNs-prevPaintNs_)/1e9,0.,.1);
        prevPaintNs_=nowNs;
        if(!pacing || !locked_ || pendingSwaps_==0) {
            // No new displayed interval to follow: advance by wall time (a paint with no swap in between, such as
            // grabFramebuffer, a resize paint, or the first frames before the period is known).
            contentNs_=content_valid_?contentNs_+static_cast<int64_t>(wallDt*1e9):nowNs;content_valid_=true;
            pending_=0;pendingSwaps_=0;
            return {wallDt,double(contentNs_)/1e9,false};
        }
        const double dt=std::min(double(pending_)/1e9,.1);
        pending_=0;pendingSwaps_=0;
        if(!content_valid_)contentNs_=nowNs;
        content_valid_=true;
        // Keep the paced clock tied to the wall clock with a slow slew, so a refresh-rate estimate that is a hair
        // off cannot drift the two apart. The mean lead (paced - wall) is captured once locked.
        const double lead=double(contentNs_-nowNs);
        if(!leadSet_){leadEma_=lead;leadAnchor_=lead;leadSet_=true;}
        leadEma_+=(lead-leadEma_)*.02;
        const double slew=std::clamp((leadEma_-leadAnchor_)*.005,-dt*1e9*.02,dt*1e9*.02);
        const double advanced=std::max(0.,dt*1e9-slew);
        contentNs_+=static_cast<int64_t>(advanced);
        if(std::llabs(contentNs_-nowNs)>250'000'000){contentNs_=nowNs;leadSet_=false;}   // a stall: start again
        return {advanced/1e9,double(contentNs_)/1e9,true};
    }
    double periodMs() const { return period_/1e6; }
    bool locked() const { return locked_; }
    void reset() { *this=FramePacer{}; }
private:
    void refit() {
        if(count_<12){locked_=false;return;}
        std::array<int64_t,64> sorted=gaps_;
        std::sort(sorted.begin(),sorted.begin()+count_);
        period_=double(sorted[count_/2]);
        // Locked when most gaps sit on the refresh period itself (a handful of missed vblanks are allowed). A
        // free-running swap (vsync off) spreads over a wide range and falls back to wall time.
        int near=0;
        for(int i=0;i<count_;++i)if(std::abs(double(gaps_[i])-period_)<period_*.15)++near;
        locked_=near*100>=count_*70;
    }
    std::array<int64_t,64> gaps_{};
    int head_=0,count_=0;
    int64_t last_=0,pending_=0,prevPaintNs_=0,contentNs_=0;
    int pendingSwaps_=0;
    double period_=16'666'667.;
    bool locked_=false,content_valid_=false,leadSet_=false;
    double leadEma_=0,leadAnchor_=0;
};
}
