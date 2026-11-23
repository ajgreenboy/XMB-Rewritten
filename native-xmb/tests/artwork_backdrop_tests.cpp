// PIC1 backdrop timing (src/artwork_backdrop.h): the dwell, the linear 200 ms fade in and the cut-like fade out that
// audit/BOOT-AND-BACKDROP-2026-11-22.md measured on the recording. Pins the constants and the per-frame maths.
#include "artwork_backdrop.h"
#include <cmath>
#include <cstdio>
using namespace xmb::backdrop;
static int failures=0;
#define CHECK(c) do{if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)
static bool near(double a,double b,double e=1e-9){return std::abs(a-b)<e;}
int main() {
    // The measured constants.
    CHECK(dwellMs==1200 && near(fadeInMs,200.) && near(fadeOutMs,33.));
    // Dwell: nothing before 1200 ms of rest, from 1200 on the fade may start.
    CHECK(!dwellElapsed(0) && !dwellElapsed(1199) && dwellElapsed(1200) && dwellElapsed(5000));
    // Fade in, linear: 100 ms is half, 200 ms is full, whatever the frame rate.
    {
        double o=0;for(int i=0;i<6;++i)o=step(o,true,.0166667);   // 6 frames at 60 Hz = 100 ms
        CHECK(near(o,.5,1e-3));
        for(int i=0;i<6;++i)o=step(o,true,.0166667);
        CHECK(near(o,1.,1e-3));
        for(int i=0;i<10;++i)o=step(o,true,.0166667);
        CHECK(o==1.);                                          // clamped
        double p=0;for(int i=0;i<12;++i)p=step(p,true,1./120);  // 12 frames at 120 Hz = 100 ms
        CHECK(near(p,.5,1e-3));
        CHECK(near(step(0,true,.2),1.));                       // a single 200 ms frame completes it
    }
    // Fade out: a cut within 33 ms; two 60 Hz frames are enough, and it never goes below 0.
    {
        double o=1;o=step(o,false,.0166667);CHECK(o>0. && o<1.);
        o=step(o,false,.0166667);CHECK(o==0.);
        CHECK(step(0,false,.5)==0.);
        CHECK(near(step(1,false,.033),0.,1e-9));
        double d=1;d=departingStep(d,.0166667);CHECK(d>0. && d<1.);
        d=departingStep(d,.0166667);CHECK(d==0.);
        CHECK(departingStep(0,.01)==0.);
    }
    // A zero-length frame changes nothing.
    CHECK(near(step(.3,true,0),.3) && near(step(.3,false,0),.3));
    if(failures){std::printf("%d failures\n",failures);return 1;}
    std::printf("artwork backdrop timing tests passed\n");return 0;
}
