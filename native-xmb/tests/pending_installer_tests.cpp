#include "pending_installers.h"
#include "content_registry.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
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
    const QString registry=temp.filePath("data/pending-installers.json"); const QString library=temp.filePath("library"); QDir().mkpath(QDir(library).filePath("Download Archives")); xmb::PendingInstaller entry;
    entry.id=QStringLiteral("job");entry.title=QStringLiteral("Fixture");entry.installerPath=candidate;entry.archivePath=QDir(library).filePath("Download Archives/fixture.zip");entry.libraryRoot=library;entry.sha256=hash;entry.status=QStringLiteral("pending");
    ok &= check(xmb::PendingInstallerRegistry::upsert(registry,entry,error),"registry writes atomically");
    qputenv("XMB_PENDING_REGISTRY_FAIL_WRITE","1"); QString injectedError; ok &= check(!xmb::PendingInstallerRegistry::upsert(registry,entry,injectedError)&&injectedError.contains(QStringLiteral("Injected")),"injected registry write failure is reported"); qunsetenv("XMB_PENDING_REGISTRY_FAIL_WRITE");
    QVector<xmb::PendingInstaller> loaded; ok &= check(xmb::PendingInstallerRegistry::load(registry,loaded,error)&&loaded.size()==1,"registry survives reload without launching") &&
        check(loaded.first().installerPath==candidate,"registry preserves exact path");
    QFile second(QDir(root).filePath("install.exe")); Q_UNUSED(second.open(QIODevice::WriteOnly)); Q_UNUSED(second.write("fixture")); second.close();
    ok &= check(!xmb::inspectPendingInstaller(root,&candidate,&error)&&error.contains(QStringLiteral("More than one")),"ambiguous installer candidates are refused");
    const QString destination=QDir(library).filePath("Synthetic Game"); QDir().mkpath(destination);
    QFile game(QDir(destination).filePath("game.exe")); Q_UNUSED(game.open(QIODevice::WriteOnly)); Q_UNUSED(game.write("game")); game.close();
    entry.libraryRoot=library; entry.destination=destination; entry.installerPath=QDir(root).filePath("setup.exe"); entry.status=QStringLiteral("pending");
    const QString apps=temp.filePath("data/apps.json"); QString verifyError,registryError;
    qputenv("XMB_PENDING_REGISTRY_FAIL_WRITE","1");
    ok &= check(!xmb::verifyAndRegisterPendingInstaller(registry,apps,entry,&verifyError),"registry failure after app registration is surfaced");
    qunsetenv("XMB_PENDING_REGISTRY_FAIL_WRITE");
    ok &= check(xmb::verifyAndRegisterPendingInstaller(registry,apps,entry,&verifyError),"verified destination registers exactly one game executable");
    QVector<xmb::ContentApp> recoveredApps; ok &= check(xmb::ContentRegistry::load(apps,recoveredApps,registryError)&&recoveredApps.size()==1,"idempotent retry does not duplicate the recovered game registration");
    QFile redist(QDir(destination).filePath("vc_redist.exe")); Q_UNUSED(redist.open(QIODevice::WriteOnly)); Q_UNUSED(redist.write("redist")); redist.close();
    QVector<xmb::ContentApp> registered;
    ok &= check(xmb::ContentRegistry::load(apps,registered,registryError)&&registered.size()==1&&registered.first().executable.endsWith(QStringLiteral("game.exe")),"redist is never registered");
    QFile replacement(entry.installerPath); Q_UNUSED(replacement.open(QIODevice::WriteOnly|QIODevice::Truncate)); Q_UNUSED(replacement.write("replacement")); replacement.close();
    ok &= check(!xmb::verifyAndRegisterPendingInstaller(registry,apps,entry,&verifyError),"post-confirm installer replacement is refused");
    Q_UNUSED(replacement.open(QIODevice::WriteOnly|QIODevice::Truncate)); Q_UNUSED(replacement.write("fixture")); replacement.close();
    QFile secondGame(QDir(destination).filePath("second.exe")); Q_UNUSED(secondGame.open(QIODevice::WriteOnly)); Q_UNUSED(secondGame.write("second")); secondGame.close();
    xmb::PendingInstaller chooserEntry=entry; chooserEntry.id=QStringLiteral("chooser-job"); chooserEntry.status=QStringLiteral("pending");
    ok &= check(xmb::PendingInstallerRegistry::upsert(registry,chooserEntry,verifyError),"chooser fixture is registered");
    ok &= check(!xmb::verifyAndRegisterPendingInstaller(registry,apps,chooserEntry,&verifyError),"multiple post-install candidates require a chooser");
    ok &= check(xmb::verifyAndRegisterPendingInstaller(registry,apps,chooserEntry,&verifyError,QFileInfo(secondGame).canonicalFilePath()),"selected executable completes chooser registration with canonical full path");
    xmb::PendingInstaller bad=entry; bad.destination=temp.filePath("outside");
    ok &= check(!xmb::verifyAndRegisterPendingInstaller(registry,apps,bad,&verifyError),"unsafe destination is refused");
    xmb::PendingInstaller launching=entry; launching.id=QStringLiteral("launch-job"); launching.destination=QDir(library).filePath("Not Yet Created"); launching.status=QStringLiteral("launching"); launching.helperPid=12345; launching.helperCreationTime=QStringLiteral("1337");
    ok &= check(xmb::PendingInstallerRegistry::upsert(registry,launching,verifyError),"launching record with a not-yet-created destination is accepted");
    xmb::PendingInstaller escaping=launching; escaping.id=QStringLiteral("escape-job"); escaping.destination=temp.filePath("outside-escape");
    ok &= check(!xmb::PendingInstallerRegistry::upsert(registry,escaping,verifyError),"launching record with an escaping destination is still refused");
#ifdef Q_OS_WIN
    {
        // A directory junction whose target is outside the library must be
        // rejected for a not-yet-created leaf: canonicalInstallDestination
        // resolves the junction parent instead of trusting a lexical prefix.
        QTemporaryDir outside;
        const QString junction=QDir(library).filePath(QStringLiteral("escape-link"));
        QProcess mklink;
        mklink.start(QStringLiteral("cmd.exe"),{QStringLiteral("/c"),QStringLiteral("mklink"),QStringLiteral("/J"),
            QDir::toNativeSeparators(junction),QDir::toNativeSeparators(outside.path())});
        mklink.waitForFinished(10000);
        if(mklink.exitStatus()==QProcess::NormalExit && mklink.exitCode()==0) {
            xmb::PendingInstaller reparse=launching; reparse.id=QStringLiteral("reparse-job"); reparse.destination=QDir(junction).filePath(QStringLiteral("Game"));
            ok &= check(!xmb::PendingInstallerRegistry::upsert(registry,reparse,verifyError),"launching record with a junction parent escaping the library is refused");
            QDir().rmdir(junction);   // removes the junction, not its target
        }
    }
#endif
    {
        // Staging a pending installer folder writes a recovery journal and moves
        // the folder; injected failures surface an error and leave the staging in
        // place so the caller can discard it rather than leave unindexed files.
        const QString pendingParent=temp.filePath("pi-root"); QDir().mkpath(pendingParent);
        const QByteArray recoveryBytes=QByteArrayLiteral("{\"id\":\"stage-job\"}");
        {
            QTemporaryDir stage;
            QFile f(QDir(stage.path()).filePath("setup.exe")); Q_UNUSED(f.open(QIODevice::WriteOnly)); Q_UNUSED(f.write("fixture")); f.close();
            const QString pendingRoot=QDir(pendingParent).filePath("stage-job");
            ok &= check(xmb::stagePendingInstallerFolder(stage.path(),pendingRoot,recoveryBytes).isEmpty(),"staging moves to the pending location on success");
            ok &= check(QFileInfo(pendingRoot).isDir() && QFileInfo(QDir(pendingRoot).filePath(".xmb-pending-recovery.json")).isFile(),"staging journal lands at the pending location");
        }
        {
            QTemporaryDir stage;
            QFile f(QDir(stage.path()).filePath("setup.exe")); Q_UNUSED(f.open(QIODevice::WriteOnly)); Q_UNUSED(f.write("fixture")); f.close();
            const QString pendingRoot=QDir(pendingParent).filePath("stage-fail");
            qputenv("XMB_PENDING_STAGING_JOURNAL_FAIL","1");
            const QString err=xmb::stagePendingInstallerFolder(stage.path(),pendingRoot,recoveryBytes);
            qunsetenv("XMB_PENDING_STAGING_JOURNAL_FAIL");
            ok &= check(!err.isEmpty(),"journal-write failure is surfaced");
            ok &= check(QFileInfo(stage.path()).isDir() && !QFileInfo(pendingRoot).exists(),"journal failure leaves staging in place for the caller");
        }
        {
            QTemporaryDir stage;
            QFile f(QDir(stage.path()).filePath("setup.exe")); Q_UNUSED(f.open(QIODevice::WriteOnly)); Q_UNUSED(f.write("fixture")); f.close();
            const QString pendingRoot=QDir(pendingParent).filePath("stage-rename-fail");
            qputenv("XMB_PENDING_STAGING_RENAME_FAIL","1");
            const QString err=xmb::stagePendingInstallerFolder(stage.path(),pendingRoot,recoveryBytes);
            qunsetenv("XMB_PENDING_STAGING_RENAME_FAIL");
            ok &= check(!err.isEmpty(),"rename failure is surfaced");
            ok &= check(QFileInfo(stage.path()).isDir() && !QFileInfo(pendingRoot).exists(),"rename failure leaves staging in place for the caller");
        }
    }
    QFile corrupt(registry); Q_UNUSED(corrupt.open(QIODevice::WriteOnly|QIODevice::Truncate)); Q_UNUSED(corrupt.write("{broken")); corrupt.close(); QVector<xmb::PendingInstaller> rejected; ok &= check(!xmb::PendingInstallerRegistry::load(registry,rejected,error),"corrupt registry is rejected without bypass");
    return ok?0:1;
}
