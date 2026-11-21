// Game-boot background presets (src/gameboot_background.h): event schedule, preset selection and blend maths.
// The schedule times and durations are TRACED; the blend curve and the level reduction are INFERRED (no RPCS3 reference),
// so these tests pin the model's own contract, not firmware output.
#include "gameboot_background.h"
#include <cmath>
#include <cstdio>
using namespace xmb::gameboot;
static int failures=0;
#define CHECK(c) do{if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)
static bool near(double a,double b,double e=1e-9){return std::abs(a-b)<e;}
int main() {
    // The traced events: BG2 at 0 s over 0.25 s, BG3 at 0.5 s over 1.25 s.
    CHECK(schedule.size()==2);
    CHECK(schedule[0].preset==2 && near(schedule[0].at,0) && near(schedule[0].seconds,.25));
    CHECK(schedule[1].preset==3 && near(schedule[1].at,.5) && near(schedule[1].seconds,1.25));
    {   // Selection: gameboot2 from the start, then gameboot3 from gameboot2.
        Blend b=blendAt(0);CHECK(b.from==0 && b.to==2 && near(b.weight,0));
        b=blendAt(.125);CHECK(b.from==0 && b.to==2 && near(b.weight,.5));
        b=blendAt(.25);CHECK(b.from==0 && b.to==2 && near(b.weight,1));
        b=blendAt(.4999);CHECK(b.to==2 && near(b.weight,1));
        b=blendAt(.5);CHECK(b.from==2 && b.to==3 && near(b.weight,0));
        b=blendAt(1.125);CHECK(b.from==2 && b.to==3 && near(b.weight,.5));
        b=blendAt(1.75);CHECK(b.from==2 && b.to==3 && near(b.weight,1));
        b=blendAt(2.8);CHECK(b.to==3 && near(b.weight,1));
        b=blendAt(-1);CHECK(b.to==0 && near(b.weight,1));       // before the sequence: no event, the start state
    }
    const std::array<double,5> level=fallbackPresetLevel;
    CHECK(near(levelAt(-1,level),startLevel));                      // untouched XMB backdrop
    CHECK(near(levelAt(0,level),startLevel));                       // BG2 has only just fired
    CHECK(near(levelAt(.125,level),(startLevel+level[1])/2));       // half-way to gameboot2
    CHECK(near(levelAt(.25,level),level[1]) && near(levelAt(.5,level),level[1]));
    CHECK(near(levelAt(1.125,level),level[1]/2));                   // half-way gameboot2 -> gameboot3 (level 0)
    CHECK(near(levelAt(1.75,level),0) && near(levelAt(2.8,level),0) && near(levelAt(60,level),0));
    // The veil is the complement and the level never leaves 0..1, whatever the presets hold.
    CHECK(near(veilAt(1.125,level),1-levelAt(1.125,level)));
    CHECK(near(veilAt(2.8,level),1));
    const std::array<double,5> wild{{5,-3,9,0,0}};
    for(double t=0;t<3;t+=.05){const double v=levelAt(t,wild);CHECK(v>=0 && v<=1);}
    // Monotone fade once BG3 runs (gameboot2 level above gameboot3's).
    double last=2;for(double t=.5;t<=1.75;t+=.05){const double v=levelAt(t,level);CHECK(v<=last+1e-12);last=v;}
    // Presets 1..5 are indexable: gameboot1 is near black, 2 is the bright one, 3..5 are black (4.93 BACKGROUND.mnu).
    CHECK(level[0]<.01 && near(level[1],.8) && near(level[2],0) && near(level[3],0) && near(level[4],0));
    if(failures){std::printf("%d failure(s)\n",failures);return 1;}
    std::printf("gameboot background tests passed\n");
    return 0;
}
