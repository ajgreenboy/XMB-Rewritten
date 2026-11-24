#pragma once
// PS3 BD Remote Control (Sony, Bluetooth HID, 054C:0306): the pure half. Header-only and free of Qt, Windows and the
// pad reader, so tests/bd_remote_tests.cpp covers it with synthetic reports and no device (audit/BD-REMOTE.md).
//
// Report layout, as Windows returns an input report (byte 0 = report id). Facts from the Linux kernel's hid-sony.c
// PS3 BD remote descriptor and key tables (read for facts only; nothing is copied) - see the audit doc:
//   [0]      report id 0x01 (the descriptor Linux substitutes calls this byte a constant)
//   [1..3]   24 joypad-style bits: bit n-1 of the little-endian 24-bit value is button usage n (Select, L3, R3, Start,
//            Up, Right, Down, Left, L2, R2, L1, R1, Triangle, Circle, Cross, Square, PS, ..., Enter at usage 20)
//   [4]      remote key code (array); 0xFF = no remote key held
//   [5..10]  constant, 0xFF in bytes 5-9; byte 10 marks a press
//   [11]     battery strength 0-5
// Real-device behaviour is UNVERIFIED on this machine (no remote was available); the audit doc lists what to check.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace xmb::bd {

constexpr unsigned short kVendorId=0x054c;
constexpr unsigned short kProductId=0x0306;
constexpr std::size_t kReportSize=12;
constexpr std::uint8_t kReportId=0x01;
constexpr std::uint8_t kNoKey=0xff;

// Joypad usage numbers (1-based) of the 24-bit field.
enum Joypad : unsigned {
    JpSelect=1,JpL3=2,JpR3=3,JpStart=4,JpUp=5,JpRight=6,JpDown=7,JpLeft=8,JpL2=9,JpR2=10,JpL1=11,JpR1=12,
    JpTriangle=13,JpCircle=14,JpCross=15,JpSquare=16,JpPs=17,JpEnter=20
};
constexpr std::uint32_t joypadBit(unsigned usage){return std::uint32_t(1)<<(usage-1);}

// Remote key codes in byte 4 (hid-sony's remote table), named the way the PS3 BD remote is labelled.
enum class Key : std::uint8_t {
    None,
    Num1,Num2,Num3,Num4,Num5,Num6,Num7,Num8,Num9,Num0,
    Return,Clear,Eject,TopMenu,Time,Prev,Next,Play,ScanBack,ScanForward,Stop,Pause,PopUpMenu,
    StepBack,StepForward,Subtitle,Audio,Angle,Display,Blue,Red,Green,Yellow,
    InstantBack,InstantForward,   // CECH-ZRC1 only (kInstantCodes below)
    ButtonCode,   // the code the remote sends alongside a joypad button bit (arrows, Enter, shapes, PS): the bit acts, not this
    Unknown   // a code no table entry names: held like any key, does nothing
};

// ---- CECH-ZRC1 Instant Back / Instant Forward: THE ONE PLACE TO FILL IN THE CAPTURED CODES ----
// The 2011 Media/Blu-ray Disc Remote (CECH-ZRC1) has two Bluetooth keys the original remote lacks. Sony's manual says
// they jump back/forward 15 seconds. CAPTURED on a real CECH-ZRC1 (2026-11-23, audit/BD-REMOTE.md): Instant Back sends
// key code 0x76 and Instant Forward 0x75, with no joypad bits, released with 0xff like every other key.
struct InstantCodes { std::optional<std::uint8_t> back,forward; };
constexpr InstantCodes kInstantCodes{std::optional<std::uint8_t>{0x76},std::optional<std::uint8_t>{0x75}};

inline Key keyForCode(std::uint8_t code,const InstantCodes& instant=kInstantCodes) {
    if(code==kNoKey)return Key::None;
    if(instant.back && code==*instant.back)return Key::InstantBack;
    if(instant.forward && code==*instant.forward)return Key::InstantForward;
    if(code<=0x08)return Key(unsigned(Key::Num1)+code);   // codes 0x00-0x08 are the keys 1-9
    switch(code) {
    case 0x09:return Key::Num0;
    // Captured on a real CECH-ZRC1: these codes arrive together with a joypad button bit (Enter 0x0b, PS 0x43,
    // Up/Right/Down/Left 0x54-0x57, Triangle/Circle/Cross/Square 0x5c-0x5f). The bit drives the action.
    case 0x0b:case 0x43:case 0x54:case 0x55:case 0x56:case 0x57:case 0x5c:case 0x5d:case 0x5e:case 0x5f:return Key::ButtonCode;
    case 0x0e:return Key::Return;
    case 0x0f:return Key::Clear;
    case 0x16:return Key::Eject;
    case 0x1a:return Key::TopMenu;
    case 0x28:return Key::Time;
    case 0x30:return Key::Prev;
    case 0x31:return Key::Next;
    case 0x32:return Key::Play;
    case 0x33:return Key::ScanBack;
    case 0x34:return Key::ScanForward;
    case 0x38:return Key::Stop;
    case 0x39:return Key::Pause;
    case 0x40:return Key::PopUpMenu;
    case 0x60:return Key::StepBack;
    case 0x61:return Key::StepForward;
    case 0x63:return Key::Subtitle;
    case 0x64:return Key::Audio;
    case 0x65:return Key::Angle;
    case 0x70:return Key::Display;
    case 0x80:return Key::Blue;
    case 0x81:return Key::Red;
    case 0x82:return Key::Green;
    case 0x83:return Key::Yellow;
    default:return Key::Unknown;
    }
}

inline const char* keyName(Key key) {
    static const char* const numbers[]={"1","2","3","4","5","6","7","8","9","0"};
    if(key>=Key::Num1 && key<=Key::Num0)return numbers[unsigned(key)-unsigned(Key::Num1)];
    switch(key) {
    case Key::Return:return "Return";
    case Key::Clear:return "Clear";
    case Key::Eject:return "Eject";
    case Key::TopMenu:return "Top Menu";
    case Key::Time:return "Time";
    case Key::Prev:return "Prev";
    case Key::Next:return "Next";
    case Key::Play:return "Play";
    case Key::ScanBack:return "Scan Back";
    case Key::ScanForward:return "Scan Forward";
    case Key::Stop:return "Stop";
    case Key::Pause:return "Pause";
    case Key::PopUpMenu:return "Pop Up/Menu";
    case Key::StepBack:return "Step Back";
    case Key::StepForward:return "Step Forward";
    case Key::Subtitle:return "Subtitle";
    case Key::Audio:return "Audio";
    case Key::Angle:return "Angle";
    case Key::Display:return "Display";
    case Key::Blue:return "Blue";
    case Key::Red:return "Red";
    case Key::Green:return "Green";
    case Key::Yellow:return "Yellow";
    case Key::InstantBack:return "Instant Back";
    case Key::InstantForward:return "Instant Forward";
    case Key::ButtonCode:return "button";
    case Key::Unknown:return "unknown";
    default:return "none";
    }
}

struct Report {
    std::uint32_t joypad=0;     // 24 bits, bit n-1 = usage n
    std::uint8_t code=kNoKey;   // raw byte 4
    Key key=Key::None;
    int battery=-1;             // 0-5, or -1 when byte 11 is outside that range
};

// One input report in, with the report id in byte 0. Rejects what is not the 12-byte report 0x01: a short report
// (nothing past its end is ever read), a different report id, or no data.
inline bool parse(const std::uint8_t* data,std::size_t size,Report& out,const InstantCodes& instant=kInstantCodes) {
    out=Report();
    if(!data || size<kReportSize || data[0]!=kReportId)return false;
    out.joypad=std::uint32_t(data[1])|(std::uint32_t(data[2])<<8)|(std::uint32_t(data[3])<<16);
    out.code=data[4];
    out.key=keyForCode(data[4],instant);
    out.battery=data[11]<=5?int(data[11]):-1;
    return true;
}

// What the shell sees as held: XInput's wButtons layout (the pad's own, see pad.h), so the XMB, its panels and the
// pad's key-repeat treat the remote exactly like a controller. Enter is Cross and the Return key is Circle; the
// remote's own Triangle/Square/Circle/Cross keys are those buttons. L2/R2 are digital here, so they read 1.0.
struct PadBits { std::uint16_t buttons=0; bool l2=false,r2=false; };
inline PadBits padBits(const Report& r) {
    struct Map { unsigned usage; std::uint16_t bit; };
    static const Map map[]={
        {JpUp,0x0001},{JpDown,0x0002},{JpLeft,0x0004},{JpRight,0x0008},{JpStart,0x0010},{JpSelect,0x0020},
        {JpL3,0x0040},{JpR3,0x0080},{JpL1,0x0100},{JpR1,0x0200},{JpPs,0x0400},
        {JpCross,0x1000},{JpEnter,0x1000},{JpCircle,0x2000},{JpSquare,0x4000},{JpTriangle,0x8000}};
    PadBits out;
    for(const Map& m:map)if(r.joypad&joypadBit(m.usage))out.buttons|=m.bit;
    if(r.key==Key::Return)out.buttons|=0x2000;
    out.l2=(r.joypad&joypadBit(JpL2))!=0;
    out.r2=(r.joypad&joypadBit(JpR2))!=0;
    return out;
}

// The player commands the remote's media keys stand for. One per press (see Tracker); the shell decides whether the
// open surface takes it, exactly as it does for a Windows media key.
enum class Action : std::uint8_t {
    None,Play,Pause,Stop,Next,Previous,ScanForward,ScanBack,Options,Display,Subtitle,Audio,
    JumpBack,JumpForward   // Instant Back / Instant Forward: seek -/+ kJumpMs in the open player
};
constexpr int kJumpMs=15000;   // Sony's manual: the ZRC1's Instant keys jump 15 seconds
inline Action actionFor(Key key) {
    switch(key) {
    case Key::Play:return Action::Play;
    case Key::Pause:return Action::Pause;
    case Key::Stop:return Action::Stop;
    case Key::Next:return Action::Next;
    case Key::Prev:return Action::Previous;
    case Key::ScanForward:return Action::ScanForward;
    case Key::ScanBack:return Action::ScanBack;
    case Key::PopUpMenu:return Action::Options;   // the options panel, the pad's Triangle
    case Key::Display:case Key::Time:return Action::Display;
    case Key::Subtitle:return Action::Subtitle;
    case Key::Audio:return Action::Audio;
    case Key::InstantBack:return Action::JumpBack;
    case Key::InstantForward:return Action::JumpForward;
    default:return Action::None;   // numbers, Clear, Eject, Top Menu, Angle, Step, colours: the app has no command
    }
}
inline const char* actionName(Action a) {
    switch(a) {
    case Action::Play:return "play";case Action::Pause:return "pause";case Action::Stop:return "stop";
    case Action::Next:return "next";case Action::Previous:return "previous";
    case Action::ScanForward:return "scan-forward";case Action::ScanBack:return "scan-back";
    case Action::Options:return "options";case Action::Display:return "display";
    case Action::Subtitle:return "subtitle";case Action::Audio:return "audio";
    case Action::JumpBack:return "jump-back";case Action::JumpForward:return "jump-forward";
    default:return "none";
    }
}

// Press detection for the remote's key byte: the action fires when a key appears or changes, never while it is
// held (the remote repeats its report while a key is down), and the release (0xFF) re-arms it. One Tracker per
// device.
struct Tracker {
    std::uint8_t held=kNoKey;
    Action press(const Report& r) {
        if(r.code==held)return Action::None;
        held=r.code;
        return r.code==kNoKey?Action::None:actionFor(r.key);
    }
};

// ---- Recognising the remote (CECH-ZR1 054C:0306 and the CECH-ZRC1, whose product id is not public) ----
// A Sony HID interface is a BD remote candidate when it is 054C:0306, or when its product string names a remote. A
// candidate found by name is only ever TRUSTED once a real input report has the BD remote's shape (looksLikeBdReport),
// so a DualShock or DualSense (known product ids are excluded, and their reports are 49-78 bytes) is never taken.
inline bool isGamepadProduct(unsigned short pid) {
    return pid==0x0268||pid==0x05c4||pid==0x09cc||pid==0x0ba0||pid==0x0ce6||pid==0x0df2;   // DS3, DS4 x3, DualSense x2
}
inline std::string foldAscii(std::wstring_view text) {
    std::string out;
    for(wchar_t c:text)out.push_back(c>=L'A'&&c<=L'Z'?char(c-L'A'+'a'):(c>0&&c<0x80?char(c):'?'));
    return out;
}
// Case-insensitive: "BD Remote", "Remote Control", or "PLAYSTATION" together with "Remote".
inline bool productStringMatches(std::wstring_view product) {
    const std::string s=foldAscii(product);
    const auto has=[&](const char* word){return s.find(word)!=std::string::npos;};
    return has("bd remote")||has("remote control")||(has("playstation")&&has("remote"));
}
inline bool isCandidate(unsigned short vendor,unsigned short product,std::wstring_view productName) {
    if(vendor!=kVendorId||isGamepadProduct(product))return false;
    return product==kProductId||productStringMatches(productName);
}
// The shape of a BD remote input report as Windows returns it: exactly 12 bytes, report id 0x01, the five constant
// 0xFF filler bytes 5-9, a press marker (0/1) at byte 10 and a battery level 0-5 at byte 11 (the layout above).
inline bool looksLikeBdReport(const std::uint8_t* data,std::size_t size) {
    if(!data||size!=kReportSize||data[0]!=kReportId)return false;
    for(std::size_t i=5;i<=9;++i)if(data[i]!=0xff)return false;
    return data[10]<=1&&data[11]<=5;
}
// An input-report length from the HID descriptor that cannot be the BD remote's (0 = unknown, so no verdict).
inline bool lengthRulesOutBdReport(std::size_t inputReportLength) {
    return inputReportLength!=0&&inputReportLength!=kReportSize;
}

// ---- Standard HID consumer usages (Consumer page 0x0C): a GUESS pending capture ----
// If the ZRC1 exposes Instant Back/Forward on a separate consumer collection instead of in byte 4, they would arrive
// as usages. Which usage is unknown; AC Back/AC Forward are the likeliest guess. Scan Previous/Next are listed but
// disabled: Windows already turns them into the media Next/Previous keys, so mapping them here would double-act.
constexpr unsigned short kConsumerPage=0x0c;
struct ConsumerSlot { unsigned short usage; Action action; bool enabled; const char* name; };
constexpr ConsumerSlot kConsumerSlots[]={
    {0x0224,Action::JumpBack,true,"AC Back"},{0x0225,Action::JumpForward,true,"AC Forward"},
    {0x00b6,Action::JumpBack,false,"Scan Previous Track"},{0x00b5,Action::JumpForward,false,"Scan Next Track"}};
inline Action actionForConsumerUsage(unsigned short page,unsigned short usage) {
    if(page!=kConsumerPage)return Action::None;
    for(const ConsumerSlot& s:kConsumerSlots)if(s.enabled&&s.usage==usage)return s.action;
    return Action::None;
}

// ---- Diagnostic capture lines (XMB_BD_REMOTE_LOG=1 / --bd-remote-log / "bdRemoteLog" in xmb-config.json) ----
// Only hardware facts: ids, product string, usages, bytes. No device path (it holds the Bluetooth address).
inline std::string hexBytes(const std::uint8_t* data,std::size_t size) {
    std::string out;
    char buf[4];
    for(std::size_t i=0;data&&i<size;++i){std::snprintf(buf,sizeof(buf),"%02x",data[i]);if(i)out+=' ';out+=buf;}
    return out;
}
// "unknown key code 0xNN" when the report's key byte is a code no table entry names; empty otherwise.
inline std::string unknownKeyNote(const Report& r) {
    if(r.key!=Key::Unknown)return {};
    char buf[40];std::snprintf(buf,sizeof(buf),"unknown key code 0x%02x",unsigned(r.code));
    return buf;
}
inline std::string describeInterface(unsigned short vid,unsigned short pid,std::wstring_view product,
                                     unsigned short usagePage,unsigned short usage,std::size_t inputLength) {
    char buf[160];
    std::snprintf(buf,sizeof(buf),"vid=%04x pid=%04x usagePage=0x%04x usage=0x%04x inputReportLength=%zu product=\"",
        unsigned(vid),unsigned(pid),unsigned(usagePage),unsigned(usage),inputLength);
    return std::string(buf)+foldAscii(product)+"\"";
}
inline std::string rawReportLine(unsigned short vid,unsigned short pid,const std::uint8_t* data,std::size_t size,
                                 const InstantCodes& instant=kInstantCodes) {
    char buf[96];
    std::snprintf(buf,sizeof(buf),"report vid=%04x pid=%04x len=%zu hex=",unsigned(vid),unsigned(pid),size);
    std::string line=std::string(buf)+hexBytes(data,size);
    Report r;
    if(parse(data,size,r,instant)){const std::string note=unknownKeyNote(r);if(!note.empty())line+="; "+note;}
    return line;
}

}   // namespace xmb::bd
