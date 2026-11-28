#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>
#include <vector>
#include "bd_remote.h"

// ---- XInput wButtons layout, portable (audit/INPUT.md) ----
// The shell's navigation body (View::pollController) names these bits by their Xinput.h macro names. Windows
// includes <Xinput.h>; everywhere else this identical-value shim stands in, so the one navigation path serves
// every backend and platform with no duplicated logic. The values are the same wButtons layout pad.cpp applies
// HID reports into (its up/down/left/right/start/select/l3/r3/l1/r1/ps/cross/circle/square/triangle bits) and
// sdlButtonBit() maps SDL2 buttons onto, so PlayStation-HID, XInput and SDL2 controllers all arrive as the same
// bits. Mirrors Xinput.h's XINPUT_GAMEPAD_* set exactly (0x1..0x8000).
#if !defined(Q_OS_WIN)
#define XINPUT_GAMEPAD_DPAD_UP          0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN        0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT        0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT       0x0008
#define XINPUT_GAMEPAD_START            0x0010
#define XINPUT_GAMEPAD_BACK             0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB       0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB      0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER    0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER   0x0200
#define XINPUT_GAMEPAD_GUIDE            0x0400
#define XINPUT_GAMEPAD_A                0x1000
#define XINPUT_GAMEPAD_B                0x2000
#define XINPUT_GAMEPAD_X                0x4000
#define XINPUT_GAMEPAD_Y                0x8000
#endif

namespace xmb {
// PlayStation controllers read straight over HID: DualShock 3 (USB), DualShock 4 and DualSense (USB or Bluetooth),
// so no driver or Steam layer is needed. Ported from MaXMB (MIT, src-tauri/src/hidpad.rs): the same report layouts
// and start-up feature reports. A watcher thread looks for controllers every 2 s and gives each one a reader thread;
// the newest reading is kept here and goes stale after 300 ms.
namespace pad {
// Buttons in XInput's wButtons layout, so callers handle both kinds of controller alike:
// D-pad 0x1/0x2/0x4/0x8, START 0x10, SELECT 0x20, L3 0x40, R3 0x80, L1 0x100, R1 0x200, PS 0x400,
// Cross 0x1000, Circle 0x2000, Square 0x4000, Triangle 0x8000.
struct State { quint16 buttons=0; float lx=0,ly=0,rx=0,ry=0,l2=0,r2=0; };
// What is attached, how, and its battery as the report carries it (Phase 4.3, audit/SESSION-POWER.md).
enum class Kind { None, Ds3, Ds4, DualSense, BdRemote };
// The input report's own id names the transport, so it is read from the report rather than guessed from a device
// path: a DualSense sends 0x01 over USB and 0x31 over Bluetooth, a DualShock 4 sends 0x01 and 0x11. A DualShock 3
// in this build is USB only.
enum class Transport { Unknown, Usb, Bluetooth };
// batteryPercent is -1 when the report does not carry a level (DualShock 3), when the report is too short to hold
// the battery byte, or when the level field says "unknown" (a DualSense voltage/temperature/charging error).
// charging is "discharging", "charging", "full", "not-charging" or "unknown"; empty when there is no level.
struct Info { Kind kind=Kind::None; Transport transport=Transport::Unknown; int batteryPercent=-1; QString charging; };
// Starts the watcher once (Windows only; elsewhere a no-op).
void start();
// The newest reading from any PlayStation controller, if one reported in the last 300 ms.
// includeRemote=false leaves the BD remote's buttons out: the main shell takes them as held state plus press edges
// (takeRemotePad) so a tap shorter than one poll is not lost. Every other reader keeps the default.
bool latest(State& out,bool includeRemote=true);
// The BD remote's joypad-style buttons for the main shell (audit/BD-REMOTE.md, "Presses"): `held` is what the remote
// reports down now (it sends no repeat while a key is held, so down = press seen and no release yet; a release lost to
// a Bluetooth drop is forgotten after 30 s), and `pressed` every 0->1 transition since the last call, even one whose
// release arrived before the shell looked. Bits are XInput's wButtons layout; the triggers are flags (the remote's L2/R2
// are digital). Returns false when there is nothing held and nothing pressed, and always false (edges drained and
// counted in ignoredEvents()) while the scripted-run gate is on, unless `scripted` asks for the BDLIVE= latch instead.
struct RemotePad { quint16 held=0,pressed=0; bool l2Held=false,r2Held=false,l2Pressed=false,r2Pressed=false; };
bool takeRemotePad(RemotePad& out,bool scripted=false);
// A synthetic remote report (12 bytes, id 01 first) into the scripted latch above: the script token BDLIVE=<hex>.
bool injectScriptedBdReport(const QByteArray& report);
// ---- Scripted-run input gate (audit/PS-BUTTON.md) ----
// A scripted capture must be deterministic: the script drives and a live controller must not. A run is scripted
// when --script is present or XMB_INPUT_SCRIPT_ONLY=1; main.cpp calls setScriptOnly() once, before the View (and
// so before pad::start()) exists. While the gate is on, latest() reports NO live state at all: a D-pad
// direction, stick axis, trigger, button or PS bit from a physical controller is still parsed, observed and
// counted, but never delivered, so View::pollController() cannot merge it into navigation and PsWatcher cannot
// open the system menu with it. The controller itself keeps being read (info() still answers --pad-report and
// the battery line still logs); only its influence on the shell is removed. Off by default: with no --script and
// no XMB_INPUT_SCRIPT_ONLY, latest() behaves exactly as it did before this gate existed.
void setScriptOnly(bool on);
bool scriptOnly();
// Live pad samples withheld because the run is scripted: one per latest() call that saw a live state carrying a
// button bit, a trigger past 0.5 or a stick axis past the same 0.5 deadzone pollController uses. A scripted
// sidecar publishes it as padEventsIgnored, so a capture proves the pad was not steering it. Always 0 in a
// normal run. XMB_INPUT_TRACE=1 logs each decision this count is taken from, source=pad.
quint64 ignoredEvents();
// The identity, transport and battery of the controller behind latest(), on the same 300 ms freshness window.
// False when no controller reported recently; out is left untouched then.
bool info(Info& out);
// The PS3 BD Remote Control (Bluetooth HID 054C:0306, audit/BD-REMOTE.md). Its joypad-style keys and Return reach the
// shell as ordinary pad buttons through latest(); its media keys are one-shot player commands taken here, once per
// press, newest last. Commands older than 500 ms are dropped (the XMB was not listening), and while the scripted-run
// gate is on nothing is delivered: the commands are discarded and counted in ignoredEvents().
std::vector<bd::Action> takeRemoteActions();
// Every supported controller Windows currently lists as a HID device, by kind name, whether or not it has sent a
// report: a BD remote is silent until a key is pressed, so --pad-report uses this to say it is paired and visible.
QStringList presentDevices();
// ---- BD remote capture log (audit/BD-REMOTE.md, "Capturing the ZRC1") ----
// On when XMB_BD_REMOTE_LOG=1, when main.cpp calls setBdRemoteLog(true) (--bd-remote-log, or "bdRemoteLog": true in
// xmb-config.json) or when --pad-report runs. While on, every raw input report of a BD remote candidate is logged as
// hex ("bd-remote: report vid=... hex=..."), with "unknown key code 0xNN" for a key byte no table entry names, and every
// Sony (054C) HID interface Windows exposes is listed once. Only ids, the product string, usages and report bytes;
// never the device path (it holds the Bluetooth address). Set it before start().
void setBdRemoteLog(bool on);
bool bdRemoteLog();
// One line per Sony HID interface (vid, pid, product string, usage page/usage, input report length), recognised or not.
QStringList sonyInterfaces();
// The raw-report and interface lines logged so far (first 64), for --pad-report.
QStringList bdRemoteCapture();
// One input report through the same parser the watcher uses (report id in byte 0, as Windows returns it). The
// battery byte is read only when the report is long enough for the layout's own offset, so a short or unknown
// report is rejected instead of reading past its end. Local verification runs documented synthetic reports here.
bool parseReport(Kind kind,const QVector<quint8>& report,Info& out);
// The same parse, also returning the button/stick/trigger State the report carries (Controller mode's mapping tests feed
// documented synthetic USB and Bluetooth reports through it).
bool parseReportState(Kind kind,const QVector<quint8>& report,State& state,Info& out);
// ---- XInput (Xbox-layout) controllers, all four slots (PHASES H10, audit/INPUT.md) ----
// Slots 0-3 merged into one state: buttons OR'd (wButtons layout, as State), the stick and each trigger taken
// from whichever slot deflects it most. ly is +down, as the HID reports. A slot that reported nothing is
// re-probed at most once a second, because XInputGetState on an empty slot is slow. The Guide button is read
// through XInputGetStateEx (xinput1_4.dll ordinal 100; undocumented, so absent = no Guide) and reported as the
// PS bit 0x400, which latest() also merges so PsWatcher treats Guide exactly like a PlayStation PS button.
// Withheld (false) while the scripted-run gate is on, like latest().
struct XInputState { quint16 buttons=0; float lx=0,ly=0,rx=0,ry=0,l2=0,r2=0; int connected=0; };   // rx/ry: right stick, +down like ly (Desktop Mode scroll)
bool xinputLatest(XInputState& out);
// The connected XInput slots and their battery as XInputGetBatteryInformation reports it: battery is "wired",
// "full", "medium", "low", "empty" or "unknown" (the API reports four levels, not a percentage). Read on demand
// for the Accessory Settings rows; empty when no XInput controller is connected or xinput1_4.dll is absent.
struct XInputPad { int slot=0; QString battery; };
QVector<XInputPad> xinputPads();

// ---- On-screen keyboard pad mapping (BEHAVIOUR-GAPS-SPEC section 3) ----
// The pad buttons and triggers that drive the OSK directly, mapped here so the view dispatches semantics
// (caret / page / input mode) instead of XInput button bits: L1/R1 move the text caret, L2/R2 step between
// key pages, SELECT is Input Mode. `buttons` is the merged XInput wButtons layout (HID and XInput already
// agree on it); `l2`/`r2` are the analog trigger values, with the same 0.5 press threshold padAction uses
// for its trace. The caller edges the result (one step per press), exactly as it edges every other button.
enum OskPadBit : quint32 {
    OskPadNone       = 0x0,
    OskPadCaretLeft  = 0x1,   // L1 (XInput left shoulder / HID 0x100)
    OskPadCaretRight = 0x2,   // R1 (XInput right shoulder / HID 0x200)
    OskPadPagePrev   = 0x4,   // L2 analog trigger past 0.5 (accent page on the console)
    OskPadPageNext   = 0x8,   // R2 analog trigger past 0.5 (caps page on the console)
    OskPadMode       = 0x10   // SELECT (XInput back / HID 0x20)
};
quint32 oskPadActions(quint16 buttons, float l2, float r2);

QString kindName(Kind kind);          // "DualShock 3", "DualShock 4", "DualSense", "BD Remote", "None"
QString transportName(Transport kind); // "USB", "Bluetooth", "Unknown"

// ---- SDL2 GameController mapping (Linux backend, pad.cpp __linux__) ----
// One SDL_GameControllerButton constant (SDL_CONTROLLER_BUTTON_*, 0..20) to the wButtons bit that button
// means on this layout, so an SDL2 controller arrives exactly as the Windows XInput/HID paths deliver it.
// The numeric values are SDL2's public enum (stable across the 2.x series); the function is pure and works on
// every platform, which is what makes the mapping unit-testable without a controller. Returns 0 for an
// unmapped button (touchpad, paddles, misc). The axes need no table: LEFT/RIGHTX/Y scale to lx/ly/rx/ry and
// the two triggers to l2/r2, each by value/32767 (pad y is +down, exactly SDL's lefty/righty sign).
quint16 sdlButtonBit(quint8 sdlControllerButton);
}
}
