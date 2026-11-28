#pragma once
// Controller mode (audit/CONTROLLER-MODE.md, spec 4.11): while a Steam title launched from the XMB runs, the XMB
// presents one virtual Xbox 360 pad whose state follows the physical DualSense, so Xbox-only games see a pad while
// the XMB keeps the PS button. Everything in this header is pure (no driver, no window, no Steam), so the mapping,
// the state machine and the lifecycle ordering are unit-tested in CI (tests/controller_mode_tests.cpp). The driver
// side lives behind the VirtualPad interface: FakePad here (tests and scripted runs) and ViGEmPad in
// controller_mode.cpp (the real ViGEmClient adapter, Windows builds only; elsewhere a truthful "unavailable" backend).
#include "pad.h"
#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace xmb::controller {

// ---- Xbox 360 (XUSB) report, the layout ViGEm and XInput share ----
// wButtons bits are XInput's XINPUT_GAMEPAD_* values. The names are this file's own so no platform header is needed.
namespace xbox {
constexpr quint16 DpadUp=0x0001,DpadDown=0x0002,DpadLeft=0x0004,DpadRight=0x0008,Start=0x0010,Back=0x0020,
    LeftThumb=0x0040,RightThumb=0x0080,LeftShoulder=0x0100,RightShoulder=0x0200,Guide=0x0400,
    A=0x1000,B=0x2000,X=0x4000,Y=0x8000;
}
struct Xbox360Report {
    quint16 buttons=0;
    quint8 leftTrigger=0,rightTrigger=0;
    qint16 thumbLX=0,thumbLY=0,thumbRX=0,thumbRY=0;
    bool operator==(const Xbox360Report& o) const {
        return buttons==o.buttons && leftTrigger==o.leftTrigger && rightTrigger==o.rightTrigger &&
            thumbLX==o.thumbLX && thumbLY==o.thumbLY && thumbRX==o.thumbRX && thumbRY==o.thumbRY;
    }
    bool operator!=(const Xbox360Report& o) const { return !(*this==o); }
};

// ---- Mapping (spec R-C4) ----
// pad::State carries the DualSense in XInput's wButtons layout: Cross/Circle/Square/Triangle are the bits of A/B/X/Y,
// L1/R1 of LB/RB, Create/Options of Back/Start, so the face, shoulder, stick-click and D-pad bits map one to one. The
// PS bit (0x400, the Xbox Guide position) is never forwarded, and neither is anything outside this list: the touchpad
// click and the mute button are not in pad::State at all, and an unknown bit is dropped, never passed through.
constexpr quint16 kForwardedButtons=xbox::DpadUp|xbox::DpadDown|xbox::DpadLeft|xbox::DpadRight|xbox::Start|xbox::Back|
    xbox::LeftThumb|xbox::RightThumb|xbox::LeftShoulder|xbox::RightShoulder|xbox::A|xbox::B|xbox::X|xbox::Y;
static_assert((kForwardedButtons&xbox::Guide)==0,"the PS/Guide bit must never be forwarded");

// A trigger 0..1 -> 0..255 (the DualSense reports a byte, pad.cpp divides it by 255, so rounding restores it exactly).
inline quint8 triggerByte(float v) {
    if(!(v>0.f))return 0;                       // also catches NaN
    if(v>=1.f)return 255;
    return quint8(std::lround(v*255.f));
}
// A stick axis -1..+1 -> the signed 16-bit range -32768..32767. The negative half scales by 32768 and the positive
// half by 32767, so a full deflection reaches both ends of the range and the centre stays exactly 0.
inline qint16 axis16(float v) {
    if(!(v==v))return 0;
    v=std::clamp(v,-1.f,1.f);
    return qint16(std::lround(v<0.f?v*32768.f:v*32767.f));
}
// The pad reports Y as +down (the HID report's own sign); an Xbox pad reports +up, so the two Y axes are inverted.
inline Xbox360Report mapState(const pad::State& s) {
    Xbox360Report r;
    r.buttons=quint16(s.buttons&kForwardedButtons);
    r.leftTrigger=triggerByte(s.l2);
    r.rightTrigger=triggerByte(s.r2);
    r.thumbLX=axis16(s.lx);
    r.thumbLY=axis16(-s.ly);
    r.thumbRX=axis16(s.rx);
    r.thumbRY=axis16(-s.ry);
    return r;
}

// ---- Backend ----
// What the driver adapter must provide. update() is called with the newest mapped report; it is cheap to call often
// (the session only calls it on a change). Rumble (phase 2): the backend calls the handler with the game's large and
// small motor values when the game sends them; nothing in phase 1 installs one.
class VirtualPad {
public:
    using RumbleHandler=std::function<void(quint8 largeMotor,quint8 smallMotor)>;
    virtual ~VirtualPad()=default;
    virtual QString backendName() const=0;
    virtual bool create(QString* why)=0;                           // false + reason: driver missing, access denied, no slot
    virtual bool update(const Xbox360Report& report,QString* why)=0;
    virtual void destroy()=0;                                      // idempotent
    virtual bool exists() const=0;
    void setRumbleHandler(RumbleHandler handler){rumble_=std::move(handler);}
protected:
    void deliverRumble(quint8 largeMotor,quint8 smallMotor) const {if(rumble_)rumble_(largeMotor,smallMotor);}
private:
    RumbleHandler rumble_;
};

// A pad that exists only in memory (and, when a log path is given, as lines in a file so a scripted run can be read
// back from another process): "create", "update <hex>", "destroy", "create-failed <reason>", each with the time in ms.
class FakePad final : public VirtualPad {
public:
    FakePad()=default;
    explicit FakePad(QString logPath):logPath_(std::move(logPath)){}
    QString backendName() const override { return QStringLiteral("fake"); }
    bool create(QString* why) override {
        if(!createFailure.isEmpty()){record(QStringLiteral("create-failed ")+createFailure);if(why)*why=createFailure;return false;}
        if(exists_){record(QStringLiteral("create-ignored"));return true;}
        exists_=true;++createCount;record(QStringLiteral("create"));return true;
    }
    bool update(const Xbox360Report& r,QString* why) override {
        if(!exists_){if(why)*why=QStringLiteral("no virtual pad");return false;}
        if(failUpdatesAfter>=0 && updateCount>=failUpdatesAfter){record(QStringLiteral("update-failed"));if(why)*why=QStringLiteral("update refused");return false;}
        ++updateCount;last=r;
        record(QStringLiteral("update %1 lt=%2 rt=%3 lx=%4 ly=%5 rx=%6 ry=%7").arg(r.buttons,4,16,QLatin1Char('0')).arg(r.leftTrigger).arg(r.rightTrigger)
            .arg(r.thumbLX).arg(r.thumbLY).arg(r.thumbRX).arg(r.thumbRY));
        return true;
    }
    void destroy() override {
        if(!exists_)return;
        exists_=false;++destroyCount;record(QStringLiteral("destroy"));
    }
    bool exists() const override { return exists_; }
    // Test hook: the game "sends" rumble to the pad.
    void simulateRumble(quint8 largeMotor,quint8 smallMotor){deliverRumble(largeMotor,smallMotor);}
    QStringList events;                 // "create", "update ...", "destroy", ... in order
    QString createFailure;              // non-empty: create() fails with this reason
    int failUpdatesAfter=-1;            // >=0: update() fails once this many updates were accepted
    int createCount=0,destroyCount=0,updateCount=0;
    Xbox360Report last;
private:
    void record(const QString& what){
        events.push_back(what);
        if(logPath_.isEmpty())return;
        QFile file(logPath_);
        if(file.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text))
            file.write((QString::number(QDateTime::currentMSecsSinceEpoch())+QLatin1Char(' ')+what+QLatin1Char('\n')).toUtf8());
    }
    QString logPath_;
    bool exists_=false;
};

// ---- State machine ----
// What the Controller Mode row says. Active is only ever reported while the virtual pad exists.
enum class Mode { Off, DriverMissing, Ready, Active, Error };
inline QString modeName(Mode m) {
    switch(m){
    case Mode::Off:return QStringLiteral("Off");
    case Mode::DriverMissing:return QStringLiteral("Driver missing");
    case Mode::Ready:return QStringLiteral("Ready");
    case Mode::Active:return QStringLiteral("Active");
    case Mode::Error:return QStringLiteral("Error");
    }
    return QStringLiteral("Off");
}
// Per-game override stored in the XMB overlay file: absent = Default (follow the global setting).
enum class Override { Default, Off, On };
inline QString overrideName(Override o){return o==Override::On?QStringLiteral("on"):o==Override::Off?QStringLiteral("off"):QString();}
inline Override overrideFromName(const QString& name){
    const QString n=name.trimmed().toLower();
    return n==QLatin1String("on")?Override::On:n==QLatin1String("off")?Override::Off:Override::Default;
}
inline bool effectiveEnabled(bool globalOn,Override o){return o==Override::On?true:o==Override::Off?false:globalOn;}

// One Steam title at a time. The session owns the lifecycle rules:
//   - the virtual pad is created in beginTitle(), which the caller runs BEFORE it starts the game;
//   - it is destroyed in endTitle(), which the caller runs when the game exits, the launch fails or the title is left;
//   - between titles (the desktop, the XMB navigating) nothing exists, whatever the setting says;
//   - a failed create never blocks the launch: beginTitle() returns normally and the mode becomes Error (or
//     DriverMissing) with the reason, and the caller launches the game anyway.
class Session {
public:
    explicit Session(VirtualPad* pad=nullptr):pad_(pad){}
    void setBackend(VirtualPad* pad){endTitle();pad_=pad;}
    // Input to the machine: is the driver there, and is the mode on for the title about to start (global setting
    // already combined with the per-game override by effectiveEnabled()).
    void setDriverPresent(bool present,const QString& reason=QString()){driverPresent_=present;driverReason_=reason;if(!padUp())refreshIdle();}
    void setEnabled(bool enabled){enabled_=enabled;if(!padUp())refreshIdle();}
    // Before the game starts. Never throws, never blocks the launch; returns whether the pad exists afterwards.
    bool beginTitle(bool enabledForTitle){
        enabled_=enabledForTitle;
        if(padUp())return true;                        // a second press for the title already running: nothing new
        if(!enabledForTitle){mode_=Mode::Off;reason_.clear();return false;}
        if(!driverPresent_||!pad_){mode_=Mode::DriverMissing;reason_=driverReason_.isEmpty()?QStringLiteral("The virtual gamepad driver is not installed."):driverReason_;return false;}
        QString why;
        if(!pad_->create(&why)){
            mode_=Mode::Error;reason_=why.isEmpty()?QStringLiteral("The virtual pad could not be created."):why;
            pad_->destroy();return false;
        }
        mode_=Mode::Active;reason_.clear();lastSent_=Xbox360Report();haveSent_=false;
        return true;
    }
    // The game exited, the launch failed or the title was left. Safe to call repeatedly and when nothing was started.
    void endTitle(){
        if(pad_)pad_->destroy();
        haveSent_=false;
        if(mode_==Mode::Error){return;}                // an Error stays visible until the next launch changes it
        refreshIdle();
    }
    // The newest physical state. Sends only while Active, and only a change. A refused update ends the session's
    // pad (the game keeps running) and reports Error with the reason.
    void feed(const pad::State& state){
        if(mode_!=Mode::Active||!pad_||!pad_->exists())return;
        const Xbox360Report report=mapState(state);
        if(haveSent_&&report==lastSent_)return;
        QString why;
        if(!pad_->update(report,&why)){
            pad_->destroy();mode_=Mode::Error;reason_=why.isEmpty()?QStringLiteral("The virtual pad stopped accepting input."):why;haveSent_=false;return;
        }
        lastSent_=report;haveSent_=true;
    }
    void feedNeutral(){feed(pad::State());}
    Mode mode() const {return mode_;}
    QString reason() const {return reason_;}
    bool padExists() const {return pad_&&pad_->exists();}
    bool driverPresent() const {return driverPresent_;}
    // The Controller Mode row's value line, truthful by construction: "Active" only when the pad exists.
    QString statusText() const {
        QString text=modeName(mode_);
        if((mode_==Mode::Error||mode_==Mode::DriverMissing)&&!reason_.isEmpty())text+=QStringLiteral(": ")+reason_;
        return text;
    }
private:
    bool padUp() const {return mode_==Mode::Active;}
    void refreshIdle(){
        if(!driverPresent_){mode_=Mode::DriverMissing;reason_=driverReason_.isEmpty()?QStringLiteral("The virtual gamepad driver is not installed."):driverReason_;}
        else if(!enabled_){mode_=Mode::Off;reason_.clear();}
        else {mode_=Mode::Ready;reason_.clear();}
    }
    VirtualPad* pad_=nullptr;
    bool driverPresent_=false,enabled_=true,haveSent_=false;
    Mode mode_=Mode::DriverMissing;
    QString reason_,driverReason_;
    Xbox360Report lastSent_;
};

// The lifecycle in one call, so the order is a property of one function: create the pad, THEN start the game; when
// the game does not start, the pad goes at once. `start` is the caller's launch (it returns whether the game started).
inline bool launchWithPad(Session& session,bool enabledForTitle,const std::function<bool()>& start) {
    session.beginTitle(enabledForTitle);
    const bool started=start?start():false;
    if(!started)session.endTitle();
    return started;
}

// ---- Driver and Steam Input status (controller_mode.cpp) ----
enum class DriverState { Missing, Stopped, Running, Unknown };
struct DriverStatus { DriverState state=DriverState::Unknown; QString detail; };
DriverStatus queryDriver();                       // read-only service query; works without the client library
// Steam Input for PlayStation controllers, read-only. Today always Unknown (see steamInputStatus() for why).
enum class SteamInput { Unknown, On, Off };
struct SteamInputStatus { SteamInput state=SteamInput::Unknown; QString detail; };
SteamInputStatus steamInputStatus(const QString& steamRoot,const QString& appId);
// The backend this build can really create: a "backend not built" stub, or the fake when asked for.
std::unique_ptr<VirtualPad> makeBackend(const QString& which=QString());
bool killSwitchEngaged();                         // XMB_CONTROLLER_MODE=0
QString rowDetail(const DriverStatus& driver,const SteamInputStatus& steamInput);

// ---- Bundled driver installer (spec R-C9, bundled) ----
// The ViGEmBus installer ships beside the exe under drivers\ (packaging/vigem-driver.json pins it). The XMB never
// downloads it at run time and never runs elevated itself: installDriver() verifies the pinned size, SHA-256 and
// Authenticode signer immediately before it hands the file to ShellExecute "runas" (the installer alone is elevated).
struct DriverInstaller { QString fileName,version,sha256,signer; qint64 size=0; };
const DriverInstaller& pinnedInstaller();         // compiled-in pin; must equal packaging/vigem-driver.json (a test checks)
QString bundledInstallerPath(const QString& appDirectory);   // empty when drivers\<file> is absent (a dev build)
bool verifyInstaller(const QString& path,QString* why);
bool installDriver(const QString& appDirectory,QString* why); // verify, then run the installer through UAC; false + reason

}
