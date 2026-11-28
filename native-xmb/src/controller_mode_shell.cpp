// Controller mode, the Shell side (audit/CONTROLLER-MODE.md): the lifecycle around a Steam launch, the feed from the
// physical pad while the title runs, and the Game Settings "Install controller driver" action. The rules live in
// controller_mode.h (pure, unit-tested); this file only connects them to the launch and return paths.
#include "shell.h"
#include "controller_mode.h"
#include <QCoreApplication>
#include <QDebug>

namespace xmb {

controller::Override Shell::controllerOverride(const QString& appId) const {
    return controller::overrideFromName(steamOverlay_.controllerMode.value(appId));
}

bool Shell::controllerLaunch(const QString& appId,const std::function<bool()>& start) {
    if(!controllerPad_) {
        controllerPad_=controller::makeBackend();
        controllerSession_.setBackend(controllerPad_.get());
        controllerFeed_.setTimerType(Qt::PreciseTimer);
        controllerFeed_.setInterval(4);   // a DualSense reports at about 250 Hz over USB, 100 Hz over Bluetooth
        connect(&controllerFeed_,&QTimer::timeout,this,&Shell::controllerTick);
    }
    const bool globalOn=!controller::killSwitchEngaged() && preferences_.value("steam/controllerMode").toBool();
    const bool enabled=!controller::killSwitchEngaged() && controller::effectiveEnabled(globalOn,controllerOverride(appId));
    const auto driver=controller::queryDriver();
    controllerSession_.setDriverPresent(driver.state==controller::DriverState::Running,driver.detail);
    const bool started=controller::launchWithPad(controllerSession_,enabled,start);
    qInfo().noquote()<<"controller: launch app="+appId<<"backend="+controllerPad_->backendName()<<"enabled="+QString::number(enabled?1:0)
        <<"status="+controllerSession_.statusText()<<"pad="+QString::number(controllerSession_.padExists()?1:0)<<"started="+QString::number(started?1:0);
    if(controllerSession_.padExists())controllerFeed_.start();
    else controllerFeed_.stop();
    return started;
}

void Shell::controllerEnd(const char* why) {
    if(!controllerPad_)return;
    controllerFeed_.stop();
    const bool had=controllerSession_.padExists();
    controllerSession_.endTitle();
    if(had)qInfo().noquote()<<"controller: virtual pad removed ("+QString::fromLatin1(why)+")";
}

void Shell::controllerTick() {
    if(!controllerSession_.padExists()){controllerFeed_.stop();return;}
    // While the in-game XMB is up the game must not move: it gets a neutral pad. The PS button is never forwarded.
    pad::State state;
    if(inGame_ || !pad::latest(state,false))controllerSession_.feedNeutral();
    else controllerSession_.feed(state);
    if(controllerSession_.mode()==controller::Mode::Error) {
        qWarning().noquote()<<"controller: the virtual pad stopped:"<<controllerSession_.reason();
        controllerFeed_.stop();
    }
}

void Shell::activateControllerDriver(const Entry& entry) {
    const QString appDirectory=QCoreApplication::applicationDirPath();
    const QString path=controller::bundledInstallerPath(appDirectory);
    const auto& pin=controller::pinnedInstaller();
    if(path.isEmpty()) {
        showMessage(entry.title,tr("This build has no bundled controller driver installer (drivers\\%1 is missing). A packaged release carries it.").arg(pin.fileName),MessageKind::Page);
        return;
    }
    showConfirm(entry.title,
        tr("Install the virtual gamepad driver?\n\nFile: %1\nDriver: ViGEmBus %2 (signed by %3)\nSource: bundled with this XMB; its SHA-256 and signature are checked again just before it runs.\n\nWindows asks for administrator permission and the installer shows its own window, so a keyboard or mouse is needed once.")
            .arg(pin.fileName,pin.version,pin.signer),
        [this,appDirectory,title=entry.title]{
            QString why;
            if(controller::installDriver(appDirectory,&why)) {
                qInfo().noquote()<<"controller: driver installer started (bundled, verified)";
                notify(title,tr("The driver installer was started"),QStringLiteral("tex_notification_info"),false);
            } else {
                qWarning().noquote()<<"controller: driver install not started:"<<why;
                showMessage(title,why,MessageKind::Page);
            }
        });
}

}
