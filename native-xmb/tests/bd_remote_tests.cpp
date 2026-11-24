#include "bd_remote.h"
#include <cstdio>
#include <string>
#include <vector>

// The pure half of PS3 BD Remote support (src/bd_remote.h, audit/BD-REMOTE.md): the 12-byte report 0x01, the key
// table, the pad-bit mapping, the media-key actions and press detection. Synthetic reports only: no remote was
// available when this was written, so these prove the code against the documented layout, not against a device.
// Build with -DXMB_BD_REMOTE_BUILD_TESTS=ON.
using namespace xmb::bd;

static int failures=0;
static int checks=0;
static void check(bool ok,const char* what) {
    ++checks;
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}

// byte 4 is the remote key code, bytes 1-3 the joypad bits (little-endian 24-bit), byte 11 the battery.
static std::vector<std::uint8_t> report(std::uint8_t code,std::uint32_t joypad=0,std::uint8_t battery=5) {
    std::vector<std::uint8_t> r(12,0);
    r[0]=0x01;
    r[1]=std::uint8_t(joypad);r[2]=std::uint8_t(joypad>>8);r[3]=std::uint8_t(joypad>>16);
    r[4]=code;
    for(int i=5;i<=9;++i)r[i]=0xff;
    r[10]=code==0xff && joypad==0?0x00:0x01;
    r[11]=battery;
    return r;
}
static Report parsed(const std::vector<std::uint8_t>& r) {
    Report out;
    if(!parse(r.data(),r.size(),out))std::printf("(parse refused a report the test built)\n");
    return out;
}
static bool accepts(const std::vector<std::uint8_t>& r) {Report out;return parse(r.data(),r.size(),out);}

int main() {
    // ---- framing ----
    check(accepts(report(0xff)),"an idle 12-byte report 0x01 parses");
    {auto r=report(0xff);r.push_back(0);check(accepts(r),"a longer report parses (extra bytes are not read)");}
    {auto r=report(0xff);r[0]=0x02;check(!accepts(r),"another report id is rejected");}
    {auto r=report(0xff);r[0]=0x00;check(!accepts(r),"report id 0 is rejected");}
    {auto r=report(0xff);r.resize(11);check(!accepts(r),"an 11-byte report is rejected");}
    {auto r=report(0xff);r.resize(1);check(!accepts(r),"a one-byte report is rejected");}
    {Report out;check(!parse(nullptr,12,out),"a null buffer is rejected");}
    {std::vector<std::uint8_t> empty;Report out;check(!parse(empty.data(),0,out),"an empty report is rejected");}

    // ---- release and idle ----
    {
        const Report r=parsed(report(0xff));
        check(r.key==Key::None && r.joypad==0,"code ff with no bits is a release");
        const PadBits p=padBits(r);
        check(p.buttons==0 && !p.l2 && !p.r2,"a release holds no pad button");
        check(actionFor(r.key)==Action::None,"a release is no action");
    }

    // ---- every remote key code ----
    struct KeyCase { std::uint8_t code; Key key; const char* name; Action action; };
    const KeyCase keys[]={
        {0x00,Key::Num1,"1",Action::None},{0x01,Key::Num2,"2",Action::None},{0x02,Key::Num3,"3",Action::None},
        {0x03,Key::Num4,"4",Action::None},{0x04,Key::Num5,"5",Action::None},{0x05,Key::Num6,"6",Action::None},
        {0x06,Key::Num7,"7",Action::None},{0x07,Key::Num8,"8",Action::None},{0x08,Key::Num9,"9",Action::None},
        {0x09,Key::Num0,"0",Action::None},
        {0x0e,Key::Return,"Return",Action::None},{0x0f,Key::Clear,"Clear",Action::None},
        {0x16,Key::Eject,"Eject",Action::None},{0x1a,Key::TopMenu,"Top Menu",Action::None},
        {0x28,Key::Time,"Time",Action::Display},{0x30,Key::Prev,"Prev",Action::Previous},
        {0x31,Key::Next,"Next",Action::Next},{0x32,Key::Play,"Play",Action::Play},
        {0x33,Key::ScanBack,"Scan Back",Action::ScanBack},{0x34,Key::ScanForward,"Scan Forward",Action::ScanForward},
        {0x38,Key::Stop,"Stop",Action::Stop},{0x39,Key::Pause,"Pause",Action::Pause},
        {0x40,Key::PopUpMenu,"Pop Up/Menu",Action::Options},
        {0x60,Key::StepBack,"Step Back",Action::None},{0x61,Key::StepForward,"Step Forward",Action::None},
        {0x63,Key::Subtitle,"Subtitle",Action::Subtitle},{0x64,Key::Audio,"Audio",Action::Audio},
        {0x65,Key::Angle,"Angle",Action::None},{0x70,Key::Display,"Display",Action::Display},
        {0x80,Key::Blue,"Blue",Action::None},{0x81,Key::Red,"Red",Action::None},
        {0x82,Key::Green,"Green",Action::None},{0x83,Key::Yellow,"Yellow",Action::None}};
    bool codesOk=true,namesOk=true,actionsOk=true,padOk=true;
    for(const KeyCase& k:keys) {
        const Report r=parsed(report(k.code));
        codesOk&=r.key==k.key && r.code==k.code;
        namesOk&=std::string(keyName(r.key))==k.name;
        actionsOk&=actionFor(r.key)==k.action;
        const PadBits p=padBits(r);
        padOk&=p.buttons==(k.key==Key::Return?0x2000:0) && !p.l2 && !p.r2;   // only Return acts as a pad button
    }
    check(codesOk,"every documented key code decodes to its key");
    check(namesOk,"every key has its console label");
    check(actionsOk,"media keys map to player actions; keys the app has no command for map to none");
    check(padOk,"a remote key sets no pad button except Return (Circle)");
    check(parsed(report(0x20)).key==Key::Unknown && parsed(report(0x20)).code==0x20,"an unlisted code is Unknown, with its raw code kept");
    check(actionFor(Key::Unknown)==Action::None && std::string(keyName(Key::Unknown))=="unknown","an unknown key does nothing");
    check(parsed(report(0xfe)).key==Key::Unknown,"fe is not a release: only ff is");

    // ---- joypad-style bits ----
    struct PadCase { unsigned usage; std::uint16_t bit; const char* name; };
    const PadCase pads[]={
        {JpUp,0x0001,"Up"},{JpDown,0x0002,"Down"},{JpLeft,0x0004,"Left"},{JpRight,0x0008,"Right"},
        {JpStart,0x0010,"Start"},{JpSelect,0x0020,"Select"},{JpL3,0x0040,"L3"},{JpR3,0x0080,"R3"},
        {JpL1,0x0100,"L1"},{JpR1,0x0200,"R1"},{JpPs,0x0400,"PS"},
        {JpCross,0x1000,"Cross"},{JpEnter,0x1000,"Enter (is Cross)"},{JpCircle,0x2000,"Circle"},
        {JpSquare,0x4000,"Square"},{JpTriangle,0x8000,"Triangle"}};
    for(const PadCase& p:pads) {
        const Report r=parsed(report(0xff,joypadBit(p.usage)));
        const PadBits bits=padBits(r);
        char what[96];std::snprintf(what,sizeof(what),"joypad usage %u (%s) sets exactly its pad bit",p.usage,p.name);
        check(bits.buttons==p.bit && !bits.l2 && !bits.r2,what);
    }
    {
        const PadBits l2=padBits(parsed(report(0xff,joypadBit(JpL2))));
        const PadBits r2=padBits(parsed(report(0xff,joypadBit(JpR2))));
        check(l2.l2 && !l2.r2 && l2.buttons==0,"L2 is the left trigger only, no button bit");
        check(r2.r2 && !r2.l2 && r2.buttons==0,"R2 is the right trigger only, no button bit");
    }
    {   // the byte/bit placement itself, one bit per byte of the 24-bit field
        check(parsed(report(0xff,0x000001)).joypad==0x000001 && padBits(parsed(report(0xff,0x000001))).buttons==0x0020,"byte 1 bit 0 is Select");
        check(padBits(parsed(report(0xff,0x000010))).buttons==0x0001,"byte 1 bit 4 is Up");
        check(padBits(parsed(report(0xff,0x001000))).buttons==0x8000,"byte 2 bit 4 is Triangle");
        check(padBits(parsed(report(0xff,0x010000))).buttons==0x0400,"byte 3 bit 0 is PS");
        check(padBits(parsed(report(0xff,0x080000))).buttons==0x1000,"byte 3 bit 3 is Enter (Cross)");
        check(padBits(parsed(report(0xff,0x700000))).buttons==0,"byte 3 bits 4-6 are not mapped");
        check(parsed(report(0xff,0xffffff)).joypad==0xffffff,"all 24 bits survive parsing");
    }

    // ---- several things at once ----
    {
        const Report r=parsed(report(0x0e,joypadBit(JpUp)|joypadBit(JpRight)|joypadBit(JpL1)));
        const PadBits p=padBits(r);
        check(p.buttons==(0x0001|0x0008|0x0100|0x2000),"Up+Right+L1 with the Return key held: all four bits");
    }
    {
        const Report r=parsed(report(0xff,joypadBit(JpCross)|joypadBit(JpEnter)));
        check(padBits(r).buttons==0x1000,"Cross and Enter together are still one Cross");
    }
    {
        const Report r=parsed(report(0x32,joypadBit(JpDown)));
        check(r.key==Key::Play && padBits(r).buttons==0x0002 && actionFor(r.key)==Action::Play,"a joypad bit and a remote key in one report both count");
    }
    {
        const Report r=parsed(report(0xff,joypadBit(JpL2)|joypadBit(JpR2)));
        check(padBits(r).l2 && padBits(r).r2,"both triggers can be held");
    }

    // ---- battery ----
    check(parsed(report(0xff,0,0)).battery==0 && parsed(report(0xff,0,5)).battery==5 && parsed(report(0xff,0,3)).battery==3,"battery 0-5 is read from byte 11");
    check(parsed(report(0xff,0,6)).battery==-1 && parsed(report(0xff,0,0xff)).battery==-1,"a battery byte past 5 is not a level");

    // ---- press detection ----
    {
        Tracker t;
        check(t.press(parsed(report(0xff)))==Action::None,"idle reports do nothing");
        check(t.press(parsed(report(0x32)))==Action::Play,"Play fires on the first report");
        check(t.press(parsed(report(0x32)))==Action::None && t.press(parsed(report(0x32)))==Action::None,"the repeat while Play is held does not fire again");
        check(t.press(parsed(report(0xff)))==Action::None,"the release does not fire");
        check(t.press(parsed(report(0x32)))==Action::Play,"a second press after the release fires again");
        check(t.press(parsed(report(0x39)))==Action::Pause,"a different key without a release in between fires");
        check(t.press(parsed(report(0x00)))==Action::None,"a number key fires nothing");
        check(t.press(parsed(report(0x20)))==Action::None,"an unknown key fires nothing");
        check(t.press(parsed(report(0x34)))==Action::ScanForward,"Scan Forward after an unknown key fires");
        check(t.press(parsed(report(0xff,joypadBit(JpCross))))==Action::None,"a joypad press alone fires no action (it is a pad bit)");
        check(t.held==0xff,"the tracker follows the key byte, not the joypad bits");
    }
    {
        Tracker t;   // a lost release: the next different key still fires
        t.press(parsed(report(0x31)));
        check(t.press(parsed(report(0x30)))==Action::Previous,"Prev fires even though Next was never released");
    }

    // ---- recognising the CECH-ZRC1 (unknown product id) ----
    check(isCandidate(0x054c,0x0306,L""),"054C:0306 is a candidate with no product string");
    check(isCandidate(0x054c,0x1234,L"BD Remote Control"),"an unknown Sony pid named 'BD Remote Control' is a candidate");
    check(isCandidate(0x054c,0x1234,L"Remote Control"),"'Remote Control' alone is a candidate");
    check(isCandidate(0x054c,0x1234,L"PLAYSTATION(R)3 Remote"),"'PLAYSTATION' + 'Remote' is a candidate");
    check(isCandidate(0x054c,0x1234,L"bd remote"),"matching is case-insensitive");
    check(!isCandidate(0x054c,0x1234,L"PLAYSTATION(R)3 Controller"),"'PLAYSTATION' without 'Remote' is not");
    check(!isCandidate(0x054c,0x1234,L"Wireless Controller"),"a gamepad name is not");
    check(!isCandidate(0x054c,0x1234,L""),"an unknown pid with no name is not");
    check(!isCandidate(0x046d,0x0306,L"BD Remote"),"another vendor is never a candidate");
    {
        bool gamepadsOut=true;
        const unsigned short pids[]={0x0268,0x05c4,0x09cc,0x0ba0,0x0ce6,0x0df2};
        for(unsigned short pid:pids)gamepadsOut&=!isCandidate(0x054c,pid,L"BD Remote Control");   // even a misleading name
        check(gamepadsOut,"DualShock 3/4 and DualSense pids are never a BD remote, whatever the name");
    }
    check(productStringMatches(L"  Blu-ray Disc Remote Control  ") && !productStringMatches(L"DualSense Wireless Controller"),"product-string matching on realistic names");
    // shape
    check(looksLikeBdReport(report(0xff).data(),12),"an idle BD report has the BD shape");
    check(looksLikeBdReport(report(0x32,joypadBit(JpUp)).data(),12),"a key-down BD report has the BD shape");
    {auto r=report(0xff);r[7]=0x00;check(!looksLikeBdReport(r.data(),12),"a filler byte that is not ff breaks the shape");}
    {auto r=report(0xff);r.push_back(0);check(!looksLikeBdReport(r.data(),r.size()),"13 bytes is not the shape");}
    {auto r=report(0xff);r[0]=0x11;check(!looksLikeBdReport(r.data(),12),"another report id is not the shape");}
    {auto r=report(0xff);r[11]=9;check(!looksLikeBdReport(r.data(),12),"a battery byte past 5 is not the shape");}
    {
        // A DualShock 3 USB input report (49 bytes, id 01) and a DualSense one (64 bytes) must never match.
        std::vector<std::uint8_t> ds3(49,0x00);ds3[0]=0x01;
        std::vector<std::uint8_t> ds5(64,0xff);ds5[0]=0x01;
        check(!looksLikeBdReport(ds3.data(),ds3.size()) && !looksLikeBdReport(ds5.data(),ds5.size()),"DualShock 3 / DualSense reports are never BD-shaped");
        check(lengthRulesOutBdReport(49) && lengthRulesOutBdReport(64) && !lengthRulesOutBdReport(12) && !lengthRulesOutBdReport(0),"descriptor input lengths: 12 and unknown stay possible");
    }

    // ---- diagnostic lines ----
    check(hexBytes(report(0x32).data(),12)=="01 00 00 00 32 ff ff ff ff ff 01 05","raw report hex");
    check(hexBytes(nullptr,0).empty(),"hex of nothing is empty");
    check(unknownKeyNote(parsed(report(0x20)))=="unknown key code 0x20","an unlisted key code is reported as 'unknown key code 0xNN'");
    check(unknownKeyNote(parsed(report(0xfe)))=="unknown key code 0xfe","0xfe is reported too");
    check(unknownKeyNote(parsed(report(0x32))).empty() && unknownKeyNote(parsed(report(0xff))).empty(),"known keys and releases report nothing");
    {
        const auto r=report(0x77);
        const std::string line=rawReportLine(0x054c,0x0306,r.data(),r.size());
        check(line.find("vid=054c pid=0306 len=12 hex=01 00 00 00 77")!=std::string::npos && line.find("unknown key code 0x77")!=std::string::npos,"the raw line carries ids, hex and the unknown-code note");
        const auto k=report(0x32);
        check(rawReportLine(0x054c,0x0306,k.data(),k.size()).find("unknown")==std::string::npos,"a known key's raw line has no unknown note");
        std::vector<std::uint8_t> other(20,0x07);
        check(rawReportLine(0x054c,0x1234,other.data(),other.size()).find("len=20")!=std::string::npos,"a report of another shape is still logged raw");
    }
    check(describeInterface(0x054c,0x1234,L"BD Remote",0xff00,0x01,12)=="vid=054c pid=1234 usagePage=0xff00 usage=0x0001 inputReportLength=12 product=\"bd remote\"","interface description");

    // ---- Instant Back / Instant Forward slots (codes unknown) ----
    check(!kInstantCodes.back && !kInstantCodes.forward,"both Instant slots ship disabled: the codes are unknown");
    check(actionFor(Key::InstantBack)==Action::JumpBack && actionFor(Key::InstantForward)==Action::JumpForward,"the Instant keys map to jump back / forward");
    check(std::string(actionName(Action::JumpBack))=="jump-back" && std::string(actionName(Action::JumpForward))=="jump-forward" && kJumpMs==15000,"jump actions are named and 15 s");
    check(std::string(keyName(Key::InstantBack))=="Instant Back" && std::string(keyName(Key::InstantForward))=="Instant Forward","Instant keys have labels");
    check(parsed(report(0x42)).key==Key::Unknown,"with the slots disabled, a code is never an Instant key");
    {
        const InstantCodes filled{std::uint8_t(0x42),std::uint8_t(0x43)};   // what filling in the captured codes would look like
        Report back,fwd,other;
        const auto b=report(0x42),f=report(0x43),o=report(0x44);
        parse(b.data(),b.size(),back,filled);parse(f.data(),f.size(),fwd,filled);parse(o.data(),o.size(),other,filled);
        check(back.key==Key::InstantBack && fwd.key==Key::InstantForward && other.key==Key::Unknown,"a filled slot decodes its own code only");
        check(actionFor(back.key)==Action::JumpBack && actionFor(fwd.key)==Action::JumpForward,"a filled slot gives the jump action");
        check(unknownKeyNote(back).empty(),"a filled Instant code is no longer 'unknown'");
        check(padBits(back).buttons==0,"an Instant key sets no pad button");
        check(keyForCode(0x32,filled)==Key::Play && keyForCode(0xff,filled)==Key::None,"filling the slots leaves the known table alone");
        const InstantCodes onlyBack{std::uint8_t(0x42),std::nullopt};
        check(keyForCode(0x42,onlyBack)==Key::InstantBack && keyForCode(0x43,onlyBack)==Key::Unknown,"each slot is independent");
        Tracker t;
        check(t.press(back)==Action::JumpBack && t.press(back)==Action::None && t.press(parsed(report(0xff)))==Action::None && t.press(back)==Action::JumpBack,"an Instant press fires once, then again after a release");
    }
    // consumer usages (a guess pending capture)
    check(actionForConsumerUsage(0x0c,0x0224)==Action::JumpBack && actionForConsumerUsage(0x0c,0x0225)==Action::JumpForward,"consumer AC Back / AC Forward are the jump slots");
    check(actionForConsumerUsage(0x0c,0x00b5)==Action::None && actionForConsumerUsage(0x0c,0x00b6)==Action::None,"Scan Next/Previous are listed but disabled (Windows already maps them)");
    check(actionForConsumerUsage(0x01,0x0224)==Action::None && actionForConsumerUsage(0x0c,0x00cd)==Action::None,"another page or an unrelated usage maps to nothing");

    std::printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
