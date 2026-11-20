#include "pending_installers.h"
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
    const QString registry=temp.filePath("data/pending-installers.json"); xmb::PendingInstaller entry;
    entry.id=QStringLiteral("job");entry.title=QStringLiteral("Fixture");entry.installerPath=candidate;entry.archivePath=temp.filePath("fixture.zip");entry.sha256=hash;entry.status=QStringLiteral("pending");
    ok &= check(xmb::PendingInstallerRegistry::upsert(registry,entry,error),"registry writes atomically");
    QVector<xmb::PendingInstaller> loaded; ok &= check(xmb::PendingInstallerRegistry::load(registry,loaded,error)&&loaded.size()==1,"registry survives reload without launching") &&
        check(loaded.first().installerPath==candidate,"registry preserves exact path");
    QFile second(QDir(root).filePath("install.exe")); Q_UNUSED(second.open(QIODevice::WriteOnly)); Q_UNUSED(second.write("fixture")); second.close();
    ok &= check(!xmb::inspectPendingInstaller(root,&candidate,&error)&&error.contains(QStringLiteral("More than one")),"ambiguous installer candidates are refused");
    return ok?0:1;
}
