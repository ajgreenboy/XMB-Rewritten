#include "ps_pad_status.h"
#include <QCoreApplication>
#include <cstdio>

// The pure decisions behind the PS-menu controller panel, the PS-tap plate and the confirm hint (audit/BEHAVIOUR-GAPS-SPEC.md 1,
// FIDELITY-BEHAVIOUR P8). No window and no pad. Build with -DXMB_PS_PAD_STATUS_BUILD_TESTS=ON; exit code 0 = all pass.
using namespace xmb;

static int failures=0;
static int checks=0;
static void check(bool ok,const char* what) {
    ++checks;
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}
static pad::Info padInfo(pad::Kind kind,int battery,const char* charging="discharging") {
    pad::Info info;info.kind=kind;info.transport=pad::Transport::Usb;info.batteryPercent=battery;info.charging=QString::fromLatin1(charging);return info;
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);

    const PsPadPanel reference{0,QStringLiteral("1"),QStringLiteral("-")};
    check(psPadPanel(PsPadSource::Reference,false,{})==reference,"reference source keeps the console frame (slot 1 lit, 1, dash)");
    check(psPadPanel(PsPadSource::Reference,true,padInfo(pad::Kind::DualSense,80))==reference,"reference source ignores a live reading");

    const PsPadPanel none=psPadPanel(PsPadSource::Live,false,{});
    check(none.litSlot==-1 && none.controller==QStringLiteral("-") && none.charge==QStringLiteral("-"),"live, no pad: nothing lit, dash and dash");
    check(psPadPanel(PsPadSource::Live,true,{})==none,"live, attached flag with Kind::None: treated as no pad");

    PsPadPanel panel=psPadPanel(PsPadSource::Live,true,padInfo(pad::Kind::DualSense,75));
    check(panel.litSlot==0 && panel.controller==QStringLiteral("1") && panel.charge==QStringLiteral("75%"),"live DualSense 75%: slot 1 lit, 1, 75%");
    check(psPadPanel(PsPadSource::Live,true,padInfo(pad::Kind::Ds4,0)).charge==QStringLiteral("0%"),"0% prints 0%, not the dash");
    check(psPadPanel(PsPadSource::Live,true,padInfo(pad::Kind::Ds4,100,"full")).charge==QStringLiteral("100%"),"100% prints 100%");
    check(psPadPanel(PsPadSource::Live,true,padInfo(pad::Kind::Ds4,140)).charge==QStringLiteral("100%"),"an out-of-range level is clamped to 100%");
    panel=psPadPanel(PsPadSource::Live,true,padInfo(pad::Kind::Ds3,-1,""));
    check(panel.litSlot==0 && panel.controller==QStringLiteral("1") && panel.charge==QStringLiteral("-"),"DualShock 3 (no battery in the report): lit, 1, dash");
    check(psPadChargeText(padInfo(pad::Kind::DualSense,-5))==QStringLiteral("-"),"any negative level is the unknown dash");

    check(psConfirmHint(true)==QStringLiteral("X turns the PC off   O cancels"),"kiosk confirm hint says the PC turns off");
    check(!psConfirmHint(true).contains(QStringLiteral("stays on")),"kiosk confirm hint never says the PC stays on");
    check(psConfirmHint(false)==QStringLiteral("X turns the XMB off; the PC stays on   O cancels"),"non-kiosk confirm hint keeps the existing wording");

    std::printf("%d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
