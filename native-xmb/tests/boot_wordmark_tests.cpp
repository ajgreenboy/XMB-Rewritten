// "Sony Computer Entertainment" boot stage (src/boot_wordmark.h): the measured fade curves, the fit into the window before
// the PS3 logo, the XMB_BOOT_WORDMARK switch and the placement table. Pure logic, no GL.
#include "boot_wordmark.h"
#include <cmath>
#include <cstdio>
using namespace xmb::bootwordmark;
static int failures=0;
#define CHECK(c) do{if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)
static bool near(double a,double b,double e=1e-9){return std::abs(a-b)<e;}
int main() {
    // The measured stage: 11.67 .. 12.39 in, hold to 14.12, out to 14.84 (m2), so 0.72 + 1.73 + 0.72 = 3.17 s.
    {
        Schedule m;
        CHECK(near(m.fadeIn,.72) && near(m.hold,1.73) && near(m.fadeOut,.72));
        CHECK(near(m.end(),3.17) && near(m.fullAt(),.72) && near(m.fadeOutAt(),2.45));
        CHECK(alphaAt(m,0)==0 && alphaAt(m,-1)==0);
        CHECK(near(alphaAt(m,.36),.5));                       // half way up
        CHECK(near(alphaAt(m,.72),1) && near(alphaAt(m,1.5),1) && near(alphaAt(m,2.45),1));
        CHECK(near(alphaAt(m,2.81),.5));                      // half way down
        CHECK(alphaAt(m,3.17)==0 && alphaAt(m,9)==0);
        // The recording's samples, as offsets from the first visible time (11.67 s): 11.8 s 0.18, 12.2 s 0.74, 14.4 s 0.62.
        CHECK(near(alphaAt(m,11.8-11.67),.18,.01) && near(alphaAt(m,12.2-11.67),.74,.01) && near(alphaAt(m,14.4-11.67),.62,.12));
    }
    // Monotone up through the fade in, monotone down through the fade out, never outside 0..1.
    {
        Schedule m;double last=-1;bool ok=true;
        for(int i=0;i<=72;++i){const double a=alphaAt(m,i*.01);if(a<last-1e-12||a<0||a>1)ok=false;last=a;}
        CHECK(ok);
        last=2;ok=true;
        for(int i=0;i<=72;++i){const double a=alphaAt(m,m.fadeOutAt()+i*.01);if(a>last+1e-12||a<0||a>1)ok=false;last=a;}
        CHECK(ok);
    }
    // Fitted into the window before the logo (0.2 .. 2.1 s): the fades keep their length, only the hold shortens.
    {
        const Schedule s=fitted(startSeconds,logoFirstVisibleSeconds);
        CHECK(near(s.start,.2) && near(s.fadeIn,.72) && near(s.fadeOut,.72));
        CHECK(near(s.hold,2.1-.2-.72-.72));
        CHECK(near(s.end(),2.1));
        CHECK(alphaAt(s,.2)==0 && alphaAt(s,2.1)==0 && alphaAt(s,2.2)==0);
        CHECK(near(alphaAt(s,.56),.5) && near(alphaAt(s,s.fullAt()),1));
        CHECK(alphaAt(s,logoFirstVisibleSeconds-.001)<.01);   // gone when the logo's first light shows
        // A roomy window keeps the measured hold; a tiny one keeps both fades and a zero hold.
        CHECK(near(fitted(0,10).hold,1.73) && near(fitted(0,10).end(),3.17));
        CHECK(near(fitted(0,1).hold,0) && near(fitted(0,1).end(),1.44));
    }
    // XMB_BOOT_WORDMARK: on by default, off for 0 / off / false / no, anything else on.
    CHECK(enabledFrom(nullptr) && enabledFrom("") && enabledFrom("1") && enabledFrom("on") && enabledFrom("yes"));
    CHECK(!enabledFrom("0") && !enabledFrom("off") && !enabledFrom("false") && !enabledFrom("no"));
    // Placement: words left to right, positive widths, gaps of a few design px between them, inside the 1920 design width,
    // on the baseline with the cap height measured on the recording (cap top 350 .. baseline 366 of 720).
    {
        CHECK(words[0].left<words[1].left && words[1].left<words[2].left);
        for(int i=0;i<2;++i){const double gap=words[i+1].left-(words[i].left+words[i].width);CHECK(gap>3 && gap<15);}
        for(const auto& w:words)CHECK(w.width>0 && w.left>0 && w.left+w.width<1920);
        CHECK(near(words[2].left+words[2].width,1624.5) && near(words[0].left,1081.5));
        CHECK(near(baseline,549.) && near(capHeight,24.));
        CHECK(near(baseline/1.5,366.) && near((baseline-capHeight)/1.5,350.));
    }
    // The ramp under the wordmark: one linear clock from this boot to m2's, the text's first/last visible moments on the stage's ends.
    {
        CHECK(near(recordingTime(startSeconds),11.67) && near(recordingTime(logoFirstVisibleSeconds),14.84));
        CHECK(near(recordingPerSecond,3.17/1.9));
        // Black before the recording's background starts, 1 (the existing background) once the ramp is done, monotone between.
        CHECK(backgroundAlpha(0)==0 && backgroundAlpha(-5)==0 && backgroundAlpha(startSeconds)<.12);   // the tone map shows nothing below about .1: m2 is .003 at 11.6 s
        CHECK(backgroundAlpha(rampEnd)==1 && backgroundAlpha(rampEnd+.5)==1 && backgroundAlpha(1e9)==1);
        double last=-1;bool ok=true;
        for(int i=0;i<=900;++i){const double a=backgroundAlpha(i*.01);if(a<last-1e-12||a<0||a>1)ok=false;last=a;}
        CHECK(ok);
        // The fitted knots: alpha .168 at 12.0 s, .564 at 14.0 s, .718 at 14.8 s (the table, which already corrects for the tone map).
        CHECK(near(backgroundAlpha(startSeconds+(12.0-11.67)/recordingPerSecond),.168,1e-9));
        CHECK(near(backgroundAlpha(startSeconds+(14.0-11.67)/recordingPerSecond),.564,1e-9));
        CHECK(near(backgroundAlpha(logoFirstVisibleSeconds),.718,.02));
        // The ramp is done well before ShowGUI (5.5 s) and the handoff is continuous: no step at rampEnd.
        CHECK(rampEnd<5.5 && rampEnd>logoFirstVisibleSeconds);
        CHECK(backgroundAlpha(rampEnd-.01)>.99);
        // Ribbon: dark at the first frame (no flash), monotone up, exactly the native ribbon from ribbonFullAt, never above it.
        CHECK(ribbonGain(0)==0 && ribbonGain(-1)==0);
        CHECK(ribbonGain(.05)<.1 && ribbonGain(startSeconds)<.2);
        last=-1;ok=true;
        for(int i=0;i<=900;++i){const double g=ribbonGain(i*.01);if(g<last-1e-12||g<0||g>1)ok=false;last=g;}
        CHECK(ok);
        CHECK(ribbonGain(ribbonFullAt)==1 && ribbonGain(ribbonFullAt+3)==1 && ribbonGain(1e9)==1);
        CHECK(ribbonFullAt<logoFirstVisibleSeconds);
        // piecewise: flat outside the knots, linear inside.
        const Knot k[]={{1,10},{3,20}};
        CHECK(piecewise(k,0)==10 && piecewise(k,4)==20 && near(piecewise(k,2),15));
    }
    std::printf(failures?"boot-wordmark: %d failure(s)\n":"boot-wordmark: all tests passed\n",failures);
    return failures?1:0;
}
