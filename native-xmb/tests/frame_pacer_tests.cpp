// Frame pacer (src/frame_pacer.h): the animation step follows the displayed swap timeline.
#include "frame_pacer.h"
#include <cstdio>
#include <cstdlib>
#include <random>
using xmb::FramePacer;
static int failures=0;
#define CHECK(c) do{if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)
// Drives paint->swap alternation on a display of `periodNs`: paint starts `startNs(i)` after the previous swap.
template<class F> static void drive(FramePacer& p,int64_t periodNs,int frames,F startOffset,std::vector<FramePacer::Step>& out,
                                   int skipEvery=0) {
    int64_t vblank=1'000'000'000;
    p.swapped(vblank);
    for(int i=0;i<frames;++i) {
        const int64_t paintAt=vblank+startOffset(i);
        out.push_back(p.step(paintAt,true));
        vblank+=periodNs*((skipEvery && i%skipEvery==skipEvery-1)?2:1);   // a missed vblank every skipEvery frames
        p.swapped(vblank);
    }
}
int main() {
    const int64_t P=16'666'667;
    std::mt19937 rng(7);std::uniform_int_distribution<int> jitter(100'000,3'500'000);
    {   // steady 60 Hz with paint starts jittering by 3 ms: every paced step is exactly one period.
        FramePacer p;std::vector<FramePacer::Step> steps;
        drive(p,P,200,[&](int){return int64_t(jitter(rng));},steps);
        int paced=0;double worst=0;
        for(size_t i=40;i<steps.size();++i){if(steps[i].paced)++paced;worst=std::max(worst,std::abs(steps[i].dt-double(P)/1e9));}
        CHECK(paced>=150);CHECK(worst<2e-5);CHECK(p.locked());CHECK(std::abs(p.periodMs()-16.667)<.01);
    }
    {   // the wall-clock step jitters by the same 3 ms (what the pacer replaces).
        double lo=1,hi=0;int64_t prev=0;
        for(int i=0;i<200;++i){const int64_t t=int64_t(i)*P+jitter(rng);if(i>1){const double d=double(t-prev)/1e9;lo=std::min(lo,d);hi=std::max(hi,d);}prev=t;}
        CHECK(hi-lo>.002);
    }
    {   // a missed vblank: the step after it is two periods (the animation catches up), the total matches the display.
        FramePacer p;std::vector<FramePacer::Step> steps;
        drive(p,P,240,[&](int){return int64_t(1'000'000);},steps,40);
        double total=0;int doubled=0;
        for(size_t i=60;i<steps.size();++i){total+=steps[i].dt;if(std::abs(steps[i].dt-2.*double(P)/1e9)<2e-5)++doubled;}
        CHECK(doubled>=3);
        const double displayed=(180.+4)*double(P)/1e9;   // 180 frames plus the 4 extra periods of missed vblanks in that span
        CHECK(std::abs(total-displayed)<.03);
    }
    {   // 239 Hz and 60 Hz both lock; a free-running swap (no fixed period) stays on the wall clock.
        FramePacer p;std::vector<FramePacer::Step> steps;
        drive(p,4'180'000,300,[&](int){return int64_t(500'000);},steps);
        CHECK(p.locked());CHECK(std::abs(steps.back().dt-.00418)<1e-5);
        FramePacer q;std::vector<FramePacer::Step> free;int64_t t=1'000'000'000;std::uniform_int_distribution<int> any(3'000'000,40'000'000);
        q.swapped(t);for(int i=0;i<200;++i){t+=any(rng);free.push_back(q.step(t-500'000,true));q.swapped(t);}
        CHECK(!q.locked());CHECK(!free.back().paced);
    }
    {   // a paint with no swap since the last paint (grab, resize) uses the wall step.
        FramePacer p;std::vector<FramePacer::Step> steps;drive(p,P,60,[&](int){return int64_t(1'000'000);},steps);
        const auto again=p.step(5'000'000'000LL+1'000'000,true);const auto next=p.step(5'000'000'000LL+4'000'000,true);
        CHECK(!next.paced);CHECK(std::abs(next.dt-.003)<1e-6);(void)again;
    }
    std::printf(failures?"frame pacer tests: %d FAILED\n":"frame pacer tests: ok\n",failures);
    return failures?1:0;
}
