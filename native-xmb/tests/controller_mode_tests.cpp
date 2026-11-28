#include "controller_mode.h"
#include "steam_collections.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

// Controller mode (audit/CONTROLLER-MODE.md): the DualSense -> Xbox 360 mapping (fed through pad.cpp's own report
// parser, USB report 0x01 and Bluetooth 0x31), the state machine, the lifecycle ordering and the per-game override, all
// against the fake backend. No driver, no Steam, no window. Build with -DXMB_CONTROLLER_MODE_BUILD_TESTS=ON.
//   xmb-controller-mode-tests.exe [scratch-dir] [packaging/vigem-driver.json] [vendor installer to verify for real]
using namespace xmb;
using namespace xmb::controller;

static int failures=0,checks=0;
static void check(bool ok,const char* what) {
    ++checks;
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what);
    if(!ok)++failures;
}

// A documented synthetic DualSense input report. Payload (after the report id, and after one more byte over Bluetooth):
// 0 lx, 1 ly, 2 rx, 3 ry, 4 l2, 5 r2, 6 counter, 7 hat+faces, 8 shoulders/sticks/create/options, 9 ps/touchpad/mute.
static QVector<quint8> dsReport(bool bluetooth,quint8 lx,quint8 ly,quint8 rx,quint8 ry,quint8 l2,quint8 r2,quint8 b7,quint8 b8,quint8 b9) {
    QVector<quint8> r;
    if(bluetooth){r.push_back(0x31);r.push_back(0x01);} else r.push_back(0x01);
    const quint8 payload[10]={lx,ly,rx,ry,l2,r2,0,b7,b8,b9};
    for(const quint8 b:payload)r.push_back(b);
    for(int i=0;i<54;++i)r.push_back(0);   // the rest of the report (motion, touch, battery) is zero
    return r;
}
static pad::State parsed(bool bluetooth,quint8 lx,quint8 ly,quint8 rx,quint8 ry,quint8 l2,quint8 r2,quint8 b7,quint8 b8,quint8 b9) {
    pad::State s;pad::Info info;
    const bool ok=pad::parseReportState(pad::Kind::DualSense,dsReport(bluetooth,lx,ly,rx,ry,l2,r2,b7,b8,b9),s,info);
    if(!ok)std::printf("FAIL  report did not parse\n");
    if(ok)check(info.transport==(bluetooth?pad::Transport::Bluetooth:pad::Transport::Usb),"the report id names the transport");
    return s;
}
static Xbox360Report mapped(bool bluetooth,quint8 lx,quint8 ly,quint8 rx,quint8 ry,quint8 l2,quint8 r2,quint8 b7,quint8 b8,quint8 b9) {
    return mapState(parsed(bluetooth,lx,ly,rx,ry,l2,r2,b7,b8,b9));
}

static void mappingTests() {
    // Neutral report: sticks at 128, hat 8 (released), nothing pressed.
    for(const bool bt:{false,true}) {
        const char* t=bt?"Bluetooth":"USB";
        const auto r=mapped(bt,128,128,128,128,0,0,0x08,0,0);
        char label[160];
        std::snprintf(label,sizeof label,"%s: a resting pad maps to an all-zero Xbox report",t);
        check(r==Xbox360Report(),label);
        // Face buttons: Square 0x10 -> X, Cross 0x20 -> A, Circle 0x40 -> B, Triangle 0x80 -> Y.
        struct B{quint8 b7;quint16 xbox;const char* name;};
        for(const B& b:{B{0x10,xbox::X,"Square -> X"},B{0x20,xbox::A,"Cross -> A"},B{0x40,xbox::B,"Circle -> B"},B{0x80,xbox::Y,"Triangle -> Y"}}) {
            const auto m=mapped(bt,128,128,128,128,0,0,quint8(0x08|b.b7),0,0);
            std::snprintf(label,sizeof label,"%s: %s and nothing else",t,b.name);
            check(m.buttons==b.xbox,label);
        }
        for(const B& b:{B{0x01,xbox::LeftShoulder,"L1 -> LB"},B{0x02,xbox::RightShoulder,"R1 -> RB"},B{0x10,xbox::Back,"Create -> Back"},
                        B{0x20,xbox::Start,"Options -> Start"},B{0x40,xbox::LeftThumb,"L3 -> L3"},B{0x80,xbox::RightThumb,"R3 -> R3"}}) {
            const auto m=mapped(bt,128,128,128,128,0,0,0x08,b.b7,0);
            std::snprintf(label,sizeof label,"%s: %s and nothing else",t,b.name);
            check(m.buttons==b.xbox,label);
        }
        // D-pad hat: 0 N, 1 NE, 2 E, 3 SE, 4 S, 5 SW, 6 W, 7 NW, 8 released.
        const quint16 hat[9]={xbox::DpadUp,quint16(xbox::DpadUp|xbox::DpadRight),xbox::DpadRight,quint16(xbox::DpadRight|xbox::DpadDown),
            xbox::DpadDown,quint16(xbox::DpadDown|xbox::DpadLeft),xbox::DpadLeft,quint16(xbox::DpadLeft|xbox::DpadUp),0};
        bool allHats=true;
        for(int h=0;h<9;++h)allHats=allHats && mapped(bt,128,128,128,128,0,0,quint8(h),0,0).buttons==hat[h];
        std::snprintf(label,sizeof label,"%s: every D-pad hat value (8 directions and released) maps to the Xbox D-pad bits",t);
        check(allHats,label);
        // PS, touchpad click and Mute are never forwarded.
        std::snprintf(label,sizeof label,"%s: PS, touchpad click and Mute (byte 9 bits 0..2) forward nothing",t);
        check(mapped(bt,128,128,128,128,0,0,0x08,0,0x07).buttons==0,label);
        // Everything at once keeps all fourteen forwarded bits and still drops PS.
        const auto all=mapped(bt,128,128,128,128,0,0,0xF8,0xF3,0x07);
        std::snprintf(label,sizeof label,"%s: every forwarded button together maps to the full Xbox mask without the Guide bit",t);
        check(all.buttons==quint16(xbox::A|xbox::B|xbox::X|xbox::Y|xbox::LeftShoulder|xbox::RightShoulder|xbox::Back|xbox::Start|xbox::LeftThumb|xbox::RightThumb),label);
        check((all.buttons&xbox::Guide)==0,"the Guide bit is clear");
        // Sticks: byte 0 = full left/up, 128 = centre, 255 = full right/down.
        const auto lo=mapped(bt,0,0,0,0,0,0,0x08,0,0),mid=mapped(bt,128,128,128,128,0,0,0x08,0,0),hi=mapped(bt,255,255,255,255,0,0,0x08,0,0);
        std::snprintf(label,sizeof label,"%s: X axes run -32768 (left) .. 0 .. 32767 (right) on both sticks",t);
        check(lo.thumbLX==-32768&&lo.thumbRX==-32768&&mid.thumbLX==0&&mid.thumbRX==0&&hi.thumbLX==32767&&hi.thumbRX==32767,label);
        std::snprintf(label,sizeof label,"%s: Y axes are inverted (pad up = byte 0 = +32767; pad down = byte 255 = -32768)",t);
        check(lo.thumbLY==32767&&lo.thumbRY==32767&&mid.thumbLY==0&&mid.thumbRY==0&&hi.thumbLY==-32768&&hi.thumbRY==-32768,label);
        // Triggers: analog 0..255 straight through.
        const auto tr=mapped(bt,128,128,128,128,0,255,0x08,0,0),tr2=mapped(bt,128,128,128,128,255,0,0x08,0,0),trm=mapped(bt,128,128,128,128,128,64,0x08,0,0);
        std::snprintf(label,sizeof label,"%s: L2/R2 map to LT/RT 0..255, analog values preserved",t);
        check(tr.leftTrigger==0&&tr.rightTrigger==255&&tr2.leftTrigger==255&&tr2.rightTrigger==0&&trm.leftTrigger==128&&trm.rightTrigger==64,label);
    }
    // USB and Bluetooth reports carrying the same data map to the same Xbox report (offsets differ by one byte).
    const auto usb=mapped(false,17,200,90,251,33,222,0x2A,0xC3,0x01),bt=mapped(true,17,200,90,251,33,222,0x2A,0xC3,0x01);
    check(usb==bt,"USB report 0x01 and Bluetooth report 0x31 with the same data give the identical Xbox report");
    check(usb.thumbLX!=0&&usb.thumbLY!=0&&usb.leftTrigger==33&&usb.rightTrigger==222,"the mixed report is not trivially zero");

    // Direct State mapping edge cases.
    pad::State s;s.buttons=0x0400;
    check(mapState(s).buttons==0,"the PS bit (0x400) alone is dropped");
    s.buttons=0x0800|0x0400|0x1000;
    check(mapState(s).buttons==xbox::A,"an unknown bit is dropped, never passed through");
    s=pad::State();s.lx=-1;s.ly=-1;s.rx=1;s.ry=1;s.l2=1;s.r2=1;
    auto m=mapState(s);
    check(m.thumbLX==-32768&&m.thumbLY==32767&&m.thumbRX==32767&&m.thumbRY==-32768&&m.leftTrigger==255&&m.rightTrigger==255,"float extremes map to the exact 16-bit and 8-bit ends");
    s=pad::State();s.lx=2.f;s.ly=-3.f;s.l2=5.f;s.r2=-1.f;
    m=mapState(s);
    check(m.thumbLX==32767&&m.thumbLY==32767&&m.leftTrigger==255&&m.rightTrigger==0,"out-of-range input is clamped");
    s=pad::State();s.lx=std::nanf("");s.l2=std::nanf("");
    m=mapState(s);
    check(m.thumbLX==0&&m.leftTrigger==0,"NaN input is treated as neutral");
    s=pad::State();s.lx=0.5f;s.l2=0.5f;
    m=mapState(s);
    check(m.thumbLX==16384&&m.leftTrigger==128,"half deflection and half trigger scale linearly");
    check(axis16(-0.5f)==-16384,"the negative half scales by 32768");
}

static void stateMachineTests() {
    {   // Idle states: never a pad on the desktop.
        FakePad fake;Session s(&fake);
        check(s.mode()==Mode::DriverMissing&&!s.padExists(),"a fresh session with no driver is DriverMissing and has no pad");
        s.setDriverPresent(true);
        check(s.mode()==Mode::Ready&&!s.padExists()&&fake.createCount==0,"driver present and enabled is Ready, with no pad (desktop)");
        s.setEnabled(false);
        check(s.mode()==Mode::Off&&!s.padExists(),"disabled is Off");
        s.setEnabled(true);s.setDriverPresent(false,QStringLiteral("not installed"));
        check(s.mode()==Mode::DriverMissing&&s.reason()==QStringLiteral("not installed"),"driver missing keeps its reason");
        check(s.statusText().contains(QStringLiteral("not installed")),"the status text carries the reason");
    }
    {   // Lifecycle ordering: create, THEN the game starts; destroy when the title ends.
        FakePad fake;Session s(&fake);s.setDriverPresent(true);
        QStringList order;
        const bool started=launchWithPad(s,true,[&]{order<<QStringLiteral("start:pad=")+(fake.exists()?"1":"0");return true;});
        check(started&&order==QStringList{QStringLiteral("start:pad=1")},"the virtual pad exists before the game is started");
        check(s.mode()==Mode::Active&&s.padExists()&&fake.events==QStringList{QStringLiteral("create")},"Active only while the pad exists; exactly one create");
        s.endTitle();
        check(s.mode()==Mode::Ready&&!s.padExists()&&fake.destroyCount==1,"title left: the pad is destroyed and the mode is Ready again");
        s.endTitle();
        check(fake.destroyCount==1,"endTitle is idempotent");
    }
    {   // A second press for the title already running does not create a second pad.
        FakePad fake;Session s(&fake);s.setDriverPresent(true);
        s.beginTitle(true);s.beginTitle(true);
        check(fake.createCount==1,"a repeated begin does not create a second pad");
    }
    {   // A failed create never blocks the launch.
        FakePad fake;fake.createFailure=QStringLiteral("access denied");
        Session s(&fake);s.setDriverPresent(true);
        bool launched=false;
        const bool started=launchWithPad(s,true,[&]{launched=true;return true;});
        check(started&&launched,"a failed create does not block the launch");
        check(s.mode()==Mode::Error&&s.reason()==QStringLiteral("access denied")&&!s.padExists(),"the failure is Error with its reason and no pad");
        check(s.statusText()==QStringLiteral("Error: access denied"),"the status text names the reason");
        s.endTitle();
        check(s.mode()==Mode::Error,"an Error stays visible after the title ends");
        fake.createFailure.clear();
        s.beginTitle(true);
        check(s.mode()==Mode::Active,"the next launch retries and can recover");
        s.endTitle();
    }
    {   // A launch that fails takes the pad away at once.
        FakePad fake;Session s(&fake);s.setDriverPresent(true);
        const bool started=launchWithPad(s,true,[]{return false;});
        check(!started&&!s.padExists()&&fake.destroyCount==1&&s.mode()==Mode::Ready,"a launch that fails leaves no pad behind");
    }
    {   // Disabled and driver-missing never touch the backend.
        FakePad fake;Session s(&fake);s.setDriverPresent(true);
        launchWithPad(s,false,[]{return true;});
        check(fake.createCount==0&&s.mode()==Mode::Off,"disabled for the title: no create, Off");
        FakePad other;Session t(&other);t.setDriverPresent(false);
        launchWithPad(t,true,[]{return true;});
        check(other.createCount==0&&t.mode()==Mode::DriverMissing,"driver missing: no create, DriverMissing, the launch still ran");
        Session none(nullptr);none.setDriverPresent(true);
        none.beginTitle(true);
        check(none.mode()==Mode::DriverMissing,"no backend at all is reported as missing, not as a crash");
    }
    {   // Feeding: only while Active, only changes, PS never forwarded, errors stop the pad.
        FakePad fake;Session s(&fake);s.setDriverPresent(true);
        pad::State st;st.buttons=xbox::A|0x0400;
        s.feed(st);
        check(fake.updateCount==0,"nothing is sent before the title starts");
        s.beginTitle(true);
        s.feed(st);s.feed(st);
        check(fake.updateCount==1&&fake.last.buttons==xbox::A,"a change is sent once and PS is stripped");
        st.rx=1.f;s.feed(st);
        check(fake.updateCount==2&&fake.last.thumbRX==32767,"the next change is sent");
        s.feedNeutral();
        check(fake.updateCount==3&&fake.last==Xbox360Report(),"neutral (the in-game XMB is up) releases everything");
        s.endTitle();
        s.feed(st);
        check(fake.updateCount==3,"nothing is sent after the title ended");
        FakePad failing;failing.failUpdatesAfter=1;Session f(&failing);f.setDriverPresent(true);f.beginTitle(true);
        st=pad::State();st.buttons=xbox::B;f.feed(st);
        st.buttons=xbox::X;f.feed(st);
        check(f.mode()==Mode::Error&&!f.padExists()&&failing.destroyCount==1,"a refused update ends the pad and reports Error");
    }
    {   // Rumble slot (phase 2) exists and is delivered.
        FakePad fake;int large=-1,small=-1;
        fake.setRumbleHandler([&](quint8 l,quint8 sm){large=l;small=sm;});
        fake.simulateRumble(200,50);
        check(large==200&&small==50,"the rumble handler slot receives the game's motor values");
    }
    {   // Override resolution.
        check(effectiveEnabled(true,Override::Default)&&!effectiveEnabled(false,Override::Default),"Default follows the global setting");
        check(effectiveEnabled(false,Override::On)&&!effectiveEnabled(true,Override::Off),"On and Off override the global setting");
        check(overrideFromName(QStringLiteral("ON"))==Override::On&&overrideFromName(QStringLiteral("off"))==Override::Off&&
              overrideFromName(QString())==Override::Default&&overrideFromName(QStringLiteral("junk"))==Override::Default,"override names parse; anything else is Default");
        check(overrideName(Override::Default).isEmpty()&&overrideName(Override::On)==QStringLiteral("on"),"override names round-trip");
    }
}

static void backendAndStatusTests(const QString& scratch,const QString& pinJson,const QString& realInstaller) {
    {
        auto backend=makeBackend(QStringLiteral("fake"));
        check(backend&&backend->backendName()==QStringLiteral("fake"),"the fake backend is selectable");
        auto real=makeBackend(QStringLiteral("real"));
        QString why;
        // In this test build the ViGEm client is not linked, so the default backend must say so and never claim a pad.
        check(real&&(real->backendName()==QStringLiteral("unavailable")||real->backendName()==QStringLiteral("vigem")),"the default backend is the unavailable stub or the ViGEm adapter");
        if(real->backendName()==QStringLiteral("unavailable")) {
            check(!real->create(&why)&&!why.isEmpty()&&!real->exists(),"the unavailable backend fails create truthfully and never exists");
        }
    }
    {
        qputenv("XMB_CONTROLLER_DRIVER","present");
        check(queryDriver().state==DriverState::Running,"XMB_CONTROLLER_DRIVER=present is a running driver (test seam)");
        qputenv("XMB_CONTROLLER_DRIVER","missing");
        check(queryDriver().state==DriverState::Missing,"XMB_CONTROLLER_DRIVER=missing is a missing driver (test seam)");
        qunsetenv("XMB_CONTROLLER_DRIVER");
        const auto real=queryDriver();   // read-only service query on this machine; any answer is valid, none may throw
        check(real.state==DriverState::Missing||real.state==DriverState::Stopped||real.state==DriverState::Running||real.state==DriverState::Unknown,"the real read-only service query answers");
        qputenv("XMB_CONTROLLER_MODE","0");
        check(killSwitchEngaged(),"XMB_CONTROLLER_MODE=0 engages the kill switch");
        qunsetenv("XMB_CONTROLLER_MODE");
        check(!killSwitchEngaged(),"no kill switch by default");
    }
    {
        const auto input=steamInputStatus(QString(),QStringLiteral("480"));
        check(input.state==SteamInput::Unknown&&input.detail==QStringLiteral("Steam Input status unknown"),"Steam Input is reported as unknown, never guessed");
        check(rowDetail({DriverState::Running,QString()},input)==QStringLiteral("Driver: ready. Steam Input status unknown."),"row detail: driver ready, Steam Input unknown");
        check(rowDetail({DriverState::Missing,QString()},input).startsWith(QStringLiteral("Driver: not installed")),"row detail: driver not installed");
        SteamInputStatus on;on.state=SteamInput::On;on.detail=QStringLiteral("Steam Input is on");
        check(rowDetail({DriverState::Running,QString()},on).contains(QStringLiteral("turn it off to avoid double input")),"the double-input warning appears only when Steam Input is known to be on");
    }
    {   // Per-game override in the overlay file.
        const QString dir=QDir(scratch).filePath(QStringLiteral("overlay"));
        QDir(dir).removeRecursively();QDir().mkpath(dir);
        SteamUserOverlay o;
        o.controllerMode.insert(QStringLiteral("480"),QStringLiteral("off"));
        o.controllerMode.insert(QStringLiteral("481"),QStringLiteral("on"));
        o.controllerMode.insert(QStringLiteral("482"),QStringLiteral("junk"));
        check(!o.isEmpty(),"an overlay with only controller overrides is not empty");
        check(saveSteamUserOverlay(dir,o),"the overlay saves");
        const auto back=loadSteamUserOverlay(dir);
        check(back.controllerMode.value(QStringLiteral("480"))==QStringLiteral("off")&&back.controllerMode.value(QStringLiteral("481"))==QStringLiteral("on")&&
              !back.controllerMode.contains(QStringLiteral("482")),"per-game overrides round-trip; an invalid value is dropped");
    }
    {   // The compiled-in pin equals packaging/vigem-driver.json.
        const auto& pin=pinnedInstaller();
        check(pin.size==6278576&&pin.sha256.size()==64&&pin.version==QStringLiteral("1.22.0"),"the pinned installer has its version, size and SHA-256 set");
        if(!pinJson.isEmpty()) {
            QFile f(pinJson);
            const bool opened=f.open(QIODevice::ReadOnly);
            const auto doc=QJsonDocument::fromJson(opened?f.readAll():QByteArray()).object();
            check(opened&&doc.value(QStringLiteral("sha256")).toString().toLower()==pin.sha256&&doc.value(QStringLiteral("size")).toInteger()==pin.size&&
                  doc.value(QStringLiteral("version")).toString()==pin.version&&doc.value(QStringLiteral("signer")).toString()==pin.signer&&
                  doc.value(QStringLiteral("fileName")).toString()==pin.fileName,"packaging/vigem-driver.json equals the compiled-in pin");
        }
        const QString dir=QDir(scratch).filePath(QStringLiteral("drivers"));
        QDir(dir).removeRecursively();QDir().mkpath(dir);
        check(bundledInstallerPath(scratch).isEmpty(),"no bundled installer is reported when drivers\\ is absent (a dev build)");
        QString why;
        check(!installDriver(scratch,&why)&&why.contains(QStringLiteral("no bundled")),"installDriver says so when the bundle is missing");
        QFile fake(QDir(dir).filePath(pin.fileName));
        check(fake.open(QIODevice::WriteOnly)&&fake.write("not the installer")>0,"a decoy installer is written");
        fake.close();
        check(!bundledInstallerPath(scratch).isEmpty(),"the bundled path is found once the file exists");
        check(!verifyInstaller(fake.fileName(),&why)&&why.contains(QStringLiteral("size")),"a decoy of the wrong size fails verification");
        QFile same(fake.fileName());
        check(same.open(QIODevice::WriteOnly)&&same.write(QByteArray(int(pin.size),'x'))>0,"a decoy of the right size is written");
        same.close();
        check(!verifyInstaller(same.fileName(),&why)&&why.contains(QStringLiteral("SHA-256")),"a decoy of the right size fails the SHA-256 check");
        check(!installDriver(scratch,&why),"installDriver refuses a file that fails verification (nothing is run)");
        if(!realInstaller.isEmpty()) {
            QString realWhy;
            check(verifyInstaller(realInstaller,&realWhy),"the real vendor installer passes size, SHA-256 and Authenticode signer verification");
            if(!realWhy.isEmpty())std::printf("  (%s)\n",realWhy.toLocal8Bit().constData());
        }
    }
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const QString scratch=argc>1?QString::fromLocal8Bit(argv[1]):QDir::current().filePath(QStringLiteral("controller-mode-scratch"));
    const QString pinJson=argc>2?QString::fromLocal8Bit(argv[2]):QString();
    QDir().mkpath(scratch);
    mappingTests();
    stateMachineTests();
    backendAndStatusTests(scratch,pinJson,argc>3?QString::fromLocal8Bit(argv[3]):QString());
    std::printf("\n%d checks, %d failure(s)\n",checks,failures);
    return failures?1:0;
}
