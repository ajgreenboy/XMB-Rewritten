#include "preferences.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <filesystem>
#include <memory>

using namespace xmb;

// A damaged preferences file heals itself; a newer or foreign one is never touched. Usage:
//   xmb-preferences-heal-tests [scratch-directory]     exit code 0 = all pass.
static int failures=0;
static int checks=0;
static void check(bool ok,const QByteArray& what) {
    ++checks;
    std::printf("%s  %s\n",ok?"PASS":"FAIL",what.constData());
    if(!ok)++failures;
}
static QString root;
static int caseNumber=0;
static const QString OldError=QStringLiteral("Preferences have an invalid or newer format. The existing file has been preserved.");
static QString freshDir() {
    const QString dir=QDir(root).filePath(QStringLiteral("case%1").arg(++caseNumber));
    QDir(dir).removeRecursively();
    std::error_code ec;std::filesystem::create_directories(dir.toStdWString(),ec);
    return dir;
}
static void put(const QString& dir,const QByteArray& bytes) {
    QFile f(QDir(dir).filePath("preferences.json"));
    if(!f.open(QIODevice::WriteOnly) || f.write(bytes)!=bytes.size())check(false,"could not write fixture: "+f.fileName().toUtf8()+" "+f.errorString().toUtf8());
}
static QByteArray get(const QString& dir) {
    QFile f(QDir(dir).filePath("preferences.json"));
    return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray("<missing>");
}
static QStringList backups(const QString& dir) {
    return QDir(dir).entryList({QStringLiteral("preferences.json.corrupt-*")},QDir::Files,QDir::Name);
}
static QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray("<unreadable>");
}
static bool freshFileValid(const QString& dir) {
    const auto doc=QJsonDocument::fromJson(get(dir));
    return doc.isObject() && doc.object().value("version").toInt()==2 && doc.object().value("values").isObject();
}
// Heals: writable, no error, a valid fresh file and exactly one backup holding the original bytes.
static void healed(const QByteArray& original,bool expectNotice,const QByteArray& name) {
    const auto dir=freshDir();
    put(dir,original);
    std::unique_ptr<Preferences> prefs(new Preferences(dir));
    check(prefs->error().isEmpty(),name+": no error");
    check(freshFileValid(dir),name+": fresh valid file written");
    check(backups(dir).size()==1 && readAll(QDir(dir).filePath(backups(dir).value(0)))==original,name+": one backup with the original bytes");
    check(prefs->healedBackup().isEmpty()!=expectNotice,name+(expectNotice?": healedBackup() set":": healedBackup() empty (silent)"));
    if(expectNotice)check(backups(dir).value(0)==prefs->healedBackup(),name+": healedBackup() names the backup file");
    check(prefs->setValue("audio/volume",42) && prefs->value("audio/volume").toInt()==42,name+": settings save again");
    prefs.reset(new Preferences(dir));
    check(prefs->error().isEmpty() && prefs->healedBackup().isEmpty() && backups(dir).size()==1 && prefs->value("audio/volume").toInt()==42,
          name+": second construction loads cleanly");
}
// Untouched: read-only, the old error text, original bytes in place, no backup.
static void preserved(const QByteArray& original,const QByteArray& name) {
    const auto dir=freshDir();
    put(dir,original);
    Preferences prefs(dir);
    check(prefs.error()==OldError,name+": old error text");
    check(get(dir)==original && backups(dir).isEmpty() && prefs.healedBackup().isEmpty(),name+": file untouched, no backup");
    check(!prefs.setValue("audio/volume",42) && get(dir)==original,name+": stays read-only");
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    root=argc>1?QString::fromLocal8Bit(argv[1]):QStringLiteral("C:/dev/out/prefs-heal-test");
    QDir(root).removeRecursively();
    {
        std::error_code ec;
        std::filesystem::create_directories(root.toStdWString(),ec);
        if(ec)std::printf("FAIL  scratch directory %s: %s\n",root.toUtf8().constData(),ec.message().c_str());
    }
    // (a) no data to lose: silent.
    healed(QByteArray(528,'\0'),false,"all-NUL 528 bytes");
    healed(QByteArray(),false,"empty");
    healed(QByteArray("  \r\n\t \n"),false,"whitespace only");
    healed(QByteArray("\0 \0\n\0",5),false,"NUL and whitespace mix");
    // (b) not JSON: heals with a notice.
    healed(QByteArray("{\"version\":2,\"active\":\"\",\"values\":{\"audio/vol"),true,"truncated JSON");
    healed(QByteArray("not json at all"),true,"garbage text");
    // (c) newer or foreign: untouched.
    preserved(QByteArray("{\"version\":3,\"values\":{\"x\":1}}"),"version 3");
    preserved(QByteArray("[1,2,3]"),"JSON array");
    preserved(QByteArray("42"),"JSON scalar");
    preserved(QByteArray("{\"hello\":\"world\"}"),"foreign object");
    preserved(QByteArray("\"just a string\""),"JSON string");
    preserved(QByteArray("null"),"JSON null");
    {
        const QByteArray bytes("{\"version\":2,\"active\":\"Bad_Id\",\"values\":{}}");
        const auto dir=freshDir();put(dir,bytes);
        Preferences prefs(dir);
        check(!prefs.error().isEmpty() && get(dir)==bytes && backups(dir).isEmpty() && prefs.healedBackup().isEmpty() && !prefs.setValue("audio/volume",42),
              "invalid profile id: untouched and read-only");
    }
    {
        QJsonObject values;
        for(int i=0;i<2049;++i)values.insert(QStringLiteral("k%1").arg(i),1);
        const auto bytes=QJsonDocument(QJsonObject{{"version",2},{"active",""},{"values",values}}).toJson();
        const auto dir=freshDir();put(dir,bytes);
        Preferences prefs(dir);
        check(!prefs.error().isEmpty() && get(dir)==bytes && backups(dir).isEmpty(),"too many keys: untouched and read-only");
    }
    {
        const auto dir=freshDir();put(dir,QByteArray(1024*1024+1,'\0'));
        Preferences prefs(dir);
        check(prefs.error().startsWith("Preferences could not be read") && backups(dir).isEmpty() && get(dir).size()==1024*1024+1,"over 1 MiB: unchanged behaviour");
    }
    // Backup name collision: the names for this second and the next few are taken, so a counter is added.
    {
        const auto dir=freshDir();
        QStringList taken;
        for(int offset=-1;offset<=3;++offset) {
            const auto name=QStringLiteral("preferences.json.corrupt-")+QDateTime::currentDateTime().addSecs(offset).toString("yyyyMMdd-HHmmss");
            QFile f(QDir(dir).filePath(name));
            if(f.open(QIODevice::WriteOnly))f.write("old backup");
            taken.push_back(name);
        }
        put(dir,QByteArray("{\"truncated"));
        Preferences prefs(dir);
        const auto name=prefs.healedBackup();
        check(prefs.error().isEmpty() && !name.isEmpty() && !taken.contains(name) && backups(dir).size()==taken.size()+1,"collision: a counter keeps the new backup apart");
        bool oldIntact=true;
        for(const auto& t:taken)oldIntact=oldIntact && readAll(QDir(dir).filePath(t))=="old backup";
        check(oldIntact,"collision: earlier backups untouched");
        check(readAll(QDir(dir).filePath(name))=="{\"truncated","collision: backup holds the damaged bytes");
    }
    // Rename failure: another open handle keeps the file from being renamed, so the old read-only behaviour applies.
    {
        const auto dir=freshDir();
        const QByteArray original("{\"truncated");
        put(dir,original);
        QFile hold(QDir(dir).filePath("preferences.json"));
        const bool held=hold.open(QIODevice::ReadOnly);
        Preferences prefs(dir);
        hold.close();
        check(held && prefs.error()==OldError && get(dir)==original && backups(dir).isEmpty() && prefs.healedBackup().isEmpty() && !prefs.setValue("audio/volume",1),
              "rename failure: read-only, same error, file preserved");
    }
    // A healthy file still loads unchanged.
    {
        const auto dir=freshDir();
        {Preferences prefs(dir);prefs.setValue("audio/volume",7);}
        Preferences prefs(dir);
        check(prefs.error().isEmpty() && prefs.value("audio/volume").toInt()==7 && backups(dir).isEmpty(),"healthy file: unchanged");
    }
    std::printf("%d checks, %d failed: %s\n",checks,failures,failures?"FAILED":"ALL PASSED");
    return failures?1:0;
}
