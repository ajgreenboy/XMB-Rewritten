#include "osk_pad_input.h"
#include <QCoreApplication>
#include <cstdio>

// The pure part of the on-screen keyboard's pad extras: L1/R1 caret, L2/R2 pages, SELECT, the trigger latch and the
// open/close fade (audit/BEHAVIOUR-GAPS-SPEC.md 3). No window and no pad. Build with -DXMB_OSK_PAD_BUILD_TESTS=ON.
using namespace xmb;

static int failures=0;
static int checks=0;
static void check(bool ok,const char* what) {
    ++checks;
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}
static QVector<OskPage> pagesNamed(const QStringList& names) {
    QVector<OskPage> pages;
    for(const QString& name:names){OskPage page;page.name=name;pages.push_back(page);}
    return pages;
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    using A=OskAction;

    OskTriggerLatch latch;
    check(oskPadActions(0,0,0,latch).isEmpty(),"no buttons, no actions");
    check(oskPadActions(oskPadL1,0,0,latch)==QVector<A>{A::CaretLeft},"L1 moves the caret left");
    check(oskPadActions(oskPadR1,0,0,latch)==QVector<A>{A::CaretRight},"R1 moves the caret right");
    check(oskPadActions(oskPadSelect,0,0,latch)==QVector<A>{A::Mode},"SELECT is Input Mode");
    check(oskPadActions(0x1000|0x0010|0x0001,0,0,latch).isEmpty(),"Cross, START and the d-pad are not extras");
    check((oskPadActions(oskPadL1|oskPadR1|oskPadSelect,0,0,latch)==QVector<A>{A::CaretLeft,A::CaretRight,A::Mode}),"several buttons keep the fixed order");

    OskTriggerLatch triggers;
    check(oskPadActions(0,0.4f,0,triggers).isEmpty(),"a trigger below 0.5 does nothing");
    check(oskPadActions(0,0.9f,0,triggers)==QVector<A>{A::Accent},"L2 past 0.5 is the accent page");
    check(oskPadActions(0,1.0f,0,triggers).isEmpty(),"a held L2 fires once");
    check(oskPadActions(0,0.3f,0,triggers).isEmpty() && triggers.l2,"L2 between 0.25 and 0.5 stays latched (no chatter)");
    check(oskPadActions(0,0.1f,0,triggers).isEmpty() && !triggers.l2,"L2 below 0.25 re-arms");
    check(oskPadActions(0,0.9f,0,triggers)==QVector<A>{A::Accent},"L2 fires again after release");
    OskTriggerLatch second;
    check(oskPadActions(0,0,0.7f,second)==QVector<A>{A::Shift},"R2 past 0.5 is the caps page");
    OskTriggerLatch both;
    check((oskPadActions(0,0.8f,0.8f,both)==QVector<A>{A::Accent,A::Shift}),"both triggers in one poll: L2 then R2");

    const auto twoPages=pagesNamed({"lower","caps"});
    check(oskPageForAction(twoPages,0,A::Shift)==1,"R2 on lower goes to caps");
    check(oskPageForAction(twoPages,1,A::Shift)==0,"R2 on caps goes back to lower");
    check(oskPageForAction(twoPages,0,A::Accent)==-1,"L2 is inert while no accent page exists");
    check(oskPageForAction(twoPages,1,A::Accent)==-1,"L2 is inert on caps too");
    check(oskPageForAction(twoPages,0,A::CaretLeft)==-1,"a caret action never changes the page");
    const auto threePages=pagesNamed({"lower","caps","accent"});
    check(oskPageForAction(threePages,0,A::Accent)==2,"with an accent page, L2 opens it");
    check(oskPageForAction(threePages,2,A::Accent)==0,"L2 on the accent page returns to lower");
    check(oskPageForAction(threePages,2,A::Shift)==1,"R2 on the accent page goes to caps");
    check(oskPageForAction(pagesNamed({"tenkey"}),0,A::Shift)==-1,"a one-page layout has nowhere to go");
    check(oskPageForAction(pagesNamed({"a","b","c"}),0,A::Shift)==1,"unnamed pages fall back to the next-page step");

    check(oskFadeAlpha(0,true)==0.,"open fade starts at 0");
    check(oskFadeAlpha(oskOpenFadeMilliseconds,true)==1. && oskFadeAlpha(500,true)==1.,"open fade ends at 1 and stays");
    check(oskFadeAlpha(oskOpenFadeMilliseconds/2,true)>0.45 && oskFadeAlpha(oskOpenFadeMilliseconds/2,true)<0.55,"open fade is linear at its midpoint");
    check(oskFadeAlpha(0,false)==1.,"close fade starts at 1");
    check(oskFadeAlpha(oskCloseFadeMilliseconds,false)==0. && oskFadeAlpha(500,false)==0.,"close fade ends at 0");
    check(oskFadeAlpha(-5,true)==0. && oskFadeAlpha(-5,false)==1.,"a negative elapsed clamps");
    check(!oskFadeDone(oskOpenFadeMilliseconds-1,true) && oskFadeDone(oskOpenFadeMilliseconds,true),"open fade finishes at its duration");
    check(!oskFadeDone(oskCloseFadeMilliseconds-1,false) && oskFadeDone(oskCloseFadeMilliseconds,false),"close fade finishes at its duration");
    check(oskOpenFadeMilliseconds>=66 && oskOpenFadeMilliseconds<=96 && oskCloseFadeMilliseconds==104,"durations sit inside the audited ranges");

    std::printf("%d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
