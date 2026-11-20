#include "pending_installers.h"
#include "content_registry.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>

static bool check(bool value,const char* text) { if(!value)std::cerr<<"FAIL: "<<text<<"\n"; return value; }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); QTemporaryDir temp; if(!temp.isValid())return 2;
    const QString root=temp.filePath("owned"); QDir().mkpath(root);
    QFile setup(QDir(root).filePath("setup.exe")); Q_UNUSED(setup.open(QIODevice::WriteOnly)); Q_UNUSED(setup.write("fixture")); setup.close();
    QString candidate,error,hash;
    bool ok=check(xmb::inspectPendingInstaller(root,&candidate,&error),"one installer candidate is recognized") &&
        check(QFileInfo(candidate).fileName()==QStringLiteral("setup.exe"),"candidate stays in owned folder") &&
        check(xmb::sha256File(candidate,&hash,&error)&&hash.size()==64,"installer hash is computed");
    const QString registry=temp.filePath("data/pending-installers.json"); const QString library=temp.filePath("library"); QDir().mkpath(library); xmb::PendingInstaller entry;
    entry.id=QStringLiteral("job");entry.title=QStringLiteral("Fixture");entry.installerPath=candidate;entry.archivePath=QDir(root).filePath("fixture.zip");entry.libraryRoot=library;entry.sha256=hash;entry.status=QStringLiteral("pending");
    ok &= check(xmb::PendingInstallerRegistry::upsert(registry,entry,error),"registry writes atomically");
    qputenv("XMB_PENDING_REGISTRY_FAIL_WRITE","1"); QString injectedError; ok &= check(!xmb::PendingInstallerRegistry::upsert(registry,entry,injectedError)&&injectedError.contains(QStringLiteral("Injected")),"injected registry write failure is reported"); qunsetenv("XMB_PENDING_REGISTRY_FAIL_WRITE");
    QVector<xmb::PendingInstaller> loaded; ok &= check(xmb::PendingInstallerRegistry::load(registry,loaded,error)&&loaded.size()==1,"registry survives reload without launching") &&
        check(loaded.first().installerPath==candidate,"registry preserves exact path");
    QFile second(QDir(root).filePath("install.exe")); Q_UNUSED(second.open(QIODevice::WriteOnly)); Q_UNUSED(second.write("fixture")); second.close();
    ok &= check(!xmb::inspectPendingInstaller(root,&candidate,&error)&&error.contains(QStringLiteral("More than one")),"ambiguous installer candidates are refused");
    const QString destination=QDir(library).filePath("Synthetic Game"); QDir().mkpath(destination);
    QFile game(QDir(destination).filePath("game.exe")); Q_UNUSED(game.open(QIODevice::WriteOnly)); Q_UNUSED(game.write("game")); game.close();
    entry.libraryRoot=library; entry.destination=destination; entry.installerPath=QDir(root).filePath("setup.exe"); entry.status=QStringLiteral("pending");
    const QString apps=temp.filePath("data/apps.json"); QString verifyError;
    ok &= check(xmb::verifyAndRegisterPendingInstaller(registry,apps,entry,&verifyError),"verified destination registers exactly one game executable");
    QFile redist(QDir(destination).filePath("vc_redist.exe")); Q_UNUSED(redist.open(QIODevice::WriteOnly)); Q_UNUSED(redist.write("redist")); redist.close();
    QVector<xmb::ContentApp> registered; QString registryError; Q_UNUSED(registryError);
    ok &= check(xmb::ContentRegistry::load(apps,registered,registryError)&&registered.size()==1&&registered.first().executable.endsWith(QStringLiteral("game.exe")),"redist is never registered");
    QFile replacement(entry.installerPath); Q_UNUSED(replacement.open(QIODevice::WriteOnly|QIODevice::Truncate)); Q_UNUSED(replacement.write("replacement")); replacement.close();
    ok &= check(!xmb::verifyAndRegisterPendingInstaller(registry,apps,entry,&verifyError),"post-confirm installer replacement is refused");
    Q_UNUSED(replacement.open(QIODevice::WriteOnly|QIODevice::Truncate)); Q_UNUSED(replacement.write("fixture")); replacement.close();
    QFile secondGame(QDir(destination).filePath("second.exe")); Q_UNUSED(secondGame.open(QIODevice::WriteOnly)); Q_UNUSED(secondGame.write("second")); secondGame.close();
    ok &= check(!xmb::verifyAndRegisterPendingInstaller(registry,apps,entry,&verifyError),"multiple post-install candidates require a chooser");
    ok &= check(xmb::verifyAndRegisterPendingInstaller(registry,apps,entry,&verifyError,secondGame.fileName()),"selected executable completes chooser registration");
    xmb::PendingInstaller bad=entry; bad.destination=temp.filePath("outside");
    ok &= check(!xmb::verifyAndRegisterPendingInstaller(registry,apps,bad,&verifyError),"unsafe destination is refused");
    QFile corrupt(registry); Q_UNUSED(corrupt.open(QIODevice::WriteOnly|QIODevice::Truncate)); Q_UNUSED(corrupt.write("{broken")); corrupt.close(); QVector<xmb::PendingInstaller> rejected; ok &= check(!xmb::PendingInstallerRegistry::load(registry,rejected,error),"corrupt registry is rejected without bypass");
    return ok?0:1;
}
